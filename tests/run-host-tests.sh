#!/usr/bin/env sh
set -eu

root="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
protocol_output="$root/tests/.pixels-protocol-tests"
calculator_output="$root/tests/.dice-calculator-tests"
trap 'rm -f "$protocol_output" "$calculator_output"' EXIT

c++ -std=c++17 -Wall -Wextra -Werror \
  -I"$root/components/pixels_protocol/include" \
  "$root/components/pixels_protocol/pixels_protocol.cpp" \
  "$root/tests/test_pixels_protocol.cpp" \
  -o "$protocol_output"

"$protocol_output"

c++ -std=c++17 -Wall -Wextra -Werror \
  -I"$root/components/pixels_protocol/include" \
  -I"$root/main" \
  "$root/main/dice_calculator.cpp" \
  "$root/components/pixels_protocol/pixels_protocol.cpp" \
  "$root/tests/test_dice_calculator.cpp" \
  -o "$calculator_output"

"$calculator_output"
