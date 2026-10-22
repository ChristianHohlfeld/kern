// Kern editor page: the DOM side of the C++ editor (kern/src/editor).
//
// This file contains no editing logic. It instantiates the engine module,
// draws the panels from the JSON the engine returns (kern_editor_state /
// kern_editor_status), forwards pointer and keyboard input, and performs the
// platform jobs only a browser can do: file pickers, downloads, clipboard,
// decoding images, local storage and the export of standalone games.
import {loadKern} from "../kern.mjs";
import {EditorEngine, POINTER, buildGameHtml, toBase64} from "./player.mjs";

const $ = s => document.querySelector(s), $$ = s => [...document.querySelectorAll(s)];
const esc = s => String(s ?? "").replace(/[&<>"']/g, c => ({"&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;"}[c]));
const AUTOSAVE_KEY = "kern-editor.workspace.v1", LEGACY_AUTOSAVE_KEY = "editor3d.workspace.v2";
const MAX_IMAGE_SIDE = 2048;

/**
 * Module sources needed to export standalone games. The single-file build
 * embeds them (window.KERN_SOURCES); the development page fetches them.
 */
async function moduleSources() {
  if (window.KERN_SOURCES) return window.KERN_SOURCES;
  const get = async rel => (await fetch(new URL(rel, import.meta.url))).text();
  return (window.KERN_SOURCES = {kern: await get("../kern.mjs"), player: await get("./player.mjs")});
}

export async function startEditor({url = null, base64 = null, bytes = null} = {}) {
  if (base64 == null && bytes == null && url) bytes = new Uint8Array(await (await fetch(url)).arrayBuffer());
  const kern = await loadKern({bytes, base64});
  const wasmBase64 = base64 ?? toBase64(bytes);
  const engine = new EditorEngine(kern);
  const ui = new EditorPage(kern, engine, wasmBase64);
  ui.start();
  window.KernEditor = ui.api();
  return ui;
}

class EditorPage {
  constructor(kern, engine, wasmBase64) {
    this.kern = kern;
    this.engine = engine;
    this.wasmBase64 = wasmBase64;
    this.canvas = $("#gl");
    this.views = new Map();   // view id -> DOM record
    this.state = null;
    this.revision = -1;
    this.logShown = 0;
    this.menu = null;
    this.lastPointer = {x: 120, y: 120};
    this.lastImagePoll = 0;
    this.lastSavePoll = 0;
    this.mobile = matchMedia("(pointer:coarse)").matches || innerWidth < 900;
  }

  // ---- startup -------------------------------------------------------------
  start() {
    const gl = this.canvas.getContext("webgl2", {antialias: true, alpha: false, preserveDrawingBuffer: false, powerPreference: "high-performance"});
    if (!gl) throw Error("WebGL2 is not available in this browser");
    this.gl = gl;
    this.kern.attachContext(this.canvas, gl);
    this.resizeCanvas();
    if (!this.engine.e.kern_editor_init(this.canvas.width, this.canvas.height, 0)) throw Error(this.kern.lastError());
    this.engine.e.kern_editor_set_mobile(this.mobile ? 1 : 0);
    this.canvas.addEventListener("webglcontextlost", e => { e.preventDefault(); this.engine.e.kern_editor_context_lost(); this.kern.resetTables(); });
    this.canvas.addEventListener("webglcontextrestored", () => {
      this.kern.attachContext(this.canvas, gl);
      if (!this.engine.e.kern_editor_context_restored()) this.report("WebGL context could not be restored: " + this.kern.lastError(), "error");
    });
    this.initToolbar();
    this.initEvents();
    this.engine.command({cmd: "report", message: "Kern editor ready · C++ engine with Box3D, WebGL2 renderer"});
    if (!this.restoreWorkspace()) this.engine.command({cmd: "loadDemo", kind: "cradle"});
    requestAnimationFrame(t => this.frame(t));
  }

  restoreWorkspace() {
    for (const key of [AUTOSAVE_KEY, LEGACY_AUTOSAVE_KEY]) {
      let text = null;
      try { text = localStorage.getItem(key); } catch { /* storage unavailable */ }
      if (!text) continue;
      const r = this.engine.command({cmd: "restoreWorkspace", text});
      if (r?.ok) return true;
    }
    return false;
  }

  api() {
    const self = this;
    return {
      command: c => self.run(c),
      get state() { return self.engine.state(); },
      get status() { return self.engine.status(); },
      key: (key, code, opts = {}) => self.handleKeyResult(self.engine.key(key, code, opts.down !== false, (opts.shift ? 1 : 0) | (opts.ctrl ? 2 : 0), false)),
      pointer: (view, type, x, y, opts = {}) => self.engine.pointer(view, POINTER[type], x, y, {...opts, mods: (opts.shift ? 1 : 0) | (opts.ctrl ? 2 : 0)}),
      exportGameHtml: (scene, title) => self.gameHtml(scene, title),
      get views() { return [...self.views.keys()]; },
    };
  }

  // ---- the frame loop ------------------------------------------------------
  frame(time) {
    try {
      this.resizeCanvas();
      this.placeViews();
      this.engine.e.kern_editor_frame(time);
      const rev = this.engine.e.kern_editor_ui_revision();
      if (rev !== this.revision) {
        this.revision = rev;
        this.state = this.engine.state();
        this.render(this.state);
        this.runUi(this.state.ui);
      }
      this.renderStatus(this.engine.status());
      if (time - this.lastImagePoll > 200) { this.lastImagePoll = time; this.pollImages(); }
      if (time - this.lastSavePoll > 400) { this.lastSavePoll = time; this.autosave(time); }
    } catch (error) {
      console.error(error);
      this.showNotice("Editor error: " + error.message);
    }
    requestAnimationFrame(t => this.frame(t));
  }

  resizeCanvas() {
    const dpr = Math.min(devicePixelRatio || 1, this.mobile ? 1.5 : 2);
    const w = Math.max(1, Math.round(this.canvas.clientWidth * dpr)), h = Math.max(1, Math.round(this.canvas.clientHeight * dpr));
    if (this.canvas.width !== w || this.canvas.height !== h) {
      this.canvas.width = w;
      this.canvas.height = h;
      this.engine.e.kern_editor_resize?.(w, h);
    }
    this.dpr = dpr;
  }

  placeViews() {
    const c = this.canvas.getBoundingClientRect(), dpr = this.dpr;
    for (const [id, rec] of this.views) {
      const r = rec.wrap.getBoundingClientRect();
      const visible = rec.el.classList.contains("visible") && r.width > 1 && r.height > 1;
      const x = Math.round((r.left - c.left) * dpr), w = Math.round(r.width * dpr), h = Math.round(r.height * dpr);
      const y = Math.round((c.bottom - r.bottom) * dpr);
      const key = `${x},${y},${w},${h},${visible},${r.width},${r.height}`;
      if (key === rec.rectKey) continue;
      rec.rectKey = key;
      const cx = Math.max(0, x), cy = Math.max(0, y);
      const cw = Math.min(this.canvas.width - cx, w - (cx - x)), ch = Math.min(this.canvas.height - cy, h - (cy - y));
      this.engine.e.kern_editor_set_view_rect(id, cx, cy, Math.max(1, cw), Math.max(1, ch), r.width, r.height, dpr, visible && cw > 0 && ch > 0 ? 1 : 0);
    }
  }

  // ---- commands and UI actions ----------------------------------------------
  run(command) {
    const r = this.engine.command(command);
    if (!r) return r;
    if (r.ok === false && r.error) this.report(r.error, "error");
    this.runUi(r.ui);
    return r;
  }

  runUi(actions) {
    for (const a of actions || []) {
      switch (a.action) {
        case "openFile": (a.accept === "image" ? $("#imageFile") : $("#sceneFile")).click(); break;
        case "download": this.download(a.name, new Blob([a.text], {type: a.type || "application/json"})); break;
        case "downloadPixels": this.downloadPixels(a.name, a.width, a.height); break;
        case "dialog": this.dialog(a.dialog); break;
        case "popup": {
          const at = a.at === "pointer" ? this.pointerScreen() : (a.x != null ? {x: a.x, y: a.y} : this.pointerScreen());
          this.openPopup(a.entries || this.engine.command({cmd: "menu", name: a.menu}).entries, at.x, at.y);
          break;
        }
        case "clipboard": navigator.clipboard?.writeText(a.text).catch(() => {}); break;
        case "togglePanels": document.body.classList.toggle("panels-hidden"); break;
        case "togglePanel": { const p = $("#" + a.id); if (p) p.hidden = !p.hidden; break; }
        case "resetPanels": this.resetPanels(); break;
        case "openUrl": window.open(a.url, "_blank", "noopener"); break;
        case "closeMenu": this.closeMenu(); break;
        case "exportView": this.exportView(a.view); break;
        case "exportGame": this.gameHtml(a.scene, a.title).then(html => this.download(a.name, new Blob([html], {type: "text/html"}))).catch(e => this.report("Game export failed: " + e.message, "error")); break;
        case "focusCanvas": this.focusActiveView(); break;
        case "showLog": $("#logPanel").hidden = false; break;
        default: break;
      }
    }
  }

  report(message, type = "info") { this.engine.command({cmd: "report", message, type}); }
  showNotice(text) { const n = $("#engineNotice"); n.style.display = "block"; n.textContent = text; }

  pointerScreen() {
    const active = this.state?.views?.find(v => v.focused);
    const rec = active && this.views.get(active.id);
    if (!rec) return {x: 120, y: 120};
    const r = rec.wrap.getBoundingClientRect();
    return {x: r.left + this.lastPointer.x, y: r.top + this.lastPointer.y};
  }

  focusActiveView() {
    const active = this.state?.views?.find(v => v.focused);
    this.views.get(active?.id)?.wrap.focus({preventScroll: true});
  }

  // ---- panels ---------------------------------------------------------------
  render(s) {
    this.renderTabsAndViews(s);
    this.renderTree(s.tree);
    this.renderProperties(s.properties);
    this.renderHistory(s.history);
    $("#contextHelp").innerHTML = s.context || "";
    $("#counts").textContent = s.counts || "";
    $$("[data-mode]").forEach(b => b.classList.toggle("on", b.dataset.mode === s.mode));
    if (s.logCount < this.logShown) { $("#log").replaceChildren(); this.logShown = 0; }
    this.pullLog(s.logCount);
  }

  pullLog(count) {
    if (count <= this.logShown) return;
    const lines = this.engine.result(this.engine.e.kern_editor_log(this.logShown)) || [];
    this.logShown = count;
    const log = $("#log");
    for (const line of lines) {
      const row = document.createElement("div");
      row.className = "log-line " + (line.type || "info");
      row.textContent = new Date().toLocaleTimeString() + "  " + line.message;
      log.append(row);
      if (line.type === "error") $("#logPanel").hidden = false;
    }
    while (log.children.length > 250) log.firstChild.remove();
    log.scrollTop = log.scrollHeight;
  }

  renderTabsAndViews(s) {
    const tabs = $("#tabs");
    tabs.replaceChildren();
    for (const t of s.tabs || []) {
      const el = document.createElement("div");
      el.className = "tab" + (t.active ? " active" : "");
      el.setAttribute("role", "tab");
      el.setAttribute("aria-selected", String(t.active));
      el.innerHTML = `<span>${esc(t.title)}</span><button aria-label="Close ${esc(t.title)}">×</button>`;
      el.onclick = () => this.run({cmd: "activateView", view: t.view});
      el.querySelector("button").onclick = e => { e.stopPropagation(); this.run({cmd: "closeView", view: t.view}); };
      tabs.append(el);
    }
    const seen = new Set();
    for (const v of s.views || []) {
      seen.add(v.id);
      let rec = this.views.get(v.id);
      if (!rec) rec = this.createViewElement(v.id);
      rec.el.classList.toggle("visible", v.visible);
      rec.el.classList.toggle("focused", v.focused);
      rec.el.querySelector(".viewtitle span").textContent = v.title;
      rec.data = v;
      this.renderViewbar(rec, v);
      rec.toggle.hidden = !this.mobile;
      if (this.mobile) {
        rec.toggle.dataset.mode = v.touchOrbit ? "orbit" : "interact";
        rec.toggle.querySelector(".ico").textContent = v.touchOrbit ? "⟳" : "⌖";
        rec.toggle.querySelector(".txt").textContent = v.touchOrbit ? "Orbit" : (v.type === "player" ? "Grab" : "Select");
      }
    }
    for (const [id, rec] of this.views)
      if (!seen.has(id)) { rec.el.remove(); this.views.delete(id); }
    $("#views").classList.toggle("tiled", !!s.tiled);
  }

  createViewElement(id) {
    const el = document.createElement("div");
    el.className = "view";
    el.dataset.view = id;
    el.innerHTML = '<div class="viewtitle"><span></span><button title="Close view">×</button></div>' +
      '<div class="canvas-wrap" tabindex="0" aria-label="3D physics scene view"><div class="viewbar"></div><div class="labels"></div>' +
      '<div class="hud" hidden></div><div class="hud-message" hidden></div><div class="viewhint"></div><div class="selectbox" hidden></div>' +
      '<button type="button" class="e3d-mobile-orbit-toggle" hidden><span class="ico">⟳</span><span class="txt">Orbit</span></button></div>';
    $("#views").append(el);
    const rec = {
      id, el, wrap: el.querySelector(".canvas-wrap"), bar: el.querySelector(".viewbar"), hint: el.querySelector(".viewhint"),
      labels: el.querySelector(".labels"), hud: el.querySelector(".hud"), message: el.querySelector(".hud-message"),
      box: el.querySelector(".selectbox"), toggle: el.querySelector(".e3d-mobile-orbit-toggle"), barKey: "", statusKey: "",
    };
    el.querySelector(".viewtitle button").onclick = () => this.run({cmd: "closeView", view: id});
    rec.toggle.addEventListener("click", e => {
      e.preventDefault();
      e.stopPropagation();
      this.run({cmd: "setTouchOrbit", view: id, on: !rec.data?.touchOrbit});
    });
    rec.toggle.addEventListener("pointerdown", e => e.stopPropagation());
    this.bindView(rec);
    this.views.set(id, rec);
    return rec;
  }

  renderViewbar(rec, v) {
    const key = `${v.type}|${v.playing}|${v.speed}`;
    if (key === rec.barKey) return;
    rec.barKey = key;
    const bar = rec.bar;
    if (v.type === "editor") {
      bar.innerHTML = '<button data-run title="Open player view (Ctrl+R)">⇥ Run</button>';
      bar.querySelector("button").onclick = () => { this.run({cmd: "activateView", view: v.id}); this.run({cmd: "openPlayer"}); };
    } else {
      bar.innerHTML = `<button data-play>${v.playing ? "Ⅱ Pause" : "▶ Play"}</button><button data-reload>↻ Reload</button><button data-step>▸|</button>` +
        `<span>Speed:</span><select aria-label="Simulation speed">${[0.1, 0.25, 0.5, 1, 2, 4].map(n => `<option ${n === v.speed ? "selected" : ""}>${n}</option>`).join("")}</select>`;
      bar.querySelector("[data-play]").onclick = () => this.run({cmd: "togglePlay", view: v.id});
      bar.querySelector("[data-reload]").onclick = () => this.run({cmd: "reloadPlayer", view: v.id});
      bar.querySelector("[data-step]").onclick = () => this.run({cmd: "stepPlayer", view: v.id});
      bar.querySelector("select").onchange = e => this.run({cmd: "setSpeed", view: v.id, speed: Number(e.target.value)});
    }
    for (const b of bar.querySelectorAll("button,select")) b.addEventListener("pointerdown", e => e.stopPropagation());
  }

  renderTree(tree) {
    const root = $("#itemTree");
    root.replaceChildren();
    if (!tree?.rows?.length) { root.innerHTML = `<div class="empty">${esc(tree?.empty || "No items.")}</div>`; return; }
    for (const row of tree.rows) {
      const r = document.createElement("div");
      r.className = "tree-row" + (row.indent ? " fixture" : "") + (row.selected ? " selected" : "");
      const disclose = row.disclose === "none" ? '<span style="width:12px"></span>' : `<button class="disclose">${row.disclose === "closed" ? "▸" : "▾"}</button>`;
      r.innerHTML = `${disclose}<input type="checkbox" ${row.selected ? "checked" : ""} aria-label="Select ${esc(row.name)}"><span class="dot ${esc(row.mode)}"></span><span class="name">${esc(row.name)}</span>`;
      r.querySelector(".disclose")?.addEventListener("click", e => { e.stopPropagation(); this.run({cmd: "treeToggle", id: row.id}); });
      r.onclick = e => this.run({cmd: "select", ids: [row.id], mode: row.mode, shift: e.shiftKey, toggle: e.target.type === "checkbox" || e.ctrlKey || e.metaKey});
      r.ondblclick = () => this.run({cmd: "dialog", name: "rename", id: row.id});
      root.append(r);
    }
  }

  renderProperties(p) {
    const root = $("#properties");
    if (!p) { root.replaceChildren(); return; }
    if (p.empty) { root.innerHTML = `<div class="empty">${p.empty}</div>`; return; }
    root.innerHTML = `<div class="proptitle">${esc(p.title)}</div><table class="props"><thead><tr><th>Property</th><th>Value</th></tr></thead><tbody></tbody></table>`;
    const tbody = root.querySelector("tbody");
    for (const f of p.fields || []) {
      const tr = document.createElement("tr");
      if (f.group) { tr.className = "group"; tr.innerHTML = `<td colspan="2">▾ ${esc(f.group)}</td>`; tbody.append(tr); continue; }
      const a = document.createElement("td"), c = document.createElement("td");
      a.textContent = f.label;
      tr.append(a, c);
      let input;
      if (f.type === "select") {
        input = document.createElement("select");
        for (const [value, label] of f.choices || []) {
          const o = document.createElement("option");
          o.value = String(value);
          o.textContent = label;
          o.dataset.number = typeof value === "number" ? "1" : "";
          input.append(o);
        }
        if (f.mixed) {
          const o = document.createElement("option");
          o.value = "__mixed";
          o.textContent = "— multiple —";
          input.prepend(o);
          input.value = "__mixed";
        } else input.value = String(f.value ?? "");
      } else {
        input = document.createElement("input");
        input.type = f.type;
        if (f.step != null) input.step = f.step;
        if (f.min != null) input.min = f.min;
        if (f.max != null) input.max = f.max;
        if (f.type === "number" && f.step == null) input.step = "any";
        if (f.type === "checkbox") { input.checked = !!f.value; input.indeterminate = !!f.mixed; }
        else { input.value = f.mixed ? "" : (f.value ?? ""); input.placeholder = f.mixed ? "— multiple —" : ""; }
      }
      input.setAttribute("aria-label", f.label);
      input.onchange = () => {
        let value = f.type === "checkbox" ? input.checked : input.value;
        if (value === "__mixed") return;
        if (f.type === "select") {
          const opt = input.selectedOptions[0];
          if (opt?.dataset.number) value = Number(value);
        }
        this.run({cmd: "setProperty", path: f.path, value});
      };
      c.append(input);
      tbody.append(tr);
    }
    const actions = document.createElement("div");
    actions.className = "prop-actions";
    for (const a of p.actions || []) {
      const b = document.createElement("button");
      b.className = "plain";
      b.textContent = a.label;
      b.onclick = () => this.run(a.cmd);
      actions.append(b);
    }
    root.append(actions);
  }

  renderHistory(h) {
    const root = $("#undoList");
    root.replaceChildren();
    if (!h) return;
    const z = document.createElement("div");
    z.className = "undo-row" + (h.index === 0 ? " current" : "");
    z.textContent = "Initial state";
    z.onclick = () => this.run({cmd: "historyJump", index: -1});
    root.append(z);
    h.entries.forEach((label, i) => {
      const el = document.createElement("div");
      el.className = "undo-row" + (h.index === i + 1 ? " current" : h.index < i + 1 ? " future" : "");
      el.textContent = label;
      el.onclick = () => this.run({cmd: "historyJump", index: i});
      root.append(el);
    });
    root.querySelector(".current")?.scrollIntoView({block: "nearest"});
  }

  renderStatus(st) {
    if (!st) return;
    const set = (sel, text) => { const el = $(sel); if (el.textContent !== text) el.textContent = text; };
    set("#modeStatus", st.mode);
    set("#coordStatus", st.coords);
    set("#statusMessage", st.message);
    if (st.zoom) set("#zoomStatus", st.zoom);
    if (st.logCount > this.logShown) this.pullLog(st.logCount);
    for (const v of st.views || []) {
      const rec = this.views.get(v.id);
      if (!rec) continue;
      if (rec.hint.textContent !== v.hint) rec.hint.textContent = v.hint;
      if (v.box) {
        rec.box.hidden = false;
        Object.assign(rec.box.style, {left: v.box.x + "px", top: v.box.y + "px", width: v.box.w + "px", height: v.box.h + "px"});
      } else if (!rec.box.hidden) rec.box.hidden = true;
      const labelsKey = JSON.stringify(v.labels || null);
      if (labelsKey !== rec.labelsKey) {
        rec.labelsKey = labelsKey;
        rec.labels.replaceChildren(...(v.labels || []).map(l => {
          const s = document.createElement("span");
          s.textContent = l.text;
          s.style.left = l.x + "px";
          s.style.top = l.y + "px";
          return s;
        }));
      }
      this.renderHud(rec, v.hud);
    }
  }

  renderHud(rec, hud) {
    const key = JSON.stringify(hud || null);
    if (key === rec.hudKey) return;
    rec.hudKey = key;
    if (!hud) { rec.hud.hidden = true; rec.message.hidden = true; return; }
    rec.hud.hidden = false;
    const parts = [];
    if (hud.title) parts.push(`<b>${esc(hud.title)}</b>`);
    parts.push(`Score ${hud.score}`);
    if (hud.pickupsTotal) parts.push(`${hud.pickups}/${hud.pickupsTotal} collected`);
    parts.push(hud.remaining != null ? `${hud.remaining.toFixed(1)} s left` : `${hud.elapsed.toFixed(1)} s`);
    rec.hud.innerHTML = parts.join(" · ");
    if (hud.status === "won" || hud.status === "lost") {
      rec.message.hidden = false;
      rec.message.innerHTML = `${esc(hud.message || (hud.status === "won" ? "You win!" : "Game over"))}<small>Press R to restart</small>`;
    } else rec.message.hidden = true;
  }

  // ---- menus ----------------------------------------------------------------
  closeMenu() { this.menu?.remove(); this.menu = null; }

  openPopup(entries, x = 100, y = 100, parent = null) {
    this.closeMenu();
    const menu = document.createElement("div");
    menu.className = "popup";
    menu.setAttribute("role", "menu");
    if (parent) {
      const b = document.createElement("button");
      b.className = "back";
      b.textContent = "‹ Back";
      b.onclick = () => this.openPopup(parent.entries, parent.x, parent.y, parent.parent);
      menu.append(b);
    }
    for (const e of entries || []) {
      if (e === "-") { menu.append(document.createElement("hr")); continue; }
      const b = document.createElement("button");
      b.disabled = !!e.disabled;
      let label = e.label;
      if (e.panel) label = ($("#" + e.panel)?.hidden ? "   " : "✓ ") + label;
      b.innerHTML = `<span>${esc(label)}</span><kbd>${esc(e.sub ? "▶" : e.key || "")}</kbd>`;
      b.onclick = () => {
        if (e.sub) { this.openPopup(e.sub, x, y, {entries, x, y, parent}); return; }
        this.closeMenu();
        if (e.cmd) this.run(e.cmd);
      };
      menu.append(b);
    }
    document.body.append(menu);
    this.menu = menu;
    const r = menu.getBoundingClientRect();
    menu.style.left = Math.max(2, Math.min(x, innerWidth - r.width - 3)) + "px";
    menu.style.top = Math.max(2, Math.min(y, innerHeight - r.height - 3)) + "px";
    menu.querySelector("button:not(:disabled)")?.focus();
  }

  // ---- dialogs --------------------------------------------------------------
  dialog(spec) {
    if (!spec) return;
    this.closeMenu();
    $(".modal-back")?.remove();
    const back = document.createElement("div");
    back.className = "modal-back";
    back.innerHTML = `<section class="dialog" role="dialog" aria-modal="true" aria-label="${esc(spec.title)}"><div class="dialog-title"><span>${esc(spec.title)}</span><button aria-label="Close">×</button></div><div class="dialog-body"></div><div class="dialog-footer"></div></section>`;
    document.body.append(back);
    const body = back.querySelector(".dialog-body"), foot = back.querySelector(".dialog-footer");
    const collectors = [];
    for (const b of spec.body || []) body.append(this.dialogBlock(b, collectors));
    const errorbox = document.createElement("div");
    errorbox.className = "errorbox";
    body.append(errorbox);
    back.querySelector(".dialog-title button").onclick = () => back.remove();
    for (const btn of spec.buttons || []) {
      const b = document.createElement("button");
      b.className = "plain";
      b.textContent = btn.label;
      b.onclick = () => {
        try {
          if (btn.action === "close") { back.remove(); return; }
          if (btn.action === "cmd") { back.remove(); this.run(btn.cmd); return; }
          const values = {};
          for (const c of collectors) values[c.id] = c.read();
          const r = this.engine.command({cmd: "dialogSubmit", name: spec.name, values, button: btn.value || "", dialogView: spec.view ?? 0, dialogId: spec.id ?? ""});
          if (r?.ok === false) { errorbox.textContent = r.error || "Invalid input"; return; }
          back.remove();
          this.runUi(r?.ui);
        } catch (error) {
          errorbox.textContent = error.message;
        }
      };
      foot.append(b);
    }
    setTimeout(() => back.querySelector("input,select,textarea,.dialog-footer button")?.focus(), 0);
  }

  dialogBlock(b, collectors) {
    const wrap = document.createElement("div");
    if (b.type === "p") { wrap.innerHTML = `<p>${b.html}</p>`; return wrap.firstChild; }
    if (b.type === "list") { wrap.innerHTML = `<ul>${(b.items || []).map(i => `<li>${esc(i)}</li>`).join("")}</ul>`; return wrap.firstChild; }
    if (b.type === "table") {
      wrap.innerHTML = `<table>${(b.rows || []).map(r => `<tr>${r.map((c, i) => `<td class="${i === 0 && b.keyColumn ? "key" : ""}">${esc(c)}</td>`).join("")}</tr>`).join("")}</table>`;
      return wrap.firstChild;
    }
    if (b.type === "field") {
      const label = document.createElement("label");
      label.append(document.createTextNode(b.label + " "));
      let input;
      if (b.input === "select") {
        input = document.createElement("select");
        for (const [v, l] of b.options || []) { const o = document.createElement("option"); o.value = v; o.textContent = l; input.append(o); }
        input.value = b.value;
      } else {
        input = document.createElement("input");
        input.type = b.input;
        if (b.input === "checkbox") input.checked = !!b.value;
        else input.value = b.value ?? "";
        if (b.min != null) input.min = b.min;
        if (b.max != null) input.max = b.max;
        if (b.input === "number") input.step = "any";
      }
      input.id = "dlg_" + b.id;
      label.append(input);
      collectors.push({id: b.id, read: () => (b.input === "checkbox" ? input.checked : input.value)});
      return label;
    }
    if (b.type === "properties") {
      const types = ["int", "float", "string", "vec3", "bool", "color"];
      let rows = (b.value || []).map(r => ({...r}));
      const box = document.createElement("div");
      box.className = "props-editor";
      const draw = () => {
        box.innerHTML = `<table><thead><tr><th>Name</th><th>Type</th><th>Value</th><th></th></tr></thead><tbody>${rows.map((p, i) =>
          `<tr><td><input data-n="${i}" value="${esc(p.name)}"></td><td><select data-t="${i}">${types.map(t => `<option ${t === p.type ? "selected" : ""}>${t}</option>`).join("")}</select></td><td><input data-v="${i}" value="${esc(p.value)}"></td><td><button data-x="${i}" aria-label="Remove">×</button></td></tr>`).join("")}</tbody></table><button class="plain" data-add>+ Add property</button>`;
        box.querySelectorAll("[data-n]").forEach(el => el.oninput = () => rows[+el.dataset.n].name = el.value);
        box.querySelectorAll("[data-t]").forEach(el => el.onchange = () => rows[+el.dataset.t].type = el.value);
        box.querySelectorAll("[data-v]").forEach(el => el.oninput = () => rows[+el.dataset.v].value = el.value);
        box.querySelectorAll("[data-x]").forEach(el => el.onclick = () => { rows.splice(+el.dataset.x, 1); draw(); });
        box.querySelector("[data-add]").onclick = () => { rows.push({name: "property", type: "string", value: ""}); draw(); };
      };
      draw();
      collectors.push({id: b.id, read: () => rows});
      return box;
    }
    if (b.type === "bits") {
      const box = document.createElement("div");
      box.className = "bits";
      let rowsHtml = "";
      for (let i = 0; i < 32; i++) {
        const bit = 2 ** i;
        rowsHtml += `<tr><td>${i}</td><td><input type="checkbox" data-bit="${i}" data-field="category" ${((b.category >>> 0) & bit) ? "checked" : ""} aria-label="Category bit ${i}"></td>` +
          `<td><input type="checkbox" data-bit="${i}" data-field="mask" ${((b.mask >>> 0) & bit) ? "checked" : ""} aria-label="Mask bit ${i}"></td></tr>`;
      }
      box.innerHTML = `<table><tr><th>Bit</th><th>Category</th><th>Mask</th></tr>${rowsHtml}</table>`;
      collectors.push({id: b.id, read: () => {
        const out = {category: 0, mask: 0};
        box.querySelectorAll("[data-field]").forEach(el => { if (el.checked) out[el.dataset.field] = (out[el.dataset.field] + 2 ** (+el.dataset.bit)) >>> 0; });
        return out;
      }});
      return box;
    }
    return wrap;
  }

  // ---- input ----------------------------------------------------------------
  bindView(rec) {
    const w = rec.wrap;
    const mods = e => (e.shiftKey ? 1 : 0) | (e.ctrlKey || e.metaKey ? 2 : 0) | (e.altKey ? 4 : 0);
    const kind = e => (e.pointerType === "touch" ? 1 : e.pointerType === "pen" ? 2 : 0);
    const send = (type, e) => {
      const r = w.getBoundingClientRect();
      const x = e.clientX - r.left, y = e.clientY - r.top;
      if (type !== POINTER.wheel) this.lastPointer = {x, y};
      return this.engine.result(this.engine.e.kern_editor_pointer(rec.id, type, x, y, e.button ?? 0, e.buttons ?? 0, e.pointerId ?? 1, kind(e), mods(e), e.deltaY ?? 0));
    };
    w.addEventListener("pointerdown", e => {
      if (e.target.closest(".viewbar,.e3d-mobile-orbit-toggle")) return;
      w.focus({preventScroll: true});
      this.closeMenu();
      try { w.setPointerCapture(e.pointerId); } catch { /* not capturable */ }
      send(POINTER.down, e);
      e.preventDefault();
    });
    w.addEventListener("pointermove", e => send(POINTER.move, e));
    w.addEventListener("pointerup", e => { send(POINTER.up, e); try { w.releasePointerCapture(e.pointerId); } catch { /* released */ } });
    w.addEventListener("pointercancel", e => send(POINTER.cancel, e));
    w.addEventListener("lostpointercapture", e => send(POINTER.cancel, e));
    w.addEventListener("wheel", e => { e.preventDefault(); send(POINTER.wheel, e); }, {passive: false});
    w.addEventListener("contextmenu", e => e.preventDefault());
    w.addEventListener("selectstart", e => e.preventDefault());
    w.addEventListener("dragover", e => e.preventDefault());
    w.addEventListener("drop", e => { e.preventDefault(); this.readFiles(e.dataTransfer.files); });
  }

  initToolbar() {
    const bar = $("#toolbar");
    const button = (text, label, command, attrs = {}) => {
      const b = document.createElement("button");
      b.className = "tool";
      b.title = label;
      b.setAttribute("aria-label", label);
      b.textContent = text;
      for (const [k, v] of Object.entries(attrs)) b.setAttribute(k, v);
      b.onclick = typeof command === "function" ? command : () => this.run(command);
      bar.append(b);
      return b;
    };
    const sep = () => { const s = document.createElement("span"); s.className = "sep"; bar.append(s); };
    button("＋", "New scene (Ctrl+N)", {cmd: "newScene"});
    button("▱", "Open scene (Ctrl+O)", {cmd: "openScene"});
    button("▣", "Save scene (Ctrl+S)", {cmd: "saveScene"});
    sep();
    for (const [m, k, l] of [["body", "B", "Body"], ["fixture", "F", "Fixture"], ["vertex", "V", "Vertex"], ["joint", "J", "Joint"], ["image", "I", "Image"], ["sampler", "M", "Sampler"], ["world", "W", "World"]])
      button(k, l + " edit", {cmd: "setMode", mode: m}, {"data-mode": m});
    sep();
    button("↶", "Undo", {cmd: "undo"});
    button("↷", "Redo", {cmd: "redo"});
    sep();
    button("↔", "Translate (T/G)", {cmd: "beginOperation", kind: "translate"});
    button("⟳", "Rotate (R)", {cmd: "beginOperation", kind: "rotate"});
    button("⇱", "Scale (S)", {cmd: "beginOperation", kind: "scale"});
    button("⌖", "Move cursor (C)", {cmd: "beginOperation", kind: "cursor"});
    sep();
    button("□", "Fit scene (Home)", {cmd: "fitScene"});
    button("◉", "Newton cradle example", {cmd: "loadDemo", kind: "cradle"});
    button("★", "Game example: Roll & Collect", {cmd: "loadDemo", kind: "game"});
    button("▶", "Open player (Ctrl+R)", {cmd: "openPlayer"});
    sep();
    button("Items", "Toggle Items", () => $("#leftDock").classList.toggle("mobile-open")).classList.add("mobile-toggle");
    button("Props", "Toggle Properties", () => $("#rightDock").classList.toggle("mobile-open")).classList.add("mobile-toggle");
    button("+", "Actions (Space)", e => {
      const r = e.currentTarget.getBoundingClientRect();
      this.openPopup(this.engine.command({cmd: "menu", name: "actions"}).entries, r.left, r.bottom);
    });
  }

  initEvents() {
    for (const b of $$("[data-menu]"))
      b.onclick = () => {
        const r = b.getBoundingClientRect();
        this.openPopup(this.engine.command({cmd: "menu", name: b.dataset.menu}).entries, r.left, r.bottom);
      };
    document.addEventListener("pointerdown", e => {
      if (this.menu && !this.menu.contains(e.target) && !e.target.closest("[data-menu]") && !e.target.closest("#toolbar")) this.closeMenu();
    });
    const filter = () => this.run({cmd: "setTreeFilter", query: $("#itemFilter").value, kind: $("#itemKind").value});
    $("#itemFilter").oninput = filter;
    $("#itemKind").onchange = filter;
    $("#clearLog").onclick = () => { $("#log").replaceChildren(); this.run({cmd: "clearLog"}); };
    $("#sceneFile").onchange = e => { this.readFiles(e.target.files); e.target.value = ""; };
    $("#imageFile").onchange = e => { this.readFiles(e.target.files); e.target.value = ""; };
    $("#helpKeys").onclick = () => this.run({cmd: "dialog", name: "keyboard"});
    $("#helpAbout").onclick = () => this.run({cmd: "dialog", name: "about"});
    this.initPanels();
    addEventListener("keydown", e => this.onKey(e, true));
    addEventListener("keyup", e => this.onKey(e, false));
    addEventListener("blur", () => this.engine.e.kern_editor_blur());
    addEventListener("beforeunload", e => {
      if (this.engine.e.kern_editor_any_dirty()) {
        this.autosave(performance.now() + 1e6);
        e.preventDefault();
        e.returnValue = "";
      }
    });
    addEventListener("resize", () => { this.mobile = matchMedia("(pointer:coarse)").matches || innerWidth < 900; this.engine.e.kern_editor_set_mobile(this.mobile ? 1 : 0); this.revision = -1; });
  }

  initPanels() {
    for (const p of $$(".panel")) {
      p.dataset.home = p.parentElement.id;
      const title = p.querySelector(".paneltitle");
      title.addEventListener("pointerdown", e => {
        if (e.target.closest("button")) return;
        const start = {x: e.clientX, y: e.clientY}, rect = p.getBoundingClientRect();
        let dragging = false;
        const move = ev => {
          if (Math.hypot(ev.clientX - start.x, ev.clientY - start.y) < 5 && !dragging) return;
          if (!dragging) { if (!p.classList.contains("floating")) this.toggleFloat(p.id); dragging = true; }
          p.style.left = Math.max(0, Math.min(rect.left + ev.clientX - start.x, innerWidth - 80)) + "px";
          p.style.top = Math.max(25, Math.min(rect.top + ev.clientY - start.y, innerHeight - 50)) + "px";
        };
        const done = ev => {
          removeEventListener("pointermove", move);
          removeEventListener("pointerup", done);
          if (!dragging) return;
          const target = ev.clientX < 100 ? $("#leftDock") : ev.clientX > innerWidth - 100 ? $("#rightDock") : ev.clientY > innerHeight - 65 ? $("#bottomDock") : null;
          if (target) { p.classList.remove("floating"); p.removeAttribute("style"); p.dataset.home = target.id; target.append(p); }
        };
        addEventListener("pointermove", move);
        addEventListener("pointerup", done);
      });
    }
    $$("[data-hide]").forEach(b => b.onclick = () => { $("#" + b.dataset.hide).hidden = true; });
    $$("[data-float]").forEach(b => b.onclick = () => this.toggleFloat(b.dataset.float));
    for (const [id, axis] of [["leftSplit", "left"], ["rightSplit", "right"], ["bottomSplit", "bottom"]])
      $("#" + id).onpointerdown = e => {
        e.preventDefault();
        const move = ev => document.documentElement.style.setProperty(axis === "left" ? "--left" : axis === "right" ? "--right" : "--log",
          (axis === "left" ? Math.max(150, Math.min(ev.clientX, innerWidth * 0.4)) : axis === "right" ? Math.max(170, Math.min(innerWidth - ev.clientX, innerWidth * 0.4)) : Math.max(45, Math.min(innerHeight - ev.clientY - 27, innerHeight * 0.4))) + "px");
        const done = () => { removeEventListener("pointermove", move); removeEventListener("pointerup", done); };
        addEventListener("pointermove", move);
        addEventListener("pointerup", done);
      };
  }

  toggleFloat(id) {
    const p = $("#" + id);
    if (p.classList.contains("floating")) {
      p.classList.remove("floating");
      p.removeAttribute("style");
      $("#" + p.dataset.home).append(p);
    } else {
      const r = p.getBoundingClientRect();
      p.dataset.home = p.parentElement.id;
      p.classList.add("floating");
      Object.assign(p.style, {left: Math.max(5, r.left) + "px", top: Math.max(65, r.top) + "px", width: Math.max(230, r.width) + "px", height: Math.max(180, r.height) + "px"});
      document.body.append(p);
    }
  }

  resetPanels() {
    for (const p of $$(".panel")) {
      p.hidden = false;
      p.classList.remove("floating");
      p.removeAttribute("style");
      if (p.dataset.home) $("#" + p.dataset.home).append(p);
    }
    document.body.classList.remove("panels-hidden");
    for (const k of ["--left", "--right", "--log"]) document.documentElement.style.removeProperty(k);
  }

  onKey(e, down) {
    const key = e.key.toLowerCase(), ctrl = e.ctrlKey || e.metaKey;
    if (down && $(".modal-back")) {
      if (key === "escape") { $(".modal-back").remove(); e.preventDefault(); }
      return;
    }
    if (down && this.menu) {
      const bs = [...this.menu.querySelectorAll("button:not(:disabled)")], i = bs.indexOf(document.activeElement);
      if (key === "escape") { this.closeMenu(); this.focusActiveView(); e.preventDefault(); return; }
      if (key === "arrowdown" || key === "arrowup") { bs[(i + (key === "arrowdown" ? 1 : -1) + bs.length) % bs.length]?.focus(); e.preventDefault(); return; }
      if (key === "arrowright" || key === "enter") { document.activeElement.click(); e.preventDefault(); return; }
      if (key === "arrowleft") { this.menu.querySelector(".back")?.click(); e.preventDefault(); return; }
      return;
    }
    if (/^(INPUT|TEXTAREA|SELECT)$/.test(e.target.tagName) || e.target.isContentEditable) {
      if (down && ctrl && key === "s") { e.preventDefault(); e.target.blur(); this.run({cmd: "saveScene"}); }
      return;
    }
    const mods = (e.shiftKey ? 1 : 0) | (ctrl ? 2 : 0) | (e.altKey ? 4 : 0);
    const r = this.engine.key(e.key, e.code, down, mods, e.repeat);
    if (r?.handled) e.preventDefault();
    this.handleKeyResult(r);
  }

  handleKeyResult(r) { this.runUi(r?.ui); return r; }

  // ---- files, images, storage -----------------------------------------------
  async readFiles(files) {
    for (const f of files) {
      try {
        if (f.size > 30 * 1024 * 1024) throw Error("File exceeds 30 MB");
        if (f.type.startsWith("image/")) await this.addImageFile(f);
        else this.run({cmd: "loadDocument", text: await f.text(), name: f.name});
      } catch (error) {
        this.report(f.name + ": " + error.message, "error");
      }
    }
  }

  async addImageFile(f) {
    if (!/^image\/(png|jpeg|webp|gif)$/.test(f.type)) throw Error("Use PNG, JPEG, WebP or GIF images");
    if (f.size > 15 * 1024 * 1024) throw Error("Image exceeds 15 MB");
    const src = await new Promise((res, rej) => { const r = new FileReader(); r.onload = () => res(r.result); r.onerror = () => rej(Error("Could not read image")); r.readAsDataURL(f); });
    const img = new Image();
    img.src = src;
    await img.decode();
    this.run({cmd: "addImageData", name: f.name, src, width: img.naturalWidth, height: img.naturalHeight});
  }

  pollImages() { this.engine.pollImages(MAX_IMAGE_SIDE); }

  autosave(now) {
    const text = this.engine.text(this.engine.e.kern_editor_take_autosave(now));
    if (!text) return;
    try { localStorage.setItem(AUTOSAVE_KEY, text); } catch { this.report("Autosave unavailable or full. Use File → Save scene.", "warn"); }
  }

  download(name, blob) {
    const url = URL.createObjectURL(blob), a = document.createElement("a");
    a.href = url;
    a.download = name;
    document.body.append(a);
    a.click();
    a.remove();
    setTimeout(() => URL.revokeObjectURL(url), 15000);
  }

  downloadPixels(name, width, height) {
    const ptr = this.engine.e.kern_editor_pixels(), n = this.engine.e.kern_editor_pixels_length();
    if (!ptr || n !== width * height * 4) { this.report("No image data to save", "error"); return; }
    const c = document.createElement("canvas");
    c.width = width;
    c.height = height;
    const img = new ImageData(new Uint8ClampedArray(this.kern.u8.slice(ptr, ptr + n).buffer), width, height);
    c.getContext("2d").putImageData(img, 0, 0);
    c.toBlob(b => b ? this.download(name, b) : this.report("Could not encode PNG", "error"), "image/png");
  }

  exportView(viewId) {
    const rec = this.views.get(viewId);
    if (!rec) return;
    this.engine.e.kern_editor_frame(performance.now());
    const c = this.canvas.getBoundingClientRect(), r = rec.wrap.getBoundingClientRect(), dpr = this.dpr;
    const out = document.createElement("canvas");
    out.width = Math.max(1, Math.round(r.width * dpr));
    out.height = Math.max(1, Math.round(r.height * dpr));
    out.getContext("2d").drawImage(this.canvas, Math.round((r.left - c.left) * dpr), Math.round((r.top - c.top) * dpr), out.width, out.height, 0, 0, out.width, out.height);
    out.toBlob(b => b && this.download("scene3d.png", b), "image/png");
  }

  /** A standalone HTML game: the same engine module in player mode plus the scene. */
  async gameHtml(sceneText, title) {
    return buildGameHtml({sources: await moduleSources(), wasmBase64: this.wasmBase64, sceneText, title});
  }
}
