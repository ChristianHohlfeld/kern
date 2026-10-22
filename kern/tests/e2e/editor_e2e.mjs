// Browser end-to-end test of the Kern editor and player (Playwright + Chromium).
//
//   node kern/tests/e2e/editor_e2e.mjs [base-url]     (default http://127.0.0.1:8771)
//
// Serve the repository root first (python3 -m http.server 8771). The test opens
// generated/editor3d.html (the single-file build) and kern/web/editor/index.html
// (the module build), drives the editor through its UI and its command API,
// runs the player, exports a game and plays the exported page.
import {createRequire} from "node:module";
import fs from "node:fs";
import path from "node:path";
import os from "node:os";

// ESM ignores NODE_PATH; fall back to require() for a global Playwright install.
const {chromium} = await import("playwright").catch(() => createRequire(import.meta.url)("playwright"));
const base = process.argv[2] || "http://127.0.0.1:8771";
const shots = process.env.KERN_E2E_SHOTS || fs.mkdtempSync(path.join(os.tmpdir(), "kern-e2e-"));
let failures = 0, checks = 0;
const annotate = text => { if (process.env.GITHUB_ACTIONS) console.log("::error::" + String(text).replace(/\r?\n/g, " ").slice(0, 900)); };
const check = (ok, what) => { checks++; if (!ok) { failures++; console.log("FAIL", what); annotate("e2e: " + what); } else console.log("ok  ", what); };
process.on("uncaughtException", e => { annotate("e2e crashed: " + (e?.stack || e)); console.error(e); process.exit(1); });
process.on("unhandledRejection", e => { annotate("e2e crashed: " + (e?.stack || e)); console.error(e); process.exit(1); });

const browser = await chromium.launch({args: ["--use-gl=angle", "--use-angle=swiftshader", "--enable-unsafe-swiftshader", "--ignore-gpu-blocklist"]});

async function openEditor(url, viewport = {width: 1400, height: 860}, extra = {}) {
  const ctx = await browser.newContext({viewport, acceptDownloads: true, ...extra});
  const page = await ctx.newPage();
  const errors = [];
  page.on("pageerror", e => errors.push(e.message));
  page.on("console", m => { if (m.type() === "error") errors.push(m.text()); });
  await page.goto(url);
  await page.waitForFunction(() => window.KernEditor && window.KernEditor.views.length > 0, null, {timeout: 60000});
  await page.waitForTimeout(500);
  return {ctx, page, errors};
}

/** Polls `fn` (in Node) until it returns truthy or `ms` pass; returns the last value. */
async function until(fn, ms = 3000) {
  const end = Date.now() + ms;
  let v;
  while (!(v = await fn()) && Date.now() < end) await new Promise(r => setTimeout(r, 50));
  return v;
}

/** Fraction of non-background pixels inside a view (proves the renderer drew). */
async function viewInk(page) {
  // Read the canvas in a rAF callback queued after the editor's own frame, while
  // the drawing buffer is still valid.
  return page.evaluate(() => new Promise(resolve => requestAnimationFrame(() => {
    const c = document.getElementById("gl");
    const t = document.createElement("canvas");
    t.width = 160; t.height = 100;
    const g = t.getContext("2d");
    g.drawImage(c, 0, 0, t.width, t.height);
    const d = g.getImageData(0, 0, t.width, t.height).data;
    const colors = new Set();
    for (let i = 0; i < d.length; i += 4) colors.add((d[i] >> 4) << 8 | (d[i + 1] >> 4) << 4 | d[i + 2] >> 4);
    resolve(colors.size);
  })));
}

// ---- single-file editor ------------------------------------------------------
{
  const {ctx, page, errors} = await openEditor(base + "/generated/editor3d.html");
  const st = await page.evaluate(() => window.KernEditor.state);
  check(st.views.length === 1 && st.views[0].type === "editor", "editor starts with one editor view");
  check(st.tree.rows.length > 3, "Newton cradle demo is in the item tree (" + st.tree.rows.length + " rows)");
  check(await page.locator("#itemTree .tree-row").count() === st.tree.rows.length, "tree rendered into the DOM");
  check((await page.textContent("#counts")).includes("bodies"), "status bar counts");
  await page.waitForTimeout(300);
  await page.screenshot({path: path.join(shots, "editor.png")});
  check(await viewInk(page) > 20, "renderer draws the scene");

  // Menus.
  await page.click('[data-menu="File"]');
  check(await page.locator(".popup button").count() > 5, "File menu opens");
  await page.keyboard.press("Escape");
  check(await page.locator(".popup").count() === 0, "Escape closes the menu");

  // New scene + add body via the Actions menu (keyboard: Space).
  await page.click("#toolbar button[title^='New scene']");
  await page.waitForTimeout(100);
  let s = await page.evaluate(() => window.KernEditor.state);
  check(s.tabs.length === 2, "new scene opens a second tab");
  const view = s.views.find(v => v.focused).id;
  await page.locator(".view.focused .canvas-wrap").click({position: {x: 300, y: 200}});
  await page.keyboard.press("Space");
  check(await page.locator(".popup").count() === 1, "Space opens the action menu");
  await page.locator(".popup button", {hasText: "Add"}).first().click();
  await page.locator(".popup button", {hasText: "Body"}).first().click();
  await page.locator(".popup button", {hasText: "Dynamic"}).first().click().catch(() => {});
  await page.waitForTimeout(100);
  s = await page.evaluate(() => window.KernEditor.state);
  const bodies = s.tree.rows.filter(r => r.mode === "body").length;
  check(bodies >= 1, "action menu adds a body (" + bodies + ")");
  if (await page.locator(".popup").count()) await page.keyboard.press("Escape");

  // Transform by keyboard: G X 2 Enter.
  const before = await page.evaluate(() => window.KernEditor.state.properties);
  await page.locator(".view.focused .canvas-wrap").hover({position: {x: 300, y: 200}});
  for (const k of ["g", "x", "2", "Enter"]) await page.keyboard.press(k);
  await page.waitForTimeout(50);
  const after = await page.evaluate(() => window.KernEditor.state.properties);
  const px = p => p.fields.find(f => f.path === "position.0")?.value;
  check(before && after && Math.abs(Number(px(after)) - Number(px(before)) - 2) < 1e-6, `G X 2 Enter moves the body by 2 (x ${px(before)} -> ${px(after)})`);
  await page.keyboard.press("Control+z");
  const undone = await page.evaluate(() => window.KernEditor.state.properties);
  check(Number(px(undone)) === Number(px(before)), "Ctrl+Z undoes the move");

  // Property edit through the DOM.
  const mass = page.locator('#properties input[aria-label="Name"]');
  if (await mass.count()) {
    await mass.fill("Crate");
    await mass.press("Enter");
    await mass.dispatchEvent("change");
    await page.waitForTimeout(50);
    check((await page.evaluate(() => window.KernEditor.state.tree.rows.map(r => r.name))).includes("Crate"), "renaming via the properties panel");
  }

  // Dialog: keyboard reference.
  await page.locator(".view.focused .canvas-wrap").focus();
  await page.keyboard.press("F1");
  check(await page.locator(".dialog").count() === 1, "F1 opens the keyboard dialog");
  await page.keyboard.press("Escape");

  // Game demo, player, game controls.
  await page.evaluate(() => window.KernEditor.command({cmd: "loadDemo", kind: "game"}));
  await page.waitForTimeout(200);
  await page.evaluate(() => window.KernEditor.command({cmd: "openPlayer"}));
  await page.waitForTimeout(400);
  s = await page.evaluate(() => window.KernEditor.state);
  const player = s.views.find(v => v.type === "player");
  check(player && player.game, "player view runs the game");
  await page.waitForTimeout(800);
  let hud = await page.evaluate(() => window.KernEditor.status.views.find(v => v.hud)?.hud);
  check(hud && hud.status === "playing" && hud.pickupsTotal > 0, "game HUD: " + JSON.stringify(hud));
  check(await page.locator(".view.focused .hud").isVisible(), "HUD shown over the player view");
  const h0 = await page.evaluate(() => window.KernEditor.status.views.find(v => v.hud).hud);
  await page.keyboard.down("KeyW");
  await page.waitForTimeout(1200);
  await page.keyboard.up("KeyW");
  const h1 = await page.evaluate(() => window.KernEditor.status.views.find(v => v.hud).hud);
  check(h1.elapsed > h0.elapsed, "game clock advances (" + h0.elapsed + " -> " + h1.elapsed + ")");
  const moved = Math.hypot(h1.player[0] - h0.player[0], h1.player[1] - h0.player[1]);
  check(moved > 0.3, "W rolls the player ball (" + moved.toFixed(2) + " m)");
  await page.screenshot({path: path.join(shots, "player.png")});

  // Export game.
  await page.evaluate(() => window.KernEditor.command({cmd: "activateView", view: window.KernEditor.state.views.find(v => v.type === "editor" && v.title.includes("Roll")).id}));
  const [dl] = await Promise.all([page.waitForEvent("download"), page.evaluate(() => window.KernEditor.command({cmd: "exportGame"}))]);
  const gamePath = path.join(shots, dl.suggestedFilename());
  await dl.saveAs(gamePath);
  const size = fs.statSync(gamePath).size;
  check(size > 1e6, "exported game " + dl.suggestedFilename() + " (" + size + " bytes)");

  // Save scene.
  const [sv] = await Promise.all([page.waitForEvent("download"), page.keyboard.press("Control+s")]);
  const scenePath = path.join(shots, sv.suggestedFilename());
  await sv.saveAs(scenePath);
  const sceneJson = JSON.parse(fs.readFileSync(scenePath, "utf8"));
  check(sceneJson.format === "editor3d" || sceneJson.version || sceneJson.bodies, "Ctrl+S downloads the .e3d scene");
  // Served copy for the scenegame test below (repository root is the web root).
  const served = path.join(process.cwd(), "generated", "e2e-scene.e3d");
  if (fs.existsSync(path.dirname(served))) { fs.copyFileSync(scenePath, served); globalThis.__savedScene = "/generated/e2e-scene.e3d"; }

  // Re-open the saved scene through the file input.
  await page.setInputFiles("#sceneFile", scenePath);
  await page.waitForTimeout(300);
  s = await page.evaluate(() => window.KernEditor.state);
  check(s.tabs.some(t => t.title.startsWith(sceneJson.name || "")) && s.tabs.length >= 3, "opening the saved file adds a tab");

  // Autosave survives reload.
  await page.waitForTimeout(2500);
  const saved = await page.evaluate(() => localStorage.getItem("kern-editor.workspace.v1"));
  check(!!saved && JSON.parse(saved).length >= 1, "workspace autosaved to localStorage");
  check(errors.length === 0, "no page errors: " + errors.join(" | "));
  const logText = await page.textContent("#log");
  check(!/Render:|error/i.test(logText), "message log has no render errors");
  await ctx.close();

  // ---- exported game page ----------------------------------------------------
  const g = await browser.newContext({viewport: {width: 1000, height: 640}});
  const gp = await g.newPage();
  const gerr = [];
  gp.on("pageerror", e => gerr.push(e.message));
  await gp.goto("file://" + gamePath);
  await gp.waitForFunction(() => window.KernPlayer, null, {timeout: 60000});
  await gp.waitForTimeout(800);
  const gh = await gp.evaluate(() => window.KernPlayer.status.views[0].hud);
  check(gh && gh.status === "playing", "exported game runs standalone from file:// (" + JSON.stringify(gh) + ")");
  check((await gp.textContent("#hud")).includes("Score"), "exported game HUD");
  const g0 = await gp.evaluate(() => window.KernPlayer.status.views[0].hud.player);
  await gp.keyboard.down("KeyD");
  await gp.waitForTimeout(800);
  await gp.keyboard.up("KeyD");
  const g1 = await gp.evaluate(() => window.KernPlayer.status.views[0].hud.player);
  check(Math.hypot(g1[0] - g0[0], g1[1] - g0[1]) > 0.2, "D moves the ball in the exported game");
  await gp.evaluate(() => window.KernPlayer.command({cmd: "gameInput", view: window.KernPlayer.view, moveY: 1}));
  await gp.waitForTimeout(600);
  await gp.evaluate(() => window.KernPlayer.command({cmd: "gameInput", view: window.KernPlayer.view, moveY: 0}));
  const g2 = await gp.evaluate(() => window.KernPlayer.status.views[0].hud.player);
  check(Math.hypot(g2[0] - g1[0], g2[1] - g1[1]) > 0.1, "touch joystick input (gameInput) moves the ball");
  await gp.evaluate(() => window.KernPlayer.command({cmd: "restartGame", view: window.KernPlayer.view}));
  const gh2 = await gp.evaluate(() => window.KernPlayer.status.views[0].hud);
  check(gh2.elapsed < 0.5, "restart resets the game clock");
  await gp.screenshot({path: path.join(shots, "game.png")});
  check(gerr.length === 0, "no errors in the exported game: " + gerr.join(" | "));
  await g.close();
}

// ---- sweep: every menu entry, every dialog, images, grab ------------------------
{
  const {ctx, page, errors} = await openEditor(base + "/generated/editor3d.html");
  page.on("download", () => {});            // downloads from menu entries are fine
  await page.evaluate(() => { HTMLInputElement.prototype.click = function () {}; });  // no OS file pickers
  const sweep = await page.evaluate(() => {
    const E = window.KernEditor, bad = [], ran = [];
    const collect = (entries, out) => {
      for (const e of entries || []) {
        if (e === "-" || !e) continue;
        if (e.sub) collect(e.sub, out);
        else if (e.cmd) out.push({label: e.label, cmd: e.cmd});
      }
      return out;
    };
    const skip = new Set(["closeView"]);
    for (const demo of ["mechanism", "joints", "hulls", "game"]) {
      E.command({cmd: "loadDemo", kind: demo});
      for (const menu of ["File", "Edit", "View", "Scene", "Window", "Tools", "Help", "actions"]) {
        const entries = collect(E.command({cmd: "menu", name: menu}).entries, []);
        for (const {label, cmd} of entries) {
          if (skip.has(cmd.cmd)) continue;
          E.command({cmd: "selectAll"});
          let r;
          try { r = E.command(cmd); } catch (err) { bad.push(menu + "/" + label + ": threw " + err.message); continue; }
          ran.push(menu + "/" + label);
          if (!r) bad.push(menu + "/" + label + ": no result");
          else if (r.ok === false && /unknown/i.test(r.error || "")) bad.push(menu + "/" + label + ": " + r.error);
          if (E.state.operation) E.command({cmd: "finishOperation", accept: false});
          document.querySelector(".modal-back")?.remove();
          document.querySelector(".popup")?.remove();
        }
      }
    }
    return {bad, ran: ran.length};
  });
  check(sweep.bad.length === 0 && sweep.ran > 150, `all ${sweep.ran} menu entries run (${sweep.bad.slice(0, 5).join("; ")})`);

  // Every dialog opens and its default values submit cleanly.
  const dialogs = await page.evaluate(() => {
    const E = window.KernEditor, out = [];
    E.command({cmd: "loadDemo", kind: "joints"});
    E.command({cmd: "setMode", mode: "body"});
    E.command({cmd: "selectAll"});
    const id = E.state.tree.rows[0].id;
    for (const name of ["chain", "cursor", "saveAs", "options", "custom", "collision", "validation", "keyboard", "about", "rename"]) {
      if (name === "collision") { E.command({cmd: "setMode", mode: "fixture"}); E.command({cmd: "selectAll"}); }
      const r = E.command({cmd: "dialog", name, id});
      const d = r.ui?.find(a => a.action === "dialog")?.dialog;
      if (!d) { out.push(name + ": no dialog"); continue; }
      const values = {};
      for (const b of d.body || []) {
        if (b.type === "field") values[b.id] = b.value;
        if (b.type === "properties") values[b.id] = b.value || [];
        if (b.type === "bits") values[b.id] = {category: b.category, mask: b.mask};
      }
      const submit = (d.buttons || []).find(b => b.action === "submit");
      if (!submit) continue;
      const s = E.command({cmd: "dialogSubmit", name: d.name, values, button: submit.value || "", dialogView: d.view ?? 0, dialogId: d.id ?? ""});
      if (s?.ok === false) out.push(name + ": " + s.error);
      document.querySelector(".modal-back")?.remove();
    }
    return out;
  });
  check(dialogs.length === 0, "every dialog opens and submits (" + dialogs.join("; ") + ")");

  // The DOM dialog: Scene → Chain via the menu bar.
  await page.evaluate(() => window.KernEditor.command({cmd: "loadDemo", kind: "mechanism"}));
  await page.evaluate(() => window.KernEditor.command({cmd: "dialog", name: "options"}));
  await page.waitForTimeout(50);
  check(await page.locator(".dialog input, .dialog select").count() > 0, "options dialog rendered with inputs");
  await page.locator(".dialog-footer button").first().click();
  await page.waitForTimeout(50);
  check(await page.locator(".modal-back").count() === 0, "dialog closes after submit");

  // Every mode, every fixture shape and joint type through the API.
  const build = await page.evaluate(() => {
    const E = window.KernEditor, out = [];
    E.command({cmd: "newScene"});
    E.command({cmd: "addBody", type: 0, shape: "box"});
    E.command({cmd: "addBody", type: 2, shape: "sphere"});
    E.command({cmd: "setMode", mode: "body"});
    E.command({cmd: "selectAll"});
    for (const shape of ["box", "sphere", "capsule", "hull"]) {
      E.command({cmd: "setMode", mode: "body"});
      E.command({cmd: "select", ids: [E.state.tree.rows.find(r => r.mode === "body").id], mode: "body"});
      const r = E.command({cmd: "addFixture", shape});
      if (r.ok === false) out.push("fixture " + shape + ": " + r.error);
    }
    for (const type of ["revolute", "distance", "prismatic", "wheel", "rope", "weld", "friction", "motor", "spherical", "parallel", "filter"]) {
      E.command({cmd: "setMode", mode: "body"});
      E.command({cmd: "selectAll"});
      const r = E.command({cmd: "addJoint", type});
      if (r.ok === false) out.push("joint " + type + ": " + r.error);
    }
    for (const mode of ["body", "fixture", "vertex", "joint", "image", "sampler", "world"]) {
      const r = E.command({cmd: "setMode", mode});
      if (r.ok === false) out.push("mode " + mode);
      E.command({cmd: "selectAll"});
      const props = E.state.properties;
      if (!props) out.push("no properties in " + mode);
    }
    const st = E.state;
    return {out, joints: st.tree.rows.filter(r => r.mode === "joint").length, fixtures: st.tree.rows.filter(r => r.mode === "fixture").length};
  });
  check(build.out.length === 0 && build.fixtures >= 6 && build.joints >= 11, `all shapes and joint types (${build.fixtures} fixtures, ${build.joints} joints) ${build.out.join("; ")}`);

  // Image import (PNG with alpha) → texture → trace to fixtures.
  const png = await page.evaluate(async () => {
    const c = document.createElement("canvas");
    c.width = 64; c.height = 64;
    const g = c.getContext("2d");
    g.fillStyle = "#d33"; g.fillRect(8, 20, 48, 24);
    const blob = await new Promise(r => c.toBlob(r, "image/png"));
    return Array.from(new Uint8Array(await blob.arrayBuffer()));
  });
  const pngPath = path.join(shots, "shape.png");
  fs.writeFileSync(pngPath, Buffer.from(png));
  await page.evaluate(() => window.KernEditor.command({cmd: "newScene"}));
  await page.setInputFiles("#imageFile", pngPath);
  await page.waitForTimeout(800);
  const traced = await page.evaluate(() => {
    const E = window.KernEditor;
    E.command({cmd: "setMode", mode: "image"});
    E.command({cmd: "selectAll"});
    const r = E.command({cmd: "traceImage"});
    return {ok: r.ok, images: E.state.tree.rows.filter(x => x.mode === "image").length, fixtures: E.state.tree.rows.filter(x => x.mode === "fixture").length};
  });
  check(traced.images === 1 && traced.fixtures >= 1, "image import + trace to fixtures (" + JSON.stringify(traced) + ")");

  // Tiled views + clone.
  await page.evaluate(() => window.KernEditor.command({cmd: "cloneView"}));   // a clone tiles the views
  await page.waitForTimeout(200);
  check(await until(async () => (await page.locator(".view.visible").count()) >= 2), "clone view tiles the layout");
  check((await page.evaluate(() => window.KernEditor.state.views.length)) <= 31, "view count stays within the 31 renderer layers");
  await page.evaluate(() => window.KernEditor.command({cmd: "toggleTile"}));
  await page.waitForTimeout(100);
  check(await until(async () => (await page.locator(".view.visible").count()) === 1), "Ctrl+T layout toggle back to one view");

  // Player: grab a dynamic body with the mouse (Box3D MotorJoint).
  await page.evaluate(() => { window.KernEditor.command({cmd: "loadDemo", kind: "cradle"}); window.KernEditor.command({cmd: "openPlayer"}); });
  await page.waitForFunction(() => { const v = window.KernEditor.state.views.find(x => x.type === "player" && x.focused); return v && document.querySelector(`.view[data-view="${v.id}"] .canvas-wrap`); });
  await page.waitForTimeout(700);  // a few frames: view rect placed, camera fitted
  const grab = await page.evaluate(() => {
    const E = window.KernEditor;
    const v = E.state.views.find(x => x.type === "player" && x.focused).id;
    const r = document.querySelector(`.view[data-view="${v}"] .canvas-wrap`).getBoundingClientRect();
    // Scan for a dynamic body under the cursor, then drag it.
    for (let y = 0.3; y < 0.9; y += 0.04) for (let x = 0.25; x < 0.8; x += 0.02) {
      const px = r.width * x, py = r.height * y;
      E.pointer(v, "down", px, py, {button: 0, buttons: 1});
      const grabbing = E.status.views.find(s => s.id === v).grabbing;
      for (let i = 1; i <= 5; i++) E.pointer(v, "move", px - i * 15, py, {buttons: 1});
      E.pointer(v, "up", px - 75, py, {button: 0});
      if (grabbing) return {ok: true, at: [x.toFixed(2), y.toFixed(2)], released: !E.status.views.find(s => s.id === v).grabbing};
    }
    return {ok: false};
  });
  check(grab.ok && grab.released, "left-drag grabs a dynamic body in the player and releases it (" + JSON.stringify(grab) + ")");
  check(errors.length === 0, "no page errors during the sweep: " + errors.join(" | "));
  await page.screenshot({path: path.join(shots, "sweep.png")});
  await ctx.close();
}

// ---- module build, mobile ----------------------------------------------------
{
  const {ctx, page, errors} = await openEditor(base + "/kern/web/editor/index.html", {width: 390, height: 780}, {hasTouch: true, isMobile: true});
  const s = await page.evaluate(() => window.KernEditor.state);
  check(s.views.length >= 1, "module build starts on a phone viewport");
  check(await page.locator(".e3d-mobile-orbit-toggle:visible").count() === 1, "mobile orbit toggle visible");
  await page.locator(".e3d-mobile-orbit-toggle:visible").tap();
  await page.waitForTimeout(50);
  const t = await page.evaluate(() => window.KernEditor.state.views[0].touchOrbit);
  check(t === false, "orbit toggle switches to select");
  await page.screenshot({path: path.join(shots, "mobile.png")});
  check(errors.length === 0, "no page errors (module build): " + errors.join(" | "));
  await ctx.close();
}

// ---- player page with ?scene -------------------------------------------------
{
  const ctx = await browser.newContext({viewport: {width: 900, height: 600}});
  const page = await ctx.newPage();
  const errs = [];
  page.on("pageerror", e => errs.push(e.message));
  await page.goto(base + "/kern/web/editor/player.html");
  await page.waitForFunction(() => window.KernPlayer, null, {timeout: 60000});
  await page.waitForTimeout(500);
  const hud = await page.evaluate(() => window.KernPlayer.status.views[0].hud);
  check(hud && hud.status === "playing", "player.html plays the example game");
  check(errs.length === 0, "no errors in player.html: " + errs.join(" | "));
  await ctx.close();
}

// ---- C++ App example playing an .e3d scene --------------------------------------
{
  const ctx = await browser.newContext({viewport: {width: 900, height: 600}});
  const page = await ctx.newPage();
  const errs = [];
  page.on("pageerror", e => errs.push(e.message));
  await page.goto(base + "/kern/examples/scenegame/index.html");
  await page.waitForFunction(() => window.__kernApp || window.__kernError, null, {timeout: 60000});
  const status0 = await until(async () => { const t = await page.textContent("#status"); return /score/.test(t) && t; });
  check(!!status0, "scenegame (C++ App) runs the built-in level: " + status0);
  await page.keyboard.down("KeyW");
  await page.waitForTimeout(1500);
  await page.keyboard.up("KeyW");
  const status1 = await page.textContent("#status");
  check(/score/.test(status1) && status1 !== status0, "scenegame advances: " + status1);
  // The same game plays a scene saved by the editor (?file=).
  if (globalThis.__savedScene) {
    await page.goto(base + "/kern/examples/scenegame/index.html?file=" + encodeURIComponent(globalThis.__savedScene));
    const st = await until(async () => { const t = await page.textContent("#status"); return /Roll & Collect/.test(t) && t; }, 20000);
    check(!!st, "scenegame plays the editor's Roll & Collect scene: " + st);
  }
  await page.screenshot({path: path.join(shots, "scenegame.png")});
  check(errs.length === 0, "no errors in scenegame: " + errs.join(" | "));
  await ctx.close();
}

await browser.close();
console.log(`\n${checks - failures}/${checks} checks passed · screenshots in ${shots}`);
process.exit(failures ? 1 : 0);
