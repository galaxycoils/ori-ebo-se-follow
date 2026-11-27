#!/bin/sh
set -eu

if [ $# -lt 1 ]; then
  echo "Usage: $0 <path-to-rola.apk-or-xapk>" >&2
  exit 2
fi

SRC="$1"
if [ ! -f "$SRC" ]; then
  echo "File not found: $SRC" >&2
  exit 1
fi

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
DEST="$ROOT_DIR/private/vendor/lib"
mkdir -p "$DEST"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

unzip -q -o "$SRC" -d "$TMP"

# Handle split APKs (.xapk / .apkm / .zip) containing config.arm64_v8a.apk or base.apk.
for sub in "$TMP"/*.apk; do
  [ -f "$sub" ] && unzip -q -o "$sub" "lib/arm64-v8a/*" -d "$TMP" 2>/dev/null || true
done

ABI_DIR="$TMP/lib/arm64-v8a"
if [ ! -d "$ABI_DIR" ]; then
  echo "No lib/arm64-v8a/ found in $SRC. Ensure you have the arm64-v8a APK." >&2
  exit 1
fi

REQUIRED="libTUTKGlobalAPIs.so libIOTCAPIs.so libRDTAPIs.so libAVAPIs.so"
for lib in $REQUIRED; do
  if [ ! -f "$ABI_DIR/$lib" ]; then
    echo "Missing $lib in $ABI_DIR" >&2
    exit 1
  fi
  cp "$ABI_DIR/$lib" "$DEST/$lib"
  chmod 755 "$DEST/$lib"
  file "$DEST/$lib"
done

echo "Extracted 64-bit TUTK libraries to $DEST"
