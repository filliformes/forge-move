#!/usr/bin/env bash
# Forge — deploy to Move via SCP
set -e

MODULE_ID="forge"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
MOVE_HOST="${MOVE_HOST:-ableton@move.local}"
DEST="/data/UserData/schwung/modules/sound_generators/$MODULE_ID"

if [ ! -f "$ROOT/dist/$MODULE_ID/dsp.so" ]; then
    echo "ERROR: $ROOT/dist/$MODULE_ID/dsp.so not found. Run scripts/build.sh first."
    exit 1
fi

echo "==> Installing $MODULE_ID to $MOVE_HOST:$DEST"

ssh "$MOVE_HOST" "mkdir -p $DEST"
scp "$ROOT/dist/$MODULE_ID/dsp.so"     "$MOVE_HOST:$DEST/"
scp "$ROOT/dist/$MODULE_ID/module.json" "$MOVE_HOST:$DEST/"
[ -f "$ROOT/dist/$MODULE_ID/movy_config.json" ] && \
    scp "$ROOT/dist/$MODULE_ID/movy_config.json" "$MOVE_HOST:$DEST/"
ssh "$MOVE_HOST" "chmod +x $DEST/dsp.so && chown -R ableton:users $DEST 2>/dev/null || true"

echo "==> Done."
echo "    Power-cycle the Move (or remove + re-add the module from FX slot)"
echo "    to load the new module.json."
