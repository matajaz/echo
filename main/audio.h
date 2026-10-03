// echo -- audio output via the ES8311 codec (I2S + esp_codec_dev).
//
// Hardware (verified against the vendor BSP
// esp32_c5_touch_lcd_2.8.{c,h}, not guessed):
//   - Codec: ES8311 on the shared I2C bus (ES8311_CODEC_DEFAULT_ADDR).
//   - I2S: SCLK=GPIO24, WS/LRCK=GPIO25, DOUT=GPIO26, DSIN=GPIO27,
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

// ---------------------------------------------------------------------------
// THREADING / OWNERSHIP CONTRACT -- read this before calling anything below
// ---------------------------------------------------------------------------
// This module owns the codec handle, the I2S channels and the output volume,
// and playback blocks for seconds (a whole song). The rules:
//
//  1. ONE OWNER. Exactly ONE task -- the module's audio task -- drives this
//     module: audio_init(), audio_play_tone(), audio_play_song() and the
//     volume accessors are all issued from it (or by a caller that has
//     already serialized on the audio mutex itself). No other task may call
//     them, because a second caller would interleave codec open/close/write
//     and volume sets on the same DMA.
//
//  2. NOT RE-ENTRANT. audio_play_tone()/audio_play_song() block until the
//     last sample has drained and the codec has been closed. Never call them
//     from the LVGL task, from an ISR/timer callback, or re-entrantly from
//     anywhere inside this module.
//
//  3. LIFECYCLE. The volume accessors are only meaningful AFTER
//     audio_init() has returned ESP_OK and BEFORE any teardown: calling them
//     while audio_init() is still creating the codec, or while a playback is
//     closing it, races the codec handle. Rule (1) is what makes that
//     impossible -- there is no concurrent init/deinit path.
//
//  4. LOCK ORDERING. If a caller adds its own locking instead of routing
//     through the audio task, the audio lock is the INNERMOST (leaf) lock:
//     take the display/I2C (and any UI) lock FIRST, then the audio lock,
//     never the reverse -- audio_init() likewise resolves its
//     display_get_i2c_bus()/display_get_io_expander() handles before it
//     touches the codec. Never hold the audio lock across
//     audio_play_song(): it blocks for seconds and would stall volume
//     changes behind it.
//
// Bring up I2S + the ES8311 codec and enable the speaker PA. Requires
// display_init() to have succeeded (reuses its shared I2C bus + CH32
// expander handle). Non-fatal to the rest of the system.
// Must run on the audio task (THREADING rule 1); it is idempotent.
esp_err_t audio_init(void);

// Play a short sine-wave test tone (blocking) at the given frequency and
// duration, so audio output is verifiable by ear. Returns ESP_OK if the
// tone was written. No-op error if audio_init() hasn't succeeded.
// Serialized with audio_play_song(): the audio task must not have another
// playback in flight, or the tone blocks until that playback has drained and
// closed the codec.
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

// Play song `index` from the registry (blocking, a few seconds). Uses the
// click-free engine (per-note raised-cosine envelope, continuous phase,
// large write chunks, DMA drain before close). Safe no-op error if audio
// isn't initialized or the index is out of range. Run it OFF the LVGL
// task (its own FreeRTOS task) since it blocks for the whole tune.
//
// SERIALIZATION (THREADING rule 1): playback is issued from, and completed
// by, the single audio task. A song never overlaps another playback -- the
// previous one has drained its DMA and closed the codec before this one
// opens it. Enqueue the request on that task; if you need the previous tune
// gone first, call audio_request_stop() and let the task finish unwinding
// (it returns only after the drain + close) before starting the next song.
esp_err_t audio_play_song(int index);

// Request that an in-progress song stop early (checked at note boundaries,
// then drains cleanly). Used so a new song press can interrupt the current
// one. No-op if nothing is playing.
//
// STOP-COMPLETION: audio_request_stop() is ASYNCHRONOUS and non-blocking --
// it only bumps the cancel generation, so it returns long before the current
// playback has actually drained and closed the codec. That makes it safe to
// call from ANY task (including the UI task), and a stop that lands just
// before a new song starts is never lost. It does NOT provide a
// stop-AND-WAIT guarantee: do not treat it as "the song has stopped" before
// the audio task has returned from audio_play_song().
void audio_request_stop(void);

// Playback-state query. Because audio_request_stop() is asynchronous and
// audio_play_song() blocks for seconds, this is the non-blocking way for
// another task (e.g. the UI) to observe when playback has actually finished:
// it returns true while a song is in flight and false once the audio task
// has drained the DMA and closed the codec. The flag is owned and updated by
// the audio task only (THREADING rule 1). Safe to call from any task.
bool audio_is_playing(void);

// Volume control (0-100%). audio_adjust_volume clamps the result to
// [0,100], applies it to the codec, and returns the new level (or -1 if
// audio isn't initialized). audio_get_volume returns the current level, or
// -1 if audio isn't initialized -- callers cannot otherwise distinguish a
// genuine 0% from a failure, so check for -1 rather than assuming >= 0.
// Valid only after audio_init() has returned ESP_OK and while the audio task
// is alive (THREADING rule 3): the level is owned by the audio task, so a
// volume change can never race init, playback or teardown of the codec.
int audio_get_volume(void);
int audio_adjust_volume(int delta);

#ifdef __cplusplus
}
#endif
