"""Transport for the desktop simulator. Its output is a trace, never motor I/O."""

import json
import subprocess
from pathlib import Path

BINARY = Path(__file__).resolve().parents[1] / "build" / "ori-replay"


class ReplayClient:
    def __enter__(self):
        self.process = subprocess.Popen(
            [BINARY], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
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
