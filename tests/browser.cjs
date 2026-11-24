// Uses a fresh loopback demo unless ORI_DEMO_URL selects an existing server.
const assert = require("node:assert/strict");
const { chromium } = require("playwright");

const { spawn } = require("node:child_process");
const { once } = require("node:events");
const { createInterface } = require("node:readline");
const path = require("node:path");

async function testDemo(base) {
  const browser = await chromium.launch({
    executablePath: process.env.CHROMIUM || "/usr/bin/chromium",
    headless: true,
  });
  try {
    const page = await browser.newPage({ viewport: { width: 390, height: 844 } });
    const errors = [];
    page.on("pageerror", error => errors.push(error));
    await page.goto(base);
    await page.waitForFunction(() => displayed.frame > 0 && status.boot);
    const previousBoot = await page.request.post(base + "/api/stop", {
      headers: { Origin: base },
      data: { driver: 1, generation: 1, boot: "previous-instance" },
    });
    assert.equal((await previousBoot.json()).accepted, false);
    const crossOrigin = await page.request.post(base + "/api/stop", {
      headers: { Origin: "https://example.com" },
      data: {},
    });
    assert.equal(crossOrigin.status(), 403);
    const preview = page.locator("#preview");
    const position = await preview.evaluate(canvas => ({
      x: canvas.clientWidth * 0.5, y: canvas.clientHeight * 105 / 180,
    }));
    const bounds = await preview.boundingBox();
    async function drawBox() {
      await page.mouse.move(bounds.x + bounds.width * 120 / 320, bounds.y + bounds.height * 65 / 180);
      await page.mouse.down();
      await page.mouse.move(bounds.x + bounds.width * 200 / 320, bounds.y + bounds.height * 145 / 180, { steps: 4 });
      await page.mouse.up();
    }
    await drawBox();
    await page.waitForFunction(() => selection === null && hint.includes("expired"));
    assert(await page.getByRole("button", { name: "Confirm selection" }).isHidden());
    await page.waitForFunction(() => previewIsLive());
    await drawBox();
    await page.getByRole("button", { name: "Confirm selection" }).click();
    await page.waitForFunction(() => status.state === "TRACKING");
    await page.getByRole("button", { name: "Stop", exact: true }).click();
    await page.waitForFunction(() => status.state === "OFF" && !pending);
    await preview.click({ position });
    await page.waitForFunction(() => status.state === "TRACKING");
    const selectedFrame = await page.evaluate(() => status.frame);
    await page.waitForFunction(frame => status.frame >= frame + 3, selectedFrame);
    await page.getByRole("button", { name: "Set gap here" }).click();
    await page.waitForFunction(() => status.state === "READY");

    const observed = new Set();
    page.on("response", response => {
      const raw = response.headers()["x-ori-status"];
      if (raw) observed.add(JSON.parse(raw).motion);
    });
    await page.getByRole("button", { name: "Follow", exact: true }).click();
    await page.waitForFunction(() => status.state === "FOLLOWING");
    await page.waitForFunction(() => status.motion === "FORWARD");
    assert(observed.has("LEFT"), "The moving target must first cause a turn");
    await page.getByRole("button", { name: "Track only" }).click();
    await page.waitForFunction(() => status.state === "READY" && status.motion === "HOLD");
    await page.setViewportSize({ width: 844, height: 390 });
    observed.clear();
    await page.getByRole("button", { name: "Move demo target" }).click();
    await page.waitForTimeout(3000);
    assert.deepEqual([...observed], ["HOLD"]);

    await page.getByRole("button", { name: "Follow", exact: true }).click();
    await page.waitForFunction(() => status.state === "FOLLOWING");
    await page.route("**/api/frame", route => route.abort());
    await page.waitForTimeout(900);
    const response = await page.request.get(base + "/api/frame");
    const afterFreeze = JSON.parse(response.headers()["x-ori-status"]);
    assert.equal(afterFreeze.reason, "preview-expired");
    assert.equal(afterFreeze.motion, "HOLD");
    assert.match(await page.locator("#motion").textContent(), /unknown/);
    await page.unroute("**/api/frame");
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
    console.log("Phone-sized browser flow passed");
  } finally {
    await browser.close();
  }
}

async function main() {
  if (process.env.ORI_DEMO_URL) return testDemo(process.env.ORI_DEMO_URL);
  const server = spawn(process.env.PYTHON || "python3", ["demo/server.py", "--port", "0"], {
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
    const address = line.match(/^Ori replay demo at (http:\/\/127\.0\.0\.1:\d+)$/);
    if (!address) throw new Error("Unexpected demo startup message");
    await testDemo(address[1]);
  } finally {
    lines.close();
    server.kill("SIGINT");
    await exited;
  }
}

main().catch(error => {
  console.error(error);
  process.exitCode = 1;
});
