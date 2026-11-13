#!/bin/sh
# Builds hs2capture.exe, a 32-bit Windows program (the original Hypersonic 2 is a 32-bit plugin), with MinGW-w64:
#   macOS: brew install mingw-w64     Linux: apt install g++-mingw-w64-i686
# On Windows without MinGW, see README.md (Visual Studio "x86 Native Tools" command line).
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
OUT="$ROOT/dist/hs2capture"
mkdir -p "$OUT"
${CXX:-i686-w64-mingw32-g++} -std=c++17 -O2 -Wall -Wextra -static -s -o "$OUT/hs2capture.exe" \
    "$HERE/hs2capture.cpp" -lole32 -luser32
cp "$HERE/README.md" "$HERE/protected_patches.tsv" "$OUT/"
(cd "$ROOT/dist" && rm -f hs2capture.zip && zip -qr hs2capture.zip hs2capture)
echo "Built $OUT/hs2capture.exe and dist/hs2capture.zip"
