// core/ota_identity.h — exact-image deployment acceptance policy (pure C).
//
// V09 root cause: version strings + reachability are NOT image identity, so a
// same-version failed update can read as SUCCESS and a fast reboot can go
// unconfirmed. This encodes the acceptance DECISION as a pure, host-testable
// function the deploy client uses: a deploy is accepted ONLY when the running
// image's identity matches the expected target AND it is confirmed VALID.
//
// The device exports its real identity (running ELF/app sha, project, version,
// boot id, image state) via /info; the client passes the expected target here.
#ifndef CORE_OTA_IDENTITY_H
#define CORE_OTA_IDENTITY_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

// Image A/B state as reported by the device (mirrors esp_ota_img_states_t but
// kept framework-free for host tests).
typedef enum {
    IMG_UNKNOWN = 0,
    IMG_PENDING_VERIFY,
    IMG_VALID,
    IMG_INVALID,
    IMG_ABORTED,
} img_state_t;

// What the client EXPECTS the device to be running after a successful deploy.
typedef struct {
    char     project[16];  // e.g. "echo"
    char     version[16];  // PROJECT_VER string
    char     app_sha[65];  // hex sha256 of the published .bin (immutable name)
    bool     require_sha;  // if false, fall back to version match (legacy)
} ota_target_t;

// What the device actually reports (parsed from /info). boot_id distinguishes
// a reboot that happened entirely between polls.
typedef struct {
    char       project[16];
    char       version[16];
    char       app_sha[65];  // "" if the device does not export it yet
    img_state_t state;
    uint32_t   boot_id;      // changes on every boot
    bool       reachable;    // got a parseable /info this poll
} ota_report_t;

typedef enum {
    OTA_ACCEPT = 0,          // exact target running + VALID
    OTA_WAIT,                // reachable but not yet VALID / not yet matching
    OTA_REJECT_IDENTITY,     // running a DIFFERENT image than expected (e.g. rollback, concurrent publisher)
    OTA_REJECT_UNCONFIRMED,  // right image but still PENDING/UNKNOWN past the window
} ota_accept_t;

// Decide acceptance. `baseline_boot_id` is the device's boot_id BEFORE the
// trigger; `saw_reboot` is set by the client when it observed a reboot
// (uptime reset / unreachable / boot_id change). Pure.
static inline ota_accept_t ota_identity_accept(const ota_target_t *tgt,
                                               const ota_report_t *rep,
                                               uint32_t baseline_boot_id,
                                               bool saw_reboot) {
    if (!tgt || !rep || !rep->reachable) return OTA_WAIT;

    // A reboot must have happened: no reboot => the trigger never acted even if
    // the version string happens to match (V09 same-version false success).
    bool rebooted = saw_reboot || (rep->boot_id != 0u && rep->boot_id != baseline_boot_id);
    if (!rebooted) return OTA_WAIT;

    // Identity: prefer sha (immutable), else project+version.
    bool identity_ok;
    if (tgt->require_sha && tgt->app_sha[0] && rep->app_sha[0]) {
        identity_ok = (strncmp(tgt->app_sha, rep->app_sha, sizeof(tgt->app_sha)) == 0);
    } else {
        identity_ok = (strncmp(tgt->project, rep->project, sizeof(tgt->project)) == 0) &&
                      (strncmp(tgt->version, rep->version, sizeof(tgt->version)) == 0);
    }
    if (!identity_ok) return OTA_REJECT_IDENTITY;   // different image is running

    if (rep->state == IMG_VALID) return OTA_ACCEPT; // exact target, confirmed
    if (rep->state == IMG_PENDING_VERIFY || rep->state == IMG_UNKNOWN) return OTA_WAIT;
    return OTA_REJECT_UNCONFIRMED;                   // INVALID/ABORTED right-image
}

#ifdef __cplusplus
}
#endif
#endif // CORE_OTA_IDENTITY_H
