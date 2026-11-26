"""Transport for the desktop simulator and ori-bridge. Output is JSON status; control fd handles hardware."""

import json
import subprocess
from pathlib import Path

BINARY = Path(__file__).resolve().parents[1] / "build" / "ori-replay"
BRIDGE_BINARY = Path(__file__).resolve().parents[1] / "build" / "ori-bridge"


class ReplayClient:
    def __init__(self, binary=BINARY, args=(), pass_fds=()):
        self.binary = Path(binary)
        self.args = list(args)
        self.pass_fds = tuple(pass_fds)

    def __enter__(self):
        self.process = subprocess.Popen(
            [str(self.binary), *self.args],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            pass_fds=self.pass_fds,
        )
        return self

    def __exit__(self, *_):
        try:
            self.process.stdin.close()
            self.process.wait(timeout=2)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait()
        finally:
            self.process.stdout.close()

    def call(self, command, now, *arguments, pixels=b""):
        line = " ".join(map(str, (command, now, *arguments))) + "\n"
        self.process.stdin.write(line.encode() + pixels)
        self.process.stdin.flush()
        reply = self.process.stdout.readline()
        if not reply:
            raise RuntimeError("Replay process ended before replying")
        return json.loads(reply)
