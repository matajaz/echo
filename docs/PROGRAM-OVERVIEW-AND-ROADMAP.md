# echo — program overview and roadmap

Updated 2026-10-11. Firmware **v110**. Hardware: **ESP32-C5** (Waveshare
ESP32-C5-Touch-LCD-2.8), ESP-IDF **v6.1-dev**, 8 MB PSRAM, 32 MB flash,
single RISC-V core, 240×320 SPI TFT + CST3530 touch.

The v110 description below is retained as a historical baseline. Current
v144 changes, tests, data limitations and hardware follow-ups are documented
in [REVIEW-IMPLEMENTATION-2026-10-12.md](REVIEW-IMPLEMENTATION-2026-10-12.md).

**Current implementation (2026-10-12, v144):** nine modular LVGL pages, stable
HA roles and units, per-entity freshness/errors, Nord Pool day curves, hourly
HA recorder energy changes, bounded flash cache, corrected audio admission and
WAV streaming, explicit Stop semantics, absolute volume and readiness, complete
input-stroke gating, RTC/NTP synchronization, optional pickup wake and idle PA
power-off. Solar energy, storage flows and tariff-specific costs require the
corresponding real HA data; the panel does not invent them.

## 1. What it is

echo is a custom ESP-IDF firmware (replacing the stock xiaozhi voice-assistant
app) for a small wall-mounted touch panel. The design goal is a **verifiable,
reliably field-updatable** always-on panel: WiFi station + network diagnostics +
secure pull-OTA with honest acceptance, an LVGL touch UI (overview, sensors,
HA temperatures, melodies), on-board sensors, and an audio subsystem.

### Hardware (verified against the vendor BSP, not guessed)
- SoC ESP32-C5 (dual-band WiFi 6, BLE5, 802.15.4), single core, 8 MB PSRAM.
- Display 2.8" ST7789 (SPI2); touch CST3530 (I2C, local component override).
- Sensors on a shared I2C bus: QMI8658 IMU, SHTC3 temp/humidity, PCF85063A RTC,
  battery ADC via a CH32V003 I/O-expander.
- Audio: ES8311 I2S codec + NS4150-class speaker amplifier (PA enabled via the
  CH32 expander, pin 3). Speaker on the SPK1 connector.

### Architecture
```
core/              PURE C policies, NO ESP-IDF, host-unit-tested (49/49)
  sample_model.h   quality/freshness (64-bit µs reader clock, ever_good)
  readiness.h      readiness_eval: WAIT / CONFIRM_FULL / CONFIRM_DEGRADED / REJECT
  ota_identity.h   exact sha/version + VALID + reboot acceptance (fail-closed)
  audio_cmd.h      audio state machine (Idle/Starting/Playing/Stopping/Failed)
  comfort_rules.h  sustained-comfort rule engine (hysteresis + min-duration)
  test/            test_core.c + Makefile (-Wall -Wextra -Werror + ASan/UBSan)
main/              ESP-IDF board glue behind thin seams
  app_main.c       boot, WiFi (dual-SSID), SNTP, readiness-gated OTA confirm
  board_services.c SINGLE OWNER of the shared I2C bus + CH32 expander + ops
  ota.c            pull-OTA streaming worker + HTTP server (all routes)
  display.c touch.c panel + CST3530 (consume board_services handles)
  sensors.c        QMI8658/SHTC3/RTC/battery poll task + /metrics
  audio.c          ES8311 command-owner service: tones, note-array songs,
                   WAV streaming; stop/mute; PA re-assert before playback
  ha_client.c      Home Assistant REST poll (generation-tagged OTA pause)
  ui.c ui_graphs.c LVGL tiles + timers
  weblog.c         in-RAM web log (/log, /log.txt?since=)
tools/             host generators: gen_entities.py (HA list), gen_songs.py (songs)
catalogue/         entities.yaml (HA entity catalogue, A15)
scripts/deploy_ota.sh  build->publish->trigger->confirm(img_state)->ledger
```

Key properties:
- **One host-testable policy core.** Freshness/quality, boot readiness, OTA
  acceptance, audio state and comfort rules are pure C in `core/`, unit-tested
  on the host with fake clocks (49 assertions, ASan/UBSan clean, -Werror).
- **Single ownership of shared resources.** `board_services.c` owns the I2C bus
  + CH32 expander (created before the display, so a display failure can't
  invalidate the bus for touch/sensors/audio). `audio.c` owns the codec via a
  bounded command queue + a service task — the sole codec driver; callers only
  enqueue.
- **Readiness-gated OTA.** A freshly-OTA'd image confirms only after a bounded
  probation with essential readiness (HTTP maintenance + usable UI); an
  independent deadline rolls back a pre-loop/loop hang (task-WDT panic is off).
  This net is PROVEN: a UI-hang build (over-full LVGL pool) auto-rolled-back.
- **Exact-image acceptance.** `/info` exports app sha + img_state + boot_id; the
  deploy client gates success on `img_state==valid` (confirmed), not a version
  string.

### HTTP API (verified live)
`GET /info` → fw, idf, ota_in_progress, rssi, heap_free/min, uptime_s,
dma_free/largest, int_free/largest, app_sha, img_state, boot_id, reset_reason,
and audio diag (audio_ready/codec/vol/muted/err).
`GET /metrics` (Prometheus), `GET /log` + `/log.txt?since=` (web log),
`GET /ota?key=&url=` (key-gated, host-allowlist, A/B), `GET /reboot?key=`,
`GET /beep?key=` (audio self-test + ES8311/PA diagnostic),
`GET /play?key=&name=` (stream a WAV song from the server).

### UI (6 swipeable LVGL tiles; Overview is the default landing)
Overview (clock, WiFi/HA status, local climate with freshness, HA summary,
battery estimate, comfort advisory, Clean + Timer buttons) → graph (one metric,
dropdown, visible axes) → HA temperatures → HA batteries → Melodies (song list)
→ Device info (build/idf/ssid/ip/wifi/uptime/reset-reason/ota + battery+WiFi
icons). UX behaviours: idle-return to Overview after 2 min; cleaning lock
(touch-locked 30 s countdown); night dim after 60 s with first-touch wake
(the wake tap is consumed — hence a "double tap" after it has dimmed).

### Audio
ES8311 I2S codec, command-owner service (single codec owner). Three modes:
1. **Synthesized tones** (sine + per-note envelope + drain — click-free).
2. **Note-array songs** — 17 melodies (5 built-in + 12 classic tunes generated
   from robsoncouto/arduino-songs via tools/gen_songs.py). Zero RAM/server cost
   (static flash arrays). **The song COUNT is bounded by the 32 KB LVGL object
   pool, not flash** — one list button per song; 26 songs exhausted the pool and
   hung the UI (reverted). Expanding needs an LVGL-pool change first.
3. **WAV streaming** — 16-bit PCM mono WAV streamed over HTTP from the LAN nginx
   host (WAV_STREAM_PREFIX allowlist), PCM fed straight to the codec (no decoder;
   HTTP buffer in PSRAM, I2S DMA in internal RAM). `/play?name=x.wav`.
Audio diagnostics in `/info` + `/beep` (reports codec/PA/ES8311 register state).

### WiFi
Station, dual-band (all-channel scan, 5 GHz-biased), country SE, perpetual
reconnect, **dual-SSID fallback** (home + phone hotspot; hops after repeated
failures). SNTP wall clock (Europe/Stockholm). Credentials compile-time in a
git-ignored secrets.h (no NVS provisioning yet — deferred until a validation/
recovery policy exists).

### Deploy
`scripts/deploy_ota.sh`: bump PROJECT_VER → build → publish atomically →
SHA-verify served copy → trigger → confirm via `/info` img_state==valid → append
a JSON ledger on every outcome. `--no-bump` / `--log`.

### Known constraints / honest gaps
- Single core: LVGL, audio, sensors, WiFi, OTA share one core.
- Internal DMA-capable RAM (~24 KB free) and the **32 KB LVGL object pool** are
  the scarce resources; large PSRAM totals are not admission evidence. New
  widgets/list-items must be budgeted against the LVGL pool.
- Battery SOC is uncalibrated (labelled "est").
- OTA trust is LAN-only (key + host-allowlist, HTTP), not signed firmware bytes.
- No ambient-light sensor; dim is time/inactivity based.

## 2. Current review surface (what's new/notable)
- All 11 defects from the v82 independent review (N01–N11) fixed; host-tested.
- Milestone A (overview home, idle-return, cleaning lock), B (HA entity catalogue
  compiler A15), C (comfort rules A05, offline timer I02, dim/wake K05) delivered.
- Audio subsystem: command-owner service, note-array song importer, WAV
  streaming, and a self-test/diagnostic path.
- A real intermittent HARDWARE issue (loose SPK connector) was diagnosed with the
  `/beep` + ES8311-register diagnostics, confirming the firmware audio path is
  healthy — the diagnostics are retained.

## 3. Remaining roadmap
Highest value next: **LVGL pool headroom** (bump 32 KB, or move the pool to
PSRAM) to lift the song/list-item ceiling, then expand songs; a **browseable
WAV song list** on the panel; RTTTL text-melody player. HA-dependent features
(exceptions checklist A01, confirmed presets I04, expiring override A17) need
user-defined HA templates/scripts. Technical: watchdog + per-subsystem
heartbeats; validated retained state; port the OTA fix + diagnostics to lumen;
CI with the host tests + an LVGL SDL simulator; NVS runtime provisioning (after a
recovery policy).

## 4. Pointers for the reviewer
- Host tests: `cd core/test && make test` (no board).
- Generators: `tools/gen_entities.py` (HA list), `tools/gen_songs.py` (songs).
- Live diagnostics: `/info`, `/metrics`, `/log.txt?since=`, `/beep`,
  `scripts/deploy_ota.sh --log`.
- OTA root-cause history: `OTA-VERIFY-FAILURE-{EVIDENCE,ANALYSIS}.md`.
- The local CST3530 touch override under `components/` is git-tracked but not in
  the managed-components manifest — ensure release artifacts include it.
- All work since commit a66d0c6 is currently UNCOMMITTED in the working tree.
