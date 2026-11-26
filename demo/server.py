"""Serve the desktop demo on loopback or authenticated LAN/HTTPS. All tracking runs in the native core."""

import argparse
from http.cookies import CookieError, SimpleCookie
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import secrets
import shlex
import shutil
import ssl
import struct
import subprocess
import threading
import time
from urllib.parse import parse_qs, urlsplit
import zlib

from client import BRIDGE_BINARY, ReplayClient
from fixtures import HEIGHT, MOVEMENT, REFERENCE, WIDTH, scene

PAGE = Path(__file__).with_name("index.html")
APP_ASSETS = {"companion.js": "text/javascript", "sw.js": "text/javascript",
              "manifest.webmanifest": "application/manifest+json",
              "icon-192.png": "image/png", "icon-512.png": "image/png"}
ACTIONS = {"select", "teach", "follow", "heartbeat", "track", "stop", "move", "home", "lift",
           "resume", "forget", "spot", "policy", "manual", "motors"}
FRAME_INTERVAL = 0.1
FRAME_BYTES = WIDTH * HEIGHT
LOOPBACK_HOSTS = {"127.0.0.1", "localhost", "::1"}


def find_ffmpeg():
    for candidate in (os.environ.get("FFMPEG"), shutil.which("ffmpeg")):
        if candidate and os.path.isfile(candidate) and os.access(candidate, os.X_OK):
            return candidate
    return None


def png(pixels):
    def chunk(kind, data):
        return (struct.pack(">I", len(data)) + kind + data
                + struct.pack(">I", zlib.crc32(kind + data)))

    view = memoryview(pixels)
    stride = WIDTH + 1
    rows = bytearray(HEIGHT * stride)
    for row in range(HEIGHT):
        src = row * WIDTH
        dst = row * stride + 1
        rows[dst:dst + WIDTH] = view[src:src + WIDTH]
    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", WIDTH, HEIGHT, 8, 0, 0, 0, 0))
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


def _parse_battery_status(data):
    latest = None
    i = 0
    n = len(data)
    while i + 8 <= n:
        if data[i] == 0xFE:
            plen = data[i + 1]
            msgid = data[i + 5]
            if msgid == 207 and i + 24 <= n and i + 8 + plen <= n:
                latest = (data[i + 22], data[i + 23])
            i += (8 + plen) if (plen > 0 and i + 8 + plen <= n) else 1
        else:
            i += 1
    return latest


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
            "-vf", f"fps=10,scale={WIDTH}:{HEIGHT},format=gray",
            "-an", "-f", "rawvideo", "pipe:1",
        ])
        self.process = subprocess.Popen(command, stdout=subprocess.PIPE)
        self.frame_lock = threading.Lock()
        self.latest = (None, 0)
        self.reader = threading.Thread(target=self.read_frames, daemon=True)
        self.reader.start()

    def read_frames(self):
        while True:
            data = self.process.stdout.read(FRAME_BYTES)
            if len(data) != FRAME_BYTES:
                return
            with self.frame_lock:
                self.latest = (data, time.monotonic_ns() // 1_000_000)

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
    def __init__(self, cmd):
        self.ffmpeg = find_ffmpeg()
        if not self.ffmpeg:
            raise RuntimeError("FFmpeg is required for --kalay-cmd mode")
        self.shim = subprocess.Popen(
            shlex.split(cmd), stdin=subprocess.PIPE, stdout=subprocess.PIPE
        )
        self.control_fd = self.shim.stdin.fileno()
        self.ff = None
        self._frame_lock = threading.Lock()
        self._latest_pixels = None
        self._latest_mono = 0
        self._primed = False
        self._closed = False
        self.on_battery = None
        self._demux_thread = threading.Thread(target=self._demux, daemon=True)
        self._demux_thread.start()

    def _close_ff(self):
        proc = self.ff
        self.ff = None
        if proc is None:
            return
        for pipe in (proc.stdin, proc.stdout):
            if pipe:
                try:
                    pipe.close()
                except Exception:
                    pass
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=1)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()

    def _reader_loop(self, proc):
        while not self._closed:
            data = proc.stdout.read(FRAME_BYTES)
            if len(data) != FRAME_BYTES:
                break
            with self._frame_lock:
                self._latest_pixels = data
                self._latest_mono = time.monotonic_ns() // 1_000_000

    def _start_decoder(self, codec):
        fmt = "hevc" if codec == 80 else "h264"
        self._close_ff()
        self.ff = subprocess.Popen([
            self.ffmpeg, "-nostdin", "-v", "error",
            "-threads", "2", "-thread_type", "slice",
            "-fflags", "nobuffer+discardcorrupt", "-flags", "low_delay", "-flags2", "fast",
            "-probesize", "4096", "-analyzeduration", "0",
            "-f", fmt, "-i", "pipe:0",
            "-vf", f"scale={WIDTH}:{HEIGHT},format=gray",
            "-an", "-f", "rawvideo", "pipe:1",
        ], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
        threading.Thread(target=self._reader_loop, args=(self.ff,), daemon=True).start()

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
                continue
            if kind == 0xFF:
                telemetry = _parse_battery_status(payload)
                if telemetry is not None and self.on_battery is not None:
                    self.on_battery(*telemetry)
                continue
            if kind not in {78, 80}:
                continue
            if not self._primed:
                if not _has_param_set(payload, kind):
                    continue
                self._primed = True
                self._start_decoder(kind)
            try:
                self.ff.stdin.write(payload)
                self.ff.stdin.flush()
            except Exception:
                self._primed = False
                self._close_ff()

    def read_latest(self):
        with self._frame_lock:
            return self._latest_pixels, self._latest_mono

    def close(self):
        self._closed = True
        self._close_ff()
        if self.shim is not None:
            for pipe in (self.shim.stdin, self.shim.stdout):
                if pipe:
                    try:
                        pipe.close()
                    except Exception:
                        pass
            if self.shim.poll() is None:
                self.shim.terminate()
                try:
                    self.shim.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    self.shim.kill()
                    self.shim.wait()


class Demo:
    def __init__(self, replay, video=None, synthetic_dock=True, live_hardware=False, memory_file=None):
        self.replay = replay
        self.video = video
        self.synthetic_dock = synthetic_dock
        self.live_hardware = live_hardware
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
        self.frame_received = 0
        self.manual_driver = None
        self._persisted_key = None

    @staticmethod
    def now():
        return time.monotonic_ns() // 1_000_000

    def record(self, status):
        self.revision += 1
        action = status["dock_action"]
        if action == "start":
            self.return_started = self.now()
        elif action == "cancel":
            self.return_started = None
        self.status = {**status, "boot": self.boot, "revision": self.revision, "live_hardware": self.live_hardware}
        if not self.status.get("manual_active"):
            self.manual_driver = None

    def _persist(self, force=False):
        if not self.memory_file:
            return
        mem = self.status.get("memory", "none")
        key = (mem, self.status.get("state"), tuple(sorted((self.status.get("policy") or {}).items())))
        if not force and key == self._persisted_key:
            return
        blob = ""
        if mem != "none":
            exported = self.replay.call("export-memory", self.now())
            blob = exported.get("memory_blob", "")
        self.memory_file.parent.mkdir(parents=True, exist_ok=True)
        tmp = self.memory_file.with_suffix(".tmp")
        tmp.write_text(json.dumps({"policy": self.status.get("policy", {}), "memory_blob": blob}), encoding="utf-8")
        os.replace(tmp, self.memory_file)
        self._persisted_key = key

    def on_battery(self, percent, charging):
        if not 0 <= percent <= 100 or charging not in (0, 1):
            return
        with self.lock:
            if self.finished.is_set():
                return
            try:
                self.record(self.replay.call("battery-telemetry", self.now(), percent, charging))
            except Exception:
                return
            self.new_frame.notify_all()

    def capture(self):
        pixels = None
        received = self.now()
        if self.video is not None:
            latest, received = self.video.read_latest()
            if latest is not None and received > self.frame_received and self.now() - received < 250:
                pixels = latest
                if pixels == self._cached_pixels:
                    image = self._cached_png
                else:
                    image = png(pixels)
                    self._cached_pixels = pixels
                    self._cached_png = image
        else:
            with self.lock:
                box = next(self.movement, REFERENCE)
                self.box = box
                same_scene = box == self._cached_box
                if same_scene:
                    pixels = self._cached_pixels
                    image = self._cached_png
            if not same_scene:
                pixels = bytes(scene(box))
                image = png(pixels)
                self._cached_box = box
                self._cached_pixels = pixels
                self._cached_png = image

        with self.lock:
            now = self.now()
            if self.synthetic_dock:
                # These are simulated factory telemetry samples, never wheel commands.
                elapsed = 0 if self.return_started is None else now - self.return_started
                signal = "idle"
                if self.return_started is not None:
                    signal = "returning" if elapsed < 1200 else "contact" if elapsed < 1800 else "charging"
                self.record(self.replay.call("dock-status", now, signal))
            if pixels is None or now - received >= 250:
                self.record(self.replay.call("tick", now))
                self.new_frame.notify_all()
                return
            self.seq += 1
            self.record(self.replay.call("frame", now, self.seq, received, pixels=pixels))
            self.frame_received = received
            if self.memory_file and self.status.get("state") in {"TRACKING", "READY"}:
                self._persist()
            self.image = image
            self.new_frame.notify_all()

    def run(self):
        deadline = time.monotonic() + FRAME_INTERVAL
        while not self.finished.is_set():
            if self.finished.wait(max(0, deadline - time.monotonic())):
                break
            try:
                self.capture()
            except RuntimeError:
                self.finished.set()
                break
            deadline = max(deadline + FRAME_INTERVAL, time.monotonic())

    def action(self, action, payload):
        driver = payload["driver"]
        generation = payload["generation"]
        if (type(driver) is not int or not 0 < driver < 2**53
                or type(generation) is not int or not 0 < generation < 2**53):
            raise ValueError("Invalid session")
        with self.lock:
            if payload["boot"] != self.boot:
                return {**self.status, "accepted": False}
            args = [driver, generation]
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
                if not (-1.0 <= ly <= 1.0 and -1.0 <= rx <= 1.0 and 50 <= ttl <= 400):
                    raise ValueError("Invalid manual drive")
                if self.manual_driver not in (None, driver):
                    return {**self.status, "accepted": False}
                if (ly or rx) and (generation != self.status["generation"] or
                                   (self.status["owner"] not in (0, driver) and not self.status["autonomous"])):
                    return {**self.status, "accepted": False}
                self.record(self.replay.call("manual-drive", self.now(), driver, round(ly, 3), round(rx, 3), ttl))
                if self.status.get("manual_active"):
                    self.manual_driver = driver
                if ly == 0 and rx == 0:
                    self._persist()
                return self.status
            if action == "motors":
                enabled = bool(payload["enabled"])
                if enabled and generation != self.status["generation"]:
                    return {**self.status, "accepted": False}
                fwd = float(payload.get("forward_speed", 0.30))
                turn = float(payload.get("turn_speed", 0.22))
                inv = 1 if payload.get("invert_turn") else 0
                if not (0.1 <= fwd <= 1.0 and 0.1 <= turn <= 1.0):
                    raise ValueError("Invalid speed")
                self.record(self.replay.call("set-motors", self.now(), 1 if enabled else 0, round(fwd, 3), round(turn, 3), inv))
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
                self._persist(force=True)
                return self.status
            if action in {"resume", "spot"}:
                cmd = "resume-memory" if action == "resume" else "spot-lock"
                self.record(self.replay.call(cmd, self.now(), *args))
                return self.status
            if action in {"select", "heartbeat"}:
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
            self.record(self.replay.call(action, self.now(), *args))
            if action in {"select", "teach"} and self.status["accepted"]:
                self._persist(force=True)
            if action == "follow" and self.status["accepted"]:
                self.movement = movement(self.box)
            return self.status


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
            with demo.new_frame:
                def tag():
                    return f'"{demo.boot}-{demo.seq}"'

                demo.new_frame.wait_for(lambda: demo.seq > 0 and previous != tag(), timeout=0.4)
                etag = tag()
                frame_id, frame, status = demo.seq, demo.image, demo.status
                age = demo.now() - demo.frame_received
            if role == "viewer" and self.role() != "viewer":
                self.send_error(401)
                return
            if not frame_id or previous == etag or age >= 750:
                self.response(204, b"", "image/png")
            else:
                self.response(200, frame, "image/png", [
                    ("ETag", etag), ("X-Frame-Id", str(frame_id)),
                    ("X-Ori-Frame-Age-Ms", str(age)),
                    ("X-Ori-Role", role),
                    ("X-Ori-Can-Share", "1" if role == "owner" and self.server.token else "0"),
                    ("X-Ori-Status", json.dumps(status, separators=(",", ":"))),
                ])
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
            if not 0 < length <= 2048 or self.headers.get_content_type() != "application/json":
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


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--bind", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--token", default="")
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
    if options.bind not in LOOPBACK_HOSTS and not (options.token or options.require_auth):
        raise SystemExit("Refusing non-loopback bind without --require-auth or --token")

    token = options.token or (secrets.token_urlsafe(24) if options.require_auth else "")
    source = KalaySource(options.kalay_cmd) if options.kalay_cmd else (VideoSource(options.video) if options.video else None)
    if options.kalay_cmd:
        bridge_args = ["--control-fd", str(source.control_fd)]
        if options.enable_follow:
            bridge_args.append("--enable-follow")
        client = ReplayClient(binary=BRIDGE_BINARY, args=bridge_args, pass_fds=(source.control_fd,))
    else:
        client = ReplayClient()

    try:
        with client as replay, ThreadingHTTPServer((options.bind, options.port), Handler) as server:
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
            smart = options.smart or bool(options.kalay_cmd)
            mem_file = options.memory_file or (str(Path(__file__).resolve().parents[1] / "private" / "target_memory.json") if options.kalay_cmd else "")
            if smart or mem_file:
                replay.call("policy", Demo.now(), 1, int(smart), 0, int(smart), 0, 0, 0)
            if mem_file and Path(mem_file).exists():
                try:
                    saved = json.loads(Path(mem_file).read_text(encoding="utf-8"))
                    p = saved.get("policy") or {}
                    if p:
                        keys = ("auto_reacquire", "auto_gap", "auto_spot", "base_watch",
                                "auto_dock_low_battery", "auto_dock_idle", "auto_dock_disconnect")
                        flags = [1 if p.get(k) else 0 for k in keys]
                        replay.call("policy", Demo.now(), *flags, max(1000, int(p.get("idle_timeout_ms", 90000))))
                    blob = saved.get("memory_blob", "")
                    if blob:
                        replay.call("import-memory", Demo.now(), int(p.get("auto_reacquire", True)), blob)
                except Exception:
                    pass
            server.demo = Demo(replay, video=source, synthetic_dock=not bool(options.kalay_cmd),
                               live_hardware=bool(options.kalay_cmd), memory_file=mem_file or None)
            if options.kalay_cmd:
                source.on_battery = server.demo.on_battery
            server.demo.capture()
            producer = threading.Thread(target=server.demo.run)
            producer.start()
            scheme = "https" if server.tls else "http"
            print(f"Ori replay demo at {scheme}://{server.host}", flush=True)
            if token:
                print(f"Owner pairing URL: {scheme}://{server.host}/?token={token}", flush=True)
            try:
                server.serve_forever()
            except KeyboardInterrupt:
                pass
            finally:
                server.demo.finished.set()
                producer.join()
    finally:
        if source is not None:
            source.close()


if __name__ == "__main__":
    main()
