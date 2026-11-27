#!/usr/bin/env python3
"""Automate extracting ROLA arm64 TUTK .so libraries, .env credentials, and ioctl9930.bin from the Mac emulator."""

import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import threading
import time

ROOT = Path(__file__).resolve().parents[1]
ADB = ROOT / "private" / "android-sdk" / "platform-tools" / "adb"
VENDOR_DIR = ROOT / "private" / "vendor"
LIB_DIR = VENDOR_DIR / "lib"
ENV_FILE = ROOT / ".env"
FRIDA_SCRIPT = ROOT / "bridge" / "extract_rola_keys.js"
PACKAGES = ("com.enabot.ebo.intl", "com.enabot.ebox.intl", "com.enabot.rola")
REQUIRED_LIBS = ("libTUTKGlobalAPIs.so", "libIOTCAPIs.so", "libRDTAPIs.so", "libAVAPIs.so")
REQUIRED_KEYS = ("EBO_LICENSE", "EBO_UID", "EBO_AUTHKEY", "EBO_IDENTITY", "EBO_TOKEN")


def adb(*args, check=True, capture=True):
    return subprocess.run(
        [str(ADB), *args],
        check=check,
        text=True,
        stdout=subprocess.PIPE if capture else None,
        stderr=subprocess.PIPE if capture else None,
    )


def find_installed_package():
    for pkg in PACKAGES:
        res = adb("shell", "pm", "path", pkg, check=False)
        paths = [
            line.removeprefix("package:").strip()
            for line in res.stdout.splitlines()
            if line.startswith("package:")
        ]
        if paths:
            return pkg, paths
    return None, []


def extract_so_libraries(apk_paths):
    LIB_DIR.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        tmp_dir = Path(tmp)
        pulled = []
        for idx, remote in enumerate(apk_paths):
            local = tmp_dir / f"part_{idx}_{Path(remote).name}"
            adb("pull", remote, str(local))
            pulled.append(local)
        for apk in pulled:
            subprocess.run(
                ["unzip", "-q", "-o", str(apk), "lib/arm64-v8a/*", "-d", str(tmp_dir)],
                stderr=subprocess.DEVNULL,
            )
        abi_dir = tmp_dir / "lib" / "arm64-v8a"
        if not abi_dir.is_dir():
            raise RuntimeError("No lib/arm64-v8a/ found in installed ROLA APKs")
        for lib in REQUIRED_LIBS:
            src = abi_dir / lib
            if not src.is_file():
                raise RuntimeError(f"Missing {lib} in {abi_dir}")
            dst = LIB_DIR / lib
            shutil.copy2(src, dst)
            os.chmod(dst, 0o755)
    print(f"Extracted 4 arm64-v8a TUTK libraries to {LIB_DIR}")


def write_env(values):
    existing = {}
    if ENV_FILE.is_file():
        for line in ENV_FILE.read_text().splitlines():
            if "=" in line and not line.lstrip().startswith("#"):
                k, v = line.split("=", 1)
                existing[k.strip()] = v.strip()
    existing.update(values)
    ordered = list(REQUIRED_KEYS) + [k for k in existing if k not in REQUIRED_KEYS]
    ENV_FILE.write_text("".join(f"{k}={existing.get(k, '')}\n" for k in ordered))


def ensure_frida_server():
    adb("root")
    adb("wait-for-device")
    ps = adb("shell", "pidof", "frida-server", check=False)
    if ps.returncode == 0 and ps.stdout.strip():
        return
    frida_bin = ROOT / "private" / "android-sdk" / "frida-server"
    if frida_bin.is_file():
        adb("push", str(frida_bin), "/data/local/tmp/frida-server")
        adb("shell", "chmod", "755", "/data/local/tmp/frida-server")
    subprocess.Popen(
        [str(ADB), "shell", "/data/local/tmp/frida-server -D"],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    time.sleep(1.5)


def main():
    import frida

    if not ADB.is_file():
        raise SystemExit(f"Missing adb at {ADB}")
    adb("wait-for-device")
    pkg, apk_paths = find_installed_package()
    if not pkg:
        raise SystemExit("ROLA is not installed in the emulator yet. Install it via Aurora Store first.")
    print(f"Found {pkg} ({len(apk_paths)} APK file(s))")
    extract_so_libraries(apk_paths)

    ensure_frida_server()
    device = frida.get_usb_device(timeout=10)
    captured = {}
    got_ioctl = False
    done = threading.Event()

    def on_message(message, _data):
        nonlocal got_ioctl
        if message.get("type") == "error":
            print("Frida error:", message.get("stack") or message.get("description"))
            return
        if message.get("type") != "send":
            return
        payload = message.get("payload", {})
        if payload.get("type") == "env":
            key = payload["key"]
            val = payload["value"]
            if key in REQUIRED_KEYS and val:
                if captured.get(key) != val:
                    captured[key] = val
                    write_env(captured)
                    print(f"Captured {key}")
        elif payload.get("type") == "ioctl9930":
            raw = bytes.fromhex(payload["hex"])
            VENDOR_DIR.mkdir(parents=True, exist_ok=True)
            (VENDOR_DIR / "ioctl9930.bin").write_bytes(raw)
            got_ioctl = True
            print(f"Captured private/vendor/ioctl9930.bin ({len(raw)} bytes)")
        if all(k in captured for k in REQUIRED_KEYS) and got_ioctl:
            done.set()

    # Respawn via am start (avoids Android 14 zygote spawn crashes) and attach immediately.
    respawn = "--respawn" in sys.argv[1:]
    running = [p for p in device.enumerate_processes() if p.name in {pkg, "ROLA"}]
    if respawn or not running:
        adb("shell", "am", "force-stop", pkg, check=False)
        adb("shell", "am", "start", "-n", f"{pkg}/com.ebo.ebocode.login.SplashActivity")
        pid = None
        for _ in range(50):
            found = [p for p in device.enumerate_processes() if p.name in {pkg, "ROLA"}]
            if found:
                pid = found[0].pid
                break
            time.sleep(0.1)
        if pid is None:
            raise SystemExit("ROLA failed to start")
    else:
        pid = running[0].pid
    session = device.attach(pid)

    script = session.create_script(FRIDA_SCRIPT.read_text())
    script.on("message", on_message)
    script.load()

    print("Sign into ROLA in the emulator window and open Ori's live view once...")
    print("Waiting for EBO_LICENSE, EBO_UID, EBO_AUTHKEY, EBO_IDENTITY, EBO_TOKEN, and ioctl9930...")
    while not done.wait(timeout=1.0):
        pass

    print("All 5 ROLA credentials + ioctl9930.bin + 4 TUTK .so libraries saved!")
    session.detach()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
