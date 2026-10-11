# echo — program description for review (v82)

Updated 2026-10-11. Firmware **v82**. Hardware: **ESP32-C5** (Waveshare
ESP32-C5-Touch-LCD-2.8), ESP-IDF **v6.1-dev**, 8 MB PSRAM, 32 MB flash,
single RISC-V core. This document describes the current program for an
independent code/architecture review. It supersedes the v71 overview; the
"What changed since v71" section lists exactly what was done so the reviewer
can focus on the new/changed surface.

## 1. What the program is

echo is a custom ESP-IDF firmware (replacing the stock xiaozhi voice-assistant
app) for a small wall-mounted touch panel. Goals: a **verifiable, reliably
field-updatable** embedded system — WiFi station, network diagnostics, secure
pull-OTA with honest acceptance, and an LVGL touch UI showing sensors, battery,
and Home Assistant temperatures.

### Hardware (verified against the vendor BSP, not guessed)
- SoC ESP32-C5 (dual-band WiFi 6 2.4/5 GHz, BLE5, 802.15.4), single core, 8 MB PSRAM.
- Display 2.8" ST7789 (SPI2); touch CST3530 (I2C, local component override with
  OOB/stuck-press fixes).
- Sensors on the shared I2C bus: QMI8658 IMU, SHTC3 temp/humidity, PCF85063A RTC,
  battery ADC via a CH32V003 I/O-expander.
- Audio: ES8311 codec over I2S; speaker PA enabled via the CH32 expander.

### Architecture (v82)
```
core/                 PURE C policies, NO ESP-IDF, host-unit-tested (34/34)
  sample_model.h      value+source+last_good+sequence+quality+last_error; reader-clock freshness
  readiness.h         readiness_eval(): WAIT / CONFIRM_FULL / CONFIRM_DEGRADED / REJECT
  ota_identity.h      ota_identity_accept(): exact sha/version + VALID + reboot classification
  audio_cmd.h         audio_status_t state machine + latest-play + stop completion
  test/               test_core.c + Makefile (-Wall -Wextra -Werror + ASan/UBSan)
main/                 ESP-IDF board glue (I/O, LVGL, drivers) behind thin seams
  app_main.c          boot sequence, WiFi (dual-SSID), readiness-driven OTA confirm
  board_services.c    SINGLE OWNER of the shared I2C bus + CH32 expander + named ops
  ota.c               pull-OTA streaming worker + HTTP server (/ota /info /reboot)
  display.c touch.c   panel + CST3530 (consume board_services handles)
  sensors.c audio.c   sensors + ES8311 audio command-owner service
  ha_client.c         Home Assistant REST poll (generation-tagged OTA pause)
  weblog.c            in-RAM web log (/log, /log.txt?since=)
scripts/deploy_ota.sh build->publish->trigger->confirm(img_state)->ledger
```

Key architectural properties:
- **One host-testable policy core.** Freshness/quality, boot readiness, OTA
  acceptance and audio state are pure C in `core/`, unit-tested on the host with
  a fake clock (34 assertions, ASan/UBSan clean). Device code fills these; it
  does not re-implement the policy.
- **Single ownership of shared resources.** `board_services.c` owns the shared
  I2C bus + CH32 expander (created before the display), so a display failure
  cannot invalidate the bus for touch/sensors/audio. `audio.c` owns the codec via
  a bounded command queue + a service task — the sole codec driver; UI and
  app_main only enqueue.
- **Readiness-gated OTA confirm.** A freshly-OTA'd image is confirmed only after
  a bounded probation with essential readiness (maintenance HTTP + usable UI),
  and an independent deadline rolls back a pre-loop/loop hang (task-WDT panic is
  off). Optional hardware (sensors/HA/audio) is explicitly degraded, never fatal.
- **Exact-image acceptance.** `/info` exports the running image's `app_sha`,
  `img_state` (new/pending_verify/valid/invalid/aborted) and a per-boot `boot_id`;
  the deploy client gates success on `img_state`, not just a version string.

### HTTP API (verified live)
`GET /info` → `{fw, idf, ota_in_progress, rssi, heap_free, heap_min, uptime_s,
dma_free, dma_largest, int_free, int_largest, app_sha, img_state, boot_id}`.
`GET /metrics` (Prometheus), `GET /log` + `GET /log.txt?since=` (web log),
`GET /ota?key=&url=` (key-gated, host-allowlist-prefix, A/B, redirects off),
`GET /reboot?key=`.

### UI (6 swipeable LVGL tiles)
controls + live sensor readout, temp/humidity chart, accel/gyro chart,
battery SOC chart, Home Assistant temperatures, device-info (build/idf/ssid/
wifi band/uptime/ota status).

### WiFi
Station, dual-band (all-channel scan, 5 GHz-biased), country SE, perpetual
reconnect. **Dual-SSID fallback (new):** profile 0 = home, profile 1 = phone
hotspot; after 5 consecutive failures on one SSID it hops to the other and keeps
hopping until one associates (counter resets on a successful IP), so the panel
stays reachable for OTA/HA if the home AP is down. Credentials are compile-time
in a git-ignored `secrets.h` (no NVS provisioning yet — deliberately deferred
until a recovery/validation policy exists).

### Deploy
`scripts/deploy_ota.sh`: bump PROJECT_VER → build → publish atomically →
SHA-verify served copy → trigger → confirm via `/info` (reboot via uptime/boot_id
reset + `img_state`) → append a JSON ledger record on EVERY outcome (success/
fail/abort) with heap/rssi/identity. `--log` prints the ledger; `--no-bump`
redeploys as-is.

### Known constraints / open items
- Single-core: LVGL, audio, sensors, WiFi, OTA share one core.
- Internal DMA-capable RAM is the scarce resource; LVGL draw buffer and I2S DMA
  buffers are sized to coexist (I2S is 6×240, reduced from 8×512 after a boot-time
  DMA-exhaustion fix — see below).
- Battery SOC is uncalibrated (ADC→% estimate).
- OTA trust is LAN-only (key + host-allowlist, HTTP), not signed/authenticated bytes.

## 2. What changed since the v71 review (the review surface)

All 12 verified defects from the v71 review were fixed, then the 6 architectural
extensions were implemented. Two field issues were also found and fixed.

### 12 verified defects (all fixed + verified)
- V01 readiness/probation/deadline for OTA confirm (host-tested policy + wired).
- V02 display_fill never frees an in-flight DMA buffer (fail-closed completion).
- V03 single codec owner (mutex → full command-owner service).
- V04 extract_state OOB read on unterminated JSON key.
- V05 reader-time freshness + IMU all-required (not die-temp-only).
- V06 chart fixed wall-time grid with explicit outage gaps.
- V07 s_playing cleared on codec-open failure.
- V08 deploy ledger emits valid JSON on every outcome; preserves exit status.
- V09 exact-image acceptance (img_state/boot_id/app_sha; uptime-reset reboot detect).
- V10 generation-tagged HA pause/ack (no stale-idle false ack).
- V11 HA row color follows quality on every refresh.
- V12 incomplete OTA body → non-OK error (no "OTA failed: ESP_OK").

### 6 architectural extensions (new)
1. Host-testable `core/` + test harness (34/34, ASan/UBSan, -Werror).
2. Shared sample/health model (`sample_model.h`); its logic is live in sensors/ui.
3. Readiness supervisor (`readiness.h`) drives the OTA confirm path.
4. Audio command-owner service (bounded queue + `audio_status_t`; stop/mute/status;
   removed the UI-owned player task).
5. Exact-image OTA identity + acceptance (`ota_identity.h` + `/info` exports + client gate).
6. `board_services.c` — single owner of the shared I2C bus + CH32 expander + named
   reset/backlight/PA ops.

### Field fixes found this round
- **Audio I2S DMA alloc was failing at boot** (deep 8×512 full-duplex buffers didn't
  fit the DMA pool after display/WiFi/HTTP). Reduced to 6×240 (~5.6 KB); audio now
  inits and the chime plays. This also removed the DMA pressure that made the HTTP
  server flap during OTA.
- **Warning-gate cleared:** fixed the `weblog LINE_MAX` redefinition and removed dead
  `retry_count_get`; firmware builds with zero warnings.
- **Home Assistant auth now works** (fixed externally in the HA/token path); the
  earlier `HTTP_CLIENT: Bearer not supported` is resolved.

### Deliberately deferred (not defects)
- Mechanical migration of `sensors.c` to store `sample_health_t` structs (pure churn;
  the policy/behavior is already live).
- NVS runtime provisioning (needs a config-validation + recovery policy first).
- The feature wishlist below.

## 3. Remaining roadmap (candidate review targets)

Prefer reliability/correctness review of the new architecture first, then features.

### Likely-highest-value review focus
- The `core/` policies: are the freshness/readiness/ota-identity/audio state
  machines correct at the boundaries (ms-wrap, probation/deadline edges, latest-play
  races, img_state transitions)? Host tests exist — challenge their coverage.
- `board_services` ownership: any remaining path where a display/bus failure or an
  init retry leaves another consumer with a stale/half-open handle?
- Audio command-owner: queue-full behavior, stop-vs-start ordering, mute/volume
  interaction, the service task vs the still-present blocking executors.
- Readiness/OTA confirm on real fault injection (display fail, touch fail, LVGL
  stall, pre-loop hang, degraded sensor) — only partially exercised on HW.

### Feature candidates (not yet built)
Offline/overview home screen; hardware-test + diagnostic screen; WiFi/HA
provisioning + configurable entity list (needs NVS policy); visible stop/mute UI
surfacing the new `audio_status_t`; brightness/idle-dimming + touch-wake; calibrated
battery; real clock with holdover; persistent fault/incident evidence; mDNS +
browser status page; BOOT-button recovery menu; TF-card media/logs.

### Technical open items
- Watchdog + per-subsystem heartbeats (task-WDT panic is off; readiness heartbeat is
  app_main-only).
- Validated retained state (the OTA-boot-count lives in uninitialized RTC RAM).
- Port the OTA transport-free-before-verify fix + `/info` heap/identity diagnostics to
  the sibling lumen project.
- CI: fresh-checkout dependency resolve + pinned SDK/toolchain/component checksums;
  wire the `core/` host tests + an LVGL SDL simulator into a gate.

## 4. Pointers for the reviewer
- OTA root-cause history: `OTA-VERIFY-FAILURE-EVIDENCE.md`, `OTA-VERIFY-FAILURE-ANALYSIS.md`.
- Prior review + resolution: `REVIEW-2026-10-10.md` (all items addressed).
- Host tests: `cd core/test && make test` (no board needed).
- Live diagnostics: `/info` (heap classes, rssi, img_state, boot_id, app_sha),
  `/metrics`, `/log.txt?since=`, and the server-side deploy ledger
  (`scripts/deploy_ota.sh --log`).
- The local CST3530 touch override under `components/` is git-tracked but not in the
  managed-components manifest — ensure release artifacts include it.
