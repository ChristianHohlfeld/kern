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

// Screenshots are slow with software WebGL: only when KERN_E2E_SHOTS names a folder.
const shot = (page, name) => !process.env.KERN_E2E_SHOTS ? Promise.resolve()
  : page.screenshot({path: path.join(shots, name), timeout: 180000}).catch(e => console.log("screenshot", name, "skipped:", e.message.split("\n")[0]));
const browser = await chromium.launch({args: ["--use-gl=angle", "--use-angle=swiftshader", "--enable-unsafe-swiftshader", "--ignore-gpu-blocklist"]});

// Software WebGL (SwiftShader) renders the city at about one frame per second,
// so every check waits for its effect instead of a fixed time.
async function openStudio(url, viewport = {width: 1024, height: 640}) {
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

  // Left mouse: a click selects, dragging an actor slides it over the surfaces, the camera stays put.
  {
    const vr = (await vpCenter(page)).r;
    const prevSelection = (await state(page)).selection;
    await cmd(page, {cmd: "tool", tool: "select"});  // no gizmo in the way: grab the actor itself
    // A cube in the open (high above the city) so nothing else is under the cursor.
    await cmd(page, {cmd: "spawn", item: "cube", at: [40, 40, 120]});
    const id = (await state(page)).selection[0];
    await cmd(page, {cmd: "focus", ids: [id]});
    await page.waitForTimeout(200);
    const pos = async () => (await state(page)).details.sections.find(sec => sec.id === "transform").fields.find(f => f.path === "transform.position").value;
    const p0 = await pos();
    const cam0 = (await status(page)).camera.position;
    const pr = await cmd(page, {cmd: "project", id});
    check(pr.visible, "the new cube is on screen");
    await page.mouse.move(vr.left + pr.x, vr.top + pr.y);
    await page.mouse.down();
    for (let k = 1; k <= 10; k++) await page.mouse.move(vr.left + pr.x + k * 12, vr.top + pr.y + k * 3);
    await page.mouse.up();
    const st = await state(page);
    check(st.selection.length === 1 && st.selection[0] === id, `pressing an actor selects it (${JSON.stringify(st.selection)} / ${id})`);
    const p1 = await pos();
    const cam1 = (await status(page)).camera.position;
    check(Math.hypot(p1[0] - p0[0], p1[1] - p0[1]) > 0.5, `left-dragging an actor moves it (${Math.hypot(p1[0] - p0[0], p1[1] - p0[1]).toFixed(2)} m)`);
    check(Math.hypot(cam1[0] - cam0[0], cam1[1] - cam0[1], cam1[2] - cam0[2]) < 1e-4, "left-dragging never moves the camera");
    await page.keyboard.press("Control+z");
    check(await until(async () => { const p = await pos(); return Math.hypot(p[0] - p0[0], p[1] - p0[1]) < 1e-3; }), "undo puts the dragged actor back");
    // Shift + drag always draws a marquee (as does dragging empty space).
    await page.keyboard.down("Shift");
    await page.mouse.move(vr.left + 8, vr.top + 30);
    await page.mouse.down();
    await page.mouse.move(vr.left + vr.width - 8, vr.top + vr.height - 8, {steps: 6});
    await page.mouse.up();
    await page.keyboard.up("Shift");
    check(await until(async () => (await state(page)).selection.length > 1), "a marquee selects everything inside");
    await cmd(page, {cmd: "select", ids: prevSelection});
    await cmd(page, {cmd: "tool", tool: "translate"});
  }

  // Sound: the C++ mixer plays through Web Audio (worklet), built-ins preview, imported files decode.
  {
    await page.mouse.click(5, 5);
    check(await until(async () => (await page.evaluate(() => window.KernStudio.audio))?.state === "running", 10000), "the audio context runs after the first click");
    const a0 = await page.evaluate(() => window.KernStudio.audio);
    check(a0.mode === "worklet", `audio goes through an AudioWorklet (${a0.mode})`);
    check(a0.rate >= 22050, `the mixer runs at the device rate (${a0.rate} Hz)`);
    await page.waitForTimeout(400);
    const a1 = await page.evaluate(() => window.KernStudio.audio);
    check(a1.consumed > a0.consumed, "the device consumes the mixer's output");
    await page.keyboard.press("Control+Space");
    await page.click('#drawer .tab[data-dtab="sounds"]');
    check(await until(async () => (await page.locator("#soundPane .snd").count()) >= 25), "the Sounds tab lists the built-in library");
    await page.locator('#soundPane .snd[data-sound="builtin:quack"]').click();
    check(await until(async () => (await status(page)).audio?.voices > 0, 5000), "clicking a sound previews it");
    check(await until(async () => (await status(page)).audio?.peak?.[0] > 0.01, 5000), "the preview is audible on the meter");
    // Import a generated WAV file.
    const sid = await page.evaluate(async () => {
      const rate = 22050, n = Math.round(rate * 2), bytes = new DataView(new ArrayBuffer(44 + n * 2));
      const str = (o, t) => { for (let i = 0; i < t.length; i++) bytes.setUint8(o + i, t.charCodeAt(i)); };
      str(0, "RIFF"); bytes.setUint32(4, 36 + n * 2, true); str(8, "WAVEfmt "); bytes.setUint32(16, 16, true);
      bytes.setUint16(20, 1, true); bytes.setUint16(22, 1, true); bytes.setUint32(24, rate, true); bytes.setUint32(28, rate * 2, true);
      bytes.setUint16(32, 2, true); bytes.setUint16(34, 16, true); str(36, "data"); bytes.setUint32(40, n * 2, true);
      for (let i = 0; i < n; i++) bytes.setInt16(44 + i * 2, Math.round(Math.sin(i / rate * 2 * Math.PI * 660) * 12000), true);
      return window.KernStudio.importSoundFile(new File([bytes.buffer], "beep.wav", {type: "audio/wav"}));
    });
    check(!!sid, `an imported WAV becomes a sound asset (${sid})`);
    const st = await state(page);
    check(st.sounds.imported.some(x => x.id === sid && Math.abs(x.duration - 2) < 0.05), "the import knows its duration");
    await cmd(page, {cmd: "stopSound"});
    await page.waitForTimeout(150);
    await cmd(page, {cmd: "previewSound", sound: sid});
    check(await until(async () => (await state(page)).sounds.previewing && (await status(page)).audio?.voices > 0, 5000), "the imported sound decodes and plays");
    await page.keyboard.press("Control+z");
    check(await until(async () => !(await state(page)).sounds.imported.some(x => x.id === sid)), "undo removes the imported sound");
    // Mute.
    await page.click("#muteBtn");
    check(await until(async () => (await state(page)).sounds.muted), "the status bar mutes the sound");
    await page.click("#muteBtn");
    await page.keyboard.press("Control+Space");
  }

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
    await scrubLabel.scrollIntoViewIfNeeded();
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
  check(await until(async () => (await page.locator(".dialog .tpl").count()) === 5), "New Level shows five templates");
  await page.locator(".tpl", {hasText: "Physics Lab"}).click();
  await page.locator(".dialog .btn.primary").click();
  check(await until(async () => (await state(page)).level.name === "Physics Lab"), "Physics Lab opens");
  await page.click("#simBtn");
  check(await until(async () => (await status(page)).sim?.joints >= 10), "Physics Lab simulates its joints");
  await page.waitForTimeout(800);
  await shot(page, "studio-physics-lab.png");
  await page.click("#stopBtn");

  // Soldier (first-person shooter): the Shooting Range template, mouse fires, HUD shows ammo and crosshair.
  await cmd(page, {cmd: "new", template: "shootingRange"});
  await cmd(page, {cmd: "play", mode: "play"});
  check(await until(async () => (await status(page)).hud?.soldier?.ammo === 30, 30000), "Shooting Range plays a Soldier with a full magazine");
  check(await until(async () => (await page.locator(".vp .k-xh, .vp .k-xh-dot").count()) > 0, 20000), "the soldier HUD draws a crosshair");
  {
    const c = await vpCenter(page);
    await page.mouse.move(c.x, c.y);
    await page.mouse.down();  // capture click: does not fire
    await page.mouse.up();
    await page.mouse.down();
    check(await until(async () => (await status(page)).hud?.soldier?.ammo < 30, 120000), "holding the left mouse button fires");
    await page.mouse.up();
    await page.keyboard.press("Escape");
  }
  await page.waitForTimeout(300);
  if ((await state(page)).play?.state !== "editing") await cmd(page, {cmd: "stop"});

  // Logic (Blueprint-style graph): start node, drag a wire into empty space, search, type a value, play.
  {
    await cmd(page, {cmd: "new", template: "basic"});
    await page.click("#logicBtn");
    check(await until(async () => await page.locator("#logicEditor:not([hidden]) .lg-canvas").count() === 1, 30000), "the Logic button opens the graph editor");
    await page.click(".lg-starts [data-t='event.begin']");
    check(await until(async () => await page.locator(".lg-node").count() === 1, 30000), "a start button adds Begin Play");
    const pin = await page.locator(".lg-node .lg-row.out[data-pin='then'] .lg-pin").first().boundingBox();
    await page.mouse.move(pin.x + 5, pin.y + 5);
    await page.mouse.down();
    await page.mouse.move(pin.x + 220, pin.y + 60, {steps: 5});
    await page.mouse.up();
    check(await until(async () => await page.locator(".lg-palette").count() === 1, 20000), "dropping a wire on empty space opens the node search");
    await page.keyboard.type("show message");
    check(await until(async () => (await page.locator(".lg-pal-it.active").textContent())?.startsWith("Show Message"), 20000), "the search finds Show Message");
    await page.keyboard.press("Enter");
    check(await until(async () => (await state(page)).logic?.links?.length === 1, 20000), "the new node is wired to the pin it came from");
    const txt = page.locator(".lg-node input.lg-lit.txt").first();
    await txt.fill("Hello from Logic");
    await txt.press("Enter");
    check(await until(async () => (await state(page)).logic?.issues?.filter(i => i.error).length === 0, 20000), "the graph compiles");
    await page.click(".lg-back");
    await cmd(page, {cmd: "spawn", item: "rolling_ball"});
    await cmd(page, {cmd: "play", mode: "play"});
    check(await until(async () => (await status(page)).hud?.message === "Hello from Logic", 60000), "Play runs the graph");
    check(await until(async () => (await status(page)).logic?.compiled >= 1, 30000), "the graph is compiled to machine code (JIT)");
    await cmd(page, {cmd: "stop"});
  }

  // Help.
  await page.keyboard.press("F1");
  check(await until(async () => (await page.locator(".dialog .keys tr").count()) > 10), "F1 shows the controls");
  await page.keyboard.press("Escape");

  // Multiplayer in the editor: a listen server and a local client in the next viewport.
  await cmd(page, {cmd: "new", template: "orbRun"});
  await cmd(page, {cmd: "netPlayers", players: 2});
  await cmd(page, {cmd: "play", mode: "play"});
  check(await until(async () => (await status(page)).net?.clients?.[0]?.hud?.status === "playing", 45000), "2 players: the local client joins the listen server");
  check((await status(page)).net?.players === 2, "the server counts both players");
  await page.waitForTimeout(500);
  await shot(page, "studio-multiplayer.png");
  await cmd(page, {cmd: "stop"});
  await cmd(page, {cmd: "netPlayers", players: 1});
  await cmd(page, {cmd: "layout", layout: "single"});

  // Online between two tabs of the browser (BroadcastChannel transport, Kern Net on top).
  {
    // Both tabs play the lighter Shooting Range (software rendering of two cities is too slow here).
    await cmd(page, {cmd: "new", template: "shootingRange"});
    const guest = await ctx.newPage();
    guest.on("pageerror", e => errors.push("guest: " + e.message));
    await guest.goto(base + "/generated/studio.html");
    await guest.waitForFunction(() => window.KernStudio && window.KernStudio.frames() > 2, null, {timeout: 240000});
    await cmd(guest, {cmd: "new", template: "shootingRange"});
    await cmd(page, {cmd: "submitDialog", name: "netHost", values: {room: "e2e", relay: ""}});
    check(await until(async () => (await status(page)).net?.role === "server", 15000), "Host Online Game starts a server");
    await cmd(guest, {cmd: "submitDialog", name: "netJoin", values: {room: "e2e", relay: ""}});
    check(await until(async () => (await status(guest)).net?.status?.startsWith("Connected (slot"), 90000), "a second tab joins the hosted game");
    check(await until(async () => (await status(page)).net?.players === 2, 60000), "the host sees two players");
    const gs = await status(guest);
    check(gs.hud?.status === "playing", "the guest plays the host's game");
    await cmd(guest, {cmd: "netStop"});
    check(await until(async () => (await status(page)).net?.players === 1, 60000), "the host notices the guest leaving");
    await cmd(page, {cmd: "netStop"});
    await guest.close();
  }

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
  check(await until(async () => (await game.evaluate(() => window.KernGame.audio?.context?.state)) === "running", 10000), "the packaged game's audio runs");
  check(await until(async () => (await game.evaluate(() => window.KernGame.status.audio?.voices)) >= 2, 10000), "the packaged game plays music and the engine");
  check(await game.evaluate(() => window.KernGame.audio.mode) !== "none", `the packaged game outputs sound (${await game.evaluate(() => window.KernGame.audio.mode)})`);
  check(await ink(game) > 40, "the packaged game renders");
  check(await game.locator(".k-hud-stats").count() === 1, "the packaged game shows its HUD");
  await shot(game, "game.png");
  check(gerrors.length === 0, "no page errors in the game" + (gerrors.length ? ": " + gerrors.join(" | ") : ""));
  await gctx.close();

  // The same game on a phone: the engine detects touch, draws its sticks and never locks the pointer.
  const tctx = await browser.newContext({viewport: {width: 844, height: 390}, hasTouch: true, isMobile: true, deviceScaleFactor: 1});
  const phone = await tctx.newPage();
  const terrors = [];
  phone.on("pageerror", e => terrors.push(e.message));
  await phone.goto(pathToFileURL(gameFile).href);
  await phone.waitForFunction(() => window.KernGame, null, {timeout: 90000});
  const tst = await until(async () => { const st = await phone.evaluate(() => window.KernGame.status); return st.touch && st; }, 30000);
  check(tst?.input?.device === "touch", "a phone is detected as a touch device");
  check(await phone.locator(".k-touch .ts").count() === 2, "the touch sticks are drawn");
  const cdp = await tctx.newCDPSession(phone);
  const touch = (type, pts) => cdp.send("Input.dispatchTouchEvent", {type, touchPoints: pts.map(([x, y], i) => ({x, y, id: i + 1}))});
  const mx = tst.touch.move.x + tst.touch.move.size / 2, my = tst.touch.move.y + tst.touch.move.size / 2;
  await touch("touchStart", [[mx, my]]);
  for (let k = 1; k <= 6; k++) await touch("touchMove", [[mx, my - k * 14]]);
  const moving = await until(async () => (await phone.evaluate(() => window.KernGame.status.hud?.speed)) > 1.5, 30000);
  check(moving, "the left touch stick moves the player");
  check(await phone.evaluate(() => window.KernGame.status.touch?.move?.live) === true, "the stick shows the thumb");
  await touch("touchEnd", []);
  check(await phone.evaluate(() => !document.pointerLockElement), "touch play never locks the pointer");
  await shot(phone, "game-phone.png");
  check(terrors.length === 0, "no page errors on the phone" + (terrors.length ? ": " + terrors.join(" | ") : ""));
  await tctx.close();
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
