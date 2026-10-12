// core/test/test_core.c — host unit tests for the pure policies.
//
// Builds and runs on the host with no board and no ESP-IDF (see Makefile).
// Covers the review's fault matrix (07-TESTING-STRATEGY):
//   - sample model: stopped producer, partial IMU, 49.7-day ms wrap  (V05/V06)
//   - readiness:    display/touch fail, LVGL stall, pre-loop hang, degraded    (V01)
//   - ota identity: same-version fail, fast reboot, lost trigger, concurrent   (V09)
//   - audio state:  session overlap/replace, open failure, stop completion     (V03/V07)
#include "../sample_model.h"
#include "../readiness.h"
#include "../ota_identity.h"
#include "../audio_cmd.h"
#include "../comfort_rules.h"

#include <stdio.h>
#include <string.h>

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, msg) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; printf("  FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); } \
} while (0)

// ---------- sample model (V05/V06) ----------
static void test_sample_model(void) {
    printf("sample_model:\n");
    const uint32_t W = SAMPLE_STALE_MS_DEFAULT;      // 15000 ms window
    sample_health_t h = {0};
    // never read -> UNKNOWN, not fresh (times are microseconds now)
    CHECK(sample_quality(&h, 1000000ull, W, false) == SAMPLE_UNKNOWN, "unread=UNKNOWN");
    CHECK(!sample_is_fresh(&h, 1000000ull, W), "unread not fresh");

    // good read at t=1s; fresh at t=5s (<15s), stale at t=20s. Reader clock
    // advances even though the producer stopped stamping (V05a).
    sample_mark_good(&h, 1000000ull);
    CHECK(h.sequence == 1 && h.ever_good, "mark_good bumps seq");
    CHECK(sample_is_fresh(&h, 5000000ull, W), "fresh within window");
    CHECK(!sample_is_fresh(&h, 20000000ull, W), "stopped producer expires (V05a)");
    CHECK(sample_quality(&h, 20000000ull, W, false) == SAMPLE_STALE, "stale verdict");

    // estimated (uncalibrated battery) degrades FRESH->ESTIMATED but not STALE
    CHECK(sample_quality(&h, 5000000ull, W, true) == SAMPLE_ESTIMATED, "estimated degrades fresh");
    CHECK(sample_quality(&h, 20000000ull, W, true) == SAMPLE_STALE, "estimated stays stale when old");

    // N10: a sample stamped at monotonic time ZERO is VALID (ever_good, not a
    // zero sentinel) and fresh within the window.
    sample_health_t z = {0};
    sample_mark_good(&z, 0ull);
    CHECK(z.ever_good, "t=0 sample is ever_good");
    CHECK(sample_is_fresh(&z, 5000000ull, W), "t=0 sample is fresh (no zero sentinel, N10)");

    // N10: no 49.7-day alias. A sample from just below a 2^32-ms boundary must
    // NOT look fresh when the 64-bit reader clock is a full ms-cycle later.
    sample_health_t a = {0};
    uint64_t near_wrap_us = ((uint64_t)0xFFFFF000u) * 1000ull;        // old stamp
    sample_mark_good(&a, near_wrap_us);
    uint64_t full_cycle_later = near_wrap_us + ((uint64_t)1u << 32) * 1000ull;
    CHECK(!sample_is_fresh(&a, full_cycle_later, W), "no revival after full 32-bit cycle (N10)");

    // fail does not refresh timing (retain last good, drop freshness over time)
    sample_mark_fail(&h, -5);
    CHECK(h.last_error == -5 && h.sequence == 1, "fail keeps seq/last_good, records error");
}

// ---------- readiness (V01) ----------
static void test_readiness(void) {
    printf("readiness:\n");
    const uint32_t t0 = 1000;      // pending window start
    readiness_input_t in = {0};

    // no maintenance yet -> WAIT (never confirm without recovery path)
    in.maintenance_ready = false; in.ui_ready = true; in.input_ready = true;
    in.last_heartbeat_ms = t0; in.stable_ticks = 10;
    CHECK(readiness_eval(&in, t0 + 1000, t0) == READINESS_WAIT, "no maintenance=WAIT");

    // healthy: maintenance+ui+input, probation met, fresh heartbeat -> FULL
    in.maintenance_ready = true;
    in.last_heartbeat_ms = t0 + 30000; in.stable_ticks = 3;
    CHECK(readiness_eval(&in, t0 + 31000, t0) == READINESS_CONFIRM_FULL, "healthy=CONFIRM_FULL");

    // probation not yet met -> WAIT (not the first tick)
    in.stable_ticks = 1;
    CHECK(readiness_eval(&in, t0 + 31000, t0) == READINESS_WAIT, "probation<3=WAIT");

    // UI failed but maintenance up + probation met -> DEGRADED (keep recovery)
    in.stable_ticks = 3; in.ui_ready = false;
    CHECK(readiness_eval(&in, t0 + 31000, t0) == READINESS_CONFIRM_DEGRADED, "ui-fail=CONFIRM_DEGRADED");

    // LVGL stall: heartbeat too old even though ticks counted earlier -> WAIT
    in.ui_ready = true; in.last_heartbeat_ms = t0; // stale heartbeat
    CHECK(readiness_eval(&in, t0 + 60000, t0) == READINESS_WAIT, "stale heartbeat=WAIT");

    // Pre-loop hang: deadline exceeded, nothing confirmed -> REJECT (rollback)
    in.maintenance_ready = false; in.ui_ready = false; in.stable_ticks = 0;
    CHECK(readiness_eval(&in, t0 + READINESS_DEADLINE_MS, t0) == READINESS_REJECT, "deadline=REJECT");
}

// ---------- ota identity (V09) ----------
static void mk_target(ota_target_t *t, const char *ver, const char *sha, bool req) {
    memset(t, 0, sizeof(*t));
    snprintf(t->project, sizeof(t->project), "echo");
    snprintf(t->version, sizeof(t->version), "%s", ver);
    if (sha) snprintf(t->app_sha, sizeof(t->app_sha), "%s", sha);
    t->require_sha = req;
}
static void mk_report(ota_report_t *r, const char *ver, const char *sha,
                      img_state_t st, uint32_t boot, bool reach) {
    memset(r, 0, sizeof(*r));
    snprintf(r->project, sizeof(r->project), "echo");
    snprintf(r->version, sizeof(r->version), "%s", ver);
    if (sha) snprintf(r->app_sha, sizeof(r->app_sha), "%s", sha);
    r->state = st; r->boot_id = boot; r->reachable = reach;
}
static void test_ota_identity(void) {
    printf("ota_identity:\n");
    ota_target_t tgt; ota_report_t rep;
    const char *shaA = "aaaa"; const char *shaB = "bbbb";

    // exact sha + VALID + rebooted -> ACCEPT
    mk_target(&tgt, "75", shaA, true);
    mk_report(&rep, "75", shaA, IMG_VALID, 2, true);
    CHECK(ota_identity_accept(&tgt, &rep, 1, false) == OTA_ACCEPT, "exact+valid+reboot=ACCEPT");

    // N09: require_sha=true but the report OMITS its digest (matches version,
    // rebooted, VALID). Must FAIL CLOSED (WAIT), not fall back to version match.
    mk_target(&tgt, "75", shaA, true);               // require sha
    mk_report(&rep, "75", NULL, IMG_VALID, 2, true);  // report has no app_sha
    CHECK(ota_identity_accept(&tgt, &rep, 1, false) == OTA_WAIT, "require_sha + missing report digest=WAIT (N09)");
    // require_sha but TARGET digest empty -> also fail closed
    mk_target(&tgt, "75", NULL, true);
    mk_report(&rep, "75", shaA, IMG_VALID, 2, true);
    CHECK(ota_identity_accept(&tgt, &rep, 1, false) == OTA_WAIT, "require_sha + missing target digest=WAIT (N09)");

    // same-version FAILURE: version matches but it ROLLED BACK (different sha)
    // and boot_id unchanged (no real reboot) -> WAIT then identity reject
    mk_target(&tgt, "75", shaB, true);               // wanted B
    mk_report(&rep, "75", shaA, IMG_VALID, 1, true);  // running A, same boot
    CHECK(ota_identity_accept(&tgt, &rep, 1, false) == OTA_WAIT, "no-reboot same-version=WAIT (not success)");
    // with a reboot observed but wrong sha -> identity reject
    CHECK(ota_identity_accept(&tgt, &rep, 1, true) == OTA_REJECT_IDENTITY, "rebooted wrong-image=REJECT_IDENTITY");

    // fast reboot caught by boot_id change even if client didn't see unreachable
    mk_target(&tgt, "76", shaB, true);
    mk_report(&rep, "76", shaB, IMG_VALID, 9, true);  // boot_id changed 1->9
    CHECK(ota_identity_accept(&tgt, &rep, 1, false) == OTA_ACCEPT, "fast reboot via boot_id=ACCEPT");

    // right image but still PENDING -> WAIT (don't declare success early)
    mk_report(&rep, "76", shaB, IMG_PENDING_VERIFY, 9, true);
    CHECK(ota_identity_accept(&tgt, &rep, 1, true) == OTA_WAIT, "right image pending=WAIT");

    // right image but ABORTED/INVALID -> unconfirmed reject
    mk_report(&rep, "76", shaB, IMG_INVALID, 9, true);
    CHECK(ota_identity_accept(&tgt, &rep, 1, true) == OTA_REJECT_UNCONFIRMED, "invalid=REJECT_UNCONFIRMED");

    // unreachable -> WAIT
    mk_report(&rep, "76", shaB, IMG_VALID, 9, false);
    CHECK(ota_identity_accept(&tgt, &rep, 1, true) == OTA_WAIT, "unreachable=WAIT");
}

// ---------- audio state (V03/V07) ----------
static void test_audio_cmd(void) {
    printf("audio_cmd:\n");
    audio_status_t s; audio_status_init(&s, 60);
    CHECK(s.state == AUDIO_IDLE && s.playing_index == -1 && s.volume == 60, "init idle");

    // start a song -> STARTING then PLAYING
    audio_on_start(&s, true, 2);
    CHECK(s.state == AUDIO_STARTING && s.playing_index == 2, "song starting");
    audio_on_playing(&s);
    CHECK(s.state == AUDIO_PLAYING && audio_is_active(&s), "song playing/active");

    // latest-play: a new tone/song is accepted even while playing (owner cancels)
    CHECK(audio_cmd_accept(&s, AUDIO_CMD_SONG), "latest-play replaces current");
    CHECK(audio_cmd_accept(&s, AUDIO_CMD_STOP), "stop always accepted");
    CHECK(audio_cmd_accept(&s, AUDIO_CMD_SET_VOLUME), "volume always accepted");

    // stop completion bumps stop_seq and clears playing
    uint32_t seq0 = s.stop_seq;
    audio_on_stopping(&s);
    audio_on_finished(&s, 0);
    CHECK(s.state == AUDIO_IDLE && s.playing_index == -1, "stop -> idle");
    CHECK(s.stop_seq == seq0 + 1, "stop completion signalled");

    // V07: open failure -> FAILED, playback inactive, retry permitted
    audio_on_start(&s, false, -1);     // tone
    audio_on_finished(&s, -123);       // open failed before playing
    CHECK(s.state == AUDIO_FAILED && !audio_is_active(&s), "open-fail inactive (V07)");
    CHECK(s.last_error == -123, "open-fail error surfaced");
    CHECK(audio_cmd_accept(&s, AUDIO_CMD_TONE), "retry permitted after fail");
}

// ---------- comfort rules (A05) ----------
static void test_comfort_rules(void) {
    printf("comfort_rules:\n");
    // "High humidity >= 70%, clear at 65%, sustained for 10 s"
    const comfort_rule_t r = { "High humidity", COMFORT_ABOVE, 70.0f, 65.0f, 10000 };
    comfort_rt_t rt; comfort_rt_init(&rt);

    // Below threshold -> idle
    CHECK(comfort_rule_update(&r, &rt, true, 50.0f, 1000) == COMFORT_IDLE, "below=IDLE");
    // Breach -> pending (not active yet)
    CHECK(comfort_rule_update(&r, &rt, true, 72.0f, 2000) == COMFORT_PENDING, "breach=PENDING");
    // Still pending before min_duration
    CHECK(comfort_rule_update(&r, &rt, true, 73.0f, 9000) == COMFORT_PENDING, "pending<10s");
    // Persisted >= 10 s -> active
    CHECK(comfort_rule_update(&r, &rt, true, 73.0f, 12000) == COMFORT_ACTIVE, "sustained=ACTIVE");
    // Hysteresis: 68% (between clear 65 and threshold 70) must NOT clear -> stays active
    CHECK(comfort_rule_update(&r, &rt, true, 68.0f, 13000) == COMFORT_ACTIVE, "hysteresis holds ACTIVE");
    // Cross clear point -> idle
    CHECK(comfort_rule_update(&r, &rt, true, 64.0f, 14000) == COMFORT_IDLE, "cross clear=IDLE");

    // No chatter: a breach that ends before maturing goes back to idle.
    comfort_rt_init(&rt);
    comfort_rule_update(&r, &rt, true, 72.0f, 1000);                 // PENDING
    CHECK(comfort_rule_update(&r, &rt, true, 60.0f, 3000) == COMFORT_IDLE, "breach ended early=IDLE (no chatter)");

    // Stale-suspend: a PENDING breach CANNOT mature across a data gap.
    comfort_rt_init(&rt);
    comfort_rule_update(&r, &rt, true, 72.0f, 1000);                 // PENDING @1s
    comfort_rule_update(&r, &rt, false, 0.0f, 5000);                 // STALE @5s -> resets PENDING
    CHECK(rt.state == COMFORT_IDLE, "stale resets PENDING (no silent maturation)");
    // Even if a late valid breach arrives past the original min_duration, the
    // clock restarts from the NEW breach — stale data can't back-date it.
    CHECK(comfort_rule_update(&r, &rt, true, 72.0f, 12000) == COMFORT_PENDING, "re-breach restarts PENDING");
    CHECK(comfort_rule_update(&r, &rt, true, 72.0f, 21500) == COMFORT_PENDING, "still pending 9.5s after re-breach");

    // ms-wrap: breach starting just below 2^32 matures correctly after wrap.
    comfort_rt_init(&rt);
    comfort_rule_update(&r, &rt, true, 72.0f, 0xFFFFF000u);          // PENDING near wrap
    // 0xFFFFF000 + 11000ms wraps to 0x00001AF8; elapsed = 11000 >= 10000 -> ACTIVE
    CHECK(comfort_rule_update(&r, &rt, true, 72.0f, 0x00001AF8u) == COMFORT_ACTIVE, "matures across ms-wrap");
}

int main(void) {
    printf("=== echo core/ host tests ===\n");
    test_sample_model();
    test_readiness();
    test_ota_identity();
    test_audio_cmd();
    test_comfort_rules();
    printf("=== %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
