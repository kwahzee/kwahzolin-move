#!/usr/bin/env bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
MODULE_ID="kwahzolin"
DEVICE="${MOVE_HOST:-move.local}"
REMOTE_BASE="/data/UserData/schwung/modules/sound_generators"
REMOTE_DIR="$REMOTE_BASE/$MODULE_ID"

cd "$REPO_ROOT"

if [ ! -d "dist/$MODULE_ID" ]; then
    echo "Error: dist/$MODULE_ID not found — run ./scripts/build.sh first"
    exit 1
fi

echo "Installing $MODULE_ID → ableton@${DEVICE}:${REMOTE_DIR}"
ssh ableton@"$DEVICE" "mkdir -p '$REMOTE_DIR'"
scp -r "dist/$MODULE_ID/." "ableton@${DEVICE}:${REMOTE_DIR}/"

echo "Done. Restart Schwung on the device to pick up the new module."
