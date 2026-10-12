// core/comfort_rules.h — offline sustained-comfort rule engine (A05, pure C).
//
// A tiny local temporal rule engine: "humidity has stayed high for N minutes",
// "temperature has stayed low", etc. Works OFFLINE (no HA). Host-testable with
// a fake clock + a fake sample stream. The review's hard requirements:
//   - threshold + HYSTERESIS (no chatter at the boundary)
//   - MIN-DURATION: a breach must PERSIST through valid observations
//   - STALE/missing samples SUSPEND qualification — stale data can NEVER
//     newly trigger, and a reconnect must not replay an old trigger
//   - visual-only advisory (audio is optional, gated on the audio owner)
// NOT a fire/CO/medical alarm — a convenience advisory only.
#ifndef CORE_COMFORT_RULES_H
#define CORE_COMFORT_RULES_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define COMFORT_MAX_RULES 4

typedef enum {
    COMFORT_ABOVE = 0,   // breach when value >= threshold
    COMFORT_BELOW = 1,    // breach when value <= threshold
} comfort_cmp_t;

// Static rule definition. `clear` is the value the breach must cross back
// through to end (hysteresis band = |threshold - clear|). For ABOVE, clear <
// threshold; for BELOW, clear > threshold.
typedef struct {
    const char  *label;        // advisory text, e.g. "High humidity"
    comfort_cmp_t cmp;
    float         threshold;
    float         clear;        // hysteresis return level
    uint32_t      min_duration_ms;  // breach must persist this long to go ACTIVE
} comfort_rule_t;

typedef enum {
    COMFORT_IDLE = 0,    // not breached
    COMFORT_PENDING,     // breached, waiting for min_duration to elapse
    COMFORT_ACTIVE,      // sustained breach -> advisory shown
} comfort_state_t;

// Per-rule runtime state (parallel to the rule defs).
typedef struct {
    comfort_state_t state;
    uint32_t        breach_start_ms;  // when the current breach began (PENDING)
} comfort_rt_t;

static inline void comfort_rt_init(comfort_rt_t *rt) {
    if (rt) { rt->state = COMFORT_IDLE; rt->breach_start_ms = 0; }
}

static inline bool comfort_is_breach(const comfort_rule_t *r, float v) {
    return (r->cmp == COMFORT_ABOVE) ? (v >= r->threshold) : (v <= r->threshold);
}
static inline bool comfort_is_cleared(const comfort_rule_t *r, float v) {
    // Cleared once the value crosses BACK past the hysteresis return level.
    return (r->cmp == COMFORT_ABOVE) ? (v <= r->clear) : (v >= r->clear);
}

// Feed one observation. `valid` is false when the sample is stale/missing — in
// that case the rule's qualification is SUSPENDED: a PENDING rule resets (so a
// gap can never silently complete min_duration), and an ACTIVE rule holds its
// state but does not re-arm from stale data. Only a VALID breach advances
// toward ACTIVE. `now_ms` is a monotonic reader clock (wrap-safe).
//
// Returns the (possibly changed) state. Transition to ACTIVE happens only when
// a VALID breach has persisted >= min_duration_ms of continuous valid breach.
static inline comfort_state_t comfort_rule_update(const comfort_rule_t *r,
                                                  comfort_rt_t *rt,
                                                  bool valid, float v,
                                                  uint32_t now_ms) {
    if (!r || !rt) return COMFORT_IDLE;

    if (!valid) {
        // Stale/missing: suspend qualification. A PENDING breach cannot mature
        // across a data gap; an ACTIVE advisory is retained (don't flap it off
        // on a single missing read) but gains no new ground.
        if (rt->state == COMFORT_PENDING) {
            rt->state = COMFORT_IDLE;
            rt->breach_start_ms = 0;
        }
        return rt->state;
    }

    switch (rt->state) {
        case COMFORT_IDLE:
            if (comfort_is_breach(r, v)) {
                rt->state = COMFORT_PENDING;
                rt->breach_start_ms = now_ms;
            }
            break;
        case COMFORT_PENDING:
            if (!comfort_is_breach(r, v)) {
                // Breach ended before maturing -> back to idle (no chatter).
                rt->state = COMFORT_IDLE;
                rt->breach_start_ms = 0;
            } else if ((uint32_t)(now_ms - rt->breach_start_ms) >= r->min_duration_ms) {
                rt->state = COMFORT_ACTIVE;
            }
            break;
        case COMFORT_ACTIVE:
            if (comfort_is_cleared(r, v)) {
                rt->state = COMFORT_IDLE;   // crossed back past hysteresis
                rt->breach_start_ms = 0;
            }
            break;
    }
    return rt->state;
}

#ifdef __cplusplus
}
#endif
#endif // CORE_COMFORT_RULES_H
