# UI specification

## Visual direction

- Portrait canvas: 172 x 320 pixels.
- Dark background for strong contrast and reduced visual noise.
- Dice use distinct accent colors; color is not the only status indicator.
- The latest value is always the largest text inside a die tile.
- The aggregate occupies the bottom 56 pixels on every dashboard.
- Minimum intended touch target: 44 x 44 pixels.

## Adaptive dashboard

| Registered dice | Grid | Approximate tile area |
|---:|---|---|
| 0 | Empty-state card | 160 x 210 |
| 1 | 1 column x 1 row | 160 x 238 |
| 2 | 1 column x 2 rows | 160 x 116 |
| 3-4 | 2 columns x 2 rows | 78 x 116 |
| 5-6 | 2 columns x 3 rows | 78 x 76 |
| 7-8 | 2 columns x 4 rows | 78 x 56 |

Each tile can contain:

- Die type and slot number
- Last completed roll
- Compact battery indicator
- Text state for rolled, handling, rolling, crooked, on-face, or offline
- State-colored border/background highlighting

## Interaction

- Tap the aggregate panel to cycle `SUM`, `HIGH`, and `LOW`.
- Tap the separate `CLEAR` control to begin a new aggregate round.
- Clearing a round does not erase the last-20-roll history.
- Tap a die tile to identify it with a short blink and open its newest-first
  history of up to 20 completed rolls. The page also displays battery level,
  charging state, live roll state, and signal strength. Closing the history
  view blinks it again.
- Tap the header pairing icon to open registration.
- The header shows Wi-Fi as `OFF`, `AP`, `...` while connecting, or `NET`.
- Tap the Wi-Fi row on the pairing page to enable or disable networking. The
  touchscreen defaults it off and remembers the selection across reboots.
- Tap an available die's `ADD` control to register it.
- Tap a registered die's remove control, then confirm, to unregister it.
- Rolling a discovered die highlights its row to identify the physical die.

## Status styling

| State | Treatment |
|---|---|
| Rolled | Green highlight and `ROLLED` label |
| On face | Purple highlight and `ON FACE` label |
| Rolling | Cyan highlight and `ROLLING` label |
| Handling | Amber highlight and `HANDLING` label |
| Crooked | Red highlight and `CROOKED` label |
| Stale/offline | Dim tile and broken-link indicator |
| Battery under 15% | Red battery indicator |
| Charging | Lightning indicator |
| No completed roll | Em dash as value |

## Mockups

The SVGs use the real 172 x 320 aspect ratio and are intended to establish
information hierarchy rather than final typography:

- [Pairing](mockups/pairing.svg)
- [One die](mockups/dashboard-1-die.svg)
- [Four dice](mockups/dashboard-4-dice.svg)
- [Eight dice](mockups/dashboard-8-dice.svg)
- [Landscape with four dice](mockups/dashboard-landscape-4-dice.svg)
- [Landscape with eight dice](mockups/dashboard-landscape-8-dice.svg)

## Automatic orientation

The selected ESP32-C6 touch board includes a QMI8658 accelerometer/gyroscope.
The firmware samples acceleration at 10 Hz, requires eight stable readings
before rotating, and applies hysteresis near diagonal/flat positions to avoid
screen chatter.

Portrait uses a bottom aggregate panel. Landscape uses a permanent right-side
aggregate panel, leaving the wider area for a 2-, 3-, or 4-column dice grid.
LVGL rotates both display output and touch coordinates together.
