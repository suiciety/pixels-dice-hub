# Touchscreen UI audit

These mockups use the display's native pixel dimensions. Geometry follows the
current LVGL implementation; desktop font rasterization is only an
approximation of LVGL's bundled Montserrat fonts.

## Native layouts

| View | Canvas | Main geometry |
|---|---:|---|
| Portrait dashboard | 172 x 320 | 26 px header, 156 x 226 content, 156 x 52 aggregate |
| Landscape dashboard | 320 x 172 | 26 px header, 230 x 138 content, 72 x 138 aggregate |
| Pairing/settings | Full screen | 30 px actions, 48 px scroll rows |
| Die history | Full screen | 58 px heading/status area, 36 px scroll rows |

## Current strengths

- Full dice tiles remain comfortably tappable with eight dice.
- State is conveyed by both text and color.
- The aggregate remains visible in both orientations.
- Pairing, Wi-Fi, history, battery, charging, RSSI, and blink identification
  are reachable without nested menus.
- The history page preserves its scroll position unless a new roll arrives.

## Risks at eight dice

- Portrait tiles are 76 x 53 px. A 14 px heading, 24 px value, and 12 px state
  consume nearly the complete vertical interior, so text can look crowded.
- Landscape tiles are 54 x 67 px. `D20` plus `100%` can overlap horizontally,
  and long states such as `HANDLING` exceed the padded content width.
- The landscape aggregate is only 72 px wide. Long mode/count text needs
  abbreviation or two lines.
- The current pairing Wi-Fi text is wider than 156 px in states such as
  `WIFI CONNECTED - TAP TO DISABLE`, so it can clip.
- Long pairing names and IDs can render beneath the right-side action button
  because the left labels have no explicit maximum width.

## Touch target risks

The intended minimum target is 44 x 44 px, but several controls are shorter:

| Control | Current size |
|---|---:|
| Header pair button | 28 x 24 |
| Aggregate clear | 52 x 22 |
| Pairing Done/Remove/Add | 54 x 30 |
| Wi-Fi toggle | 156 x 30 |
| History Back | 54 x 30 |

The dice tiles and aggregate panel themselves meet the target. The smaller
controls may still work with a stylus or careful touch, but are the highest
priority for physical usability testing.

## Mockups

Each PNG is rasterized at the exact native panel resolution; the SVG is the
editable source.

| View | Native PNG | SVG source |
|---|---|---|
| Eight dice, portrait | [172 x 320 PNG](mockups/current-dashboard-8-portrait.png) | [SVG](mockups/current-dashboard-8-portrait.svg) |
| Eight dice, landscape | [320 x 172 PNG](mockups/current-dashboard-8-landscape.png) | [SVG](mockups/current-dashboard-8-landscape.svg) |
| Pairing and Wi-Fi | [172 x 320 PNG](mockups/current-pairing-portrait.png) | [SVG](mockups/current-pairing-portrait.svg) |
| Per-die status/history | [172 x 320 PNG](mockups/current-history-portrait.png) | [SVG](mockups/current-history-portrait.svg) |
