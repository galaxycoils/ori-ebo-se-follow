// GitHub Pages runs the native replay protocol in WebAssembly in each tab.
import createReplay from "./ori.mjs";
import fixtures from "./fixtures.mjs";

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
  let frame = 0;
  let revision = 0;
  let status;
  let dockStarted = null;
  let box = reference;
  let poses = [][Symbol.iterator]();
  let pixels;

  function run(command, now, ...args) {
    const result = replay.ccall("ori_command", "string", ["string"], [[command, now, ...args].join(" ")]);
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
    pixels = new Uint8Array(width * height).fill(34);
    for (let row = 0; row < size; row++) {
      pixels.set(patch.subarray(row * size, (row + 1) * size), (y + row) * width + x);
    }
    const elapsed = now - dockStarted;
    const signal = dockStarted === null ? "idle" : elapsed < 1200 ? "returning" : elapsed < 1800 ? "contact" : "charging";
    run("dock-status", now, signal);
    replay.HEAPU8.set(pixels, replay._ori_pixels());
    run("frame", now, ++frame, now);
  }

  capture();
  setInterval(capture, 100);
  return {
    async frame(previous) {
      if (previous === frame) await new Promise(resolve => setTimeout(resolve, 100));
      if (previous === frame) return null;
      const source = pixels;
      return {
        frame, tag: frame, status,
        image: () => {
          const image = new ImageData(width, height);
          for (let i = 0, j = 0; i < source.length; i++, j += 4) {
            image.data[j] = image.data[j + 1] = image.data[j + 2] = source[i];
            image.data[j + 3] = 255;
          }
          return createImageBitmap(image);
        },
        discard: () => {},
      };
    },
    async control(action, payload) {
      const {driver, generation} = payload;
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
      if (action === "select" || action === "heartbeat") args.push(payload.frame);
      if (action === "select") args.push(...payload.box.map((value, i) => Math.round(value * (i % 2 ? height : width))));
      const result = run(action, now, ...args);
      if (action === "follow" && result.accepted) poses = movement(box);
      return result;
    },
  };
}
