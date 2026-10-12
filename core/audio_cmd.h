// core/audio_cmd.h — audio command + playback-state policy (pure C).
//
// V03 preferred extension + F08 (visible stop/mute/playback feedback): ONE
// audio owner drives tone/song/volume/stop/mute through a bounded queue; this
// is the pure state machine it runs, host-testable with a mock codec call log.
// The device audio service holds the real codec; this decides transitions and
// which command "wins" (latest-play replacement), with no ESP-IDF deps.
#ifndef CORE_AUDIO_CMD_H
#define CORE_AUDIO_CMD_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AUDIO_CMD_NONE = 0,
    AUDIO_CMD_TONE,
    AUDIO_CMD_SONG,
    AUDIO_CMD_STOP,
    AUDIO_CMD_SET_VOLUME,
    AUDIO_CMD_MUTE,     // set muted true/false (arg != 0 => muted)
    AUDIO_CMD_WAV,
    AUDIO_CMD_VOLUME_ABSOLUTE,
} audio_cmd_type_t;

typedef struct {
    audio_cmd_type_t type;
    int32_t          arg;   // song index / tone freq / volume / mute flag
    int32_t          arg2;  // tone duration (ms) when type==TONE
    uint32_t         id;    // monotonic accepted-command id
    char            filename[48]; // owned by command, never a shared mailbox
} audio_cmd_t;

typedef enum {
    AUDIO_IDLE = 0,
    AUDIO_STARTING,
    AUDIO_PLAYING,
    AUDIO_STOPPING,
    AUDIO_FAILED,
} audio_state_t;

// Observable playback state the UI/diagnostics read (F08). `last_cmd_id` is the
// most recently ACCEPTED command; `playing_index` is -1 unless a song plays.
typedef struct {
    audio_state_t state;
    int32_t  playing_index;  // -1 when not playing a song
    int32_t  volume;         // 0..100
    bool     muted;
    int32_t  last_error;     // 0 = none; set on open/write failure
    uint32_t last_cmd_id;    // id of the last accepted command
    uint32_t stop_seq;       // bumped each time a stop completes (completion signal)
    bool ready;
} audio_status_t;

static inline void audio_status_init(audio_status_t *s, int32_t vol) {
    if (!s) return;
    s->state = AUDIO_IDLE; s->playing_index = -1; s->volume = vol;
    s->muted = false; s->last_error = 0; s->last_cmd_id = 0; s->stop_seq = 0;
    s->ready = false;
}

// Decide whether a NEW incoming command should be accepted given current state.
// Latest-play policy: a new TONE/SONG replaces an in-progress one (the owner
// will cancel the current session). STOP is always accepted. VOLUME/MUTE are
// always accepted (they don't own the codec session). Returns true if accepted.
static inline bool audio_cmd_accept(const audio_status_t *s, audio_cmd_type_t t) {
    if (!s) return false;
    switch (t) {
        case AUDIO_CMD_STOP:
        case AUDIO_CMD_SET_VOLUME:
        case AUDIO_CMD_VOLUME_ABSOLUTE:
        case AUDIO_CMD_MUTE:
            return true;              // control commands always accepted
        case AUDIO_CMD_TONE:
        case AUDIO_CMD_SONG:
        case AUDIO_CMD_WAV:
            return true;              // latest-play: replaces any current session
        default:
            return false;
    }
}

// Pure transitions; the firmware wraps them in its status lock so readers
// always observe a whole lifecycle update. `index` is ignored for tones/WAV.
static inline void audio_on_start(audio_status_t *s, bool is_song, int32_t index) {
    if (!s) return;
    s->state = AUDIO_STARTING;
    s->playing_index = is_song ? index : -1;
    s->last_error = 0;
}
static inline void audio_on_playing(audio_status_t *s) {
    if (s) s->state = AUDIO_PLAYING;
}
static inline void audio_on_stopping(audio_status_t *s) {
    if (s) s->state = AUDIO_STOPPING;
}
// A session ended. `err` != 0 => FAILED (and surfaced to the UI). Either way we
// return to IDLE-capable: a failed open must leave playback inactive (V07) and
// permit a retry. Bumps stop_seq so a stop requester sees completion.
static inline void audio_on_finished(audio_status_t *s, int32_t err) {
    if (!s) return;
    s->playing_index = -1;
    s->last_error = err;
    s->state = (err != 0) ? AUDIO_FAILED : AUDIO_IDLE;
    s->stop_seq += 1u;
}
static inline bool audio_is_active(const audio_status_t *s) {
    return s && (s->state == AUDIO_STARTING || s->state == AUDIO_PLAYING ||
                 s->state == AUDIO_STOPPING);
}

#ifdef __cplusplus
}
#endif
#endif // CORE_AUDIO_CMD_H
