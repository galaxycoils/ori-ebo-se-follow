"""Measure box overlap on local footage. No images are written or uploaded."""

import argparse
import json
import os
import shutil
import subprocess
import tempfile
from pathlib import Path

from replay import Scenario
from client import ReplayClient
from fixtures import HEIGHT, WIDTH, color_scene, scene


def find_ffmpeg():
    for candidate in (os.environ.get("FFMPEG"), shutil.which("ffmpeg")):
        if candidate and os.path.isfile(candidate) and os.access(candidate, os.X_OK):
            return candidate
    return None


def overlap(box, label):
    if box is None:
        return 0.0
    x, y, w, h = box
    a, b, c, d = label
    intersection = max(0, min(x + w, a + c) - max(x, a)) * max(0, min(y + h, b + d) - max(y, b))
    return intersection / (w * h + c * d - intersection)


def labels_from(path):
    content = json.loads(path.read_text())
    if not isinstance(content, list) or not content or content[0] is None:
        raise ValueError("Provide a nonempty list of boxes with an initial selection.")
    for index, box in enumerate(content):
        if box is None:
            continue
        if not isinstance(box, list) or len(box) != 4 or any(type(n) is not int for n in box):
            raise ValueError("Each annotation must be a list of four integers or null.")
        x, y, w, h = box
        if not (0 <= x < x + w <= WIDTH and 0 <= y < y + h <= HEIGHT):
            raise ValueError("Each annotated box must fit within the 320 × 180 image.")
        if index == 0 and (x == 0 or y == 0 or x + w == WIDTH or y + h == HEIGHT):
            raise ValueError("Choose an initial frame with the entire pet in view.")
    return content


def evaluate(clip, labels, frame_size, rgb=False):
    ffmpeg = find_ffmpeg()
    if not ffmpeg:
        raise RuntimeError("FFmpeg executable not found")
    pix_fmt = "rgb24" if rgb else "gray"
    decoder = subprocess.Popen([
        ffmpeg, "-nostdin", "-v", "error", "-i", str(clip), "-vf",
        f"fps=10,scale=320:180,format={pix_fmt}", "-an", "-f", "rawvideo", "pipe:1",
    ], stdout=subprocess.PIPE)
    visible = 0
    missed = []
    try:
        with ReplayClient() as client:
            scenario = Scenario(client)
            for index, label in enumerate(labels, 1):
                pixels = decoder.stdout.read(frame_size)
                if len(pixels) != frame_size:
                    raise ValueError("The clip has fewer frames than labels.")
                scenario.now = index * 100
                if rgb:
                    status = scenario.run("frame-rgb", index, scenario.now, WIDTH, HEIGHT, 0, pixels=pixels)
                else:
                    status = scenario.run("frame", index, scenario.now, pixels=pixels)
                if index == 1:
                    status = scenario.control("select", index, *label)
                    if not status["accepted"]:
                        raise ValueError("The initial selection could not be tracked.")
                if status["owner"]:
                    scenario.control("heartbeat", index)
                if label is not None:
                    visible += 1
                    if overlap(status["box"], label) < 0.5:
                        missed.append(index)
            if decoder.stdout.read(1):
                raise ValueError("The clip has more frames than labels.")
        if decoder.wait() != 0:
            raise ValueError("FFmpeg did not decode the clip successfully.")
    finally:
        decoder.stdout.close()
        if decoder.poll() is None:
            decoder.terminate()
            decoder.wait()
    return missed, visible


def self_test():
    assert overlap(None, [10, 10, 20, 20]) == 0.0
    assert overlap([10, 10, 20, 20], [10, 10, 20, 20]) == 1.0
    assert abs(overlap([10, 10, 20, 20], [20, 10, 20, 20]) - (1 / 3)) < 1e-9

    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        for bad in ("[]", "[null]", "[[1,2,3]]", "[[0,10,20,20]]", "[[10,10,311,20]]"):
            path = root / "bad.json"
            path.write_text(bad)
            try:
                labels_from(path)
            except ValueError:
                pass
            else:
                raise AssertionError(f"Expected validation error for {bad}")

        ffmpeg = find_ffmpeg()
        if not ffmpeg:
            print("Evaluator unit self-test passed (FFmpeg not installed; skipped clip decode)")
            return 0

        labels = [[120 - i * 2, 65, 72 + (i % 3) * 2, 72 + (i % 3) * 2] for i in range(25)]
        raw_bytes = b"".join(bytes(scene(tuple(box))) for box in labels)
        clip = root / "synthetic.mp4"
        labels_path = root / "synthetic.json"
        labels_path.write_text(json.dumps(labels))
        subprocess.run([
            ffmpeg, "-y", "-v", "error", "-f", "rawvideo", "-pix_fmt", "gray",
            "-s", f"{WIDTH}x{HEIGHT}", "-r", "10", "-i", "pipe:0",
            "-c:v", "mpeg4", "-q:v", "2", str(clip),
        ], input=raw_bytes, check=True)
        missed, visible = evaluate(clip, labels_from(labels_path), WIDTH * HEIGHT)
        assert visible == len(labels) and not missed, (missed, visible)

        rgb_bytes = b"".join(color_scene(tuple(box)) for box in labels)
        rgb_clip = root / "synthetic_rgb.mp4"
        subprocess.run([
            ffmpeg, "-y", "-v", "error", "-f", "rawvideo", "-pix_fmt", "rgb24",
            "-s", f"{WIDTH}x{HEIGHT}", "-r", "10", "-i", "pipe:0",
            "-c:v", "libx264", "-pix_fmt", "yuv420p", "-crf", "18", str(rgb_clip),
        ], input=rgb_bytes, check=True)
        rgb_missed, rgb_visible = evaluate(rgb_clip, labels_from(labels_path), WIDTH * HEIGHT * 3, rgb=True)
        assert rgb_visible == len(labels) and (rgb_visible - len(rgb_missed)) / rgb_visible >= 0.9, (
            rgb_missed, rgb_visible)
        print(f"Evaluator self-test passed ({visible}/{visible} gray, "
              f"{rgb_visible - len(rgb_missed)}/{rgb_visible} RGB synthetic MP4 frames >= 0.5 IoU)")
    return 0


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--rgb", action="store_true")
    parser.add_argument("clip", nargs="?", type=Path)
    parser.add_argument("labels", nargs="?", type=Path)
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    if args.clip is None or args.labels is None:
        parser.error("Provide <clip> and <labels> or pass --self-test")
    labels = labels_from(args.labels)
    frame_size = WIDTH * HEIGHT * (3 if args.rgb else 1)
    missed, visible = evaluate(args.clip, labels, frame_size, rgb=args.rgb)
    successes = visible - len(missed)
    fraction = successes / visible
    print(f"{len(labels)} labeled frames ({visible} visible)")
    print(f"{successes}/{visible} visible frames reached IoU >= 0.5 ({fraction:.1%})")
    if missed:
        print("Visible frames below threshold (1-based):", ", ".join(map(str, missed)))
    return 0 if fraction >= .9 else 1


if __name__ == "__main__":
    raise SystemExit(main())
