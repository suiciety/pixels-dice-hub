# Web UI

The firmware serves one responsive HTML page directly from program flash. It
does not use a filesystem, JavaScript framework, WebSocket library, or external
web assets.

## Runtime design

- The browser polls `/api/state` every 750 ms for a compact JSON snapshot.
- The existing fixed-size dice model supplies dashboard and pairing data.
- A 20-entry in-memory ring buffer records transitions into the completed-roll
  state for paired dice.
- Each paired die also has its own 20-entry in-memory history. Selecting a die
  card opens that die's newest-first roll list and briefly blinks the physical
  die. The detail view includes battery level, charging state, live roll state,
  signal strength, and online status. Closing the detail view blinks it again.
- Pair, remove, aggregate-mode, and clear-round actions use `/api/action`.
- Dice cards show the current roll state with state-colored highlighting.
- The aggregate counts every completed roll until manually cleared; history is
  retained when a round is cleared.
- `/api/wifi` stores optional station credentials in NVS.
- While Wi-Fi is enabled, the `Pixels-Dice` WPA2 access point remains active
  in AP+STA mode so a failed home-network configuration cannot make the device
  unreachable.
- The touchscreen build defaults Wi-Fi off and provides a persistent toggle on
  the pairing/settings page. Its header reports `OFF`, `AP`, `...`, or `NET`.
- The headless build always enables Wi-Fi because the web page is its only UI.
- BLE scanning uses a 95% duty cycle while touchscreen Wi-Fi is off and 50%
  while Wi-Fi and BLE share the radio.

Polling was selected over persistent WebSocket connections to keep socket,
heap, and implementation overhead predictable on the ESP32-C6 without PSRAM.
At a 750 ms interval the payload and CPU cost are small, while updates still
feel immediate for dice rolls.

## Persistence

Paired IDs, aggregate mode, touchscreen Wi-Fi enablement, and Wi-Fi credentials
survive reboot. Overall and per-die roll histories do not: persisting every
roll would create unnecessary flash wear and would require defining rollover
and retention behavior.

## Constraints

- Wi-Fi and BLE share the 2.4 GHz radio. Normal use is expected to work, but an
  eight-die soak test is required to quantify missed advertisements.
- The page is HTTP-only and has no user authentication. Use it on a trusted
  local network and do not expose it through router port forwarding.
- The classic ESP32 build disables Wi-Fi IRAM optimizations and uses size
  optimization to preserve instruction RAM. This trades maximum network
  throughput for additional firmware headroom, which is appropriate for the
  small dashboard responses.
