# Pixels Dice Hub

ESP-IDF firmware for monitoring and interacting with up to eight
[Pixels](https://gamewithpixels.com/) dice. It provides a compact touchscreen
dashboard for the Waveshare ESP32-C6-Touch-LCD-1.47 and a responsive web
dashboard that can also run on a headless classic ESP32.

The adaptive dice grid shows each die type, live roll state, and latest value.
A permanent aggregate panel cycles through sum, highest, and lowest, while a
separate clear action starts a new round. Repeated rolls from the same die are
included, and the newest 20 completed rolls are retained in RAM both globally
and per die.

> [!NOTE]
> This is an independent community project and is not affiliated with or
> endorsed by Systemic Games or the Pixels team.

## Hardware targets

| Target | Interface | Status |
| --- | --- | --- |
| Waveshare ESP32-C6-Touch-LCD-1.47 | 172 x 320 touch display and web UI | Builds; physical touchscreen validation still required |
| Waveshare ESP32-C6-Touch-AMOLED-1.64 | 280 x 456 touch display and web UI | Planned |
| Classic ESP32/ESP32-WROOM | Web UI only | Hardware tested |
| Generic ESP32-C3/C6/S3 boards | Web UI only | Planned |
| Selected ESP32-S3 LCD/AMOLED boards | Touch display and web UI | Provisional pending exact models |

The C6 board combines a JD9853 LCD, AXS5106L capacitive touch controller, and
QMI8658 IMU. The dashboard automatically switches between portrait and
landscape layouts. Planned boards will share the application through
board-specific hardware profiles; see the
[project plan](docs/project-plan.md#hardware-targets) for selection criteria
and porting phases.

## Features

- Discovers and pairs up to eight Pixels dice using BLE advertisements.
- Persists paired Pixel IDs in NVS without requiring BLE bonding.
- Shows die type, latest value, live state, battery, charging, RSSI, and
  online status.
- Retrieves firmware, installed-profile, available-flash, and temperature
  details when a die is selected.
- Tracks sum, highest, or lowest across a manually cleared round.
- Guides D20 checks, advantage/disadvantage, success pools,
  keep-highest/lowest pools, and damage rolls with modifiers and critical
  doubling from the touchscreen or web dashboard.
- Stores the newest 20 overall rolls and 20 rolls per die in RAM.
- Serializes identification and information commands through one short-lived
  GATT connection at a time.
- Provides a responsive, dependency-free web dashboard embedded in firmware.
- Runs the touchscreen with Wi-Fi off by default for higher BLE scan duty.
- Supports four-way touchscreen orientation using the onboard IMU.

## Planning artifacts

- [Project plan](docs/project-plan.md)
- [UI behavior and layout](docs/ui-spec.md)
- [Pairing mockup](docs/mockups/pairing.svg)
- [One-die dashboard](docs/mockups/dashboard-1-die.svg)
- [Four-dice dashboard](docs/mockups/dashboard-4-dice.svg)
- [Eight-dice dashboard](docs/mockups/dashboard-8-dice.svg)
- [Four-dice landscape dashboard](docs/mockups/dashboard-landscape-4-dice.svg)
- [Eight-dice landscape dashboard](docs/mockups/dashboard-landscape-8-dice.svg)
- [Current touchscreen UI audit](docs/touchscreen-ui-audit.md)

## Architecture

The normal dashboard uses active BLE scanning to collect advertisements and
scan responses without maintaining GATT connections. Pixels packets contain the
unique Pixel ID, die type/color, roll state, face, battery level, charging
state, and firmware timestamp. This supports eight tracked dice without
allocating eight BLE connection slots. Identification and information commands
use a bounded queue of serialized, short-lived GATT connections and resume
scanning after each command.

## Web dashboard

When enabled, both firmware variants create a WPA2 access point:

- Network: `Pixels-Dice`
- Password: `pixelsdice`
- Address: `http://192.168.4.1`

The single-page dashboard mirrors the dice grid and aggregate control, supports
pairing and removal, displays live roll states, and keeps the last 20
completed rolls overall plus the last 20 rolls for each paired die in RAM. Tap
or click a die card to open its individual history and briefly blink that
physical die for identification. On the touchscreen, a separate `INFO` view
shows battery level, charging state, roll state, signal strength, firmware
information, installed profile hash, available die flash, and temperatures,
and provides an unpair action. Closing the history view blinks the die again.
The aggregate has a separate `CLR` action that does not erase history.
Server-sent events notify the page of model changes, with periodic HTTP refresh
as a fallback. The Wi-Fi form can
additionally connect the device to an existing network while leaving the access
point available.

The touchscreen firmware defaults Wi-Fi off. Its pairing/settings page can
enable or disable Wi-Fi, and the saved choice persists across reboots. The
header shows `OFF`, `AP`, `...`, or `NET` for the current network state. BLE
scan duty is 95% while Wi-Fi is off and 50% while the radios are shared. The
headless firmware keeps Wi-Fi enabled because its web page is the only UI.

Roll history intentionally resets on reboot to avoid frequent flash writes.
Paired Pixel IDs, aggregate mode, the touchscreen Wi-Fi choice, and optional
Wi-Fi credentials are persisted in NVS. The web server is intended for a
trusted local network and should not be exposed directly to the internet.

## Guided dice calculator

Tap the result panel on the touchscreen to choose `SUM`, `HIGHEST`, `LOWEST`,
or a guided preset from a scrollable list. The web page continues to expose its
**Guided calculator** section directly. A started round names the paired
physical die to roll, its die type, the logical roll number, and any
substitution step. Only a completed roll from that instructed die advances the
round. The same physical die may therefore be requested multiple times when a
pool is larger than the number of paired dice.

Pool presets select the largest group of equivalent paired dice, such as two
D6s. Each throw may use any number of those dice, from one die through the
whole group, and every new completed roll counts toward the requested total.
Direct D20 advantage/disadvantage behaves the same way with paired D20s.
The pool die setting defaults to `AUTO` and can be overridden with an available
`D4`, `D6`, `D8`, `D10`, `D12`, or `D20`. For success pools, AUTO ignores dice
that cannot reach the configured success threshold, then prefers the largest
equivalent group.

The presets are:

- D20 check with modifier and optional DC
- D&D advantage or disadvantage
- success pool with configurable success threshold
- keep-highest or keep-lowest N from a configurable pool
- damage pool with modifier and optional critical doubling (dice subtotal is
  doubled, then the modifier is added)

If no physical D20 is paired, the calculator constructs an exact virtual D20
from paired D4, D6/D6 pipped, D8, D10, D00, or D12 dice. It prefers a physical
D20, then the recipe requiring the fewest expected physical rolls after
rejections are accounted for. Outcomes are normalized to zero-based digits
and combined as a mixed-radix integer. Only
`floor(product / 20) * 20` states are accepted; the result is
`state modulo 20 + 1`. A state in the remaining tail is rejected without bias,
and the display explicitly asks for every step of that virtual-D20 attempt to
be rerolled. Fudge and unknown dice are not used for exact substitution.

For pool, keep, and damage presets, D10 `0` is treated as `10`. D00 percentile
dice are reserved for exact D20 substitution and are not selected for pools.

Calculator configuration and active rounds are RAM-only and reset on reboot.
Roll history and the normal aggregate continue to operate while a calculator
round is active.

## Build the touchscreen target

ESP-IDF 5.3 or newer is required:

```sh
idf.py set-target esp32c6
idf.py build
idf.py flash monitor
```

Host-side protocol tests do not require ESP-IDF:

```sh
./tests/run-host-tests.sh
```

## Build the headless target

For an ESP32-WROOM-based Duinotech board:

```sh
cd headless
idf.py set-target esp32
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

The generated application image is
`headless/build/pixels_dice_hub_headless.bin`. Change the serial device as
needed.

## Current limitations

- Histories and the active aggregate round reset on reboot.
- Calculator configuration, results, and active rounds reset on reboot.
- The local web interface is HTTP-only and has no authentication beyond the
  Wi-Fi network password.
- The touchscreen layouts are functional but remain constrained at the
  eight-die maximum; see the
  [touchscreen UI audit](docs/touchscreen-ui-audit.md).
- OTA partitions and firmware updates are not yet implemented.
- Named Pixels animation profiles are not exposed because animation indexes
  depend on the dataset installed on each die.

## References

- [Pixels communications protocol](https://github.com/GameWithPixels/.github/blob/main/doc/CommunicationsProtocol.md)
- [Official Pixels Unity plugin](https://github.com/GameWithPixels/PixelsUnityPlugin)
- [Arduino Pixels Dice library](https://github.com/axlan/arduino-pixels-dice)
- [Original ESP32 example](https://gist.github.com/JpEncausse/cb1dbcca156784ac1e0804243da8e481)
- [Waveshare ESP32-C6-Touch-LCD-1.47](https://docs.waveshare.com/ESP32-C6-Touch-LCD-1.47)

## License

Pixels Dice Hub is available under the [MIT License](LICENSE). See
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for bundled component
attribution.
