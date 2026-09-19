# Touchscreen UI audit

These mockups use the display's native pixel dimensions. Geometry follows the
current LVGL implementation; desktop font rasterization is only an
approximation of LVGL's bundled Montserrat fonts.

## Native layouts

| View | Canvas | Main geometry |
|---|---:|---|
| Portrait dashboard | 172 x 320 | 26 px header, 156 x 216 content, 156 x 62 aggregate |
| Landscape dashboard | 320 x 172 | 26 px header, 230 x 138 content, 72 x 138 aggregate |
| Pairing/settings | Full screen | 30 px actions, 48 px scroll rows |
| Die history | Full screen | 38 px heading/actions area, 36 px scroll rows |
| Die information | Full screen | Scrollable telemetry with a separate unpair action |

## Current strengths

- Full dice tiles remain comfortably tappable with eight dice.
- State is conveyed by a colored ring and tile treatment.
- The aggregate remains visible in both orientations.
- Pairing, Wi-Fi, history, die information, unpairing, and blink identification
  are reachable from the touchscreen.
- The history page preserves its scroll position unless a new roll arrives.

## Risks at eight dice

- Dense portrait tiles use a 32 px outlined value, compact battery gauge, and
  status ring; two-digit values remain the limiting case.
- Landscape tiles are 54 x 67 px. Two-digit values and the battery gauge need
  physical validation at the eight-die maximum.
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
| Header pair button | 36 x 26 |
| Aggregate clear (portrait) | 42 x 48 |
| Pairing Done/Remove/Add | 54 x 30 |
| Wi-Fi toggle | 156 x 30 |
| History Info/Back | 46 x 30 |

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
