// GitHub Pages runs the native replay protocol in WebAssembly in each tab.
import createReplay from "./ori.mjs";
import fixtures from "./fixtures.mjs";

const ACTIONS = new Set(["select", "teach", "follow", "heartbeat", "track", "stop", "move", "home", "lift"]);

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
  rgba.data.fill(255);
  const waiters = [];
  let activeBuffer = 0;
  let frame = 0;
  let revision = 0;
  let status;
  let dockStarted = null;
  let box = reference;
  let poses = [][Symbol.iterator]();
  let pixels = pixelBuffers[0];
  let timer = null;

  function notifyWaiters() {
    while (waiters.length) waiters.shift()();
  }

  function run(command, now, ...args) {
    const result = replay.ccall("ori_command", "string", ["string"], [[command, now, ...args].join(" ")]);
    if (!result) throw new Error("Simulator command rejected");
    status = {...JSON.parse(result), boot, revision: ++revision};
    if (status.dock_action === "start") dockStarted = now;
    if (status.dock_action === "cancel") dockStarted = null;
    return status;
  }

  function capture() {
    const now = Math.floor(performance.now());
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
      return {
        frame, tag: frame, status,
        image: () => {
          const data = rgba.data;
          for (let i = 0, j = 0; i < source.length; i++, j += 4) {
            data[j] = data[j + 1] = data[j + 2] = source[i];
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
      const args = [driver, generation];
      if (action === "select" || action === "heartbeat") {
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
