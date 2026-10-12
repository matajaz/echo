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
#include "esp_http_client.h"
#include "secrets.h"
#include "board_services.h"
#include "wav_stream.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "audio";

// ESP-side directions from the board schematic, not the inconsistent vendor
// example header: ES8311 pin 9 DSDIN <- GPIO27; pin 7 ASDOUT -> GPIO26.
// https://github.com/waveshareteam/ESP32-C5-Touch-LCD-2.8/blob/main/schematic/ESP32-C5-Touch-LCD-2.8.pdf
#define AUD_I2S_NUM I2S_NUM_0
#define AUD_I2S_MCLK I2S_GPIO_UNUSED
#define AUD_I2S_SCLK GPIO_NUM_24
#define AUD_I2S_WS GPIO_NUM_25
#define AUD_I2S_DOUT GPIO_NUM_27
#define AUD_I2S_DSIN GPIO_NUM_26

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
static bool s_muted = false;   // N04: applied gain = muted ? 0 : s_volume (desired)

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
static volatile bool s_service_ready = false;   // N05: true only once the service task is live
static audio_status_t s_status;                 // owned by the service task
static portMUX_TYPE s_status_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_cmd_next_id = 1;
#define AUDIO_CMD_QUEUE_LEN 6
static void audio_service_task(void *arg);
static audio_cmd_t s_pending;
static bool s_pending_valid;
static int s_pa_result = -1;
static audio_path_diag_t s_path;
static void status_start(bool song, int index) {
    portENTER_CRITICAL(&s_status_mux);
    audio_on_start(&s_status, song, index);
    portEXIT_CRITICAL(&s_status_mux);
}
static void status_playing(void) {
    portENTER_CRITICAL(&s_status_mux);
    audio_on_playing(&s_status);
    portEXIT_CRITICAL(&s_status_mux);
}
static void status_finished(esp_err_t error) {
    portENTER_CRITICAL(&s_status_mux);
    audio_on_finished(&s_status, error);
    portEXIT_CRITICAL(&s_status_mux);
}
static void status_gain(void) {
    portENTER_CRITICAL(&s_status_mux);
    s_status.volume = s_volume; s_status.muted = s_muted;
    portEXIT_CRITICAL(&s_status_mux);
}
static void audio_capture_pa(const char *stage) {
    esp_io_expander_handle_t exp=board_io_expander();
    uint32_t input=0, output=0, direction=0;
    if (!exp) return;
    esp_err_t read=esp_io_expander_get_level(exp,0xff,&input);
    // These two callbacks expose the driver's command shadows, not physical
    // readback. Log all pins so INPUT can be compared with known reset outputs.
    exp->read_output_reg(exp,&output);
    exp->read_direction_reg(exp,&direction);
    portENTER_CRITICAL(&s_status_mux);
    s_path.pa_level=read==ESP_OK?((input&AUD_PA_EXPANDER_PIN)?1:0):-1;
    portEXIT_CRITICAL(&s_status_mux);
    ESP_LOGI(TAG,"PA %s: input=%02lx commanded=%02lx direction=%02lx read=%d",
        stage,(unsigned long)input,(unsigned long)output,(unsigned long)direction,(int)read);
}
static esp_err_t audio_enable_pa(void) {
    esp_io_expander_handle_t exp=board_io_expander();
    esp_err_t result=exp?esp_io_expander_set_dir(exp,AUD_PA_EXPANDER_PIN,IO_EXPANDER_OUTPUT):ESP_ERR_INVALID_STATE;
    if (result==ESP_OK) result=board_pa_enable(true);
    uint32_t level=0;
    esp_err_t read=result==ESP_OK?esp_io_expander_get_level(exp,AUD_PA_EXPANDER_PIN,&level):result;
    portENTER_CRITICAL(&s_status_mux);
    s_path.pa_error=result;
    s_path.pa_level=read==ESP_OK?((level&AUD_PA_EXPANDER_PIN)?1:0):-1;
    portEXIT_CRITICAL(&s_status_mux);
    __atomic_store_n(&s_pa_result,result==ESP_OK?1:0,__ATOMIC_RELEASE);
    if (result==ESP_OK) audio_capture_pa("settled");
    return result;
}
static void audio_capture_path(uint32_t rate) {
    // Run ONLY on the codec owner while the device is open. The HTTP handler
    // used to sample powered-down registers before open and disturb scheduling.
    int power=-1,dac=-1,vol=-1,clock1=-1,clock2=-1,format=-1,mute=-1;
    esp_codec_dev_read_reg(s_codec,0x0D,&power);
    esp_codec_dev_read_reg(s_codec,0x12,&dac);
    esp_codec_dev_read_reg(s_codec,0x32,&vol);
    esp_codec_dev_read_reg(s_codec,0x01,&clock1);
    esp_codec_dev_read_reg(s_codec,0x02,&clock2);
    esp_codec_dev_read_reg(s_codec,0x09,&format);
    esp_codec_dev_read_reg(s_codec,0x31,&mute);
    portENTER_CRITICAL(&s_status_mux);
    s_path.sample_rate=rate;
    s_path.codec_regs=power<0 || dac<0 || vol<0?-1:((power&255)<<16)|((dac&255)<<8)|(vol&255);
    s_path.clock1=clock1; s_path.clock2=clock2; s_path.format=format; s_path.mute_reg=mute;
    portEXIT_CRITICAL(&s_status_mux);
    ESP_LOGI(TAG,"live path: pa=%d power=%02x dac=%02x vol=%02x clk=%02x/%02x fmt=%02x mute=%02x",
        s_path.pa_level,power,dac,vol,clock1,clock2,format,mute);
}


void audio_request_stop(void) {
    __atomic_add_fetch(&s_stop_generation, 1, __ATOMIC_ACQ_REL); // invalidate whatever is currently playing
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
    portENTER_CRITICAL(&s_status_mux);
    audio_status_init(&s_status, AUD_DEFAULT_VOL);
    portEXIT_CRITICAL(&s_status_mux);
    i2c_master_bus_handle_t bus = display_get_i2c_bus();
    esp_io_expander_handle_t exp = display_get_io_expander();
    if (bus == NULL || exp == NULL) {
        ESP_LOGE(TAG, "I2C bus / expander not ready -- call display_init() first");
        return ESP_ERR_INVALID_STATE;
    }

    // Keep the amplifier off until the audio owner starts playback.
    // The ES8311's own pa_pin is NC on this board.
    esp_err_t pa_off=board_pa_enable(false);
    if (pa_off!=ESP_OK) return pa_off;

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
    // 5). N05: do NOT treat s_cmd_q/s_codec as readiness — publish readiness
    // (s_service_ready) ONLY after the consumer task actually exists, and fully
    // unwind on failure so a retry re-creates everything.
    if (s_cmd_q == NULL) s_cmd_q = xQueueCreate(AUDIO_CMD_QUEUE_LEN, sizeof(audio_cmd_t));
    if (s_cmd_q == NULL) {
        ESP_LOGE(TAG, "audio: failed to create command queue");
        goto codec_fail;
    }
    if (xTaskCreate(audio_service_task, "audio_svc", 4096, NULL, 20, NULL) != pdPASS) {
        ESP_LOGE(TAG, "audio: failed to start service task");
        goto codec_fail;
    }
    portENTER_CRITICAL(&s_status_mux);
    s_status.ready = true;
    portEXIT_CRITICAL(&s_status_mux);
    __atomic_store_n(&s_service_ready, true, __ATOMIC_RELEASE);
    // The amplifier was switched off before the service was created. Once
    // ready is published only the service may change it.
    return ESP_OK;

codec_fail:
    // Partial-init unwind (N05 extended): tear down EVERYTHING published so a
    // retry of audio_init() rebuilds a complete service (not an early return on
    // a half-built one), and so no command can be accepted into a queue with no
    // consumer. Order: PA off -> delete queue -> delete codec dev -> delete I2S
    // channels -> NULL every handle.
    esp_io_expander_set_level(exp, AUD_PA_EXPANDER_PIN, 0);
    if (s_cmd_q) { vQueueDelete(s_cmd_q); s_cmd_q = NULL; }
    if (s_codec) { esp_codec_dev_delete(s_codec); s_codec = NULL; }
    if (s_rx_chan) { i2s_channel_disable(s_rx_chan); i2s_del_channel(s_rx_chan); s_rx_chan = NULL; }
    if (s_tx_chan) { i2s_channel_disable(s_tx_chan); i2s_del_channel(s_tx_chan); s_tx_chan = NULL; }
    __atomic_store_n(&s_service_ready, false, __ATOMIC_RELEASE);
    return ESP_FAIL;
}

int audio_get_volume(void) {
    audio_status_t status;
    audio_get_status(&status);
    return status.ready ? status.volume : -1;
}

// N04: apply the correct hardware gain = muted ? 0 : desired. Single place that
// writes the codec gain, so mute and volume can never disagree.
static void audio_apply_gain(void) {
    if (s_codec == NULL) return;
    int applied = s_muted ? 0 : s_volume;
    esp_codec_dev_set_out_vol(s_codec, applied);
}

int audio_adjust_volume(int delta) {
    if (s_codec == NULL) {
        return -1;
    }
    int v = s_volume + delta;
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    s_volume = v;                 // update DESIRED
    audio_apply_gain();
    status_gain();
    ESP_LOGI(TAG, "volume -> %d%% (desired; %s)", s_volume, s_muted ? "muted" : "active");
    return s_volume;
}

// N04: idempotent mute. Muted => applied gain 0 but DESIRED volume retained;
// unmute restores the desired volume. A volume change while muted updates
// desired only (hardware stays 0) via audio_apply_gain().
static void audio_set_muted(bool mute) {
    if (s_codec == NULL) return;
    s_muted = mute;               // idempotent: setting the same value re-applies safely
    audio_apply_gain();
    status_gain();
    ESP_LOGI(TAG, "mute -> %s (desired vol %d%%)", mute ? "ON" : "OFF", s_volume);
}

// Consumer-owned pending slot. Never put a removed command back into a queue
// that another producer can fill. Newer Play wins; a later Stop clears it.
static void audio_apply_control(const audio_cmd_t *c) {
    if (c->type == AUDIO_CMD_SET_VOLUME) audio_adjust_volume(c->arg);
    else if (c->type == AUDIO_CMD_VOLUME_ABSOLUTE) audio_adjust_volume(c->arg - s_volume);
    else if (c->type == AUDIO_CMD_MUTE) audio_set_muted(c->arg != 0);
    else if (c->type == AUDIO_CMD_STOP) {
        s_pending_valid = false;
        audio_request_stop();
        portENTER_CRITICAL(&s_status_mux);
        audio_on_stopping(&s_status);
        portEXIT_CRITICAL(&s_status_mux);
    }
}
static void audio_pump_controls(void) {
    if (!s_cmd_q) return;
    audio_cmd_t c;
    for (int budget = AUDIO_CMD_QUEUE_LEN; budget > 0; --budget) {
        if (xQueueReceive(s_cmd_q, &c, 0) != pdTRUE) break;
        if (c.type == AUDIO_CMD_TONE || c.type == AUDIO_CMD_SONG || c.type == AUDIO_CMD_WAV) {
            s_pending = c;
            s_pending_valid = true;
            audio_request_stop();
        } else audio_apply_control(&c);
    }
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
    uint32_t nonzero=0,peak=0;
    for (size_t i=0;i<count;++i) {
        int value=samples[i]; uint32_t magnitude=value<0?(uint32_t)-value:(uint32_t)value;
        if (value) ++nonzero;
        if (magnitude>peak) peak=magnitude;
    }
    int ret = esp_codec_dev_write(s_codec, (void *)samples, bytes);
    if (ret != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "codec write failed: %d (%s)", ret,
                 esp_err_to_name(ret));
        return (ret < 0) ? ret : ESP_FAIL;
    }
    portENTER_CRITICAL(&s_status_mux);
    bool first_signal=s_path.nonzero_samples==0 && nonzero!=0;
    s_path.samples+=count; s_path.nonzero_samples+=nonzero;
    if (peak>s_path.peak) s_path.peak=peak;
    portEXIT_CRITICAL(&s_status_mux);
    if (first_signal) audio_capture_pa("PCM queued");
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
    __atomic_store_n(&s_playing, false, __ATOMIC_RELEASE);
    esp_err_t pa_off=board_pa_enable(false);
    if (first==ESP_OK && pa_off!=ESP_OK) first=pa_off;
    return first;
}

// #3: non-blocking playback-liveness query (declared in audio.h, was never
// defined). True while a tone/song is in flight; false once playback_finalize
// has drained the DMA and closed the codec.
bool audio_is_playing(void) {
    return __atomic_load_n(&s_playing, __ATOMIC_ACQUIRE);
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
        audio_pump_controls();
        if (__atomic_load_n(&s_stop_generation, __ATOMIC_ACQUIRE) != generation) {
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
        audio_pump_controls();
        if (__atomic_load_n(&s_stop_generation, __ATOMIC_ACQUIRE) != generation) {
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

    const uint32_t generation = __atomic_load_n(&s_stop_generation, __ATOMIC_ACQUIRE);
    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel = 1,
        .sample_rate = AUD_SAMPLE_RATE,
    };
    esp_err_t ret = esp_codec_dev_open(s_codec, &fs);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "codec open failed: 0x%x", ret);
        // N11: esp_codec_dev sets output_opened BEFORE set_fs/enable and does
        // NOT clear it on their failure, so a later open would short-circuit
        // and skip reconfiguration. Close/reset here so the next open fully
        // reconfigures. (Harmless if the device never actually opened.)
        esp_codec_dev_close(s_codec);
        return ret;   // V07: open failed -> playback never started, flag stays false
    }
    audio_capture_path(AUD_SAMPLE_RATE);
    __atomic_store_n(&s_playing, true, __ATOMIC_RELEASE); status_playing();   // V07: publish live only AFTER a successful open; playback_finalize() clears it

    // Mono 16-bit sine with raised-cosine fade in/out + drain before close
    // (per the audio analysis: hard edges + no drain = clicks).
    const int chunk_samples = 256;
    int16_t buf[chunk_samples];
    const uint32_t total_samples = (AUD_SAMPLE_RATE * duration_ms) / 1000;
    const uint32_t fade = AUD_SAMPLE_RATE / 100; // 10 ms
    const float w = 2.0f * (float)M_PI * (float)freq_hz / (float)AUD_SAMPLE_RATE;
    uint32_t n = 0;
    while (n < total_samples) {
        audio_pump_controls();
        if (__atomic_load_n(&s_stop_generation, __ATOMIC_ACQUIRE) != generation) break;
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

#include "songs_generated.h"   // 12 classic melodies (tools/gen_songs.py)

static const audio_song_t SONGS[] = {
    {"Happy Birthday", SONG_HAPPY_BIRTHDAY, sizeof(SONG_HAPPY_BIRTHDAY) / sizeof(audio_note_t)},
    {"Twinkle Twinkle", SONG_TWINKLE, sizeof(SONG_TWINKLE) / sizeof(audio_note_t)},
    {"Ode to Joy", SONG_ODE_TO_JOY, sizeof(SONG_ODE_TO_JOY) / sizeof(audio_note_t)},
    {"Jingle Bells", SONG_JINGLE_BELLS, sizeof(SONG_JINGLE_BELLS) / sizeof(audio_note_t)},
    {"Fur Elise", SONG_FUR_ELISE, sizeof(SONG_FUR_ELISE) / sizeof(audio_note_t)},
    GENERATED_SONGS
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
    const uint32_t my_generation = __atomic_load_n(&s_stop_generation, __ATOMIC_ACQUIRE);
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
        esp_codec_dev_close(s_codec);   // N11: clear stale output_opened so retry reconfigures
        return ret;
    }
    audio_capture_path(AUD_SAMPLE_RATE);
    __atomic_store_n(&s_playing, true, __ATOMIC_RELEASE); status_playing();   // V07: publish live only AFTER a successful open; playback_finalize() clears it
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
        // N02: pump CONTROL commands (volume/mute/stop) between notes so they
        // apply with bounded latency (~1 note) instead of waiting for the whole
        // tune. A STOP found here cancels THIS song via the generation counter.
        audio_pump_controls();
        if (__atomic_load_n(&s_stop_generation, __ATOMIC_ACQUIRE) != my_generation) {
            canceled = true;
            break;
        }
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
// WAV streaming: bounded incremental header, command-owned name, exact data
// length and a carried byte across fragmented network reads.
#define WAV_HEADER_MAX 16384
#define WAV_RX_BYTES 2048

static esp_err_t audio_play_wav_locked(const char *name) {
    if (!s_codec) return ESP_ERR_INVALID_STATE;
    const uint32_t generation = __atomic_load_n(&s_stop_generation, __ATOMIC_ACQUIRE);
    char url[192];
    int len = snprintf(url, sizeof(url), "%s%s", WAV_STREAM_PREFIX, name);
    if (len < 0 || (size_t)len >= sizeof(url)) return ESP_ERR_INVALID_SIZE;
    uint8_t *rx = heap_caps_malloc(WAV_HEADER_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    int16_t *pcm = heap_caps_malloc((WAV_RX_BYTES + 2), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!rx || !pcm) { free(rx); free(pcm); return ESP_ERR_NO_MEM; }
    esp_http_client_config_t cfg = { .url = url, .timeout_ms = 1000, .disable_auto_redirect = true };
    esp_http_client_handle_t cl = esp_http_client_init(&cfg);
    esp_err_t err = ESP_FAIL;
    bool opened = false;
    if (!cl) goto done;
    if (esp_http_client_open(cl, 0) != ESP_OK) goto done;
    if (esp_http_client_fetch_headers(cl) < 0 || esp_http_client_get_status_code(cl) != 200) goto done;
    wav_info_t info = {0};
    size_t have = 0;
    wav_result_t result = WAV_MORE;
    while (have < WAV_HEADER_MAX && result == WAV_MORE) {
        audio_pump_controls();
        if (__atomic_load_n(&s_stop_generation, __ATOMIC_ACQUIRE) != generation) { err = ESP_OK; goto done; }
        int n = esp_http_client_read(cl, (char *)rx + have, WAV_HEADER_MAX-have<256?WAV_HEADER_MAX-have:256);
        if (n <= 0) goto done;
        have += (size_t)n;
        result = wav_header(rx, have, &info);
    }
    if (result != WAV_READY) { err = ESP_ERR_NOT_SUPPORTED; goto done; }
    esp_codec_dev_sample_info_t fs = { .bits_per_sample = 16, .channel = 1, .sample_rate = info.rate };
    err = esp_codec_dev_open(s_codec, &fs);
    if (err != ESP_OK) { esp_codec_dev_close(s_codec); goto done; }
    opened = true;
    audio_capture_path(info.rate);
    __atomic_store_n(&s_playing, true, __ATOMIC_RELEASE);
    status_playing();
    wav_fragment_t fragment = {0};
    uint32_t remaining = info.bytes;
    size_t buffered = have - info.offset;
    const uint8_t *src = rx + info.offset;
    err = ESP_OK;
    while (remaining && err == ESP_OK) {
        audio_pump_controls();
        if (__atomic_load_n(&s_stop_generation, __ATOMIC_ACQUIRE) != generation) break;
        size_t n;
        if (buffered) { n = buffered; buffered = 0; }
        else {
            int got = esp_http_client_read(cl, (char *)rx, remaining < WAV_RX_BYTES ? remaining : WAV_RX_BYTES);
            if (got <= 0) { err = ESP_ERR_INVALID_SIZE; break; }
            n = (size_t)got; src = rx;
        }
        if (n > remaining) n = remaining;
        // Header is read in 256-byte increments, so the initial PCM prefix is
        // bounded too. Stream body chunks are bounded by WAV_RX_BYTES.
        size_t samples = wav_samples(&fragment, src, n, pcm);
        if (samples) err = codec_write_checked(pcm, samples);
        remaining -= n;
    }
    if (err == ESP_OK && !remaining && fragment.have_byte) err = ESP_ERR_INVALID_SIZE;
    if (opened) {
        memset(pcm, 0, 256 * sizeof(*pcm));
        esp_err_t tail = codec_write_checked(pcm, 256);
        if (err == ESP_OK) err = tail;
        vTaskDelay(pdMS_TO_TICKS(AUD_DRAIN_DELAY_MS));
        err = playback_finalize(err);
    }
done:
    if (cl) esp_http_client_cleanup(cl);
    free(rx); free(pcm);
    return err;
}

static void audio_service_task(void *arg) {
    (void)arg;
    for (;;) {
        audio_cmd_t c;
        if (s_pending_valid) { c = s_pending; s_pending_valid = false; }
        else if (xQueueReceive(s_cmd_q, &c, portMAX_DELAY) != pdTRUE) continue;
        if (c.type == AUDIO_CMD_TONE || c.type == AUDIO_CMD_SONG || c.type == AUDIO_CMD_WAV) {
            status_start(c.type == AUDIO_CMD_SONG, c.arg);
            portENTER_CRITICAL(&s_status_mux);
            memset(&s_path,0,sizeof(s_path)); s_path.codec_regs=-1; s_path.pa_level=-1;
            portEXIT_CRITICAL(&s_status_mux);
            esp_err_t pa=audio_enable_pa();
            if (pa!=ESP_OK) { status_finished(pa); continue; }
            esp_err_t err;
            if (c.type == AUDIO_CMD_SONG) err = audio_play_song(c.arg);
            else if (c.type == AUDIO_CMD_TONE) err = audio_play_tone(c.arg, c.arg2);
            else {
                xSemaphoreTake(s_play_mutex, portMAX_DELAY);
                err = audio_play_wav_locked(c.filename);
                xSemaphoreGive(s_play_mutex);
            }
            board_pa_enable(false);
            audio_path_diag_t path; audio_get_path_diag(&path);
            ESP_LOGI(TAG,"PCM finished: samples=%lu nonzero=%lu peak=%lu pa=%d err=%d",
                (unsigned long)path.samples,(unsigned long)path.nonzero_samples,(unsigned long)path.peak,path.pa_level,(int)err);
            status_finished(err);
        } else {
            audio_apply_control(&c);
            if (c.type == AUDIO_CMD_STOP) status_finished(ESP_OK);
        }
    }
}

// C5 is single-core. Suspending task scheduling across a NONBLOCKING send and
// cancellation publication prevents the newly woken consumer from starting
// before admission is complete. Queue-full rejection has no playback effect.
// No I/O, waits or allocation occurs in this transaction; ISR callers excluded.
static bool audio_post_command(audio_cmd_t c) {
    if (!__atomic_load_n(&s_service_ready, __ATOMIC_ACQUIRE)) return false;
    vTaskSuspendAll();
    c.id = __atomic_fetch_add(&s_cmd_next_id, 1, __ATOMIC_RELAXED);
    bool accepted = xQueueSend(s_cmd_q, &c, 0) == pdTRUE;
    if (accepted) {
        portENTER_CRITICAL(&s_status_mux);
        s_status.last_cmd_id = c.id;
        bool active = audio_is_active(&s_status);
        portEXIT_CRITICAL(&s_status_mux);
        if (active && (c.type == AUDIO_CMD_SONG || c.type == AUDIO_CMD_TONE ||
                       c.type == AUDIO_CMD_WAV || c.type == AUDIO_CMD_STOP)) audio_request_stop();
    }
    xTaskResumeAll();
    return accepted;
}
bool audio_post_song(int i) {
    return audio_get_song(i) && audio_post_command((audio_cmd_t){.type=AUDIO_CMD_SONG, .arg=i});
}
bool audio_post_tone(uint32_t f, uint32_t ms) {
    if (f < 20 || f > 8000 || !ms || ms > 60000) return false;
    return audio_post_command((audio_cmd_t){.type=AUDIO_CMD_TONE, .arg=f, .arg2=ms});
}
bool audio_post_stop(void) { return audio_post_command((audio_cmd_t){.type=AUDIO_CMD_STOP}); }
bool audio_post_volume(int delta) { if (delta < -100 || delta > 100) return false; return audio_post_command((audio_cmd_t){.type=AUDIO_CMD_SET_VOLUME, .arg=delta}); }
bool audio_post_volume_absolute(int volume) {
    if (volume < 0 || volume > 100) return false;
    return audio_post_command((audio_cmd_t){.type=AUDIO_CMD_VOLUME_ABSOLUTE, .arg=volume});
}
bool audio_post_mute(bool mute) { return audio_post_command((audio_cmd_t){.type=AUDIO_CMD_MUTE, .arg=mute}); }
bool audio_post_wav(const char *name) {
    if (!name || !*name || strlen(name) >= sizeof(((audio_cmd_t *)0)->filename)) return false;
    for (const char *p = name; *p; ++p)
        if (!( (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
               (*p >= '0' && *p <= '9') || *p == '_' || *p == '-' || *p == '.')) return false;
    if (strstr(name, "..")) return false;
    audio_cmd_t c = {.type=AUDIO_CMD_WAV};
    strlcpy(c.filename, name, sizeof(c.filename));
    return audio_post_command(c);
}
void audio_get_status(audio_status_t *out) {
    if (!out) return;
    portENTER_CRITICAL(&s_status_mux);
    *out = s_status;
    portEXIT_CRITICAL(&s_status_mux);
}
void audio_diag(bool *ready, bool *codec, int *volume, bool *muted, int *error) {
    audio_status_t s; audio_get_status(&s);
    if (ready) *ready = s.ready;
    if (codec) *codec = s.ready;
    if (volume) *volume = s.volume;
    if (muted) *muted = s.muted;
    if (error) *error = s.last_error;
}
int audio_diag_last_cmd(void) { audio_status_t s; audio_get_status(&s); return s.last_cmd_id; }
int audio_diag_state(void) { audio_status_t s; audio_get_status(&s); return s.state; }
int audio_pa_reassert_read(void) { return __atomic_load_n(&s_pa_result, __ATOMIC_ACQUIRE); }
void audio_get_path_diag(audio_path_diag_t *out) {
    if (!out) return;
    portENTER_CRITICAL(&s_status_mux); *out=s_path; portEXIT_CRITICAL(&s_status_mux);
}
int audio_read_codec_regs(void) {
    audio_path_diag_t path; audio_get_path_diag(&path); return path.codec_regs;
}
