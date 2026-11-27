"use strict";

// Normalized thumbnails tolerate global brightness changes and require room texture.
function roomSignature(image, context) {
  context.drawImage(image, 0, 0, 16, 9);
  const pixels = context.getImageData(0, 0, 16, 9).data;
  const samples = Array.from({length: 144}, (_, i) => Math.round(0.299 * pixels[4 * i] + 0.587 * pixels[4 * i + 1] + 0.114 * pixels[4 * i + 2]));
  const mean = samples.reduce((sum, value) => sum + value, 0) / samples.length;
  const centered = samples.map(value => value - mean);
  const norm = Math.sqrt(centered.reduce((sum, value) => sum + value * value, 0));
  return norm >= 144 ? centered.map(value => value / norm) : null;
}

function roomSimilarity(a, b) {
  return a.reduce((sum, value, index) => sum + value * b[index], 0);
}

function roomMatch(signature, landmarks) {
  if (!signature) return -1;
  const best = landmarks.map((landmark, index) => [roomSimilarity(signature, landmark), index])
    .sort((a, b) => b[0] - a[0]);
  return best[0]?.[0] >= 0.88 && (!best[1] || best[0][0] - best[1][0] >= 0.10) ? best[0][1] : -1;
}

function savedLandmarks(raw) {
  const route = JSON.parse(raw);
  return Array.isArray(route) && route.length >= 2 && route.length <= 12 &&
    route.every(landmark => Array.isArray(landmark) && landmark.length === 144 &&
      landmark.every(value => typeof value === "number" && Math.abs(value) <= 1)) ? route : [];
}

if (typeof module !== "undefined") module.exports = {roomMatch, savedLandmarks};

function addCompanionTools(current, dock) {
  const snap = document.querySelector("#snapshot");
  const record = document.querySelector("#record");
  const playback = document.querySelector("#playback");
  const download = document.querySelector("#download-clip");
  const mediaText = document.querySelector("#media-text");
  const motionToggle = document.querySelector("#motion-alert");
  const motionText = document.querySelector("#motion-alert-status");
  const routeText = document.querySelector("#route-text");
  const teach = document.querySelector("#teach-room");
  const mark = document.querySelector("#mark-room");
  const save = document.querySelector("#save-room");
  const forget = document.querySelector("#forget-room");
  const guide = document.querySelector("#guide-room");
  const finish = document.querySelector("#dock-approach");
  const canvas = document.createElement("canvas");
  canvas.width = 16;
  canvas.height = 9;
  const context = canvas.getContext("2d", {willReadFrequently: true});
  const key = "ori-room-v1:" + location.pathname;
  let route = [];
  try { route = savedLandmarks(localStorage.getItem(key)); } catch {}
  let teaching = null;
  let guiding = false;
  let match = -1;
  let streak = 0;
  let previousFrame = 0;
  let lastCheck = 0;
  let previousScene = null;
  let lastAlert = -Infinity;
  let recorder;
  let clipUrl;
  const format = globalThis.MediaRecorder && ["video/webm", "video/mp4"].find(type => MediaRecorder.isTypeSupported(type));
  record.hidden = !format || !HTMLCanvasElement.prototype.captureStream;
  motionToggle.onchange = () => {
    previousScene = null;
    motionText.textContent = motionToggle.checked ? "Watching for motion while Ori is still." : "";
  };

  function liveView() {
    const view = current();
    return view.live && view.image ? view : null;
  }

  function update() {
    const view = liveView();
    snap.disabled = !view;
    record.disabled = !recorder && !view;
    guide.hidden = !route.length || Boolean(teaching);
    forget.hidden = !route.length || Boolean(teaching);
    mark.hidden = save.hidden = !teaching;
    teach.disabled = !teaching && (!view || view.dock !== "charging");
    mark.disabled = !view || view.dock !== "idle" || view.moving || teaching?.length >= 12;
    save.disabled = (teaching?.length ?? 0) < 2;
    finish.hidden = !guiding || match !== 0 || streak < 3 || !view || view.moving || view.dock !== "idle";
  }

  snap.onclick = () => {
    const view = liveView();
    if (!view) return;
    const photo = document.createElement("canvas");
    photo.width = view.image.width;
    photo.height = view.image.height;
    photo.getContext("2d").drawImage(view.image, 0, 0);
    photo.toBlob(blob => {
      if (!blob) return;
      const url = URL.createObjectURL(blob);
      const link = document.createElement("a");
      link.href = url;
      link.download = "ori-snapshot.png";
      link.click();
      setTimeout(() => URL.revokeObjectURL(url), 30000);
    });
  };

  record.onclick = () => {
    if (recorder) {
      if (recorder.state === "recording") recorder.stop();
      return;
    }
    const stream = document.querySelector("#preview").captureStream(10);
    try {
      recorder = new MediaRecorder(stream, {mimeType: format, videoBitsPerSecond: 500000});
    } catch {
      stream.getTracks().forEach(track => track.stop());
      mediaText.textContent = "Recording unavailable.";
      return;
    }
    const chunks = [];
    const timer = setTimeout(() => {
      if (recorder.state === "recording") recorder.stop();
    }, 60000);
    recorder.ondataavailable = event => { if (event.data.size) chunks.push(event.data); };
    recorder.onstop = () => {
      clearTimeout(timer);
      stream.getTracks().forEach(track => track.stop());
      record.textContent = "Record clip";
      recorder = null;
      if (!chunks.length) {
        mediaText.textContent = "No video was captured.";
        return;
      }
      if (clipUrl) URL.revokeObjectURL(clipUrl);
      clipUrl = URL.createObjectURL(new Blob(chunks, {type: format}));
      playback.src = download.href = clipUrl;
      download.download = format === "video/mp4" ? "ori-clip.mp4" : "ori-clip.webm";
      playback.hidden = download.hidden = false;
      mediaText.textContent = "Clip ready. Download to keep it.";
      update();
    };
    try {
      recorder.start(1000);
    } catch {
      clearTimeout(timer);
      stream.getTracks().forEach(track => track.stop());
      recorder = null;
      mediaText.textContent = "Recording unavailable.";
      return;
    }
    record.textContent = "End clip";
    mediaText.textContent = "Recording up to 60 seconds.";
  };

  teach.onclick = () => {
    if (teaching) {
      teaching = null;
      teach.textContent = "Teach dock";
      routeText.textContent = "Teaching cancelled.";
    } else if (liveView()?.dock === "charging") {
      teaching = [];
      guiding = false;
      teach.textContent = "Cancel teaching";
      routeText.textContent = "Move off the dock. Stop facing it and capture the clear approach first.";
    }
    update();
  };
  mark.onclick = () => {
    const view = liveView();
    if (!teaching || !view || view.moving || view.dock !== "idle" || teaching.length >= 12) return;
    const signature = roomSignature(view.image, context);
    if (!signature || teaching.some(candidate => roomSimilarity(candidate, signature) >= 0.78)) {
      routeText.textContent = "Choose a distinct view with more room detail.";
      return;
    }
    teaching.push(signature);
    routeText.textContent = `${teaching.length} landmarks captured. Face toward the dock at each stop.`;
    update();
  };
  save.onclick = () => {
    if (!teaching || teaching.length < 2) return;
    try { localStorage.setItem(key, JSON.stringify(teaching)); } catch {
      routeText.textContent = "Could not save the route on this phone.";
      return;
    }
    route = teaching;
    teaching = null;
    teach.textContent = "Teach dock";
    routeText.textContent = `Saved ${route.length} landmarks on this phone.`;
    update();
  };
  forget.onclick = () => {
    try { localStorage.removeItem(key); } catch {
      routeText.textContent = "Could not remove the saved route.";
      return;
    }
    route = [];
    guiding = false;
    routeText.textContent = "Route forgotten.";
    update();
  };
  guide.onclick = () => {
    guiding = !guiding;
    guide.textContent = guiding ? "End route guidance" : "Guide me home";
    routeText.textContent = guiding ? "Face toward the dock to locate a taught view." : "Route guidance ended.";
    match = -1;
    streak = 0;
    update();
  };
  finish.onclick = () => {
    if (finish.hidden || !liveView()) return;
    dock();
  };
  if ("serviceWorker" in navigator) navigator.serviceWorker.register("./sw.js").catch(() => {});

  return () => {
    const view = liveView();
    if (recorder?.state === "recording" && !view) recorder.stop();
    if (guiding && !view) {
      match = -1;
      streak = 0;
      routeText.textContent = "Waiting for a fresh view.";
    }
    if (!view || view.moving) previousScene = null;
    if ((guiding || motionToggle.checked) && view && view.frame !== previousFrame && performance.now() - lastCheck >= 160) {
      previousFrame = view.frame;
      lastCheck = performance.now();
      const signature = view.moving ? null : roomSignature(view.image, context);
      if (guiding) {
        const next = roomMatch(signature, route);
        streak = next >= 0 ? (next === match ? Math.min(3, streak + 1) : 1) : 0;
        match = next;
        const message = streak < 3 ? "Stop at a clear taught view to locate Ori."
          : match === 0 ? "Dock approach recognized. Check the path before docking."
          : `Landmark ${match + 1} of ${route.length}. Steer toward landmark ${match}.`;
        if (routeText.textContent !== message) routeText.textContent = message;
      }
      if (motionToggle.checked) {
        if (signature && previousScene && roomSimilarity(signature, previousScene) < 0.90 &&
            lastCheck - lastAlert >= 3000) {
          lastAlert = lastCheck;
          motionText.textContent = `Motion seen at ${new Date().toLocaleTimeString()}.`;
        }
        previousScene = signature;
      }
    }
    update();
  };
}

function addPatrol(current, controlPatrol) {
  const panel = document.querySelector("#patrol-tools");
  const message = document.querySelector("#patrol-status");
  const edit = document.querySelector("#patrol-edit");
  const capture = document.querySelector("#patrol-capture");
  const save = document.querySelector("#patrol-save");
  const start = document.querySelector("#patrol-start");
  const end = document.querySelector("#patrol-stop");
  const name = document.querySelector("#patrol-name");
  const map = document.querySelector("#patrol-map");
  const kindSelect = document.querySelector("#patrol-kind");
  const lapsSelect = document.querySelector("#patrol-laps");
  const spotBox = document.querySelector("#patrol-spot");
  const dockBox = document.querySelector("#patrol-dock");
  const optionsBar = document.querySelector("#patrol-options");
  const drawing = map.getContext("2d");
  const sampleCanvas = document.createElement("canvas");
  sampleCanvas.width = 16;
  sampleCanvas.height = 9;
  const sampleContext = sampleCanvas.getContext("2d", {willReadFrequently: true});
  const key = "ori-patrol-v1";
  const defaultArea = [.22, .22, .78, .78];
  const hasServerPatrol = typeof controlPatrol === "function";
  let mode;
  let kind = "area";
  let preset;
  let draft = null;
  let corner = null;
  let run = null;
  let legsBuffer = [];
  let lastLegSample = 0;
  let stopRequested = false;

  function tell(text) {
    if (message.textContent !== text) message.textContent = text;
  }

  function validArea(area) {
    return Array.isArray(area) && area.length === 4 &&
      area.every(value => Number.isFinite(value) && value >= .05 && value <= .95) &&
      area[2] - area[0] >= .12 && area[3] - area[1] >= .12;
  }

  function readPreset(activeKind = kind) {
    try {
      if (hasServerPatrol && activeKind === "area") {
        const saved = JSON.parse(localStorage.getItem(key + ":area:" + mode));
        return validArea(saved) ? saved : null;
      }
      const saved = JSON.parse(localStorage.getItem(key + ":" + mode));
      if (!hasServerPatrol && mode === "simulator") return validArea(saved) ? saved : null;
      if (!Array.isArray(saved) || saved.some(point => typeof point?.name !== "string" || point.name.length > 40)) return null;
      return savedLandmarks(JSON.stringify(saved.map(point => point.signature))).length ? saved : null;
    } catch {
      return null;
    }
  }

  function draw(position) {
    if (hasServerPatrol && kind !== "area") return;
    if (!hasServerPatrol && mode !== "simulator") return;
    if (!map.hidden) {
      const rect = map.getBoundingClientRect();
      const dpr = Math.max(1, window.devicePixelRatio || 1);
      const nextW = rect.width > 0 ? Math.round(rect.width * dpr) : 320;
      const nextH = rect.height > 0 ? Math.round(rect.height * dpr) : 180;
      if (map.width !== nextW || map.height !== nextH) {
        map.width = nextW;
        map.height = nextH;
      }
    }
    const {width, height} = map;
    const scale = width / 320;
    const night = document.documentElement.dataset.theme === "night";
    drawing.fillStyle = night ? "#121915" : "#e2e8e4";
    drawing.fillRect(0, 0, width, height);
    drawing.strokeStyle = night ? "#223028" : "#ccd7cf";
    drawing.lineWidth = Math.max(1, scale);
    const step = width / 16;
    for (let x = step; x < width; x += step) {
      drawing.beginPath();
      drawing.moveTo(x, 0);
      drawing.lineTo(x, height);
      drawing.stroke();
    }
    const yStep = height / 9;
    for (let y = yStep; y < height; y += yStep) {
      drawing.beginPath();
      drawing.moveTo(0, y);
      drawing.lineTo(width, y);
      drawing.stroke();
    }
    const area = draft || preset || (hasServerPatrol ? defaultArea : null);
    if (area && Array.isArray(area) && area.length === 4) {
      const [left, top, right, bottom] = area;
      drawing.fillStyle = night ? "rgba(43, 166, 114, 0.22)" : "rgba(15, 104, 72, 0.16)";
      drawing.strokeStyle = night ? "#2ba672" : "#0f6848";
      drawing.lineWidth = 2 * scale;
      drawing.fillRect(left * width, top * height, (right - left) * width, (bottom - top) * height);
      drawing.strokeRect(left * width, top * height, (right - left) * width, (bottom - top) * height);
    }
    const dot = position || corner || (Array.isArray(area) && area.length === 4 ? area.slice(0, 2) : null);
    if (dot) {
      drawing.beginPath();
      drawing.arc(dot[0] * width, dot[1] * height, 5.5 * scale, 0, Math.PI * 2);
      drawing.fillStyle = position ? "#f0b44c" : (night ? "#2ba672" : "#0f6848");
      drawing.fill();
    }
    drawing.lineWidth = 1;
  }

  function stop(text = "Patrol ended.") {
    if (hasServerPatrol) {
      if (current().patrol?.active && !stopRequested) {
        stopRequested = true;
        controlPatrol({mode: "stop"});
        if (text) tell(text);
      }
      return;
    }
    if (!run) return;
    run = null;
    tell(text);
    update();
  }

  function nextCheckpoint() {
    tell(`Checkpoint ${run.index + 1} of ${preset.length} · ${preset[run.index].name}. Drive there, then stop and face the saved view.`);
  }

  function recordLeg(ly, rx) {
    if (!draft || kind !== "checkpoints" || (!ly && !rx)) return;
    const now = performance.now();
    const dt = lastLegSample ? Math.min(350, Math.max(50, Math.round(now - lastLegSample))) : 80;
    lastLegSample = now;
    const last = legsBuffer[legsBuffer.length - 1];
    if (last && Math.abs(last.ly - ly) < 0.04 && Math.abs(last.rx - rx) < 0.04 && last.dt < 2500) {
      last.dt += dt;
    } else if (legsBuffer.length < 24) {
      legsBuffer.push({ly: Number(ly.toFixed(3)), rx: Number(rx.toFixed(3)), dt});
    }
  }

  function endLeg() {
    lastLegSample = 0;
  }

  if (kindSelect) {
    kindSelect.onchange = () => {
      if (current().patrol?.active || run) return;
      if (hasServerPatrol && draft && kind === "checkpoints") controlPatrol({mode: "teach-cancel"});
      kind = kindSelect.value;
      draft = null;
      corner = null;
      preset = readPreset(kind);
      draw();
      update();
    };
  }

  function update() {
    const view = current();
    if (!view.patrol?.active) stopRequested = false;
    panel.hidden = view.viewer || (view.mode !== "robot" && view.mode !== "simulator");
    if (panel.hidden) {
      stop();
      return;
    }
    if (mode !== view.mode) {
      mode = view.mode;
      if (kindSelect) {
        const factoryOpt = kindSelect.querySelector('option[value="factory"]');
        if (factoryOpt) factoryOpt.hidden = mode !== "robot";
        if (kind === "factory" && mode !== "robot") kind = kindSelect.value = "area";
      }
      preset = readPreset(kind);
      if (!hasServerPatrol) {
        tell(mode === "robot" ? "Save camera checkpoints and use Manual drive to visit them in order."
          : "Set an area on the map for a one-lap simulator patrol.");
      } else {
        tell(kind === "area" ? "Outline an area on the map or use the default zone, then start auto patrol."
          : kind === "checkpoints" ? "Teach named checkpoints to drive them autonomously."
          : "Use Ori's built-in roving security patrol.");
      }
      draw(view.patrol?.position);
    }

    const robot = mode === "robot";
    const activeServer = hasServerPatrol && Boolean(view.patrol?.active);
    const active = activeServer || Boolean(run);

    if (!hasServerPatrol) {
      edit.textContent = draft ? "Cancel setup" : robot ? "Teach patrol area" : "Set patrol area";
      edit.disabled = Boolean(run);
      capture.hidden = !robot || !draft;
      name.closest("label").hidden = capture.hidden;
      capture.disabled = !view.live || !view.ready || view.moving || (draft?.length ?? 0) >= 12;
      save.hidden = !draft;
      save.disabled = robot ? (draft?.length ?? 0) < 2 : Boolean(corner);
      start.hidden = !preset || Boolean(draft || run);
      start.textContent = robot ? "Start guided patrol" : "Start simulator patrol";
      start.disabled = !view.live || !view.ready || view.moving;
      end.hidden = !run;
      map.hidden = robot || !draft && !preset;
      if (!run || !robot) return;
      if (view.boot !== run.boot) {
        stop("Patrol ended after Ori reconnected.");
        return;
      }
      if (!view.live || !view.ready) {
        run.streak = 0;
        tell(!view.live ? "Patrol paused. Waiting for live video." : "Patrol paused. Waiting for Ori to be ready.");
        return;
      }
      if (view.moving || view.frame === run.frame || performance.now() - run.checked < 160) {
        if (view.moving) run.streak = 0;
        return;
      }
      run.frame = view.frame;
      run.checked = performance.now();
      const signature = roomSignature(view.image, sampleContext);
      const found = roomMatch(signature, run.signatures);
      run.streak = found === run.index ? run.streak + 1 : 0;
      if (run.streak < 3) {
        nextCheckpoint();
        return;
      }
      run.streak = 0;
      if (++run.index === preset.length) {
        stop("Guided patrol complete. All checkpoints visited.");
      } else {
        nextCheckpoint();
      }
      return;
    }

    if (kindSelect) kindSelect.disabled = active || Boolean(draft);
    if (optionsBar) optionsBar.hidden = kind !== "area";
    edit.hidden = kind === "factory";
    edit.textContent = draft ? "Cancel setup" : kind === "checkpoints" ? "Teach patrol area" : "Set patrol area";
    edit.disabled = active;
    capture.hidden = kind !== "checkpoints" || !draft;
    name.closest("label").hidden = capture.hidden;
    capture.disabled = !view.live || view.moving || (draft?.length ?? 0) >= 12;
    save.hidden = !draft;
    save.disabled = kind === "checkpoints" ? (draft?.length ?? 0) < 2 : Boolean(corner);
    const canStartKind = kind === "area" || kind === "factory" || Boolean(preset);
    start.hidden = active || Boolean(draft) || !canStartKind;
    start.textContent = kind === "area"
      ? (robot ? "Start auto patrol" : "Start simulator patrol")
      : kind === "checkpoints" ? "Start checkpoint patrol" : "Start factory patrol";
    const patrolReady = view.live && !view.moving && (kind === "area" ? view.patrolReady : view.ready);
    start.disabled = !patrolReady;
    end.hidden = !active;
    map.hidden = kind !== "area";
    if (kind === "area") draw(view.patrol?.position);
    if (activeServer && view.patrol?.message) tell(view.patrol.message);
    else if (!active && !draft && view.patrol?.message) tell(view.patrol.message);
  }

  edit.onclick = () => {
    if (run || current().patrol?.active) return;
    corner = null;
    if (draft) {
      if (hasServerPatrol && kind === "checkpoints") controlPatrol({mode: "teach-cancel"});
      draft = null;
      legsBuffer = [];
      tell("Setup cancelled.");
    } else if (kind === "checkpoints" || (!hasServerPatrol && mode === "robot")) {
      if (hasServerPatrol) controlPatrol({mode: "teach-start"});
      draft = [];
      legsBuffer = [];
      lastLegSample = 0;
      name.value = "";
      tell("Drive to your first checkpoint. Stop, enter a name, and capture the view.");
    } else {
      draft = preset || defaultArea;
      tell("Click two opposite corners or save the outlined area.");
    }
    draw();
    update();
  };

  map.onclick = event => {
    if (!draft || (hasServerPatrol ? kind !== "area" : mode !== "simulator")) return;
    const bounds = map.getBoundingClientRect();
    const pt = [(event.clientX - bounds.left) / bounds.width, (event.clientY - bounds.top) / bounds.height]
      .map(value => Math.max(.05, Math.min(.95, value)));
    if (!corner) {
      corner = pt;
      tell("Choose the opposite corner.");
    } else {
      const area = [Math.min(corner[0], pt[0]), Math.min(corner[1], pt[1]),
        Math.max(corner[0], pt[0]), Math.max(corner[1], pt[1])];
      corner = null;
      if (validArea(area)) {
        draft = area;
        tell("Area outlined. Save it or choose two new corners.");
      } else {
        tell("Choose corners farther apart. The previous outline is still available.");
      }
    }
    draw();
    update();
  };

  capture.onclick = async () => {
    const view = current();
    if (!draft || !view.live || !view.image || view.moving || draft.length >= 12) return;
    if (!hasServerPatrol && (mode !== "robot" || !view.ready)) return;
    const signature = roomSignature(view.image, sampleContext);
    if (!signature || draft.some(pt => roomSimilarity(pt.signature, signature) >= .78)) {
      tell("Choose a distinct view with more room detail.");
      return;
    }
    let legs = [];
    if (hasServerPatrol) {
      const res = await controlPatrol({mode: "teach-capture"});
      if (!draft) return;
      legs = draft.length === 0 ? [] : (res?.teach_legs?.length ? res.teach_legs : legsBuffer.slice());
    } else {
      legs = legsBuffer.slice();
    }
    draft.push({
      name: name.value.trim() || `Checkpoint ${draft.length + 1}`,
      signature,
      legs,
    });
    legsBuffer = [];
    lastLegSample = 0;
    name.value = "";
    tell(`Saved view ${draft.length}. Drive to the next checkpoint or save the area.`);
    update();
  };

  save.onclick = () => {
    if (!draft || save.disabled) return;
    const storageKey = hasServerPatrol && kind === "area" ? key + ":area:" + mode : key + ":" + mode;
    try {
      localStorage.setItem(storageKey, JSON.stringify(draft));
    } catch {
      tell("This browser could not save the patrol area.");
      return;
    }
    if (hasServerPatrol && kind === "checkpoints") controlPatrol({mode: "teach-cancel"});
    preset = draft;
    draft = null;
    legsBuffer = [];
    tell(kind === "checkpoints" || (!hasServerPatrol && mode === "robot")
      ? `Saved ${preset.length} checkpoints in this browser.`
      : "Patrol area saved.");
    draw();
    update();
  };

  start.onclick = () => {
    if (run || current().patrol?.active || start.disabled || start.hidden) return;
    if (!hasServerPatrol) {
      if (!preset) return;
      run = {boot: current().boot};
      Object.assign(run, {index: 0, streak: 0, frame: 0, checked: 0, signatures: preset.map(pt => pt.signature)});
      nextCheckpoint();
      update();
      return;
    }
    const laps = lapsSelect ? Number(lapsSelect.value) : 1;
    const spot_stop = spotBox ? spotBox.checked : false;
    const dock_after = dockBox ? dockBox.checked : false;
    if (kind === "area") {
      controlPatrol({mode: "start", kind: "area", area: preset || defaultArea, laps, spot_stop, dock_after});
    } else if (kind === "checkpoints") {
      if (!preset?.length) return;
      controlPatrol({mode: "start", kind: "checkpoints", checkpoints: preset});
    } else {
      controlPatrol({mode: "start", kind: "factory"});
    }
    update();
  };
  end.onclick = () => stop("Patrol ended.");
  return {update, stop, recordLeg, endLeg};
}
