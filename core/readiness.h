// core/readiness.h — product-readiness + OTA-confirm probation policy (pure C).
//
// The review (V01 + 03-ARCHITECTURE readiness supervisor) wants ONE bounded
// readiness contract used by boot/OTA acceptance, not inline booleans and a
// first-tick confirm. This is that contract as a pure, host-testable policy:
// no ESP-IDF, clock supplied by the caller.
//
// Essential readiness for a wall display = the HTTP maintenance server (the
// only remote-recovery path) + a usable UI. Optional hardware (sensors/HA/
// audio) is DEGRADED, never fatal (a dead sensor must not roll back forever).
#ifndef CORE_READINESS_H
#define CORE_READINESS_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Inputs the supervisor evaluates each tick. Filled by app_main from real init
// results + a heartbeat; pure data so it is trivial to unit-test.
typedef struct {
    bool maintenance_ready;   // HTTP server (remote recovery) is up
    bool ui_ready;            // LVGL up OR raw fallback demonstrably rendered
    bool input_ready;         // touch attached (or N/A on fallback)
    uint32_t last_heartbeat_ms;   // monotonic ms of the most recent service tick
    uint32_t stable_ticks;        // consecutive healthy service ticks observed
} readiness_input_t;

// Tunables (defaults; app_main may override).
#define READINESS_PROBATION_TICKS 3u       // healthy ticks before confirm
#define READINESS_HEARTBEAT_MAX_MS 20000u  // heartbeat older than this = wedged
#define READINESS_DEADLINE_MS 120000u      // hard pending-image deadline

// The confirm decision. CONFIRM_FULL = essential ready + probation passed.
// CONFIRM_DEGRADED = maintenance up (remote recovery preserved) but UI not
// ready after probation — confirm anyway (rolling back a display-only fault
// would lose recovery) but EXPLICITLY, logged. WAIT = probation not satisfied
// yet. REJECT = deadline exceeded without confirmation -> caller restarts so
// the bootloader rolls the still-pending image back.
typedef enum {
    READINESS_WAIT = 0,
    READINESS_CONFIRM_FULL,
    READINESS_CONFIRM_DEGRADED,
    READINESS_REJECT,
} readiness_verdict_t;

// Evaluate at `now_ms` for an image whose pending window started at
// `pending_start_ms`. Pure; no side effects.
static inline readiness_verdict_t readiness_eval(const readiness_input_t *in,
                                                 uint32_t now_ms,
                                                 uint32_t pending_start_ms) {
    // Hard deadline first: an init/loop hang must roll back even if nothing
    // else is observable (task-WDT panic is disabled on this board).
    if ((uint32_t)(now_ms - pending_start_ms) >= READINESS_DEADLINE_MS) {
        return READINESS_REJECT;
    }
    if (!in || !in->maintenance_ready) {
        return READINESS_WAIT;   // no recovery path yet: never confirm, keep waiting
    }
    // Heartbeat must be recent (the service loop is actually ticking).
    bool heartbeat_ok =
        (uint32_t)(now_ms - in->last_heartbeat_ms) <= READINESS_HEARTBEAT_MAX_MS;
    if (!heartbeat_ok || in->stable_ticks < READINESS_PROBATION_TICKS) {
        return READINESS_WAIT;
    }
    if (in->ui_ready && in->input_ready) return READINESS_CONFIRM_FULL;
    // Maintenance up + probation passed but UI/input not fully ready: confirm
    // in explicit degraded mode to preserve remote recovery.
    return READINESS_CONFIRM_DEGRADED;
}

#ifdef __cplusplus
}
#endif
#endif // CORE_READINESS_H
