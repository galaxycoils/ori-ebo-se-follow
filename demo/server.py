"""Serve the desktop demo on loopback or authenticated LAN/HTTPS. All tracking runs in the native core."""

import argparse
from contextlib import ExitStack
from http.cookies import CookieError, SimpleCookie
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import math
import os
from pathlib import Path
import queue
import re
import secrets
import shlex
import shutil
import signal
import ssl
import struct
import subprocess
import threading
import time
from urllib.parse import parse_qs, urlsplit
import zlib

from client import BRIDGE_BINARY, ReplayClient
from fixtures import HEIGHT, MOVEMENT, REFERENCE, WIDTH, color_scene, scene

PAGE = Path(__file__).with_name("index.html")
APP_ASSETS = {"companion.js": "text/javascript", "sw.js": "text/javascript",
              "manifest.webmanifest": "application/manifest+json",
              "icon-192.png": "image/png", "icon-512.png": "image/png"}
ACTIONS = {"select", "teach", "follow", "heartbeat", "track", "stop", "move", "home", "lift",
           "resume", "forget", "spot", "policy", "manual", "motors", "camera", "patrol", "cutoff-test"}
FRAME_INTERVAL = 0.1
FRAME_BYTES = WIDTH * HEIGHT
FRAME_RGB_BYTES = WIDTH * HEIGHT * 3
LIVE_WIDTH = 480
LIVE_HEIGHT = 270
LIVE_RGB_BYTES = LIVE_WIDTH * LIVE_HEIGHT * 3
LOOPBACK_HOSTS = {"127.0.0.1", "localhost", "::1"}

BOOST_LUT = bytes(
    max(0, min(255, round(((i / 255.0) ** 0.62) * 242 + (i - 128) * 0.12 + 8)))
    for i in range(256)
)
R_LUMA_LUT = bytes((77 * i) >> 8 for i in range(256))
G_LUMA_LUT = bytes((150 * i) >> 8 for i in range(256))
B_LUMA_LUT = bytes((29 * i) >> 8 for i in range(256))
IR_R_LUT = bytes(max(0, min(255, round(i * 0.20))) for i in range(256))
IR_G_LUT = bytes(max(0, min(255, round(min(255, i * 1.10 + 10)))) for i in range(256))
IR_B_LUT = bytes(max(0, min(255, round(i * 0.34))) for i in range(256))


def rgb_to_gray(rgb):
    r = rgb[0::3].translate(R_LUMA_LUT)
    g = rgb[1::3].translate(G_LUMA_LUT)
    b = rgb[2::3].translate(B_LUMA_LUT)
    return bytes(x + y + z for x, y, z in zip(r, g, b))


def apply_emerald_tint(luma):
    out = bytearray(len(luma) * 3)
    out[0::3] = luma.translate(IR_R_LUT)
    out[1::3] = luma.translate(IR_G_LUT)
    out[2::3] = luma.translate(IR_B_LUT)
    return bytes(out)


def room_signature_rgb(raw_rgb, width, height):
    if not raw_rgb or width < 16 or height < 9 or len(raw_rgb) < width * height * 3:
        return None
    samples = []
    for gy in range(9):
        sy = (2 * gy + 1) * height // 18
        row_off = sy * width * 3
        for gx in range(16):
            sx = (2 * gx + 1) * width // 32
            p = row_off + sx * 3
            samples.append((77 * raw_rgb[p] + 150 * raw_rgb[p + 1] + 29 * raw_rgb[p + 2] + 128) >> 8)
    mean = sum(samples) / 144.0
    centered = [s - mean for s in samples]
    norm = math.sqrt(sum(c * c for c in centered))
    return [c / norm for c in centered] if norm >= 144.0 else None


def visual_shift_and_score(live_sig, target_sig):
    if not live_sig or not target_sig or len(live_sig) != 144 or len(target_sig) != 144:
        return (0, 0.0, 0.0)

    def corr(shift):
        gx0 = max(0, shift)
        gx1 = min(16, 16 + shift)
        n = 9 * (gx1 - gx0)
        if n <= 0:
            return 0.0
        vals_a = []
        vals_b = []
        for gy in range(9):
            row = gy * 16
            for gx in range(gx0, gx1):
                vals_a.append(live_sig[row + gx])
                vals_b.append(target_sig[row + gx - shift])
        ma = sum(vals_a) / n
        mb = sum(vals_b) / n
        da = [a - ma for a in vals_a]
        db = [b - mb for b in vals_b]
        na = sum(x * x for x in da)
        nb = sum(y * y for y in db)
        if na < 1e-6 or nb < 1e-6:
            return 0.0
        return sum(x * y for x, y in zip(da, db)) / math.sqrt(na * nb)

    zero_score = corr(0)
    best_shift = 0
    best_score = zero_score
    for s in range(-4, 5):
        if s == 0:
            continue
        sc = corr(s)
        if sc > best_score:
            best_score = sc
            best_shift = s
    return (best_shift, best_score, zero_score)


def find_ffmpeg():
    for candidate in (os.environ.get("FFMPEG"), shutil.which("ffmpeg")):
        if candidate and os.path.isfile(candidate) and os.access(candidate, os.X_OK):
            return candidate
    return None


def png(pixels, width=WIDTH, height=HEIGHT):
    def chunk(kind, data):
        return (struct.pack(">I", len(data)) + kind + data
                + struct.pack(">I", zlib.crc32(kind + data)))

    view = memoryview(pixels)
    if len(view) == width * height * 3:
        color_type = 2
        row_bytes = width * 3
    elif len(view) == width * height:
        color_type = 0
        row_bytes = width
    else:
        raise ValueError(f"Unexpected pixel buffer size {len(view)} for {width}x{height}")
    stride = row_bytes + 1
    rows = bytearray(height * stride)
    for row in range(height):
        src = row * row_bytes
        dst = row * stride + 1
        rows[dst:dst + row_bytes] = view[src:src + row_bytes]
    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, color_type, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(rows, level=1))
            + chunk(b"IEND", b""))


def movement(start):
    # Start from the visible pose, including when a second request interrupts a move.
    for end, steps in MOVEMENT:
        for step in range(1, steps + 1):
            t = step / steps
            weight = t * t * (3 - 2 * t)
            yield tuple(round(a + (b - a) * weight) for a, b in zip(start, end))
        start = end


def _has_param_set(data, codec):
    i = 0
    n = len(data)
    while i + 4 < n:
        if data[i] == 0 and data[i + 1] == 0 and (
            data[i + 2] == 1 or (data[i + 2] == 0 and i + 3 < n and data[i + 3] == 1)
        ):
            j = i + (3 if data[i + 2] == 1 else 4)
            if j < n:
                if codec == 80 and ((data[j] >> 1) & 0x3F) in {32, 33}:
                    return True
                if codec != 80 and (data[j] & 0x1F) == 7:
                    return True
            i = j
        else:
            i += 1
    return False


def _crc_x25(data, crc_extra):
    crc = 0xFFFF
    for byte in data:
        tmp = byte ^ (crc & 0xFF)
        tmp = (tmp ^ (tmp << 4)) & 0xFF
        crc = ((crc >> 8) ^ (tmp << 8) ^ (tmp << 3) ^ (tmp >> 4)) & 0xFFFF
    tmp = crc_extra ^ (crc & 0xFF)
    tmp = (tmp ^ (tmp << 4)) & 0xFF
    return ((crc >> 8) ^ (tmp << 8) ^ (tmp << 3) ^ (tmp >> 4)) & 0xFFFF


class RdtParser:
    MAX_BUFFER_BYTES = 4096
    BATTERY_MSG_ID = 207
    BATTERY_PAYLOAD_LEN = 18
    # shortcut: CRC_EXTRA unverified against device, confirm with live capture
    BATTERY_CRC_EXTRA = 0

    def __init__(self):
        self._buf = bytearray()

    def reset(self):
        self._buf.clear()

    def feed(self, data):
        updates = []
        if not data:
            return updates
        view = memoryview(data)
        pos = 0
        total = len(view)
        while pos < total:
            chunk = view[pos:pos + self.MAX_BUFFER_BYTES]
            pos += len(chunk)
            if len(self._buf) + len(chunk) > self.MAX_BUFFER_BYTES:
                drop = len(self._buf) + len(chunk) - self.MAX_BUFFER_BYTES
                if drop >= len(self._buf):
                    self._buf.clear()
                else:
                    del self._buf[:drop]
            self._buf.extend(chunk)
            i = 0
            n = len(self._buf)
            while i < n:
                if self._buf[i] != 0xFE:
                    i += 1
                    continue
                if n - i < 8:
                    break
                plen = self._buf[i + 1]
                msgid = self._buf[i + 5]
                if msgid == self.BATTERY_MSG_ID:
                    if plen != self.BATTERY_PAYLOAD_LEN:
                        i += 1
                        continue
                    frame_len = 8 + plen
                    if n - i < frame_len:
                        break
                    got_crc = self._buf[i + 6 + plen] | (self._buf[i + 7 + plen] << 8)
                    expected_crc = _crc_x25(self._buf[i + 1:i + 6 + plen], self.BATTERY_CRC_EXTRA)
                    percent = self._buf[i + 22]
                    charging_raw = self._buf[i + 23]
                    if got_crc == expected_crc and percent <= 100 and charging_raw <= 2:
                        updates.append((percent, 1 if charging_raw > 0 else 0))
                        i += frame_len
                    else:
                        i += 1
                else:
                    frame_len = 8 + plen
                    if n - i < frame_len:
                        break
                    i += frame_len
            if i > 0:
                del self._buf[:i]
        return updates


def _parse_battery_status(data):
    updates = RdtParser().feed(data)
    return updates[-1] if updates else None


class VideoSource:
    def __init__(self, source):
        ffmpeg = find_ffmpeg()
        if not ffmpeg:
            raise RuntimeError("FFmpeg is required for --video mode")
        command = [ffmpeg, "-nostdin", "-v", "error"]
        if Path(source).exists():
            command.extend(["-re", "-stream_loop", "-1"])
        else:
            command.extend(["-fflags", "nobuffer", "-flags", "low_delay",
                            "-probesize", "32768", "-analyzeduration", "0"])
        command.extend([
            "-i", source,
            "-vf", f"fps=10,scale={WIDTH}:{HEIGHT},format=rgb24",
            "-an", "-f", "rawvideo", "pipe:1",
        ])
        self.process = subprocess.Popen(command, stdout=subprocess.PIPE)
        self.frame_lock = threading.Lock()
        self.latest = (None, WIDTH, HEIGHT, 0)
        self.reader = threading.Thread(target=self.read_frames, daemon=True)
        self.reader.start()

    def read_frames(self):
        while True:
            data = self.process.stdout.read(FRAME_RGB_BYTES)
            if len(data) != FRAME_RGB_BYTES:
                return
            with self.frame_lock:
                self.latest = (data, WIDTH, HEIGHT, time.monotonic_ns() // 1_000_000)

    def read_latest(self):
        with self.frame_lock:
            return self.latest

    def close(self):
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
        self.reader.join()
        self.process.stdout.close()


class KalaySource:
    def __init__(self, cmd, ffmpeg_bin=None):
        self.ffmpeg = ffmpeg_bin or find_ffmpeg()
        if not self.ffmpeg:
            raise RuntimeError("FFmpeg is required for --kalay-cmd mode")
        self.shim = subprocess.Popen(
            shlex.split(cmd), stdin=subprocess.PIPE, stdout=subprocess.PIPE
        )
        self.control_fd = self.shim.stdin.fileno()
        self.ff = None
        self._ff_queue = None
        self._writer_thread = None
        self._reader_thread = None
        self._frame_lock = threading.Lock()
        self.frame_ready = threading.Event()
        self._latest_pixels = None
        self._latest_size = (LIVE_WIDTH, LIVE_HEIGHT)
        self._latest_mono = 0
        self.decoded_frames = 0
        self.decoder_starts = 0
        self.session_starts = 0
        self._primed = False
        self._closed = False
        self._cb_lock = threading.Lock()
        self._last_shim = None
        self._last_battery = None
        self._rdt_parser = RdtParser()
        self.on_battery = None
        self.on_shim = None
        self._demux_thread = threading.Thread(target=self._demux, daemon=True)
        self._demux_thread.start()

    def bind(self, on_battery, on_shim):
        with self._cb_lock:
            self.on_battery = on_battery
            self.on_shim = on_shim
            shim_state = self._last_shim
            battery_state = self._last_battery
        if shim_state is not None:
            on_shim(*shim_state)
        if battery_state is not None:
            on_battery(*battery_state)

    def _close_ff(self):
        q = self._ff_queue
        self._ff_queue = None
        if q is not None:
            while True:
                try:
                    q.get_nowait()
                except queue.Empty:
                    break
            try:
                q.put_nowait(None)
            except queue.Full:
                pass
        proc = self.ff
        self.ff = None
        if proc is None:
            return
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=0.1)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
        for pipe in (proc.stdin, proc.stdout):
            try:
                pipe.close()
            except (OSError, ValueError):
                pass

    def _writer_loop(self, proc, q):
        while not self._closed:
            item = q.get()
            if item is None:
                try:
                    proc.stdin.close()
                except (OSError, ValueError):
                    pass
                return
            try:
                proc.stdin.write(item)
                proc.stdin.flush()
            except Exception:
                return

    def _reader_loop(self, proc):
        while not self._closed:
            data = proc.stdout.read(LIVE_RGB_BYTES)
            if len(data) != LIVE_RGB_BYTES:
                break
            with self._frame_lock:
                self.decoded_frames += 1
                self._latest_pixels = data
                self._latest_size = (LIVE_WIDTH, LIVE_HEIGHT)
                self._latest_mono = time.monotonic_ns() // 1_000_000
            self.frame_ready.set()

    def _start_decoder(self, codec):
        fmt = "hevc" if codec == 80 else "h264"
        self._close_ff()
        with self._frame_lock:
            self.decoder_starts += 1
        self.ff = subprocess.Popen([
            self.ffmpeg, "-nostdin", "-v", "error",
            "-threads", "2", "-thread_type", "slice",
            "-fflags", "discardcorrupt", "-flags", "low_delay", "-flags2", "fast",
            "-probesize", "4096", "-analyzeduration", "0",
            "-f", fmt, "-i", "pipe:0",
            "-fps_mode", "passthrough",
            "-vf", f"scale={LIVE_WIDTH}:{LIVE_HEIGHT},format=rgb24",
            "-an", "-f", "rawvideo", "pipe:1",
        ], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
        q = queue.Queue(maxsize=8)
        self._ff_queue = q
        self._writer_thread = threading.Thread(target=self._writer_loop, args=(self.ff, q), daemon=True)
        self._reader_thread = threading.Thread(target=self._reader_loop, args=(self.ff,), daemon=True)
        self._writer_thread.start()
        self._reader_thread.start()

    def stats(self):
        with self._frame_lock:
            return {
                "decoded_frames": self.decoded_frames,
                "decoder_starts": self.decoder_starts,
                "session_starts": self.session_starts,
            }

    def _demux(self):
        stream = self.shim.stdout
        while not self._closed:
            hdr = stream.read(5)
            if len(hdr) < 5:
                break
            length = struct.unpack("<I", hdr[:4])[0]
            kind = hdr[4]
            payload = stream.read(length) if length > 0 else b""
            if len(payload) < length:
                break
            if kind == 0xFE:
                self._primed = False
                self._close_ff()
                self._rdt_parser.reset()
                with self._frame_lock:
                    self.session_starts += 1
                    self._latest_pixels = None
                    self._latest_mono = 0
                with self._cb_lock:
                    self._last_shim = (0, 0, 0)
                    self._last_battery = None
                    cb = self.on_shim
                if cb is not None:
                    cb(0, 0, 0)
                continue
            if kind == 0xFD:
                if len(payload) >= 4:
                    shim_state = struct.unpack("<BBH", payload[:4])
                    with self._cb_lock:
                        self._last_shim = shim_state
                        cb = self.on_shim
                    if cb is not None:
                        cb(*shim_state)
                continue
            if kind == 0xFF:
                for telemetry in self._rdt_parser.feed(payload):
                    with self._cb_lock:
                        self._last_battery = telemetry
                        cb = self.on_battery
                    if cb is not None:
                        cb(*telemetry)
                continue
            if kind not in {78, 80}:
                continue
            if not self._primed:
                if not _has_param_set(payload, kind):
                    continue
                self._primed = True
                self._start_decoder(kind)
            q = self._ff_queue
            if q is None or (self.ff is not None and self.ff.poll() is not None):
                self._primed = False
                self._close_ff()
                continue
            try:
                q.put_nowait(payload)
            except queue.Full:
                self._primed = False
                self._close_ff()
        if self._ff_queue is not None and self.ff is not None:
            try:
                self._ff_queue.put_nowait(None)
            except queue.Full:
                pass
            if self._writer_thread is not None:
                self._writer_thread.join(timeout=0.3)
            if self._reader_thread is not None:
                self._reader_thread.join(timeout=0.5)

    def read_latest(self):
        with self._frame_lock:
            return self._latest_pixels, self._latest_size[0], self._latest_size[1], self._latest_mono

    def running(self):
        return self.shim.poll() is None and self._demux_thread.is_alive()

    def close(self):
        if self.shim.poll() is None:
            self.shim.terminate()
            try:
                self.shim.wait(timeout=1)
            except subprocess.TimeoutExpired:
                self.shim.kill()
                self.shim.wait()
        self._demux_thread.join(timeout=1.5)
        self._closed = True
        self._close_ff()
        for pipe in (self.shim.stdin, self.shim.stdout):
            try:
                pipe.close()
            except (OSError, ValueError):
                pass


class Demo:
    def __init__(self, replay, video=None, synthetic_dock=True, live_hardware=False, memory_file=None):
        self.replay = replay
        self.video = video
        self.synthetic_dock = synthetic_dock
        self.live_hardware = live_hardware
        self.source_mode = "robot" if live_hardware else "video" if video is not None else "simulator"
        self.memory_file = Path(memory_file) if memory_file else None
        self.lock = threading.Lock()
        self.new_frame = threading.Condition(self.lock)
        self.finished = threading.Event()
        self.seq = 0
        self.boot = secrets.token_hex(16)
        self.revision = 0
        self.image = b""
        self.status = {}
        self.return_started = None
        self.box = REFERENCE
        self.movement = iter(())
        self._cached_box = None
        self._cached_pixels = b""
        self._cached_png = b""
        self._cached_key = None
        self.frame_received = 0
        self.last_raw_mono = 0
        self.last_dock_sync_at = 0
        self.last_tick_at = 0
        self.frame_processing_ms = 0
        self.manual_driver = None
        self.manual_sequence = {}
        self._persisted_key = None
        self._human_memory_blob = ""
        if self.memory_file and self.memory_file.exists():
            try:
                saved = json.loads(self.memory_file.read_text(encoding="utf-8"))
                self._human_memory_blob = saved.get("memory_blob", "") or ""
            except Exception:
                pass
        self.last_battery_at = 0
        self.last_charging = -1
        self.rdt_ok = False
        self.cutoff_verified = not live_hardware and video is None
        self.camera = {
            "color_mode": "color",
            "night_mode": "off",
            "ir_tint": "clean",
            "night_active": False,
            "boost_active": False,
            "eyes": True,
            "avoid": True,
            "fall": True,
        }
        self.auto_ir_on = False
        self.auto_dark_since = None
        self.auto_bright_since = None
        self.auto_switched_at = 0
        self.patrol = None
        self.patrol_message = ""
        self.patrol_drove = False
        self.invert_turn = False
        self._latest_sig = None
        self._cached_sig = None
        self.teach_recording = False
        self.teach_segments = []
        self.teach_active = None

    @staticmethod
    def now():
        return time.monotonic_ns() // 1_000_000

    def _patrol_snapshot(self):
        p = self.patrol
        if not p:
            return {
                "active": False,
                "kind": "area",
                "phase": "idle",
                "lap": 0,
                "laps": 1,
                "side": 0,
                "position": None,
                "message": self.patrol_message,
            }
        return {
            "active": True,
            "kind": p["kind"],
            "phase": p["phase"],
            "lap": p["lap"] + 1,
            "laps": p["laps"],
            "side": p["side"] + 1,
            "position": p["position"],
            "message": self.patrol_message,
        }

    def record(self, status):
        action = status["dock_action"]
        if action == "start":
            self.return_started = self.now()
        elif action == "cancel":
            self.return_started = None
        status = {
            **status,
            "boot": self.boot,
            "revision": self.revision,
            "live_hardware": self.live_hardware,
            "source_mode": self.source_mode,
            "camera": dict(self.camera),
            "invert_turn": self.invert_turn,
            "patrol": self._patrol_snapshot(),
        }
        status.setdefault("follow_permitted", self.source_mode != "video")
        status.setdefault("rdt_connected", self.rdt_ok if self.live_hardware else True)
        status.setdefault("cutoff_verified", self.cutoff_verified if not self.live_hardware else False)
        status.setdefault("shim_cutoffs", 0)
        if self.live_hardware and hasattr(self.video, "stats"):
            status.update(self.video.stats())
        if status != self.status:
            self.revision += 1
            self.status = {**status, "revision": self.revision}
            self.new_frame.notify_all()
        if not self.status.get("manual_active"):
            self.manual_driver = None

    def _persist(self, force=False, update_target=False):
        if not self.memory_file:
            return
        if update_target:
            mem = self.status.get("memory", "none")
            if mem == "none":
                self._human_memory_blob = ""
            else:
                exported = self.replay.call("export-memory", self.now())
                self._human_memory_blob = exported.get("memory_blob", "")
        key = (self._human_memory_blob, tuple(sorted((self.status.get("policy") or {}).items())))
        if not force and key == self._persisted_key:
            return
        self.memory_file.parent.mkdir(parents=True, exist_ok=True)
        tmp = self.memory_file.with_suffix(".tmp")
        tmp.write_text(json.dumps({"policy": self.status.get("policy", {}),
                                   "memory_blob": self._human_memory_blob}), encoding="utf-8")
        os.replace(tmp, self.memory_file)
        self._persisted_key = key

    def on_battery(self, percent, charging):
        if not 0 <= percent <= 100 or charging not in (0, 1):
            return
        with self.lock:
            if self.finished.is_set():
                return
            now = self.now()
            self.last_battery_at = now
            self.last_charging = charging
            try:
                self.record(self.replay.call("battery-telemetry", now, percent, charging))
            except Exception:
                return
            self.new_frame.notify_all()

    def on_shim(self, event, rdt_ok, cutoffs):
        with self.lock:
            if self.finished.is_set():
                return
            if event == 0:
                self.last_battery_at = 0
                self.last_charging = -1
                self.last_raw_mono = 0
                self.frame_received = 0
                self._cached_key = None
                self._cached_png = b""
                self._cached_sig = None
                self.image = b""
            self.rdt_ok = bool(rdt_ok)
            now = self.now()
            try:
                self.record(self.replay.call("shim-status", now, int(self.rdt_ok), int(cutoffs)))
                if event == 1 and self.rdt_ok:
                    self.record(self.replay.call("device-control", now, "night_vision",
                                                 1 if self.camera["night_active"] else 0))
                    self.record(self.replay.call("device-control", now, "eyes", int(self.camera["eyes"])))
                    self.record(self.replay.call("device-control", now, "avoid", int(self.camera["avoid"])))
                    self.record(self.replay.call("device-control", now, "fall", int(self.camera["fall"])))
            except Exception:
                return
            self.new_frame.notify_all()

    def _flush_teach_active(self, now):
        if not self.teach_active:
            return
        ly, rx, started = self.teach_active
        self.teach_active = None
        dt = max(0, min(6000, int(now - started)))
        if dt < 40 or (abs(ly) < 0.01 and abs(rx) < 0.01):
            return
        if self.teach_segments:
            prev = self.teach_segments[-1]
            if abs(prev["ly"] - ly) < 0.04 and abs(prev["rx"] - rx) < 0.04 and prev["dt"] < 4000:
                prev["dt"] = min(6000, prev["dt"] + dt)
                return
        if len(self.teach_segments) < 32:
            self.teach_segments.append({"ly": round(ly, 3), "rx": round(rx, 3), "dt": dt})

    def _record_teach_drive(self, now, ly, rx):
        if not self.teach_recording:
            return
        if abs(ly) < 0.01 and abs(rx) < 0.01:
            self._flush_teach_active(now)
            return
        if self.teach_active:
            prev_ly, prev_rx, _ = self.teach_active
            if abs(prev_ly - ly) < 0.04 and abs(prev_rx - rx) < 0.04:
                return
            self._flush_teach_active(now)
        self.teach_active = [float(ly), float(rx), now]

    def _stop_patrol(self, now, message="Patrol ended.", send_zero=True):
        p = self.patrol
        if not p:
            return
        self.patrol = None
        self.patrol_message = message
        if p["kind"] == "factory" and self.live_hardware:
            self.record(self.replay.call("device-control", now, "factory_patrol", 0))
        elif send_zero and self.patrol_drove:
            self.record(self.replay.call("manual-drive", now, p["driver"], 0, 0, 250))
        self.patrol_drove = False
        self.record(self.replay.call("tick", now))

    def _patrol_drive(self, now, driver, ly, rx):
        p = self.patrol
        if not p:
            return
        axes = (round(ly, 3), round(rx, 3))
        if axes == p["last_axes"] and now - p["last_drive_at"] < 50:
            return
        p["last_drive_at"] = now
        p["last_axes"] = axes
        if axes[0] or axes[1]:
            self.patrol_drove = True
        self.record(self.replay.call("manual-drive", now, driver, axes[0], axes[1], 250))

    def _patrol_pan(self):
        p = self.patrol
        if not p or not p.get("position"):
            return (0, 0)
        return (round((p["position"][0] - 0.5) * 40), round((p["position"][1] - 0.5) * 16))

    def _step_patrol(self, now, raw_rgb=None):
        p = self.patrol
        if not p:
            return
        if p["kind"] == "factory":
            return
        drv = p["driver"]
        turn_sign = -1.0 if self.invert_turn else 1.0
        if p["kind"] == "checkpoints":
            steps = p["steps"]
            if p["index"] >= len(steps):
                self._stop_patrol(now, "Checkpoint patrol complete.", send_zero=True)
                return
            step = steps[p["index"]]
            legs = step.get("legs") or []
            if p["phase"] == "leg":
                if p["leg_idx"] >= len(legs):
                    if self.patrol_drove:
                        self._patrol_drive(now, drv, 0.0, 0.0)
                    p["phase"] = "verify"
                    p["phase_at"] = now
                    p["align_sub"] = "settle"
                    p["align_pulses"] = 0
                    p["align_dir"] = 0
                    p["align_zero"] = None
                    step_name = step.get("name") or f"Checkpoint {p['index'] + 1}"
                    self.patrol_message = f"Checking {step_name}"
                    return
                seg = legs[p["leg_idx"]]
                dt = max(60, min(6000, int(seg.get("dt", 200))))
                if now - p["phase_at"] < dt:
                    self._patrol_drive(now, drv, float(seg.get("ly", 0.0)), float(seg.get("rx", 0.0)))
                else:
                    p["leg_idx"] += 1
                    p["phase_at"] = now
                    if p["leg_idx"] >= len(legs):
                        if self.patrol_drove:
                            self._patrol_drive(now, drv, 0.0, 0.0)
                        p["phase"] = "verify"
                        p["align_sub"] = "settle"
                        p["align_pulses"] = 0
                        p["align_dir"] = 0
                        p["align_zero"] = None
                        step_name = step.get("name") or f"Checkpoint {p['index'] + 1}"
                        self.patrol_message = f"Checking {step_name}"
            elif p["phase"] == "verify":
                if p.get("align_sub") == "pulse":
                    if now - p["phase_at"] < 140:
                        self._patrol_drive(now, drv, 0.0, 0.18 * p["align_dir"] * turn_sign)
                        return
                    self._patrol_drive(now, drv, 0.0, 0.0)
                    p["align_sub"] = "settle"
                    p["phase_at"] = now
                    return
                settle_ms = 220 if self.live_hardware else 120
                if now - p["phase_at"] < settle_ms:
                    return
                target_sig = step.get("signature")
                did_nudge = False
                if (self._latest_sig is not None and isinstance(target_sig, list) and
                        len(target_sig) == 144 and p.get("align_pulses", 0) < 3):
                    shift, best_score, zero_score = visual_shift_and_score(self._latest_sig, target_sig)
                    prev_zero = p.get("align_zero")
                    worse = prev_zero is not None and zero_score < prev_zero - 0.06
                    if (not worse and abs(shift) >= 1 and best_score >= 0.58 and
                            best_score > zero_score + 0.03):
                        p["align_dir"] = 1 if shift > 0 else -1
                        p["align_zero"] = zero_score
                        p["align_pulses"] = p.get("align_pulses", 0) + 1
                        p["align_sub"] = "pulse"
                        p["phase_at"] = now
                        step_name = step.get("name") or f"Checkpoint {p['index'] + 1}"
                        self.patrol_message = f"Aligning {step_name}"
                        self._patrol_drive(now, drv, 0.0, 0.18 * p["align_dir"] * turn_sign)
                        did_nudge = True
                if did_nudge:
                    return
                p["index"] += 1
                p["leg_idx"] = 0
                p["phase"] = "leg"
                p["phase_at"] = now
                if p["index"] >= len(steps):
                    self._stop_patrol(now, "Checkpoint patrol complete.", send_zero=True)
                else:
                    nxt = steps[p["index"]]
                    nxt_name = nxt.get("name") or f"Checkpoint {p['index'] + 1}"
                    prefix = "Returning to" if nxt.get("retrace") else "Heading to"
                    self.patrol_message = f"{prefix} {nxt_name}"
            return

        l, t, r, b = p["area"]
        corners = ((l, t), (r, t), (r, b), (l, b), (l, t))
        lap_label = f"Lap {p['lap'] + 1} of {p['laps']}" if p["laps"] else f"Lap {p['lap'] + 1}"
        if p["phase"] == "undock":
            if not self.live_hardware:
                self.return_started = None
                self.record(self.replay.call("dock-status", now, "idle"))
                p["phase"] = "leg"
                p["phase_at"] = now
                p["position"] = [l, t]
                self.patrol_message = f"{lap_label} · patrolling side 1"
                return
            if self.status.get("battery_charging") and now - p["phase_at"] < 1800:
                self._patrol_drive(now, drv, 0.36, 0.0)
                return
            self._patrol_drive(now, drv, 0.0, 0.0)
            p["phase"] = "leg"
            p["phase_at"] = now
            p["position"] = [l, t]
            self.patrol_message = f"{lap_label} · patrolling side 1"
            return

        if p["phase"] == "leg":
            ca = corners[p["side"]]
            cb = corners[p["side"] + 1]
            span = abs(cb[0] - ca[0]) + abs(cb[1] - ca[1])
            dur = (1200 + span * 2200) if self.live_hardware else (450 + span * 600)
            frac = min(1.0, (now - p["phase_at"]) / dur)
            p["position"] = [round(ca[0] + (cb[0] - ca[0]) * frac, 4),
                             round(ca[1] + (cb[1] - ca[1]) * frac, 4)]
            if self.live_hardware and raw_rgb is not None and now - p["phase_at"] >= 450:
                sample = raw_rgb[::96]
                prev = p["prev_sample"]
                if prev is not None and len(prev) == len(sample):
                    diff = sum(abs(x - y) for x, y in zip(sample, prev)) / len(sample)
                    p["stuck"] = p["stuck"] + 1 if diff < 2.2 else 0
                p["prev_sample"] = sample
                if p["stuck"] >= 4:
                    p["stuck"] = 0
                    p["phase"] = "avoid"
                    p["phase_at"] = now
                    self.patrol_message = "Obstacle detected · backing up"
                    self._patrol_drive(now, drv, -0.22, 0.0)
                    return
            if frac < 1.0:
                self._patrol_drive(now, drv, 0.28, 0.0)
                return
            self._patrol_drive(now, drv, 0.0, 0.0)
            p["phase"] = "turn"
            p["phase_at"] = now
            self.patrol_message = f"{lap_label} · corner {p['side'] + 1}"
            return

        if p["phase"] == "avoid":
            if now - p["phase_at"] < 350:
                self._patrol_drive(now, drv, -0.22, 0.0)
                return
            self._patrol_drive(now, drv, 0.0, 0.0)
            p["phase"] = "turn"
            p["phase_at"] = now
            return

        if p["phase"] == "turn":
            turn_dur = 640 if self.live_hardware else 200
            if now - p["phase_at"] < turn_dur:
                self._patrol_drive(now, drv, 0.0, 0.25 * (-1.0 if self.invert_turn else 1.0))
                return
            self._patrol_drive(now, drv, 0.0, 0.0)
            p["phase"] = "scan"
            p["phase_at"] = now
            return

        if p["phase"] == "scan":
            if p["spot_stop"]:
                if self.status.get("spotted_box"):
                    self.record(self.replay.call("spot-lock", now, drv, self.status["generation"]))
                    self._stop_patrol(now, "Pet spotted · tracking target.", send_zero=False)
                    return
                if self.status.get("state") in {"TRACKING", "READY"}:
                    self._stop_patrol(now, "Pet spotted · tracking target.", send_zero=False)
                    return
            scan_dur = 650 if self.live_hardware else 200
            if now - p["phase_at"] >= scan_dur:
                p["side"] += 1
                if p["side"] >= 4:
                    p["side"] = 0
                    p["lap"] += 1
                    if p["laps"] > 0 and p["lap"] >= p["laps"]:
                        dock_after = p["dock_after"]
                        self._stop_patrol(now, "Auto patrol complete.", send_zero=True)
                        if dock_after and self.status.get("can_home"):
                            self.record(self.replay.call("home", now, drv, self.status["generation"]))
                        return
                p["phase"] = "leg"
                p["phase_at"] = now
                p["stuck"] = 0
                p["prev_sample"] = None
                next_lap = f"Lap {p['lap'] + 1} of {p['laps']}" if p["laps"] else f"Lap {p['lap'] + 1}"
                self.patrol_message = f"{next_lap} · patrolling side {p['side'] + 1}"

    def _render_preview(self, raw_rgb, width, height, now):
        night_mode = self.camera["night_mode"]
        mean_luma = sum(raw_rgb[::96]) / max(1, len(raw_rgb) // 96)
        if night_mode == "auto":
            if not self.auto_ir_on:
                if mean_luma < 38 and now - self.auto_switched_at >= 5000:
                    if self.auto_dark_since is None:
                        self.auto_dark_since = now
                    elif now - self.auto_dark_since >= 2500:
                        self.auto_ir_on = True
                        self.auto_dark_since = None
                        self.auto_switched_at = now
                        if self.live_hardware:
                            self.replay.call("device-control", now, "night_vision", 1)
                else:
                    self.auto_dark_since = None
            else:
                if mean_luma > 92 and now - self.auto_switched_at >= 5000:
                    if self.auto_bright_since is None:
                        self.auto_bright_since = now
                    elif now - self.auto_bright_since >= 4000:
                        self.auto_ir_on = False
                        self.auto_bright_since = None
                        self.auto_switched_at = now
                        if self.live_hardware:
                            self.replay.call("device-control", now, "night_vision", 0)
                else:
                    self.auto_bright_since = None
        night_active = (night_mode == "night") or (night_mode == "auto" and self.auto_ir_on)
        boost_active = (night_mode == "boost") or (night_mode == "auto" and not self.auto_ir_on and mean_luma < 72)
        self.camera["night_active"] = night_active
        self.camera["boost_active"] = boost_active

        if night_active:
            luma = rgb_to_gray(raw_rgb)
            if self.camera["ir_tint"] == "emerald":
                return png(apply_emerald_tint(luma), width, height), boost_active
            return png(luma, width, height), boost_active
        if self.camera["color_mode"] == "bw":
            luma = rgb_to_gray(raw_rgb)
            if boost_active:
                luma = luma.translate(BOOST_LUT)
            return png(luma, width, height), boost_active
        preview_rgb = raw_rgb.translate(BOOST_LUT) if boost_active else raw_rgb
        return png(preview_rgb, width, height), boost_active

    def capture(self):
        raw_rgb = None
        tracker_gray = None
        latest_sig = None
        raw_mono = 0
        width, height = WIDTH, HEIGHT
        received = self.now()
        started = self.now()
        with self.lock:
            cam_key = (self.camera["color_mode"], self.camera["night_mode"], self.camera["ir_tint"], self.auto_ir_on)
            pan = self._patrol_pan()
        if self.video is not None:
            latest, width, height, raw_mono = self.video.read_latest()
            fresh_limit = 600
            if (latest is not None and raw_mono > self.last_raw_mono and
                    raw_mono > self.frame_received and self.now() - raw_mono < fresh_limit):
                self.last_raw_mono = raw_mono
                now_ts = self.now()
                received = raw_mono
                raw_rgb = latest
                cache_key = (raw_rgb, cam_key)
                if cache_key == self._cached_key:
                    image, boost_active = self._cached_png
                    latest_sig = self._cached_sig
                else:
                    image, boost_active = self._render_preview(raw_rgb, width, height, now_ts)
                    latest_sig = room_signature_rgb(raw_rgb, width, height)
                    self._cached_key = cache_key
                    self._cached_png = (image, boost_active)
                    self._cached_sig = latest_sig
        else:
            with self.lock:
                box = next(self.movement, REFERENCE)
                self.box = box
                scene_key = (box, pan, cam_key)
                same_scene = scene_key == self._cached_key
                if same_scene:
                    tracker_gray = self._cached_pixels
                    image, boost_active = self._cached_png
                    latest_sig = self._cached_sig
            if not same_scene:
                tracker_gray = bytes(scene(box))
                raw_rgb = color_scene(box, pan=pan)
                image, boost_active = self._render_preview(raw_rgb, WIDTH, HEIGHT, self.now())
                latest_sig = room_signature_rgb(raw_rgb, WIDTH, HEIGHT)
                self._cached_key = scene_key
                self._cached_pixels = tracker_gray
                self._cached_png = (image, boost_active)
                self._cached_sig = latest_sig

        with self.lock:
            now = self.now()
            if latest_sig is not None:
                self._latest_sig = latest_sig
            if self.teach_recording and self.teach_active and not self.status.get("manual_active"):
                self._flush_teach_active(now)
            if self.synthetic_dock:
                # These are simulated factory telemetry samples, never wheel commands.
                elapsed = 0 if self.return_started is None else now - self.return_started
                signal = "idle"
                if self.return_started is not None:
                    signal = "returning" if elapsed < 1200 else "contact" if elapsed < 1800 else "charging"
                self.record(self.replay.call("dock-status", now, signal))
            elif self.live_hardware and self.rdt_ok and (raw_rgb is not None or (self.frame_received > 0 and now - self.frame_received < 750)) and self.last_battery_at > 0 and now - self.last_battery_at <= 4000:
                if self.last_charging == 0:
                    if self.return_started is not None:
                        if now - self.return_started >= 60000:
                            self.return_started = None
                            self.record(self.replay.call("dock-status", now, "failed"))
                        elif now - self.return_started >= 300:
                            self.record(self.replay.call("dock-status", now, "returning"))
                    elif self.status.get("dock") != "stopping" and (
                        self.status.get("dock") != "idle" or now - self.last_dock_sync_at >= 600
                    ):
                        self.last_dock_sync_at = now
                        self.record(self.replay.call("dock-status", now, "idle"))
                else:
                    self.return_started = None
            frame_age_limit = 650 if self.video is not None else 250
            if (raw_rgb is None and tracker_gray is None) or now - received >= frame_age_limit:
                camera_lost = self.frame_received == 0 or now - self.frame_received >= 750
                if self.patrol and camera_lost:
                    self._stop_patrol(now, "Patrol paused: camera connection lost.", send_zero=True)
                if not self.live_hardware or self.patrol or camera_lost or now - self.last_tick_at >= 120:
                    self.last_tick_at = now
                    self.record(self.replay.call("tick", now))
                if self.patrol and not camera_lost:
                    self._step_patrol(now, None)
                self.new_frame.notify_all()
                return
            self.seq += 1
            if self.video is not None:
                self.record(self.replay.call("frame-rgb", now, self.seq, received, width, height, int(boost_active), pixels=raw_rgb))
            else:
                self.record(self.replay.call("frame", now, self.seq, received, pixels=tracker_gray))
            self.frame_received = received
            if self.patrol:
                self._step_patrol(now, raw_rgb)
            if self.memory_file and self.status.get("state") in {"TRACKING", "READY"} and self.status.get("owner") not in (0, 2**53 - 1):
                self._persist(update_target=True)
            self.image = image
            self.frame_processing_ms = self.now() - started
            self.new_frame.notify_all()

    def run(self):
        interval = 0.04 if self.live_hardware else FRAME_INTERVAL
        deadline = time.monotonic() + interval
        while not self.finished.is_set():
            if self.live_hardware:
                wait_s = 0.02 if self.patrol else 0.05
                self.video.frame_ready.wait(wait_s)
                self.video.frame_ready.clear()
            elif self.finished.wait(max(0, deadline - time.monotonic())):
                break
            if self.finished.is_set():
                break
            try:
                self.capture()
            except RuntimeError:
                self.finished.set()
                break
            step_interval = 0.02 if (self.live_hardware and self.patrol) else interval
            deadline = max(deadline + step_interval, time.monotonic())

    def action(self, action, payload):
        driver = payload["driver"]
        generation = payload["generation"]
        if (type(driver) is not int or not 0 < driver < 2**53
                or type(generation) is not int or not 0 < generation < 2**53):
            raise ValueError("Invalid session")
        with self.lock:
            if payload["boot"] != self.boot:
                return {**self.status, "accepted": False}
            if self.source_mode == "video" and action in {"follow", "home", "lift", "move", "manual", "motors", "patrol", "cutoff-test"}:
                return {**self.status, "accepted": False, "control_error": "video-only"}
            args = [driver, generation]
            if action == "camera":
                now = self.now()
                if payload.get("color_mode") in {"color", "bw"}:
                    self.camera["color_mode"] = payload["color_mode"]
                if payload.get("night_mode") in {"off", "auto", "night", "boost"}:
                    self.camera["night_mode"] = payload["night_mode"]
                    self.auto_ir_on = False
                    self.auto_dark_since = None
                    self.auto_bright_since = None
                    self.camera["night_active"] = payload["night_mode"] == "night"
                    self.camera["boost_active"] = payload["night_mode"] == "boost"
                    if self.live_hardware:
                        nv = 1 if payload["night_mode"] == "night" else 0
                        self.record(self.replay.call("device-control", now, "night_vision", nv))
                if payload.get("ir_tint") in {"clean", "emerald"}:
                    self.camera["ir_tint"] = payload["ir_tint"]
                for key in ("eyes", "avoid", "fall"):
                    if key in payload and isinstance(payload[key], bool):
                        self.camera[key] = payload[key]
                        if self.live_hardware:
                            self.record(self.replay.call("device-control", now, key, int(payload[key])))
                self._cached_key = None
                self.record(self.replay.call("tick", now))
                return {**self.status, "accepted": True}
            if action == "cutoff-test":
                if self.patrol:
                    return {**self.status, "accepted": False, "control_error": "patrol-active"}
                if self.live_hardware:
                    self.record(self.replay.call("cutoff-check", self.now()))
                else:
                    self.cutoff_verified = True
                    self.record(self.replay.call("tick", self.now()))
                return self.status
            if action == "patrol":
                now = self.now()
                mode = payload.get("mode")
                if mode == "stop":
                    self._stop_patrol(now, "Patrol ended.", send_zero=True)
                    return {**self.status, "accepted": True}
                if mode == "teach-start":
                    self.teach_recording = True
                    self.teach_segments = []
                    self.teach_active = None
                    return {**self.status, "accepted": True}
                if mode == "teach-cancel":
                    self.teach_recording = False
                    self.teach_segments = []
                    self.teach_active = None
                    return {**self.status, "accepted": True}
                if mode == "teach-capture":
                    self._flush_teach_active(now)
                    legs = self.teach_segments[:]
                    self.teach_segments = []
                    self.teach_active = None
                    self.teach_recording = True
                    return {**self.status, "accepted": True, "teach_legs": legs}
                if mode == "start":
                    self.teach_recording = False
                    self.teach_active = None
                    if (self.status.get("owner") not in (0, driver, 2**53 - 1) or
                        self.status.get("state") == "FOLLOWING" or
                        self.status.get("dock") in {"requested", "returning", "checking-charge", "stopping"}):
                        return {**self.status, "accepted": False}
                    kind = payload.get("kind", "area")
                    if kind == "factory":
                        if not self.live_hardware:
                            return {**self.status, "accepted": False}
                        self.patrol = {"kind": "factory", "phase": "roving", "lap": 0, "laps": 0,
                                       "side": 0, "position": None, "driver": driver, "last_drive_at": 0, "last_axes": (0, 0)}
                        self.patrol_message = "Factory roving patrol active."
                        self.record(self.replay.call("device-control", now, "factory_patrol", 1))
                        return {**self.status, "accepted": True}
                    if kind == "checkpoints":
                        cps = payload.get("checkpoints") or []
                        if not isinstance(cps, list) or len(cps) < 2 or len(cps) > 12:
                            raise ValueError("Invalid checkpoints")
                        best_idx = -1
                        best_sim = 0.72
                        if self._latest_sig is not None:
                            for idx, cp in enumerate(cps):
                                sig = cp.get("signature")
                                if isinstance(sig, list) and len(sig) == 144:
                                    sim = sum(a * b for a, b in zip(self._latest_sig, sig))
                                    if sim >= best_sim:
                                        best_sim = sim
                                        best_idx = idx
                        steps = []
                        if best_idx == len(cps) - 1:
                            for k in range(len(cps) - 1, 0, -1):
                                target_cp = cps[k - 1]
                                fwd_legs = cps[k].get("legs") or []
                                rev_legs = [
                                    {"ly": round(-float(s.get("ly", 0.0)), 3),
                                     "rx": round(-float(s.get("rx", 0.0)), 3),
                                     "dt": int(s.get("dt", 200))}
                                    for s in reversed(fwd_legs)
                                ]
                                target_name = target_cp.get("name") or f"Checkpoint {k}"
                                steps.append({
                                    "name": target_name,
                                    "signature": target_cp.get("signature"),
                                    "legs": rev_legs,
                                    "retrace": True,
                                })
                            for k in range(1, len(cps)):
                                cp = cps[k]
                                cp_name = cp.get("name") or f"Checkpoint {k + 1}"
                                steps.append({
                                    "name": cp_name,
                                    "signature": cp.get("signature"),
                                    "legs": cp.get("legs") or [],
                                    "retrace": False,
                                })
                        elif 0 < best_idx < len(cps) - 1:
                            for k in range(best_idx + 1, len(cps)):
                                cp = cps[k]
                                cp_name = cp.get("name") or f"Checkpoint {k + 1}"
                                steps.append({
                                    "name": cp_name,
                                    "signature": cp.get("signature"),
                                    "legs": cp.get("legs") or [],
                                    "retrace": False,
                                })
                        else:
                            first_name = cps[0].get("name") or "Checkpoint 1"
                            steps.append({
                                "name": first_name,
                                "signature": cps[0].get("signature"),
                                "legs": [],
                                "retrace": False,
                            })
                            for k in range(1, len(cps)):
                                cp = cps[k]
                                cp_name = cp.get("name") or f"Checkpoint {k + 1}"
                                steps.append({
                                    "name": cp_name,
                                    "signature": cp.get("signature"),
                                    "legs": cp.get("legs") or [],
                                    "retrace": False,
                                })
                        init_phase = "leg" if steps[0]["legs"] else "verify"
                        first_label = steps[0]["name"]
                        start_verb = ("Returning to" if steps[0]["retrace"] else "Heading to") if init_phase == "leg" else "Checking"
                        self.patrol = {
                            "kind": "checkpoints",
                            "phase": init_phase,
                            "phase_at": now,
                            "align_sub": "settle",
                            "align_pulses": 0,
                            "align_dir": 0,
                            "align_zero": None,
                            "lap": 0,
                            "laps": 1,
                            "side": 0,
                            "index": 0,
                            "leg_idx": 0,
                            "steps": steps,
                            "position": None,
                            "driver": driver,
                            "last_drive_at": 0,
                            "last_axes": (0, 0),
                        }
                        self.patrol_drove = False
                        self.patrol_message = f"{start_verb} {first_label}"
                        self.record(self.replay.call("tick", now))
                        return {**self.status, "accepted": True}
                    area = payload.get("area") or [0.22, 0.22, 0.78, 0.78]
                    if (not isinstance(area, list) or len(area) != 4 or
                        any(type(v) not in (int, float) or not 0.05 <= v <= 0.95 for v in area) or
                        area[2] - area[0] < 0.12 or area[3] - area[1] < 0.12):
                        raise ValueError("Invalid area")
                    laps = int(payload.get("laps", 1))
                    if not 0 <= laps <= 10:
                        raise ValueError("Invalid laps")
                    spot_stop = bool(payload.get("spot_stop", False))
                    dock_after = bool(payload.get("dock_after", False))
                    start_phase = "undock" if self.status.get("dock") == "charging" else "leg"
                    self.patrol = {
                        "kind": "area",
                        "area": [float(v) for v in area],
                        "laps": laps,
                        "lap": 0,
                        "side": 0,
                        "phase": start_phase,
                        "phase_at": now,
                        "position": [float(area[0]), float(area[1])],
                        "spot_stop": spot_stop,
                        "dock_after": dock_after,
                        "driver": driver,
                        "stuck": 0,
                        "prev_sample": None,
                        "last_drive_at": 0,
                        "last_axes": (0, 0),
                    }
                    self.patrol_drove = False
                    self.patrol_message = (
                        "Undocking for auto patrol…" if start_phase == "undock"
                        else f"Lap 1{f' of {laps}' if laps else ''} · patrolling side 1"
                    )
                    self.record(self.replay.call("tick", now))
                    return {**self.status, "accepted": True}
                raise ValueError("Invalid patrol mode")
            if action == "lift":
                if self.status["dock"] != "charging" or self.status["generation"] != generation:
                    return {**self.status, "accepted": False}
                self.return_started = None
                self.record(self.replay.call("dock-status", self.now(), "idle"))
                return self.status
            if action == "move":
                if self.status["owner"] != driver or self.status["generation"] != generation:
                    return {**self.status, "accepted": False}
                self.movement = movement(self.box)
                return {**self.status, "accepted": True}
            if action == "manual":
                ly = float(payload["ly"])
                rx = float(payload["rx"])
                ttl = int(payload.get("ttl", 280))
                sequence = payload.get("sequence")
                if not (-1.0 <= ly <= 1.0 and -1.0 <= rx <= 1.0 and 50 <= ttl <= 400):
                    raise ValueError("Invalid manual drive")
                if sequence is not None:
                    if type(sequence) is not int or not 0 < sequence < 2**53:
                        raise ValueError("Invalid manual sequence")
                    if sequence <= self.manual_sequence.get(driver, 0):
                        return {**self.status, "accepted": False}
                    self.manual_sequence.pop(driver, None)
                    self.manual_sequence[driver] = sequence
                    if len(self.manual_sequence) > 32:
                        del self.manual_sequence[next(iter(self.manual_sequence))]
                if (ly or rx) and self.live_hardware and not self.rdt_ok:
                    return {**self.status, "accepted": False, "control_error": "rdt-disconnected"}
                if (ly or rx) and self.patrol:
                    self._stop_patrol(self.now(), "Patrol stopped by manual drive.", send_zero=False)
                if self.manual_driver not in (None, driver):
                    return {**self.status, "accepted": False}
                if (ly or rx) and (generation != self.status["generation"] or
                                   (self.status["owner"] not in (0, driver) and not self.status["autonomous"])):
                    return {**self.status, "accepted": False}
                now = self.now()
                self.record(self.replay.call("manual-drive", now, driver, round(ly, 3), round(rx, 3), ttl))
                if self.teach_recording:
                    if (ly == 0 and rx == 0) or not self.status.get("manual_active"):
                        self._flush_teach_active(now)
                    elif self.status.get("accepted"):
                        self._record_teach_drive(now, ly, rx)
                if self.status.get("manual_active"):
                    self.manual_driver = driver
                if ly == 0 and rx == 0:
                    self._persist()
                return self.status
            if action == "motors":
                if (generation != self.status["generation"] or
                    self.status["owner"] not in (0, driver, 2**53 - 1) or
                    self.status["dock_owner"] not in (0, driver, 2**53 - 1)):
                    return {**self.status, "accepted": False}
                enabled = 1 if payload.get("enabled", self.status.get("follow_enabled", False)) else 0
                fwd = float(payload.get("forward_speed", 0.30))
                turn = float(payload.get("turn_speed", 0.22))
                inv = 1 if payload.get("invert_turn") else 0
                if not (0.1 <= fwd <= 1.0 and 0.1 <= turn <= 1.0):
                    raise ValueError("Invalid speed")
                self.invert_turn = bool(inv)
                self.record(self.replay.call("set-motors", self.now(), enabled, round(fwd, 3), round(turn, 3), inv))
                return self.status
            if action == "policy":
                p = payload.get("policy") or {}
                keys = ("auto_reacquire", "auto_gap", "auto_spot", "base_watch",
                        "auto_dock_low_battery", "auto_dock_idle", "auto_dock_disconnect")
                flags = [1 if p.get(k) else 0 for k in keys]
                timeout = int(p.get("idle_timeout_ms", 90000))
                self.record(self.replay.call("policy", self.now(), *flags, max(1000, timeout)))
                self._persist(force=True)
                return self.status
            if action == "forget":
                self.record(self.replay.call("forget-memory", self.now(), *args))
                self._human_memory_blob = ""
                self._persist(force=True)
                return self.status
            if action in {"resume", "spot"}:
                cmd = "resume-memory" if action == "resume" else "spot-lock"
                self.record(self.replay.call(cmd, self.now(), *args))
                if action == "spot" and self.status["accepted"]:
                    self._persist(force=True, update_target=True)
                return self.status
            if action in {"select", "heartbeat", "follow"}:
                frame = payload["frame"]
                if type(frame) is not int or not 0 < frame <= self.seq:
                    raise ValueError("Invalid frame")
                args.append(frame)
            if action == "select":
                x, y, width, height = payload["box"]
                box = (x, y, width, height)
                if any(type(n) not in {int, float} or not 0 <= n <= 1 for n in box):
                    raise ValueError("Invalid box")
                args.extend(round(n * size) for n, size in zip(box, (WIDTH, HEIGHT, WIDTH, HEIGHT)))
            if action in {"stop", "follow", "home"} and self.patrol:
                self._stop_patrol(self.now(), "Patrol ended.", send_zero=True)
            self.record(self.replay.call(action, self.now(), *args))
            if action in {"select", "teach"} and self.status["accepted"]:
                self._persist(force=True, update_target=True)
            if action == "follow" and self.status["accepted"] and self.video is None:
                self.movement = movement(self.box)
            return self.status


class OriServer(ThreadingHTTPServer):
    def service_actions(self):
        if self.demo.finished.is_set():
            raise RuntimeError("Ori processing worker stopped")
        if self.demo.live_hardware and not self.demo.video.running():
            raise RuntimeError("Ori camera bridge stopped")


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def handle(self):
        try:
            super().handle()
        except (BrokenPipeError, ConnectionResetError, TimeoutError):
            pass

    def log_message(self, *_):
        pass

    def valid_host(self):
        host = self.headers.get("Host")
        if not host:
            return False
        if self.server.wildcard:
            return host.endswith(f":{self.server.server_port}")
        return host == self.server.host

    def role(self):
        auth = self.headers.get("Authorization", "")
        cookie = SimpleCookie()
        try:
            cookie.load(self.headers.get("Cookie", ""))
        except CookieError:
            pass
        if auth.startswith("Bearer ") and self.matches(auth[7:], self.server.token):
            return "owner"
        viewer = cookie.get("ori_viewer")
        if viewer:
            return "viewer" if self.matches(viewer.value, self.server.viewer_token) else None
        if not self.server.token or (not self.server.require_auth and self.client_address[0] in LOOPBACK_HOSTS):
            return "owner"
        owner = cookie.get("ori_owner")
        return "owner" if owner and self.matches(owner.value, self.server.token) else None

    @staticmethod
    def matches(supplied, expected):
        return bool(expected) and secrets.compare_digest(supplied.encode(), expected.encode())

    def pair(self, query):
        for param, token, role in (("token", self.server.token, "owner"),
                                   ("viewer", self.server.viewer_token, "viewer")):
            if param not in query:
                continue
            if not self.matches(query[param][0], token):
                self.send_error(401)
                return True
            secure = "; Secure" if self.server.tls else ""
            cookie = f"ori_{role}={token}; Path=/; HttpOnly; SameSite=Strict{secure}"
            other = "viewer" if role == "owner" else "owner"
            self.response(303, b"", "text/plain; charset=utf-8", [
                ("Location", "/"),
                ("Referrer-Policy", "no-referrer"),
                ("Set-Cookie", cookie),
                ("Set-Cookie", f"ori_{other}=; Path=/; Max-Age=0; HttpOnly; SameSite=Strict{secure}"),
            ])
            return True
        return False

    def response(self, status, data, content_type, headers=()):
        self.send_response(status)
        if status != 204:
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("Cross-Origin-Resource-Policy", "same-origin")
        self.send_header("X-Frame-Options", "DENY")
        for name, value in headers:
            self.send_header(name, value)
        try:
            self.end_headers()
            if data:
                self.wfile.write(data)
        except (BrokenPipeError, ConnectionResetError):
            # A closed tab can cancel a request while it waits for a new frame.
            self.close_connection = True

    def do_GET(self):
        if not self.valid_host():
            self.send_error(403)
            return
        parsed = urlsplit(self.path)
        path = parsed.path
        if path == "/" and self.pair(parse_qs(parsed.query, keep_blank_values=True)):
            return
        role = self.role()
        if not role:
            self.send_error(401)
            return
        if path == "/":
            self.response(200, PAGE.read_bytes(), "text/html; charset=utf-8")
        elif path[1:] in APP_ASSETS:
            asset = path[1:]
            self.response(200, PAGE.with_name(asset).read_bytes(), APP_ASSETS[asset])
        elif path == "/api/frame":
            demo = self.server.demo
            previous = self.headers.get("If-None-Match")
            previous_status = self.headers.get("X-Ori-Status-Tag")
            with demo.new_frame:
                def tag():
                    return f'"{demo.boot}-{demo.seq}"'

                def status_tag():
                    return f"{demo.boot}-{demo.revision}"

                def image_ready():
                    return demo.seq > 0 and previous != tag() and demo.now() - demo.frame_received < 750

                demo.new_frame.wait_for(lambda: image_ready() or previous_status != status_tag(), timeout=0.4)
                send_image = image_ready()
                frame_id, frame, status = demo.seq, demo.image, demo.status
                etag, revision = tag(), status_tag()
                age = demo.now() - demo.frame_received
                processing_ms = demo.frame_processing_ms
            if role == "viewer" and self.role() != "viewer":
                self.send_error(401)
                return
            headers = [
                ("X-Ori-Status-Tag", revision), ("X-Ori-Role", role),
                ("X-Ori-Can-Share", "1" if role == "owner" and self.server.token else "0"),
                ("X-Ori-Status", json.dumps(status, separators=(",", ":"))),
            ]
            if send_image:
                headers.extend([
                    ("ETag", etag), ("X-Frame-Id", str(frame_id)),
                    ("X-Ori-Frame-Age-Ms", str(age)),
                    ("X-Ori-Frame-Processing-Ms", str(processing_ms)),
                ])
            self.response(200 if send_image else 204, frame if send_image else b"", "image/png", headers)
        else:
            self.send_error(404)

    def do_POST(self):
        if not self.valid_host():
            self.send_error(403)
            return
        scheme = "https" if self.server.tls else "http"
        host = self.headers.get("Host", "")
        if self.headers.get("Origin") != f"{scheme}://{host}":
            self.send_error(403)
            return
        role = self.role()
        if role != "owner":
            self.send_error(403 if role else 401)
            return
        path = urlsplit(self.path).path
        action = path.removeprefix("/api/")
        if action not in ACTIONS | {"share-view"} or path != f"/api/{action}":
            self.send_error(404)
            return
        try:
            length = int(self.headers.get("Content-Length", "0"))
            if not 0 < length <= 32768 or self.headers.get_content_type() != "application/json":
                raise ValueError("Invalid request")
            payload = json.loads(self.rfile.read(length))
            if not isinstance(payload, dict):
                raise ValueError("Invalid request")
            if action == "share-view":
                if not self.server.token or payload.get("mode") not in {"create", "revoke"}:
                    raise ValueError("Invalid sharing request")
                with self.server.demo.lock:
                    token = secrets.token_urlsafe(24) if payload["mode"] == "create" else ""
                    self.server.viewer_token = token
                status = {"url": f"/?viewer={token}" if token else ""}
            else:
                status = self.server.demo.action(action, payload)
        except (KeyError, ValueError, TypeError, OverflowError):
            self.send_error(400)
            return
        self.response(200, json.dumps(status).encode(), "application/json")


def format_host(bind, port):
    if ":" in bind and not bind.startswith("["):
        return f"[{bind}]:{port}"
    return f"{bind}:{port}"


def load_memory(replay, mem_file, live=False, smart=False, now=None):
    ts = Demo.now() if now is None else now
    status = None
    if smart or mem_file:
        status = replay.call("policy", ts, 1, int(bool(smart)), 0, int(bool(smart)), 0, 0, 0)
    path = Path(mem_file) if mem_file else None
    if path and path.exists():
        try:
            saved = json.loads(path.read_text(encoding="utf-8"))
            p = dict(saved.get("policy") or {})
            if p:
                p["auto_spot"] = False
                if live:
                    p["auto_dock_disconnect"] = False
                    p["auto_dock_idle"] = False
                keys = ("auto_reacquire", "auto_gap", "auto_spot", "base_watch",
                        "auto_dock_low_battery", "auto_dock_idle", "auto_dock_disconnect")
                flags = [1 if p.get(k) else 0 for k in keys]
                status = replay.call("policy", ts, *flags, max(1000, int(p.get("idle_timeout_ms", 90000))))
            blob = saved.get("memory_blob", "")
            if blob:
                arm_watch = 0 if live else int(p.get("auto_reacquire", True))
                status = replay.call("import-memory", ts, arm_watch, blob)
        except Exception:
            pass
    return status


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--bind", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--token", default="")
    parser.add_argument("--token-file", type=Path, help="Read the owner token from a private file")
    parser.add_argument("--require-auth", action="store_true")
    parser.add_argument("--tls-cert", default="")
    parser.add_argument("--tls-key", default="")
    parser.add_argument("--video", default="")
    parser.add_argument("--kalay-cmd", default="")
    parser.add_argument("--enable-follow", action="store_true")
    parser.add_argument("--smart", action="store_true")
    parser.add_argument("--memory-file", default="")
    options = parser.parse_args()

    if bool(options.tls_cert) != bool(options.tls_key):
        raise SystemExit("Provide both --tls-cert and --tls-key to enable HTTPS")
    if options.video and options.kalay_cmd:
        raise SystemExit("Choose either --video or --kalay-cmd, not both")
    if options.bind not in LOOPBACK_HOSTS and not (options.token or options.token_file or options.require_auth):
        raise SystemExit("Refusing non-loopback bind without --require-auth or --token")

    token = options.token or (secrets.token_urlsafe(24) if options.require_auth else "")
    if options.token_file:
        if options.token:
            parser.error("Choose either --token or --token-file")
        try:
            if options.token_file.stat().st_mode & 0o077:
                parser.error("Owner token file must be readable only by its owner")
            token = options.token_file.read_text(encoding="ascii").strip()
        except (OSError, UnicodeError) as error:
            parser.error(f"Could not read owner token file: {error.__class__.__name__}")
        if not re.fullmatch(r"[A-Za-z0-9_-]{24,128}", token):
            parser.error("Owner token file must contain one URL-safe token")
    with ExitStack() as services:
        server = services.enter_context(OriServer((options.bind, options.port), Handler))
        server.tls = bool(options.tls_cert)
        if server.tls:
            context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            context.load_cert_chain(certfile=options.tls_cert, keyfile=options.tls_key)
            server.socket = context.wrap_socket(server.socket, server_side=True)
        server.token = token
        server.viewer_token = ""
        server.require_auth = bool(options.require_auth)
        server.wildcard = options.bind in {"0.0.0.0", "::"}
        display_bind = "127.0.0.1" if options.bind == "0.0.0.0" else "::1" if options.bind == "::" else options.bind
        server.host = format_host(display_bind, server.server_port)
        if options.kalay_cmd:
            source = KalaySource(options.kalay_cmd)
            services.callback(source.close)
            bridge_args = ["--control-fd", str(source.control_fd)]
            if options.enable_follow:
                bridge_args.append("--enable-follow")
            client = ReplayClient(binary=BRIDGE_BINARY, args=bridge_args, pass_fds=(source.control_fd,))
        else:
            source = VideoSource(options.video) if options.video else None
            if source is not None:
                services.callback(source.close)
            client = ReplayClient()
        replay = services.enter_context(client)
        smart = options.smart or bool(options.kalay_cmd)
        mem_file = options.memory_file or (str(Path(__file__).resolve().parents[1] / "private" / "target_memory.json") if options.kalay_cmd else "")
        load_memory(replay, mem_file, live=bool(options.kalay_cmd), smart=smart)
        server.demo = Demo(replay, video=source, synthetic_dock=source is None,
                           live_hardware=bool(options.kalay_cmd), memory_file=mem_file or None)
        if options.kalay_cmd:
            source.bind(server.demo.on_battery, server.demo.on_shim)
        server.demo.capture()
        producer = threading.Thread(target=server.demo.run)
        producer.start()
        scheme = "https" if server.tls else "http"
        label = "Live Ori" if options.kalay_cmd else "Ori replay demo"
        print(f"{label} at {scheme}://{server.host}", flush=True)
        if token and not options.token_file:
            print(f"Owner pairing URL: {scheme}://{server.host}/?token={token}", flush=True)
        signal.signal(signal.SIGTERM, signal.default_int_handler)
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            pass
        finally:
            server.demo.finished.set()
            producer.join()


if __name__ == "__main__":
    main()
