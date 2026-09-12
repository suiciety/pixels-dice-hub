# Project plan

## Goal

Build an ESP-IDF device that discovers and registers up to eight Pixels dice,
shows their latest completed rolls on a 172 x 320 color display, and publishes
the same state to Home Assistant.

The display has a permanent aggregate area. Activating that area cycles:

`SUM -> HIGH -> LOW -> SUM`

A separate clear control starts a new aggregate round.

## Selected hardware

The selected target is the **Waveshare ESP32-C6-Touch-LCD-1.47**:

- 172 x 320 JD9853 display
- AXS5106L capacitive touch controller
- QMI8658 six-axis IMU for automatic orientation
- ESP32-C6 with 8 MB flash and no PSRAM

The similarly named ESP32-S3-Touch-LCD-1.47 has touch and PSRAM but no IMU.
The original ESP32-C3-LCD-1.47 has neither touch nor an IMU suitable for the
requested interaction.

The implementation uses ESP-IDF rather than ESPHome. Direct SDK access gives
better control over the C6's limited internal RAM, BLE scan scheduling, display
buffers, touch driver, and runtime orientation.

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
dynamic tiles           dashboard + roll history
```

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
- A die becomes stale after a configurable timeout, initially 15 seconds.
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

Planned repository layout after approval:

```text
pixels-dice-hub/
├── components/
│   ├── pixels_protocol/
│   ├── esp_lcd_jd9853/
│   └── esp_lcd_touch_axs5106/
├── main/
│   ├── app_main.cpp
│   ├── board.cpp
│   ├── dice_model.cpp
│   ├── pixels_ble.cpp
│   └── ui.cpp
├── tests/
├── docs/
├── CMakeLists.txt
└── sdkconfig.defaults
```

The C++ application owns BLE parsing, slot state, persistence, aggregate
calculation, board drivers, and LVGL layout. Home Assistant integration remains
a later network/API phase rather than a dependency of the local dashboard.

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

## Principal risks

| Risk | Mitigation |
|---|---|
| C6 has no PSRAM | Partial double buffers, fixed-size state, connectionless BLE only |
| Single-core Wi-Fi/BLE/display contention | Add networking only after eight-die soak testing |
| BLE advertisements may be missed | Continuous active scan, fragment merging, duplicate tolerance, real eight-die soak test |
| LVGL/BLE memory pressure | Fixed-size state, partial display buffer, no unnecessary GATT clients |
| Board display needs custom offsets/init | Start from Waveshare ST7789 sequence and verify on hardware |
| Protocol revisions | Strict length checks, version-aware mappings, captured packet fixtures |
