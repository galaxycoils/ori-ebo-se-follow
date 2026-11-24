# Ori EBO SE Follow

A desktop-tested C++ prototype of on-device pet tracking and optional supervised Follow for the original Enabot EBO SE. Device installation, live pet footage, and physical movement are pending.

## Run the replay and phone demo

Install GNU Make, a C++17 compiler, and Python 3.9 or later. From the repository root run

```sh
make test
make replay CASE=retreat
make demo
```

The retreat fixture finishes with `HOLD → FORWARD → HOLD`. Open `http://127.0.0.1:8765` for the loopback demo. Tap the center of the textured target to start Track only. For a custom box, drag tightly around it and confirm. Set the gap while the target is still. Press **Follow** to watch a simulated turn, advance, and stop. In **Track only**, use **Move demo target** to see the tracker move the box while simulated wheels hold. If a frozen selection expires, the preview returns to live video so you can tap again. **Stop** ends the session and clears the selection and gap.

The simulator uses generated images and wheel decisions. It needs no robot, SDK, private footage, or network service.

With Playwright and Chromium installed, run `make test-browser` to start an isolated demo, exercise the phone controls, and shut it down automatically. Set `CHROMIUM` if the local binary is at a path other than `/usr/bin/chromium`.

## Evaluate private footage locally

Install FFmpeg and put clips and annotations under the ignored `private/` directory. Use a short, uninterrupted clip with the whole pet in view. Prepare one JSON entry per frame after resampling to 320 × 180 at 10 FPS. Each entry is `[x, y, width, height]` in image pixels, or `null` when the pet is not visible. The first entry is the initial selection and must name a visible box. Use separate clips for tuning and final evaluation.

```sh
make
python3 tests/evaluate.py private/walk.mp4 private/walk.json
```

The evaluator reports the fraction of visible frames with intersection-over-union of at least 0.5 and passes at 90%. The generated replay tests exercise turns, Track only, target loss, ambiguity, interrupted sessions, delayed starts and stale frames. Use the real clips to evaluate the camera angle and appearance changes seen on Ori.

## Device and tracking notes

The pure C++ core accepts frames, owner requests, and an injected monotonic clock. It emits decisions with their authorizing frame and expiry. Tracking uses an appearance match around the previous box, anchored to the initial selection. Perspective changes, occlusion, and visually similar animals can cause loss. The current gap estimate uses box size and the bottom edge, so the marked-floor trials are required to establish distance and speed.

See [the device runbook](docs/device.md) for the compatibility evidence, interfaces, and acceptance tests, and [the task ledger](docs/status.md) for the current milestone.
