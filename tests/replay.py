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
    transitions = [row["motion"] for index, row in enumerate(result)
                   if index == 0 or row["motion"] != result[index - 1]["motion"]]
    assert transitions == (
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


def test_stale_calibration(s):
    s.ready()
    old_generation = s.status["generation"]
    s.now += 250
    assert s.run("tick")["state"] == "LOST"
    assert not s.control("follow", generation=old_generation)["accepted"]
    assert not s.frame(REFERENCE)["can_teach"]


def test_frozen_selection(s):
    s.ready()
    frozen = s.seq
    for _ in range(9):
        s.frame(REFERENCE)
        assert s.control("heartbeat", frozen)["accepted"]
    assert not s.control("follow")["accepted"]
    assert s.control("select", frozen, *REFERENCE)["accepted"]


def test_overlapping_inhibits(s):
    s.following()
    s.run("dock")
    s.run("battery")
    assert s.control("stop")["state"] == "OFF"
    s.run("dock-clear")
    assert not s.control("select", s.seq, *REFERENCE)["accepted"]
    old = s.status["generation"]
    assert s.run("battery-clear")["state"] == "OFF"
    assert not s.control("select", s.seq, *REFERENCE, generation=old)["accepted"]
    s.ready()
    assert s.control("follow")["accepted"]


def test_track_cancels_pending_follow(s):
    s.ready()
    old = s.status["generation"]
    assert s.control("track")["accepted"]
    assert not s.control("follow", generation=old)["accepted"]
    assert s.frame(REFERENCE)["motion"] == "HOLD"
    before_start = s.status["generation"]
    assert s.control("follow")["accepted"]
    assert not s.control("track", generation=before_start, owner=2)["accepted"]
    assert s.control("track", generation=before_start)["state"] == "READY"


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


def test_selection(s):
    s.frame(REFERENCE)
    frozen = s.seq
    s.frame((127, 65, 80, 80))
    status = s.control("select", frozen, *REFERENCE)
    assert status["accepted"] and abs(status["box"][0] - 127) <= 2
    assert s.control("stop")["motion"] == "HOLD"
    for _ in range(21):
        s.frame(REFERENCE)
    assert not s.control("select", frozen, *REFERENCE)["accepted"]


def test_heartbeat_before_selection(s):
    s.frame(REFERENCE)
    before_selection = s.seq
    s.frame(REFERENCE)
    assert s.control("select", s.seq, *REFERENCE)["accepted"]
    assert not s.control("heartbeat", before_selection)["accepted"]
    assert s.control("heartbeat", s.seq)["accepted"]


def test_track_owner_expiry(s):
    s.ready()
    for _ in range(8):
        status = s.frame(REFERENCE)
    assert status["state"] == "OFF" and status["reason"] == "heartbeat-expired"
    assert s.control("select", s.seq, *REFERENCE, owner=2)["accepted"]


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


def dock_signal(s, signal, interval=100):
    s.now += interval
    return s.run("dock-status", signal)


def test_home(s):
    s.following()
    assert not s.control("home")["accepted"]  # Missing factory telemetry.
    dock_signal(s, "idle")
    assert not s.control("home", owner=2)["accepted"]
    before = s.status["generation"]
    out = s.control("home")
    assert out["accepted"] and out["dock_action"] == "start" and out["motion"] == "HOLD"
    assert not s.control("home", generation=before)["accepted"]
    assert not s.control("select", s.seq, *REFERENCE)["accepted"]
    assert dock_signal(s, "returning")["dock_action"] == "none"
    for _ in range(4):
        assert dock_signal(s, "contact", 500)["dock"] == "checking-charge"
    dock_signal(s, "charging")
    dock_signal(s, "contact")  # Contact bounce resets confirmation time.
    dock_signal(s, "charging")
    assert dock_signal(s, "charging", 1000)["dock"] == "checking-charge"
    assert dock_signal(s, "charging", 500)["dock"] == "charging"
    assert not s.control("home")["accepted"]
    assert dock_signal(s, "idle")["dock"] == "idle"
    assert s.status["state"] == "OFF"


def test_home_cancel(s):
    dock_signal(s, "idle")
    s.control("home")
    assert not s.control("stop", owner=2)["accepted"]
    assert s.control("stop")["dock_action"] == "cancel"
    assert dock_signal(s, "returning")["dock"] == "stopping"
    assert dock_signal(s, "idle")["can_home"]


def test_home_no_ack(s):
    dock_signal(s, "idle")
    s.control("home")
    actions = [dock_signal(s, "idle", 1000)["dock_action"] for _ in range(5)]
    assert actions == ["none", "none", "cancel", "none", "none"]
    assert s.status["dock"] == "failed" and s.status["can_home"]


def test_home_late_ack(s):
    dock_signal(s, "idle")
    s.control("home")
    dock_signal(s, "idle", 1000)
    dock_signal(s, "idle", 1000)
    s.now += 1000
    timeout = s.run("tick")
    assert timeout["dock_action"] == "cancel" and timeout["dock"] == "stopping"
    assert dock_signal(s, "returning")["dock"] == "stopping"
    assert dock_signal(s, "idle")["dock"] == "failed"


def test_stock_low_battery_return(s):
    s.following()
    s.run("battery")
    assert dock_signal(s, "returning")["dock_action"] == "none"
    assert not s.control("home")["accepted"]
    for _ in range(65):
        status = dock_signal(s, "returning", 1000)
        assert status["dock_action"] == "none" and status["dock"] == "returning"
    for _ in range(4):
        status = dock_signal(s, "charging", 500)
    assert status["dock"] == "charging"
    s.now += 1600
    assert s.run("tick")["dock"] == "unknown"
    assert not dock_signal(s, "idle")["can_home"]  # Low battery still asserted.
    s.run("battery-clear")
    assert s.status["can_home"]


def test_home_contact_failure(s):
    dock_signal(s, "idle")
    s.control("home")
    dock_signal(s, "contact")
    assert dock_signal(s, "failed")["dock_action"] == "cancel"
    assert not s.control("home")["accepted"]
    assert dock_signal(s, "idle")["dock"] == "failed"
    assert s.control("home")["dock_action"] == "start"


def test_home_timeout(s):
    dock_signal(s, "idle")
    s.control("home")
    actions = [dock_signal(s, "returning", 1000)["dock_action"] for _ in range(62)]
    assert actions.count("cancel") == 1 and "start" not in actions
    assert dock_signal(s, "idle")["dock"] == "failed"


def test_home_stale(s):
    dock_signal(s, "idle")
    s.control("home")
    s.now += 1601
    assert s.run("tick")["dock_action"] == "cancel"
    assert not s.control("home")["accepted"]
    assert dock_signal(s, "idle")["can_home"]


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
        test_home, test_home_cancel, test_home_no_ack, test_home_late_ack,
        test_home_contact_failure, test_home_timeout,
        test_stock_low_battery_return, test_home_stale,
        retreat, lambda s: retreat(s, track_only=True),
        test_camera, test_disconnect, test_frozen_preview, test_ownership,
        test_stale_calibration, test_frozen_selection,
        test_overlapping_inhibits, test_track_cancels_pending_follow,
        test_turn, test_motion_expiry, test_selection, test_heartbeat_before_selection, test_track_owner_expiry,
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
