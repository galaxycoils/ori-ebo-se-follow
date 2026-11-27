# Direct Kalay Bridge Setup (macOS + iPhone ROLA App)

This guide connects Ori (`EBO SE`) directly to `ori` on your Mac mini without an Android phone, Home Assistant, MediaMTX, FastAPI, or MQTT.

## 1. Machine Prerequisites (Already Installed & Built)

- `ffmpeg`, `colima`, and `docker` are installed via Homebrew.
- The 64-bit Android `bionic` runtime container (`ori-kalay-shim:latest`) is pre-built with `ori_kalay_shim` compiled directly against Android `bionic`—no manual `vendor/bionic/` extraction is needed.
- An isolated, temporary ARM64 Android Emulator (`ori_extract`) with `frida-server` and **Aurora Store** lives under git-ignored `private/android-sdk/` so you can pull your ROLA keys & `.so` files directly on macOS and delete the emulator afterward with `./bridge/cleanup_emulator.sh`.

## 2. Capture Your ROLA `.so` Libraries & Session Credentials (One-Time)

1. Close the **ROLA** app on your iPhone (Ori accepts one Kalay client at a time).
2. In the Mac Android emulator window (`Aurora Store`), install **ROLA** (`com.enabot.ebo.intl`).
3. Run the automated capture script on your Mac, then sign into ROLA in the emulator and tap **Ori** once:
   ```sh
   private/venv/bin/python bridge/capture_from_emulator.py
   ```
   This automatically extracts the 4 `arm64-v8a` TUTK `.so` files to `private/vendor/lib/`, writes `private/vendor/ioctl9930.bin`, and saves your 5 keys to `.env`.
4. Clean up the temporary Android emulator & SDK (~2 GB reclaimed) while keeping `.env` and `private/vendor/`:
   ```sh
   ./bridge/cleanup_emulator.sh
   ```

*(If you already have a ROLA `.apk`/`.xapk` file on disk instead, run `./bridge/unpack_apk.sh <path-to-rola.apk>`.)*

## 3. Mandatory Raised-Wheel Cradle Deadman Check (Before Enabling Follow)

Before ever running Follow on the floor, place Ori in a **stable raised cradle** with its wheels completely off the ground, close the ROLA app on your iPhone/emulator, and verify whether MCU `v0.0.35` cuts wheel power automatically when packets stop:

```sh
make all
python3 - <<'PY'
import os, subprocess
r, w = os.pipe()
shim = subprocess.Popen(["./bridge/run_shim.sh"], stdin=r, stdout=subprocess.DEVNULL)
os.close(r)
probe = subprocess.run(["./build/ori-bridge", "--control-fd", str(w), "--cradle-test"], pass_fds=(w,))
os.close(w)
shim.terminate()
raise SystemExit(probe.returncode)
PY
```

- **Pass**: Wheels turn briefly on the single packet and stop automatically within `~150–400 ms` with zero stop packet sent.
- **Fail**: Keep Ori in `Track only` mode (omit `--enable-follow`) until an on-device watchdog is established.

## 4. Run Live Tracking, Patrol, and Physical Follow

- Start Live Ori (rebuilds `ori-kalay-shim:latest` with the 300 ms RDT wheel-cutoff watchdog and enables Follow permission):
  ```sh
  make live
  ```
- **Drive readiness & wheel cutoff**: `ori_kalay_shim` enforces a 300 ms RDT motor deadman timer and emits `0xFD` link/cutoff heartbeats; `ori-bridge` adds a 260 ms `poll()` cutoff guard and arms EBO SE's onboard collision avoidance and fall arrest on connect. Live battery/dock telemetry (`msgId = 207`) + active RDT automatically resolves drive readiness off the dock so **Physical Follow** and **Auto Patrol** unlock cleanly.
- Use **Diagnostics → Verify wheel cutoff** in a raised cradle to test a 120 ms drive pulse and automatic stop, and use **Allow Physical Follow** under **Manual drive** to lock or unlock Follow at any time.

Close the server (`Ctrl-C`) at any time to immediately cut motors and release Ori's Kalay session back to your iPhone ROLA app.
