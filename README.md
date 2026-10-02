# echo

New ESP32 project. Board was connected to the USB port previously used by
**lumen**. Hardware identified and factory-backed-up on 2026-10-01 before any
flashing.

## Hardware

**Waveshare ESP32-C5-Touch-LCD-2.8** dev board, running the stock
**xiaozhi-esp32** voice-assistant firmware (board profile
`main/boards/waveshare/esp32-c5-touch-lcd-2.8`).

- **SoC: ESP32-C5** (chip revision v1.2, eco3) — dual-band Wi-Fi 6 (802.11ax),
  Bluetooth 5 (LE), IEEE 802.15.4 (Zigbee/Thread/Matter capable radio),
  single RISC-V core + LP core, 240 MHz, 48 MHz crystal.
- **Flash: 32 MB** (QIO, 80 MHz). Factory image header reports 16 MB — the
  physical chip is double that; verified via `esptool flash_id`.
- **PSRAM: 8 MB** (80 MHz).
- **Display: 2.8" touch LCD** — ST7789 panel driver (SPI2), **CST3530** touch
  controller (I2C, `esp_lcd_touch_cst3530`).
- **IMU: QMI8658** (6-axis accel+gyro), initializes successfully at boot.
- **MAC:** `10:bd:a3:df:29:fc` (base), USB serial VID:PID `303a:1001`
  (Espressif "USB JTAG/serial debug unit" — native USB-CDC, no external
  USB-UART chip).
- Factory firmware runs **ESP-Brookesia** (Espressif's phone-style
  LVGL launcher/UI framework, v0.6.0) as a demo (`Display ESP-Brookesia
  phone demo`), on top of ESP-IDF v5.5.4.
- Hits `https://api.tenclass.net/xiaozhi/ota/` for OTA; wake word
  `nihaoxiaozhi` ("你好小智").
- Factory WiFi provisioning portal is a full captive-portal page with
  dozens of languages embedded (SSID picker, BSSID-remember, sleep-mode
  toggle) — standard xiaozhi-esp32 provisioning flow.

### Partition table (factory image, from boot log)
| # | Label    | Type | Offset     | Length     |
|---|----------|------|------------|------------|
| 0 | nvs      | data | 0x00009000 | 0x00004000 |
| 1 | otadata  | data | 0x0000d000 | 0x00002000 |
| 2 | phy_init | data | 0x0000f000 | 0x00001000 |
| 3 | factory  | app  | 0x00020000 | 0x004e2000 |
| 4 | storage  | data | 0x00502000 | 0x00400000 |
| 5 | ota_0    | app  | 0x00910000 | 0x00300000 |
| 6 | assets   | data | 0x00c10000 | 0x00300000 |

Note: bootloader on this chip lives at **0x2000** (not the classic 0x1000 —
same as C3/C6/H2), confirmed by the `0xE9` image-header magic byte at that
offset in the raw flash dump.

## Physical / network topology (how we're reaching it)

The board's USB is physically plugged into **hp.local** (the Proxmox
hypervisor — must stay clean, no packages installed there). We pass the
specific USB device through live to the **debian.local** VM (VM id 100,
`debian-docker`) via:
```
ssh hp.local 'qm set 100 -usb0 host=303a:1001'
```
This is reversible/live (no VM restart needed) and does not touch the
hypervisor's own package state. On `debian.local` the device appears as
`/dev/ttyACM0` and is passed into the pre-existing `sandbox` Docker
container (`/home/mathias/docker/sandbox/docker-compose.yml`, `devices:`
entry) alongside `/dev/ttyUSB0` (Vegena's CH340 adapter, same container).
`esptool` is installed inside `sandbox` via
`pip install --break-system-packages esptool==4.11.0` (not persisted across
container recreation — reinstall after any `docker compose up -d` that
recreates the container).

## factory/ — pre-flash backups (DO NOT SKIP THIS STEP ON FUTURE BOARDS)

- `factory_backup_esp32c5.bin` — full 32 MB raw flash read
  (`esptool read_flash 0x0 0x2000000`), taken BEFORE any write to this board.
  sha256: `7654557000159b53834e752058560c504a2b9ba71eee6f156da34ea724dc8b99`
- `bootlog_full.txt` — clean serial boot capture (reset via RTS pulse,
  12s window) showing full ROM bootloader → ESP-IDF boot → app_main →
  ESP-Brookesia UI init → WiFi manager connect-retry loop (steady state).

This follows the project's standing hardware lesson (see the esp32-lessons
steering note): always capture the factory boot log + a full flash backup
before writing anything, since the factory log is the authoritative
reference for exact hardware bring-up (panel driver, touch controller,
pin config) and the backup is the restore point if anything goes wrong.

## Custom firmware (replaces stock xiaozhi app for now)

Stock firmware's WiFi-config-mode fallback could not be triggered (NVS erase,
60s+ waits, and multiple real power cycles all left it in a permanent
STA-retry loop — see git history / session notes). Rather than reverse-engineer
an undocumented button/GPIO trigger, built a minimal ESP-IDF app from scratch:
WiFi station + a key-gated pull-OTA HTTP server, same pattern as lumen/Vegena.

- **main/app_main.c** — WiFi STA bring-up (SSID/pass from `secrets/secrets.h`,
  gitignored), starts the OTA server once connected.
- **main/ota.c** — `GET /ota?key=...&url=...` (queues a pull-OTA via
  `esp_https_ota`, URL must match `OTA_URL_PREFIX` allowlist, rejects a 2nd
  OTA while one is in flight) + `GET /info` (fw version, idf version, OTA
  state). Device is on `192.168.1.183` (DHCP, WiFi `LilleSkutt`).
- **partitions.csv** — A/B OTA (`ota_0`/`ota_1`, 1.5 MB each) + larger `nvs`.
- Served from the existing `vegena-esp32-panel-web` nginx container on
  `debian.local:8091`, path `/echo/echo.bin` (added alongside the existing
  Vegena/lumen board paths in `nginx.conf`'s allowlist).

### The one real bug hit: `esp_https_ota` rejects plain HTTP by default
`esp_https_ota_begin()` requires `cert_pem`/`use_global_ca_store`/
`crt_bundle_attach` to be set UNLESS `CONFIG_ESP_HTTPS_OTA_ALLOW_HTTP=y` is in
sdkconfig — without it, a bare `http://` OTA target fails immediately with
`ESP_ERR_INVALID_ARG` ("No option for server verification is enabled"), no
network traffic even attempted. Added the flag to `sdkconfig.defaults`
(documented inline: the real SSRF/arbitrary-firmware guard is the
`ota_url_allowed()` prefix-allowlist in `ota.c`, not TLS, matching the
project's existing LAN-only trust model for lumen/Vegena OTA). **Must
re-flash over USB once after adding this flag** — OTA itself can't fix a
device that doesn't have the flag yet, chicken-and-egg.

### Verified end-to-end (2026-10-01)
USB-flashed v2 (with the HTTP-OTA fix) → triggered `/ota?key=...` pointing at
a freshly-published v3 binary → captured the full serial log: download,
`esp_https_ota` write to `ota_1` at `0x1a0000`, image-segment verification,
clean reboot, fresh boot banner showing `App version: 3`, WiFi reconnect,
OTA server back up → confirmed via `GET /info` → `{"fw":"3",...}`.

## Infrastructure notes (useful for the next board)
- USB passthrough chain: `hp.local` (Proxmox) → `qm set 100 -usb0
  host=303a:1001` → `debian.local` VM → `sandbox` container `devices:` entry.
  **The device node path changes on every power cycle / USB re-enumeration**
  (`/dev/ttyACM0` → `ttyACM1` → back to `ttyACM0`, etc.) — check `lsusb`/
  `ls /dev/ttyACM*` on `debian.local` after every power event and update
  `docker-compose.yml` + `docker compose up -d` (plain `up -d` won't recreate
  if compose sees no diff — use `--force-recreate` if the path is unchanged
  but the underlying device object isn't).
- `esptool` writes at 460800 baud timed out over this virtualized USB chain
  (mid-write hang, device became briefly unresponsive even to `chip_id`).
  **460800 is NOT reliable through this passthrough path — use 115200.**
  Confirmed working at 115200 for both bootloader+partition+app and app-only
  flashes.
- `esptool` is not persisted in the `sandbox` container across recreation —
  reinstall (`pip install --break-system-packages esptool==4.11.0`) after
  every `docker compose up -d` that recreates it.

## Status

- [x] Identify hardware (ESP32-C5, Waveshare touch-LCD-2.8, xiaozhi-esp32)
- [x] Capture factory boot log
- [x] Back up full factory flash image
- [x] Get OTA working (custom firmware, not stock xiaozhi app)
- [ ] Decide project scope/direction (what should `echo` actually become?)
