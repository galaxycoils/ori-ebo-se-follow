"""Measure box overlap on local footage. No images are written or uploaded."""

import argparse
import json
import subprocess
from pathlib import Path

from replay import Scenario
from client import ReplayClient
from fixtures import HEIGHT, WIDTH


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
    for box in content:
        if box is None:
            continue
        if not isinstance(box, list) or len(box) != 4 or any(type(n) is not int for n in box):
            raise ValueError("Each annotation must be a list of four integers or null.")
        x, y, w, h = box
        if not (0 < x < x + w < WIDTH and 0 < y < y + h < HEIGHT):
            raise ValueError("Each annotated box must fit within the 320 × 180 image.")
    return content


def evaluate(clip, labels, frame_size):
    decoder = subprocess.Popen([
        "ffmpeg", "-nostdin", "-v", "error", "-i", str(clip), "-vf",
        "fps=10,scale=320:180,format=gray", "-an", "-f", "rawvideo", "pipe:1",
    ], stdout=subprocess.PIPE)
    successes = visible = 0
    try:
        with ReplayClient() as client:
            scenario = Scenario(client)
            for index, label in enumerate(labels, 1):
                pixels = decoder.stdout.read(frame_size)
                if len(pixels) != frame_size:
                    raise ValueError("The clip has fewer frames than labels.")
                scenario.now = index * 100
                status = scenario.run("frame", index, scenario.now, pixels=pixels)
                if index == 1:
                    status = scenario.control("select", index, *label)
                    if not status["accepted"]:
                        raise ValueError("The initial selection could not be tracked.")
                if status["owner"]:
                    scenario.control("heartbeat", index)
                if label is not None:
                    visible += 1
                    successes += overlap(status["box"], label) >= 0.5
            if decoder.stdout.read(1):
                raise ValueError("The clip has more frames than labels.")
        if decoder.wait() != 0:
            raise ValueError("FFmpeg did not decode the clip successfully.")
    finally:
        decoder.stdout.close()
        if decoder.poll() is None:
            decoder.terminate()
            decoder.wait()
    return successes, visible


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("clip", type=Path)
    parser.add_argument("labels", type=Path)
    args = parser.parse_args()
    labels = labels_from(args.labels)
    successes, visible = evaluate(args.clip, labels, WIDTH * HEIGHT)
    fraction = successes / visible
    print(f"{successes}/{visible} visible frames reached IoU >= 0.5 ({fraction:.1%})")
    return 0 if fraction >= .9 else 1


if __name__ == "__main__":
    raise SystemExit(main())
