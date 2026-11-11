#!/usr/bin/env bash
# Builds the Schwung module for Ableton Move (ARM64 Linux): dist/hypersonic2/ and dist/hypersonic2-module.tar.gz.
#
# Uses an aarch64-linux-gnu-g++ cross compiler when one is on PATH (Homebrew: brew install
# messense/macos-cross-toolchains/aarch64-unknown-linux-gnu), else Docker (schwung/Dockerfile).
# The C++ runtime is linked statically, so the module needs only the device's libc / libm.
#
#   schwung/build.sh            build
#   schwung/build.sh --native   build a desktop shared library for the host test (schwung/tests/run_host_test.sh)
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(dirname "$HERE")"
MODULE_ID=hypersonic2
NATIVE=0
[ "${1:-}" = "--native" ] && NATIVE=1

if [ "$NATIVE" = 0 ] && [ -z "${CROSS_PREFIX:-}" ] && ! command -v aarch64-linux-gnu-g++ >/dev/null 2>&1 && [ ! -f /.dockerenv ]; then
  echo "=== no aarch64 cross compiler on PATH: building in Docker ==="
  command -v docker >/dev/null 2>&1 || { echo "ERROR: needs aarch64-linux-gnu-g++ or Docker" >&2; exit 1; }
  docker build -t hs2-schwung-builder -f "$HERE/Dockerfile" "$HERE"
  docker run --rm -v "$ROOT:/build" -u "$(id -u):$(id -g)" -w /build -e CROSS_PREFIX=aarch64-linux-gnu- \
    hs2-schwung-builder ./schwung/build.sh
  exit 0
fi

cd "$ROOT"
python3 -I schwung/tools/gen_param_tables.py >/dev/null

if [ "$NATIVE" = 1 ]; then
  CXX="${CXX:-c++}"
  OUT=build/schwung-native${NATIVE_SUFFIX:-}
  OBJ=$OUT/obj
  LIB="$OUT/hs2_module.so"
  ARCHFLAGS=""
else
  CXX="${CROSS_PREFIX:-aarch64-linux-gnu-}g++"
  OBJ=build/schwung/obj
  OUT=dist/$MODULE_ID
  LIB="$OUT/dsp.so"
  ARCHFLAGS="-mcpu=cortex-a72"
fi
mkdir -p "$OBJ" "$OUT"

CXXFLAGS="-std=c++20 -O2 -fPIC -ffp-contract=off -fvisibility=hidden -ffunction-sections -fdata-sections \
  -Wall -Wextra -Wno-unused-parameter -Icore/include -Icore/src -Iplugin/Source -Ischwung/src $ARCHFLAGS ${EXTRA_CXXFLAGS:-}"

SOURCES=$(ls core/src/*.cpp core/src/fx/*.cpp core/src/types/*.cpp plugin/Source/PatchLibrary.cpp schwung/src/hs2_module.cpp)
JOBS=$( (command -v nproc >/dev/null && nproc) || sysctl -n hw.ncpu 2>/dev/null || echo 4)

echo "Compiling $(echo "$SOURCES" | wc -l | tr -d ' ') files with $CXX ($JOBS jobs)..."
export CXX CXXFLAGS OBJ
echo "$SOURCES" | xargs -P "$JOBS" -n 1 sh -c '
  src="$0"; obj="$OBJ/$(echo "$src" | tr / _).o"
  if [ ! -f "$obj" ] || [ "$src" -nt "$obj" ] || [ "$src" = schwung/src/hs2_module.cpp ]; then
    $CXX $CXXFLAGS -c "$src" -o "$obj" || exit 255
  fi'

OBJS=$(for s in $SOURCES; do echo "$OBJ/$(echo "$s" | tr / _).o"; done)
if [ "$NATIVE" = 1 ]; then
  if [ "$(uname)" = Darwin ]; then
    $CXX -shared -o "$LIB" $OBJS ${EXTRA_CXXFLAGS:-} -Wl,-dead_strip
  else
    $CXX -shared -o "$LIB" $OBJS -pthread -Wl,--gc-sections
  fi
  echo "Built $LIB"
  exit 0
fi

$CXX -shared -o "$LIB" $OBJS -static-libstdc++ -static-libgcc -pthread -lm -Wl,--gc-sections -Wl,--as-needed -s

# The device's glibc: Move runs Schwung modules built against glibc 2.17..2.34 (the bundled modules need up to 2.34).
OBJDUMP="${CROSS_PREFIX:-aarch64-linux-gnu-}objdump"
if command -v "$OBJDUMP" >/dev/null 2>&1; then
  NEEDED=$("$OBJDUMP" -p "$LIB" | awk '/NEEDED/ {print $2}' | tr '\n' ' ')
  MAXGLIBC=$("$OBJDUMP" -T "$LIB" | grep -oE 'GLIBC_[0-9.]+' | sort -uV | tail -1)
  echo "dsp.so needs: $NEEDED(newest symbol version $MAXGLIBC)"
  case "$NEEDED" in *libstdc++*) echo "ERROR: libstdc++ is linked dynamically" >&2; exit 1 ;; esac
fi

cp schwung/module.json schwung/ui.js schwung/help.json "$OUT/"
mkdir -p "$OUT/content"
cat > "$OUT/content/README.txt" <<'TXT'
Copy the contents of your Hypersonic 2 content folder here, so that these folders exist:
  content/Patches  content/Samples  content/Wavetables  content/Drawbars  content/Effects  content/Tunings
Nothing from Steinberg is included with this module. Then choose "Rescan Content" in the module's menu
(or reload the module).
TXT

tar -czf "dist/$MODULE_ID-module.tar.gz" -C dist "$MODULE_ID"
SIZE=$(du -h "$LIB" | cut -f1)
echo "Built $LIB ($SIZE) and dist/$MODULE_ID-module.tar.gz"
