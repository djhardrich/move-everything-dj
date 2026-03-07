#!/bin/bash
# Install DJ Deck module to Move
# Copyright (c) 2026 DJ Hard Rich
# Licensed under CC BY-NC-SA 4.0
#
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
MODULE_ID="dj"

cd "$REPO_ROOT"

if [ ! -d "dist/$MODULE_ID" ]; then
    echo "Error: dist/$MODULE_ID not found. Run ./scripts/build.sh first."
    exit 1
fi

echo "=== Installing DJ Deck Module ==="

# Deploy to Move - overtake modules go to other/ subdirectory
echo "Copying module to Move..."
ssh ableton@move.local "mkdir -p /data/UserData/move-anything/modules/tools/$MODULE_ID"
scp -r dist/$MODULE_ID/* ableton@move.local:/data/UserData/move-anything/modules/tools/$MODULE_ID/

# Set permissions
echo "Setting permissions..."
ssh ableton@move.local "chmod -R a+rw /data/UserData/move-anything/modules/tools/$MODULE_ID"

echo ""
echo "=== Install Complete ==="
echo "Module installed to: /data/UserData/move-anything/modules/tools/$MODULE_ID/"
echo ""
echo "Restart Move Anything to load the new module."
