# Echo / Lumen review and Vegena page parity — 2026-10-14

## Result

Implemented changes in both projects. Echo now has 24 pages; Lumen has 30. Existing page IDs remain stable; additions are appended. The firmware has been built locally, but neither device has been flashed. Physical camera quality, display stability and OTA reliability still require device qualification.

Reviewed Echo from HEAD `38af364` and Lumen from HEAD `432b140`, including the working tree. Lumen already contained edits to `weblog_http.cpp`, `sdkconfig.defaults`, `ota_url_check.cpp`, and `test_app.cpp`, plus incident/production notes. Those changes were preserved. No Home Assistant configuration, collector service, remote firmware host or device state was changed.

## Findings and implemented fixes

| Finding | Evidence and consequence | Implemented response |
| --- | --- | --- |
| Lumen's OTA pause did not prove that background work had stopped | A boolean pause can race an already-running scan, request, WebSocket or camera stream. Internal stacks, network buffers and DMA activity remain present during OTA. | Admission/completion leases and a confirmed quiescence barrier. OTA waits up to 15 seconds and returns `poll_pause_timeout` rather than starting alongside unfinished work. WiFi scans, collector and homelab requests, HA service calls, camera streaming and camera downloads participate. |
| Lumen's internal OTA and WS allocation footprint was unnecessarily large | OTA used a 12 KB task stack containing a 4 KB transfer array; HA WS used a 16 KB RX buffer. | OTA uses an 8 KB stack and a 1.5 KB PSRAM transfer buffer. WS RX is 4 KB; reassembly remains bounded. The WS client is destroyed during maintenance and partial-message capacity is released. Internal free heap, largest internal block, largest DMA block and worker stack headroom are logged before image verification. |
| Lumen OTA diagnostics could identify the wrong attempt while pausing | The new wait begins before the worker is started. A watchdog timestamp of zero at long uptime could also cause premature recovery. | Start the attempt deadline before pausing and publish the new manifest identity immediately. Boot reconciliation recognizes interrupted `pausing` attempts. Allocation/pause failures have explicit phases. |
| Lumen deployment could report success from one transient valid observation | Exact ELF identity and confirmation were already checked, but a confirmed image repeatedly rebooting could still pass once. | Require four consecutive healthy observations from one boot different from the baseline boot. A reset, wrong image, missing diagnostic response or unhealthy observation restarts the soak. Save the observations with deployment evidence. |
| Camera preview writer and reader did not share the same critical region | PPA wrote the shared preview before taking the mutex that the reader used for copying. A frame could contain old and new pixels. | Hold the mutex across the blocking PPA write and ready publication. Consumers take a bounded five-millisecond lock and copy a coherent snapshot. Preview refresh runs only on the Camera page and pauses for maintenance. |
| Camera configuration could degrade image quality | Exposure was forced to approximately 95% of its maximum and the preview applied an additional 1.75× RGB gain. These can promote clipping and motion blur. | Remove forced exposure and software brightening. Register the repository-owned OV02C10 ISP/IPA profile in the build, allowing the calibrated exposure/AWB pipeline to operate. Validate the negotiated RGB565 dimensions and packed row layout. |
| Lumen's custom display path rendered into the active scanout buffer | The board-specific initializer used one framebuffer with partial software rotation. BSP defaults suggesting tear avoidance did not affect this custom path. | Two native framebuffers, CPU rotation of partial logical tiles into the inactive buffer, damage synchronization, and presentation at refresh boundaries. A presentation timeout freezes writes and logs the fault. This also removes display PPA competition with the camera. |
| Lumen cost/price charts mishandled negative and missing values | Y axes began at zero; zero/negative totals were treated as missing. Cumulative totals held flat across missing hours. | Signed axis ranges; valid zero/negative totals; cumulative curves stop at the first missing bucket. Cost API cumulative data follows the same rule. |
| Lumen's Energy page used current tariff × the whole day's consumption | Today's tariff varies, and household consumption is not identical to imported energy. | Show the authoritative Tibber bought-cost counter. Existing hourly household-load × hourly-tariff charts remain explicitly labelled estimates and show available-hour coverage. |
| Lumen HA values could remain displayed after disconnection | The scalar/group accessors returned previously stored values independently of WS connection state. | Accessors return unavailable when offline; disconnect invalidates tracked values so reconnect seeding fetches them again. Connection state is atomic. Seed HTTP bodies are capped at 32 KB. |
| Echo lacked Lumen-style monetary history | Existing energy and price charts did not provide hourly bought cost, sold income or cumulative costs. | Add an Energy costs page with Today/Yesterday, hourly bought/sold series, and a tap-to-switch accumulated view. Prefer recorded monetary statistics. Fall back to imported kWh × duration-weighted hourly price only when complete price coverage exists; explicitly label estimates. |
| Echo's standard fonts lacked Swedish glyphs used by collector content | Lumen already had generated Latin-1 and punctuation glyph coverage. | Reuse Lumen's compatible Montserrat 14 font for Echo's new content pages. |

### What the evidence does and does not establish

Lumen already released its HTTP transport before `esp_ota_end()` before this review. It would be incorrect to claim the exact same retained-transport defect from Echo was present. The identified issues concern competing resources, unacknowledged pauses and internal allocation headroom. The new telemetry supports checking the memory-pressure hypothesis on the actual device.

The display change removes a reachable scanout-writing race. The ESP-IDF DPI driver also explicitly reports external-memory underruns, which can cause visible blanking/blue output. No live underrun log or oscilloscope trace was available in this session, so the occasional blink is not proven fixed. The native panel timing table and lane settings have been retained; it needs a physical soak with camera, page transitions and OTA.

The shared HTTP server capacity is 23 handlers, covering the existing 21 routes and two new authenticated camera routes. Capability metadata includes the new routes.

The generated IPA configuration now contains `OV02C10`. This proves the calibration is built into the image, not that the particular camera module/lens is healthy. Sensor wiring, focus, lens condition, lighting and module variation cannot be judged from source. The preview is intentionally a center crop scaled to 360×270; it is not a full-resolution camera photograph.

## Vegena pages carried into the two projects

Equivalent pages are reused where they already exist; layouts are adapted for Echo's small screen and Lumen's landscape screen. New content pages scroll vertically.

| Vegena category | Echo | Lumen |
| --- | --- | --- |
| Power | Existing Energy now | Existing Power |
| Solar / grid power history | New Solar / grid history: last hour, minute averages, empty gaps | Existing Power rolling chart |
| Energy totals / hourly energy | Existing Energy today | Existing Energy |
| Energy costs | New Energy costs; hourly or accumulated, today/yesterday | Existing Cost, corrected estimates and signed values |
| Electricity / SE3 | Existing Prices, with timestamped quarter-hour slots | New Electricity details and hourly averages; existing Energy price overlay |
| Lights | New status and queued explicit On/Off controls | New status and queued explicit On/Off controls |
| Weather | New current weather | New current weather |
| Weather today | New four-period forecast | New four-period forecast |
| Temperatures | Existing dedicated page | New dedicated page, plus existing Home Assistant group view |
| Humidity | New dedicated page | New dedicated page, plus existing Home Assistant group view |
| Batteries | Existing dedicated page | New dedicated page, plus existing Home Assistant group view |
| Sensor graphs | Existing 24-hour sensor graphs | New selectable 24-hour sensor graphs, five-minute averages recorded since boot |
| Sun & Moon | New sun times and approximate moon illumination | New sun times and approximate moon illumination |
| Network | New collector ISP/IP/location and separately aged HA WAN latency/up/uptime | Same new page, alongside WiFi diagnostics |
| World / Markets | New FX, OMX and stock movers | Same new page |
| SL transit | New departures | Existing transit and disruption tables |
| Air quality | New AQI, pollutants and pollen | Same new page |
| Right now | New warning/holiday page alongside Overview | New warning/holiday page alongside Clock |
| Network clients | New Kuma service availability/down/important-service list | Same new page alongside Homelab |
| News | New bounded headline list | Existing richer expandable news table |
| Police events | New bounded incident summaries | Existing incident table |
| Device information | Existing Device info | Existing Device Info |

The pages share categories and data sources, not identical presentation. Neither the new Kuma page nor Vegena's corresponding page is a full router client inventory. The Right now page presents warnings and the next holiday, while existing overview/clock/power pages retain household summaries. Echo's news/police lists show at most 12 entries per fetched snapshot; Lumen retains its larger existing tables. Lumen sensor graphs hold 24 hours in RAM and restart on reboot; Echo retains its existing persisted history. No speculative sensor entities or router interfaces were added.

### Data contracts and freshness

- Collector: `http://192.168.2.7:8091/extra.json`, the existing Lumen/Vegena LAN source. Echo can override `ECHO_COLLECTOR_URL`; Lumen retains `LUMEN_COLLECTOR_URL`.
- Home Assistant: `sensor.vegena_panel` and `sensor.vegena_ambient`. Their existing template definitions are under `vegena/ha/`. Object-valued and JSON-string-valued `attributes.values` are supported, and bundle version 1 is required.
- Panel arrays use the same positional order as `vegena/ha/vegena_panel_sensor.yaml`. Do not reorder those HA arrays independently of their labels. The nine light controls use the explicit IDs in `dashboard_lights.h`.
- Bundle fetches run on existing owner tasks. No extra permanent network task is created for Echo. No request runs on the LVGL caller. HA bearer headers are attached only to separate HA clients; the public collector client never receives the token. Redirects are disabled.
- Echo bundle and Lumen HA-seed response bodies are capped at 32 KB. Lumen's existing collector response cap remains 64 KB. Each dashboard text section is capped at 1,536 bytes and lists at 12 rows.
- Successful malformed/missing sections become unavailable; failed transport retains the last snapshot with its original receipt time. Collector observation/fetch timestamps are retained where supplied. Re-fetching an old collector result does not hide its source age. Explicit `_stale` is respected.
- General dashboard receipt freshness expires after 15 minutes; supplied source time expires after 30 minutes, with future-clock tolerance. Echo transit expires after two minutes. WAN and collector network fields keep independent ages. A missing source clock is not invented.
- Failed/stale light data does not permit commands. Commands are explicit On/Off, bounded to a single queued request, and status changes are fetched from HA rather than optimistically assumed.
- Collector FX data without a source timestamp can only be assessed using bundle generation and receipt time. The UI cannot independently prove the upstream market quote's age.

Missing HA bundles or collector sections are visibly unavailable. Firmware installation does not install or reload Home Assistant templates.

## Energy accuracy and limits

Echo's new monetary recorder query is separate from the kWh query: unsupported monetary statistics cannot prevent the existing energy history from loading. The two new monetary entities are the same Tibber accumulated consumption-cost and production-compensation entities used in Lumen. Unit expectations are SEK.

Recorded monetary changes are preferred and may be zero or negative. Estimates require a fully covered timestamp interval and weight quarter-hour prices by duration; they assume uniform import within the hour. They are not metered quarter-hour cost. Today summary cards use authoritative counters including the current hour, while bars and accumulated history use completed hours. Yesterday requires recorded statistics; the current day's price array cannot estimate another day's costs.

Echo retains timestamp-based recorder hours (23/24/25 hours across daylight-saving transitions). Lumen's inherited collector format is 24 civil hourly buckets and 96 quarter-hour slots; the repeated/missing DST hour is not separately represented. That backend schema limitation remains. No quarters were relabelled as hourly metered costs.

Echo's history cache key changes to `history_v2` because the cached struct includes monetary fields. Old blobs are not interpreted using the new layout. Cached and partial data remain labelled.

## Validation

- Both final firmware builds completed: Echo ESP32-C5 / local IDF installation; Lumen ESP32-P4 / ESP-IDF 5.5.4.
- Echo existing native suite: 50 policy checks, 114 review regression checks and history-cache tests.
- New ASan/UBSan dashboard/cost tests: real collector fixture, object/string HA bundles, zero/negative readings, malformed/version mismatch, explicit stale data, source vs receipt freshness, monetary statistics, duration-weighted quarter-hour prices and missing coverage.
- Lumen existing native suite: 1,142 checks.
- New Lumen ASan/UBSan tests: rotated padded partial tiles, invalid bounds/stride, lease ownership, quiescence and 1,000 concurrent pause/admission races.
- Python deployment/WiFi/maintenance tests: 25 tests and 5 subtests passed. Entity-generator validation: 15 checks passed.
- `git diff --check` passed in both repositories.
- Shared dashboard C/header copies were checked for equality. The firmware toolchains compile the same projection against their own cJSON components.

Build evidence and exact artifact identities are recorded in `ECHO-LUMEN-BUILD-2026-10-14.json`. Lumen's out-of-tree build is `/private/tmp/lumen-echo-build`; Echo's build is `echo/build`. Existing LVGL deprecation/unused-variable warnings remain in Lumen. The sandbox dependency-manager process-enumeration workaround was confined to temporary build-support files; no SDK source was changed.

## Physical qualification still required

1. Flash using each project's documented provisioning procedure. Echo's existing first boot-affecting USB-flash rule still applies. Confirm rollback provisioning and the exact running ELF before OTA.
2. Navigate all 24 Echo and 30 Lumen pages repeatedly, including vertical scrolling and Swedish headlines. Check fresh/unavailable behavior with the collector or HA temporarily disconnected. Confirm each light targets the intended lamp.
3. On Lumen, exercise the Camera page, rapid page transitions and normal background updates for a sustained period. Look for display presentation timeout/underrun logs and declining heap/stack headroom.
4. Fetch authenticated `/api/v1/camera` and `/api/v1/camera/preview.bmp`. Confirm `profile_present`, advancing frames, frame age, coherent colors and zero recurring PPA/queue errors. The BMP isolates the display path from the processed preview, not the sensor hardware from the ISP.
5. Run Lumen OTA with camera/HA/collector previously active. Verify quiescence before download, verification heap headroom, exact ELF confirmation and the same-boot healthy soak. A camera stuck inside the driver's blocking `DQBUF` can prevent quiescence; OTA then rejects safely after the pause timeout rather than proceeding under unresolved DMA ownership.
6. Compare recorded monetary hour changes with Home Assistant and confirm zero/negative prices, gaps, yesterday and DST boundaries. Check estimate labels and that cumulative curves do not bridge unknown hours.

## Further improvements worth sharing

Echo's generated entity catalogue, explicit units/reading outcomes, timestamp-based price intervals and structured measurement freshness remain stronger than Lumen's hand-maintained entity table. Lumen's authenticated page-content API, richer homelab/CPU instrumentation and expandable news tables remain stronger than Echo's compact dashboard API/UI. This update shares the immediately applicable page projection, font, monetary history and maintenance lifecycle patterns; it does not merge incompatible LVGL versions, board drivers or OTA bootloaders into one implementation.

The bounded dashboard projection is intentionally identical but self-contained in both repositories so each can build independently. A versioned shared component would reduce copy drift once repository/release ownership for such a component is established.
