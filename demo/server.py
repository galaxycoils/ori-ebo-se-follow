"""Serve the desktop demo on loopback. All tracking runs in the native core."""

import argparse
import json
import secrets
import struct
import threading
import time
import zlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlsplit

from client import ReplayClient
from fixtures import HEIGHT, REFERENCE, WIDTH, retreat_steps, scene

PAGE = Path(__file__).with_name("index.html")
ACTIONS = {"select", "teach", "follow", "heartbeat", "track", "stop", "move"}
FRAME_INTERVAL = 0.1


def png(pixels):
    def chunk(kind, data):
        return (struct.pack(">I", len(data)) + kind + data
                + struct.pack(">I", zlib.crc32(kind + data)))

    rows = b"".join(b"\0" + pixels[i:i + WIDTH] for i in range(0, len(pixels), WIDTH))
    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", WIDTH, HEIGHT, 8, 0, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(rows, level=3))
            + chunk(b"IEND", b""))


def movement():
    for x in range(113, 70, -7):
        yield (x, 65, 80, 80)
    for x in range(71, 121, 7):
        yield (x, 65, 80, 80)
    for box in retreat_steps():
        yield box
        yield box


class Demo:
    def __init__(self, replay):
        self.replay = replay
        self.lock = threading.Lock()
        self.finished = threading.Event()
        self.seq = 0
        self.boot = secrets.token_hex(16)
        self.revision = 0
        self.image = b""
        self.status = {}
        self.movement = iter(())

    @staticmethod
    def now():
        return time.monotonic_ns() // 1_000_000

    def record(self, status):
        self.revision += 1
        self.status = {**status, "boot": self.boot, "revision": self.revision}

    def capture(self):
        with self.lock:
            pixels = scene(next(self.movement, REFERENCE))
            now = self.now()
            self.seq += 1
            self.record(self.replay.call("frame", now, self.seq, now, pixels=pixels))
            self.image = png(bytes(pixels))

    def run(self):
        while not self.finished.wait(FRAME_INTERVAL):
            self.capture()

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
            if action == "move":
                if self.status["owner"] != driver or self.status["generation"] != generation:
                    return {**self.status, "accepted": False}
                self.movement = movement()
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
                self.movement = movement()
            return self.status


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

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
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        if self.headers.get("Host") != self.server.host:
            self.send_error(403)
            return
        path = urlsplit(self.path).path
        if path == "/":
            self.response(200, PAGE.read_bytes(), "text/html; charset=utf-8")
        elif path == "/api/frame":
            with self.server.demo.lock:
                frame = self.server.demo.image
                frame_id = self.server.demo.seq
                status = json.dumps(self.server.demo.status, separators=(",", ":"))
            self.response(200, frame, "image/png", [("X-Frame-Id", str(frame_id)), ("X-Ori-Status", status)])
        else:
            self.send_error(404)

    def do_POST(self):
        if (self.headers.get("Host") != self.server.host
                or self.headers.get("Origin") != f"http://{self.server.host}"):
            self.send_error(403)
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


def main():
    args = argparse.ArgumentParser()
    args.add_argument("--port", type=int, default=8765)
    options = args.parse_args()
    with ReplayClient() as replay, ThreadingHTTPServer(("127.0.0.1", options.port), Handler) as server:
        server.host = f"127.0.0.1:{server.server_port}"
        server.demo = Demo(replay)
        server.demo.capture()
        producer = threading.Thread(target=server.demo.run)
        producer.start()
        print(f"Ori replay demo at http://{server.host}", flush=True)
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            pass
        finally:
            server.demo.finished.set()
            producer.join()


if __name__ == "__main__":
    main()
