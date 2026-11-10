#!/usr/bin/env bash
# Desktop test of the Schwung module (no Move needed): builds the module for this machine, writes a synthetic
# content folder and runs schwung/tests/host_test.cpp against it. HS2_VERBOSE=1 lists every menu address.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
cd "$ROOT"
schwung/build.sh --native
OUT=build/schwung-native
python3 -I schwung/tests/make_test_content.py "$OUT/test-content" >/dev/null
"${CXX:-c++}" -std=c++20 -O1 -g -Ischwung/src schwung/tests/host_test.cpp -o "$OUT/host_test" -ldl -pthread
"$OUT/host_test" "$ROOT/$OUT/hs2_module.so" "$ROOT/$OUT/test-content"

# The UI against the DSP (needs node and Schwung's shared UI libraries: SCHWUNG_SHARED, or ~/schwung.tar.gz).
if command -v node >/dev/null 2>&1; then
  SHARED="${SCHWUNG_SHARED:-}"
  if [ -z "$SHARED" ] && [ -f "$OUT/schwung-shared/schwung/shared/menu_layout.mjs" ]; then
    SHARED="$ROOT/$OUT/schwung-shared/schwung/shared"
  elif [ -z "$SHARED" ] && [ -f "$HOME/schwung.tar.gz" ]; then
    mkdir -p "$OUT/schwung-shared"
    tar -xzf "$HOME/schwung.tar.gz" -C "$OUT/schwung-shared" schwung/shared
    SHARED="$ROOT/$OUT/schwung-shared/schwung/shared"
  fi
  if [ -n "$SHARED" ]; then
    "${CXX:-c++}" -std=c++20 -O1 -g -Ischwung/src schwung/tests/dsp_bridge.cpp -o "$OUT/dsp_bridge" -ldl -pthread
    node schwung/tests/ui_test.mjs "$ROOT/$OUT/hs2_module.so" "$ROOT/$OUT/test-content" "$SHARED" \
      "$ROOT/$OUT/dsp_bridge" "$ROOT/$OUT/ui-work" "$ROOT/schwung/ui.js"
  else
    echo "UI test skipped: set SCHWUNG_SHARED to Schwung's shared/ folder"
  fi
fi
