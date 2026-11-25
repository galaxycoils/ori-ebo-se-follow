// Uses a fresh loopback demo unless ORI_DEMO_URL selects an existing server.
const assert = require("node:assert/strict");
const { chromium } = require("playwright");
const { existsSync } = require("node:fs");

const { spawn } = require("node:child_process");
const { once } = require("node:events");
const { createInterface } = require("node:readline");
const path = require("node:path");

function launchBrowser() {
  return chromium.launch({
    executablePath: process.env.CHROMIUM || (existsSync("/usr/bin/chromium") ? "/usr/bin/chromium" : undefined),
    headless: true,
  });
}

async function testDemo(base) {
  const browser = await launchBrowser();
  try {
    const page = await browser.newPage({ viewport: { width: 390, height: 844 } });
    const errors = [];
    page.on("pageerror", error => errors.push(error));
    await page.goto(base);
    await page.evaluate(async () => { await transport; });
    await page.waitForFunction(() => displayed.frame > 0 && status.boot);
    const isStatic = await page.evaluate(() => document.documentElement.dataset.preview === "pages");
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

    // Selection expiry returns preview cleanly to live video.
    await drawBox();
    await page.waitForFunction(() => selection === null && hint.includes("expired"));
    assert(await page.getByRole("button", { name: "Confirm selection" }).isHidden());
    await page.waitForFunction(() => previewIsLive());

    // Bottom-right border drag clamps cleanly within valid Box::visible() bounds.
    await drawBox(240, 100, 320, 180);
    const clamped = await page.evaluate(() => selection.box);
    assert(clamped[0] + clamped[2] <= 319 && clamped[1] + clamped[3] <= 179, clamped);
    await page.keyboard.press("Escape");

    // Manual drag box -> confirm -> Stop immediately clears displayed.box.
    await drawBox();
    await page.getByRole("button", { name: "Confirm selection" }).click();
    await page.waitForFunction(() => status.state === "TRACKING" && Array.isArray(displayed.box));
    await page.getByRole("button", { name: "Stop", exact: true }).click();
    await page.waitForFunction(() => status.state === "OFF" && displayed.box === null && !pending);

    // Tap target -> teach -> follow -> track-only -> move demo target.
    await preview.click({ position });
    await page.waitForFunction(() => status.state === "TRACKING");
    const selectedFrame = await page.evaluate(() => status.frame);
    await page.waitForFunction(frame => status.frame >= frame + 3, selectedFrame);
    await page.getByRole("button", { name: "Set gap here" }).click();
    await page.waitForFunction(() => status.state === "READY");

    await page.getByRole("button", { name: "Follow", exact: true }).click();
    await page.waitForFunction(() => status.state === "FOLLOWING");
    await page.waitForFunction(() => status.motion === "FORWARD");
    assert(await page.evaluate(() => window.observedMotion.has("LEFT")), "The moving target must first cause a turn");
    await page.getByRole("button", { name: "Track only" }).click();
    await page.waitForFunction(() => status.state === "READY" && status.motion === "HOLD");
    await page.setViewportSize({ width: 844, height: 390 });
    await page.evaluate(() => window.observedMotion.clear());
    await page.getByRole("button", { name: "Move demo target" }).click();
    await page.waitForTimeout(3000);
    assert.deepEqual(await page.evaluate(() => [...window.observedMotion]), ["HOLD"]);

    await page.getByRole("button", { name: "Follow", exact: true }).click();
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
    assert.equal(afterFreeze.reason, "preview-expired");
    assert.equal(afterFreeze.motion, "HOLD");
    assert.match(await page.locator("#motion").textContent(), /unknown/);
    await page.evaluate(async () => { (await transport).frame = window.liveFrame; });
    await page.waitForFunction(() => previewIsLive() && status.state === "OFF");
    await page.getByRole("button", { name: "Stop", exact: true }).click();
    await page.waitForFunction(() => status.reason === "owner-stop");
    await page.getByRole("button", { name: "Return home" }).click();
    await page.waitForFunction(() => status.dock === "returning");
    await page.getByRole("button", { name: "Stop", exact: true }).click();
    await page.waitForFunction(() => status.dock === "idle");
    await page.getByRole("button", { name: "Return home" }).click();
    await page.waitForFunction(() => status.dock === "charging");
    assert(await page.getByRole("button", { name: "Return home" }).isHidden());
    await page.getByRole("button", { name: "Lift off base (demo)" }).click();
    await page.waitForFunction(() => status.dock === "idle" && status.state === "OFF");
    assert.deepEqual(errors, []);
    console.log(`Phone-sized browser flow passed (${isStatic ? "WebAssembly" : "loopback"})`);
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
    console.log("Owner-token pairing authentication check passed");
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
    await testDemo(address[1]);
  } finally {
    lines.close();
    server.kill("SIGINT");
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
