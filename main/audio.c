// echo -- ES8311 audio output. See audio.h for the verified hardware map.

#include "audio.h"

#include "display.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "math.h"
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "audio";

// I2S pins (vendor BSP: MCLK none, SCLK=24, WS=25, DOUT=26, DSIN=27).
#define AUD_I2S_NUM I2S_NUM_0
#define AUD_I2S_MCLK I2S_GPIO_UNUSED
#define AUD_I2S_SCLK GPIO_NUM_24
#define AUD_I2S_WS GPIO_NUM_25
#define AUD_I2S_DOUT GPIO_NUM_26
#define AUD_I2S_DSIN GPIO_NUM_27

// PA enable is CH32 expander pin 3 (IO_POWER_AMP_IO), active HIGH.
#define AUD_PA_EXPANDER_PIN IO_EXPANDER_PIN_NUM_3

#define AUD_SAMPLE_RATE 16000
#define AUD_DEFAULT_VOL 70 // %

// ---- Synthesis / streaming constants (units stated, rationale inline) ----

// Tone chunk: samples rendered per esp_codec_dev_write() call. Large
// enough (~128 ms at 16 kHz) that per-call overhead is inaudible and the
// DMA is refilled far faster than it drains.
#define AUD_CHUNK_SAMPLES 2048
// Inter-note gap: ms of silence between notes (musical articulation).
#define AUD_NOTE_GAP_MS 25
// Lead/trail pad: ms of silence before the first and after the last
// sample, so the DMA has settled before we start and after we stop.
#define AUD_SONG_PAD_MS 40
// Fade: ms of raised-cosine attack/release on every note. This is the
// crackle fix -- a hard note edge is a DC step, i.e. an audible click.
#define AUD_NOTE_FADE_MS 10
// Drain delay: ms we block after the trailing silence so the I2S DMA
// actually pushes the last samples out before esp_codec_dev_close().
// Must exceed the pad, or the close truncates the tail (another click).
#define AUD_DRAIN_DELAY_MS 300
// Peak amplitude of a synthesized note. ~34% of int16 full scale: loud
// enough to hear over the PA, without clipping the ES8311 DAC.
#define AUD_NOTE_AMPLITUDE 11000.0f

// Audible unit used to drive the 16-bit codec from any task.
#define AUD_BYTES_PER_SAMPLE sizeof(int16_t)

// A stop request bumps the generation counter; a player whose token no
// longer matches the current generation has been cancelled. A single
// monotonically increasing counter (never cleared by the player) means a
// stop that lands before a new song even starts is never lost -- which is
// exactly what a shared "reset to false at entry" flag could not
// guarantee. Read/write is atomic on ESP32 for a naturally aligned
// 32-bit word, so no critical section is needed for either operation.
static volatile uint32_t s_stop_generation = 0;

// #3 (2026-10-10): playback-liveness flag. True while a tone/song is in
// flight, cleared when playback_finalize() has drained the DMA and closed the
// codec. Owned/updated by the audio (player) task; read by any task via
// audio_is_playing(). Single aligned word + volatile = the atomic,
// compiler-can't-cache contract the header documents. audio_is_playing() was
// previously DECLARED but never DEFINED (a latent link error + the UI had no
// way to observe when a song finished); this backs it.
static volatile bool s_playing = false;

// Driver/codec handles + current volume (restored after a refactor dropped
// them, which broke the build with 's_codec undeclared').
static i2s_chan_handle_t s_tx_chan = NULL;
static i2s_chan_handle_t s_rx_chan = NULL;
static esp_codec_dev_handle_t s_codec = NULL;
static int s_volume = AUD_DEFAULT_VOL;

// V03 (2026-10-10): codec-ownership mutex. The startup tone (app_main) and the
// UI song player (ui.c player_task) can both call into audio and would
// otherwise open/write/close the SAME codec concurrently (esp_codec_dev_open
// succeeds again on an already-open device, so it does not reject overlap).
// Every public playback entry (tone/song) takes this for the whole
// open->write->drain->close session, guaranteeing exactly one session + one
// close. Non-blocking acquire with a short bound so a UI press never stalls
// LVGL waiting on an in-progress tone; a busy codec simply rejects the request.
static SemaphoreHandle_t s_play_mutex = NULL;

// --- Audio command-owner service (review order 5, V03 preferred extension) ---
// ONE task owns the codec. Callers (UI, app_main chime) ENQUEUE commands and
// never touch I2S/codec/I2C. The host-tested policy in core/audio_cmd.h decides
// transitions + latest-play acceptance; s_status is the observable state the UI
// reads (F08 stop/mute/playback feedback). s_status is written ONLY by the
// service task (single-owner), read by others behind a short critical section.
#include "audio_cmd.h"
static QueueHandle_t s_cmd_q = NULL;
static audio_status_t s_status;                 // owned by the service task
static portMUX_TYPE s_status_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_cmd_next_id = 1;
#define AUDIO_CMD_QUEUE_LEN 6
static void audio_service_task(void *arg);

void audio_request_stop(void) {
    s_stop_generation++; // invalidate whatever is currently playing
}

static esp_err_t i2s_bring_up(void) {
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(AUD_I2S_NUM, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    // DMA buffer sizing (2026-10-11): the full-duplex pair (tx+rx) allocates
    // dma_desc_num * dma_frame_num * 2 bytes * 2 channels of DMA-CAPABLE
    // INTERNAL RAM. 8x512 (~16 KB) did NOT fit the internal DMA pool left after
    // display/LVGL + WiFi + HTTP init (boot log: "allocate I2S DMA buffer
    // failed", audio init 0x101/NO_MEM, chime silently absent). 6x240 (~5.6 KB)
    // fits with headroom and still gives ~90 ms/channel of buffering — enough
    // to ride out a brief LVGL/WiFi preemption on this single-core chip. The
    // deep buffers were documented "insurance", NOT the crackle fix (the real
    // fix is the per-note envelope + continuous phase + drain-before-close).
    chan_cfg.dma_desc_num = 6;
    chan_cfg.dma_frame_num = 240;
    // Create BOTH tx + rx channels (full-duplex pair sharing one BCLK/WS),
    // exactly like the vendor bsp_audio_init. Creating tx-only (rx=NULL)
    // made the ES8311 codec's clock-reconfig path mis-clock the DAC ->
    // crackle. The factory firmware (verified clean on this board) uses
    // both handles, so we match it.
    esp_err_t ret = i2s_new_channel(&chan_cfg, &s_tx_chan, &s_rx_chan);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel failed: 0x%x", ret);
        return ret;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUD_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = AUD_I2S_MCLK,
            .bclk = AUD_I2S_SCLK,
            .ws = AUD_I2S_WS,
            .dout = AUD_I2S_DOUT,
            .din = AUD_I2S_DSIN,
            .invert_flags = {.mclk_inv = false, .bclk_inv = false, .ws_inv = false},
        },
    };
    // Vendor BSP's exact proven-working config: 16-bit MONO slots, default
    // MCLK multiple, use_mclk=false, default mclk_div (256). This is the
    // config the factory 08_bookesia firmware runs on this exact board.
    ret = i2s_channel_init_std_mode(s_tx_chan, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s tx std init failed: 0x%x", ret);
        goto fail;
    }
    ret = i2s_channel_enable(s_tx_chan);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s tx enable failed: 0x%x", ret);
        goto fail;
    }
    // Init + enable the rx channel too (vendor does this; the codec's
    // clock reconfig expects both channels of the duplex pair to exist).
    ret = i2s_channel_init_std_mode(s_rx_chan, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s rx std init failed: 0x%x", ret);
        goto fail;
    }
    ret = i2s_channel_enable(s_rx_chan);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s rx enable failed: 0x%x", ret);
        goto fail;
    }
    return ESP_OK;

fail:
    // Partial-init unwind (2026-10-10): a failure after i2s_new_channel left
    // the channels allocated (DMA rings etc). Delete both so a failed audio
    // init doesn't leak I2S resources. Disable first (ignore errors — a
    // never-enabled channel just no-ops), then delete; NULL the handles.
    if (s_rx_chan) { i2s_channel_disable(s_rx_chan); i2s_del_channel(s_rx_chan); s_rx_chan = NULL; }
    if (s_tx_chan) { i2s_channel_disable(s_tx_chan); i2s_del_channel(s_tx_chan); s_tx_chan = NULL; }
    return ret;
}

esp_err_t audio_init(void) {
    if (s_codec != NULL) {
        return ESP_OK;
    }
    if (s_play_mutex == NULL) {
        s_play_mutex = xSemaphoreCreateMutex();
        if (s_play_mutex == NULL) {
            ESP_LOGE(TAG, "V03: failed to create playback mutex");
            return ESP_ERR_NO_MEM;
        }
    }
    audio_status_init(&s_status, AUD_DEFAULT_VOL);
    i2c_master_bus_handle_t bus = display_get_i2c_bus();
    esp_io_expander_handle_t exp = display_get_io_expander();
    if (bus == NULL || exp == NULL) {
        ESP_LOGE(TAG, "I2C bus / expander not ready -- call display_init() first");
        return ESP_ERR_INVALID_STATE;
    }

    // Enable the speaker power amplifier via the CH32 expander (active
    // HIGH). The ES8311's own pa_pin is NC on this board.
    esp_io_expander_set_dir(exp, AUD_PA_EXPANDER_PIN, IO_EXPANDER_OUTPUT);
    esp_io_expander_set_level(exp, AUD_PA_EXPANDER_PIN, 1);

    esp_err_t ret = i2s_bring_up();
    if (ret != ESP_OK) {
        // Partial-init unwind: we enabled the PA above; a failed I2S bring-up
        // would otherwise leave the speaker amp powered (current + buzz) with
        // no audio path. Turn it back off.
        esp_io_expander_set_level(exp, AUD_PA_EXPANDER_PIN, 0);
        return ret;
    }

    // Data interface (I2S) + control interface (I2C) for the codec.
    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = AUD_I2S_NUM,
        .tx_handle = s_tx_chan,
        .rx_handle = s_rx_chan,
    };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
    if (data_if == NULL) {
        ESP_LOGE(TAG, "audio_codec_new_i2s_data failed");
        goto codec_fail;
    }

    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = 0, // not used when bus_handle is set (new I2C master API)
        .addr = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = bus,
    };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    if (ctrl_if == NULL) {
        ESP_LOGE(TAG, "audio_codec_new_i2c_ctrl failed");
        goto codec_fail;
    }

    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    esp_codec_dev_hw_gain_t gain = {.pa_voltage = 5.0, .codec_dac_voltage = 3.3};
    es8311_codec_cfg_t es_cfg = {
        .ctrl_if = ctrl_if,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = GPIO_NUM_NC, // PA enabled via expander above, not here
        .pa_reverted = false,
        .master_mode = false,
        .use_mclk = false, // MCLK not wired; ES8311 runs off BCLK
        .digital_mic = false,
        .invert_mclk = false,
        .invert_sclk = false,
        .hw_gain = gain,
    };
    const audio_codec_if_t *es8311 = es8311_codec_new(&es_cfg);
    if (es8311 == NULL) {
        ESP_LOGE(TAG, "es8311_codec_new failed");
        goto codec_fail;
    }

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = es8311,
        .data_if = data_if,
    };
    s_codec = esp_codec_dev_new(&dev_cfg);
    if (s_codec == NULL) {
        ESP_LOGE(TAG, "esp_codec_dev_new failed");
        goto codec_fail;
    }

    esp_codec_dev_set_out_vol(s_codec, AUD_DEFAULT_VOL);
    ESP_LOGI(TAG, "ES8311 audio init success (I2S%d, %dHz, vol %d%%)",
             AUD_I2S_NUM, AUD_SAMPLE_RATE, AUD_DEFAULT_VOL);

    // Start the command-owner service now that the codec exists (review order
    // 5). Callers enqueue; this task is the sole codec driver.
    if (s_cmd_q == NULL) s_cmd_q = xQueueCreate(AUDIO_CMD_QUEUE_LEN, sizeof(audio_cmd_t));
    if (s_cmd_q == NULL) {
        ESP_LOGE(TAG, "audio: failed to create command queue");
        goto codec_fail;
    }
    if (xTaskCreate(audio_service_task, "audio_svc", 4096, NULL, 20, NULL) != pdPASS) {
        ESP_LOGE(TAG, "audio: failed to start service task");
        goto codec_fail;
    }
    return ESP_OK;

codec_fail:
    // Partial-init unwind: codec setup failed after I2S + PA were up.
    // Disable the PA and delete the I2S channels so a failed audio init
    // leaves no powered amp or leaked channels. (reached only via goto)
    esp_io_expander_set_level(exp, AUD_PA_EXPANDER_PIN, 0);
    if (s_rx_chan) { i2s_channel_disable(s_rx_chan); i2s_del_channel(s_rx_chan); s_rx_chan = NULL; }
    if (s_tx_chan) { i2s_channel_disable(s_tx_chan); i2s_del_channel(s_tx_chan); s_tx_chan = NULL; }
    return ESP_FAIL;
}

int audio_get_volume(void) {
    return (s_codec == NULL) ? -1 : s_volume;
}

int audio_adjust_volume(int delta) {
    if (s_codec == NULL) {
        return -1;
    }
    int v = s_volume + delta;
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    s_volume = v;
    esp_codec_dev_set_out_vol(s_codec, s_volume);
    ESP_LOGI(TAG, "volume -> %d%%", s_volume);
    return s_volume;
}

// Single owner of the codec handle inside this file: writes only ever
// happen through here, so a failed/short write is always noticed and the
// playback aborts instead of silently dropping audio.
static esp_err_t codec_write_checked(const int16_t *samples, size_t count) {
    size_t bytes = count * AUD_BYTES_PER_SAMPLE;
    // esp_codec_dev_write() returns a STATUS CODE, not a byte count:
    // ESP_CODEC_DEV_OK (0) means the whole buffer was written. The old code
    // compared the return against `bytes` and so rejected EVERY successful
    // write with ESP_ERR_INVALID_SIZE (260) — aborting playback on the first
    // 40 ms of silence (R1, 2026-10-10). The driver blocks until the full
    // buffer is queued, so there is no partial-write byte count to handle here.
    int ret = esp_codec_dev_write(s_codec, (void *)samples, bytes);
    if (ret != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "codec write failed: %d (%s)", ret,
                 esp_err_to_name(ret));
        return (ret < 0) ? ret : ESP_FAIL;
    }
    return ESP_OK;
}

// Shared teardown: exactly one close per open, and its result is checked
// (a failed close used to be reported as a successful playback). `first`
// carries the earliest error seen so callers can return it verbatim.
static esp_err_t playback_finalize(esp_err_t first) {
    esp_err_t close_ret = esp_codec_dev_close(s_codec);
    if (close_ret != ESP_OK) {
        ESP_LOGE(TAG, "codec close failed: 0x%x", close_ret);
        if (first == ESP_OK) {
            first = close_ret;
        }
    }
    s_playing = false;   // #3: playback fully drained + codec closed
    return first;
}

// #3: non-blocking playback-liveness query (declared in audio.h, was never
// defined). True while a tone/song is in flight; false once playback_finalize
// has drained the DMA and closed the codec.
bool audio_is_playing(void) {
    return s_playing;
}

// Stream `ms` of digital silence through the checked write path, giving
// up early if the cancel generation moved on. Returns ESP_OK when the
// whole run went out, or the first codec error. *canceled is set when we
// bailed because a stop was requested.
static esp_err_t render_silence(int16_t *buf, int chunk, uint32_t ms,
                                uint32_t generation, bool *canceled) {
    memset(buf, 0, (size_t)chunk * AUD_BYTES_PER_SAMPLE);
    const uint32_t total = (AUD_SAMPLE_RATE * ms) / 1000;
    for (uint32_t n = 0; n < total;) {
        if (s_stop_generation != generation) {
            *canceled = true;
            return ESP_OK;
        }
        const uint32_t remaining = total - n;
        const int c = (remaining < (uint32_t)chunk) ? (int)remaining : chunk;
        esp_err_t ret = codec_write_checked(buf, (size_t)c);
        if (ret != ESP_OK) {
            return ret;
        }
        n += (uint32_t)c;
    }
    return ESP_OK;
}

// Render one note into the chunk buffer and stream it. `phase` is
// threaded across notes by the caller so the waveform never jumps.
// Cancellation is tested once per chunk (not per note), so an interrupt
// takes effect within ~128 ms instead of at the end of the tune.
static esp_err_t render_note(int16_t *buf, int chunk, uint32_t ms,
                             uint32_t fade, float *phase, float w,
                             bool rest, uint32_t generation, bool *canceled) {
    const uint32_t total = (AUD_SAMPLE_RATE * ms) / 1000;
    for (uint32_t n = 0; n < total;) {
        if (s_stop_generation != generation) {
            *canceled = true;
            return ESP_OK;
        }
        const uint32_t remaining = total - n;
        const int c = (remaining < (uint32_t)chunk) ? (int)remaining : chunk;
        for (int i = 0; i < c; i++) {
            const uint32_t pos = n + (uint32_t)i;
            float env = 1.0f;
            if (pos < fade) {
                env = 0.5f * (1.0f - cosf((float)M_PI * (float)pos / (float)fade));
            } else if (pos > total - fade) {
                env = 0.5f * (1.0f - cosf((float)M_PI * (float)(total - pos) / (float)fade));
            }
            buf[i] = rest ? 0 : (int16_t)(sinf(*phase) * AUD_NOTE_AMPLITUDE * env);
            *phase += w;
            if (*phase > 2.0f * (float)M_PI) {
                *phase -= 2.0f * (float)M_PI;
            }
        }
        esp_err_t ret = codec_write_checked(buf, (size_t)c);
        if (ret != ESP_OK) {
            return ret;
        }
        n += (uint32_t)c;
    }
    return ESP_OK;
}

static esp_err_t audio_play_tone_locked(uint32_t freq_hz, uint32_t duration_ms) {
    if (s_codec == NULL) {
        ESP_LOGW(TAG, "audio not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel = 1,
        .sample_rate = AUD_SAMPLE_RATE,
    };
    esp_err_t ret = esp_codec_dev_open(s_codec, &fs);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "codec open failed: 0x%x", ret);
        return ret;   // V07: open failed -> playback never started, flag stays false
    }
    s_playing = true;   // V07: publish live only AFTER a successful open; playback_finalize() clears it

    // Mono 16-bit sine with raised-cosine fade in/out + drain before close
    // (per the audio analysis: hard edges + no drain = clicks).
    const int chunk_samples = 256;
    int16_t buf[chunk_samples];
    const uint32_t total_samples = (AUD_SAMPLE_RATE * duration_ms) / 1000;
    const uint32_t fade = AUD_SAMPLE_RATE / 100; // 10 ms
    const float w = 2.0f * (float)M_PI * (float)freq_hz / (float)AUD_SAMPLE_RATE;
    uint32_t n = 0;
    while (n < total_samples) {
        int count = chunk_samples;
        if ((uint32_t)count > total_samples - n) {
            count = (int)(total_samples - n);
        }
        for (int i = 0; i < count; i++) {
            uint32_t pos = n + i;
            float env = 1.0f;
            if (pos < fade) {
                env = 0.5f * (1.0f - cosf((float)M_PI * (float)pos / (float)fade));
            } else if (pos > total_samples - fade) {
                env = 0.5f * (1.0f - cosf((float)M_PI * (float)(total_samples - pos) / (float)fade));
            }
            buf[i] = (int16_t)(sinf(w * (float)pos) * 10000.0f * env);
        }
        esp_err_t wret = codec_write_checked(buf, (size_t)count);
        if (wret != ESP_OK) {
            (void)playback_finalize(wret);
            return wret;
        }
        n += count;
    }
    // trailing silence + drain
    for (int i = 0; i < chunk_samples; i++) buf[i] = 0;
    esp_err_t err = ESP_OK;
    for (int r = 0; r < 3; r++) {
        err = codec_write_checked(buf, (size_t)chunk_samples);
        if (err != ESP_OK) break;
    }
    // Always give the DMA the time it needs, even on an error path,
    // otherwise close() truncates mid-buffer.
    vTaskDelay(pdMS_TO_TICKS(AUD_DRAIN_DELAY_MS));

    err = playback_finalize(err);
    if (err != ESP_OK) {
        return err;
    }
    ESP_LOGI(TAG, "played %luHz tone for %lums", (unsigned long)freq_hz,
             (unsigned long)duration_ms);
    return ESP_OK;
}

// V03: public entry. Serialize the whole open->write->drain->close session so
// the startup tone and a UI song press can never own the codec at once.
esp_err_t audio_play_tone(uint32_t freq_hz, uint32_t duration_ms) {
    if (s_play_mutex == NULL) return ESP_ERR_INVALID_STATE;   // audio_init not run
    if (xSemaphoreTake(s_play_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        ESP_LOGW(TAG, "audio busy -- tone request rejected (another session active)");
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t r = audio_play_tone_locked(freq_hz, duration_ms);
    xSemaphoreGive(s_play_mutex);
    return r;
}

// Note frequencies (Hz).
#define NOTE_C4 262
#define NOTE_D4 294
#define NOTE_E4 330
#define NOTE_F4 349
#define NOTE_G4 392
#define NOTE_GS4 415
#define NOTE_A4 440
#define NOTE_AS4 466
#define NOTE_B4 494
#define NOTE_C5 523
#define NOTE_D5 587
#define NOTE_E5 659
#define NOTE_F5 698
#define NOTE_G5 784

// Note durations (ms). Q=quarter, E=eighth, H=half, D=dotted-quarter.
#define Q 300
#define E 150
#define H 600
#define D 450

// ---- Public-domain / traditional melodies (NO copyrighted songs) ----

static const audio_note_t SONG_HAPPY_BIRTHDAY[] = {
    {NOTE_C4, E}, {NOTE_C4, E}, {NOTE_D4, Q}, {NOTE_C4, Q}, {NOTE_F4, Q}, {NOTE_E4, H},
    {NOTE_C4, E}, {NOTE_C4, E}, {NOTE_D4, Q}, {NOTE_C4, Q}, {NOTE_G4, Q}, {NOTE_F4, H},
    {NOTE_C4, E}, {NOTE_C4, E}, {NOTE_C5, Q}, {NOTE_A4, Q}, {NOTE_F4, Q}, {NOTE_E4, Q}, {NOTE_D4, D},
    {NOTE_AS4, E}, {NOTE_AS4, E}, {NOTE_A4, Q}, {NOTE_F4, Q}, {NOTE_G4, Q}, {NOTE_F4, H},
};

static const audio_note_t SONG_TWINKLE[] = {
    {NOTE_C4, Q}, {NOTE_C4, Q}, {NOTE_G4, Q}, {NOTE_G4, Q}, {NOTE_A4, Q}, {NOTE_A4, Q}, {NOTE_G4, H},
    {NOTE_F4, Q}, {NOTE_F4, Q}, {NOTE_E4, Q}, {NOTE_E4, Q}, {NOTE_D4, Q}, {NOTE_D4, Q}, {NOTE_C4, H},
    {NOTE_G4, Q}, {NOTE_G4, Q}, {NOTE_F4, Q}, {NOTE_F4, Q}, {NOTE_E4, Q}, {NOTE_E4, Q}, {NOTE_D4, H},
    {NOTE_G4, Q}, {NOTE_G4, Q}, {NOTE_F4, Q}, {NOTE_F4, Q}, {NOTE_E4, Q}, {NOTE_E4, Q}, {NOTE_D4, H},
    {NOTE_C4, Q}, {NOTE_C4, Q}, {NOTE_G4, Q}, {NOTE_G4, Q}, {NOTE_A4, Q}, {NOTE_A4, Q}, {NOTE_G4, H},
    {NOTE_F4, Q}, {NOTE_F4, Q}, {NOTE_E4, Q}, {NOTE_E4, Q}, {NOTE_D4, Q}, {NOTE_D4, Q}, {NOTE_C4, H},
};

// "Ode to Joy" theme (Beethoven, public domain).
static const audio_note_t SONG_ODE_TO_JOY[] = {
    {NOTE_E4, Q}, {NOTE_E4, Q}, {NOTE_F4, Q}, {NOTE_G4, Q}, {NOTE_G4, Q}, {NOTE_F4, Q}, {NOTE_E4, Q}, {NOTE_D4, Q},
    {NOTE_C4, Q}, {NOTE_C4, Q}, {NOTE_D4, Q}, {NOTE_E4, Q}, {NOTE_E4, D}, {NOTE_D4, E}, {NOTE_D4, H},
    {NOTE_E4, Q}, {NOTE_E4, Q}, {NOTE_F4, Q}, {NOTE_G4, Q}, {NOTE_G4, Q}, {NOTE_F4, Q}, {NOTE_E4, Q}, {NOTE_D4, Q},
    {NOTE_C4, Q}, {NOTE_C4, Q}, {NOTE_D4, Q}, {NOTE_E4, Q}, {NOTE_D4, D}, {NOTE_C4, E}, {NOTE_C4, H},
};

// "Jingle Bells" chorus (traditional, public domain).
static const audio_note_t SONG_JINGLE_BELLS[] = {
    {NOTE_E4, Q}, {NOTE_E4, Q}, {NOTE_E4, H},
    {NOTE_E4, Q}, {NOTE_E4, Q}, {NOTE_E4, H},
    {NOTE_E4, Q}, {NOTE_G4, Q}, {NOTE_C4, Q}, {NOTE_D4, Q}, {NOTE_E4, H},
    {NOTE_F4, Q}, {NOTE_F4, Q}, {NOTE_F4, Q}, {NOTE_F4, Q}, {NOTE_F4, Q}, {NOTE_E4, Q}, {NOTE_E4, Q}, {NOTE_E4, E}, {NOTE_E4, E},
    {NOTE_G4, Q}, {NOTE_G4, Q}, {NOTE_F4, Q}, {NOTE_D4, Q}, {NOTE_C4, H},
};

// "Für Elise" opening motif (Beethoven, public domain).
static const audio_note_t SONG_FUR_ELISE[] = {
    {NOTE_E5, E}, {NOTE_D5, E}, {NOTE_E5, E}, {NOTE_D5, E}, {NOTE_E5, E}, {NOTE_B4, E}, {NOTE_D5, E}, {NOTE_C5, E},
    {NOTE_A4, Q}, {0, E}, {NOTE_C4, E}, {NOTE_E4, E}, {NOTE_A4, E}, {NOTE_B4, Q}, {0, E}, {NOTE_E4, E},
    {NOTE_GS4, E}, {NOTE_B4, E}, {NOTE_C5, Q},
};

static const audio_song_t SONGS[] = {
    {"Happy Birthday", SONG_HAPPY_BIRTHDAY, sizeof(SONG_HAPPY_BIRTHDAY) / sizeof(audio_note_t)},
    {"Twinkle Twinkle", SONG_TWINKLE, sizeof(SONG_TWINKLE) / sizeof(audio_note_t)},
    {"Ode to Joy", SONG_ODE_TO_JOY, sizeof(SONG_ODE_TO_JOY) / sizeof(audio_note_t)},
    {"Jingle Bells", SONG_JINGLE_BELLS, sizeof(SONG_JINGLE_BELLS) / sizeof(audio_note_t)},
    {"Fur Elise", SONG_FUR_ELISE, sizeof(SONG_FUR_ELISE) / sizeof(audio_note_t)},
};
#define N_SONGS ((int)(sizeof(SONGS) / sizeof(SONGS[0])))

int audio_song_count(void) {
    return N_SONGS;
}

const audio_song_t *audio_get_song(int index) {
    if (index < 0 || index >= N_SONGS) {
        return NULL;
    }
    return &SONGS[index];
}

static esp_err_t audio_play_song_locked(int index) {
    if (s_codec == NULL) {
        ESP_LOGW(TAG, "audio not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    const audio_song_t *song = audio_get_song(index);
    if (song == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const audio_note_t *tune = song->notes;
    const size_t n_notes = song->count;
    // Snapshot the cancel generation ONCE, before opening. Anything that
    // bumps it from now on is a cancel aimed at *this* playback; we never
    // clear it, so a request that races the start cannot be lost.
    const uint32_t my_generation = s_stop_generation;
    bool canceled = false;

    // Click-free engine (the hard-won fix -- see README "the crackle
    // saga"): raised-cosine per-note envelope + continuous phase across
    // notes + large write chunks + leading/trailing silence + DMA drain
    // before close.
    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel = 1,
        .sample_rate = AUD_SAMPLE_RATE,
    };
    esp_err_t ret = esp_codec_dev_open(s_codec, &fs);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "codec open failed: 0x%x", ret);
        return ret;
    }
    s_playing = true;   // V07: publish live only AFTER a successful open; playback_finalize() clears it
    // Every write below is checked and every exit funnels through
    // playback_finalize(), so the codec is closed exactly once and the
    // first error observed is what the caller gets back.
    esp_err_t first_err = ESP_OK;

    const int chunk = AUD_CHUNK_SAMPLES;
    static int16_t buf[AUD_CHUNK_SAMPLES];
    const uint32_t fade = (AUD_SAMPLE_RATE * AUD_NOTE_FADE_MS) / 1000;

    // Leading silence.
    first_err = render_silence(buf, chunk, AUD_SONG_PAD_MS, my_generation, &canceled);
    if (first_err != ESP_OK) {
        (void)playback_finalize(first_err);
        return first_err;
    }

    float phase = 0.0f; // continuous phase carried across notes (no reset-jump)
    for (size_t k = 0; k < n_notes && !canceled; k++) {
        const bool rest = (tune[k].freq_hz == 0);
        const float w = rest
                            ? 0.0f
                            : 2.0f * (float)M_PI * (float)tune[k].freq_hz / (float)AUD_SAMPLE_RATE;
        first_err = render_note(buf, chunk, tune[k].ms, fade, &phase, w, rest,
                                my_generation, &canceled);
        if (first_err != ESP_OK) {
            break;
        }
        if (canceled) {
            break; // interrupted -- skip the inter-note gap below
        }
        first_err = render_silence(buf, chunk, AUD_NOTE_GAP_MS, my_generation, &canceled);
        if (first_err != ESP_OK) {
            break;
        }
    }

    if (first_err == ESP_OK) {
        if (canceled) {
            ESP_LOGI(TAG, "song '%s' canceled", song->name);
            // A cancel is not a failure: return without a spurious error,
            // but still drain + close cleanly so the next song starts on a
            // quiet codec. Without the drain the codec is closed with
            // samples still queued in the I2S DMA, truncating the tail
            // (the same click the non-cancel path drains to avoid).
            vTaskDelay(pdMS_TO_TICKS(AUD_DRAIN_DELAY_MS));
            (void)playback_finalize(ESP_OK);
            return ESP_OK;
        }
        // Trailing silence, then let the DMA drain before close (README).
        first_err = render_silence(buf, chunk, AUD_SONG_PAD_MS, my_generation, &canceled);
        vTaskDelay(pdMS_TO_TICKS(AUD_DRAIN_DELAY_MS));
    }

    esp_err_t result = playback_finalize(first_err);
    if (result != ESP_OK) {
        return result;
    }
    ESP_LOGI(TAG, "played song '%s'", song->name);
    return ESP_OK;
}

// V03: public entry. Same codec-ownership serialization as audio_play_tone.
esp_err_t audio_play_song(int index) {
    if (s_play_mutex == NULL) return ESP_ERR_INVALID_STATE;   // audio_init not run
    if (xSemaphoreTake(s_play_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        ESP_LOGW(TAG, "audio busy -- song request rejected (another session active)");
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t r = audio_play_song_locked(index);
    xSemaphoreGive(s_play_mutex);
    return r;
}

// ---------------------------------------------------------------------------
// Command-owner service (review order 5). The ONLY task that drives the codec.
// ---------------------------------------------------------------------------
static void audio_service_task(void *arg) {
    (void)arg;
    for (;;) {
        audio_cmd_t c;
        if (xQueueReceive(s_cmd_q, &c, portMAX_DELAY) != pdTRUE) continue;
        switch (c.type) {
            case AUDIO_CMD_TONE: {
                audio_on_start(&s_status, false, -1);
                audio_on_playing(&s_status);
                esp_err_t r = audio_play_tone(c.arg, c.arg2); // takes the play mutex
                audio_on_finished(&s_status, (r == ESP_OK) ? 0 : (int32_t)r);
                break;
            }
            case AUDIO_CMD_SONG: {
                audio_on_start(&s_status, true, c.arg);
                audio_on_playing(&s_status);
                esp_err_t r = audio_play_song(c.arg);         // honors cancel generation
                audio_on_finished(&s_status, (r == ESP_OK) ? 0 : (int32_t)r);
                break;
            }
            case AUDIO_CMD_STOP:
                audio_on_stopping(&s_status);
                audio_request_stop();   // async; the in-flight song unwinds + finalizes
                break;
            case AUDIO_CMD_SET_VOLUME: {
                int nv = audio_adjust_volume(c.arg); // c.arg is a delta
                portENTER_CRITICAL(&s_status_mux);
                if (nv >= 0) s_status.volume = nv;
                portEXIT_CRITICAL(&s_status_mux);
                break;
            }
            case AUDIO_CMD_MUTE: {
                bool mute = (c.arg != 0);
                // Mute = set volume 0 but remember the prior level to restore.
                static int s_premute_vol = -1;
                if (mute) {
                    int cur = audio_get_volume();
                    if (cur > 0) s_premute_vol = cur;
                    audio_adjust_volume(-100);   // clamps to 0
                } else if (s_premute_vol > 0) {
                    audio_adjust_volume(s_premute_vol - audio_get_volume());
                }
                portENTER_CRITICAL(&s_status_mux);
                s_status.muted = mute;
                s_status.volume = audio_get_volume() < 0 ? s_status.volume : audio_get_volume();
                portEXIT_CRITICAL(&s_status_mux);
                break;
            }
            default: break;
        }
    }
}

// Enqueue a command (non-blocking). Latest-play policy for tone/song is applied
// by the service via audio_request_stop() before the new session. Returns false
// if audio isn't initialized or the queue is full. Safe from ANY task.
static bool audio_post(audio_cmd_type_t t, int32_t arg, int32_t arg2) {
    if (s_cmd_q == NULL) return false;
    if (!audio_cmd_accept(&s_status, t)) return false;
    // For a new tone/song, ask any in-flight song to stop first (latest-play).
    if ((t == AUDIO_CMD_TONE || t == AUDIO_CMD_SONG) && audio_is_active(&s_status)) {
        audio_request_stop();
    }
    audio_cmd_t c = { .type = t, .arg = arg, .arg2 = arg2,
                      .id = __atomic_fetch_add(&s_cmd_next_id, 1, __ATOMIC_RELAXED) };
    portENTER_CRITICAL(&s_status_mux);
    s_status.last_cmd_id = c.id;
    portEXIT_CRITICAL(&s_status_mux);
    return xQueueSend(s_cmd_q, &c, 0) == pdTRUE;
}

bool audio_post_song(int index)        { return audio_post(AUDIO_CMD_SONG, index, 0); }
bool audio_post_tone(uint32_t f, uint32_t ms) { return audio_post(AUDIO_CMD_TONE, (int32_t)f, (int32_t)ms); }
bool audio_post_stop(void)             { return audio_post(AUDIO_CMD_STOP, 0, 0); }
bool audio_post_volume(int delta)      { return audio_post(AUDIO_CMD_SET_VOLUME, delta, 0); }
bool audio_post_mute(bool mute)        { return audio_post(AUDIO_CMD_MUTE, mute ? 1 : 0, 0); }

void audio_get_status(audio_status_t *out) {
    if (!out) return;
    portENTER_CRITICAL(&s_status_mux);
    *out = s_status;
    portEXIT_CRITICAL(&s_status_mux);
}
