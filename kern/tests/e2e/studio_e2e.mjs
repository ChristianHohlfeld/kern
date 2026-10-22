// Browser end-to-end test of Kern Studio and packaged games (Playwright + Chromium).
//
//   node kern/tests/e2e/studio_e2e.mjs [base-url]     (default http://127.0.0.1:8771)
//
// Serve the repository root first (python3 -m http.server 8771). The test opens
// generated/studio.html (the single-file build) and kern/web/studio/index.html
// (the module build), drives the editor through its UI - menus, viewport fly
// navigation, the outliner, the details panel, the content drawer, Play and
// Simulate - packages a game and plays the packaged page.
import {createRequire} from "node:module";
import fs from "node:fs";
import path from "node:path";
import {pathToFileURL} from "node:url";
import os from "node:os";

// ESM ignores NODE_PATH; fall back to require() for a global Playwright install.
const {chromium} = await import("playwright").catch(() => createRequire(import.meta.url)("playwright"));
const base = process.argv[2] || "http://127.0.0.1:8771";
const shots = process.env.KERN_E2E_SHOTS ? path.resolve(process.env.KERN_E2E_SHOTS) : fs.mkdtempSync(path.join(os.tmpdir(), "kern-e2e-"));
fs.mkdirSync(shots, {recursive: true});
let failures = 0, checks = 0;
const annotate = text => { if (process.env.GITHUB_ACTIONS) console.log("::error::" + String(text).replace(/\r?\n/g, " ").slice(0, 900)); };
const check = (ok, what) => { checks++; if (!ok) { failures++; console.log("FAIL", what); annotate("e2e: " + what); } else console.log("ok  ", what); };
process.on("uncaughtException", e => { annotate("e2e crashed: " + (e?.stack || e)); console.error(e); process.exit(1); });
process.on("unhandledRejection", e => { annotate("e2e crashed: " + (e?.stack || e)); console.error(e); process.exit(1); });

const shot = (page, name) => page.screenshot({path: path.join(shots, name), timeout: 180000}).catch(e => console.log("screenshot", name, "skipped:", e.message.split("\n")[0]));
const browser = await chromium.launch({args: ["--use-gl=angle", "--use-angle=swiftshader", "--enable-unsafe-swiftshader", "--ignore-gpu-blocklist"]});

// Software WebGL (SwiftShader) renders the city at about one frame per second,
// so every check waits for its effect instead of a fixed time.
async function openStudio(url, viewport = {width: 1280, height: 780}) {
  const ctx = await browser.newContext({viewport, acceptDownloads: true});
  const page = await ctx.newPage();
  const errors = [];
  page.on("pageerror", e => errors.push(e.message));
  page.on("console", m => { if (m.type() === "error") errors.push(m.text()); });
  await page.goto(url);
  await page.waitForFunction(() => window.KernStudio && window.KernStudio.frames() > 5, null, {timeout: 90000});
  await page.waitForTimeout(400);
  return {ctx, page, errors};
}

/** Polls `fn` until it returns truthy or `ms` pass; returns the last value. */
async function until(fn, ms = 20000) {
  const end = Date.now() + ms;
  let v;
  while (!(v = await fn()) && Date.now() < end) await new Promise(r => setTimeout(r, 50));
  return v;
}

/** Number of distinct colours in the canvas (proves the renderer drew a scene). */
async function ink(page, id = "gl") {
  return page.evaluate(id => new Promise(resolve => requestAnimationFrame(() => {
    const c = document.getElementById(id);
    const t = document.createElement("canvas");
    t.width = 160; t.height = 100;
    const g = t.getContext("2d");
    g.drawImage(c, 0, 0, t.width, t.height);
    const d = g.getImageData(0, 0, t.width, t.height).data;
    const colors = new Set();
    for (let i = 0; i < d.length; i += 4) colors.add((d[i] >> 4) << 8 | (d[i + 1] >> 4) << 4 | d[i + 2] >> 4);
    resolve(colors.size);
  })), id);
}

const state = page => page.evaluate(() => window.KernStudio.state);
const status = page => page.evaluate(() => window.KernStudio.status);
const cmd = (page, c) => page.evaluate(c => window.KernStudio.command(c), c);
const vpCenter = async (page, i = 0) => {
  const r = await page.evaluate(i => window.KernStudio.viewportRect(i), i);
  return {x: r.left + r.width / 2, y: r.top + r.height / 2, r};
};

// ---- single-file studio ---------------------------------------------------------
{
  const {ctx, page, errors} = await openStudio(base + "/generated/studio.html");
  let s = await state(page);
  check(s.level.name === "Orb Run", "starts with the Orb Run level");
  check(s.outliner.rows.some(r => r.folder === "Orbs") && s.outliner.rows.some(r => r.name === "Hover Car"), "outliner shows the Orbs folder and the hover car");
  check(await page.locator("#outliner .row").count() === s.outliner.rows.length, "outliner rendered into the DOM");
  check(await ink(page) > 40, "the viewport renders the city");
  await shot(page, "studio.png");

  // Menus.
  await page.click('.mb[data-menu="file"]');
  check(await page.locator(".menu .mi").count() > 5, "File menu opens");
  await page.hover('.mb[data-menu="edit"]');
  check(await until(async () => (await page.locator(".menu .mi").allTextContents()).some(t => t.includes("Duplicate"))), "hovering the menu bar switches to Edit");
  await page.keyboard.press("Escape");
  check(await page.locator(".menu").count() === 0, "Escape closes menus");

  // Fly mode: hold the right mouse button and W.
  const c = await vpCenter(page);
  const before = (await status(page)).camera.position;
  await page.mouse.move(c.x, c.y);
  await page.mouse.down({button: "right"});
  await page.keyboard.down("KeyW");
  await until(async () => {
    const p = (await status(page)).camera.position;
    return Math.hypot(p[0] - before[0], p[1] - before[1], p[2] - before[2]) > 1;
  }, 30000);
  await page.keyboard.up("KeyW");
  await page.mouse.move(c.x + 60, c.y, {steps: 4});
  await page.mouse.up({button: "right"});
  const after = (await status(page)).camera.position;
  const flown = Math.hypot(after[0] - before[0], after[1] - before[1], after[2] - before[2]);
  check(flown > 1, `right mouse + W flies the camera (${flown.toFixed(1)} m)`);
  check((await state(page)).tool === "translate", "W while flying does not switch tools");
  check(await page.locator(".menu").count() === 0, "a right-drag does not open the context menu");

  // Tools by keyboard and toolbar.
  await page.keyboard.press("KeyE");
  check(await until(async () => (await state(page)).tool === "rotate"), "E selects the rotate tool");
  await page.click('[data-tool="scale"]');
  check(await until(async () => (await state(page)).tool === "scale"), "toolbar selects the scale tool");
  await page.keyboard.press("KeyW");

  // Outliner selection and the details panel.
  await page.locator("#outliner .row", {hasText: "Hover Car"}).first().click();
  check(await until(async () => (await state(page)).selection.length === 1), "clicking an outliner row selects the actor");
  check(await until(async () => (await page.locator("#details .sec-head", {hasText: "Player Controller"}).count()) > 0), "details show the player controller");
  check(await page.locator("#details .sec-head", {hasText: "Rigid Body"}).count() > 0, "details show the Box3D body");
  // Edit the X location through the panel.
  const x = page.locator('#details input.num[data-path="transform.position"][data-part="0"]');
  await x.click();
  await x.fill("12.5");
  await x.press("Enter");
  check(await until(async () => {
    const st = await state(page);
    const f = st.details.sections.find(sec => sec.id === "transform").fields.find(f => f.path === "transform.position");
    return Math.abs(f.value[0] - 12.5) < 1e-3;
  }), "typing a location moves the actor");
  await page.keyboard.press("Control+z");
  check(await until(async () => {
    const st = await state(page);
    const f = st.details.sections.find(sec => sec.id === "transform").fields.find(f => f.path === "transform.position");
    return Math.abs(f.value[0] - 12.5) > 0.01;
  }), "Ctrl+Z undoes the edit");
  // Focus with F.
  await page.mouse.move(c.x, c.y);
  await page.keyboard.press("KeyF");
  await page.waitForTimeout(200);
  await shot(page, "studio-selected.png");

  // Content drawer: click and drag to place actors.
  await page.keyboard.press("Control+Space");
  check(await until(async () => await page.locator("#drawer").isVisible()), "Ctrl+Space opens the content drawer");
  const n0 = (await state(page)).level.actors;
  await page.locator('.tile[data-item="physics_cube"]').click();
  check(await until(async () => (await state(page)).level.actors === n0 + 1), "clicking a tile places an actor");
  await page.locator('.tile[data-item="orb"]').dragTo(page.locator('.vp[data-index="0"]'), {targetPosition: {x: 300, y: 300}});
  check(await until(async () => (await state(page)).level.actors === n0 + 2), "dragging a tile into the viewport places an actor");
  await page.locator('.tile[data-item="hover_car"]').click();
  await page.keyboard.press("Control+z");
  check(await until(async () => (await state(page)).level.actors === n0 + 2), "undo removes the placed prefab");
  await page.keyboard.press("Control+Space");

  // Context menu on a right click (no drag).
  await page.mouse.click(c.x, c.y, {button: "right"});
  check(await until(async () => (await page.locator(".menu .mi").count()) > 2), "right click opens the viewport context menu");
  await page.keyboard.press("Escape");

  // Right clicks belong to the engine everywhere: the browser menu never opens, panels get engine menus.
  const native = await page.evaluate(() => {
    const out = {};
    for (const sel of ["#outliner", "#details", "#toolbar", "#statusbar", "#menubar", "body", "#details input"]) {
      const el = document.querySelector(sel);
      if (!el) continue;
      const ev = new MouseEvent("contextmenu", {bubbles: true, cancelable: true, clientX: 200, clientY: 200, button: 2});
      el.dispatchEvent(ev);
      out[sel] = ev.defaultPrevented;
      document.querySelectorAll(".menu").forEach(m => m.remove());
    }
    return out;
  });
  check(Object.values(native).length >= 5 && Object.values(native).every(Boolean), "no native browser context menu anywhere: " + JSON.stringify(native));
  await page.locator("#details .f input.num").first().click({button: "right"});
  check(await until(async () => (await page.locator(".menu .mi", {hasText: "Reset to Default"}).count()) === 1), "details fields have an engine context menu");
  await page.keyboard.press("Escape");
  await page.mouse.click(5, 5);

  // Deterministic undo: a scrub is one step, undo and redo restore exactly.
  const histBefore = (await state(page)).history?.index;
  const docBefore = await page.evaluate(() => window.KernStudio.engine.command({cmd: "packageText"}).text ?? "");
  const scrubLabel = page.locator("#details label.scrub").first();
  if (await scrubLabel.count()) {
    const lb = await scrubLabel.boundingBox();
    await page.mouse.move(lb.x + lb.width / 2, lb.y + lb.height / 2);
    await page.mouse.down();
    for (let k = 1; k <= 8; k++) await page.mouse.move(lb.x + lb.width / 2 + k * 6, lb.y + lb.height / 2);
    await page.mouse.up();
    const after = (await state(page)).history;
    check(after?.index === histBefore + 1, `a scrub of ${8} moves is one undo step (${histBefore} -> ${after?.index})`);
    await page.keyboard.press("Control+z");
    const docUndo = await page.evaluate(() => window.KernStudio.engine.command({cmd: "packageText"}).text ?? "");
    check(await until(async () => (await state(page)).history?.index === histBefore) && docUndo === docBefore, "undo restores the exact document");
    await page.keyboard.press("Control+y");
    check(await until(async () => (await state(page)).history?.index === histBefore + 1), "redo re-applies the scrub");
    await page.keyboard.press("Control+z");
  }

  // Play in the viewport.
  await page.click("#playBtn");
  check(await until(async () => (await state(page)).play.state === "playing"), "Play starts the game");
  check(await until(async () => (await page.locator(".vp.active .k-hud-stats").count()) > 0), "the HUD shows while playing");
  await page.locator(".vp.active").click({position: {x: 200, y: 200}}).catch(() => {});
  await page.keyboard.down("KeyW");
  const playing = await until(async () => { const st = await status(page); return st.hud?.speed > 2 && st; }, 45000) || await status(page);
  await page.keyboard.up("KeyW");
  check(playing.hud && playing.hud.speed > 2, `W drives the hover car (${playing.hud?.speed} m/s)`);
  check(playing.hud?.pickupsTotal >= 24, `orbs to collect (${playing.hud?.pickupsTotal})`);
  await shot(page, "studio-play.png");
  await page.keyboard.press("F8");
  check(await until(async () => (await state(page)).play.state === "simulating"), "F8 ejects from the player");
  await page.keyboard.press("Escape");
  if ((await state(page)).play.state !== "editing") await page.keyboard.press("Escape");
  check(await until(async () => (await state(page)).play.state === "editing"), "Escape stops playing");

  // Simulate with the physics debug view.
  await cmd(page, {cmd: "showFlag", viewport: 0, flag: "colliders", value: true});
  await page.click("#simBtn");
  check(await until(async () => (await status(page)).sim?.bodies > 10), "Simulate builds the Box3D world");
  await page.waitForTimeout(400);
  await shot(page, "studio-simulate.png");
  await page.click("#stopBtn");
  check(await until(async () => (await state(page)).play.state === "editing"), "Stop ends the simulation");
  await cmd(page, {cmd: "showFlag", viewport: 0, flag: "colliders", value: false});

  // Four viewports.
  await page.click("#layoutBtn");
  await page.locator(".menu .mi", {hasText: "4 viewports"}).click();
  check(await until(async () => (await page.locator(".vp:not(.hidden)").count()) === 4), "quad layout shows four viewports");
  await page.waitForTimeout(300);
  check(await ink(page) > 40, "quad layout renders");
  await shot(page, "studio-quad.png");
  await page.locator('.vp[data-index="0"] [data-vp="maximize"]').click();
  check(await until(async () => (await page.locator(".vp:not(.hidden)").count()) === 1), "maximise returns to one viewport");

  // New level from a template via the dialog.
  await page.click('.mb[data-menu="file"]');
  await page.locator(".menu .mi", {hasText: "New Level"}).click();
  check(await until(async () => (await page.locator(".dialog .tpl").count()) === 4), "New Level shows four templates");
  await page.locator(".tpl", {hasText: "Physics Lab"}).click();
  await page.locator(".dialog .btn.primary").click();
  check(await until(async () => (await state(page)).level.name === "Physics Lab"), "Physics Lab opens");
  await page.click("#simBtn");
  check(await until(async () => (await status(page)).sim?.joints >= 10), "Physics Lab simulates its joints");
  await page.waitForTimeout(800);
  await shot(page, "studio-physics-lab.png");
  await page.click("#stopBtn");

  // Help.
  await page.keyboard.press("F1");
  check(await until(async () => (await page.locator(".dialog .keys tr").count()) > 10), "F1 shows the controls");
  await page.keyboard.press("Escape");

  // Package a game and play the packaged page.
  await cmd(page, {cmd: "new", template: "orbRun"});
  const html = await page.evaluate(() => window.KernStudio.packageHtml("Orb Run"));
  check(html.length > 500000 && html.includes("startGame"), `packaged game written (${(html.length / 1048576).toFixed(1)} MB)`);
  const gameFile = path.join(shots, "orb-run-game.html");
  fs.writeFileSync(gameFile, html);
  check(errors.length === 0, "no page errors in the studio" + (errors.length ? ": " + errors.slice(0, 3).join(" | ") : ""));
  await ctx.close();

  const gctx = await browser.newContext({viewport: {width: 1100, height: 680}});
  const game = await gctx.newPage();
  const gerrors = [];
  game.on("pageerror", e => gerrors.push(e.message));
  await game.goto(pathToFileURL(gameFile).href);
  await game.waitForFunction(() => window.KernGame, null, {timeout: 90000});
  check(await until(async () => (await game.evaluate(() => window.KernGame.status.hud?.status)) === "playing"), "the packaged game plays");
  await game.click("#gl", {position: {x: 500, y: 300}}).catch(() => {});
  await game.keyboard.down("KeyW");
  const gs = await until(async () => { const st = await game.evaluate(() => window.KernGame.status); return st.hud?.speed > 2 && st; }, 45000) || await game.evaluate(() => window.KernGame.status);
  await game.keyboard.up("KeyW");
  check(gs.hud.speed > 2, `the packaged game drives (${gs.hud.speed} m/s)`);
  check(await ink(game) > 40, "the packaged game renders");
  check(await game.locator(".k-hud-stats").count() === 1, "the packaged game shows its HUD");
  await shot(game, "game.png");
  check(gerrors.length === 0, "no page errors in the game" + (gerrors.length ? ": " + gerrors.join(" | ") : ""));
  await gctx.close();
}

// ---- module build (development page) ------------------------------------------------
{
  const {ctx, page, errors} = await openStudio(base + "/kern/web/studio/index.html", {width: 1280, height: 800});
  const s = await state(page);
  check(s.level.name === "Orb Run", "module build starts with Orb Run");
  check(await ink(page) > 40, "module build renders");
  check(errors.length === 0, "no page errors in the module build" + (errors.length ? ": " + errors.slice(0, 3).join(" | ") : ""));
  await ctx.close();
}

// ---- narrow window ----------------------------------------------------------------------
{
  const {ctx, page, errors} = await openStudio(base + "/generated/studio.html", {width: 820, height: 900});
  check(await ink(page) > 20, "narrow layout renders");
  const overflow = await page.evaluate(() => document.documentElement.scrollWidth > innerWidth + 1);
  check(!overflow, "no horizontal page scroll at 820 px");
  await shot(page, "studio-narrow.png");
  check(errors.length === 0, "no page errors in the narrow layout");
  await ctx.close();
}

await browser.close();
console.log(`${checks} checks, ${failures} failures (screenshots: ${shots})`);
process.exit(failures ? 1 : 0);
