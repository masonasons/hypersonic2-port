#!/usr/bin/env bash
# Copies the built module to an Ableton Move running Schwung over SSH (MOVE_HOST, default ableton@move.local).
# Leaves an existing content/ folder alone. Copy the Hypersonic 2 content once with:
#   scp -r "<your Hypersonic 2 Content>/"* "$MOVE_HOST:<module dir>/content/"
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(dirname "$HERE")"
MOVE_HOST="${MOVE_HOST:-ableton@move.local}"
TAR="$ROOT/dist/hypersonic2-module.tar.gz"
[ -f "$TAR" ] || "$HERE/build.sh"
scp -o ConnectTimeout=8 "$TAR" "$MOVE_HOST:/tmp/hypersonic2-module.tar.gz"
ssh -o ConnectTimeout=8 "$MOVE_HOST" '
  set -e
  BASE=/data/UserData/schwung
  [ -d "$BASE/modules" ] || BASE=/data/UserData/move-anything
  mkdir -p "$BASE/modules/sound_generators"
  tar -xzf /tmp/hypersonic2-module.tar.gz -C "$BASE/modules/sound_generators/"
  rm -f /tmp/hypersonic2-module.tar.gz
  echo "Installed to $BASE/modules/sound_generators/hypersonic2"
'
echo "Restart Schwung, then load Hypersonic 2 into a Signal Chain slot (Recv Ch All, Fwd Ch Thru)."
