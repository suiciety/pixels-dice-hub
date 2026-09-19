# Project plan

## Goal

Build portable ESP-IDF firmware that discovers and registers up to eight
Pixels dice, presents their state on either a local touchscreen or a web-only
dashboard, and can be built for several common ESP32 development boards.

The display has a permanent aggregate area. Activating that area cycles:

`SUM -> HIGH -> LOW -> SUM`

A separate clear control starts a new aggregate round.

## Hardware targets

The initial display target is the
**Waveshare ESP32-C6-Touch-LCD-1.47**:

- 172 x 320 JD9853 display
- AXS5106L capacitive touch controller
- QMI8658 six-axis IMU for automatic orientation
- ESP32-C6 with 8 MB flash and no PSRAM

The firmware also supports the existing classic ESP32/ESP32-WROOM headless
build. Additional targets will be added through board profiles rather than by
forking the application:

| Target family | Local UI | Status | Notes |
|---|---|---|---|
| Waveshare ESP32-C6-Touch-LCD-1.47 | 172 x 320 LCD and touch | Current | Initial display target |
| Waveshare ESP32-C6-Touch-AMOLED-1.64 | 280 x 456 AMOLED and touch | Planned | CO5300 QSPI display, FT6146 touch, QMI8658 IMU, 16 MB flash |
| Classic ESP32/ESP32-WROOM | Web only | Current | Hardware-tested headless target |
| Generic ESP32-C3 DevKit/SuperMini | Web only | Planned | Low-cost single-core BLE/Wi-Fi target |
| Generic ESP32-C6 DevKit/SuperMini | Web only | Planned | Single-core target matching the display boards' MCU family |
| Generic ESP32-S3 | Web only | Planned | Dual-core target; PSRAM capability depends on module |
| ESP32-S3 LCD touchscreen boards | Display and web | Provisional | Exact board selected by controller, touch IC, PSRAM, and vendor documentation |
| ESP32-S3 AMOLED touchscreen boards | Display and web | Provisional | Exact board selected after purchase candidates are known |

The S3 display entries describe target families, not a promise that every
AliExpress board with the same screen size is interchangeable. Boards commonly
differ in display controller, touch controller, pin routing, power control,
flash/PSRAM package, and USB wiring even when their listing titles are similar.
An exact product link, schematic, or vendor example is required before adding
a concrete board profile.

The implementation uses ESP-IDF rather than ESPHome. Direct SDK access gives
better control over memory, BLE scan scheduling, display buffers, touch
drivers, and runtime orientation across the supported MCU families.

### Board selection criteria

Candidate boards should be assessed in this order:

1. BLE and Wi-Fi support compatible with ESP-IDF and NimBLE.
2. Published schematic, pin map, and working ESP-IDF or Arduino display example.
3. Known display and touch controller part numbers.
4. At least 8 MB flash for display builds; 16 MB is preferred for future OTA.
5. PSRAM is preferred for high-resolution S3 displays but is not required when
   partial LVGL buffers are practical.
6. Touch input for display builds; an IMU is preferred for automatic rotation.
7. USB flashing/debugging that does not consume required peripheral pins.
8. Stable availability from more than one seller where possible.

## Proposed architecture

```text
Pixels dice advertisements
          |
          v
NimBLE active scanner
          |
          v
Pixels protocol/model layer
  - validate Pixels UUID/data lengths
  - parse little-endian service/manufacturer data
  - map die type/color and face index
  - track up to 8 registered Pixel IDs
  - debounce repeated advertisements
  - persist registration and aggregate mode
          |
          +--------------------+
          |                    |
          v                    v
LVGL dashboard          HTTP/JSON web UI
board capabilities      dashboard + roll history
and dynamic tiles
```

The BLE, model, command, persistence, API, and web layers remain
board-independent. A compile-time board profile supplies display dimensions,
display and touch initialization, orientation capabilities, brightness/power
handling, and board pin assignments.

### Why advertisements, not eight connections

Pixels advertisements contain all data required by the dashboard:

- Name and RSSI
- Pixel ID and firmware build timestamp
- LED count and packed die type/color
- Roll state and current face index
- Battery level and charging flag

BLE connections consume additional memory and radio time. Scanning
advertisements avoids that constraint and is sufficient for the requested
read-only dashboard.

The word **paired** in this project means that a discovered Pixel ID is saved
in the device's allow-list. It does not require BLE bonding or a permanent
GATT connection.

## Data model

Each of eight slots stores:

| Field | Purpose |
|---|---|
| Pixel ID | Stable registration key |
| BLE address | Current radio address for diagnostics |
| Name | User-set Pixels name |
| Die type | D4, D6, D8, D10, D00, D12, D20, D6 Pipped, or D6 Fudge |
| Colorway | Optional tile accent |
| Last face | Converted display value, not raw face index |
| Roll state | Unknown, rolled, handling, rolling, crooked, or on-face |
| Battery | Percentage and charging state |
| RSSI | Signal indication |
| Last seen | Offline/stale detection |
| Has rolled | Excludes an unrolled slot from aggregate calculations |

The packed design byte is decoded using the official library convention:

- High nibble: die type
- Low nibble: colorway

Face conversion follows the official type rules:

- D4/D6/D8/D12/D20: `face index + 1`
- D10: `0..9`
- D00: `0, 10, ... 90`
- D6 Fudge: numeric face index initially; symbol mapping will be finalized from
  captured hardware data because the official helper currently returns
  `face index + 1`
- Older affected firmware receives the official D4/D6 face correction

## State rules

- A last-roll value changes only when a completed `Rolled` state is observed.
- Repeated advertisements for the same roll do not create duplicate events.
- `Handling`, `Rolling`, and `Crooked` update status decoration but do not
  replace the last valid roll.
- A paired die is shown as offline after two minutes without an advertisement;
  its registration and last completed value remain visible.
- Every completed roll from a registered die is added to the current aggregate
  round, including repeated rolls from the same die.
- Last rolls remain visible after the aggregate round is cleared.
- Aggregate calculations include completed roll events since the last clear.
- With no valid rolls, the aggregate displays an em dash.
- The last 20 completed rolls for paired dice are retained in RAM and exposed
  on the web dashboard.
- Clearing the aggregate does not clear roll history.

## Pairing flow

1. Open the pairing page.
2. Scan actively so both the primary advertisement and scan response are
   received, then merge their payloads by BLE address.
3. Show unregistered dice sorted by signal strength.
4. Roll a die to make its row pulse and make identification easy.
5. Activate `ADD` to assign the next free slot.
6. Registered dice appear above discovered candidates and can be removed.
7. Stop accepting additions at eight and explain that the dashboard is full.
8. Save registrations in NVS so they survive reboot.

The implemented identification action connects to one die at a time and sends
the Pixels `Blink` command, then disconnects and resumes advertisement scanning.

## Repository structure

Target repository layout:

```text
pixels-dice-hub/
├── boards/
│   ├── waveshare_c6_lcd_147/
│   ├── waveshare_c6_amoled_164/
│   ├── generic_esp32/
│   ├── generic_esp32c3/
│   ├── generic_esp32c6/
│   ├── generic_esp32s3/
│   └── <specific_s3_display_board>/
├── components/
│   ├── pixels_protocol/
│   ├── esp_lcd_jd9853/
│   ├── esp_lcd_touch_axs5106/
│   └── <drivers required by enabled display targets>/
├── main/
│   ├── app_main.cpp
│   ├── board.h
│   ├── dice_model.cpp
│   ├── pixels_ble.cpp
│   └── ui.cpp
├── headless/
├── tests/
├── docs/
├── CMakeLists.txt
└── sdkconfig.defaults
```

The C++ application owns BLE parsing, slot state, persistence, aggregate
calculation, and LVGL layout. Board profiles own hardware initialization.
Home Assistant integration remains a later network/API phase rather than a
dependency of the local dashboard.

### Build strategy

- Each MCU family has its own ESP-IDF build directory and `sdkconfig.defaults`.
- Display profiles compile only the drivers required by that board.
- Headless profiles omit LVGL, display, touch, and IMU components.
- UI code obtains resolution and capabilities from the board profile rather
  than using hard-coded dimensions.
- The build produces a clearly named artifact for each concrete board.
- Exact pin mappings are never shared between boards merely because they use
  the same ESP32 module or display size.
- The 16 MB AMOLED target receives a board-specific partition table so its
  additional flash can eventually support OTA and assets.

## Optional Home Assistant surface

Planned entities:

- Per die: last roll, die type, battery, RSSI, online state, and roll state.
- Aggregate: value and selected mode.
- Controls: aggregate mode, clear round, scan/pairing mode, and remove all
  registered dice.
- Diagnostics: registered count, advertisements received, malformed packets,
  and last packet age.

## Implementation phases

### Phase 1 - Protocol core

- Define bounded packet structures and enums.
- Parse captured advertisement fixtures.
- Implement die-type and face conversion.
- Test malformed, short, duplicated, and old-firmware packets.

### Phase 2 - ESP-IDF application

- Register a NimBLE active scanner without opening persistent connections.
- Maintain eight fixed slots without heap churn in the scan callback.
- Persist registered Pixel IDs and aggregate mode.
- Publish per-die and aggregate state.

### Phase 3 - Display bring-up

- Configure the selected Waveshare board and its display.
- Verify orientation, offsets, colors, backlight, and partial buffering.
- Add the static LVGL shell and status indicators.

### Phase 4 - Adaptive dashboard

- Implement layouts for 1, 2, 3-4, 5-6, and 7-8 dice.
- Update only changed labels/styles.
- Add rolling, stale, low-battery, and crooked-state treatments.
- Implement aggregate mode cycling.

### Phase 5 - Pairing and persistence

- Add discovery list, add/remove actions, eight-slot enforcement, and saved
  registration restore.
- Add optional sequential blink identification if resource tests allow it.

### Phase 6 - Hardware validation

- Test mixed dice types and eight simultaneously advertising dice.
- Test Wi-Fi/API traffic while scanning and updating the screen.
- Measure missed-roll rate, memory headroom, UI latency, reboot recovery, and
  stale-device behavior.

### Phase 7 - Multi-board foundation

- Replace hard-coded display dimensions with board capabilities.
- Split current JD9853/AXS5106 initialization into the LCD board profile.
- Move brightness, touch transform, and IMU orientation behind the board API.
- Add scalable UI metrics for dimensions, pixel density, fonts, spacing, and
  minimum physical touch-target size.
- Stop rebuilding the complete dashboard on periodic status refreshes; update
  existing LVGL objects so higher-resolution displays do not add avoidable
  single-core CPU load.
- Add repeatable build commands and artifact names for every concrete target.

### Phase 8 - Additional headless targets

- Add generic ESP32-C3 and ESP32-C6 headless profiles.
- Add a generic ESP32-S3 headless profile with optional PSRAM configuration.
- Verify BLE scanning, Wi-Fi/SSE responsiveness, free heap, and flash layout on
  one representative board from each MCU family.
- Keep the classic ESP32-WROOM build as the minimum compatibility baseline.

### Phase 9 - Waveshare C6 AMOLED port

- Integrate the CO5300 QSPI panel using Waveshare's reference initialization.
- Integrate FT6146 touch and verify coordinate transforms in all orientations.
- Add 280 x 456 portrait and 456 x 280 landscape UI metrics and mockups.
- Retain partial double buffering; do not allocate a full framebuffer.
- Add controller-driven brightness, idle dimming, screen-off wake, and
  AMOLED burn-in mitigation.
- Validate display flush time while BLE scanning and while Wi-Fi is active.

### Phase 10 - Selected ESP32-S3 display ports

- Select concrete LCD and AMOLED boards only after exact listings are known.
- Prefer boards with PSRAM, documented ESP-IDF support, and maintained
  display/touch drivers.
- Add one board profile per electrical design, even when multiple listings use
  the same nominal screen.
- Reuse the common UI and choose a resolution/DPI metrics profile where
  possible; add a new layout profile only when aspect ratio requires it.
- Hardware-test touch, brightness, sleep/wake, PSRAM/DMA compatibility, BLE
  coexistence, and USB flashing before declaring a target supported.

## Acceptance criteria

- Registers zero to eight Pixels dice and restores them after reboot.
- Displays each registered die's correct type and latest completed roll.
- Adapts the dashboard without clipped labels at every supported count.
- Keeps the aggregate visible in every dashboard layout.
- Cycles sum, high, and low from the aggregate control.
- Correctly handles D10, D00, and D6 Fudge values.
- Does not count handling, rolling, crooked, malformed, or duplicate packets
  as completed rolls.
- Exposes equivalent state and controls to Home Assistant.
- Continues receiving all eight dice during normal Wi-Fi/API activity.
- Builds shared application code for each concrete board target without
  conditional pin or controller logic leaking into the model and BLE layers.
- Uses usable physical touch targets and unclipped layouts at every supported
  display resolution and orientation.
- Documents exact module, flash/PSRAM variant, controller ICs, and pin mapping
  for every supported display board.

## Principal risks

| Risk | Mitigation |
|---|---|
| C6 has no PSRAM | Partial double buffers, fixed-size state, connectionless BLE only |
| Single-core Wi-Fi/BLE/display contention | Add networking only after eight-die soak testing |
| BLE advertisements may be missed | Continuous active scan, fragment merging, duplicate tolerance, real eight-die soak test |
| LVGL/BLE memory pressure | Fixed-size state, partial display buffer, no unnecessary GATT clients |
| Board display needs custom offsets/init | Start from the vendor's controller-specific sequence and verify on hardware |
| Protocol revisions | Strict length checks, version-aware mappings, captured packet fixtures |
| Similar board names hide incompatible wiring | Require exact product revision, schematic, and pin map for every board profile |
| Higher-resolution display increases CPU and RAM pressure | Partial buffers, incremental LVGL updates, measured flush and heap budgets |
| AMOLED static-content burn-in | Dark theme, idle dimming/screen-off, and optional pixel shifting |
| Generic S3 board may ship without expected PSRAM | Record the exact module suffix and provide PSRAM and non-PSRAM configurations |
| Supporting too many one-off boards becomes costly | Promote only hardware-tested profiles to supported status; keep other profiles provisional |
