// Uses a fresh loopback demo unless ORI_DEMO_URL selects an existing server.
const assert = require("node:assert/strict");
const {chromium, webkit, devices} = require("playwright");
const {existsSync, readFileSync} = require("node:fs");

const { spawn } = require("node:child_process");
const { once } = require("node:events");
const { createInterface } = require("node:readline");
const path = require("node:path");
const {roomMatch, savedLandmarks} = require("../demo/companion.js");

const firstLandmark = Array.from({length: 144}, (_, i) => i === 0 ? 1 : 0);
const secondLandmark = Array.from({length: 144}, (_, i) => i === 1 ? 1 : 0);
assert.equal(roomMatch(firstLandmark, [firstLandmark, secondLandmark]), 0);
assert.equal(roomMatch(firstLandmark, [firstLandmark, firstLandmark]), -1);
assert.equal(roomMatch(null, [firstLandmark, secondLandmark]), -1);
assert.equal(savedLandmarks(JSON.stringify([firstLandmark, [3]])).length, 0);

const useWebKit = process.env.ORI_TEST_WEBKIT === "1";

function launchBrowser() {
  if (useWebKit) return webkit.launch();
  return chromium.launch({
    executablePath: process.env.CHROMIUM || (existsSync("/usr/bin/chromium") ? "/usr/bin/chromium" : undefined),
    headless: true,
  });
}

async function testGuidedPatrol(browser, base) {
  const page = await browser.newPage();
  try {
    const html = readFileSync(path.resolve(__dirname, "../demo/index.html"), "utf8");
    const panel = html.match(/<details id="patrol-tools"[\s\S]*?<\/details>/)[0];
    const url = new URL("guided-patrol-test", base).href;
    await page.route(url, route => route.fulfill({contentType: "text/html", body: panel}));
    await page.goto(url);
    await page.addScriptTag({path: path.resolve(__dirname, "../demo/companion.js")});
    await page.evaluate(() => {
      const image = document.createElement("canvas");
      image.width = 320;
      image.height = 180;
      const context = image.getContext("2d");
      window.view = {mode: "robot", boot: "test", live: true, ready: true, moving: false, image, frame: 0};
      window.paintWaypoint = (first) => {
        context.fillStyle = "#161616";
        context.fillRect(0, 0, 320, 180);
        context.fillStyle = "#eee";
        if (first) context.fillRect(0, 0, 160, 180);
        else context.fillRect(0, 0, 320, 80);
      };
      paintWaypoint(true);
      const patrol = addPatrol(() => view);
      setInterval(() => {
        ++view.frame;
        patrol.update();
      }, 200);
      patrol.update();
    });
    await page.getByText("Patrol area", {exact: true}).click();
    await page.getByRole("button", {name: "Teach patrol area"}).click();
    const name = page.getByLabel("Checkpoint name");
    const capture = page.getByRole("button", {name: "Capture checkpoint"});
    await name.fill("Hall door");
    await capture.click();
    await capture.click();
    assert.match(await page.locator("#patrol-status").textContent(), /distinct view/);
    await page.evaluate(() => paintWaypoint(false));
    await name.fill("Window");
    await capture.click();
    await page.getByRole("button", {name: "Save patrol area"}).click();
    const saved = await page.evaluate(() => JSON.parse(localStorage.getItem("ori-patrol-v1:robot")));
    assert.deepEqual(saved.map(point => point.name), ["Hall door", "Window"]);
    await page.getByRole("button", {name: "Start guided patrol"}).click();
    await page.getByText(/Checkpoint 1 of 2.*Hall door/).waitFor();
    await page.evaluate(() => { view.live = false; paintWaypoint(true); });
    await page.getByText(/waiting for live video/i).waitFor();
    await page.evaluate(() => { view.live = true; });
    await page.getByText(/Checkpoint 2 of 2.*Window/).waitFor();
    await page.evaluate(() => paintWaypoint(false));
    await page.getByText("Guided patrol complete. All checkpoints visited.").waitFor();
    await page.getByRole("button", {name: "Start guided patrol"}).click();
    await page.getByRole("button", {name: "End patrol"}).click();
    await page.getByText("Patrol ended.", {exact: true}).waitFor();
  } finally {
    await page.close();
  }
}

async function testDemo(base, disconnect) {
  const browser = await launchBrowser();
  try {
    const page = await browser.newPage(useWebKit ? devices["iPhone 13"]
      : {viewport: {width: 390, height: 844}});
    const errors = [];
    page.on("pageerror", error => errors.push(error));
    await page.goto(base);
    await page.evaluate(async () => { await transport; });
    await page.waitForFunction(() => displayed.frame > 0 && status.boot);
    const manifest = await page.request.get(new URL("manifest.webmanifest", page.url()).href);
    assert.deepEqual((await manifest.json()).icons.map(icon => icon.sizes), ["192x192", "512x512"]);
    const isStatic = await page.evaluate(() => document.documentElement.dataset.preview === "pages");
    for (const [width, height] of [[320, 568], [390, 844], [844, 390], [390, 844]]) {
      await page.setViewportSize({width, height});
      assert(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth));
      const stop = await page.locator("#stop").boundingBox();
      const camera = await page.locator("#preview").boundingBox();
      const instruction = await page.locator("#help").boundingBox();
      assert(camera.y + camera.height <= height && stop.y + stop.height <= height);
      assert(instruction.y + instruction.height <= height);
    }
    await page.evaluate(() => { document.documentElement.style.fontSize = "24px"; });
    assert(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth));
    await page.evaluate(() => { document.documentElement.style.removeProperty("font-size"); });
    assert.equal(await page.locator("#drive-pad").getAttribute("open"), null);
    assert.deepEqual(await page.evaluate(() => {
      const previous = status;
      status = {...status, state: "READY", owner: driver, live_hardware: true, source_mode: "robot",
        follow_blocker: "follow-locked"};
      update();
      const presentation = {
        title: statusText.textContent,
        showsFollow: !buttons.follow.hidden,
        showsBlocker: !followStatus.hidden,
      };
      status = previous;
      update();
      return presentation;
    }), {title: "Tracking target", showsFollow: false, showsBlocker: true});
    const previousBoot = await page.evaluate(async () => {
      const api = await transport;
      return api.control("stop", {driver: 1, generation: 1, boot: "previous-instance"});
    });
    assert.equal(previousBoot.accepted, false);
    if (!isStatic) {
      const crossOrigin = await page.request.post(new URL("/api/stop", base).href, {
        headers: { Origin: "https://example.com" },
        data: {},
      });
      assert.equal(crossOrigin.status(), 403);
    }
    await page.evaluate(() => {
      window.observedMotion = new Set();
      setInterval(() => window.observedMotion.add(status.motion), 20);
    });
    const preview = page.locator("#preview");
    const position = await preview.evaluate(canvas => ({
      x: canvas.clientWidth * 0.5, y: canvas.clientHeight * 105 / 180,
    }));
    const bounds = await preview.boundingBox();
    async function drawBox(x0 = 120, y0 = 65, x1 = 200, y1 = 145) {
      await page.mouse.move(bounds.x + bounds.width * x0 / 320, bounds.y + bounds.height * y0 / 180);
      await page.mouse.down();
      await page.mouse.move(bounds.x + bounds.width * x1 / 320, bounds.y + bounds.height * y1 / 180, { steps: 4 });
      await page.mouse.up();
    }

    // Keyboard Escape must cancel an active unconfirmed selection.
    await drawBox();
    await page.waitForFunction(() => selection !== null);
    await page.keyboard.press("Escape");
    await page.waitForFunction(() => selection === null);
    assert(await page.getByRole("button", { name: "Confirm selection" }).isHidden());

    // Quick tap on blank background rejects as uncertain and opens an adjustable 44x44 draft.
    const blankPos = {x: bounds.width * 20 / 320, y: bounds.height * 20 / 180};
    if (useWebKit) await preview.tap({position: blankPos});
    else await preview.click({position: blankPos});
    await page.waitForFunction(() => selection !== null && selection.box[2] === 44 && selection.box[3] === 44);
    assert.match(await page.locator("#help").textContent(), /Could not track that box/);
    await page.keyboard.press("Escape");
    await page.waitForFunction(() => selection === null);

    // Narrow drag (2 px vertical) enforces 16 px minimum per axis; move and corner resize work.
    await drawBox(110, 80, 175, 82);
    await page.waitForFunction(() => selection !== null);
    const narrowBox = await page.evaluate(() => selection.box);
    assert(narrowBox[2] >= 16 && narrowBox[3] >= 16, narrowBox);
    const moveStart = [
      bounds.x + bounds.width * (narrowBox[0] + narrowBox[2] / 2) / 320,
      bounds.y + bounds.height * (narrowBox[1] + narrowBox[3] / 2) / 180,
    ];
    await page.mouse.move(moveStart[0], moveStart[1]);
    await page.mouse.down();
    await page.mouse.move(moveStart[0] + bounds.width * 20 / 320, moveStart[1] + bounds.height * 10 / 180, {steps: 3});
    await page.mouse.up();
    const movedBox = await page.evaluate(() => selection.box);
    assert(movedBox[0] > narrowBox[0] && movedBox[1] > narrowBox[1], {narrowBox, movedBox});
    assert.equal(movedBox[2], narrowBox[2]);
    assert.equal(movedBox[3], narrowBox[3]);
    const cornerPos = [
      bounds.x + bounds.width * (movedBox[0] + movedBox[2]) / 320,
      bounds.y + bounds.height * (movedBox[1] + movedBox[3]) / 180,
    ];
    await page.mouse.move(cornerPos[0], cornerPos[1]);
    await page.mouse.down();
    await page.mouse.move(cornerPos[0] + bounds.width * 18 / 320, cornerPos[1] + bounds.height * 18 / 180, {steps: 3});
    await page.mouse.up();
    const resizedBox = await page.evaluate(() => selection.box);
    assert(resizedBox[2] > movedBox[2] && resizedBox[3] > movedBox[3], {movedBox, resizedBox});
    await page.keyboard.press("Escape");
    await page.waitForFunction(() => selection === null);

    // Keyboard Enter opens a centered box and second Enter confirms tracking.
    await preview.focus();
    await page.keyboard.press("Enter");
    await page.waitForFunction(() => selection !== null);
    await page.keyboard.press("Enter");
    await page.waitForFunction(() => status.state === "TRACKING" && !selection);
    await page.getByRole("button", {name: "Stop", exact: true}).click();
    await page.waitForFunction(() => status.state === "OFF" && !pending);

    // An expired selection rejection keeps the draft open and requires a fresh confirmation click.
    await drawBox();
    const confirm = page.getByRole("button", {name: "Confirm selection"});
    await page.evaluate(async () => {
      const api = await transport;
      window.origControlExpired = api.control;
      let rejectOnce = true;
      api.control = async (action, payload) => {
        if (action === "select" && rejectOnce) {
          rejectOnce = false;
          return {...status, accepted: false, reason: "selection-expired"};
        }
        return window.origControlExpired(action, payload);
      };
    });
    await confirm.click();
    await page.waitForFunction(() => selection?.expired === true && !pending);
    assert.equal(await page.evaluate(() => status.state), "OFF");
    assert.match(await page.locator("#help").textContent(), /expired/i);
    await page.evaluate(async () => { (await transport).control = window.origControlExpired; });
    await confirm.click();
    await page.waitForFunction(() => status.state === "TRACKING" && !selection);
    await page.getByRole("button", {name: "Stop", exact: true}).click();
    await page.waitForFunction(() => status.state === "OFF" && !pending);

    // A draft survives the old timeout and a preview outage while keeping confirmation tied to a fresh frame.
    await drawBox();
    const frameBefore = await page.evaluate(() => presented.frame);
    await page.waitForTimeout(2600);
    assert(await confirm.isEnabled());
    assert(await page.evaluate(frame => presented.frame > frame + 10, frameBefore));
    await page.evaluate(async () => {
      const api = await transport;
      window.originalFrame = api.frame;
      api.frame = () => { throw new Error("Preview unavailable for test"); };
    });
    await page.waitForFunction(() => buttons.confirm.disabled);
    assert(await confirm.isVisible());
    await page.evaluate(async () => { (await transport).frame = window.originalFrame; });
    await confirm.click();
    await page.waitForFunction(() => status.state === "TRACKING" && !selection);
    await page.getByRole("button", {name: "Stop", exact: true}).click();
    await page.waitForFunction(() => status.state === "OFF" && !pending);

    // Bottom-right border drag clamps cleanly within valid Box::visible() bounds.
    await drawBox(240, 100, 320, 180);
    const clamped = await page.evaluate(() => selection.box);
    assert(clamped[0] + clamped[2] <= 319 && clamped[1] + clamped[3] <= 179, clamped);
    await page.keyboard.press("Escape");

    // Manual drag box -> confirm -> Stop immediately clears displayed.box.
    await page.waitForFunction(() => previewIsLive());
    await drawBox();
    await page.getByRole("button", { name: "Confirm selection" }).click();
    await page.waitForFunction(() => status.state === "TRACKING" && Array.isArray(displayed.box));
    await page.getByRole("button", { name: "Stop", exact: true }).click();
    await page.waitForFunction(() => status.state === "OFF" && displayed.box === null && !pending);

    // Quick tap target -> immediate TRACKING -> teach -> follow -> track-only -> move demo target.
    if (useWebKit) await preview.tap({position});
    else await preview.click({position});
    await page.waitForFunction(() => status.state === "TRACKING" && !selection);
    const selectedFrame = await page.evaluate(() => status.frame);
    await page.waitForFunction(frame => status.frame >= frame + 3, selectedFrame);
    await page.getByRole("button", { name: "Set gap here" }).click();
    await page.waitForFunction(() => status.state === "READY");

    await page.getByRole("button", { name: "Start Follow", exact: true }).click();
    await page.waitForFunction(() => status.state === "FOLLOWING");
    await page.waitForFunction(() => status.motion === "FORWARD");
    assert(await page.evaluate(() => window.observedMotion.has("LEFT")), "The moving target must first cause a turn");
    await page.getByRole("button", { name: "Pause Follow" }).click();
    await page.waitForFunction(() => status.state === "READY" && status.motion === "HOLD");
    await page.setViewportSize({ width: 844, height: 390 });
    await page.evaluate(() => window.observedMotion.clear());
    await page.getByRole("button", { name: "Move demo target" }).click();
    await page.waitForTimeout(3000);
    assert.deepEqual(await page.evaluate(() => [...window.observedMotion]), ["HOLD"]);

    // Stop must win against a Follow that executed but whose response is still in transit.
    await page.evaluate(async () => {
      const api = await transport;
      window.liveControl = api.control;
      api.control = async (action, payload) => {
        const result = await window.liveControl(action, payload);
        if (action === "follow") {
          window.delayedFollow = {result, payload};
          await new Promise(resolve => { window.releaseFollow = resolve; });
        }
        return result;
      };
    });
    await page.getByRole("button", {name: "Start Follow"}).click();
    await page.waitForFunction(() => window.delayedFollow?.result.accepted && status.state === "FOLLOWING");
    assert(await page.evaluate(() => window.delayedFollow.payload.frame > 0));
    await page.getByRole("button", {name: "Stop", exact: true}).click();
    await page.waitForFunction(() => status.state === "OFF" && !pending);
    await page.evaluate(async () => {
      window.releaseFollow();
      (await transport).control = window.liveControl;
    });
    assert.equal(await page.locator("#status").textContent(), "Tracking off");

    const updatedPreview = await preview.boundingBox();
    await preview.click({position: {x: updatedPreview.width / 2, y: updatedPreview.height * 105 / 180}});
    await page.waitForFunction(() => status.state === "TRACKING" && status.can_teach);
    await page.getByRole("button", {name: "Set gap here"}).click();
    await page.getByRole("button", {name: "Start Follow"}).click();
    await page.waitForFunction(() => status.state === "FOLLOWING");
    await page.evaluate(async () => {
      const api = await transport;
      window.liveFrame = api.frame;
      api.frame = () => { throw new Error("Preview interrupted for testing"); };
    });
    await page.waitForTimeout(900);
    const afterFreeze = await page.evaluate(async () => {
      const preview = await window.liveFrame("");
      await preview.discard();
      return preview.status;
    });
    assert(["preview-expired", "heartbeat-expired"].includes(afterFreeze.reason), afterFreeze.reason);
    assert.equal(afterFreeze.motion, "HOLD");
    assert.match(await page.locator("#motion").textContent(), /unknown/);
    await page.evaluate(async () => { (await transport).frame = window.liveFrame; });
    await page.waitForFunction(() => previewIsLive() && status.state === "OFF");

    // Repeated 204s must not keep previewIsLive() alive past 2200 ms; a fresh 200 restores controls.
    await page.evaluate(async () => {
      const api = await transport;
      window.liveFrame204 = api.frame;
      api.frame = async () => ({
        tag: lastTag,
        statusTag: lastStatusTag,
        frame: presented?.frame || 1,
        age: 10,
        processingMs: 1,
        status: {...status},
        image: null,
        discard: async () => {},
      });
    });
    await page.waitForFunction(() => !previewIsLive() && statusText.textContent === "Video delayed", null, {timeout: 3500});
    assert.equal(await page.locator("#status").textContent(), "Video delayed");
    await page.evaluate(async () => { (await transport).frame = window.liveFrame204; });
    await page.waitForFunction(() => previewIsLive());

    // Hidden tab clears displayed.frame until a fresh image returns without reload.
    const frameBeforeHide = await page.evaluate(() => presented.frame);
    await page.evaluate(() => {
      Object.defineProperty(document, "hidden", {configurable: true, get: () => true});
      document.dispatchEvent(new Event("visibilitychange"));
    });
    assert.equal(await page.evaluate(() => previewIsLive()), false);
    await page.evaluate(() => {
      delete document.hidden;
      document.dispatchEvent(new Event("visibilitychange"));
    });
    await page.waitForFunction(prev => previewIsLive() && presented.frame > prev, frameBeforeHide);
    await page.getByRole("button", { name: "Stop", exact: true }).click();
    await page.waitForFunction(() => status.reason === "owner-stop");
    await page.getByRole("button", { name: "Return to Dock" }).click();
    await page.waitForFunction(() => status.dock === "returning");
    await page.getByRole("button", { name: "Stop", exact: true }).click();
    await page.waitForFunction(() => status.dock === "idle");
    await page.getByRole("button", { name: "Return to Dock" }).click();
    await page.waitForFunction(() => status.dock === "charging");
    assert(await page.getByRole("button", { name: "Return to Dock" }).isDisabled());
    await page.getByText("Dock route", {exact: true}).click();
    await page.getByRole("button", {name: "Teach dock"}).click();
    assert.match(await page.locator("#route-text").textContent(), /capture the clear approach/);
    await page.getByRole("button", { name: "Lift off base (demo)" }).click();
    await page.waitForFunction(() => status.dock === "idle" && status.state === "OFF");
    await page.getByRole("button", {name: "Capture landmark"}).click();
    assert.match(await page.locator("#route-text").textContent(), /1 landmarks? captured/);
    await page.getByRole("button", {name: "Capture landmark"}).click();
    assert.match(await page.locator("#route-text").textContent(), /distinct view/);
    await page.getByRole("button", {name: "Cancel teaching"}).click();
    // Camera color/B&W and Night mode work independently of the persisted UI theme.
    const colorBtn = page.locator("#camera-color");
    assert.equal(await colorBtn.textContent(), "Color");
    await colorBtn.click();
    await page.waitForFunction(() => status.camera?.color_mode === "bw");
    assert.equal(await colorBtn.textContent(), "B&W");
    await colorBtn.click();
    await page.waitForFunction(() => status.camera?.color_mode === "color");
    assert.equal(await page.evaluate(() => document.documentElement.dataset.theme ?? ""), "");
    await page.getByLabel("Night mode").selectOption("night");
    await page.waitForFunction(() => status.camera?.night_active && !document.documentElement.dataset.theme);
    await page.getByLabel("Night style").selectOption("emerald");
    await page.waitForFunction(() => status.camera?.ir_tint === "emerald");
    await page.getByRole("button", {name: "Dark UI"}).click();
    await page.waitForFunction(() => document.documentElement.dataset.theme === "night" &&
      localStorage.getItem("ori-ui-theme") === "night");
    await page.getByLabel("Night mode").selectOption("boost");
    await page.waitForFunction(() => status.camera?.boost_active && document.documentElement.dataset.theme === "night");
    await page.getByLabel("Night mode").selectOption("off");
    await page.waitForFunction(() => status.camera?.night_mode === "off" &&
      document.documentElement.dataset.theme === "night");
    await page.getByRole("button", {name: "Light UI"}).click();
    await page.waitForFunction(() => !document.documentElement.dataset.theme &&
      localStorage.getItem("ori-ui-theme") === "day");

    // Layout stationarity & side-by-side cockpit check across 390x844 and 1280x800 in both themes.
    for (const night of [false, true]) {
      const toggleLabel = night ? "Dark UI" : "Light UI";
      const wantTheme = night ? "night" : "";
      if ((await page.evaluate(() => document.documentElement.dataset.theme ?? "")) !== wantTheme) {
        await page.getByRole("button", {name: toggleLabel}).click();
      }
      for (const vp of [{width: 390, height: 844}, {width: 1280, height: 800}]) {
        await page.setViewportSize(vp);
        await page.waitForFunction(() => canvas.width >= Math.round(canvas.clientWidth * (window.devicePixelRatio || 1)) - 1);
        const beforeStop = await page.locator("#stop").boundingBox();
        const beforeCam = await preview.boundingBox();
        if (vp.width >= 840) {
          const ctrlBox = await page.locator(".controls").boundingBox();
          assert(ctrlBox.x >= beforeCam.x + beforeCam.width - 2, {ctrlBox, beforeCam});
        }
        await page.evaluate(() => {
          window.prevStatusLayout = status;
          status = {...status, live_hardware: true, battery_percent: 82, rdt_connected: true, cutoff_verified: true,
            state: "READY", owner: driver, follow_blocker: "follow-locked"};
          update();
        });
        const afterStop = await page.locator("#stop").boundingBox();
        const afterCam = await preview.boundingBox();
        assert(Math.abs(beforeStop.x - afterStop.x) <= 0.5 && Math.abs(beforeStop.y - afterStop.y) <= 0.5,
          {vp, night, beforeStop, afterStop});
        assert(Math.abs(beforeCam.x - afterCam.x) <= 0.5 && Math.abs(beforeCam.y - afterCam.y) <= 0.5,
          {vp, night, beforeCam, afterCam});
        await page.evaluate(() => { status = window.prevStatusLayout; update(); });
      }
    }
    if (await page.evaluate(() => document.documentElement.dataset.theme === "night")) {
      await page.getByRole("button", {name: "Light UI"}).click();
    }
    await page.setViewportSize({width: 390, height: 844});

    // Exclusive tool tabs preserve focus, expose tools, and serial policy/motor queues + rollback work.
    for (const [panelId, tabName] of [["patrol-tools", "Patrol"], ["room-tools", "Dock"],
      ["clip-tools", "Clips"], ["autonomy", "Auto"], ["diagnostics", "Diag"]]) {
      const tab = page.locator(`#tool-tabs button[data-panel="${panelId}"]`);
      if (await tab.isHidden()) continue;
      if (await page.evaluate(id => document.getElementById(id).open, panelId)) {
        await tab.click();
      }
      await tab.click();
      const check = await page.evaluate(id => ({
        open: document.getElementById(id).open,
        othersOpen: [...document.querySelectorAll("details")].filter(d => d.id !== id).some(d => d.open),
        focused: document.activeElement?.dataset?.panel,
      }), panelId);
      assert.deepEqual(check, {open: true, othersOpen: false, focused: panelId});
    }
    await page.locator('#tool-tabs button[data-panel="autonomy"]').click();
    await page.evaluate(async () => {
      const api = await transport;
      window.origControlPolicy = api.control;
      window.policyCalls = [];
      api.control = async (action, payload) => {
        if (action === "policy") {
          window.policyCalls.push(payload.policy);
          if (window.policyCalls.length === 1) await new Promise(r => setTimeout(r, 120));
        }
        return window.origControlPolicy(action, payload);
      };
    });
    const autoReacq = page.locator('input[data-policy="auto_reacquire"]');
    const autoGap = page.locator('input[data-policy="auto_gap"]');
    await autoReacq.click();
    await autoGap.click();
    assert.equal(await autoReacq.isChecked(), true);
    assert.equal(await autoGap.isChecked(), true);
    assert.equal(await page.locator("#stop").isDisabled(), false);
    assert.equal(await page.evaluate(() => buttons.brake.disabled), false);
    await page.waitForFunction(() => policyInFlight === 0 &&
      status.policy?.auto_reacquire === true && status.policy?.auto_gap === true);
    await page.evaluate(async () => {
      const api = await transport;
      api.control = async (action, payload) => {
        if (action === "policy") return {...status, accepted: false, reason: "factory-veto"};
        return window.origControlPolicy(action, payload);
      };
    });
    await autoGap.click();
    await page.waitForFunction(() => policyInFlight === 0);
    assert.equal(await autoGap.isChecked(), true);
    assert.match(await page.locator("#help").textContent(), /blocked movement/i);
    await page.evaluate(async () => { (await transport).control = window.origControlPolicy; });

    await page.getByText("Manual drive", {exact: true}).click();
    await page.getByText("Patrol area", {exact: true}).click();
    await page.getByRole("button", {name: "Set patrol area"}).click();
    const map = page.locator("#patrol-map");
    await map.click({position: {x: 25, y: 25}});
    await map.click({position: {x: 175, y: 100}});
    await page.getByRole("button", {name: "Save patrol area"}).click();
    const startPatrol = page.getByRole("button", {name: "Start simulator patrol"});
    await startPatrol.click();
    await page.getByText(/Lap 1 of 1/).waitFor();
    await page.getByRole("button", {name: "Stop", exact: true}).click();
    await page.getByText("Patrol ended.", {exact: true}).waitFor();
    await startPatrol.click();
    await page.getByText("Auto patrol complete.", {exact: true}).waitFor();
    await testGuidedPatrol(browser, base);

    await page.locator('#tool-tabs button[data-panel="drive-pad"]').click();
    const pad = page.getByRole("button", {name: "Drive joypad"});
    await pad.scrollIntoViewIfNeeded();
    const joystick = await pad.boundingBox();
    await page.mouse.move(joystick.x + joystick.width * .5, joystick.y + joystick.height * .5);
    await page.mouse.down();
    await page.mouse.move(joystick.x + joystick.width * .7, joystick.y + joystick.height * .2);
    await page.waitForFunction(() => status.manual_active && manualAxes.ly > .1 && manualAxes.rx > .06);
    await page.mouse.up();
    await page.waitForFunction(() => !status.manual_active && manualTimer === null);
    // Closing the drive panel or switching tabs cancels held input before releasing the key.
    await pad.focus();
    await page.keyboard.down("ArrowUp");
    await page.waitForFunction(() => status.manual_active);
    await page.evaluate(() => { drivePad.open = false; });
    await page.waitForFunction(() => !status.manual_active && manualTimer === null);
    await page.keyboard.up("ArrowUp");
    await page.locator('#tool-tabs button[data-panel="drive-pad"]').click();
    await pad.focus();
    await page.keyboard.down("ArrowUp");
    await page.waitForFunction(() => status.manual_active);
    await page.locator('#tool-tabs button[data-panel="room-tools"]').click();
    await page.waitForFunction(() => !drivePad.open && !status.manual_active && manualTimer === null);
    await page.keyboard.up("ArrowUp");
    await page.getByText("Manual drive", {exact: true}).click();
    const stale = await page.evaluate(async () => {
      return (await transport).control("manual", {driver, boot: status.boot, generation: status.generation,
        ly: .2, rx: 0, ttl: 250, sequence: manualSequence - 1});
    });
    assert.equal(stale.accepted, false);
    const speed = page.getByLabel("Speed");
    assert.deepEqual(await speed.locator("option").allTextContents(), ["Slow", "Normal", "Fast", "Turbo", "Max"]);
    await speed.selectOption("0.72");
    await pad.focus();
    await page.keyboard.down("ArrowUp");
    await page.keyboard.down("ArrowRight");
    await page.waitForFunction(() => status.manual_active && manualAxes.ly === .72 && manualAxes.rx === .28);
    await page.getByRole("button", {name: "Brake", exact: true}).click();
    await page.keyboard.up("ArrowUp");
    await page.keyboard.up("ArrowRight");
    await page.waitForFunction(() => !status.manual_active && manualTimer === null);
    // Rejecting motors rolls back UI setting; accepting invert_turn flips manual turn once.
    const invertBox = page.getByLabel("Invert turn");
    await page.evaluate(async () => {
      const api = await transport;
      window.origControlMotors = api.control;
      api.control = async (action, payload) => {
        if (action === "motors") return {...status, accepted: false, reason: "docking"};
        return window.origControlMotors(action, payload);
      };
    });
    await invertBox.click();
    await page.waitForFunction(() => motorsInFlight === 0);
    assert.equal(await invertBox.isChecked(), false);
    assert.match(await page.locator("#help").textContent(), /dock controller/i);
    await page.evaluate(async () => { (await transport).control = window.origControlMotors; });
    await invertBox.click();
    await page.waitForFunction(() => motorsInFlight === 0 && status.invert_turn === true);
    await pad.focus();
    await page.keyboard.down("ArrowRight");
    await page.waitForFunction(() => status.manual_active && manualAxes.rx < 0);
    await page.keyboard.up("ArrowRight");
    await page.waitForFunction(() => !status.manual_active && manualTimer === null);
    await invertBox.click();
    await page.waitForFunction(() => motorsInFlight === 0 && status.invert_turn === false);

    // Step off dock hold-to-move: >1.4 s hold auto-stops and latches until release; keyboard hold/release works.
    await page.evaluate(async () => {
      const api = await transport;
      window.origFrameUndock = api.frame;
      window.origControlUndock = api.control;
      window.undockLog = [];
      window.undockWrappedCount = 0;
      api.frame = async (prev, statusTag) => {
        const res = await window.origFrameUndock(prev, statusTag);
        ++window.undockWrappedCount;
        if (res?.status) {
          res.status = {
            ...res.status, live_hardware: true, dock: "charging",
            manual_mode: "docked", rdt_connected: true,
          };
        }
        return res;
      };
      api.control = async (action, payload) => {
        if (action === "manual") window.undockLog.push({ly: payload.ly, rx: payload.rx});
        if (action === "manual") {
          return {...status, accepted: true, manual_active: Boolean(payload.ly || payload.rx)};
        }
        return window.origControlUndock(action, payload);
      };
    });
    await page.waitForFunction(() => window.undockWrappedCount >= 1);
    await page.evaluate(() => {
      status = {...status, live_hardware: true, dock: "charging", manual_mode: "docked", rdt_connected: true};
      update();
    });
    const undockBtn = page.getByRole("button", {name: "Step off dock"});
    await undockBtn.waitFor({state: "visible"});
    const uBox = await undockBtn.boundingBox();
    await page.mouse.move(uBox.x + uBox.width / 2, uBox.y + uBox.height / 2);
    await page.mouse.down();
    await page.waitForFunction(() => undockHolding && manualAxes.ly > 0 && manualAxes.rx === 0);
    await page.waitForFunction(() => undockLatched && !undockHolding && manualTimer === null, null, {timeout: 2500});
    // While still latched before pointer release, a second hold attempt must not start.
    assert.equal(await page.evaluate(() => beginUndockHold()), false);
    await page.mouse.up();
    await page.waitForFunction(() => !undockLatched);
    // After release, keyboard Enter hold and release both work cleanly.
    await page.evaluate(() => { window.undockLog = []; });
    await undockBtn.focus();
    await page.keyboard.down("Enter");
    await page.waitForFunction(() => undockHolding && window.undockLog.some(m => m.ly > 0 && m.rx === 0));
    await page.keyboard.up("Enter");
    await page.waitForFunction(() => !undockHolding && manualTimer === null &&
      window.undockLog.at(-1)?.ly === 0 && window.undockLog.at(-1)?.rx === 0);
    await page.evaluate(async () => {
      const api = await transport;
      api.frame = window.origFrameUndock;
      api.control = window.origControlUndock;
      lastTag = "";
    });
    await page.waitForFunction(() => !status.live_hardware && status.dock === "idle");
    await page.getByText("Camera clips", {exact: true}).click();
    const downloaded = page.waitForEvent("download");
    await page.getByRole("button", {name: "Save snapshot"}).click();
    assert.equal((await downloaded).suggestedFilename(), "ori-snapshot.png");
    const record = page.getByRole("button", {name: "Record clip"});
    if (await record.isVisible()) {
      await record.click();
      await page.waitForTimeout(400);
      await page.getByRole("button", {name: "End clip"}).click();
      await page.getByRole("link", {name: "Download clip"}).waitFor();
      assert.match(await page.locator("#playback").getAttribute("src"), /^blob:/);
    }
    await page.waitForFunction(() => navigator.serviceWorker.controller);
    if (disconnect || !useWebKit) {
      if (disconnect) await disconnect();
      else await page.context().setOffline(true);
      await page.reload({waitUntil: "domcontentloaded"});
      assert.equal(await page.title(), "Ori Companion");
      assert(await page.getByRole("button", {name: "Start Follow"}).isHidden());
    } else {
      assert(await page.evaluate(async () => Boolean(await caches.match(location.href))));
    }
    assert.deepEqual(errors, []);
    console.log(`Phone-sized browser flow passed (${useWebKit ? "WebKit" : "Chromium"}, ${isStatic ? "WebAssembly" : "loopback"})`);
  } finally {
    await browser.close();
  }
}

async function testOwnerAuth() {
  const server = spawn(process.env.PYTHON || "python3", ["demo/server.py", "--port", "0", "--require-auth"], {
    cwd: path.resolve(__dirname, ".."),
    stdio: ["ignore", "pipe", "inherit"],
  });
  const exited = once(server, "exit");
  const lines = createInterface({ input: server.stdout });
  const iterator = lines[Symbol.asyncIterator]();
  const browser = await launchBrowser();
  try {
    const first = await Promise.race([
      iterator.next(),
      exited.then(([code]) => { throw new Error("Auth server exited before startup: " + code); }),
    ]);
    const second = await iterator.next();
    const base = first.value.match(/^Ori replay demo at (http:\/\/127\.0\.0\.1:\d+)$/)?.[1];
    const pairUrl = second.value.match(/^Owner pairing URL: (http:\/\/127\.0\.0\.1:\d+\/\?token=.+)$/)?.[1];
    assert(base && pairUrl, "Expected pairing URL on startup");

    const context = await browser.newContext();
    const unauth = await context.request.get(base + "/");
    assert.equal(unauth.status(), 401);
    const unauthFrame = await context.request.get(base + "/api/frame");
    assert.equal(unauthFrame.status(), 401);

    const page = await context.newPage();
    await page.goto(pairUrl);
    await page.waitForFunction(() => displayed.frame > 0 && status.boot);
    assert.equal(page.url(), base + "/");
    await page.getByText("Family viewing", {exact: true}).click();
    assert.equal(await page.evaluate(() => document.getElementById("sharing").open), true);
    await page.getByRole("button", {name: "Create viewing link"}).click();
    const input = page.getByRole("textbox", {name: "Family viewing link"});
    await input.waitFor();
    const guest = await browser.newContext();
    const viewer = await guest.newPage();
    await viewer.goto(await input.inputValue());
    await viewer.waitForFunction(() => status.role === "viewer" && displayed.frame > 0);
    assert(await viewer.locator("#stop").isHidden());
    assert(await viewer.locator("#patrol-tools").isHidden());
    assert(await viewer.locator('#tool-tabs button[data-panel="sharing"]').isHidden());
    const blocked = await guest.request.post(base + "/api/stop", {headers: {Origin: base}, data: {}});
    assert.equal(blocked.status(), 403);
    await page.getByRole("button", {name: "Revoke viewing link"}).click();
    await page.getByText("Viewing link revoked.", {exact: true}).waitFor();
    assert.equal((await guest.request.get(base + "/api/frame")).status(), 401);
    console.log("Owner pairing, family view, and revocation passed");
  } finally {
    await browser.close();
    lines.close();
    server.kill("SIGINT");
    await exited;
  }
}

async function main() {
  if (process.env.ORI_DEMO_URL) return testDemo(process.env.ORI_DEMO_URL);
  const pages = process.env.ORI_TEST_PAGES === "1";
  const args = pages
    ? ["-u", "-m", "http.server", "0", "--bind", "127.0.0.1", "--directory", "build/pages"]
    : ["demo/server.py", "--port", "0"];
  const server = spawn(process.env.PYTHON || "python3", args, {
    cwd: path.resolve(__dirname, ".."),
    stdio: ["ignore", "pipe", "inherit"],
  });
  const exited = once(server, "exit");
  const lines = createInterface({ input: server.stdout });
  try {
    const [line] = await Promise.race([
      once(lines, "line"),
      exited.then(([code]) => { throw new Error("Demo exited before startup: " + code); }),
    ]);
    const address = pages
      ? line.match(/^Serving HTTP on 127\.0\.0\.1 port \d+ \((http:\/\/127\.0\.0\.1:\d+\/)\)/)
      : line.match(/^Ori replay demo at (http:\/\/127\.0\.0\.1:\d+)$/);
    if (!address) throw new Error("Unexpected demo startup message");
    await testDemo(address[1], async () => {
      server.kill("SIGINT");
      await exited;
    });
  } finally {
    lines.close();
    if (server.exitCode === null && !server.signalCode) server.kill("SIGINT");
    await exited;
  }
  if (!pages) {
    await testOwnerAuth();
  }
}

main().catch(error => {
  console.error(error);
  process.exitCode = 1;
});
