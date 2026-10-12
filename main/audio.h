// echo -- audio output via the ES8311 codec (I2S + esp_codec_dev).
//
// Hardware directions verified against the board schematic. The older vendor
// BSP labels DOUT/DSIN incorrectly; codec ASDOUT is an ESP input, not an output.
//   - Codec: ES8311 on the shared I2C bus (ES8311_CODEC_DEFAULT_ADDR).
//   - I2S: SCLK=GPIO24, WS/LRCK=GPIO25, ESP DOUT=GPIO27, ESP DIN=GPIO26,
//     MCLK=none (ES8311 use_mclk=false -- runs off BCLK), I2S_NUM_0,
//     master, 16-bit, Philips slot.
//   - Power amplifier (NS4150-class) enable: CH32 expander pin
//     IO_EXPANDER_PIN_NUM_3 (IO_POWER_AMP_IO), active HIGH. The codec's
//     own pa_pin is GPIO_NUM_NC on this board -- the PA is enabled
//     separately via the expander (what the vendor BSP does at I2C init).
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Initialize once from startup. Thereafter callers enqueue commands; the
// priority-20 audio service alone owns codec I/O, gain and session teardown.
// Snapshot getters are thread-safe and nonblocking. Enqueue calls are safe
// from tasks (not ISRs), bounded, and have no side effects when rejected.
esp_err_t audio_init(void);
// Blocking executor APIs: reserved for the audio service, never a UI callback.
esp_err_t audio_play_tone(uint32_t freq_hz, uint32_t duration_ms);

// A single note: frequency in Hz (0 = rest) and duration in ms.
typedef struct {
    uint16_t freq_hz;
    uint16_t ms;
} audio_note_t;

// A named song = an array of notes. All melodies are PUBLIC DOMAIN /
// traditional tunes (no copyrighted compositions).
typedef struct {
    const char *name;
    const audio_note_t *notes;
    uint16_t count;
} audio_song_t;

// Song registry. audio_song_count() returns how many songs exist;
// audio_get_song(i) returns song i (or NULL out of range) so the UI can
// build one button per song without hardcoding the list.
int audio_song_count(void);
const audio_song_t *audio_get_song(int index);

esp_err_t audio_play_song(int index);
void audio_request_stop(void); // atomic asynchronous cancellation
bool audio_is_playing(void);
int audio_get_volume(void); // -1 until the service is ready
int audio_adjust_volume(int delta); // executor-only
#include "audio_cmd.h"
bool audio_post_song(int index);
bool audio_post_tone(uint32_t freq_hz, uint32_t duration_ms);
bool audio_post_stop(void);
bool audio_post_volume(int delta);
bool audio_post_volume_absolute(int volume);
bool audio_post_mute(bool mute);
bool audio_post_wav(const char *name);
void audio_get_status(audio_status_t *out);

// Compact diagnostic for /info (debuggable without serial): writes
// *service_ready (audio_init fully succeeded + task live), *codec_ok (codec
// handle present), *volume (desired %), *muted, *last_error (0 = none).
void audio_diag(bool *service_ready, bool *codec_ok, int *volume, bool *muted,
                int *last_error);
int audio_diag_last_cmd(void);   // last accepted command id (advances per accepted cmd)
int audio_diag_state(void);      // audio_state_t as int
int audio_pa_reassert_read(void); // last amplifier-enable result; does not change hardware
int audio_read_codec_regs(void);  // ES8311 regs 0x0D/0x12/0x32 packed, -1 fail
// Cached by the codec owner after opening and while writing actual PCM.
// Reading this snapshot never takes the playback lock or touches I2C.
typedef struct {
    uint32_t samples, nonzero_samples, peak, sample_rate;
    int pa_level, pa_error, codec_regs, clock1, clock2, format, mute_reg;
} audio_path_diag_t;
void audio_get_path_diag(audio_path_diag_t *out);

#ifdef __cplusplus
}
#endif
