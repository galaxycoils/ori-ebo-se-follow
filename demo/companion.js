"use strict";

// Normalized thumbnails tolerate global brightness changes and require room texture.
function roomSignature(image, context) {
  context.drawImage(image, 0, 0, 16, 9);
  const pixels = context.getImageData(0, 0, 16, 9).data;
  const samples = Array.from({length: 144}, (_, i) => pixels[4 * i]);
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
