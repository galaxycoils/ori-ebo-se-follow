// Optional browser smoke test. Start make demo first.
const assert = require("node:assert/strict");
const { chromium } = require("playwright");

const base = process.env.ORI_DEMO_URL || "http://127.0.0.1:8765";

async function main() {
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
    await page.locator("#preview").click({ position: await page.locator("#preview").evaluate(canvas => ({
      x: canvas.clientWidth * 0.5, y: canvas.clientHeight * 105 / 180,
    })) });
    await page.getByRole("button", { name: "Confirm selection" }).click();
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
    await page.unroute("**/api/frame");
    await page.getByRole("button", { name: "Stop", exact: true }).click();
    await page.waitForFunction(() => status.reason === "owner-stop");
    assert.deepEqual(errors, []);
    console.log("Phone-sized browser flow passed");
  } finally {
    await browser.close();
  }
}

main().catch(error => {
  console.error(error);
  process.exitCode = 1;
});
