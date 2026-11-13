#!/bin/sh
# Builds hs2capture and the mock plugin for this computer and checks a capture run (no Windows, no Hypersonic 2).
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
B="$ROOT/build/hs2capture-test"
rm -rf "$B"; mkdir -p "$B"
CXX=${CXX:-c++}
$CXX -std=c++17 -O1 -g -Wall -Wextra ${EXTRA_CXXFLAGS:-} -o "$B/hs2capture" "$HERE/../hs2capture.cpp" ${EXTRA_LDFLAGS:-}
$CXX -std=c++17 -O1 -g -Wall -Wextra -shared -fPIC ${EXTRA_CXXFLAGS:-} -o "$B/mock.so" "$HERE/mock_plugin.cpp" ${EXTRA_LDFLAGS:-}
printf '# test list\n8\t30\tElectric Piano 2\n15\t0\tSlash/Name: "x"\n99\t1\tRefused\n8\t5\tSilent\n' > "$B/list.tsv"
run() { "$B/hs2capture" --plugin "$B/mock.so" --list "$B/list.tsv" --out "$B/out" --speed 0 --settle 1 \
          --notes 60,72 --vels 40,100 --hold 0.2 --tail 1 "$@"; }
set +e
run > "$B/run1.log" 2>&1; echo "exit $?" >> "$B/run1.log"
run > "$B/run2.log" 2>&1; echo "exit $?" >> "$B/run2.log"
set -e
python3 -I "$HERE/check.py" "$B"
