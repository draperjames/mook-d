#!/bin/bash
# Deploy Mook D to a Move over scp (sound-generator module path).
# SSH user is `ableton` (modules are owned ableton:users), NOT root.
set -e
MODULE_ID="mook-d"
MOVE_HOST="${MOVE_HOST:-move.local}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$SCRIPT_DIR/.."
DEST="/data/UserData/schwung/modules/sound_generators/$MODULE_ID"

if [ ! -f "$ROOT/dist/$MODULE_ID/dsp.so" ]; then
    echo "dsp.so not found. Run scripts/build.sh first."
    exit 1
fi

echo "Installing $MODULE_ID to ableton@$MOVE_HOST..."
ssh "ableton@$MOVE_HOST" "mkdir -p $DEST"
HELP=""; [ -f "$ROOT/dist/$MODULE_ID/help.json" ] && HELP="$ROOT/dist/$MODULE_ID/help.json"
scp "$ROOT/dist/$MODULE_ID/dsp.so" "$ROOT/dist/$MODULE_ID/module.json" $HELP "ableton@$MOVE_HOST:$DEST/"
ssh "ableton@$MOVE_HOST" "chown -R ableton:users $DEST && chmod 755 $DEST/dsp.so"
echo "Installed to $DEST"
echo "Reload: remove + re-add the module in a slot, or power-cycle the Move."
