#!/usr/bin/env bash
# Remove build artifacts
# Copyright (c) 2026 DJ Hard Rich
# Licensed under CC BY-NC-SA 4.0
#
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"

echo "Cleaning build artifacts..."
rm -rf "$REPO_ROOT/build" "$REPO_ROOT/dist"

echo "Cleaning dependencies..."
rm -rf "$REPO_ROOT/src/dsp/bungee" "$REPO_ROOT/src/dsp/libxmp" \
       "$REPO_ROOT/src/dsp/minimp3" "$REPO_ROOT/src/dsp/fdk-aac"

echo "Done."
echo "Run ./scripts/build.sh to fetch dependencies and rebuild."
