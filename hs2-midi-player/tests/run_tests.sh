#!/usr/bin/env bash
# Desktop tests of HS2 MIDI Player (no Move needed): builds the module for this machine, writes the test content and
# MIDI files (make_test_files.py) and runs tests/host_test.cpp against them. HS2_VERBOSE=1 shows the module's log.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
cd "$ROOT"
hs2-midi-player/build.sh --native
OUT=build/hs2mp-native${NATIVE_SUFFIX:-}
rm -rf "$OUT/test-files" "$OUT/module"
mkdir -p "$OUT/module"
python3 -I hs2-midi-player/tests/make_test_files.py "$OUT/test-files" >/dev/null
"${CXX:-c++}" -std=c++20 -O1 -g ${EXTRA_CXXFLAGS:-} -Ischwung/src hs2-midi-player/tests/host_test.cpp -o "$OUT/host_test" -ldl -pthread
"$OUT/host_test" "$ROOT/$OUT/hs2mp.so" "$ROOT/$OUT/test-files" "$ROOT/$OUT/module"

# The UI against the DSP (needs node and Schwung's shared UI libraries: SCHWUNG_SHARED, or ~/schwung.tar.gz).
if command -v node >/dev/null 2>&1; then
  SHARED="${SCHWUNG_SHARED:-}"
  SH=build/schwung-native/schwung-shared
  if [ -z "$SHARED" ] && [ -f "$SH/schwung/shared/menu_layout.mjs" ]; then
    SHARED="$ROOT/$SH/schwung/shared"
  elif [ -z "$SHARED" ] && [ -f "$HOME/schwung.tar.gz" ]; then
    mkdir -p "$SH"
    tar -xzf "$HOME/schwung.tar.gz" -C "$SH" schwung/shared
    SHARED="$ROOT/$SH/schwung/shared"
  fi
  if [ -n "$SHARED" ]; then
    "${CXX:-c++}" -std=c++20 -O1 -g -Ischwung/src schwung/tests/dsp_bridge.cpp -o "$OUT/dsp_bridge" -ldl -pthread
    node hs2-midi-player/tests/ui_test.mjs "$ROOT/$OUT/hs2mp.so" "$ROOT/$OUT/test-files/content" "$SHARED" \
      "$ROOT/$OUT/dsp_bridge" "$ROOT/$OUT/ui-work" "$ROOT/hs2-midi-player/ui.js" "$ROOT/$OUT/test-files/midi"
  else
    echo "UI test skipped: set SCHWUNG_SHARED to Schwung's shared/ folder"
  fi
fi
