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


def test_home_failed_clears_on_select_and_stop(s):
    dock_signal(s, "idle")
    s.control("home")
    dock_signal(s, "failed")
    assert dock_signal(s, "idle")["dock"] == "failed"
    s.frame(REFERENCE)
    selected = s.control("select", s.seq, *REFERENCE)
    assert selected["accepted"] and selected["dock"] == "idle"
    assert s.control("stop")["dock"] == "idle"

    s.control("home")
    dock_signal(s, "failed")
    assert dock_signal(s, "idle")["dock"] == "failed"
    assert s.control("stop")["dock"] == "idle"


def test_home_stopping_timeout(s):
    dock_signal(s, "idle")
    s.control("home")
    dock_signal(s, "returning")
    assert s.control("stop")["dock"] == "stopping"
    for _ in range(2):
        assert dock_signal(s, "returning", 1000)["dock"] == "stopping"
    s.now += 1000
    status = s.run("tick")
    assert status["dock"] == "failed" and status["dock_owner"] == 0
    idle = dock_signal(s, "idle")
    assert idle["dock"] == "failed" and idle["can_home"]
    assert s.control("stop")["dock"] == "idle"


def test_invalid_zero_driver_stop(s):
    s.ready()
    generation = s.status["generation"]
    res = s.control("stop", owner=0)
    assert not res["accepted"] and res["generation"] == generation


def test_ori_bridge_pipes():
    import os, struct, subprocess
    bridge = Path(__file__).resolve().parents[1] / "build" / "ori-bridge"
    r_fd, w_fd = os.pipe()
    try:
        # 1) Safety-locked Track/Follow + battery-telemetry -> dock transition over pipe.
        with ReplayClient(binary=bridge, args=["--control-fd", str(w_fd)], pass_fds=(w_fd,)) as client:
            os.close(w_fd)
            w_fd = -1
            s = Scenario(client)
            assert not s.control("home")["accepted"]  # No real battery telemetry yet.
            s.now = 100
            bat = s.run("battery-telemetry", 85, 0)
            assert not bat["can_home"]
            assert dock_signal(s, "idle")["can_home"]
            s.ready()
            assert not s.run("set-motors", 1, .3, .22)["accepted"]
            locked = s.control("follow", s.seq)
            assert not locked["accepted"] and locked["state"] == "READY"
            assert locked["control_error"] == locked["follow_blocker"] == "follow-locked"
            for x in (113, 106, 99, 92, 85, 78):
                st = s.frame((x, 65, 80, 80), ack=True)
                assert st["motion"] == "HOLD" and not st["follow_enabled"]
            assert s.control("home")["dock"] == "requested"
            assert s.run("battery-telemetry", 85, 0)["dock"] == "requested"
            assert dock_signal(s, "returning")["dock"] == "returning"
            for _ in range(4):
                s.now += 500
                docked = s.run("battery-telemetry", 86, 1)
            assert docked["dock"] == "charging"
        os.set_blocking(r_fd, False)
        emitted = os.read(r_fd, 4096)
        assert len(emitted) == 17 and emitted[4] == 0 and emitted[5] == 0xFE and emitted[10] == 200
    finally:
        if w_fd >= 0:
            os.close(w_fd)
        os.close(r_fd)

    # A live bridge needs fresh factory idle evidence. Battery off-base alone allows tracking.
    r_fd, w_fd = os.pipe()
    try:
        args = ["--enable-follow", "--control-fd", str(w_fd)]
        with ReplayClient(binary=bridge, args=args, pass_fds=(w_fd,)) as client:
            os.close(w_fd)
            w_fd = -1
            s = Scenario(client)
            for _ in range(4):
                s.frame(REFERENCE, interval=500)
                docked = s.run("battery-telemetry", 90, 1)
            assert docked["dock"] == "charging" and docked["can_select"]
            s.frame(REFERENCE)
            off_base = s.run("battery-telemetry", 90, 0)
            assert off_base["dock"] == "unknown" and off_base["can_select"]
            s.ready()
            blocked = s.control("follow", s.seq)
            assert not blocked["accepted"] and blocked["control_error"] == "dock-unconfirmed"
            generation = blocked["generation"]
            idle = dock_signal(s, "idle")
            assert idle["state"] == "READY" and idle["generation"] == generation
            assert not s.control("follow", s.seq - 1, owner=2)["accepted"]
            assert s.control("stop")["accepted"]
            assert not s.control("follow", s.seq, generation=generation)["accepted"]

            s.ready()
            started = s.control("follow", s.seq)
            assert started["state"] == "FOLLOWING"
            for x in (113, 106, 99, 92, 85, 78):
                s.frame((x, 65, 80, 80), ack=True)
            assert s.status["motion"] == "LEFT"
            assert s.control("stop")["state"] == "OFF"
            assert not s.control("follow", s.seq, generation=started["generation"])["accepted"]
            s.frame(REFERENCE)

            dock_signal(s, "idle")
            s.ready()
            assert s.control("follow", s.seq)["accepted"]
            for _ in range(13):
                s.frame(REFERENCE, ack=True)
            stale_dock = s.run("tick")
            assert stale_dock["state"] == "READY" and stale_dock["motion"] == "HOLD"
            assert stale_dock["follow_blocker"] == "dock-unconfirmed"

        raw = os.read(r_fd, 4096)
        assert raw and len(raw) % 30 == 0 and all(raw[offset+10] == 202 for offset in range(0, len(raw), 30))
        axes = [struct.unpack_from("<ff", raw, offset + 15) for offset in range(0, len(raw), 30)]
        assert any(y or x for y, x in axes[:-1]) and axes[-1] == (0, 0), axes
    finally:
        if w_fd >= 0:
            os.close(w_fd)
        os.close(r_fd)

    # 2) Cradle deadman test emits exactly one forward MAVLink packet and exits 0.
    r_fd, w_fd = os.pipe()
    try:
        cradle = subprocess.run(
            [str(bridge), "--control-fd", str(w_fd), "--cradle-test", "--max-ttl", "50", "--send-stop"],
            pass_fds=(w_fd,), capture_output=True, text=True, check=True,
        )
        os.close(w_fd)
        w_fd = -1
        report = json.loads(cradle.stdout)
        assert report["mode"] == "cradle-test" and report["packets_sent"] == 1 and report["zero_stop_sent"]
        os.set_blocking(r_fd, False)
        raw = os.read(r_fd, 4096)
        assert len(raw) == 60 and raw[10] == 202 and raw[40] == 202
    finally:
        if w_fd >= 0:
            os.close(w_fd)
        os.close(r_fd)

    # A legacy motor history cannot initiate or replay movement.
    with ReplayClient(binary=bridge, args=["--enable-follow"]) as client:
        s = Scenario(client)
        assert not s.run("import-route", "r1", 1, 350, 0, 200)["accepted"]
        assert not s.control("home")["accepted"]
        assert dock_signal(s, "idle")["can_home"]
        assert s.control("home")["dock_starts"] == 1
        assert s.run("battery-telemetry", 88, 0)["dock"] == "requested"
        assert not s.run("manual-drive", 1, 0.35, 0.0, 200)["accepted"]

    # The manual transport works with Follow locked, and emits a zero packet on release or expiry.
    r_fd, w_fd = os.pipe()
    try:
        with ReplayClient(binary=bridge, args=["--control-fd", str(w_fd)], pass_fds=(w_fd,)) as client:
            os.close(w_fd)
            w_fd = -1
            s = Scenario(client)
            for _ in range(4):
                s.frame(REFERENCE, interval=500)
                charging = s.run("battery-telemetry", 92, 1)
            assert charging["manual_mode"] == "docked" and not charging["follow_enabled"]
            assert not s.run("manual-drive", 1, 0, 0.25, 250)["accepted"]
            assert s.run("manual-drive", 1, 0.35, 0.25, 250)["manual_active"]
            assert not s.run("manual-drive", 1, 0, 0, 250)["manual_active"]
            s.now += 100
            assert s.run("battery-telemetry", 92, 0)["manual_mode"] == "drive"
            assert s.run("manual-drive", 1, -0.2, 0.3, 250)["manual_active"]
            s.now += 250
            assert not s.run("tick")["manual_active"]
            s.frame(REFERENCE)
            assert s.run("manual-drive", 1, 0.3, 0, 250)["manual_active"]
            assert not s.run("veto")["manual_active"]
            assert not s.run("manual-drive", 1, 0.3, 0, 250)["accepted"]
        raw = os.read(r_fd, 4096)
        assert len(raw) == 180 and all(raw[offset+10] == 202 for offset in range(0, len(raw), 30))
        axes = [tuple(round(value, 2) for value in struct.unpack_from("<ff", raw, offset+15))
                for offset in range(0, len(raw), 30)]
        assert axes == [(-.25, 0), (0, 0), (.2, .12), (0, 0), (-.25, 0), (0, 0)], axes
    finally:
        if w_fd >= 0:
            os.close(w_fd)
        os.close(r_fd)

    # Live battery telemetry enables one explicit factory return without inventing an acknowledgement.
    r_fd, w_fd = os.pipe()
    try:
        with ReplayClient(binary=bridge, args=["--control-fd", str(w_fd)], pass_fds=(w_fd,)) as client:
            os.close(w_fd)
            w_fd = -1
            s = Scenario(client)
            s.frame(REFERENCE)
            assert s.run("battery-telemetry", 89, 0)["can_home"]
            assert s.control("home")["dock"] == "requested"
            assert not s.control("home")["accepted"]
            for _ in range(5):
                s.frame(REFERENCE, interval=1000)
                assert s.run("battery-telemetry", 89, 0)["dock"] == "requested"
            for _ in range(4):
                s.frame(REFERENCE, interval=500)
                charged = s.run("battery-telemetry", 89, 1)
            assert charged["dock"] == "charging" and not charged["can_home"]
            s.frame(REFERENCE)
            assert s.run("battery-telemetry", 89, 0)["can_home"]
            assert s.control("home")["dock"] == "requested"
            assert s.control("stop")["dock"] == "stopping"
        raw = os.read(r_fd, 4096)
        assert len(raw) == 64 and [raw[i] for i in (10, 27, 44)] == [200, 200, 202]
    finally:
        if w_fd >= 0:
            os.close(w_fd)
        os.close(r_fd)

    # 3) Shim status, safety protections, RGB frame luma, dynamic Follow toggle, and cutoff check.
    r_fd, w_fd = os.pipe()
    try:
        with ReplayClient(binary=bridge, args=["--enable-follow", "--control-fd", str(w_fd)], pass_fds=(w_fd,)) as client:
            os.close(w_fd)
            w_fd = -1
            s = Scenario(client)
            s.now = 100
            shim = s.run("shim-status", 1, 0)
            assert shim["rdt_connected"] and not shim["cutoff_verified"]
            assert s.run("device-control", "night_vision", 1)["night_vision"]
            assert s.run("device-control", "eyes", 0)["eyes"] is False
            assert s.run("device-control", "factory_patrol", 1)["factory_patrol"]
            assert s.run("set-motors", 0, 0.3, 0.22)["follow_enabled"] is False
            assert s.run("set-motors", 1, 0.3, 0.22)["follow_enabled"] is True
            s.now += 100
            s.seq += 1
            from fixtures import color_scene
            rgb = color_scene(REFERENCE)
            fr = s.run("frame-rgb", s.seq, s.now, 320, 180, 1, pixels=rgb)
            assert fr["can_select"]
            assert s.control("select", s.seq, *REFERENCE)["state"] == "TRACKING"
            assert s.control("stop")["state"] == "OFF"
            dock_signal(s, "idle")
            cc = s.run("cutoff-check")
            assert cc["accepted"] and cc["cutoff_checks"] == 1

            # Opposite-sign manual-drive reversal must immediately emit negative rx (never positive).
            s.control("stop")
            s.frame(REFERENCE)
            s.run("battery-telemetry", 92, 0)
            dock_signal(s, "idle")
            s.now += 100
            assert s.run("manual-drive", 1, 0.0, 0.24, 250)["manual_active"]
            s.now += 60
            assert s.run("manual-drive", 1, 0.0, -0.24, 250)["manual_active"]
            assert s.run("manual-drive", 1, 0.0, 0.0, 250)["accepted"]

            # Anti-spin circuit breaker: 3 settled pulses drifting further off-center halt Follow.
            s.ready()
            dock_signal(s, "idle")
            assert s.control("follow", s.seq)["state"] == "FOLLOWING"
            tripped = None
            for x in (85, 85, 85, 85, 65, 65, 65, 65, 45, 45, 45, 45, 25):
                st = s.frame((x, 65, 80, 80), ack=True)
                if st.get("control_error") == "turn-runaway":
                    tripped = st
                    break
            assert tripped is not None and tripped["state"] == "READY" and tripped["motion"] == "HOLD", tripped
        os.set_blocking(r_fd, False)
        raw = os.read(r_fd, 8192)
        assert b"\xfe" in raw
        motor_rx = [round(struct.unpack_from("<f", raw, off + 19)[0], 2)
                    for off in range(len(raw) - 29)
                    if raw[off + 4] == 0 and raw[off + 5] == 0xFE and raw[off + 10] == 202]
        assert any(a > 0 and b < 0 for a, b in zip(motor_rx, motor_rx[1:])), motor_rx
    finally:
        if w_fd >= 0:
            os.close(w_fd)
        os.close(r_fd)


def test_camera_and_patrol_server():
    import threading
    from urllib.request import Request, urlopen
    from server import Demo, Handler, OriServer

    with ReplayClient() as replay, OriServer(("127.0.0.1", 0), Handler) as server:
        server.tls = server.require_auth = server.wildcard = False
        server.token = server.viewer_token = ""
        server.host = f"127.0.0.1:{server.server_port}"
        server.demo = Demo(replay)
        server.demo.capture()
        producer = threading.Thread(target=server.serve_forever, daemon=True)
        producer.start()
        base = f"http://{server.host}"
        try:
            def post(path, body):
                req = Request(base + path, method="POST", data=json.dumps(body).encode(),
                              headers={"Content-Type": "application/json", "Origin": base})
                with urlopen(req, timeout=2) as r:
                    return json.load(r)

            with urlopen(base + "/api/frame", timeout=2) as img:
                data = img.read()
                assert data[:8] == b"\x89PNG\r\n\x1a\n" and data[25] == 2 # RGB24 default
                st = json.loads(img.headers["X-Ori-Status"])
                assert st["camera"]["color_mode"] == "color"

            bw = post("/api/camera", {"boot": st["boot"], "generation": st["generation"], "driver": OWNER, "color_mode": "bw"})
            assert bw["accepted"] and bw["camera"]["color_mode"] == "bw"
            server.demo.capture()
            with urlopen(base + "/api/frame", timeout=2) as img:
                data = img.read()
                assert data[25] == 0 # Grayscale PNG in B&W

            night = post("/api/camera", {"boot": st["boot"], "generation": bw["generation"], "driver": OWNER,
                                         "night_mode": "night", "ir_tint": "emerald"})
            assert night["accepted"] and night["camera"]["night_active"]
            server.demo.capture()
            with urlopen(base + "/api/frame", timeout=2) as img:
                data = img.read()
                assert data[25] == 2 # Emerald night tint PNG

            pat = post("/api/patrol", {"boot": st["boot"], "generation": night["generation"], "driver": OWNER,
                                       "mode": "start", "kind": "area", "area": [0.2, 0.2, 0.8, 0.8], "laps": 1})
            assert pat["accepted"] and pat["patrol"]["active"]
            server.demo.capture()
            # Intermediate polls between decoded frames (< 750 ms) must keep patrol running.
            from fixtures import color_scene
            server.demo.video = type("StubVideo", (), {
                "read_latest": staticmethod(lambda: (color_scene(REFERENCE), 320, 180, server.demo.frame_received)),
            })()
            server.demo.capture()
            assert server.demo.status["patrol"]["active"]
            server.demo.video = None
            stopped = post("/api/stop", {"boot": st["boot"], "generation": server.demo.status["generation"], "driver": OWNER})
            assert stopped["accepted"] and not stopped["patrol"]["active"]

            # Checkpoint teaching records exact active segments (not pauses) and retraces from end checkpoint.
            import time
            t0 = post("/api/patrol", {"boot": st["boot"], "generation": stopped["generation"], "driver": OWNER,
                                      "mode": "teach-start"})
            assert t0["accepted"]
            seq = int(time.time() * 1000)
            m1 = post("/api/manual", {"boot": st["boot"], "generation": stopped["generation"], "driver": OWNER,
                                      "ly": 0.30, "rx": 0.0, "ttl": 250, "sequence": seq + 1})
            assert m1["manual_active"]
            time.sleep(0.08)
            m2 = post("/api/manual", {"boot": st["boot"], "generation": m1["generation"], "driver": OWNER,
                                      "ly": 0.0, "rx": 0.0, "ttl": 250, "sequence": seq + 2})
            time.sleep(0.18) # pause must NOT inflate taught leg durations
            m3 = post("/api/manual", {"boot": st["boot"], "generation": m2["generation"], "driver": OWNER,
                                      "ly": 0.0, "rx": -0.22, "ttl": 250, "sequence": seq + 3})
            assert m3["manual_active"]
            time.sleep(0.08)
            cap = post("/api/patrol", {"boot": st["boot"], "generation": m3["generation"], "driver": OWNER,
                                       "mode": "teach-capture"})
            legs = cap.get("teach_legs") or []
            assert len(legs) == 2 and legs[0]["dt"] < 180 and legs[1]["rx"] == -0.22, legs
            sig1 = [1 / 12.0 if i < 72 else -1 / 12.0 for i in range(144)]
            sig2 = [1 / 12.0 if i % 2 == 0 else -1 / 12.0 for i in range(144)]
            server.demo._latest_sig = sig2
            cp_start = post("/api/patrol", {"boot": st["boot"], "generation": cap["generation"], "driver": OWNER,
                                            "mode": "start", "kind": "checkpoints",
                                            "checkpoints": [
                                                {"name": "Base view", "signature": sig1, "legs": []},
                                                {"name": "Window", "signature": sig2, "legs": legs},
                                            ]})
            assert cp_start["accepted"] and cp_start["patrol"]["kind"] == "checkpoints"
            steps = server.demo.patrol["steps"]
            assert len(steps) == 2 and steps[0]["retrace"] is True and not steps[1]["retrace"], steps
            assert steps[0]["legs"][0]["rx"] == 0.22 and steps[0]["legs"][1]["ly"] == -0.3, steps[0]["legs"]
            post("/api/stop", {"boot": st["boot"], "generation": cp_start["generation"], "driver": OWNER})
        finally:
            server.shutdown()
            producer.join()



def test_status_without_video():
    import threading
    from urllib.request import Request, urlopen
    from server import Demo, Handler, OriServer

    with ReplayClient() as replay, OriServer(("127.0.0.1", 0), Handler) as server:
        server.tls = server.require_auth = server.wildcard = False
        server.token = server.viewer_token = ""
        server.host = f"127.0.0.1:{server.server_port}"
        server.demo = Demo(replay)
        server.demo.capture()
        producer = threading.Thread(target=server.serve_forever, daemon=True)
        producer.start()
        url = f"http://{server.host}/api/frame"
        try:
            with urlopen(url, timeout=2) as image:
                assert image.status == 200 and image.read(8) == b"\x89PNG\r\n\x1a\n"
                etag = image.headers["ETag"]
                revision = image.headers["X-Ori-Status-Tag"]
                initial = json.loads(image.headers["X-Ori-Status"])
            stop = Request(url.replace("/frame", "/stop"), method="POST",
                           data=json.dumps({"boot": initial["boot"], "generation": initial["generation"],
                                            "driver": OWNER}).encode(),
                           headers={"Content-Type": "application/json", "Origin": f"http://{server.host}"})
            with urlopen(stop, timeout=2) as reply:
                assert json.load(reply)["accepted"]
            poll = Request(url, headers={"If-None-Match": etag, "X-Ori-Status-Tag": revision})
            with urlopen(poll, timeout=2) as update:
                status = json.loads(update.headers["X-Ori-Status"])
                assert update.status == 204 and not update.read()
                assert update.headers["X-Frame-Id"] is None
                assert status["revision"] > initial["revision"] and status["reason"] == "owner-stop"
        finally:
            server.shutdown()
            producer.join()


def test_smart_memory_and_autodock(s):
    s.run("policy", 1, 1, 1, 1, 1, 1, 1, 5000)
    assert not s.run("battery-telemetry", 80, 0)["can_home"]
    assert dock_signal(s, "idle")["can_home"]
    s.frame(REFERENCE)
    assert s.control("select", s.seq, *REFERENCE)["state"] == "TRACKING"
    s.frame(REFERENCE, ack=True)
    st = s.frame(REFERENCE, ack=True)
    assert st["state"] == "READY" and st["reason"] == "gap-auto-set"
    blob = s.run("export-memory")["memory_blob"]
    assert blob.startswith("v1 ")
    assert s.control("follow")["state"] == "FOLLOWING"
    for _ in range(5):
        assert s.frame(ack=True)["motion"] == "HOLD"
    for _ in range(2):
        assert s.frame((40, 65, 80, 80))["state"] == "LOST"
    resumed = s.frame((40, 65, 80, 80), ack=True)
    assert resumed["state"] == "READY" and abs(resumed["box"][0] - 40) <= 2
    assert s.run("battery-telemetry", 18, 0)["dock"] == "requested"
    assert s.status["auto_dock_reason"] == "auto-dock-low-battery"

    # Manual drive overrides state, emits manual_active=True, and deadman expires on frame/tick.
    m = s.run("manual-drive", OWNER, 0.4, -0.2, 200)
    assert m["accepted"] and m["manual_active"]
    s.now += 200
    assert not s.run("tick")["manual_active"]
    sm = s.run("set-motors", 1, 0.45, 0.35)
    assert sm["follow_enabled"]


def test_rdt_battery_packets():
    import os
    import struct
    import sys
    import time
    from server import BRIDGE_BINARY, Demo, KalaySource, RdtParser

    # Shared RDT reassembly and msg 207 validation vectors (mirrored in tests/core.cpp).
    vec_a = bytes.fromhex("fe12010000cf000000000000000000000000000000005302b8c0")
    vec_b = bytes.fromhex("fe12020000cf000000000000000000000000000000002900e092")
    vec_non207 = bytes.fromhex("fe04000000c89a9cff01755e")
    vec_bad_len = bytes.fromhex("fe11030000cf0000000000000000000000000000005302fdc9")
    vec_bad_bat = bytes.fromhex("fe12040000cf0000000000000000000000000000000065016804")
    vec_bad_chg = bytes.fromhex("fe12050000cf000000000000000000000000000000003203f7b1")
    vec_bad_crc = vec_a[:-1] + bytes([vec_a[-1] ^ 0x01])
    vec_bad_payload = vec_a[:22] + b"\x54" + vec_a[23:]

    rdt = RdtParser()
    for split in range(1, len(vec_a)):
        rdt.reset()
        assert rdt.feed(vec_a[:split]) == []
        assert rdt.feed(vec_a[split:]) == [(83, 1)]
    rdt.reset()
    byte_by_byte = []
    for idx in range(len(vec_a)):
        byte_by_byte.extend(rdt.feed(vec_a[idx:idx + 1]))
    assert byte_by_byte == [(83, 1)]

    rdt.reset()
    assert rdt.feed(vec_non207 + vec_a + vec_b) == [(83, 1), (41, 0)]

    rdt.reset()
    assert rdt.feed(vec_a[:10]) == []
    rdt.reset()
    assert rdt.feed(vec_a[10:]) == []
    assert rdt.feed(vec_b) == [(41, 0)]

    for bad in (vec_bad_crc, vec_bad_payload, vec_bad_len, vec_bad_bat, vec_bad_chg):
        rdt.reset()
        assert rdt.feed(bad) == []
    rdt.reset()
    assert rdt.feed(vec_bad_crc + vec_b) == [(41, 0)]

    rdt.reset()
    assert rdt.feed(b"\xaa" * 5000 + vec_a) == [(83, 1)]
    assert len(rdt._buf) <= RdtParser.MAX_BUFFER_BYTES

    # Verify truncated/corrupt/previous-session RDT chunks never refresh battery or drive readiness.
    import shlex
    stub_code = (
        "import sys, struct, time\n"
        "def emit(k, p):\n"
        "    sys.stdout.buffer.write(struct.pack('<IB', len(p), k) + p)\n"
        "    sys.stdout.buffer.flush()\n"
        "vec_a = bytes.fromhex('fe12010000cf000000000000000000000000000000005302b8c0')\n"
        "vec_b = bytes.fromhex('fe12020000cf000000000000000000000000000000002900e092')\n"
        "vec_bad = vec_a[:-1] + bytes([vec_a[-1] ^ 1])\n"
        "emit(0xFE, b'')\n"
        "emit(0xFD, struct.pack('<BBH', 1, 1, 0))\n"
        "emit(0xFF, vec_a[:10])\n"
        "time.sleep(0.05)\n"
        "emit(0xFE, b'')\n"
        "emit(0xFD, struct.pack('<BBH', 1, 1, 0))\n"
        "emit(0xFF, vec_a[10:])\n"
        "emit(0xFF, vec_bad)\n"
        "time.sleep(0.2)\n"
        "emit(0xFF, vec_b)\n"
        "time.sleep(0.5)\n"
    )
    cmd = f"{shlex.quote(sys.executable)} -c {shlex.quote(stub_code)}"
    source = KalaySource(cmd)
    try:
        with ReplayClient(binary=BRIDGE_BINARY, args=["--control-fd", str(source.control_fd)],
                          pass_fds=(source.control_fd,)) as replay:
            demo = Demo(replay, video=source, synthetic_dock=False, live_hardware=True)
            source.bind(demo.on_battery, demo.on_shim)
            deadline = time.monotonic() + 0.15
            while time.monotonic() < deadline and not demo.rdt_ok:
                time.sleep(0.01)
            assert demo.rdt_ok
            demo.capture()
            assert demo.last_battery_at == 0
            assert demo.status.get("battery_percent", -1) == -1
            assert demo.status.get("manual_mode") == "blocked"
            assert not demo.status.get("can_home")
            deadline = time.monotonic() + 1.0
            while time.monotonic() < deadline and demo.status.get("battery_percent", -1) != 41:
                time.sleep(0.02)
            assert demo.status.get("battery_percent") == 41, demo.status
    finally:
        source.close()


def test_battery_charging_and_docked_selection():
    import os
    bridge = Path(__file__).resolve().parents[1] / "build" / "ori-bridge"
    r_fd, w_fd = os.pipe()
    try:
        with ReplayClient(binary=bridge, args=["--enable-follow", "--control-fd", str(w_fd)],
                          pass_fds=(w_fd,)) as client:
            os.close(w_fd)
            w_fd = -1
            s = Scenario(client)
            # Eight battery samples 2,000 ms apart confirm charging on sample 2 and keep selection enabled.
            states = []
            for _ in range(8):
                s.frame(REFERENCE, interval=2000)
                st = s.run("battery-telemetry", 94, 1)
                states.append((st["dock"], st["can_select"]))
            assert states[0] == ("checking-charge", True), states
            assert all(item == ("charging", True) for item in states[1:]), states

            # Base Watch off: explicit camera-only selection succeeds while docked; Follow stays blocked.
            assert not st["policy"]["base_watch"]
            sel = s.control("select", s.seq, *REFERENCE)
            assert sel["accepted"] and sel["state"] == "TRACKING" and sel["drive_writes"] == 0
            s.frame(REFERENCE, ack=True)
            s.frame(REFERENCE, ack=True)
            assert s.control("teach")["state"] == "READY"
            blocked = s.control("follow", s.seq)
            assert not blocked["accepted"] and blocked["follow_blocker"] == "on-base"
            assert blocked["drive_writes"] == 0
            os.set_blocking(r_fd, False)
            try:
                pre_home = os.read(r_fd, 4096)
            except BlockingIOError:
                pre_home = b""
            assert pre_home == b"", pre_home

            # Gap over 4,000 ms expires battery-backed charging evidence; 3,000 ms apart re-confirms.
            s.now += 4001
            assert s.run("tick")["dock"] == "unknown"
            assert s.run("battery-telemetry", 94, 1)["dock"] == "checking-charge"
            s.now += 3000
            assert s.run("battery-telemetry", 94, 1)["dock"] == "charging"

            # Contact, active return, and other motion inhibits still block selection.
            assert s.control("stop")["state"] == "OFF"
            assert dock_signal(s, "contact")["dock"] == "checking-charge"
            s.frame(REFERENCE)
            assert not s.status["can_select"] and not s.control("select", s.seq, *REFERENCE)["accepted"]
            assert dock_signal(s, "idle")["can_home"]
            assert s.control("home")["dock"] == "requested"
            s.frame(REFERENCE)
            assert not s.status["can_select"] and not s.control("select", s.seq, *REFERENCE)["accepted"]
            assert s.control("stop")["accepted"]
            assert dock_signal(s, "idle")["can_select"]
            s.run("sleep")
            s.frame(REFERENCE)
            assert not s.status["can_select"] and not s.control("select", s.seq, *REFERENCE)["accepted"]
    finally:
        if w_fd >= 0:
            os.close(w_fd)
        os.close(r_fd)


def test_autospot_and_ownership():
    import tempfile
    from server import Demo, load_memory

    small_box = (156, 81, 18, 19)
    with ReplayClient() as client:
        s = Scenario(client)
        s.frame(small_box)
        assert s.control("select", s.seq, *small_box)["accepted"]
        small_blob = s.run("export-memory")["memory_blob"]
        assert small_blob.startswith("v1 156 81 18 19 ")

    with tempfile.TemporaryDirectory() as tmp:
        mem_path = Path(tmp) / "memory.json"
        mem_path.write_text(json.dumps({
            "policy": {
                "auto_reacquire": True,
                "auto_gap": False,
                "auto_spot": True,
                "base_watch": True,
                "auto_dock_low_battery": True,
                "auto_dock_idle": False,
                "auto_dock_disconnect": False,
                "idle_timeout_ms": 45000,
            },
            "memory_blob": small_blob,
        }), encoding="utf-8")

        # 1) load_memory disarms auto_spot while preserving other settings and the valid small saved target.
        with ReplayClient() as client:
            st = load_memory(client, mem_path, live=True, smart=True, now=100)
            assert st["policy"]["auto_spot"] is False
            assert st["policy"]["auto_reacquire"] is True
            assert st["policy"]["base_watch"] is True
            assert st["policy"]["auto_dock_low_battery"] is True
            assert st["policy"]["idle_timeout_ms"] == 45000
            assert st["memory"] == "saved", st

        # 2) Startup flicker leaves Ori idle when policy is loaded on restart.
        with ReplayClient() as client:
            load_memory(client, mem_path, live=True, smart=True, now=100)
            s = Scenario(client)
            s.now = 100
            for i in range(6):
                st = s.frame((110 + i * 3, 60, 72, 72))
            assert st["state"] == "OFF" and st["owner"] == 0, st

        # 3) After explicit opt-in, stationary-then-moving textured target triggers autonomous TRACKING;
        #    a human heartbeat is rejected and does not claim it, and Demo keeps the saved human target.
        with ReplayClient() as client:
            load_memory(client, mem_path, live=True, smart=True, now=100)
            demo = Demo(client, synthetic_dock=True, memory_file=mem_path)
            s = Scenario(client)
            s.now = 100
            with demo.lock:
                demo.record(s.run("policy", 1, 0, 1, 1, 1, 0, 0, 45000))
                for _ in range(3):
                    demo.record(s.frame())
                for i in range(4, 7):
                    demo.record(s.frame((110 + i * 2, 60, 72, 72)))
            assert demo.status["state"] == "TRACKING" and demo.status["autonomous"]
            assert demo.status["owner"] == 2**53 - 1
            hb = s.control("heartbeat", s.seq)
            assert not hb["accepted"] and hb["owner"] == 2**53 - 1
            with demo.lock:
                demo._persist(force=True)
            persisted = json.loads(mem_path.read_text(encoding="utf-8"))
            assert persisted["memory_blob"] == small_blob

        # 4) Human selection using pre-autonomous-bump generation wins the race, and persists on adoption.
        with ReplayClient() as client:
            load_memory(client, mem_path, live=True, smart=True, now=100)
            demo = Demo(client, synthetic_dock=True, memory_file=mem_path)
            s = Scenario(client)
            s.now = 100
            demo.now = lambda: s.now + 10
            with demo.lock:
                demo.record(s.run("policy", 1, 0, 1, 1, 1, 0, 0, 45000))
                for _ in range(3):
                    demo.record(s.frame())
                pre_gen = demo.status["generation"]
                frame_with_human_target = s.seq + 1
                demo.record(s.frame(small_box, (180, 60, 64, 64)))
                for i in range(5, 7):
                    demo.record(s.frame(small_box, (180 + i * 2, 60, 64, 64)))
                demo.seq = s.seq
            assert demo.status["autonomous"] and demo.status["generation"] > pre_gen
            won = demo.action("select", {
                "driver": OWNER, "boot": demo.boot, "generation": pre_gen,
                "frame": frame_with_human_target,
                "box": [small_box[0] / 320, small_box[1] / 180, small_box[2] / 320, small_box[3] / 180],
            })
            assert won["accepted"] and won["owner"] == OWNER and not won["autonomous"], won
            assert json.loads(mem_path.read_text(encoding="utf-8"))["memory_blob"].startswith("v1 156 81 18 19 ")

        # 5) Delayed selection after Stop is rejected.
        with ReplayClient() as client:
            s = Scenario(client)
            s.run("policy", 1, 0, 1, 0, 0, 0, 0)
            for _ in range(3):
                s.frame()
            pre_gen = s.status["generation"]
            target_frame = s.seq + 1
            for i in range(4, 7):
                s.frame((110 + i * 2, 60, 72, 72))
            assert s.status["autonomous"]
            assert s.control("stop")["state"] == "OFF"
            assert not s.control("select", target_frame, 120, 60, 72, 72, generation=pre_gen)["accepted"]

        # 6) Delayed selection after another human takes control (or stops) is rejected.
        with ReplayClient() as client:
            s = Scenario(client)
            s.frame(REFERENCE)
            stale_gen = s.status["generation"]
            target_frame = s.seq
            assert s.control("select", target_frame, *REFERENCE, owner=2, generation=stale_gen)["accepted"]
            assert not s.control("select", target_frame, *REFERENCE, owner=1, generation=stale_gen)["accepted"]
            assert s.control("stop", owner=2)["state"] == "OFF"
            assert not s.control("select", target_frame, *REFERENCE, owner=1, generation=stale_gen)["accepted"]


def test_frame_rgb_replay_and_video():
    import subprocess
    import time
    from client import BINARY, BRIDGE_BINARY
    from fixtures import HEIGHT, WIDTH, color_backdrop, color_scene, color_texture
    from server import Demo

    # 1) Native ori-replay and ori-bridge agree identically on 640x360 RGB input with boost.
    def upscale_2x(rgb320):
        out = bytearray(640 * 360 * 3)
        for y in range(180):
            row = rgb320[y * 320 * 3:(y + 1) * 320 * 3]
            up_row = bytearray(640 * 3)
            for x in range(320):
                px = row[x * 3:x * 3 + 3]
                up_row[x * 6:x * 6 + 3] = px
                up_row[x * 6 + 3:x * 6 + 6] = px
            out[(y * 2) * 640 * 3:(y * 2 + 1) * 640 * 3] = up_row
            out[(y * 2 + 1) * 640 * 3:(y * 2 + 2) * 640 * 3] = up_row
        return bytes(out)

    seq_boxes = [(120, 65, 60, 60), (124, 65, 60, 60), (128, 65, 60, 60), (134, 65, 60, 60)]
    outputs = []
    for bin_path, extra_args in ((BINARY, []), (BRIDGE_BINARY, ["--assume-idle"])):
        with ReplayClient(binary=bin_path, args=extra_args) as client:
            s = Scenario(client)
            res = []
            for idx, b in enumerate(seq_boxes, 1):
                s.now += 150
                s.seq += 1
                rgb640 = upscale_2x(color_scene(b))
                st = s.run("frame-rgb", s.seq, s.now, 640, 360, 1, pixels=rgb640)
                if idx == 1:
                    st = s.control("select", s.seq, *b)
                else:
                    st = s.control("heartbeat", s.seq)
                res.append((st["state"], st["box"], st["confidence"], st["motion"], st["can_select"]))
            outputs.append(res)
    assert outputs[0] == outputs[1], outputs

    # Invalid dimensions in frame-rgb are rejected identically.
    for bin_path in (BINARY, BRIDGE_BINARY):
        bad = subprocess.run([str(bin_path)], input=b"frame-rgb 100 1 100 160 90 0\n",
                             capture_output=True)
        assert bad.returncode == 2, (bin_path, bad.returncode)

    # 2) File --video routes through frame-rgb (survives 450 ms frame spacing) and stays camera-only.
    with ReplayClient() as client:
        rgb_frame = color_scene(REFERENCE)
        stub_ts = [Demo.now()]
        stub_video = type("StubVideo", (), {
            "read_latest": staticmethod(lambda: (rgb_frame, WIDTH, HEIGHT, stub_ts[0])),
        })()
        demo = Demo(client, video=stub_video, synthetic_dock=False, live_hardware=False)
        demo.capture()
        assert demo.status["state"] == "OFF" and demo.status["source_mode"] == "video"
        assert demo.seq == 1
        sel = demo.action("select", {
            "driver": OWNER, "boot": demo.boot, "generation": demo.status["generation"],
            "frame": 1, "box": [120 / 320, 65 / 180, 80 / 320, 80 / 180],
        })
        assert sel["accepted"] and sel["state"] == "TRACKING" and sel["frame"] == 1
        # Wait 400 ms (> 250 ms grayscale limit, < 650 ms frame-rgb limit) and deliver next frame-rgb.
        time.sleep(0.4)
        stub_ts[0] = Demo.now()
        demo.capture()
        assert demo.status["state"] == "TRACKING", demo.status
        for blocked_action in ("follow", "home", "manual", "patrol", "cutoff-test"):
            out = demo.action(blocked_action, {
                "driver": OWNER, "boot": demo.boot, "generation": demo.status["generation"],
                "frame": demo.seq,
            })
            assert not out["accepted"] and out.get("control_error") == "video-only", (blocked_action, out)

    # 3) Uniform background cannot acquire lock, grayscale IR tracks, and crossing twins hold motion.
    with ReplayClient() as client:
        s = Scenario(client)
        uniform_rgb = bytes([95, 130, 105]) * (WIDTH * HEIGHT)
        s.now += 100
        s.seq += 1
        s.run("frame-rgb", s.seq, s.now, WIDTH, HEIGHT, 0, pixels=uniform_rgb)
        assert not s.control("select", s.seq, 120, 60, 44, 44)["accepted"]

    def make_ir_rgb(*boxes):
        gray = scene(*boxes)
        out = bytearray(WIDTH * HEIGHT * 3)
        for idx, px in enumerate(gray):
            out[idx * 3] = out[idx * 3 + 1] = out[idx * 3 + 2] = px
        return bytes(out)

    with ReplayClient() as client:
        s = Scenario(client)
        for step in range(10):
            box = (110 + step * 5, 65, 60, 60)
            s.now += 100
            s.seq += 1
            st = s.run("frame-rgb", s.seq, s.now, WIDTH, HEIGHT, 1, pixels=make_ir_rgb(box))
            if step == 0:
                st = s.control("select", s.seq, *box)
                assert st["accepted"], st
            else:
                st = s.control("heartbeat", s.seq)
            assert st["box"] is not None and abs(st["box"][0] - box[0]) <= 6, (step, st)

    with ReplayClient() as client:
        s = Scenario(client)
        a0, b0 = (70, 60, 44, 56), (206, 60, 44, 56)
        for step in range(4):
            s.now += 100
            s.seq += 1
            st = s.run("frame-rgb", s.seq, s.now, WIDTH, HEIGHT, 0, pixels=color_scene(a0, b0))
            if step == 0:
                assert s.control("select", s.seq, *a0)["accepted"]
            else:
                s.control("heartbeat", s.seq)
        assert s.control("teach")["state"] == "READY"
        assert s.control("follow")["state"] == "FOLLOWING"
        crossing_motions = []
        for step in range(1, 14):
            ax = (70 + step * 10, 60, 44, 56)
            bx = (206 - step * 10, 60, 44, 56)
            s.now += 100
            s.seq += 1
            st = s.run("frame-rgb", s.seq, s.now, WIDTH, HEIGHT, 0, pixels=color_scene(ax, bx))
            if st["owner"]:
                st = s.control("heartbeat", s.seq)
            if abs(ax[0] - bx[0]) <= 24:
                crossing_motions.append(st["motion"])
        assert crossing_motions and all(m == "HOLD" for m in crossing_motions), crossing_motions


def _build_fake_shim(tmp_dir):
    import os
    import shutil
    import subprocess
    cc = shutil.which("cc") or shutil.which("clang") or shutil.which("gcc")
    if not cc:
        return None, "C compiler unavailable"
    root = Path(__file__).resolve().parents[1]
    lib_path = Path(tmp_dir) / "libTUTKGlobalAPIs.so"
    shim_path = Path(tmp_dir) / "shim"
    r1 = subprocess.run([cc, "-std=c11", "-shared", "-fPIC", str(root / "tests" / "fake_tutk.c"),
                         "-o", str(lib_path)], capture_output=True, text=True)
    if r1.returncode != 0:
        return None, f"fake_tutk build failed: {r1.stderr.strip()}"
    for name in ("libIOTCAPIs.so", "libRDTAPIs.so", "libAVAPIs.so"):
        link = Path(tmp_dir) / name
        if not link.exists():
            os.symlink(lib_path.name, link)
    shim_cmd = [cc, "-std=c11", str(root / "bridge" / "ori_kalay_shim.c"), "-o", str(shim_path)]
    if sys.platform.startswith("linux"):
        shim_cmd.append("-ldl")
    r2 = subprocess.run(shim_cmd, capture_output=True, text=True)
    if r2.returncode != 0:
        return None, f"shim build failed: {r2.stderr.strip()}"
    return shim_path, None


def _split_annexb_aus(stream, is_hevc):
    starts = []
    i = 0
    n = len(stream)
    while i + 3 < n:
        if stream[i] == 0 and stream[i + 1] == 0 and stream[i + 2] == 1:
            starts.append((i, i + 3))
            i += 3
        elif i + 4 < n and stream[i] == 0 and stream[i + 1] == 0 and stream[i + 2] == 0 and stream[i + 3] == 1:
            starts.append((i, i + 4))
            i += 4
        else:
            i += 1
    aus = []
    pending = bytearray()
    for idx, (sc_pos, nal_pos) in enumerate(starts):
        end_pos = starts[idx + 1][0] if idx + 1 < len(starts) else n
        chunk = stream[sc_pos:end_pos]
        if nal_pos >= end_pos:
            continue
        hdr = stream[nal_pos]
        nal_type = ((hdr >> 1) & 0x3F) if is_hevc else (hdr & 0x1F)
        is_vcl = (0 <= nal_type <= 31) if is_hevc else (1 <= nal_type <= 5)
        pending.extend(chunk)
        if is_vcl:
            aus.append(bytes(pending))
            pending.clear()
    if pending and aus:
        aus[-1] = aus[-1] + bytes(pending)
    return aus


def test_video_freshness_and_shim():
    import os
    import shlex
    import struct
    import subprocess
    import tempfile
    import time
    from evaluate import find_ffmpeg
    from server import KalaySource

    ffmpeg = find_ffmpeg()
    if not ffmpeg:
        print("Skipped shim video freshness test: FFmpeg unavailable")
        return

    with tempfile.TemporaryDirectory() as tmp:
        shim_path, err = _build_fake_shim(tmp)
        if not shim_path:
            print(f"Skipped fake TUTK tests: {err}")
            return

        enc_check = subprocess.run([ffmpeg, "-encoders"], capture_output=True, text=True)
        encoders = enc_check.stdout
        codecs = [("h264", 78, "libx264", ["-x264-params", "slices=1"])]
        if "libx265" in encoders:
            codecs.append(("hevc", 80, "libx265", ["-x265-params", "slices=1:pools=none:frame-threads=1"]))
        elif "hevc_videotoolbox" in encoders:
            codecs.append(("hevc", 80, "hevc_videotoolbox", []))
        else:
            print("Skipped HEVC fake-shim test: neither libx265 nor hevc_videotoolbox available")

        base_env = {
            **os.environ,
            "EBO_LIB_DIR": tmp,
            "EBO_LICENSE": "lic",
            "EBO_UID": "uid",
            "EBO_AUTHKEY": "auth",
            "EBO_IDENTITY": "id",
            "EBO_TOKEN": "tok",
        }

        # 1) Both H.264 and HEVC 20-frame 10 FPS streams decode 20/20 frames with passthrough.
        for fmt, codec_id, encoder, extra_enc in codecs:
            raw_bitstream = subprocess.run([
                ffmpeg, "-y", "-v", "error",
                "-f", "lavfi", "-i", "testsrc=size=320x180:rate=10",
                "-frames:v", "20", "-g", "10", "-bf", "0",
                "-c:v", encoder, *extra_enc, "-f", fmt, "pipe:1",
            ], capture_output=True, check=True).stdout
            aus = _split_annexb_aus(raw_bitstream, is_hevc=(codec_id == 80))
            assert len(aus) == 20, (fmt, len(aus))
            vid_file = Path(tmp) / f"video_{fmt}.bin"
            with vid_file.open("wb") as f:
                for au in aus:
                    f.write(struct.pack("<I", len(au)) + au)
            log_file = Path(tmp) / f"log_{fmt}.txt"
            env = {**base_env, "FAKE_TUTK_VIDEO": str(vid_file), "FAKE_TUTK_CODEC": str(codec_id),
                   "FAKE_TUTK_LOG": str(log_file)}
            wrapper = (f"env EBO_LIB_DIR={shlex.quote(tmp)} EBO_LICENSE=l EBO_UID=u EBO_AUTHKEY=a "
                       f"EBO_IDENTITY=i EBO_TOKEN=t FAKE_TUTK_VIDEO={shlex.quote(str(vid_file))} "
                       f"FAKE_TUTK_CODEC={codec_id} FAKE_TUTK_LOG={shlex.quote(str(log_file))} "
                       f"{shlex.quote(str(shim_path))}")
            source = KalaySource(wrapper)
            try:
                deadline = time.monotonic() + 5.0
                while time.monotonic() < deadline:
                    if log_file.exists() and "video 19" in log_file.read_text(encoding="utf-8"):
                        break
                    time.sleep(0.05)
                time.sleep(0.15)
            finally:
                source.close()
            st = source.stats()
            assert st["decoded_frames"] == 20, (fmt, st)
            assert st["decoder_starts"] == 1 and st["session_starts"] == 1, (fmt, st)

        # 2) Genuine disconnect triggers reconnect and resumes decoded frames.
        vid_h264 = Path(tmp) / "video_h264.bin"
        drop_log = Path(tmp) / "log_drop.txt"
        wrapper_drop = (f"env EBO_LIB_DIR={shlex.quote(tmp)} EBO_LICENSE=l EBO_UID=u EBO_AUTHKEY=a "
                        f"EBO_IDENTITY=i EBO_TOKEN=t FAKE_TUTK_VIDEO={shlex.quote(str(vid_h264))} "
                        f"FAKE_TUTK_CODEC=78 FAKE_TUTK_DROP_AFTER_MS=350 "
                        f"FAKE_TUTK_LOG={shlex.quote(str(drop_log))} {shlex.quote(str(shim_path))}")
        source = KalaySource(wrapper_drop)
        try:
            deadline = time.monotonic() + 5.0
            while time.monotonic() < deadline and (
                source.stats()["session_starts"] < 2 or source.stats()["decoded_frames"] < 10
            ):
                time.sleep(0.05)
            st = source.stats()
            assert st["session_starts"] >= 2 and st["decoded_frames"] >= 10, st
        finally:
            source.close()

        # 3) Slow decoder never starves battery or shim callbacks.
        slow_ff = Path(tmp) / "slow_ffmpeg.py"
        slow_ff.write_text("#!/usr/bin/env python3\nimport time\ntime.sleep(30)\n", encoding="utf-8")
        slow_ff.chmod(0o755)
        bat_times = []
        shim_times = []
        wrapper_slow = (f"env EBO_LIB_DIR={shlex.quote(tmp)} EBO_LICENSE=l EBO_UID=u EBO_AUTHKEY=a "
                        f"EBO_IDENTITY=i EBO_TOKEN=t FAKE_TUTK_VIDEO={shlex.quote(str(vid_h264))} "
                        f"FAKE_TUTK_CODEC=78 FAKE_TUTK_BATTERY=1 {shlex.quote(str(shim_path))}")
        source = KalaySource(wrapper_slow, ffmpeg_bin=str(slow_ff))
        t0 = time.monotonic()
        source.bind(lambda *_: bat_times.append(time.monotonic() - t0),
                    lambda *_: shim_times.append(time.monotonic() - t0))
        try:
            deadline = time.monotonic() + 1.5
            while time.monotonic() < deadline and len(bat_times) < 3:
                time.sleep(0.02)
            assert len(bat_times) >= 3 and bat_times[0] < 0.3, bat_times
            assert len(shim_times) >= 1 and shim_times[0] < 0.3, shim_times
        finally:
            source.close()

        # 4) Shim 300 ms motor cutoff check via fake TUTK RDT_Write log.
        cutoff_log = Path(tmp) / "log_cutoff.txt"
        proc = subprocess.Popen(
            [str(shim_path)],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            env={**base_env, "FAKE_TUTK_LOG": str(cutoff_log), "FAKE_TUTK_KEEP_OPEN": "1"},
        )
        try:
            # Wait for initial 0xFE and 0xFD (link up, event=1, rdt_ok=1, cutoffs=0).
            events = []
            for _ in range(2):
                hdr = proc.stdout.read(5)
                assert len(hdr) == 5
                plen = struct.unpack("<I", hdr[:4])[0]
                pl = proc.stdout.read(plen)
                events.append((hdr[4], pl))
            assert events[0][0] == 0xFE
            assert events[1][0] == 0xFD and struct.unpack("<BBH", events[1][1]) == (1, 1, 0)

            # Build MAVLink v1 motorFrame(ly=0.35, rx=0.0, seq=1) wrapped in shimControlPacket(0, ...).
            def crc_x25(d, extra):
                c = 0xFFFF
                for b in list(d) + [extra]:
                    tmp_b = b ^ (c & 0xFF)
                    tmp_b = (tmp_b ^ (tmp_b << 4)) & 0xFF
                    c = ((c >> 8) ^ (tmp_b << 8) ^ (tmp_b << 3) ^ (tmp_b >> 4)) & 0xFFFF
                return c

            mp = struct.pack("<ffffB", 0.0, -0.35, 0.0, 0.0, 0)
            hdr_m = bytes([17, 1, 0, 0, 202])
            mcrc = crc_x25(hdr_m + mp, 211)
            mav = b"\xFE" + hdr_m + mp + struct.pack("<H", mcrc)
            pkt = struct.pack("<IB", 1 + len(mav), 0) + mav
            proc.stdin.write(pkt)
            proc.stdin.flush()

            # Silence for > 300 ms should trigger shim event 3 (cutoff) and 2 zero RDT writes.
            cutoff_ev = None
            deadline = time.monotonic() + 2.0
            while time.monotonic() < deadline:
                hdr = proc.stdout.read(5)
                if len(hdr) < 5:
                    break
                plen = struct.unpack("<I", hdr[:4])[0]
                pl = proc.stdout.read(plen)
                if hdr[4] == 0xFD:
                    ev = struct.unpack("<BBH", pl)
                    if ev[0] == 3:
                        cutoff_ev = ev
                        break
            assert cutoff_ev == (3, 1, 1), cutoff_ev
            log_lines = [ln for ln in cutoff_log.read_text(encoding="utf-8").splitlines() if "rdt_write" in ln]
            assert len(log_lines) == 3, log_lines
            t_start = int(log_lines[0].split()[0])
            t_zero1 = int(log_lines[1].split()[0])
            assert abs(float(log_lines[0].split()[3]) - 0.35) < 0.01
            assert float(log_lines[1].split()[3]) == 0.0 and float(log_lines[2].split()[3]) == 0.0
            assert 260 <= (t_zero1 - t_start) <= 600, (t_zero1 - t_start, log_lines)
        finally:
            proc.stdin.close()
            proc.terminate()
            proc.wait(timeout=2)

        # 5) Under ORI_SLOW=1, verify >10s run has zero periodic ioctl 1ff/9936 restarts and 60s frame stability.
        if os.environ.get("ORI_SLOW") == "1":
            slow_log = Path(tmp) / "log_slow10s.txt"
            proc10 = subprocess.Popen(
                [str(shim_path)], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                env={**base_env, "FAKE_TUTK_LOG": str(slow_log), "FAKE_TUTK_KEEP_OPEN": "1"},
            )
            try:
                time.sleep(11.2)
            finally:
                proc10.stdin.close()
                proc10.terminate()
                proc10.wait(timeout=2)
            ioctls = [ln.split()[2] for ln in slow_log.read_text(encoding="utf-8").splitlines()
                      if "ioctl " in ln]
            assert ioctls.count("1ff") == 1 and ioctls.count("9936") == 1, ioctls


def test_single_clock_and_undock_safety():
    import os
    import struct
    import time
    from client import BRIDGE_BINARY
    from fixtures import HEIGHT, WIDTH, color_scene
    from server import Demo

    # 1) Single monotonic receipt clock (no 60 ms clamp): stalled video expires to camera-stale/LOST
    #    within 650 ms + tick even while battery & shim updates continue.
    with ReplayClient(binary=BRIDGE_BINARY, args=[]) as client:
        frame_ts = [Demo.now() - 200]
        frame_px = [color_scene(REFERENCE)]
        stub_video = type("StubVideo", (), {
            "read_latest": staticmethod(lambda: (frame_px[0], WIDTH, HEIGHT, frame_ts[0])),
        })()
        demo = Demo(client, video=stub_video, synthetic_dock=False, live_hardware=True)
        demo.on_shim(1, 1, 0)
        demo.on_battery(90, 0)
        demo.capture()
        # Raw mono timestamp (-200 ms) must not be clamped to -60 ms.
        assert demo.frame_received == frame_ts[0], (demo.frame_received, frame_ts[0])
        sel = demo.action("select", {
            "driver": OWNER, "boot": demo.boot, "generation": demo.status["generation"],
            "frame": demo.seq, "box": [120 / 320, 65 / 180, 80 / 320, 80 / 180],
        })
        assert sel["accepted"] and sel["state"] == "TRACKING"
        # Freeze frames while battery & shim updates keep arriving.
        lost_at = None
        deadline = time.monotonic() + 1.2
        while time.monotonic() < deadline:
            demo.on_battery(90, 0)
            demo.on_shim(2, 1, 0)
            demo.capture()
            if demo.status.get("state") == "LOST" and lost_at is None:
                lost_at = time.monotonic()
            if demo.status.get("state") == "LOST" and demo.status.get("manual_mode") == "blocked":
                break
            time.sleep(0.05)
        assert demo.status["state"] == "LOST" and demo.status["reason"] == "camera-stale", demo.status
        assert demo.status["manual_mode"] == "blocked", demo.status

    # 2) Sequence ordering rejects delayed move after release, and RDT down blocks drive.
    with ReplayClient(binary=BRIDGE_BINARY, args=[]) as client:
        rgb = color_scene(REFERENCE)
        stub_video = type("StubVideo", (), {
            "read_latest": staticmethod(lambda: (rgb, WIDTH, HEIGHT, Demo.now())),
        })()
        demo = Demo(client, video=stub_video, synthetic_dock=False, live_hardware=True)
        t0 = Demo.now()
        with demo.lock:
            demo.record(demo.replay.call("battery-telemetry", t0 - 2000, 92, 1))
        demo.on_shim(1, 1, 0)
        demo.on_battery(92, 1)
        demo.capture()
        assert demo.status["dock"] == "charging", demo.status
        seq_base = 5000
        stop_out = demo.action("manual", {
            "driver": OWNER, "boot": demo.boot, "generation": demo.status["generation"],
            "ly": 0.0, "rx": 0.0, "ttl": 250, "sequence": seq_base + 1,
        })
        assert stop_out["accepted"]
        late_move = demo.action("manual", {
            "driver": OWNER, "boot": demo.boot, "generation": demo.status["generation"],
            "ly": 0.30, "rx": 0.0, "ttl": 250, "sequence": seq_base,
        })
        assert not late_move["accepted"] and late_move["drive_writes"] == 0
        demo.on_shim(2, 0, 0)  # RDT down
        rdt_blocked = demo.action("manual", {
            "driver": OWNER, "boot": demo.boot, "generation": demo.status["generation"],
            "ly": 0.30, "rx": 0.0, "ttl": 250, "sequence": seq_base + 2,
        })
        assert not rdt_blocked["accepted"] and rdt_blocked.get("control_error") == "rdt-disconnected"

    # 3) Missing battery or RDT down in ori-bridge keeps drive blocked; 260 ms poll cutoff emits zero packet.
    r_fd, w_fd = os.pipe()
    try:
        with ReplayClient(binary=BRIDGE_BINARY, args=["--control-fd", str(w_fd)], pass_fds=(w_fd,)) as client:
            os.close(w_fd)
            w_fd = -1
            s = Scenario(client)
            s.frame(REFERENCE)
            # Missing battery keeps manual_mode blocked.
            assert s.status["manual_mode"] == "blocked"
            assert not s.run("manual-drive", 1, 0.30, 0.0, 250)["accepted"]
            # RDT disconnected keeps manual_mode blocked even with battery.
            assert s.run("battery-telemetry", 91, 0)["manual_mode"] == "drive"
            rdt_down = s.run("shim-status", 0, 0)
            assert rdt_down["manual_mode"] == "blocked" and not rdt_down["rdt_connected"]
            assert not s.run("manual-drive", 1, 0.30, 0.0, 250)["accepted"]
            rdt_up = s.run("shim-status", 1, 0)
            assert rdt_up["rdt_connected"] and not rdt_up["cutoff_verified"]
            assert rdt_up["manual_mode"] == "drive"
            # Trigger 260 ms bridge poll cutoff by sleeping >300 ms with manual active.
            assert s.run("manual-drive", 1, 0.30, 0.0, 250)["manual_active"]
            time.sleep(0.32)
            s.now += 320
            after_cut = s.run("tick")
            assert not after_cut["manual_active"]
            assert after_cut["shim_cutoffs"] >= 1 and after_cut["cutoff_verified"]
        os.set_blocking(r_fd, False)
        raw = os.read(r_fd, 4096)
        axes = []
        off = 0
        while off + 5 <= len(raw):
            plen = struct.unpack_from("<I", raw, off)[0]
            if off + 4 + plen > len(raw):
                break
            if raw[off + 4] == 0 and plen >= 26 and raw[off + 5] == 0xFE and raw[off + 10] == 202:
                neg_ly, rx = struct.unpack_from("<ff", raw, off + 15)
                axes.append((round(-neg_ly, 2), round(rx, 2)))
            off += 4 + plen
        assert len(axes) >= 2 and axes[0][0] > 0.0 and axes[-1] == (0.0, 0.0), axes
    finally:
        if w_fd >= 0:
            os.close(w_fd)
        os.close(r_fd)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--case", choices=["retreat"])
    parser.add_argument("--bridge", action="store_true")
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
        test_home_failed_clears_on_select_and_stop, test_home_stopping_timeout,
        test_invalid_zero_driver_stop,
        retreat, lambda s: retreat(s, track_only=True),
        test_camera, test_disconnect, test_frozen_preview, test_ownership,
        test_stale_calibration, test_frozen_selection,
        test_overlapping_inhibits, test_track_cancels_pending_follow,
        test_turn, test_motion_expiry, test_selection, test_heartbeat_before_selection, test_track_owner_expiry,
        test_smart_memory_and_autodock,
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
    test_status_without_video()
    test_camera_and_patrol_server()
    test_autospot_and_ownership()
    test_frame_rgb_replay_and_video()
    if args.bridge:
        test_ori_bridge_pipes()
        test_rdt_battery_packets()
        test_battery_charging_and_docked_selection()
        test_video_freshness_and_shim()
        test_single_clock_and_undock_safety()
    print(f"{len(tests) + 4 + 5 * args.bridge} replay and transport scenarios passed")


if __name__ == "__main__":
    main()
