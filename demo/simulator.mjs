// GitHub Pages runs the native replay protocol in WebAssembly in each tab.
import createReplay from "./ori.mjs";
import fixtures from "./fixtures.mjs";

const ACTIONS = new Set(["select", "teach", "follow", "heartbeat", "track", "stop", "move", "home", "lift", "resume", "forget", "spot", "policy", "manual", "motors", "camera", "patrol", "cutoff-test", "clear-route"]);

const BOOST_LUT = Uint8Array.from({length: 256}, (_, i) =>
  Math.max(0, Math.min(255, Math.round(((i / 255) ** 0.62) * 242 + (i - 128) * 0.12 + 8)))
);

function* movement(start) {
  for (const [end, steps] of fixtures.legs) {
    for (let step = 1; step <= steps; step++) {
      const t = step / steps;
      const weight = t * t * (3 - 2 * t);
      yield start.map((value, i) => Math.round(value + (end[i] - value) * weight));
    }
    start = end;
  }
}

export async function createSimulator() {
  const {width, height, reference, smallest, largest} = fixtures;
  const [replay, textureData] = await Promise.all([
    createReplay(),
    fetch(new URL("./textures.bin", import.meta.url)).then(response => {
      if (!response.ok) throw new Error("Simulator textures unavailable");
      return response.arrayBuffer();
    }),
  ]);
  const textures = new Map();
  for (let size = smallest, offset = 0; size <= largest; size++) {
    textures.set(size, new Uint8Array(textureData, offset, size * size));
    offset += size * size;
  }

  const boot = crypto.randomUUID();
  const pixelBuffers = [new Uint8Array(width * height), new Uint8Array(width * height)];
  const rgba = new ImageData(width, height);
  const manualSequence = new Map();
  rgba.data.fill(255);
  const waiters = [];
  let activeBuffer = 0;
  let frame = 0;
  let capturedAt = 0;
  let revision = 0;
  let status;
  let dockStarted = null;
  let box = reference;
  let poses = [][Symbol.iterator]();
  let pixels = pixelBuffers[0];
  let timer = null;
  const camera = {
    color_mode: "color",
    night_mode: "off",
    ir_tint: "clean",
    night_active: false,
    boost_active: false,
    eyes: true,
    avoid: true,
    fall: true,
  };
  let patrol = null;
  let invertTurn = false;
  let patrolMessage = "Set an area on the map for auto patrol.";

  function patrolSnapshot() {
    if (!patrol) {
      return {
        active: false,
        kind: "area",
        phase: "idle",
        lap: 0,
        laps: 1,
        side: 0,
        position: null,
        message: patrolMessage,
      };
    }
    return {
      active: true,
      kind: patrol.kind,
      phase: patrol.phase,
      lap: patrol.lap + 1,
      laps: patrol.laps,
      side: patrol.side + 1,
      position: patrol.position,
      message: patrolMessage,
    };
  }

  function stopPatrol(msg = "Patrol ended.") {
    if (!patrol) return;
    patrol = null;
    patrolMessage = msg;
  }

  function stepPatrol(now) {
    if (!patrol) return;
    if (patrol.kind === "area") {
      const [left, top, right, bottom] = patrol.area;
      const corners = invertTurn
        ? [[left, top], [left, bottom], [right, bottom], [right, top], [left, top]]
        : [[left, top], [right, top], [right, bottom], [left, bottom], [left, top]];
      if (patrol.phase === "undock") {
        dockStarted = null;
        run("dock-status", now, "idle");
        patrol.phase = "leg";
        patrol.phaseAt = now;
        patrol.position = corners[0];
        patrolMessage = `Lap ${patrol.lap + 1}${patrol.laps ? ` of ${patrol.laps}` : ""} · patrolling side 1`;
        return;
      }
      if (patrol.phase === "leg") {
        const dur = 500;
        const frac = Math.min(1, (now - patrol.phaseAt) / dur);
        const a = corners[patrol.side];
        const b = corners[patrol.side + 1];
        patrol.position = [a[0] + (b[0] - a[0]) * frac, a[1] + (b[1] - a[1]) * frac];
        if (frac >= 1) {
          patrol.phase = "turn";
          patrol.phaseAt = now;
          patrolMessage = `Lap ${patrol.lap + 1}${patrol.laps ? ` of ${patrol.laps}` : ""} · corner ${patrol.side + 1}`;
        }
        return;
      }
      if (patrol.phase === "turn") {
        if (now - patrol.phaseAt >= 200) {
          patrol.phase = "scan";
          patrol.phaseAt = now;
        }
        return;
      }
      if (patrol.phase === "scan") {
        if (patrol.spotStop && (status.spotted_box || status.state === "TRACKING" || status.state === "READY")) {
          stopPatrol("Pet spotted · tracking target.");
          return;
        }
        if (now - patrol.phaseAt >= 200) {
          patrol.side += 1;
          if (patrol.side >= 4) {
            patrol.side = 0;
            patrol.lap += 1;
            if (patrol.laps > 0 && patrol.lap >= patrol.laps) {
              const shouldDock = patrol.dockAfter;
              stopPatrol("Auto patrol complete.");
              if (shouldDock && status.can_home) run("home", now, patrol.driver, status.generation);
              return;
            }
          }
          patrol.phase = "leg";
          patrol.phaseAt = now;
          patrolMessage = `Lap ${patrol.lap + 1}${patrol.laps ? ` of ${patrol.laps}` : ""} · patrolling side ${patrol.side + 1}`;
        }
      }
    } else if (patrol.kind === "checkpoints") {
      if (now - patrol.phaseAt >= 450) {
        patrol.index += 1;
        patrol.phaseAt = now;
        if (patrol.index >= patrol.total) {
          stopPatrol("Checkpoint patrol complete.");
        } else {
          patrolMessage = `Checkpoint ${patrol.index + 1} of ${patrol.total}`;
        }
      }
    }
  }

  function notifyWaiters() {
    while (waiters.length) waiters.shift()();
  }

  function run(command, now, ...args) {
    const result = replay.ccall("ori_command", "string", ["string"], [[command, now, ...args].join(" ")]);
    if (!result) throw new Error("Simulator command rejected");
    const parsed = JSON.parse(result);
    status = {
      ...parsed,
      boot,
      revision: ++revision,
      source_mode: "simulator",
      follow_permitted: true,
      rdt_connected: true,
      cutoff_verified: true,
      shim_cutoffs: 0,
      camera: {...camera},
      invert_turn: invertTurn,
      patrol: patrolSnapshot(),
    };
    if (status.dock_action === "start") dockStarted = now;
    if (status.dock_action === "cancel") dockStarted = null;
    return status;
  }

  function capture() {
    const now = Math.floor(performance.now());
    capturedAt = now;
    box = poses.next().value ?? reference;
    const [x, y, size] = box;
    const patch = textures.get(size);
    activeBuffer = 1 - activeBuffer;
    const nextPixels = pixelBuffers[activeBuffer];
    nextPixels.fill(34);
    for (let row = 0; row < size; row++) {
      nextPixels.set(patch.subarray(row * size, (row + 1) * size), (y + row) * width + x);
    }
    pixels = nextPixels;
    const elapsed = now - dockStarted;
    const signal = dockStarted === null ? "idle" : elapsed < 1200 ? "returning" : elapsed < 1800 ? "contact" : "charging";
    run("dock-status", now, signal);
    replay.HEAPU8.set(pixels, replay._ori_pixels());
    run("frame", now, ++frame, now);
    if (patrol) {
      stepPatrol(now);
      status = {...status, revision: ++revision, patrol: patrolSnapshot()};
    }
    notifyWaiters();
  }

  function startLoop() {
    if (timer !== null) return;
    timer = setInterval(() => {
      if (typeof document !== "undefined" && document.hidden) return;
      capture();
    }, 100);
  }

  capture();
  startLoop();
  if (typeof document !== "undefined") {
    document.addEventListener("visibilitychange", () => {
      if (document.hidden) {
        if (timer !== null) {
          clearInterval(timer);
          timer = null;
        }
      } else {
        capture();
        startLoop();
      }
    });
  }

  return {
    async frame(previous) {
      if (previous === frame) {
        await new Promise(resolve => {
          const fallback = setTimeout(() => {
            const index = waiters.indexOf(wake);
            if (index >= 0) waiters.splice(index, 1);
            resolve();
          }, 250);
          function wake() {
            clearTimeout(fallback);
            resolve();
          }
          waiters.push(wake);
        });
      }
      if (previous === frame) return null;
      const source = pixels;
      const cam = {...camera};
      return {
        frame, tag: frame, status, age: performance.now() - capturedAt,
        image: () => {
          const data = rgba.data;
          const nightActive = cam.night_mode === "night";
          const boostActive = cam.night_mode === "boost";
          const bw = cam.color_mode === "bw" || (nightActive && cam.ir_tint !== "emerald");
          const emerald = nightActive && cam.ir_tint === "emerald";
          for (let i = 0, j = 0; i < source.length; i++, j += 4) {
            let v = source[i];
            if (boostActive) v = BOOST_LUT[v];
            if (emerald) {
              data[j] = Math.min(255, Math.round(v * 0.20));
              data[j + 1] = Math.min(255, Math.round(v * 1.10 + 10));
              data[j + 2] = Math.min(255, Math.round(v * 0.34));
            } else if (bw) {
              data[j] = data[j + 1] = data[j + 2] = v;
            } else if (v === 34) {
              const row = Math.floor(i / width);
              const col = i % width;
              if (row < 74) {
                const stripe = Math.floor(col / 48) % 2 === 0 ? 5 : 0;
                const shade = 28 + Math.floor(row * 18 / 74);
                data[j] = shade + 6 + stripe;
                data[j + 1] = shade + 12 + stripe;
                data[j + 2] = shade + 18 + stripe;
              } else {
                const depth = (row - 74) / (height - 74);
                data[j] = Math.round(42 + depth * 28);
                data[j + 1] = Math.round(36 + depth * 22);
                data[j + 2] = Math.round(30 + depth * 16);
              }
            } else {
              data[j] = Math.min(255, Math.round(v * 1.18 + 18));
              data[j + 1] = Math.min(255, Math.round(v * 0.94 + 4));
              data[j + 2] = Math.min(255, Math.round(v * 0.70));
            }
          }
          return createImageBitmap(rgba);
        },
        discard: () => {},
      };
    },
    async control(action, payload) {
      if (!ACTIONS.has(action) || !payload || typeof payload !== "object") {
        throw new Error("Invalid control request");
      }
      const {driver, generation} = payload;
      if (!Number.isSafeInteger(driver) || driver <= 0 || !Number.isSafeInteger(generation) || generation <= 0) {
        throw new Error("Invalid session");
      }
      if (payload.boot !== boot) return {...status, accepted: false};
      const now = Math.floor(performance.now());
      if (action === "camera") {
        if (payload.color_mode === "color" || payload.color_mode === "bw") camera.color_mode = payload.color_mode;
        if (["off", "auto", "night", "boost"].includes(payload.night_mode)) camera.night_mode = payload.night_mode;
        if (payload.ir_tint === "clean" || payload.ir_tint === "emerald") camera.ir_tint = payload.ir_tint;
        if (typeof payload.eyes === "boolean") camera.eyes = payload.eyes;
        if (typeof payload.avoid === "boolean") camera.avoid = payload.avoid;
        if (typeof payload.fall === "boolean") camera.fall = payload.fall;
        camera.night_active = camera.night_mode === "night";
        camera.boost_active = camera.night_mode === "boost";
        status = {...status, revision: ++revision, accepted: true, camera: {...camera}};
        return status;
      }
      if (action === "cutoff-test") {
        status = {...status, revision: ++revision, accepted: true, cutoff_verified: true};
        return status;
      }
      if (action === "patrol") {
        if (payload.mode === "stop") {
          stopPatrol("Patrol ended.");
          status = {...status, revision: ++revision, accepted: true, patrol: patrolSnapshot()};
          return status;
        }
        if (payload.mode === "start") {
          const kind = payload.kind === "checkpoints" ? "checkpoints" : "area";
          const area = Array.isArray(payload.area) && payload.area.length === 4 ? payload.area.map(Number) : [.22, .22, .78, .78];
          const laps = Number.isInteger(payload.laps) ? Math.max(0, Math.min(10, payload.laps)) : 1;
          patrol = {
            kind,
            area,
            laps,
            lap: 0,
            side: 0,
            phase: status.dock === "charging" ? "undock" : "leg",
            phaseAt: now,
            position: [area[0], area[1]],
            spotStop: Boolean(payload.spot_stop),
            dockAfter: Boolean(payload.dock_after),
            driver,
            index: 0,
            total: Math.max(2, Array.isArray(payload.checkpoints) ? payload.checkpoints.length : 2),
          };
          patrolMessage = kind === "area"
            ? `Lap 1${laps ? ` of ${laps}` : ""} · patrolling side 1`
            : `Checkpoint 1 of ${patrol.total}`;
          status = {...status, revision: ++revision, accepted: true, patrol: patrolSnapshot()};
          return status;
        }
        return {...status, accepted: false};
      }
      if (action === "stop") {
        stopPatrol("Patrol ended.");
      }
      if (action === "manual" && (Number(payload.ly) || Number(payload.rx))) {
        stopPatrol("Patrol stopped by manual drive.");
      }
      if (action === "follow" || action === "home") {
        stopPatrol("Patrol ended.");
      }
      if (action === "move" || action === "lift") {
        if (generation !== status.generation) return {...status, accepted: false};
        if (action === "move" && driver === status.owner) {
          poses = movement(box);
          return {...status, accepted: true};
        }
        if (action === "lift" && status.dock === "charging") {
          dockStarted = null;
          return run("dock-status", now, "idle");
        }
        return {...status, accepted: false};
      }
      if (action === "clear-route") {
        return run("clear-route", now);
      }
      if (action === "manual") {
        const ly = Number(payload.ly), rx = Number(payload.rx), ttl = Math.round(Number(payload.ttl ?? 280));
        if (!Number.isFinite(ly) || !Number.isFinite(rx) || ly < -1 || ly > 1 || rx < -1 || rx > 1) throw new Error("Invalid manual drive");
        const {sequence} = payload;
        if (sequence !== undefined) {
          if (!Number.isSafeInteger(sequence) || sequence <= (manualSequence.get(driver) ?? 0)) return {...status, accepted: false};
          manualSequence.set(driver, sequence);
        }
        if ((ly || rx) && generation !== status.generation) return {...status, accepted: false};
        return run("manual-drive", now, driver, ly.toFixed(3), rx.toFixed(3), Math.min(400, Math.max(50, ttl)));
      }
      if (action === "motors") {
        const en = payload.enabled !== undefined ? (payload.enabled ? 1 : 0) : (status.follow_enabled ? 1 : 0);
        const fwd = Number(payload.forward_speed ?? 0.30), turn = Number(payload.turn_speed ?? 0.22);
        const inv = payload.invert_turn ? 1 : 0;
        invertTurn = Boolean(payload.invert_turn);
        return run("set-motors", now, en, fwd.toFixed(3), turn.toFixed(3), inv);
      }
      if (action === "policy") {
        const p = payload.policy || {};
        const keys = ["auto_reacquire", "auto_gap", "auto_spot", "base_watch",
                      "auto_dock_low_battery", "auto_dock_idle", "auto_dock_disconnect"];
        return run("policy", now, ...keys.map(k => p[k] ? 1 : 0), Math.max(1000, Number(p.idle_timeout_ms || 90000)));
      }
      const args = [driver, generation];
      if (action === "resume" || action === "forget" || action === "spot") {
        const cmd = action === "resume" ? "resume-memory" : action === "forget" ? "forget-memory" : "spot-lock";
        return run(cmd, now, ...args);
      }
      if (action === "select" || action === "heartbeat" || action === "follow") {
        if (!Number.isSafeInteger(payload.frame) || payload.frame <= 0 || payload.frame > frame) {
          throw new Error("Invalid frame");
        }
        args.push(payload.frame);
      }
      if (action === "select") {
        if (!Array.isArray(payload.box) || payload.box.length !== 4 ||
            payload.box.some(value => typeof value !== "number" || !Number.isFinite(value) || value < 0 || value > 1)) {
          throw new Error("Invalid box");
        }
        args.push(...payload.box.map((value, i) => Math.round(value * (i % 2 ? height : width))));
      }
      const result = run(action, now, ...args);
      if (action === "follow" && result.accepted) poses = movement(box);
      return result;
    },
  };
}
