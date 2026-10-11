// core/sample_model.h — shared sample + health model (host-testable, pure C).
//
// The review's highest-value architectural piece: ONE small model that boot
// acceptance, the UI, diagnostics and OTA all read, so freshness/quality is
// computed in exactly one place with a reader-supplied clock. NO ESP-IDF
// includes — this compiles and unit-tests on the host with a fake clock
// (see core/test/). Device glue (sensors.c/ui.c) fills these structs; it does
// NOT reimplement freshness.
//
// Design (from 03-ARCHITECTURE.md): per coherent group ->
//   {value(s), source, last_good_us, sequence, quality, last_error}
// Clock is ALWAYS supplied by the reader (never persisted as an epoch), so a
// stopped producer cannot read fresh forever (root cause of V05a).
#ifndef CORE_SAMPLE_MODEL_H
#define CORE_SAMPLE_MODEL_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Quality of a group's current value. Ordered worst->best so a presenter can
// compare. UNKNOWN = never observed; STALE = had a good read but it aged out;
// ESTIMATED = present but not trustworthy (e.g. uncalibrated battery, or a
// clock that is set but not SNTP-synced).
typedef enum {
    SAMPLE_UNKNOWN = 0,   // never had a good read
    SAMPLE_STALE   = 1,   // had a good read, now older than the freshness window
    SAMPLE_ESTIMATED = 2, // present but low-trust (uncalibrated/holdover)
    SAMPLE_FRESH   = 3,   // good read within the freshness window
} sample_quality_t;

// A coherent group's health metadata. The actual values (temp, accel[3], soc…)
// stay in the producer's own struct; this carries the quality/timing contract.
typedef struct {
    uint32_t last_good_ms;  // monotonic ms at the last good read (0 = never)
    uint32_t sequence;      // increments on every accepted good read
    int32_t  last_error;    // producer-defined last error code (0 = none)
    bool     ever_good;     // true once any good read happened
} sample_health_t;

// Default freshness window (ms). A group older than this is STALE. Groups may
// override per-call. 15 s matches ~3 missed 5 s polls.
#define SAMPLE_STALE_MS_DEFAULT 15000u

// Mark a good read at reader/producer time `now_ms`: stamps last_good_ms,
// bumps sequence, clears last_error, sets ever_good.
static inline void sample_mark_good(sample_health_t *h, uint32_t now_ms) {
    if (!h) return;
    h->last_good_ms = now_ms;
    h->sequence += 1u;
    h->last_error = 0;
    h->ever_good = true;
}

// Mark a failed read with a producer error code. Does NOT touch last_good_ms
// or sequence (the last good value is retained, just not refreshed).
static inline void sample_mark_fail(sample_health_t *h, int32_t err) {
    if (!h) return;
    h->last_error = err;
}

// Freshness computed against the READER's current clock (not a stored
// publish-time), with ms-wrap-safe unsigned subtraction. This is the single
// correct freshness test (V05a). `reader_now_ms` MUST be read by the caller at
// read time.
static inline bool sample_is_fresh(const sample_health_t *h,
                                   uint32_t reader_now_ms, uint32_t stale_ms) {
    if (!h || !h->ever_good || h->last_good_ms == 0u) return false;
    return (uint32_t)(reader_now_ms - h->last_good_ms) <= stale_ms;
}

// Full quality verdict for a group. `estimated` lets the producer flag a
// present-but-low-trust value (e.g. uncalibrated battery) — it degrades FRESH
// to ESTIMATED but never promotes STALE/UNKNOWN.
static inline sample_quality_t sample_quality(const sample_health_t *h,
                                              uint32_t reader_now_ms,
                                              uint32_t stale_ms, bool estimated) {
    if (!h || !h->ever_good) return SAMPLE_UNKNOWN;
    if (!sample_is_fresh(h, reader_now_ms, stale_ms)) return SAMPLE_STALE;
    return estimated ? SAMPLE_ESTIMATED : SAMPLE_FRESH;
}

#ifdef __cplusplus
}
#endif
#endif // CORE_SAMPLE_MODEL_H
