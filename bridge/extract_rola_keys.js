/*
 * One-time Frida script to capture your EBO SE Kalay session credentials and ioctl 0x9930 blob
 * from the Android ROLA app (com.enabot.ebo.intl) on an emulator or test device.
 *
 * Usage:
 *   frida -U -f com.enabot.ebo.intl -l bridge/extract_rola_keys.js
 */
"use strict";

const hooked = new Set();

function findGlobalSymbol(name) {
  for (const modName of ["libTUTKGlobalAPIs.so", "libIOTCAPIs.so", "libAVAPIs.so", "libRDTAPIs.so"]) {
    const mod = Process.findModuleByName(modName);
    if (mod) {
      const addr = mod.findExportByName(name);
      if (addr) return addr;
    }
  }
  if (typeof Module.findGlobalExportByName === "function") {
    const addr = Module.findGlobalExportByName(name);
    if (addr) return addr;
  }
  if (typeof Module.findExportByName === "function") {
    return Module.findExportByName(null, name);
  }
  return null;
}

function tryHook(name, callbacks) {
  if (hooked.has(name)) return true;
  const addr = findGlobalSymbol(name);
  if (!addr) return false;
  hooked.add(name);
  Interceptor.attach(addr, callbacks);
  console.log("[ori-keys] attached:", name, "at", addr);
  return true;
}

function hexBytes(ptr, len) {
  const bytes = new Uint8Array(ptr.readByteArray(len));
  return Array.from(bytes, b => b.toString(16).padStart(2, "0")).join("");
}

function emitEnv(key, value) {
  console.log(key + "=" + value);
  send({ type: "env", key, value });
}

function attachTutkHooks() {
  tryHook("TUTK_SDK_Set_License_Key", {
    onEnter(args) {
      if (!args[0].isNull()) emitEnv("EBO_LICENSE", args[0].readUtf8String());
    },
  });

  tryHook("IOTC_Connect_ByUIDEx", {
    onEnter(args) {
      if (!args[0].isNull()) emitEnv("EBO_UID", args[0].readUtf8String());
      if (!args[2].isNull()) emitEnv("EBO_AUTHKEY", args[2].add(8).readUtf8String());
    },
  });

  tryHook("avClientStartEx", {
    onEnter(args) {
      const inc = args[0];
      if (inc.isNull()) return;
      const ptrSize = Process.pointerSize;
      const identityPtr = inc.add(16).readPointer();
      const tokenPtr = inc.add(16 + ptrSize).readPointer();
      if (!identityPtr.isNull()) emitEnv("EBO_IDENTITY", identityPtr.readUtf8String());
      if (!tokenPtr.isNull()) emitEnv("EBO_TOKEN", tokenPtr.readUtf8String());
    },
  });

  tryHook("avSendIOCtrl", {
    onEnter(args) {
      const ioType = args[1].toInt32() & 0xFFFF;
      if (ioType === 0x9930) {
        const len = args[3].toInt32();
        if (len > 0 && len < 4096 && !args[2].isNull()) {
          const hex = hexBytes(args[2], len);
          console.log("IOCTL_9930_HEX=" + hex);
          send({ type: "ioctl9930", hex });
        }
      }
    },
  });
}

attachTutkHooks();
setInterval(attachTutkHooks, 150);
