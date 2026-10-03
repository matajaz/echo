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
- [x] Display bring-up (backlight + ST7789 panel, solid-color fill) --
      see "Display bring-up" section below
- [x] Touch (CST3530) -- see "Touch" below
- [x] Observability: in-RAM `/log` + `/log.txt?since=` web log
- [x] LVGL (esp_lvgl_port) with a 3-page swipeable tileview UI
- [x] Sensors (QMI8658 IMU, SHTC3 temp/humidity, PCF85063A RTC) on the
      shared I2C bus -- on screen, in graphs, and in `/metrics`
- [x] Battery state-of-charge (CH32 expander ADC, calibrated)
- [x] `/metrics` Prometheus endpoint (sensor + battery values)
- [x] Audio (ES8311): clean boot tone + Happy Birthday melody +
      volume buttons -- see "Audio" below (the crackle saga)
- [x] Partition table grown to 3 MB OTA slots + 1 MB storage (USB-flashed)
- [ ] Decide longer-term project scope/direction

## Display bring-up (2026-10-02)

Real hardware wiring, confirmed via `docs.waveshare.com/ESP32-C5-Touch-LCD-2.8`
and the vendor's actual source repo
(`github.com/waveshareteam/ESP32-C5-Touch-LCD-2.8`,
`example/ESP-IDF-V554/08_bookesia`'s `components/esp32_c5_touch_lcd_2_8`) --
not guessed, not ported from a different board size:

- Panel: ST7789, SPI2. SCK=GPIO6, MOSI=GPIO7, DC=GPIO9, CS=GPIO10, 80MHz.
- **LCD reset and backlight are NOT direct ESP32 GPIOs.** Both are driven
  through the onboard **CH32V003 I/O-expander** chip over I2C (GPIO0=SDA,
  GPIO1=SCL, shared bus with touch/IMU/RTC/audio), via the real managed
  component `waveshare/custom_io_expander_ch32v003` (fixed I2C address
  `0x24`). LCD_RST is `IO_EXPANDER_PIN_NUM_1`; backlight is set via the
  expander's own PWM command (`custom_io_expander_set_pwm`), not an ESP32
  LEDC channel.
- ST7789 panel driver (`esp_lcd_new_panel_st7789`) ships with ESP-IDF's
  own built-in `esp_lcd` component -- no separate managed component for
  it is needed, despite the component name pattern suggesting otherwise.
- `esp_lcd_panel_invert_color(panel, true)` is required for this exact
  panel -- confirmed from vendor source, not optional.

Implementation: `main/display.c` + `main/display.h` (`display_init()`,
`display_fill(rgb565_color)`, `display_set_backlight(percent)`). Called
non-fatally from `app_main.c` after the WiFi+OTA server is already up, so
a display bug can never block the OTA recovery path.

### The one real bug hit: RGB565 byte order

`esp_lcd_panel_dev_config_t.data_endian`, left at its zero-initialized
default (`LCD_RGB_DATA_ENDIAN_BIG`), writes a hardware register on the
ST7789 panel itself telling it to expect MSB-first (big-endian) color
bytes -- confirmed by reading the real driver source
(`esp_lcd_panel_st7789.c`): this field is **not** a software byte-swap,
it configures the panel chip's own `RAMCTL` register. A plain
`uint16_t row_buf[x] = rgb565_color` store produces **little-endian**
byte pairs in memory on this (or any) RISC-V/ESP32 chip, so every pixel
arrived at the panel with its high/low byte swapped. A requested pure
green fill (`0x07E0`) rendered as **red** on real hardware -- exactly
what byte-swapping `0x07E0` -> `0xE007` produces (R=28/31, G=0, B=7/31).
Fixed by explicitly setting `.data_endian = LCD_RGB_DATA_ENDIAN_LITTLE`
to match how the buffer is actually filled. Confirmed fixed live
(correct green after the fix).

### Deployment note: a vendor-package build gap requires a manual patch

`waveshare/custom_io_expander_ch32v003`'s own `CMakeLists.txt` declares
`REQUIRES "driver"` (the pre-5.x umbrella driver component) but its
public header includes `driver/i2c_master.h`, which current ESP-IDF
(6.x) only provides via the split-out `esp_driver_i2c` component. This
breaks the build immediately after any fresh dependency resolve
(`rm -rf managed_components/` + `dependencies.lock`, or a clean clone).
**Manual fix required every time:** edit
`managed_components/waveshare__custom_io_expander_ch32v003/CMakeLists.txt`
and change `REQUIRES "driver"` to `REQUIRES "driver" "esp_driver_i2c"`
(documented in detail in `main/idf_component.yml`'s own comment, since
`managed_components/` is gitignored and this patch does not persist).

### Deployed via OTA (USB was unavailable this session)

The USB passthrough chain (`hp.local` Proxmox -> `debian.local` VM ->
`sandbox` container, documented above) was not working this session --
the board wasn't visible in `lsusb` on either host, possibly related to
the battery being added around the same time (worth checking the USB
cable/power next time USB access is needed, e.g. for serial log viewing
-- there is currently no log-over-HTTP endpoint on this device). All
deployment this session went through the existing pull-OTA path instead
(`/ota?key=...&url=...`), published to the same nginx host Vegena/lumen
already use (`debian.local:8091`, `vegena-esp32-panel-web` container,
`/home/mathias/vegena-ota/firmware/echo/echo.bin`).

**OTA key**: `secrets/secrets.h`'s `OTA_KEY` now matches the shared
workspace convention (`"vegena-ota"`, same as Vegena's real
`OTA_PASSWORD` and lumen's documented deploy env) -- the file previously
still had the unmodified example placeholder (`"changeme"`), which was
what the device already running v4 actually had baked in from its
original USB flash.

## Audio (ES8311) — the crackle saga (2026-10-03)

Audio output works (clean boot tone + a "Happy Birthday" melody triggered
from an on-screen button, with volume up/down). Getting there took ~12
failed attempts chasing the wrong causes, so the real root cause and the
diagnostic method are documented here in detail.

### Hardware wiring (verified against the vendor BSP, not guessed)
- Codec: **ES8311** on the shared I2C bus (`ES8311_CODEC_DEFAULT_ADDR`).
- I2S: SCLK=GPIO24, WS/LRCK=GPIO25, DOUT=GPIO26, DSIN=GPIO27, **MCLK=NC**
  (not wired -- `use_mclk=false`, the ES8311 clocks off BCLK), I2S_NUM_0,
  master, **16-bit MONO** slot (the esp_codec_dev driver maps our
  `.channel=1` onto one active I2S slot -- do NOT switch to stereo; the
  driver handles mono, confirmed in `managed_components/espressif__
  esp_codec_dev/platform/audio_codec_data_i2s.c`).
- Speaker PA enable: CH32 expander pin `IO_EXPANDER_PIN_NUM_3`
  (`IO_POWER_AMP_IO`), active HIGH. The codec's own `pa_pin` is NC on this
  board; the PA is enabled separately via the expander.
- The vendor's `08_bookesia` BSP full-duplex pattern is matched: the I2S
  channel is created with BOTH tx+rx handles and both are handed to
  `audio_codec_new_i2s_data`.

### The crackle: root cause was FOUR things, not one
The "sparks/crackle" was NOT a single bug, which is why single-variable
guesses never fully fixed it. Ruled out with evidence (don't re-chase
these): hardware (factory firmware plays clean on the same speaker),
amplitude/clipping (low amplitude crackles identically), DMA
underrun/task-starvation (crackles even at task prio 20 with LVGL stopped
+ sensors paused), power (USB vs battery identical), mono-vs-stereo (tried,
no difference), clock rate (measured playback wall-clock == expected
duration, so I2S clocks at the right 16 kHz). The actual fixes, all in
`main/audio.c`:
1. **Per-note amplitude envelope** (10 ms raised-cosine fade in/out) --
   the dominant source. A raw sine cut hard at each note edge clicks on
   every note. This was the breakthrough (flagged by an external audio
   analysis after many self-attempts missed it).
2. **Drain before close** -- the I2S DMA buffer is deep (~256 ms); calling
   `esp_codec_dev_close` right after the last write cut the tail
   mid-sample and popped the PA. Write trailing silence + `vTaskDelay(300)`
   so the DMA drains fully first.
3. **Large generation chunk** (2048 samples = 128 ms/write, not 256) --
   8x fewer write calls, far more generation headroom, killed the
   remaining sporadic underruns. (2048 int16 is `static`, not on the
   song task's 4 KB stack.)
4. **Continuous phase accumulator across the whole melody** -- generating
   each note from `sinf(w * pos)` with `pos` reset per note created a
   phase discontinuity at note boundaries (a steady single tone was clean,
   but the multi-note melody sparked at transitions). A running `phase +=
   w` carried across notes (wrapped at 2*pi) + the envelope makes every
   transition click-free.

Deeper DMA buffers (`dma_desc_num=8, dma_frame_num=512`) and a high song-
task priority are also set, as cheap insurance -- but they were NOT the
fix (the crackle persisted with them alone). The song plays on its own
FreeRTOS task so the ~8 s melody never blocks the LVGL event task; the
screen stays live during playback.

### The decisive diagnostic step
What finally cracked it: **flashing the verified factory backup
(`factory/factory_backup_esp32c5.bin`) over USB and confirming the factory
firmware's audio is clean on the same speaker.** That one test proved the
hardware was fine and the bug was 100% in our code -- which stopped the
hardware/power rabbit-holes. The factory image is a full 32 MB raw dump;
it can only be restored over USB (not OTA -- OTA writes only an app
partition, and our partition table differs from factory). Re-flash echo
over USB afterwards.

## Audio / UI task-priority note (single-core ESP32-C5)
The ESP32-C5 is **single-core** -- LVGL (prio 4), the sensor poll (3),
WiFi, and the audio task all share one core. This did NOT turn out to be
the audio crackle cause (see above), but it's a real constraint to keep in
mind for any future continuous-realtime work: a long-running realtime task
should run at a higher priority than LVGL, and heavy per-tick LVGL work
(full-screen redraws, chart updates) is the thing most likely to starve a
lower-priority task on this chip.
