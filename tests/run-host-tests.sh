#!/usr/bin/env sh
set -eu

root="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
output="${TMPDIR:-/tmp}/pixels-protocol-tests"

c++ -std=c++17 -Wall -Wextra -Werror \
  -I"$root/components/pixels_protocol/include" \
  "$root/components/pixels_protocol/pixels_protocol.cpp" \
  "$root/tests/test_pixels_protocol.cpp" \
  -o "$output"

"$output"
rm -f "$output"
