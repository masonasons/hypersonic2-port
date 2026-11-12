#!/usr/bin/env bash
# Copies the built tool to an Ableton Move running Schwung over SSH (MOVE_HOST, default ableton@move.local), into
# modules/tools/hs2-midi-player. Leaves settings.json and an existing content/ folder alone.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(dirname "$HERE")"
MOVE_HOST="${MOVE_HOST:-ableton@move.local}"
TAR="$ROOT/dist/hs2-midi-player-module.tar.gz"
[ -f "$TAR" ] || "$HERE/build.sh"
scp -o ConnectTimeout=8 "$TAR" "$MOVE_HOST:/tmp/hs2-midi-player-module.tar.gz"
ssh -o ConnectTimeout=8 "$MOVE_HOST" '
  set -e
  BASE=/data/UserData/schwung
  [ -d "$BASE/modules" ] || BASE=/data/UserData/move-anything
  mkdir -p "$BASE/modules/tools"
  tar -xzf /tmp/hs2-midi-player-module.tar.gz -C "$BASE/modules/tools/"
  rm -f /tmp/hs2-midi-player-module.tar.gz
  echo "Installed to $BASE/modules/tools/hs2-midi-player"
  if [ -d "$BASE/modules/sound_generators/hypersonic2/content/Patches" ] || ls -d "$BASE"/modules/sound_generators/hypersonic2/content/*/Patches >/dev/null 2>&1; then
    echo "Content: the Hypersonic 2 module'"'"'s content folder"
  else
    echo "Content: none found yet. Copy your Hypersonic 2 content into $BASE/modules/tools/hs2-midi-player/content/"
  fi
'
echo "Restart Schwung, then open Tools > HS2 MIDI Player. Put MIDI files under /data/UserData/UserLibrary."
