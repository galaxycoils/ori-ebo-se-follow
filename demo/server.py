"""Serve the desktop demo on loopback or authenticated LAN/HTTPS. All tracking runs in the native core."""

import argparse
from http.cookies import SimpleCookie
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import secrets
import shutil
import ssl
import struct
import subprocess
import threading
import time
from urllib.parse import parse_qs, urlsplit
import zlib

from client import ReplayClient
from fixtures import HEIGHT, MOVEMENT, REFERENCE, WIDTH, scene

PAGE = Path(__file__).with_name("index.html")
ACTIONS = {"select", "teach", "follow", "heartbeat", "track", "stop", "move", "home", "lift"}
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
            + chunk(b"IDAT", zlib.compress(rows, level=3))
            + chunk(b"IEND", b""))


def movement(start):
    # Start from the visible pose, including when a second request interrupts a move.
    for end, steps in MOVEMENT:
        for step in range(1, steps + 1):
            t = step / steps
            weight = t * t * (3 - 2 * t)
            yield tuple(round(a + (b - a) * weight) for a, b in zip(start, end))
        start = end


class VideoSource:
    def __init__(self, source):
        ffmpeg = find_ffmpeg()
        if not ffmpeg:
            raise RuntimeError("FFmpeg is required for --video mode")
        command = [ffmpeg, "-nostdin", "-v", "error", "-re"]
        if Path(source).exists():
            command.extend(["-stream_loop", "-1"])
        command.extend([
            "-i", source,
            "-vf", f"fps=10,scale={WIDTH}:{HEIGHT},format=gray",
            "-an", "-f", "rawvideo", "pipe:1",
        ])
        self.process = subprocess.Popen(command, stdout=subprocess.PIPE)

    def read(self):
        data = self.process.stdout.read(FRAME_BYTES)
        if len(data) != FRAME_BYTES:
            raise RuntimeError("Video stream ended")
        return data

    def close(self):
        if self.process.stdout:
            self.process.stdout.close()
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()


class Demo:
    def __init__(self, replay, video=None):
        self.replay = replay
        self.video = video
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
        self.status = {**status, "boot": self.boot, "revision": self.revision}

    def capture(self):
        if self.video is not None:
            pixels = self.video.read()
            image = png(pixels)
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
            # These are simulated factory telemetry samples, never wheel commands.
            elapsed = 0 if self.return_started is None else now - self.return_started
            signal = "idle"
            if self.return_started is not None:
                signal = "returning" if elapsed < 1200 else "contact" if elapsed < 1800 else "charging"
            self.record(self.replay.call("dock-status", now, signal))
            self.seq += 1
            self.record(self.replay.call("frame", now, self.seq, now, pixels=pixels))
            self.image = image
            self.new_frame.notify_all()

    def run(self):
        deadline = time.monotonic() + FRAME_INTERVAL
        while not self.finished.wait(max(0, deadline - time.monotonic())):
            try:
                self.capture()
            except RuntimeError:
                self.finished.set()
                break
            deadline += FRAME_INTERVAL
            if deadline < time.monotonic():
                deadline = time.monotonic() + FRAME_INTERVAL

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
            if action == "follow" and self.status["accepted"]:
                self.movement = movement(self.box)
            return self.status


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def valid_host(self):
        host = self.headers.get("Host")
        if not host:
            return False
        if self.server.wildcard:
            return host.endswith(f":{self.server.server_port}")
        return host == self.server.host

    def authenticated(self):
        token = self.server.token
        if not token:
            return True
        auth = self.headers.get("Authorization", "")
        if auth.startswith("Bearer ") and secrets.compare_digest(auth[7:], token):
            return True
        raw_cookie = self.headers.get("Cookie", "")
        if raw_cookie:
            cookie = SimpleCookie()
            try:
                cookie.load(raw_cookie)
            except Exception:
                return False
            morsel = cookie.get("ori_owner")
            if morsel and secrets.compare_digest(morsel.value, token):
                return True
        return False

    def response(self, status, data, content_type, headers=()):
        self.send_response(status)
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
        if path == "/" and self.server.token:
            query = parse_qs(parsed.query)
            if "token" in query:
                supplied = query["token"][0]
                if secrets.compare_digest(supplied, self.server.token):
                    secure = "; Secure" if self.server.tls else ""
                    cookie = f"ori_owner={self.server.token}; Path=/; HttpOnly; SameSite=Strict{secure}"
                    self.response(303, b"", "text/plain; charset=utf-8", [
                        ("Location", "/"),
                        ("Set-Cookie", cookie),
                    ])
                else:
                    self.send_error(401)
                return
        if not self.authenticated():
            self.send_error(401)
            return
        if path == "/":
            self.response(200, PAGE.read_bytes(), "text/html; charset=utf-8")
        elif path == "/api/frame":
            demo = self.server.demo
            previous = self.headers.get("If-None-Match")
            with demo.new_frame:
                def tag():
                    return f'"{demo.boot}-{demo.seq}"'

                demo.new_frame.wait_for(lambda: previous != tag(), timeout=0.4)
                etag = tag()
                frame_id, frame, status = demo.seq, demo.image, demo.status
            if previous == etag:
                self.response(204, b"", "image/png")
            else:
                self.response(200, frame, "image/png", [
                    ("ETag", etag), ("X-Frame-Id", str(frame_id)),
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
        if not self.authenticated():
            self.send_error(401)
            return
        path = urlsplit(self.path).path
        action = path.removeprefix("/api/")
        if action not in ACTIONS or path != f"/api/{action}":
            self.send_error(404)
            return
        try:
            length = int(self.headers.get("Content-Length", "0"))
            if not 0 < length <= 2048 or self.headers.get_content_type() != "application/json":
                raise ValueError("Invalid request")
            status = self.server.demo.action(action, json.loads(self.rfile.read(length)))
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
    options = parser.parse_args()

    if bool(options.tls_cert) != bool(options.tls_key):
        raise SystemExit("Provide both --tls-cert and --tls-key to enable HTTPS")
    if options.bind not in LOOPBACK_HOSTS and not (options.token or options.require_auth):
        raise SystemExit("Refusing non-loopback bind without --require-auth or --token")

    token = options.token or (secrets.token_urlsafe(24) if options.require_auth else "")
    video = VideoSource(options.video) if options.video else None
    try:
        with ReplayClient() as replay, ThreadingHTTPServer((options.bind, options.port), Handler) as server:
            server.tls = bool(options.tls_cert)
            if server.tls:
                context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
                context.load_cert_chain(certfile=options.tls_cert, keyfile=options.tls_key)
                server.socket = context.wrap_socket(server.socket, server_side=True)
            server.token = token
            server.wildcard = options.bind in {"0.0.0.0", "::"}
            display_bind = "127.0.0.1" if options.bind == "0.0.0.0" else "::1" if options.bind == "::" else options.bind
            server.host = format_host(display_bind, server.server_port)
            server.demo = Demo(replay, video=video)
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
        if video is not None:
            video.close()


if __name__ == "__main__":
    main()
