# Ori EBO SE Follow

A desktop-tested C++ prototype of on-device pet tracking and optional supervised Follow for the original Enabot EBO SE. Device installation, live pet footage, and physical movement are pending.

[Open the GitHub Pages simulator](https://galaxycoils.github.io/ori-ebo-se-follow/). The public page runs the same C++ replay core in WebAssembly with generated frames. All movement and charging on that page are simulated.

## Open live Ori on the Mac

With the existing bridge configured, close ROLA and run `make live`. Open [Live Ori](http://127.0.0.1:8766/) on this Mac. Open **Manual drive** for the joystick. While charging, the joystick accepts straight movement only. See the [bridge setup](docs/bridge.md) for requirements.

## Run the replay and phone demo

Install GNU Make, a C++17 compiler, and Python 3.9 or later. From the repository root run

```sh
make test
make test-asan
make bench
make replay CASE=retreat
make demo
```

The retreat fixture finishes with `HOLD → FORWARD → HOLD`. `make bench` runs `build/ori-probe --synthetic 18000` (an accelerated 30-minute @ 10 FPS soak check verifying zero missed 100 ms deadlines and zero wheel commands). Open `http://127.0.0.1:8765` for the loopback demo. Quick-tap the target to lock a centered 44×44 box on that frame (uncertain taps open an adjustable reticle), or drag a box (`>= 16 px` per axis), move or corner-resize the draft, and choose **Confirm selection** to start Track only. With keyboard focus on the preview, **Enter** or **Space** opens a centered box and a second press confirms. **Escape** cancels. The **Dark UI / Light UI** button persists UI theme independently of camera Night mode, and tool tabs switch panels exclusively above the controls. Set the gap while the target is still. Press **Start Follow** to watch a simulated turn, advance, and stop. In **Track only**, use **Move demo target** to see the tracker move the box while simulated wheels hold. The preview stays live while you adjust the selection. **Stop** ends the session and clears the selection and gap.

Serve Ori on the Mac mini using a trusted TLS certificate for phone installation and owner access. The video option accepts a camera URL or a local file for camera-only tracking. Open **Diagnostics** to see recent processing, presentation, and control timings. Live preview age there starts at decoded-frame receipt.

```sh
python3 demo/server.py --bind 0.0.0.0 --require-auth --tls-cert /path/to/cert.pem --tls-key /path/to/key.pem
python3 demo/server.py --video private/walk.mp4
```

Open the printed owner pairing URL. Install Ori from the browser’s **Add to Home Screen** or **Install app** menu. **Family viewing** creates one revocable read-only link when the server uses owner authentication. A new link replaces the previous link, and restarting the server ends sharing.

Use **Camera clips** for snapshots, local playback, downloadable recordings up to 60 seconds, and on-screen motion alerts. To save a selected pet across restarts, start the local server with `--memory-file private/target_memory.json`. With auto-remember enabled, three consistent frames can restore tracking after a lock is lost. Tap **Start Follow** to resume driving.

The simulator uses generated images and wheel decisions. It needs no robot, SDK, private footage, or network service.

With Playwright and Chromium installed, run `make test-browser` to exercise the loopback demo. Set `CHROMIUM` to choose a browser executable, or install the matching Playwright WebKit and run `ORI_TEST_WEBKIT=1 make test-browser`. With Emscripten 4.0.23 active, run `make pages` to build the static site and `make test-pages` to exercise it. GitHub Actions runs the native replay and both browser checks before publishing.

## Patrol a preset area

- **Area auto-patrol (Live Ori & Simulator)**. Open **Patrol area**, accept the default zone or click **Set patrol area** to pick two opposite corners on the map, select laps (`1 lap`, `3 laps`, or `Continuous`), and choose **Start auto patrol** / **Start simulator patrol**. Optionally enable **Pause on pet** (stops and locks on if a pet is spotted) or **Dock when done**. Ori undocks if charging, patrols each edge with visual stuck/obstacle recovery, and stops immediately on **End patrol**, **Stop**, or manual joystick takeover.
- **Checkpoints & Factory roving**. Switch **Mode** in **Patrol area** to **Checkpoints** to teach named stops with recorded drive legs for autonomous replay, or **Factory roving** on Live Ori to toggle the EBO SE onboard security patrol.

## Color camera and Night Mode

Live Ori and the demo render full-color video (`640×360` live, `320×180` tracker luma) with toolbar controls for **Color / B&W** and **Night mode**:
- **Day**: Full-color daylight view with hardware IR disengaged.
- **Auto night**: Monitors ambient luminance with hysteresis to engage/disengage EBO SE IR night vision automatically.
- **Night IR**: Engages hardware infrared LEDs and IR-CUT filter with selectable **Crisp B&W** or **Night green** styling.
- **Low-light boost**: Keeps color active in dim rooms while lifting shadows for preview and pet tracking.

## Return to the charging base

Use **Return to Dock** to end Follow and hand control to the factory docking routine in the simulator. The page shows return progress, contact verification and confirmed charging. Stop cancels an active request. **Lift off base (demo)** resets the simulated charger for another run. Low-battery returns initiated by the factory are observed without sending a duplicate start.

Open **Dock route** while charging and choose **Teach dock**. Capture the first landmark while stationary, off the base, and facing a clear docking approach. Capture distinct dock-facing views farther along the same room, then save. **Guide me home** recognizes taught views and prompts you to steer through the route. The app enables **Dock from here** after three matches at the taught approach. Landmarks stay in the phone browser.

For the original SE, Enabot specifies a clear approach within two metres in front of the base. Physical return reliability and charging time still require a comparison on Ori. [Enabot EBO SE](https://www.enabot.com/home-robot/ebo-se) · [Bench procedure](docs/device.md#return-to-charge-verification)

## Evaluate private footage locally

Install FFmpeg and put clips and annotations under the ignored `private/` directory. Use a short, uninterrupted clip with the whole pet in view. Prepare one JSON entry per frame after resampling to 320 × 180 at 10 FPS. Each entry is `[x, y, width, height]` in image pixels, or `null` when the pet is not visible. The first entry is the initial selection and must name a visible box. Use separate clips for tuning and final evaluation.

```sh
make
python3 tests/evaluate.py private/walk.mp4 private/walk.json
```

The evaluator reports the count of labeled frames, the fraction of visible frames with intersection-over-union of at least 0.5, and the frame numbers below threshold. Use 90% as the threshold for ordinary unobstructed held-out clips. Record occlusion and two-pet crossings separately and keep any loss or suspect identity switch in the results. The generated replay tests exercise turns, Track only, target loss, ambiguity, interrupted sessions, delayed starts and stale frames.

## Device and tracking notes

The pure C++ core accepts frames, owner requests, and an injected monotonic clock. It emits decisions with their authorizing frame and expiry. Tracking uses an appearance match around the previous box, anchored to the initial selection. Perspective changes, occlusion, and visually similar animals can cause loss. The current gap estimate uses box size and the bottom edge, so the marked-floor trials are required to establish distance and speed.

See [the device runbook](docs/device.md) for the compatibility evidence, interfaces, and acceptance tests, and [the task ledger](docs/status.md) for the current milestone.
