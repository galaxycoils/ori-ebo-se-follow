#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
ADB="$ROOT_DIR/private/android-sdk/platform-tools/adb"

if [ -x "$ADB" ]; then
  "$ADB" emu kill >/dev/null 2>&1 || true
fi
pkill -f "ori_extract" >/dev/null 2>&1 || true

rm -rf "$ROOT_DIR/private/android-sdk" "$ROOT_DIR/private/venv"
rm -rf "$HOME/.android/avd/ori_extract.avd" "$HOME/.android/avd/ori_extract.ini"

echo "Removed temporary Android SDK, emulator AVD, and Frida venv. Kept .env and private/vendor/."
