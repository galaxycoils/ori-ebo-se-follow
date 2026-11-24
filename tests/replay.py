"""Exercise tracking, steering and session behavior through the replay program."""

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "demo"))
from client import ReplayClient
from fixtures import REFERENCE, retreat_steps, scene

OWNER = 1


class Scenario:
    def __init__(self, replay):
        self.replay = replay
        self.now = 0
        self.seq = 0
        self.status = {"generation": 1}

    def run(self, command, *arguments, pixels=b""):
        self.status = self.replay.call(command, self.now, *arguments, pixels=pixels)
        return self.status

    def control(self, command, *args, owner=OWNER, generation=None):
        if generation is None:
            generation = self.status["generation"]
        return self.run(command, owner, generation, *args)

    def frame(self, *boxes, interval=100, ack=False):
        self.now += interval
        self.seq += 1
        status = self.run("frame", self.seq, self.now, pixels=scene(*boxes))
        if ack:
            self.control("heartbeat", self.seq)
        return status

    def ready(self):
        self.frame(REFERENCE)
        assert self.control("select", self.seq, *REFERENCE)["state"] == "TRACKING"
        self.frame(REFERENCE, ack=True)
        self.frame(REFERENCE, ack=True)
        assert self.control("teach")["state"] == "READY"

    def following(self):
        self.ready()
        assert self.control("follow")["state"] == "FOLLOWING"


def retreat(s, track_only=False):
    s.following()
    if track_only:
        assert s.control("track")["state"] == "READY"
    result = [s.frame(box, ack=True) for box in retreat_steps()]
    motions = list(dict.fromkeys(
        (index,) for index, row in enumerate(result)
        if index == 0 or row["motion"] != result[index - 1]["motion"]
    ))
    assert [result[i]["motion"] for (i,) in motions] == (
        ["HOLD"] if track_only else ["HOLD", "FORWARD", "HOLD"]
    ), result
    return result


def test_frozen_preview(s):
    s.following()
    last_displayed = s.seq
    for now in range(8):
        status = s.frame(REFERENCE)
        if now < 7:
            s.control("heartbeat", last_displayed)
    assert status["reason"] == "preview-expired" and status["motion"] == "HOLD"


def test_disconnect(s):
    s.following()
    for _ in range(8):
        status = s.frame(REFERENCE)
    assert status["reason"] == "heartbeat-expired" and status["state"] == "OFF"


def test_camera(s):
    s.following()
    s.now += 250
    status = s.run("tick")
    assert status["reason"] == "camera-stale" and status["state"] == "LOST"


def test_invalid_frames(s, invalidity):
    s.following()
    s.now += 100
    candidate = {
        "duplicate": (s.seq, s.now),
        "stale": (s.seq + 1, s.now - 250),
        "future": (s.seq + 1, s.now + 1),
        "out-of-order": (s.seq - 1, s.now),
    }[invalidity]
    status = s.run("frame", *candidate, pixels=scene(REFERENCE))
    assert status["reason"] == "invalid-frame" and status["motion"] == "HOLD"


def test_ownership(s):
    s.ready()
    generation = s.status["generation"]
    assert not s.control("follow", owner=2)["accepted"]
    assert s.control("stop")["state"] == "OFF"
    assert not s.control("follow", generation=generation)["accepted"]


def test_interrupt(s, command, reason):
    s.following()
    old_generation = s.status["generation"]
    status = s.run(command)
    assert status["state"] == "OFF" and status["reason"] == reason
    assert not s.control("follow", generation=old_generation)["accepted"]


def test_vision(s, kind):
    s.following()
    if kind == "lost":
        status = s.frame()
    else:
        s.frame((120, 65, 72, 72), ack=True)
        s.frame((120, 65, 64, 64), ack=True)
        s.frame((120, 65, 58, 58), ack=True)
        s.frame((120, 65, 52, 52), ack=True)
        s.frame((120, 65, 47, 47), ack=True)
        status = s.frame((96, 65, 45, 45), (145, 65, 45, 45))
    assert status["state"] == "LOST" and status["motion"] == "HOLD", status
    assert not s.control("follow")["accepted"]


def test_turn(s):
    s.following()
    output = []
    for x in (113, 106, 99, 92, 85, 78, 71):
        output.append(s.frame((x, 65, 80, 80), ack=True)["motion"])
    assert "LEFT" in output and "FORWARD" not in output, output


def test_motion_expiry(s):
    s.following()
    for box in list(retreat_steps())[:4]:
        status = s.frame(box, ack=True)
    assert status["motion"] == "FORWARD"
    assert status["expires_ms"] == s.now + 250
    s.now += 250
    assert s.run("tick")["motion"] == "HOLD"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--case", choices=["retreat"])
    args = parser.parse_args()
    if args.case:
        with ReplayClient() as client:
            result = retreat(Scenario(client))
        for status in result:
            print(json.dumps(status))
        print("HOLD → FORWARD → HOLD")
        return

    tests = [
        retreat, lambda s: retreat(s, track_only=True),
        test_camera, test_disconnect, test_frozen_preview, test_ownership,
        test_turn, test_motion_expiry,
    ]
    tests += [lambda s, kind=kind: test_invalid_frames(s, kind)
              for kind in ("duplicate", "stale", "future", "out-of-order")]
    tests += [lambda s, kind=kind: test_vision(s, kind) for kind in ("lost", "ambiguity")]
    tests += [lambda s, command=command, reason=reason: test_interrupt(s, command, reason)
              for command, reason in [("manual", "manual-override"), ("dock", "docking"),
                                      ("sleep", "sleep"), ("veto", "factory-veto"),
                                      ("battery", "low-battery")]]
    for test in tests:
        with ReplayClient() as client:
            test(Scenario(client))
    print(f"{len(tests)} replay scenarios passed")


if __name__ == "__main__":
    main()
