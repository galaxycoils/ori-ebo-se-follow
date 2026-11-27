#!/bin/sh
set -eu

export PATH="/opt/homebrew/bin:/usr/local/bin:$PATH"
ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
ENV_FILE="${ORI_ENV_FILE:-$ROOT_DIR/.env}"
VENDOR_DIR="${ORI_VENDOR_DIR:-$ROOT_DIR/private/vendor}"
IMAGE_NAME="${ORI_SHIM_IMAGE:-ori-kalay-shim:latest}"

if [ ! -f "$ENV_FILE" ]; then
  echo "Missing $ENV_FILE (see docs/bridge.md)" >&2
  exit 1
fi
for lib in libTUTKGlobalAPIs.so libIOTCAPIs.so libRDTAPIs.so libAVAPIs.so; do
  if [ ! -f "$VENDOR_DIR/lib/$lib" ]; then
    echo "Missing $VENDOR_DIR/lib/$lib (run ./bridge/unpack_apk.sh <rola.apk>)" >&2
    exit 1
  fi
done

RUNTIME=$(command -v docker || command -v podman || true)
if [ -z "$RUNTIME" ]; then
  echo "Install Docker / OrbStack / Colima / Podman to run the Linux bionic Kalay shim." >&2
  exit 1
fi

if ! "$RUNTIME" info >/dev/null 2>&1; then
  if command -v colima >/dev/null 2>&1; then
    colima start --cpu 2 --memory 1 --disk 10 >&2
  else
    echo "Container daemon is not running." >&2
    exit 1
  fi
fi

exec "$RUNTIME" run --rm -i --name ori-kalay-shim --platform=linux/arm64 \
  --env-file "$ENV_FILE" \
  -v "$VENDOR_DIR:/opt/ori/vendor:ro" \
  "$IMAGE_NAME"
