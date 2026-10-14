#!/bin/sh
# Build and run the pure-logic tests with a plain g++: no Hyprland, no CMake, a few seconds.
#   tests/run.sh             (CXX=clang++ tests/run.sh works too)
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=$(mktemp -d "${TMPDIR:-/tmp}/glowbuffer-tests.XXXXXX")
trap 'rm -rf "$OUT"' EXIT

${CXX:-g++} -std=c++23 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -I"$ROOT/src" \
    "$ROOT/tests/unit.cpp" "$ROOT/src/UriList.cpp" "$ROOT/src/Sandbox.cpp" "$ROOT/src/PasteImage.cpp" "$ROOT/src/ClipboardPlan.cpp" -o "$OUT/unit"
"$OUT/unit"
