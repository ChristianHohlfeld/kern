// Kern Studio page: the DOM side of the C++ editor (kern/src/studio).
//
// No editing logic lives here. This file instantiates the engine module,
// draws the panels from the JSON the engine returns (kern_studio_state /
// kern_studio_status), forwards pointer and keyboard input to the viewports,
// and does what only a browser can do: menus and dialogs as DOM, file pickers,
// downloads, the clipboard, pointer lock, decoding images, local storage and
// packaging standalone games.
import {loadKern} from "../kern.mjs";
import {LogicEditor, LOGIC_STYLE} from "./logic.mjs";
import {StudioEngine, POINTER, modifiers, toBase64, escHtml, hudHtml, HUD_STYLE, GamepadInput, backgroundTicker, TouchOverlay, TOUCH_STYLE, watchInputDevice, routeTouch, buildGameHtml, startAudio, KernNet, openNetwork} from "./game.mjs";

const $ = s => document.querySelector(s), $$ = s => [...document.querySelectorAll(s)];
const esc = escHtml;
const AUTOSAVE_KEY = "kern-studio.autosave.v1", LAYOUT_KEY = "kern-studio.layout.v1";
// User settings (docs/studio.md, "Einstellungen"): only the user's own choices, as the engine reports
// them (settings.cpp). A setting never changed is not stored and follows the defaults of the build.
const SETTINGS_KEY = "kern-studio.settings.v1", SETTINGS_VERSION = 1;
const MAX_TEXTURE_SIDE = 2048;
const MAX_SOUND_BYTES = 24 * 1024 * 1024;

// ---------------------------------------------------------------------------
// Icons (24 x 24 strokes)

const ICONS = {
  save: '<path d="M5 4h11l3 3v13H5z"/><path d="M8 4v5h7V4M8 20v-6h8v6"/>',
  undo: '<path d="M9 14L4 9l5-5"/><path d="M4 9h10a6 6 0 010 12h-3"/>',
  redo: '<path d="M15 14l5-5-5-5"/><path d="M20 9H10a6 6 0 000 12h3"/>',
  plus: '<path d="M12 5v14M5 12h14"/>',
  caret: '<path d="M7 10l5 5 5-5"/>',
  chevron: '<path d="M9 6l6 6-6 6"/>',
  check: '<path d="M5 12.5l4.5 4.5L19 7.5"/>',
  cursor: '<path d="M6 3l12 9-5.5 1L15 19l-2.5 1-2.5-6L6 17z"/>',
  move: '<path d="M12 3v18M3 12h18M12 3l-3 3M12 3l3 3M12 21l-3-3M12 21l3-3M3 12l3-3M3 12l3 3M21 12l-3-3M21 12l-3 3"/>',
  rotate: '<path d="M20 12a8 8 0 11-2.4-5.7"/><path d="M20 4v5h-5"/>',
  scale: '<path d="M4 20V10h10v10z"/><path d="M14 10l6-6M15 4h5v5"/>',
  globe: '<circle cx="12" cy="12" r="8.5"/><path d="M3.5 12h17M12 3.5c2.5 2.6 3.5 5.5 3.5 8.5s-1 5.9-3.5 8.5c-2.5-2.6-3.5-5.5-3.5-8.5s1-5.9 3.5-8.5z"/>',
  local: '<path d="M12 3l8 4.5v9L12 21l-8-4.5v-9z"/><path d="M12 12l8-4.5M12 12v9M12 12L4 7.5"/>',
  grid: '<path d="M4 4h16v16H4zM4 9.3h16M4 14.6h16M9.3 4v16M14.6 4v16"/>',
  angle: '<path d="M4 20h16M4 20L16 5"/><path d="M10 20a6 6 0 00-1.8-4.3"/>',
  scaleSnap: '<path d="M4 4h6v6H4zM14 14h6v6h-6z"/><path d="M10 10l4 4"/>',
  camera: '<path d="M3 8h4l2-3h6l2 3h4v11H3z"/><circle cx="12" cy="13" r="3.5"/>',
  play: '<path d="M7 4.5v15l13-7.5z"/>',
  simulate: '<path d="M12 3v3M12 18v3M3 12h3M18 12h3"/><circle cx="12" cy="12" r="5"/><path d="M12 9.5v2.5l2 1.5"/>',
  pause: '<path d="M8 5v14M16 5v14"/>',
  step: '<path d="M6 5l9 7-9 7zM18 5v14"/>',
  stop: '<path d="M6 6h12v12H6z"/>',
  eject: '<path d="M5 15h14L12 6z"/><path d="M5 19h14"/>',
  panels: '<rect x="3" y="4" width="18" height="16" rx="2"/><path d="M14 4v16M16.5 8h2M16.5 11h2M16.5 14h2"/>',
  layout: '<path d="M4 4h16v16H4zM12 4v16M4 12h16"/>',
  layout1: '<path d="M4 4h16v16H4z"/>',
  layout2: '<path d="M4 4h16v16H4zM12 4v16"/>',
  package: '<path d="M12 3l8 4v10l-8 4-8-4V7z"/><path d="M4 7l8 4 8-4M12 11v10"/>',
  folder: '<path d="M3 6h6l2 2h10v11H3z"/>',
  folderPlus: '<path d="M3 6h6l2 2h10v11H3z"/><path d="M12 11v5M9.5 13.5h5"/>',
  search: '<circle cx="11" cy="11" r="6"/><path d="M20 20l-4.5-4.5"/>',
  eye: '<path d="M2.5 12S6 5.5 12 5.5 21.5 12 21.5 12 18 18.5 12 18.5 2.5 12 2.5 12z"/><circle cx="12" cy="12" r="3"/>',
  eyeOff: '<path d="M3 3l18 18M10.5 6c.5-.1 1-.1 1.5-.1 6 0 9.5 6.5 9.5 6.5a17 17 0 01-2.6 3.4M6.6 6.6A16 16 0 002.5 12S6 18.5 12 18.5c1.6 0 3-.4 4.3-1"/><path d="M9.9 9.9a3 3 0 004.2 4.2"/>',
  close: '<path d="M6 6l12 12M18 6L6 18"/>',
  drawer: '<path d="M3 5h18v14H3zM3 14h18"/><path d="M9 17h6"/>',
  log: '<path d="M4 5h16M4 10h16M4 15h10M4 20h7"/>',
  lock: '<path d="M6 11h12v9H6z"/><path d="M8.5 11V8a3.5 3.5 0 017 0v3"/>',
  unlock: '<path d="M6 11h12v9H6z"/><path d="M8.5 11V8a3.5 3.5 0 016.8-1"/>',
  trash: '<path d="M4 7h16M9 7V4h6v3M6 7l1 13h10l1-13"/>',
  reset: '<path d="M4 12a8 8 0 102.3-5.7"/><path d="M4 4v5h5"/>',
  menu: '<path d="M4 7h16M4 12h16M4 17h16"/>',
  maximize: '<path d="M4 9V4h5M20 9V4h-5M4 15v5h5M20 15v5h-5"/>',
  minimize: '<path d="M9 4v5H4M15 4v5h5M9 20v-5H4M15 20v-5h5"/>',
  texture: '<path d="M4 4h16v16H4z"/><path d="M4 16l5-5 4 4 3-3 4 4"/><circle cx="15.5" cy="8.5" r="1.5"/>',
  box: '<path d="M12 3l8 4.5v9L12 21l-8-4.5v-9z"/><path d="M12 12l8-4.5M12 12v9M12 12L4 7.5"/>',
  cube: '<path d="M12 3l8 4.5v9L12 21l-8-4.5v-9z"/><path d="M12 12l8-4.5M12 12v9M12 12L4 7.5"/>',
  sphere: '<circle cx="12" cy="12" r="8.5"/><path d="M3.5 12c0 2.5 3.8 4 8.5 4s8.5-1.5 8.5-4"/>',
  cylinder: '<ellipse cx="12" cy="6" rx="7" ry="2.5"/><path d="M5 6v12c0 1.4 3.1 2.5 7 2.5s7-1.1 7-2.5V6"/>',
  cone: '<path d="M12 3L5 18M12 3l7 15"/><ellipse cx="12" cy="18" rx="7" ry="2.5"/>',
  capsule: '<path d="M8 8a4 4 0 018 0v8a4 4 0 01-8 0z"/>',
  plane: '<path d="M2.5 15l6-6h13l-6 6z"/>',
  torus: '<ellipse cx="12" cy="12" rx="9" ry="5"/><ellipse cx="12" cy="11.5" rx="3.5" ry="1.6"/>',
  ramp: '<path d="M3 18h18V8z"/>',
  empty: '<path d="M12 4v16M4 12h16"/><circle cx="12" cy="12" r="2.5"/>',
  light: '<path d="M9 18h6M10 21h4"/><path d="M12 3a6 6 0 00-3.5 10.9V16h7v-2.1A6 6 0 0012 3z"/>',
  player: '<circle cx="12" cy="7" r="3.5"/><path d="M5 21v-2a7 7 0 0114 0v2"/>',
  person: '<circle cx="12" cy="6" r="3"/><path d="M8 21l1-8-2-3h10l-2 3 1 8"/>',
  trigger: '<path d="M4 4h16v16H4z" stroke-dasharray="3 2.5"/><path d="M9 12l2 2 4-4"/>',
  pickup: '<circle cx="12" cy="12" r="5"/><path d="M12 2.5v3M12 18.5v3M2.5 12h3M18.5 12h3M5.3 5.3l2.1 2.1M16.6 16.6l2.1 2.1M5.3 18.7l2.1-2.1M16.6 7.4l2.1-2.1"/>',
  joint: '<circle cx="6" cy="18" r="2.5"/><circle cx="18" cy="6" r="2.5"/><path d="M8 16l8-8"/>',
  physics: '<path d="M12 3l8 4.5v9L12 21l-8-4.5v-9z"/><path d="M8 13l3 3 5-6"/>',
  city: '<path d="M3 21V9h5v12M8 21V4h7v17M15 21v-9h6v9M2 21h20"/><path d="M10.5 7h2M10.5 10h2M10.5 13h2"/>',
  car: '<path d="M3 15l2-5h14l2 5v3H3z"/><path d="M7 10l1.5-3h7L17 10"/><circle cx="7.5" cy="18" r="1.5"/><circle cx="16.5" cy="18" r="1.5"/>',
  ball: '<circle cx="12" cy="12" r="8.5"/><path d="M5 7c4 1 8 6 9 13.5M9.5 3.8c1.5 4 6.5 7 11 7"/>',
  drone: '<circle cx="5.5" cy="5.5" r="2.5"/><circle cx="18.5" cy="5.5" r="2.5"/><circle cx="5.5" cy="18.5" r="2.5"/><circle cx="18.5" cy="18.5" r="2.5"/><path d="M7.5 7.5l3 3M16.5 7.5l-3 3M7.5 16.5l3-3M16.5 16.5l-3-3"/><path d="M10 10h4v4h-4z"/>',
  orb: '<circle cx="12" cy="12" r="5.5"/><circle cx="12" cy="12" r="9" stroke-dasharray="2 3"/>',
  star: '<path d="M12 3l2.7 5.6 6.1.9-4.4 4.3 1 6.1L12 17l-5.4 2.9 1-6.1-4.4-4.3 6.1-.9z"/>',
  goal: '<path d="M5 21V4M5 4h11l-2 3.5 2 3.5H5"/>',
  hazard: '<path d="M12 3l9.5 17h-19z"/><path d="M12 10v4.5M12 17.5v.5"/>',
  flag: '<path d="M5 21V4M5 4c3-1.5 5 1.5 8 0s4-1 6 0v8c-2-1-3-1.5-6 0s-5-1.5-8 0"/>',
  message: '<path d="M4 5h16v11H9l-5 4z"/>',
  platform: '<path d="M3 15h18v3H3z"/><path d="M12 4v8M9 9l3 3 3-3"/>',
  spinner: '<path d="M3 12h18"/><circle cx="12" cy="12" r="2"/><path d="M17 6.5a8 8 0 012 3M7 17.5a8 8 0 01-2-3"/>',
  stack: '<path d="M7 14h5v6H7zM12 14h5v6h-5zM9.5 8h5v6h-5z"/>',
  pendulum: '<path d="M5 3h14M12 3l4 12"/><circle cx="16.5" cy="17" r="3"/>',
  door: '<path d="M5 3h3v18H5z"/><path d="M8 4l10 2v13l-10 2"/><circle cx="15" cy="12.5" r=".7"/>',
  chain: '<rect x="4" y="9" width="8" height="6" rx="3"/><rect x="12" y="9" width="8" height="6" rx="3"/>',
  seesaw: '<path d="M3 9l18 4M12 11l-3 8h6z"/>',
  soldier: '<circle cx="9" cy="5.5" r="2.5"/><path d="M9 8v6l-2 6M9 14l2 6M6 10.5h9l4-1.5"/>',
  target: '<circle cx="12" cy="12" r="8.5"/><circle cx="12" cy="12" r="5"/><circle cx="12" cy="12" r="1.5"/>',
  health: '<path d="M4 6h16v13H4z"/><path d="M12 9v7M8.5 12.5h7"/>',
  ammo: '<path d="M6 20V9l2-4 2 4v11zM14 20V9l2-4 2 4v11z"/>',
  duck: '<path d="M7 10a4 4 0 1 1 7 2.6c2.5.4 5 1.4 6 3.4-1 3-4.5 4-8 4s-6.5-1.5-6.5-4.5c0-1.6.9-2.8 2.2-3.6"/><path d="M11 9h4.5"/><circle cx="9.5" cy="8" r=".6"/><path d="M9 20v1.5M13 20v1.5"/>',
  jumppad: '<ellipse cx="12" cy="18" rx="8" ry="2.5"/><path d="M12 15V4M8 8l4-4 4 4"/>',
  teleport: '<ellipse cx="7" cy="18" rx="4" ry="1.5"/><ellipse cx="17" cy="18" rx="4" ry="1.5"/><path d="M7 15c0-6 10-6 10 0M15 13l2 2 2-2"/>',
  barrel: '<ellipse cx="12" cy="5.5" rx="6" ry="2"/><path d="M6 5.5v13c0 1.1 2.7 2 6 2s6-.9 6-2v-13M6 10h12M6 15h12"/>',
  logic: '<rect x="3" y="4" width="7" height="6" rx="1.5"/><rect x="14" y="14" width="7" height="6" rx="1.5"/><path d="M10 7h3a2 2 0 012 2v5"/>',
  speaker: '<path d="M4 9h4l5-4v14l-5-4H4z"/><path d="M16.5 8.5a5 5 0 010 7M19 6a8.5 8.5 0 010 12"/>',
  speakerOff: '<path d="M4 9h4l5-4v14l-5-4H4z"/><path d="M16.5 9.5l5 5M21.5 9.5l-5 5"/>',
  note: '<path d="M9 18V5l11-2v13"/><circle cx="6.5" cy="18" r="2.5"/><circle cx="17.5" cy="16" r="2.5"/>',
  wave: '<path d="M3 12h2l2-6 3 12 3-9 2 6 2-3h4"/>',
};
const icon = name => `<svg class="i" viewBox="0 0 24 24" aria-hidden="true">${ICONS[name] || ICONS.box}</svg>`;
const fillIcons = (root = document) => {
  for (const el of root.querySelectorAll("[data-icon]")) if (!el.firstElementChild) el.innerHTML = icon(el.dataset.icon);
};

// ---------------------------------------------------------------------------

/** Module sources for packaged games: embedded by the single-file build, fetched otherwise. */
async function moduleSources() {
  if (window.KERN_SOURCES) return window.KERN_SOURCES;
  const get = async rel => (await fetch(new URL(rel, import.meta.url))).text();
  return (window.KERN_SOURCES = {kern: await get("../kern.mjs"), game: await get("./game.mjs")});
}

export async function startStudio({url = null, base64 = null, bytes = null} = {}) {
  if (base64 == null && bytes == null && url) {
    const response = await fetch(url);
    if (!response.ok) throw Error(`${url} - ${response.status} (build it with tools/build_studio.sh)`);
    bytes = new Uint8Array(await response.arrayBuffer());
  }
  const kern = await loadKern({bytes, base64});
  const engine = new StudioEngine(kern);
  const page = new StudioPage(kern, engine, () => base64 ?? toBase64(bytes));
  page.start();
  window.KernStudio = page.api();
  engine.loadPolicies().catch(() => {});  // the robot's moves (sit, pick, kicks, roll, rollers)
  return page;
}

class StudioPage {
  constructor(kern, engine, wasmBase64) {
    this.kern = kern;
    this.engine = engine;
    this.wasmBase64 = wasmBase64;
    this.canvas = $("#gl");
    this.state = null;
    this.revision = -1;
    this.status = null;
    this.views = [];          // DOM records of the 4 viewports
    this.panelKeys = {};      // last rendered JSON per panel (skip unchanged DOM)
    this.pendingDetails = false;
    this.menu = null;         // open popup stack
    this.menubarOpen = null;
    this.dialog = null;
    this.fileHandle = null;
    this.catalog = null;
    this.locked = false;      // pointer lock for play mode
    this.textureThumbs = new Map();
    this.lastTexturePoll = 0;
    this.lastSavePoll = 0;
    this.scrub = null;
    this.consoleHistory = [];
    this.consoleIndex = -1;
    this.dpr = 1;
  }

  // ---- startup ---------------------------------------------------------------
  start() {
    const gl = this.canvas.getContext("webgl2", {antialias: true, alpha: false, preserveDrawingBuffer: false, powerPreference: "high-performance"});
    if (!gl) throw Error("WebGL2 is not available in this browser");
    this.gl = gl;
    this.kern.attachContext(this.canvas, gl);
    this.restoreLayout();
    this.resizeCanvas();
    if (!this.engine.init(this.canvas.width, this.canvas.height, false)) throw Error(this.kern.lastError());
    this.loadSettings();
    this.audio = startAudio(this.engine);
    this.net = new KernNet(this.engine);
    this.net.onstatus = text => this.toast(text);
    this.canvas.addEventListener("webglcontextlost", e => { e.preventDefault(); this.engine.contextLost(); this.kern.resetTables(); });
    this.canvas.addEventListener("webglcontextrestored", () => {
      this.kern.attachContext(this.canvas, gl);
      if (!this.engine.contextRestored()) this.toast("The WebGL context could not be restored: " + this.kern.lastError());
    });
    const style = document.createElement("style");
    style.textContent = HUD_STYLE + "\n" + TOUCH_STYLE;
    document.head.append(style);
    watchInputDevice(this.engine);
    $("#touchExit").addEventListener("click", () => { this.engine.command({cmd: "stop"}); this.flushUi(); });
    $("#touchPause").addEventListener("click", () => { this.engine.command({cmd: "pause"}); this.flushUi(); });
    addEventListener("pagehide", () => this.saveSettings());
    document.addEventListener("visibilitychange", () => { if (document.hidden) this.saveSettings(); });
    fillIcons();
    this.buildViewports();
    // The logic editor covers the viewports while it is open (like a Blueprint editor tab).
    {
      const style = document.createElement("style");
      style.textContent = LOGIC_STYLE;
      document.head.append(style);
      const el = document.createElement("section");
      el.id = "logicEditor";
      el.hidden = true;
      $("#viewArea").append(el);
      this.logic = new LogicEditor(el, {
        engine: this.engine,
        state: () => this.state,
        flushUi: () => this.flushUi(),
        openMenu: (entries, x, y) => this.openMenu(entries, x, y),
        toast: (text, actions, ms) => this.toast(text, actions, ms),
        uiAction: a => this.uiAction(a),
      });
    }
    this.initToolbar();
    this.initPanels();
    this.initInput();
    this.initSplitters();
    this.catalog = this.engine.command({cmd: "catalog"}).groups;
    this.renderCatalog();
    this.offerRestore();
    requestAnimationFrame(t => this.frame(t));
    // Hosting or playing online in a background tab: keep the simulation and the network going.
    backgroundTicker(time => {
      if (!this.net.active) return;
      this.engine.frame(time);
      this.audio?.pump();
      this.net.flush();
      this.flushUi();
    });
    setTimeout(() => $("#app").classList.remove("booting"), 60);
  }

  api() {
    const self = this;
    return {
      engine: self.engine,
      command: c => { const r = self.engine.command(c); self.flushUi(); return r; },
      get state() { return self.engine.state(); },
      get status() { return self.engine.status(); },
      get logic() { return self.logic; },  // the Blueprint editor (tests)
      get dialog() { return self.dialog?.name ?? null; },
      get logicOpenCached() { return !!self.state?.logic?.open; },
      key: (code, opts = {}) => self.engine.key(opts.key ?? code, code, opts.down !== false, (opts.shift ? 1 : 0) | (opts.ctrl ? 2 : 0) | (opts.alt ? 4 : 0), false),
      pointer: (viewport, type, x, y, opts = {}) => self.engine.pointer(viewport, POINTER[type], x, y, opts),
      viewportRect: i => self.views[i].el.getBoundingClientRect().toJSON(),
      get audio() { return self.audio ? {mode: self.audio.mode, rate: self.audio.rate, state: self.audio.context?.state, written: self.audio.written, consumed: self.audio.consumed, underruns: self.audio.underruns} : null; },
      importSoundFile: file => self.importSound(file),
      packageHtml: async title => self.gameHtml(self.engine.command({cmd: "packageText"}).text, title),
      frames: () => self.frameCount,
      get net() { const n = self.net; return {kind: n.kind, self: n.self, host: n.host, sent: n.sent, received: n.received}; },
    };
  }

  // Restore from the last session (docs/studio.md, "Wiederherstellen"). The stored copy exists only
  // while a document has unsaved changes: the engine writes it for dirty documents and asks for it to
  // be deleted once they are saved or undone back to the saved state. A normal reload of a clean
  // editor finds nothing; a crash, a closed tab or an update reload with unsaved work finds it.
  offerRestore() {
    let saved = null;
    try { saved = JSON.parse(localStorage.getItem(AUTOSAVE_KEY) || "null"); } catch { /* storage unavailable */ }
    if (!saved?.text) return;
    this.toast(`Restore "${saved.name}" from your last session?`, [
      {label: "Restore", primary: true, run: () => { this.engine.command({cmd: "restore", text: saved.text, name: saved.name}); this.fileHandle = null; }},
      {label: "Dismiss", run: () => this.dropAutosave()},
    ], 15000);
  }

  dropAutosave() {
    try { localStorage.removeItem(AUTOSAVE_KEY); } catch { /* storage unavailable */ }
  }

  // ---- frame loop -------------------------------------------------------------
  frame(time) {
    try {
      this.frameCount = (this.frameCount || 0) + 1;
      this.resizeCanvas();
      this.placeViewports();
      (this.gamepad ||= new GamepadInput(this.engine)).poll();
      this.engine.frame(time);
      this.audio?.pump();
      if (this.net.active) this.net.flush();
      this.flushUi();
      const rev = this.engine.revision();
      if (rev !== this.revision) {
        this.revision = rev;
        this.state = this.engine.state();
        this.render(this.state);
      }
      this.status = this.engine.status();
      // Left the game (Stop, the host went away): close the transport. Only once the engine had a
      // game on it - while a join waits for the host's hello there is no game yet.
      if (this.status.net) this.netSeen = true;
      else if (this.netSeen) { this.netSeen = false; if (this.net.active) this.net.close(); }
      this.renderStatus(this.status);
      if (time - this.lastTexturePoll > 200) { this.lastTexturePoll = time; this.engine.pollTextures(MAX_TEXTURE_SIDE); this.engine.pollSounds(this.audio); this.engine.loadPolicies().catch(() => {}); }
      if (time - this.lastSavePoll > 500) { this.lastSavePoll = time; this.autosave(time); this.saveSettings(); }
    } catch (error) {
      console.error(error);
      $("#engineNotice").style.display = "block";
      $("#engineNotice").textContent = "Studio error: " + error.message;
    }
    requestAnimationFrame(t => this.frame(t));
  }

  resizeCanvas() {
    const dpr = Math.min(devicePixelRatio || 1, 2);
    const w = Math.max(1, Math.round(this.canvas.clientWidth * dpr)), h = Math.max(1, Math.round(this.canvas.clientHeight * dpr));
    if (this.canvas.width !== w || this.canvas.height !== h) {
      this.canvas.width = w;
      this.canvas.height = h;
      this.engine.resize(w, h);
      this.rectKeys = [];
    }
    this.dpr = dpr;
  }

  placeViewports() {
    const c = this.canvas.getBoundingClientRect(), dpr = this.dpr;
    this.rectKeys ??= [];
    for (const v of this.views) {
      const r = v.el.getBoundingClientRect();
      const visible = !v.el.classList.contains("hidden") && r.width > 1 && r.height > 1;
      let x = Math.round((r.left - c.left) * dpr), w = Math.round(r.width * dpr), h = Math.round(r.height * dpr);
      let y = Math.round((c.bottom - r.bottom) * dpr);
      // Snap a full-size viewport exactly to the drawing buffer (no scissor: bloom runs).
      if (Math.abs(x) <= 1 && Math.abs(y) <= 1 && Math.abs(w - this.canvas.width) <= 2 && Math.abs(h - this.canvas.height) <= 2) { x = 0; y = 0; w = this.canvas.width; h = this.canvas.height; }
      const key = `${x},${y},${w},${h},${visible},${r.width},${r.height}`;
      if (this.rectKeys[v.index] === key) continue;
      this.rectKeys[v.index] = key;
      this.engine.setViewport(v.index, x, y, w, h, r.width, r.height, dpr, visible);
    }
  }

  autosave(time) {
    const text = this.engine.takeAutosave(time);
    if (this.engine.takeAutosaveDiscard()) this.dropAutosave();
    if (!text) return;
    try { localStorage.setItem(AUTOSAVE_KEY, JSON.stringify({name: this.state?.level?.name || "Untitled", text, time: Date.now()})); } catch { /* full or unavailable */ }
  }

  // ---- user settings -------------------------------------------------------------
  loadSettings() {
    let stored = null;
    try { stored = JSON.parse(localStorage.getItem(SETTINGS_KEY) || "null"); } catch { /* storage unavailable or broken */ }
    if (stored?.version === SETTINGS_VERSION && stored.values && typeof stored.values === "object")
      this.engine.command({cmd: "applySettings", values: stored.values});
  }

  /** Stores the user's choices when the engine reports a change (nothing at all while none was made). */
  saveSettings() {
    const values = this.engine.command({cmd: "takeSettings"})?.values;
    if (!values) return;
    try {
      if (Object.keys(values).length) localStorage.setItem(SETTINGS_KEY, JSON.stringify({version: SETTINGS_VERSION, values}));
      else localStorage.removeItem(SETTINGS_KEY);
    } catch { /* full or unavailable */ }
  }

  /** Edit > Reset Settings to Defaults: the engine reset its settings; the page resets its own (panels, drawer). */
  resetPageSettings() {
    try { localStorage.removeItem(LAYOUT_KEY); } catch { /* unavailable */ }
    const root = document.documentElement.style;
    root.removeProperty("--side");
    root.removeProperty("--outliner");
    this.drawerTab("place");
    $("#drawer").hidden = true;
    $("#drawerBtn").classList.remove("on");
    $("#logBtn").classList.remove("on");
    this.saveSettings();
    this.rectKeys = [];
    this.toast("Settings reset to defaults");
  }

  // ---- UI actions from the engine ----------------------------------------------
  flushUi() {
    for (const a of this.engine.takeUi()) this.uiAction(a);
  }

  uiAction(a) {
    switch (a.action) {
      case "capturePointer":
        if (document.pointerLockElement !== this.canvas) {
          try { const p = this.canvas.requestPointerLock?.(); p?.catch?.(() => {}); } catch { /* needs a user gesture */ }
        }
        break;
      case "releasePointer":
        if (document.pointerLockElement) { this.releasingLock = true; document.exitPointerLock(); }
        break;
      case "clipboard":
        navigator.clipboard?.writeText(a.text).catch(() => {});
        break;
      case "readClipboard":
        navigator.clipboard?.readText().then(text => { if (text) this.engine.command({cmd: "pasteText", text}); }).catch(() => {});
        break;
      case "popup": {
        const v = this.views[a.viewport], r = v.el.getBoundingClientRect();
        this.openMenu(a.entries, r.left + a.x, r.top + a.y);
        break;
      }
      case "dialog": this.openDialog(a.name, a.dialog); break;
      case "settingsReset": this.resetPageSettings(); break;
      case "logicBreak": this.logic.onBreak(a.node); break;
      case "save": this.save(false); break;
      case "saveAs": this.save(true); break;
      case "open": this.openFile(); break;
      case "importTexture": $("#fileTexture").click(); break;
      case "packageGame":
        if (a.text) this.packageGame(a.text, a.title);
        else this.engine.command({cmd: "dialog", name: "export"});
        break;
      case "toggleDrawer": this.toggleDrawer(); break;
      case "toggleLog": this.toggleDrawer("log"); break;
      case "fullscreen":
        if (document.fullscreenElement) document.exitFullscreen();
        else document.documentElement.requestFullscreen?.().catch(() => {});
        break;
      case "rename": this.startRename(a.id || this.state?.selection?.at(-1)); break;
      case "netOpen":
        openNetwork(this.engine, this.net, a).then(r => {
          if (r?.ok) this.toast(a.host ? `Hosting room "${a.room}" - players join with Play > Join Online Game` : `Joined room "${a.room}"`);
          this.flushUi();
        }, error => { this.net.close(); this.toast("Online play: " + error.message); });
        break;
      default: break;
    }
  }

  // ---- viewports ------------------------------------------------------------------
  buildViewports() {
    const host = $("#viewports");
    host.innerHTML = "";
    for (let i = 0; i < 4; i++) {
      const el = document.createElement("div");
      el.className = "vp";
      el.dataset.index = i;
      el.innerHTML = `<div class="vp-bar">
          <button class="vb icon" data-vp="options" title="Viewport options" data-icon="menu"></button>
          <button class="vb" data-vp="type"></button>
          <button class="vb" data-vp="mode"></button>
          <button class="vb" data-vp="show">Show</button>
          <span class="spacer"></span>
          <button class="vb icon" data-vp="maximize" title="Maximise / restore (quad layout)" data-icon="maximize"></button>
        </div>
        <svg class="vp-axes" viewBox="-22 -22 44 44"></svg>
        <div class="vp-info"></div><div class="vp-gizmo" hidden></div><div class="vp-hint" hidden></div>
        <div class="vp-marquee" hidden></div><div class="k-hud" hidden></div>`;
      fillIcons(el);
      host.append(el);
      const rec = {index: i, el, bar: el.querySelector(".vp-bar"), info: el.querySelector(".vp-info"), axes: el.querySelector(".vp-axes"),
                   gizmo: el.querySelector(".vp-gizmo"), hint: el.querySelector(".vp-hint"), marquee: el.querySelector(".vp-marquee"), hud: el.querySelector(".k-hud"),
                   type: el.querySelector('[data-vp="type"]'), mode: el.querySelector('[data-vp="mode"]'), hudKey: "", infoKey: ""};
      this.views.push(rec);
      this.initViewportInput(rec);
      rec.bar.addEventListener("pointerdown", e => e.stopPropagation());
      rec.bar.addEventListener("click", e => {
        const b = e.target.closest("[data-vp]");
        if (!b) return;
        const r = b.getBoundingClientRect();
        const kind = b.dataset.vp;
        if (kind === "maximize") { this.engine.command({cmd: "maximize", viewport: i}); return; }
        this.engine.command({cmd: "activeViewport", viewport: i});
        this.openMenu(this.viewportMenu(i, kind), r.left, r.bottom + 2);
      });
      rec.hud.addEventListener("click", e => { if (e.target.closest("[data-hud=restart]")) this.engine.command({cmd: "restart"}); });
    }
  }

  viewportMenu(i, kind) {
    const v = this.state.viewports[i];
    const c = (cmd, extra = {}) => ({cmd, viewport: i, ...extra});
    const types = [["perspective", "Perspective", "Alt+G"], ["top", "Top", "Alt+J"], ["bottom", "Bottom", ""], ["left", "Left", ""], ["right", "Right", "Alt+K"], ["front", "Front", "Alt+H"], ["back", "Back", ""]]
      .map(([id, label, k]) => ({label, shortcut: k, checked: v.type === id, cmd: c("viewType", {type: id})}));
    const modes = [["lit", "Lit", "Alt+4"], ["unlit", "Unlit", "Alt+3"], ["wireframe", "Wireframe", "Alt+2"], ["collision", "Player Collision", "Alt+5"]]
      .map(([id, label, k]) => ({label, shortcut: k, checked: v.mode === id, cmd: c("viewMode", {mode: id})}));
    const flags = [["grid", "Grid"], ["icons", "Icons (lights, empties)"], ["selection", "Selection outline"], ["colliders", "Collision shapes"], ["joints", "Constraints"],
                   ["contacts", "Contact points"], ["bounds", "Bounds"], ["fog", "Fog"], ["sky", "Sky"], ["stats", "Stats"]]
      .map(([id, label]) => ({label, checked: v.flags[id], keepOpen: true, cmd: c("showFlag", {flag: id, value: !v.flags[id]})}));
    if (kind === "type") return [{header: "Perspective"}, types[0], {separator: true}, {header: "Orthographic"}, ...types.slice(1)];
    if (kind === "mode") return [{header: "View mode"}, ...modes];
    if (kind === "show") return [{header: "Show"}, ...flags];
    const fovs = [50, 70, 90, 110].map(f => ({label: `${f}°`, checked: Math.round(v.fov) === f, cmd: c("fov", {value: f})}));
    return [
      {label: "Game View", shortcut: "G", checked: v.gameView, cmd: c("gameView")},
      {label: "Field of View", submenu: fovs},
      {separator: true},
      {label: "Focus Selection", shortcut: "F", cmd: c("focus")},
      {label: "Bookmarks", submenu: Array.from({length: 9}, (_, k) => ({label: `Jump to bookmark ${k + 1}`, shortcut: `${k + 1}`, disabled: !this.state.bookmarks.includes(k + 1), cmd: {cmd: "bookmark", slot: k + 1}}))
        .concat([{separator: true}], Array.from({length: 9}, (_, k) => ({label: `Set bookmark ${k + 1}`, shortcut: `Ctrl+${k + 1}`, cmd: {cmd: "bookmark", slot: k + 1, set: true}})))},
      {separator: true},
      {label: "Layout", submenu: [["single", "1 viewport"], ["twoColumns", "2 viewports"], ["quad", "4 viewports"]].map(([id, label]) => ({label, checked: this.state.prefs.layout === id, cmd: {cmd: "layout", layout: id}}))},
    ];
  }

  initViewportInput(v) {
    const el = v.el, i = v.index;
    const pos = e => { const r = el.getBoundingClientRect(); return [e.clientX - r.left, e.clientY - r.top]; };
    const send = (type, e, extra = {}) => {
      const [x, y] = pos(e);
      return this.engine.pointer(i, type, x, y, {button: e.pointerType !== "mouse" && this.state?.play?.state === "playing" ? -1 : e.button, buttons: e.buttons, mods: modifiers(e), ...extra});
    };
    el.addEventListener("pointerdown", e => {
      if (e.target.closest(".vp-bar, .k-hud-banner")) return;
      this.closeMenus();
      if (document.activeElement && document.activeElement !== document.body && document.activeElement.blur) document.activeElement.blur();
      try { el.setPointerCapture(e.pointerId); } catch { /* ignore */ }
      // Fingers on the playing viewport drive the engine's touch controls (sticks, look, buttons).
      if (this.state?.viewports?.[i]?.active && routeTouch(this.engine, POINTER.down, e, pos)) { this.flushUi(); e.preventDefault(); return; }
      send(POINTER.down, e);
      this.flushUi();
      e.preventDefault();
    });
    el.addEventListener("pointermove", e => {
      if (document.pointerLockElement === this.canvas) return;
      if (routeTouch(this.engine, POINTER.move, e, pos)) return;
      send(POINTER.move, e);
    });
    el.addEventListener("pointerup", e => { if (!routeTouch(this.engine, POINTER.up, e, pos)) send(POINTER.up, e); this.flushUi(); });
    el.addEventListener("pointercancel", e => { if (!routeTouch(this.engine, POINTER.cancel, e, pos)) send(POINTER.cancel, e); });
    el.addEventListener("pointerleave", e => { if (!e.buttons) send(POINTER.leave, e); });
    el.addEventListener("wheel", e => {
      e.preventDefault();
      const scale = e.deltaMode === 1 ? 40 : e.deltaMode === 2 ? 400 : 1;
      send(POINTER.wheel, e, {wheel: e.deltaY * scale});
    }, {passive: false});
    el.addEventListener("contextmenu", e => e.preventDefault());
    el.addEventListener("dblclick", e => {
      if (e.target.closest(".vp-bar")) return;
      if (this.state?.selection?.length && this.state.play.state === "editing") this.engine.command({cmd: "focus", viewport: i});
    });
    // Drag and drop from the content drawer.
    el.addEventListener("dragover", e => {
      if (!e.dataTransfer.types.includes("text/x-kern-actor") && !e.dataTransfer.types.includes("text/x-kern-sound")) return;
      e.preventDefault();
      e.dataTransfer.dropEffect = "copy";
      el.classList.add("drop");
    });
    el.addEventListener("dragleave", () => el.classList.remove("drop"));
    el.addEventListener("drop", e => {
      el.classList.remove("drop");
      const item = e.dataTransfer.getData("text/x-kern-actor"), sound = e.dataTransfer.getData("text/x-kern-sound");
      if (!item && !sound) return;
      e.preventDefault();
      const [x, y] = pos(e);
      this.engine.command({cmd: "activeViewport", viewport: i});
      // A sound dropped into the world becomes an Audio Source playing it (one undo step).
      this.engine.command({cmd: "spawn", item: item || "audio_source", sound: sound || undefined, viewport: i, x, y});
    });
  }

  renderViewports(s) {
    const host = $("#viewports");
    const layout = s.prefs.layout;
    host.className = layout;
    const playing = s.play.state;
    for (const v of this.views) {
      const vs = s.viewports[v.index];
      const shown = layout === "quad" || (layout === "twoColumns" && v.index < 2) || (layout === "single" && vs.active);
      v.el.classList.toggle("hidden", !shown);
      v.el.classList.toggle("active", vs.active);
      v.el.classList.toggle("playing", vs.active && playing === "playing" && !s.play.ejected);
      v.el.classList.toggle("simulating", vs.active && playing === "simulating");
      v.type.textContent = vs.typeName;
      v.mode.textContent = vs.modeName;
      v.bar.style.visibility = vs.active && playing === "playing" && !s.play.ejected ? "hidden" : "";
      v.bar.querySelector('[data-vp="maximize"]').innerHTML = icon(layout === "single" ? "maximize" : "minimize");
      v.bar.querySelector('[data-vp="maximize"]').title = layout === "single" ? "Show four viewports" : "Maximise this viewport";
    }
  }

  /** The robot camera in the corner (drawn by the engine): its frame, what it does, its ToF grid. */
  renderRobot(r) {
    if (!r) { if (this.robotEl) this.robotEl.hidden = true; return; }
    const vp = this.views[r.viewport]?.el;
    if (!vp) return;
    if (!this.robotEl) {
      this.robotEl = document.createElement("div");
      this.robotEl.className = "robot-pip";
      this.robotEl.innerHTML = `<div class="rp-cam"><span class="rp-label">HEAD CAMERA</span><span class="rp-state"></span></div><div class="rp-tof"><canvas width="8" height="8"></canvas><span class="rp-label">TOF 8×8</span><span class="rp-near"></span></div>`;
    }
    if (this.robotEl.parentElement !== vp) vp.append(this.robotEl);
    this.robotEl.hidden = false;
    const cam = this.robotEl.querySelector(".rp-cam");
    cam.style.width = r.w + "px";
    cam.style.height = r.h + "px";
    const tof = this.robotEl.querySelector(".rp-tof");
    tof.style.width = tof.style.height = Math.round(Math.min(r.h / 2, 160)) + "px";
    this.robotEl.querySelector(".rp-state").textContent = r.fallen ? "Fallen" : r.state || "";
    this.robotEl.querySelector(".rp-near").textContent = r.nearest >= 0 ? `${r.nearest.toFixed(2)} m` : "clear";
    const g = this.robotEl.querySelector("canvas").getContext("2d");
    const img = g.createImageData(8, 8);
    for (let i = 0; i < 64; i++) {
      const z = r.zones[i], d = r.ranges[i];
      // obstacle: red, brighter when near; floor: green; too close: yellow; nothing: dark
      const near = d > 0 ? Math.max(0, Math.min(1, 1 - d / 2)) : 0;
      const c = z === 3 ? [90 + 165 * near, 40 * (1 - near), 40 * (1 - near)] : z === 2 ? [30, 90, 45] : z === 1 ? [230, 200, 40] : [24, 26, 30];
      img.data.set([c[0], c[1], c[2], 255], i * 4);
    }
    g.putImageData(img, 0, 0);
  }

  renderStatus(st) {
    if (st?.logicTrace && this.state?.logic?.open) this.logic.setTrace(st.logicTrace);
    this.renderRobot(st?.robot);
    if (!st || !this.state) return;
    const s = this.state;
    // Frame rate measured here (the page's own frames).
    const now = performance.now();
    this.fpsTimes = (this.fpsTimes || []).filter(t => now - t < 1000);
    this.fpsTimes.push(now);
    const fps = this.fpsTimes.length;
    if (fps !== this.fpsShown) { this.fpsShown = fps; $("#fps").textContent = `${fps} fps`; }
    // Simulation stats.
    // Output meter (master peak with a falling hold per channel, dB scale).
    if (st.audio) {
      const bars = $("#meter").children;
      for (let c = 0; c < 2; c++) {
        const db = 20 * Math.log10(Math.max(1e-5, (st.audio.peakHold ?? st.audio.peak)[c]));
        bars[c].style.width = `${Math.max(0, Math.min(100, (db + 60) / 60 * 100)).toFixed(0)}%`;
        bars[c].className = db > -1 ? "hot" : "";
      }
    }
    const netText = st.net ? `${st.net.role === "server" ? "Server" : "Client"} · ${st.net.players} player${st.net.players === 1 ? "" : "s"} · ${st.net.rtt.toFixed(0)} ms · ${st.net.loss.toFixed(1)}% loss · ↑${st.net.up.toFixed(0)} ↓${st.net.down.toFixed(0)} kbit/s   ` : "";
    $("#simStats").textContent = netText + (st.sim ? `${st.sim.bodies} bodies · ${st.sim.awake} awake · ${st.sim.joints} joints${st.sim.broken ? ` (${st.sim.broken} broken)` : ""} · ${st.sim.contacts} contacts` : "");
    for (const v of this.views) {
      const vs = s.viewports[v.index], vst = st.viewports?.[v.index];
      const active = vs.active;
      // HUD while playing.
      const clientHud = v.index >= 1 ? st.net?.clients?.[v.index - 1]?.hud : null;
      const showHud = clientHud ? true : (active || (st.net?.local && v.index === 0)) && st.hud && (st.play === "playing" || st.hud.status === "won" || st.hud.status === "lost");
      const html = clientHud ? hudHtml(clientHud) : showHud ? hudHtml(st.hud) : "";
      if (html !== v.hudKey) { v.hudKey = html; v.hud.innerHTML = html; v.hud.hidden = !html; }
      // Playing on a touch device: the game takes the whole screen (EXIT stops), like SpawnCity.
      if (v.index === 0) {
        const immersive = st.play === "playing" && !s.play.ejected && st.input?.device === "touch";
        if (immersive !== this.immersive) {
          this.immersive = immersive;
          $("#app").classList.toggle("touch-play", immersive);
          if (immersive) this.engine.command({cmd: "layout", layout: "single"});
        }
      }
      // Engine touch controls (only on the active viewport while playing on a touch device).
      if (active && st.touch) { (this.touchOverlay ||= new TouchOverlay(v.el)).attach(v.el); this.touchOverlay.update(st.touch); }
      else if (active) this.touchOverlay?.update(null);
      // Info line.
      let info = "";
      if (vst) {
        if (vs.type !== "perspective") info = `${vs.typeName}  ${vst.orthoWidth >= 1000 ? (vst.orthoWidth / 1000).toFixed(2) + " km" : vst.orthoWidth.toFixed(1) + " m"} wide`;
        else if (active && st.camera) info = `Speed ${st.camera.speed}   ${st.camera.position.map(n => n.toFixed(1)).join("  ")}`;
        if (vs.flags.stats && active) info += `\n${this.fpsShown || 0} fps${st.sim ? `  ${st.sim.bodies} bodies  ${st.sim.contacts} contacts  step ${st.sim.steps}` : ""}`;
      }
      if (st.play === "playing" && active && !s.play.ejected) info = "";
      if (info !== v.infoKey) { v.infoKey = info; v.info.textContent = info; }
      // Axis tripod (perspective, active view).
      const axesKey = active && vs.type === "perspective" && st.camera && !(st.play === "playing" && !s.play.ejected) ? `${st.camera.yaw},${st.camera.pitch}` : "";
      if (axesKey !== v.axesKey) {
        v.axesKey = axesKey;
        v.axes.innerHTML = axesKey ? axesSvg(st.camera.yaw, st.camera.pitch) : "";
      }
      // Marquee.
      if (vst?.marquee) {
        const [x, y, w, h] = vst.marquee;
        Object.assign(v.marquee.style, {left: x + "px", top: y + "px", width: w + "px", height: h + "px"});
        v.marquee.hidden = false;
      } else v.marquee.hidden = true;
      // Cursor and hints.
      const drag = vst?.drag || "none";
      v.el.classList.toggle("cursor-fly", drag === "fly");
      v.el.classList.toggle("cursor-pan", drag === "pan");
      v.el.classList.toggle("cursor-gizmo", drag === "gizmo");
      let hint = "";
      if (drag === "fly") hint = `Flying · WASD move · Q/E down/up · wheel speed ${st.camera?.speed ?? ""} · Shift faster · Z/C zoom`;
      else if (drag === "grab") hint = "Grabbing - release to throw";
      else if (active && st.play === "playing" && !st.captured && !s.play.ejected && st.hud?.status === "playing" && st.input?.device !== "touch") hint = "Click the viewport to control the game · Esc stops · F8 ejects";
      else if (active && st.play === "simulating") hint = s.play.ejected ? "Ejected · free camera · F8 possess · Esc stop" : "Simulating · Ctrl+drag grabs bodies · K keeps poses · Esc stops";
      if (hint !== v.hintKey) { v.hintKey = hint; v.hint.textContent = hint; v.hint.hidden = !hint; }
      const gz = active && st.gizmo ? (st.gizmo.delta ? `Δ ${st.gizmo.delta.map(n => n.toFixed(2)).join("  ")} m` : st.gizmo.angle != null ? `${st.gizmo.angle.toFixed(1)}°` : `× ${st.gizmo.factor.map(n => n.toFixed(2)).join("  ")}`) : "";
      if (gz !== v.gizmoKey) { v.gizmoKey = gz; v.gizmo.textContent = gz; v.gizmo.hidden = !gz; }
    }
    // Live transform in the details panel while a gizmo moves things.
    if (st.transform && st.gizmo) this.liveTransform(st.transform);
  }

  // ---- toolbar -----------------------------------------------------------------------
  initToolbar() {
    const fillSelect = (sel, values, fmt) => { sel.innerHTML = values.map(v => `<option value="${v}">${fmt(v)}</option>`).join(""); };
    fillSelect($("#gridSize"), [0.01, 0.05, 0.1, 0.25, 0.5, 1, 2, 5, 10, 50, 100], v => (v < 1 ? `${v * 100} cm` : `${v} m`));
    fillSelect($("#rotStep"), [1, 5, 10, 15, 30, 45, 90], v => `${v}°`);
    fillSelect($("#scaleStep"), [0.0625, 0.125, 0.25, 0.5, 1], v => `${v}`);
    fillSelect($("#camSpeed"), [1, 2, 3, 4, 5, 6, 7, 8], v => `${v}`);
    $("#toolbar").addEventListener("click", e => {
      const b = e.target.closest("button");
      if (!b) return;
      if (b.dataset.cmd) this.engine.command(JSON.parse(b.dataset.cmd));
      if (b.dataset.tool) this.engine.command({cmd: "tool", tool: b.dataset.tool});
      if (b.dataset.ui) this.uiAction({action: b.dataset.ui});
    });
    $("#spaceBtn").onclick = () => this.engine.command({cmd: "space"});
    $("#snapGrid").onclick = () => this.engine.command({cmd: "snap", grid: !this.state.snap.grid});
    $("#snapRot").onclick = () => this.engine.command({cmd: "snap", rotation: !this.state.snap.rotation});
    $("#snapScale").onclick = () => this.engine.command({cmd: "snap", scale: !this.state.snap.scale});
    $("#gridSize").onchange = e => this.engine.command({cmd: "snap", gridSize: +e.target.value, grid: true});
    $("#rotStep").onchange = e => this.engine.command({cmd: "snap", rotationStep: +e.target.value, rotation: true});
    $("#scaleStep").onchange = e => this.engine.command({cmd: "snap", scaleStep: +e.target.value, scale: true});
    $("#camSpeed").onchange = e => this.engine.command({cmd: "cameraSpeed", speed: +e.target.value});
    $("#playBtn").onclick = () => this.engine.command({cmd: "play", mode: "play"});
    $("#simBtn").onclick = () => this.engine.command({cmd: "play", mode: "simulate"});
    $("#pauseBtn").onclick = () => this.engine.command({cmd: "pause"});
    $("#stepBtn").onclick = () => this.engine.command({cmd: "stepFrame"});
    $("#stopBtn").onclick = () => this.engine.command({cmd: "stop"});
    $("#ejectBtn").onclick = () => this.engine.command({cmd: "eject"});
    $("#addBtn").onclick = e => {
      const r = e.currentTarget.getBoundingClientRect();
      this.openMenu(this.catalog.map(g => ({label: g.label, submenu: g.items.map(it => ({label: it.label, icon: it.icon, cmd: {cmd: "spawn", item: it.id}}))})), r.left, r.bottom + 2);
    };
    $("#layoutBtn").onclick = e => {
      const r = e.currentTarget.getBoundingClientRect();
      const l = this.state.prefs.layout;
      this.openMenu([{header: "Viewport layout"}, {label: "1 viewport", icon: "layout1", checked: l === "single", cmd: {cmd: "layout", layout: "single"}},
                     {label: "2 viewports", icon: "layout2", checked: l === "twoColumns", cmd: {cmd: "layout", layout: "twoColumns"}},
                     {label: "4 viewports", icon: "layout", checked: l === "quad", cmd: {cmd: "layout", layout: "quad"}}], r.left, r.bottom + 2);
    };
    // Menu bar.
    $("#menubar").innerHTML = [["file", "File"], ["edit", "Edit"], ["window", "Window"], ["play", "Play"], ["help", "Help"]]
      .map(([id, label]) => `<button class="mb" data-menu="${id}">${label}</button>`).join("");
    const openBar = b => {
      this.closeMenus();
      b.classList.add("open");
      this.menubarOpen = b;
      const r = b.getBoundingClientRect();
      this.openMenu(this.state.menus[b.dataset.menu], r.left, r.bottom + 1, true);
    };
    $("#menubar").addEventListener("pointerdown", e => {
      const b = e.target.closest(".mb");
      if (!b) return;
      e.preventDefault();
      if (this.menubarOpen === b) this.closeMenus();
      else openBar(b);
    });
    $("#menubar").addEventListener("pointerover", e => {
      const b = e.target.closest(".mb");
      if (b && this.menubarOpen && this.menubarOpen !== b) openBar(b);
    });
  }

  renderToolbar(s) {
    const editing = s.play.state === "editing";
    for (const b of $$("[data-tool]")) b.classList.toggle("on", s.tool === b.dataset.tool);
    $("#spaceBtn").innerHTML = icon(s.space === "world" ? "globe" : "local");
    $("#spaceBtn").title = s.space === "world" ? "World coordinates (Ctrl+` for local)" : "Local coordinates (Ctrl+` for world)";
    $("#snapGrid").classList.toggle("on", s.snap.grid);
    $("#snapRot").classList.toggle("on", s.snap.rotation);
    $("#snapScale").classList.toggle("on", s.snap.scale);
    const setSel = (sel, v) => {
      const el = $(sel);
      if (![...el.options].some(o => +o.value === v)) el.insertAdjacentHTML("beforeend", `<option value="${v}">${v}</option>`);
      el.value = String([...el.options].find(o => Math.abs(+o.value - v) < 1e-6)?.value ?? v);
    };
    setSel("#gridSize", s.snap.gridSize);
    setSel("#rotStep", s.snap.rotationStep);
    setSel("#scaleStep", s.snap.scaleStep);
    setSel("#camSpeed", s.prefs.cameraSpeed);
    $("#undoBtn").disabled = !editing || !s.history.canUndo;
    $("#redoBtn").disabled = !editing || !s.history.canRedo;
    $("#undoBtn").title = s.history.canUndo ? `Undo ${s.history.undo} (Ctrl+Z)` : "Undo (Ctrl+Z)";
    $("#redoBtn").title = s.history.canRedo ? `Redo ${s.history.redo} (Ctrl+Y)` : "Redo (Ctrl+Y)";
    $("#addBtn").disabled = !editing;
    const playing = s.play.state === "playing";
    $("#playBtn").classList.toggle("running", playing);
    $("#playBtn").disabled = playing;
    $("#playBtn").title = s.play.state === "simulating" ? "Possess the player (F8)" : "Play in the viewport (Alt+P)";
    $("#simBtn").classList.toggle("on", s.play.state === "simulating");
    $("#simBtn").disabled = !editing;
    $("#pauseBtn").disabled = editing;
    $("#pauseBtn").classList.toggle("on", s.play.paused);
    $("#pauseBtn").innerHTML = icon(s.play.paused ? "play" : "pause");
    $("#stepBtn").disabled = editing || !s.play.paused;
    const tp = $("#touchPause"), tpText = s.play.paused ? "▶ RESUME" : "❚❚ PAUSE";
    if (tp.textContent !== tpText) { tp.textContent = tpText; tp.setAttribute("aria-label", s.play.paused ? "Resume" : "Pause"); }
    $("#stopBtn").disabled = editing;
    $("#ejectBtn").disabled = editing;
    $("#ejectBtn").title = playing ? "Eject from the player (F8)" : "Possess the player (F8)";
    $("#levelName").textContent = s.level.name;
    $("#dirtyDot").classList.toggle("on", s.level.dirty);
    document.title = `${s.level.dirty ? "• " : ""}${s.level.name} - Kern Studio`;
    $("#statusText").textContent = s.statusText || "";
    $("#selInfo").textContent = s.selection.length ? `${s.selection.length} of ${s.level.actors} selected` : `${s.level.actors} actors`;
  }

  // ---- panels ---------------------------------------------------------------------------
  initPanels() {
    // Tabs.
    for (const t of $$("#detailsPanel .tab")) t.onclick = () => {
      for (const o of $$("#detailsPanel .tab")) o.classList.toggle("active", o === t);
      $("#details").hidden = t.dataset.tab !== "details";
      $("#worldSettings").hidden = t.dataset.tab !== "world";
    };
    this.initCompact();
    for (const t of $$("#drawer .tab")) t.onclick = () => this.drawerTab(t.dataset.dtab);
    $("#drawerClose").onclick = () => this.toggleDrawer(false);
    $("#drawerBtn").onclick = () => this.toggleDrawer();
    $("#logBtn").onclick = () => this.toggleDrawer("log");
    $("#drawerFilter").oninput = () => { this.renderCatalog(); this.panelKeys.sounds = ""; if (this.state) this.renderSounds(this.state.sounds); };
    // Outliner.
    const filter = $("#outlinerFilter");
    filter.oninput = () => this.engine.command({cmd: "filter", text: filter.value});
    $("#newFolderBtn").onclick = () => {
      if (!this.state.selection.length) { this.toast("Select actors to put them into a new folder"); return; }
      let n = 1, name = "New Folder";
      const taken = new Set(this.state.outliner.rows.filter(r => r.folder).map(r => r.folder));
      while (taken.has(name)) name = `New Folder ${++n}`;
      this.engine.command({cmd: "setFolder", folder: name});
    };
    this.initOutliner();
    // Details editors (delegated).
    this.initProps($("#details"));
    this.initProps($("#worldSettings"));
    // Console.
    const con = $("#console");
    con.addEventListener("keydown", e => {
      if (e.key === "Enter" && con.value.trim()) {
        this.consoleHistory.unshift(con.value);
        this.consoleIndex = -1;
        this.engine.command({cmd: "console", text: con.value.trim()});
        con.value = "";
      } else if (e.key === "ArrowUp") {
        this.consoleIndex = Math.min(this.consoleIndex + 1, this.consoleHistory.length - 1);
        if (this.consoleIndex >= 0) con.value = this.consoleHistory[this.consoleIndex];
        e.preventDefault();
      } else if (e.key === "ArrowDown") {
        this.consoleIndex = Math.max(this.consoleIndex - 1, -1);
        con.value = this.consoleIndex >= 0 ? this.consoleHistory[this.consoleIndex] : "";
        e.preventDefault();
      } else if (e.key === "Escape") con.blur();
      e.stopPropagation();
    });
    // Files.
    $("#fileOpen").onchange = async e => {
      const f = e.target.files[0];
      e.target.value = "";
      if (!f) return;
      try {
        const r = this.engine.command({cmd: "open", text: await f.text(), name: f.name});
        if (r?.ok) this.fileHandle = null;
        else this.toast(`"${f.name}" is not a Kern level`);
      } catch (error) {
        this.toast("Opening failed: " + error.message);
      }
    };
    $("#fileTexture").onchange = async e => {
      const f = e.target.files[0];
      e.target.value = "";
      if (f) this.importTexture(f);
    };
    $("#fileSound").onchange = async e => {
      const files = [...e.target.files];
      e.target.value = "";
      for (const f of files) await this.importSound(f);
    };
    $("#muteBtn").onclick = () => this.engine.command({cmd: "mute"});
    // Dropping files anywhere: levels open, images become textures.
    addEventListener("dragover", e => { if (e.dataTransfer.types.includes("Files")) e.preventDefault(); });
    addEventListener("drop", async e => {
      const f = e.dataTransfer.files?.[0];
      if (!f) return;
      e.preventDefault();
      if (/\.(klevel|json)$/i.test(f.name)) this.engine.command({cmd: "open", text: await f.text(), name: f.name});
      else if (f.type.startsWith("image/")) this.importTexture(f);
      else if (f.type.startsWith("audio/") || /\.(wav|mp3|ogg|oga|m4a|aac|flac|opus|webm)$/i.test(f.name)) for (const file of e.dataTransfer.files) await this.importSound(file);
    });
    // Leaving the page: write a pending autosave now (or drop a stale one). pagehide also fires
    // where beforeunload does not (mobile browsers, the tab is discarded, bfcache).
    addEventListener("pagehide", () => this.autosave(Infinity));
    addEventListener("visibilitychange", () => { if (document.visibilityState === "hidden") this.autosave(Infinity); });
    addEventListener("beforeunload", e => {
      this.autosave(Infinity);
      if (!this.engine.dirty()) return;
      // Reloading into a new build: the autosave carries the level over (offered on the next start).
      if (window.__kernReloadForUpdate) return;
      e.preventDefault();
      e.returnValue = "";
    });
  }

  render(s) {
    const logicOpen = !!s.logic?.open;
    $("#logicEditor").hidden = !logicOpen;
    $("#main").classList.toggle("logic-open", logicOpen);  // the Blueprint editor has its own panels
    // Closing the editor must not leave the keyboard focus on a hidden control (keys would vanish).
    if (!logicOpen && document.activeElement?.closest?.("#logicEditor")) document.activeElement.blur();
    $("#logicBtn")?.classList.toggle("on", logicOpen);
    if (logicOpen) this.logic.render(s.logic);
    this.renderToolbar(s);
    this.renderViewports(s);
    this.renderOutliner(s.outliner);
    this.renderDetails(s);
    this.renderWorld(s.world);
    this.renderTextures(s.textures);
    this.renderSounds(s.sounds);
    this.renderLog(s.log);
    const muted = !!s.sounds?.muted;
    const mb = $("#muteBtn");
    if (mb.dataset.muted !== String(muted)) {
      mb.dataset.muted = String(muted);
      mb.innerHTML = icon(muted ? "speakerOff" : "speaker");
      mb.classList.toggle("off", muted);
      mb.title = muted ? "Sound is off - click to turn it on" : "Sound on / off";
    }
  }

  // ---- outliner ----------------------------------------------------------------------------
  initOutliner() {
    const host = $("#outliner");
    host.addEventListener("pointerdown", e => {
      const row = e.target.closest(".row");
      if (e.target.closest(".eye, .tw, input")) return;
      if (!row) { if (e.button === 0) this.engine.command({cmd: "deselect"}); return; }
      if (e.button === 2) return;
      const id = +row.dataset.id;
      if (row.dataset.folder != null) {
        this.engine.command({cmd: "selectFolder", folder: row.dataset.folder, add: e.ctrlKey || e.metaKey});
        return;
      }
      if (e.shiftKey) this.engine.command({cmd: "selectRange", id});
      else if (e.ctrlKey || e.metaKey) this.engine.command({cmd: "select", ids: [id], toggle: true});
      else if (!row.classList.contains("sel") || this.state.selection.length <= 1) this.engine.command({cmd: "select", ids: [id]});
      else this.pendingSelect = id;  // keep the multi-selection for a drag; select on click
    });
    host.addEventListener("click", e => {
      const row = e.target.closest(".row");
      if (this.pendingSelect && row && +row.dataset.id === this.pendingSelect && !e.shiftKey && !e.ctrlKey) this.engine.command({cmd: "select", ids: [this.pendingSelect]});
      this.pendingSelect = 0;
      const eye = e.target.closest(".eye");
      if (eye) {
        const r = eye.closest(".row");
        if (r.dataset.folder != null) this.engine.command({cmd: r.classList.contains("hidden-actor") ? "show" : "hide", folder: r.dataset.folder, ids: []});
        else this.engine.command({cmd: r.classList.contains("hidden-actor") ? "show" : "hide", ids: [+r.dataset.id]});
        return;
      }
      const tw = e.target.closest(".tw");
      if (tw) { this.engine.command({cmd: "collapse", key: tw.closest(".row").dataset.key}); return; }
    });
    host.addEventListener("dblclick", e => {
      const row = e.target.closest(".row");
      if (!row || e.target.closest(".eye, .tw")) return;
      if (row.dataset.folder != null) this.startFolderRename(row);
      else this.engine.command({cmd: "focus", ids: [+row.dataset.id]});
    });
    host.addEventListener("contextmenu", e => {
      e.preventDefault();
      const row = e.target.closest(".row");
      if (row && row.dataset.id && !row.classList.contains("sel")) this.engine.command({cmd: "select", ids: [+row.dataset.id]});
      const entries = this.engine.command({cmd: "menu", name: "context"}).entries;
      if (row?.dataset.folder != null) entries.unshift(
        {label: "Rename Folder", run: () => this.startFolderRename(row)},
        {label: "Remove Folder (keep actors)", run: () => {
          this.engine.command({cmd: "selectFolder", folder: row.dataset.folder});
          this.engine.command({cmd: "setFolder", folder: ""});
        }},
        {separator: true});
      this.openMenu(entries, e.clientX, e.clientY);
    });
    // Drag rows to attach, move into folders or detach.
    host.addEventListener("dragstart", e => {
      const row = e.target.closest(".row");
      if (!row || row.dataset.folder != null) { e.preventDefault(); return; }
      const id = +row.dataset.id;
      const ids = this.state.selection.includes(id) ? this.state.selection : [id];
      this.dragIds = ids;
      e.dataTransfer.setData("text/x-kern-outliner", JSON.stringify(ids));
      e.dataTransfer.effectAllowed = "move";
    });
    host.addEventListener("dragover", e => {
      if (!e.dataTransfer.types.includes("text/x-kern-outliner")) return;
      e.preventDefault();
      for (const r of host.querySelectorAll(".drop-on")) r.classList.remove("drop-on");
      e.target.closest(".row")?.classList.add("drop-on");
    });
    host.addEventListener("dragleave", e => { if (e.target === host) for (const r of host.querySelectorAll(".drop-on")) r.classList.remove("drop-on"); });
    host.addEventListener("drop", e => {
      if (!e.dataTransfer.types.includes("text/x-kern-outliner")) return;
      e.preventDefault();
      for (const r of host.querySelectorAll(".drop-on")) r.classList.remove("drop-on");
      const ids = JSON.parse(e.dataTransfer.getData("text/x-kern-outliner"));
      const row = e.target.closest(".row");
      if (!row) this.engine.command({cmd: "moveTo", ids, folder: ""});
      else if (row.dataset.folder != null) this.engine.command({cmd: "moveTo", ids, folder: row.dataset.folder});
      else if (!ids.includes(+row.dataset.id)) this.engine.command({cmd: "moveTo", ids, parent: +row.dataset.id});
    });
  }

  renderOutliner(o) {
    const key = JSON.stringify(o);
    if (key === this.panelKeys.outliner || this.renaming) return;
    this.panelKeys.outliner = key;
    const host = $("#outliner");
    const html = o.rows.map(r => {
      const pad = 4 + r.depth * 14;
      const tw = r.children != null ? `<button class="tw${r.collapsed ? "" : " open"}" tabindex="-1">${icon("chevron")}</button>` : `<span class="tw"></span>`;
      if (r.folder != null) {
        return `<div class="row folder${r.hidden ? " hidden-actor" : ""}" data-folder="${esc(r.folder)}" data-key="f:${esc(r.folder)}">` +
          `<button class="eye" tabindex="-1" title="Show / hide">${icon(r.hidden ? "eyeOff" : "eye")}</button>` +
          `<span class="name" style="padding-left:${pad}px">${tw}<span class="ico">${icon("folder")}</span><span class="label">${esc(r.folder)}</span></span><span class="type">${r.children} actors</span></div>`;
      }
      const cls = ["row", r.selected && "sel", r.primary && "primary", r.hidden && "hidden-actor"].filter(Boolean).join(" ");
      return `<div class="${cls}" data-id="${r.id}" data-key="a:${r.id}" draggable="true">` +
        `<button class="eye" tabindex="-1" title="Show / hide (H)">${icon(r.hidden ? "eyeOff" : "eye")}</button>` +
        `<span class="name" style="padding-left:${pad}px">${tw}<span class="ico">${icon(r.icon)}</span><span class="label">${esc(r.name)}</span>${r.player ? `<span class="badge player" title="Play controls this actor">Player</span>` : ""}${r.locked ? `<span class="lock" title="Locked">${icon("lock")}</span>` : ""}</span>` +
        `<span class="type">${esc(r.type)}</span></div>`;
    }).join("");
    host.innerHTML = html || `<div class="empty dim" style="padding:16px">${o.filter ? "No actor matches" : "The level is empty - add actors from the toolbar or the content drawer"}</div>`;
    $("#outlinerFoot").textContent = `${o.total} actors${o.selected ? ` (${o.selected} selected)` : ""}`;
    const primary = host.querySelector(".primary");
    if (primary && this.lastPrimary !== primary.dataset.id) {
      this.lastPrimary = primary.dataset.id;
      const hr = host.getBoundingClientRect(), rr = primary.getBoundingClientRect();
      if (rr.top < hr.top || rr.bottom > hr.bottom) primary.scrollIntoView({block: "nearest"});
    }
  }

  startRename(id) {
    if (!id) return;
    const row = $(`#outliner .row[data-id="${id}"]`);
    if (!row) { this.engine.command({cmd: "dialog", name: "rename", args: {id}}); return; }
    const label = row.querySelector(".label");
    const old = label.textContent;
    this.renaming = true;
    label.innerHTML = `<input class="rename" value="${esc(old)}" spellcheck="false">`;
    const input = label.firstElementChild;
    input.focus();
    input.select();
    const finish = ok => {
      if (!this.renaming) return;
      this.renaming = false;
      if (ok && input.value.trim() && input.value !== old) this.engine.command({cmd: "rename", id, name: input.value.trim()});
      this.panelKeys.outliner = "";
      this.renderOutliner(this.engine.state().outliner);
    };
    input.addEventListener("keydown", e => { e.stopPropagation(); if (e.key === "Enter") finish(true); if (e.key === "Escape") finish(false); });
    input.addEventListener("blur", () => finish(true));
  }

  startFolderRename(row) {
    const label = row.querySelector(".label"), old = row.dataset.folder;
    this.renaming = true;
    label.innerHTML = `<input class="rename" value="${esc(old)}" spellcheck="false">`;
    const input = label.firstElementChild;
    input.focus();
    input.select();
    const finish = ok => {
      if (!this.renaming) return;
      this.renaming = false;
      if (ok && input.value.trim() && input.value !== old) this.engine.command({cmd: "renameFolder", from: old, to: input.value.trim()});
      this.panelKeys.outliner = "";
      this.renderOutliner(this.engine.state().outliner);
    };
    input.addEventListener("keydown", e => { e.stopPropagation(); if (e.key === "Enter") finish(true); if (e.key === "Escape") finish(false); });
    input.addEventListener("blur", () => finish(true));
  }

  // ---- details and world settings ----------------------------------------------------------------
  renderDetails(s) {
    const d = s.details;
    const key = JSON.stringify([d, s.actors.length, s.textures, s.sounds?.imported, this.detailsFilter]);
    if (key === this.panelKeys.details) return;
    const host = $("#details");
    if (host.contains(document.activeElement) && document.activeElement.matches("input:not([type=checkbox]):not([type=color]), textarea")) {
      // Do not rebuild under the cursor; refresh when the field loses focus.
      this.pendingDetails = true;
      return;
    }
    this.panelKeys.details = key;
    this.pendingDetails = false;
    if (d.empty) { host.innerHTML = `<div class="empty">${esc(d.empty)}</div>`; return; }
    const ctx = {actors: s.actors, textures: s.textures, sounds: s.sounds};
    const primary = s.outliner.rows.find(r => r.primary);
    let html = `<div class="d-head"><span class="ico">${icon(primary?.icon || "box")}</span>` +
      (d.count === 1 ? `<input class="txt" data-path="name" data-type="text" value="${esc(d.title)}" spellcheck="false" aria-label="Actor name">` : `<span class="count">${esc(d.title)}</span><span class="spacer"></span>`) +
      `<button class="d-add" data-logic title="Open this actor's logic graph (Blueprint)">${icon("logic")}Logic</button><button class="d-add" data-add>${icon("plus")}Add</button></div>`;
    html += `<div class="search d-search"><span data-icon="search">${icon("search")}</span><input data-filter placeholder="Search details" value="${esc(this.detailsFilter || "")}" spellcheck="false"></div>`;
    const filter = (this.detailsFilter || "").toLowerCase();
    for (const sec of d.sections) {
      const fields = sec.fields.filter(f => !filter || (f.label || "").toLowerCase().includes(filter) || sec.label.toLowerCase().includes(filter));
      if (!fields.length) continue;
      html += this.sectionHtml(sec, fields, ctx, !!filter);
    }
    host.innerHTML = html;
    const lb = host.querySelector("[data-logic]");
    if (lb) lb.onclick = () => { this.runCommand({cmd: "logicOpen", owner: primary?.id || 0}); };
    host.querySelector("[data-add]").onclick = e => {
      const r = e.currentTarget.getBoundingClientRect();
      this.openMenu(d.add.map(([type, label]) => ({label, cmd: {cmd: "addComponent", type}})), r.left, r.bottom + 2);
    };
    const fi = host.querySelector("[data-filter]");
    fi.oninput = () => { this.detailsFilter = fi.value; this.renderDetails(this.state); host.querySelector("[data-filter]")?.focus(); const el = host.querySelector("[data-filter]"); if (el) el.setSelectionRange(el.value.length, el.value.length); };
  }

  renderWorld(w) {
    const key = JSON.stringify([w, this.state.textures, this.state.sounds?.imported]);
    if (key === this.panelKeys.world) return;
    const host = $("#worldSettings");
    if (host.contains(document.activeElement) && document.activeElement.matches("input:not([type=checkbox]):not([type=color])")) { this.pendingWorld = true; return; }
    this.panelKeys.world = key;
    this.pendingWorld = false;
    host.innerHTML = w.sections.map(sec => this.sectionHtml(sec, sec.fields, {actors: [], textures: [], sounds: this.state.sounds}, false)).join("");
  }

  sectionHtml(sec, fields, ctx, forceOpen) {
    let body = "";
    for (const f of fields) {
      if (f.group) body += `<div class="grp">${esc(f.group)}</div>`;
      body += this.fieldHtml(f, ctx);
    }
    const closed = sec.closed && !forceOpen;
    return `<div class="sec${closed ? " closed" : ""}" data-sec="${esc(sec.id)}"><div class="sec-head"><span class="tw">${icon("chevron")}</span><span>${esc(sec.label)}</span>` +
      (sec.remove ? `<button class="ph" data-remove='${esc(JSON.stringify(sec.remove))}' title="Remove component">${icon("trash")}</button>` : "") +
      `</div><div class="sec-body">${body}</div></div>`;
  }

  fieldHtml(f, ctx) {
    (this.fieldInfo ??= new Map()).set(f.path, {value: f.value, reset: f.reset, mixed: f.mixed, label: f.label});
    const p = esc(f.path), mixed = f.mixed;
    const attrs = `data-path="${p}" data-type="${f.type}"` + (f.step != null ? ` data-step="${f.step}"` : "") + (f.min != null ? ` data-min="${f.min}"` : "") + (f.max != null ? ` data-max="${f.max}"` : "");
    const num = (v, extra = "", part = -1, partMixed = false) => {
      const shown = partMixed || (mixed && part < 0) ? "" : fmtNum(v, f.type === "integer");
      return `<input class="num${partMixed || (mixed && part < 0) ? " mixed" : ""}" ${attrs} data-part="${part}" value="${shown}" placeholder="${partMixed || mixed ? "Multiple" : ""}" ${extra} spellcheck="false" inputmode="decimal">`;
    };
    let editor = "";
    let scrub = false;
    switch (f.type) {
      case "number":
      case "integer":
        if (f.min != null && f.max != null && f.max - f.min <= 1 && f.min >= 0) {
          editor = `<div class="slider-row"><input type="range" ${attrs} min="${f.min}" max="${f.max}" step="${f.step || 0.01}" value="${f.value ?? 0}">${num(f.value)}</div>`;
        } else editor = num(f.value);
        scrub = true;
        break;
      case "vec3": {
        const v = f.value || [0, 0, 0];
        editor = [0, 1, 2].map(i => `<span class="axis ax${i}">${num(v[i], "", i, f.mixedParts?.[i])}</span>`).join("");
        if (f.uniformLock) editor += `<button class="lockbtn${this.uniformScale ? " on" : ""}" data-lock title="Lock proportions">${icon(this.uniformScale ? "lock" : "unlock")}</button>`;
        break;
      }
      case "bool":
        editor = `<input type="checkbox" class="chk${mixed ? " mixed" : ""}" ${attrs} ${f.value && !mixed ? "checked" : ""}>`;
        break;
      case "flags3": {
        const v = f.value || [false, false, false];
        editor = `<div class="flags">${["X", "Y", "Z"].map((a, i) => `<label><input type="checkbox" class="chk" ${attrs} data-part="${i}" ${v[i] ? "checked" : ""}>${a}</label>`).join("")}</div>`;
        break;
      }
      case "enum":
        editor = `<select class="sel" ${attrs}>${mixed ? `<option value="" selected>Multiple values</option>` : ""}${(f.choices || []).map(([val, label]) => `<option value="${esc(val)}" ${!mixed && val === f.value ? "selected" : ""}>${esc(label)}</option>`).join("")}</select>`;
        break;
      case "color": {
        const hex = typeof f.value === "string" ? f.value : "#ffffff";
        editor = `<div class="color"><input type="color" ${attrs} value="${esc(hex)}"><input class="txt" ${attrs} data-hex value="${mixed ? "" : esc(hex)}" placeholder="${mixed ? "Multiple" : ""}" spellcheck="false"></div>`;
        break;
      }
      case "actor": {
        const opts = [`<option value="0">World (fixed point)</option>`].concat(ctx.actors.map(([id, name]) => `<option value="${id}" ${id === f.value ? "selected" : ""}>${esc(name)}</option>`));
        editor = `<select class="sel" ${attrs}>${opts.join("")}</select>`;
        break;
      }
      case "texture": {
        const opts = [`<option value="">None</option>`].concat(ctx.textures.map(t => `<option value="${esc(t.id)}" ${t.id === f.value ? "selected" : ""}>${esc(t.name)}</option>`));
        editor = `<select class="sel" ${attrs}>${opts.join("")}</select><button class="ph" data-import-texture title="Import an image">${icon("plus")}</button>`;
        break;
      }
      case "sound": {
        const snd = ctx.sounds || {builtin: [], imported: []};
        const opt = (id, label) => `<option value="${esc(id)}" ${id === f.value ? "selected" : ""}>${esc(label)}</option>`;
        let opts = (f.auto ? opt("auto", "Automatic") : "") + opt("", "None");
        if (snd.imported?.length) opts += `<optgroup label="Imported">${snd.imported.map(x => opt(x.id, x.name)).join("")}</optgroup>`;
        const groups = new Map();
        for (const b of snd.builtin || []) { if (!groups.has(b.category)) groups.set(b.category, []); groups.get(b.category).push(b); }
        for (const [cat, list] of groups) opts += `<optgroup label="${esc(cat)}">${list.map(b => opt(b.id, b.name + (b.loop ? " (loop)" : ""))).join("")}</optgroup>`;
        const known = f.value === "" || f.value === "auto" || (snd.builtin || []).some(b => b.id === f.value) || (snd.imported || []).some(x => x.id === f.value);
        if (!known && !mixed) opts += opt(f.value, `Missing: ${f.value}`);
        if (mixed) opts = `<option value="__mixed" selected>Multiple values</option>` + opts;
        editor = `<select class="sel" ${attrs}>${opts}</select><button class="ph" data-preview-sound="${p}" title="Preview">${icon("play")}</button>`;
        break;
      }
      case "info":
        editor = `<span class="info">${esc(f.value)}</span>`;
        break;
      case "tags":
      case "text":
      default:
        editor = `<input class="txt${mixed ? " mixed" : ""}" ${attrs} value="${mixed ? "" : esc(f.value ?? "")}" placeholder="${mixed ? "Multiple values" : f.type === "tags" ? "comma, separated" : ""}" spellcheck="false">`;
        break;
    }
    const unit = f.unit ? ` <span class="dim">(${esc(f.unit)})</span>` : "";
    const canReset = f.reset != null && JSON.stringify(f.value) !== JSON.stringify(f.reset);
    const reset = f.reset != null ? `<button class="reset${canReset ? " show" : ""}" data-reset='${esc(JSON.stringify(f.reset))}' data-path="${p}" title="Reset to default">${icon("reset")}</button>` : "<span></span>";
    return `<div class="f"><label class="${scrub ? "scrub" : ""}" data-scrub="${scrub ? p : ""}" title="${esc(f.label)}${f.unit ? ` (${esc(f.unit)})` : ""}">${esc(f.label)}${unit}</label><div class="v">${editor}</div>${reset}</div>`;
  }

  initProps(host) {
    // One continuous interaction (scrub, slider drag, colour picker) carries one transaction token:
    // the engine merges its edits into a single undo step. Typed values and clicks are steps of their own.
    this.txn = null;
    this.txnSerial = 0;
    const beginTxn = kind => (this.txn ??= `${kind}-${++this.txnSerial}`);
    const endTxn = () => { this.txn = null; };
    this.beginTxn = beginTxn;
    this.endTxn = endTxn;
    const set = (path, value) => {
      const cmd = path.startsWith("world.") || path === "level.name" ? {cmd: "setWorld", path, value} : {cmd: "set", path, value};
      if (this.txn) cmd.transaction = this.txn;
      this.engine.command(cmd);
    };
    this.setProp = set;
    const vecOf = el => [...el.closest(".v").querySelectorAll(".num")].map(i => (i.value === "" ? null : evalNumber(i.value)));
    const commit = el => {
      const path = el.dataset.path, type = el.dataset.type;
      if (!path) return;
      if (type === "vec3") {
        const part = +el.dataset.part, field = this.fieldByPath(path);
        let v = vecOf(el);
        const base = field?.value || [0, 0, 0];
        if (this.uniformScale && el.closest(".v").querySelector("[data-lock]") && v[part] != null && base[part]) {
          const k = v[part] / base[part];
          v = base.map((b, i) => (i === part ? v[part] : b * k));
        }
        v = v.map((n, i) => (n == null || !isFinite(n) ? base[i] : n));
        set(path, v);
      } else if (type === "number" || type === "integer") {
        const n = evalNumber(el.value);
        if (el.value.trim() === "" || !isFinite(n)) return;
        set(path, type === "integer" ? Math.round(n) : n);
      } else if (type === "bool") set(path, el.checked);
      else if (type === "flags3") set(path, [...el.closest(".flags").querySelectorAll("input")].map(i => i.checked));
      else if (type === "enum") { if (el.value !== "") set(path, el.value); }
      else if (type === "actor") set(path, +el.value);
      else if (type === "texture") set(path, el.value);
      else if (type === "sound") { if (el.value !== "__mixed") set(path, el.value); }
      else if (type === "color") {
        let v = el.value.trim();
        if (/^[0-9a-f]{6}$/i.test(v)) v = "#" + v;
        if (/^#[0-9a-f]{6}$/i.test(v)) set(path, v.toLowerCase());
      } else set(path, el.value);
    };
    host.addEventListener("change", e => {
      const el = e.target;
      if (el.matches("[data-filter]")) return;
      if (el.type === "range") { const n = el.closest(".slider-row").querySelector(".num"); n.value = fmtNum(+el.value); }
      commit(el);
      if (el.type === "range" || el.type === "color") endTxn();  // the drag / picker session is over
    });
    host.addEventListener("input", e => {
      const el = e.target;
      if (el.type === "range") { beginTxn("slider"); el.closest(".slider-row").querySelector(".num").value = fmtNum(+el.value); commit(el); }
      if (el.type === "color") { beginTxn("color"); const t = el.closest(".color").querySelector("[data-hex]"); t.value = el.value; commit(el); }
    });
    host.addEventListener("keydown", e => {
      const el = e.target;
      if (!el.matches("input")) return;
      e.stopPropagation();
      if (e.key === "Enter") { el.blur(); }
      else if (e.key === "Escape") { this.panelKeys.details = this.panelKeys.world = ""; el.blur(); }
      else if ((e.key === "ArrowUp" || e.key === "ArrowDown") && el.classList.contains("num")) {
        const step = (+el.dataset.step || (el.dataset.type === "integer" ? 1 : 0.1)) * (e.shiftKey ? 10 : 1) * (e.ctrlKey ? 0.1 : 1);
        const n = (evalNumber(el.value) || 0) + (e.key === "ArrowUp" ? step : -step);
        el.value = fmtNum(n, el.dataset.type === "integer");
        commit(el);
        e.preventDefault();
      }
    });
    host.addEventListener("focusout", e => {
      const el = e.target;
      if (el.matches("input.num, input.txt") && !el.matches("[data-filter]") && el.dataset.dirty) { delete el.dataset.dirty; commit(el); }
      setTimeout(() => {
        if (host.contains(document.activeElement)) return;
        if (this.pendingDetails) { this.panelKeys.details = ""; this.renderDetails(this.state); }
        if (this.pendingWorld) { this.panelKeys.world = ""; this.renderWorld(this.state.world); }
      }, 0);
    });
    host.addEventListener("input", e => { if (e.target.matches("input.num, input.txt")) e.target.dataset.dirty = "1"; });
    host.addEventListener("click", e => {
      const head = e.target.closest(".sec-head");
      const rm = e.target.closest("[data-remove]");
      if (rm) { const r = JSON.parse(rm.dataset.remove); this.engine.command({cmd: "removeComponent", type: r.type, index: r.index}); return; }
      if (head) { const sec = head.parentElement; sec.classList.toggle("closed"); this.engine.command({cmd: "section", id: sec.dataset.sec, closed: sec.classList.contains("closed")}); return; }
      const reset = e.target.closest("[data-reset]");
      if (reset) { set(reset.dataset.path, JSON.parse(reset.dataset.reset)); return; }
      const lock = e.target.closest("[data-lock]");
      if (lock) { this.uniformScale = !this.uniformScale; lock.classList.toggle("on", this.uniformScale); lock.innerHTML = icon(this.uniformScale ? "lock" : "unlock"); return; }
      if (e.target.closest("[data-import-texture]")) $("#fileTexture").click();
      const pv = e.target.closest("[data-preview-sound]");
      if (pv) {
        const sel = pv.parentElement.querySelector("select");
        let value = sel?.value;
        if (value === "auto") value = this.autoSound(pv.dataset.previewSound);
        if (value && value !== "__mixed") this.engine.command({cmd: "previewSound", sound: value});
      }
    });
    // Scrubbing: drag a numeric label (or an unfocused number box) sideways.
    host.addEventListener("pointerdown", e => {
      const label = e.target.closest("label[data-scrub]:not([data-scrub=''])");
      const box = e.target.closest("input.num");
      if (!label && !(box && document.activeElement !== box)) return;
      const input = label ? label.parentElement.querySelector("input.num") : box;
      if (!input) return;
      this.scrub = {input, x: e.clientX, start: evalNumber(input.value) || 0, moved: false, id: e.pointerId, el: label || box};
      (label || box).setPointerCapture(e.pointerId);
      if (box) e.preventDefault();
    });
    host.addEventListener("pointermove", e => {
      const s = this.scrub;
      if (!s || e.pointerId !== s.id) return;
      const dx = e.clientX - s.x;
      if (!s.moved && Math.abs(dx) < 3) return;
      if (!s.moved) beginTxn("scrub");
      s.moved = true;
      const step = (+s.input.dataset.step || (s.input.dataset.type === "integer" ? 1 : 0.1)) * (e.shiftKey ? 10 : 1) * (e.ctrlKey || e.metaKey ? 0.1 : 1);
      let n = s.start + Math.round(dx / 2) * step;
      if (s.input.dataset.min != null) n = Math.max(+s.input.dataset.min, n);
      if (s.input.dataset.max != null) n = Math.min(+s.input.dataset.max, n);
      s.input.value = fmtNum(n, s.input.dataset.type === "integer");
      commit(s.input);
    });
    host.addEventListener("pointerup", e => {
      const s = this.scrub;
      if (!s || e.pointerId !== s.id) return;
      this.scrub = null;
      if (s.moved) endTxn();
      if (!s.moved && s.el === s.input) { s.input.focus(); s.input.select(); }
    });
    host.addEventListener("pointercancel", e => {
      if (this.scrub && e.pointerId === this.scrub.id) { this.scrub = null; endTxn(); }
    });
  }

  fieldByPath(path) {
    for (const sec of [...(this.state.details.sections || []), ...(this.state.world.sections || [])])
      for (const f of sec.fields) if (f.path === path) return f;
    return null;
  }

  liveTransform(t) {
    const host = $("#details");
    for (const [key, path] of [["position", "transform.position"], ["rotation", "transform.rotation"], ["scale", "transform.scale"]]) {
      const inputs = host.querySelectorAll(`input.num[data-path="${path}"]`);
      inputs.forEach((el, i) => { if (document.activeElement !== el) el.value = fmtNum(t[key][i]); });
    }
  }

  // ---- drawer ---------------------------------------------------------------------------
  toggleDrawer(tab) {
    const d = $("#drawer");
    if (tab === false) d.hidden = true;
    else if (typeof tab === "string") { d.hidden = false; this.drawerTab(tab); }
    else d.hidden = !d.hidden;
    $("#drawerBtn").classList.toggle("on", !d.hidden && this.dtab !== "log");
    $("#logBtn").classList.toggle("on", !d.hidden && this.dtab === "log");
    this.saveLayout();
  }

  drawerTab(tab) {
    this.dtab = tab;
    for (const t of $$("#drawer .tab")) t.classList.toggle("active", t.dataset.dtab === tab);
    $("#placePane").hidden = tab !== "place";
    $("#texturePane").hidden = tab !== "textures";
    $("#soundPane").hidden = tab !== "sounds";
    $("#logPane").hidden = tab !== "log";
    $("#drawerBtn").classList.toggle("on", tab !== "log");
    $("#logBtn").classList.toggle("on", tab === "log");
    if (tab === "log") { const b = $(".drawer-body"); b.scrollTop = b.scrollHeight; }
  }

  renderCatalog() {
    if (!this.catalog) return;
    const q = ($("#drawerFilter").value || "").toLowerCase();
    $("#placePane").innerHTML = this.catalog.map(g => {
      const items = g.items.filter(it => !q || it.label.toLowerCase().includes(q) || it.help.toLowerCase().includes(q));
      if (!items.length) return "";
      return `<div class="cat-group"><h4>${esc(g.label)}</h4><div class="cat-items">${items.map(it =>
        `<div class="tile" draggable="true" data-item="${esc(it.id)}" title="${esc(it.help)} - drag into a viewport or click"><div class="ti">${icon(it.icon)}</div><span>${esc(it.label)}</span></div>`).join("")}</div></div>`;
    }).join("");
    const pane = $("#placePane");
    if (!pane.dataset.wired) {
      pane.dataset.wired = "1";
      pane.addEventListener("dragstart", e => {
        const t = e.target.closest(".tile");
        if (!t) return;
        e.dataTransfer.setData("text/x-kern-actor", t.dataset.item);
        e.dataTransfer.effectAllowed = "copy";
      });
      pane.addEventListener("click", e => {
        const t = e.target.closest(".tile");
        if (t) this.engine.command({cmd: "spawn", item: t.dataset.item});
      });
    }
  }

  renderTextures(list) {
    const key = JSON.stringify(list);
    if (key === this.panelKeys.textures) return;
    this.panelKeys.textures = key;
    const html = list.map(t => {
      if (!this.textureThumbs.has(t.id)) this.textureThumbs.set(t.id, this.engine.textureSource(t.id));
      return `<div class="tex" data-tex="${esc(t.id)}"><img alt="" src="${esc(this.textureThumbs.get(t.id))}"><div><span title="${esc(t.name)}">${esc(t.name)}</span><span class="dim">${t.width}×${t.height}</span>` +
        `<button class="ph" data-remove-tex="${esc(t.id)}" title="Remove">${icon("trash")}</button></div></div>`;
    }).join("");
    $("#texturePane").innerHTML = `<div class="tex-grid">${html}<button class="tex-add" data-add-tex>${icon("plus")}<span>Import image</span></button></div>`;
    $("#texturePane").onclick = e => {
      if (e.target.closest("[data-add-tex]")) $("#fileTexture").click();
      const rm = e.target.closest("[data-remove-tex]");
      if (rm) { this.engine.command({cmd: "removeTexture", id: rm.dataset.removeTex}); this.textureThumbs.delete(rm.dataset.removeTex); }
    };
    $("#texturePane").ondblclick = e => {
      const t = e.target.closest("[data-tex]");
      if (t && this.state.selection.length) this.setProp("components.mesh.material.texture", t.dataset.tex);
    };
  }

  /** What "Automatic" means for a sound field (for the preview button). */
  autoSound(path) {
    const field = this.fieldByPath(path);
    if (path.endsWith("engineSound")) {
      const mode = this.fieldByPath("components.controller.mode")?.value;
      return mode === "vehicle" ? "builtin:engine" : mode === "hovercar" || mode === "flyer" ? "builtin:hover" : "";
    }
    if (path.endsWith("trigger.sound")) {
      const action = this.fieldByPath("components.trigger.action")?.value;
      return {goal: "builtin:goal", hazard: "builtin:hazard", checkpoint: "builtin:checkpoint", message: "builtin:confirm"}[action] || "";
    }
    return field?.value || "";
  }

  renderSounds(s) {
    if (!s) return;
    const q = ($("#drawerFilter").value || "").toLowerCase();
    const key = JSON.stringify([s.imported, s.previewing, q, s.ready]);
    if (key === this.panelKeys.sounds) return;
    this.panelKeys.sounds = key;
    const match = x => !q || x.name.toLowerCase().includes(q) || (x.category || "").toLowerCase().includes(q) || x.id.toLowerCase().includes(q);
    const tile = (x, imported) => `<div class="snd" draggable="true" data-sound="${esc(x.id)}" title="${esc(x.id)} - click to listen, double-click to use on the selection">` +
      `<button class="ph" data-play="${esc(x.id)}" title="Preview">${icon("play")}</button>` +
      `<span class="ti">${icon(x.loop ? "wave" : imported ? "note" : "speaker")}</span><span class="nm">${esc(x.name)}</span>` +
      `<span class="dim">${imported ? (x.duration ? x.duration.toFixed(1) + " s" : "") : x.loop ? "loop" : ""}</span>` +
      (imported ? `<button class="ph" data-remove-sound="${esc(x.id)}" title="Remove">${icon("trash")}</button>` : "") + `</div>`;
    let html = "";
    if (!s.ready) html += `<div class="note dim">Sound starts after the first click or key press (browser rule). ${this.audio ? "" : "This browser has no Web Audio: the editor works silently."}</div>`;
    const imported = (s.imported || []).filter(match);
    html += `<div class="cat-group"><h4>Imported</h4><div class="snd-list">${imported.map(x => tile(x, true)).join("")}` +
      `<button class="snd-add" data-add-sound>${icon("plus")}<span>Import audio (WAV, MP3, OGG, M4A, FLAC)</span></button></div></div>`;
    const groups = new Map();
    for (const b of s.builtin || []) if (match(b)) { if (!groups.has(b.category)) groups.set(b.category, []); groups.get(b.category).push(b); }
    for (const [cat, list] of groups) html += `<div class="cat-group"><h4>${esc(cat)} <span class="dim">built-in</span></h4><div class="snd-list">${list.map(x => tile(x, false)).join("")}</div></div>`;
    if (s.previewing) html = `<div class="note"><button class="ph" data-stop-sound>${icon("stop")}</button> Playing preview</div>` + html;
    const pane = $("#soundPane");
    pane.innerHTML = html;
    if (pane.dataset.wired) return;
    pane.dataset.wired = "1";
    pane.addEventListener("click", e => {
      if (e.target.closest("[data-add-sound]")) { $("#fileSound").click(); return; }
      if (e.target.closest("[data-stop-sound]")) { this.engine.command({cmd: "stopSound"}); return; }
      const rm = e.target.closest("[data-remove-sound]");
      if (rm) { this.engine.command({cmd: "removeSound", id: rm.dataset.removeSound}); return; }
      const t = e.target.closest("[data-sound]");
      if (t) this.engine.command({cmd: "previewSound", sound: t.dataset.sound});
    });
    pane.addEventListener("dblclick", e => {
      const t = e.target.closest("[data-sound]");
      if (!t || !this.state.selection.length) return;
      // Use it on the selection: an Audio Source's sound (added when missing).
      if (!this.state.details.sections?.some(sec => sec.id === "audio")) this.engine.command({cmd: "addComponent", type: "audio"});
      this.setProp("components.audio.sound", t.dataset.sound);
    });
    pane.addEventListener("dragstart", e => {
      const t = e.target.closest("[data-sound]");
      if (t) { e.dataTransfer.setData("text/x-kern-sound", t.dataset.sound); e.dataTransfer.effectAllowed = "copy"; }
    });
  }

  async importSound(file) {
    if (file.size > MAX_SOUND_BYTES) { this.toast(`${file.name} is larger than ${MAX_SOUND_BYTES >> 20} MB - shorten or compress it (OGG / MP3)`); return null; }
    const src = await new Promise((res, rej) => { const fr = new FileReader(); fr.onload = () => res(fr.result); fr.onerror = rej; fr.readAsDataURL(file); });
    let duration = 0;
    let dataUrl = src;
    if (!/^data:(audio|video)\//.test(dataUrl)) dataUrl = "data:audio/" + (file.name.split(".").pop() || "wav").toLowerCase() + ";base64," + src.slice(src.indexOf(",") + 1);
    if (this.audio) {
      try { duration = (await this.audio.decode(dataUrl)).duration; }
      catch { this.toast(`${file.name} is not an audio file this browser can play`); return null; }
    }
    const r = this.engine.command({cmd: "importSound", name: file.name.replace(/\.[^.]+$/, ""), src: dataUrl, duration});
    if (r?.ok) {
      this.toast(`Imported ${file.name}`);
      this.toggleDrawer("sounds");
    }
    return r?.id || null;
  }

  renderLog(log) {
    const last = log.at(-1);
    const key = `${log.length}:${last?.[1]}:${last?.[2]}`;
    if (key === this.panelKeys.log) return;
    this.panelKeys.log = key;
    const el = $("#log"), body = $(".drawer-body");
    const atEnd = body.scrollTop + body.clientHeight >= body.scrollHeight - 20;
    el.innerHTML = log.map(([level, text, t]) => `<div class="${level}"><span class="t">${fmtClock(t)}</span>${esc(text)}</div>`).join("");
    if (atEnd && this.dtab === "log") body.scrollTop = body.scrollHeight;
  }

  // ---- menus ------------------------------------------------------------------------------
  /**
   * A popup menu. With a mouse, submenus cascade on hover (beside the parent, never on top of it).
   * With touch, or where a submenu would not fit beside its parent, a tap on the entry drills
   * down in place instead: the submenu replaces the menu, with a back row at the top. A menu
   * therefore never opens under the finger that opened it, and every entry stays reachable.
   */
  openMenu(entries, x, y, fromBar = false, level = 0, back = null) {
    if (level === 0) this.closeMenus(fromBar);
    else while (this.menuStack.length > level) this.menuStack.pop().remove();
    if (!entries?.length) return;
    const m = document.createElement("div");
    m.className = "menu" + (back ? " drill" : "");
    m.setAttribute("role", "menu");
    m.innerHTML = (back ? `<div class="mi mback" data-back="1" role="menuitem"><span class="chk-mark">${icon("chevron")}</span><span>${esc(back.label)}</span><span class="k"></span><span class="sub"></span></div><div class="msep"></div>` : "") +
      entries.map((e, i) => {
        if (e.separator) return `<div class="msep"></div>`;
        if (e.header) return `<div class="mh">${esc(e.header)}</div>`;
        const mark = e.checked ? icon("check") : e.icon ? icon(e.icon) : "";
        return `<div class="mi${e.disabled ? " disabled" : ""}${e.submenu ? " has-sub" : ""}" data-i="${i}" role="menuitem"${e.submenu ? ' aria-haspopup="menu"' : ""}><span class="chk-mark">${mark}</span><span>${esc(e.label)}</span><span class="k">${esc(e.shortcut || "")}</span><span class="sub">${e.submenu ? icon("chevron") : ""}</span></div>`;
      }).join("");
    $("#menuLayer").append(m);
    const r = m.getBoundingClientRect();
    m.style.left = Math.max(4, Math.min(x, innerWidth - r.width - 4)) + "px";
    m.style.top = Math.max(4, Math.min(y, innerHeight - r.height - 4)) + "px";
    this.menuStack ??= [];
    this.menuStack.push(m);
    const closeChildren = () => { while (this.menuStack.length > level + 1) this.menuStack.pop().remove(); };
    // A cascading submenu beside the parent: right of it, else left of it; null when neither fits
    // without covering the parent menu.
    const cascade = (entry, it) => {
      const ir = it.getBoundingClientRect(), mr = m.getBoundingClientRect();
      for (const o of m.querySelectorAll(".mi.hover")) o.classList.remove("hover");
      it.classList.add("hover");
      this.openMenu(entry.submenu, mr.right - 2, ir.top - 4, false, level + 1);
      const sub = this.menuStack[level + 1], sr = sub?.getBoundingClientRect();
      if (!sub) return false;
      if (mr.right - 2 + sr.width <= innerWidth - 4) return true;
      if (mr.left + 2 - sr.width >= 4) { sub.style.left = (mr.left + 2 - sr.width) + "px"; return true; }
      closeChildren();
      return false;
    };
    const drill = entry => {
      const mr = m.getBoundingClientRect();
      const label = entry.label;
      const parent = {entries, x: mr.left, y: mr.top, fromBar, back};
      closeChildren();
      m.remove();
      this.menuStack.splice(level, 1);
      this.openMenu(entry.submenu, mr.left, mr.top, fromBar, level, {label, ...parent});
      // The stack slot of the drilled menu keeps the level; the back row reopens the parent there.
    };
    m.addEventListener("pointerover", e => {
      if (e.pointerType !== "mouse") return;  // touch and pen: taps decide, not hover
      const it = e.target.closest(".mi");
      if (!it || it.dataset.back) return;
      const entry = entries[+it.dataset.i];
      for (const o of m.querySelectorAll(".mi.hover")) o.classList.remove("hover");
      if (entry.submenu && !entry.disabled) cascade(entry, it);
      else closeChildren();
    });
    // Remember how the menu was last pressed (click events do not say).
    let lastPointer = "mouse";
    m.addEventListener("pointerdown", e => { lastPointer = e.pointerType || "mouse"; });
    m.addEventListener("click", e => {
      const it = e.target.closest(".mi");
      if (!it) return;
      if (it.dataset.back) {
        const b = back;
        closeChildren();
        m.remove();
        this.menuStack.splice(level, 1);
        this.openMenu(b.entries, b.x, b.y, b.fromBar, level, b.back);
        return;
      }
      const entry = entries[+it.dataset.i];
      if (entry.disabled) return;
      if (entry.submenu) {
        // Touch: drill down. Mouse: the hover already cascaded it; if it could not, drill down.
        const open = this.menuStack.length > level + 1;
        if (lastPointer === "mouse" && open) return;
        if (lastPointer !== "mouse" || !cascade(entry, it)) drill(entry);
        return;
      }
      if (!entry.keepOpen) this.closeMenus();
      if (entry.run) entry.run();
      else if (entry.cmd) this.runCommand(entry.cmd);
      if (entry.keepOpen) {
        // Toggle entries (show flags) stay open and flip their check mark.
        it.querySelector(".chk-mark").innerHTML = entry.checked ? "" : icon("check");
        entry.checked = !entry.checked;
        if (entry.cmd && "value" in entry.cmd) entry.cmd.value = !entry.cmd.value;
      }
    });
    m.addEventListener("contextmenu", e => e.preventDefault());
  }

  /** Engine context menu for places without a menu of their own: text fields, details rows, the rest of the window. */
  fallbackMenu(target) {
    const entries = [];
    const field = target.closest?.("input, textarea");
    const editable = field && !field.readOnly && !field.disabled && /^(text|search|number|)$/.test(field.type || "");
    if (editable) {
      const sel = () => field.value.slice(field.selectionStart ?? 0, field.selectionEnd ?? 0);
      const insert = text => {
        field.focus();
        field.setRangeText(text, field.selectionStart ?? field.value.length, field.selectionEnd ?? field.value.length, "end");
        field.dispatchEvent(new Event("input", {bubbles: true}));
      };
      const hasSel = (field.selectionEnd ?? 0) > (field.selectionStart ?? 0);
      entries.push(
        {label: "Cut", shortcut: "Ctrl+X", disabled: !hasSel, run: () => { navigator.clipboard?.writeText(sel()).catch(() => {}); insert(""); }},
        {label: "Copy", shortcut: "Ctrl+C", disabled: !hasSel, run: () => navigator.clipboard?.writeText(sel()).catch(() => {})},
        {label: "Paste", shortcut: "Ctrl+V", run: () => navigator.clipboard?.readText().then(insert).catch(() => {})},
        {label: "Select All", shortcut: "Ctrl+A", run: () => { field.focus(); field.select(); }});
    }
    const row = target.closest?.(".f");
    const path = row?.querySelector("[data-path]")?.dataset.path || row?.querySelector("[data-scrub]")?.dataset.scrub;
    const info = path && this.fieldInfo?.get(path);
    if (info) {
      if (entries.length) entries.push({separator: true});
      entries.push({header: info.label || path},
        {label: "Reset to Default", icon: "reset", disabled: info.reset == null || JSON.stringify(info.reset) === JSON.stringify(info.value),
         run: () => { this.engine.command({cmd: "set", path, value: info.reset}); this.flushUi(); }},
        {label: "Copy Value", disabled: info.mixed, run: () => { this.fieldClipboard = info.value; navigator.clipboard?.writeText(JSON.stringify(info.value)).catch(() => {}); }},
        {label: "Paste Value", disabled: this.fieldClipboard === undefined, run: () => { this.engine.command({cmd: "set", path, value: this.fieldClipboard}); this.flushUi(); }});
    }
    if (!entries.length) {
      const edit = this.state?.menus?.edit;
      if (edit?.length) return edit;
    }
    return entries;
  }

  runCommand(cmd) {
    if (cmd.cmd === "ui") this.uiAction({action: cmd.action});
    else if (cmd.cmd === "dialog") this.engine.command(cmd);
    else this.engine.command(cmd);
    this.flushUi();
  }

  closeMenus(keepBar = false) {
    for (const m of this.menuStack || []) m.remove();
    this.menuStack = [];
    if (!keepBar && this.menubarOpen) { this.menubarOpen.classList.remove("open"); this.menubarOpen = null; }
    else if (keepBar) for (const b of $$(".mb.open")) if (b !== this.menubarOpen) b.classList.remove("open");
  }

  // ---- dialogs ----------------------------------------------------------------------------
  openDialog(name, d) {
    if (!d) return;
    const layer = $("#dialogLayer");
    const values = {};
    let body = "";
    if (d.text) body += `<p>${esc(d.text)}</p>`;
    if (d.templates) {
      values.template = d.templates[0].id;
      body += `<div class="templates">${d.templates.map((t, i) => `<button class="tpl${i === 0 ? " on" : ""}" data-tpl="${esc(t.id)}"><b>${esc(t.label)}</b><span>${esc(t.help)}</span></button>`).join("")}</div>`;
    }
    if (d.table) body += `<table class="keys">${d.table.map(([k, v]) => `<tr><td>${esc(k)}</td><td>${esc(v)}</td></tr>`).join("")}</table>`;
    if (d.history) body += `<div class="hist">${d.history.map(h => `<button data-hist="${h.index}" class="${h.current ? "cur" : d.history.findIndex(x => x.current) < h.index ? "future" : ""}">${esc(h.label)}</button>`).join("")}</div>`;
    for (const f of d.fields || []) {
      values[f.path] = f.value;
      const attrs = `data-dpath="${esc(f.path)}"`;
      let ed;
      if (f.type === "bool") ed = `<input type="checkbox" class="chk" ${attrs} ${f.value ? "checked" : ""}>`;
      else if (f.type === "number" || f.type === "integer") ed = `<input class="num" ${attrs} value="${fmtNum(f.value, f.type === "integer")}">`;
      else ed = `<input class="txt" ${attrs} value="${esc(f.value)}" spellcheck="false">`;
      body += `<div class="f"><label>${esc(f.label)}${f.unit ? ` <span class="dim">(${esc(f.unit)})</span>` : ""}</label><div class="v">${ed}</div><span></span></div>`;
    }
    const buttons = (d.buttons || [{label: "Close", primary: true}]).map((b, i) => `<button class="btn${b.primary ? " primary" : ""}" data-btn="${i}">${esc(b.label)}</button>`).join("");
    layer.innerHTML = `<div class="dialog${d.table ? " wide" : ""}" role="dialog" aria-label="${esc(d.title)}"><h3>${esc(d.title)}</h3><div class="dbody">${body}</div><div class="dfoot">${buttons}</div></div>`;
    this.dialog = {name, d, values};
    const dlg = layer.firstElementChild;
    const close = () => { layer.innerHTML = ""; this.dialog = null; };
    const submit = () => {
      for (const el of dlg.querySelectorAll("[data-dpath]")) {
        const f = d.fields.find(x => x.path === el.dataset.dpath);
        values[el.dataset.dpath] = el.type === "checkbox" ? el.checked : f.type === "number" || f.type === "integer" ? evalNumber(el.value) : el.value;
      }
      if (d.id != null) values.id = d.id;
      close();
      if (d.buttons?.length > 1) { this.engine.command({cmd: "submitDialog", name, values}); this.flushUi(); }
    };
    this.dialog.submit = submit;
    this.dialog.close = close;
    dlg.addEventListener("click", e => {
      const tpl = e.target.closest("[data-tpl]");
      if (tpl) { for (const t of dlg.querySelectorAll(".tpl")) t.classList.toggle("on", t === tpl); values.template = tpl.dataset.tpl; }
      const h = e.target.closest("[data-hist]");
      if (h) { this.engine.command({cmd: "historyJump", index: +h.dataset.hist}); close(); }
      const b = e.target.closest("[data-btn]");
      if (b) { if (d.buttons?.[+b.dataset.btn]?.primary) submit(); else close(); }
    });
    dlg.addEventListener("dblclick", e => { if (e.target.closest("[data-tpl]")) submit(); });
    layer.onclick = e => { if (e.target === layer) close(); };
    dlg.querySelector("input")?.focus();
    dlg.querySelector("input")?.select?.();
  }

  // ---- files -------------------------------------------------------------------------------
  async save(as) {
    const r = this.engine.command({cmd: "save"});
    if (!r?.ok) return;
    try {
      if (window.showSaveFilePicker) {
        if (as || !this.fileHandle) {
          this.fileHandle = await window.showSaveFilePicker({suggestedName: r.file, types: [{description: "Kern level", accept: {"application/json": [".klevel"]}}]});
        }
        const w = await this.fileHandle.createWritable();
        await w.write(r.text);
        await w.close();
      } else {
        download(r.file, r.text, "application/json");
      }
      this.engine.command({cmd: "saved"});
      this.autosave(Infinity);  // the engine drops the stored copy if it was this level's
    } catch (error) {
      if (error?.name !== "AbortError") this.toast("Saving failed: " + error.message);
    }
  }

  async openFile() {
    if (window.showOpenFilePicker) {
      try {
        const [h] = await window.showOpenFilePicker({types: [{description: "Kern level", accept: {"application/json": [".klevel", ".json"]}}]});
        const f = await h.getFile();
        const r = this.engine.command({cmd: "open", text: await f.text(), name: f.name});
        if (r?.ok) this.fileHandle = h;
      } catch (error) {
        if (error?.name !== "AbortError") this.toast("Opening failed: " + error.message);
      }
    } else {
      // Android's file picker knows no ".klevel": with that filter it greys out every saved level
      // and only Back leads out. There (and wherever a picker cannot filter by extension) any file
      // may be picked; opening checks that it is a level.
      const input = $("#fileOpen");
      if (/Android/i.test(navigator.userAgent)) input.removeAttribute("accept");
      input.click();
    }
  }

  async importTexture(file) {
    const url = await new Promise((res, rej) => { const fr = new FileReader(); fr.onload = () => res(fr.result); fr.onerror = rej; fr.readAsDataURL(file); });
    const img = await new Promise((res, rej) => { const i = new Image(); i.onload = () => res(i); i.onerror = () => rej(Error("not an image")); i.src = url; });
    let src = url, w = img.naturalWidth, h = img.naturalHeight;
    if (Math.max(w, h) > MAX_TEXTURE_SIDE) {
      const k = MAX_TEXTURE_SIDE / Math.max(w, h);
      const c = document.createElement("canvas");
      c.width = Math.round(w * k); c.height = Math.round(h * k);
      c.getContext("2d").drawImage(img, 0, 0, c.width, c.height);
      src = c.toDataURL(file.type === "image/png" ? "image/png" : "image/jpeg", 0.9);
      w = c.width; h = c.height;
    }
    const r = this.engine.command({cmd: "importTexture", name: file.name.replace(/\.[^.]+$/, ""), src, width: w, height: h});
    if (r?.ok) this.toast(`Imported ${file.name}${this.state.selection.length ? " and applied it to the selection" : ""}`);
  }

  async gameHtml(levelText, title) {
    const sources = await moduleSources();
    return buildGameHtml({sources, wasmBase64: this.wasmBase64(), levelText, title});
  }

  async packageGame(levelText, title) {
    try {
      const html = await this.gameHtml(levelText, title);
      const file = (title || "game").replace(/[^\w.-]+/g, "_") + ".html";
      download(file, html, "text/html");
      const url = URL.createObjectURL(new Blob([html], {type: "text/html"}));
      this.toast(`Packaged ${file} (${(html.length / 1048576).toFixed(1)} MB)`, [{label: "Play it", primary: true, run: () => open(url, "_blank")}, {label: "Close", run: () => {}}], 12000);
    } catch (error) {
      this.toast("Packaging failed: " + error.message);
    }
  }

  // ---- input ---------------------------------------------------------------------------------
  initInput() {
    const editable = el => el && (el.isContentEditable || (el.matches?.("input, textarea, select") && !el.matches("input[type=checkbox], input[type=range], input[type=color]")));
    const onKey = (e, down) => {
      if (this.dialog && !$("#dialogLayer").firstElementChild) this.dialog = null;  // a dialog that is gone takes no keys
      if (this.dialog) {
        if (down && e.key === "Escape") { this.dialog.close(); e.preventDefault(); }
        else if (down && e.key === "Enter" && !e.target.matches?.("textarea")) { this.dialog.submit(); e.preventDefault(); }
        return;
      }
      if (this.menuStack?.length && down && e.key === "Escape") { this.closeMenus(); e.preventDefault(); return; }
      if (editable(e.target)) return;
      // Inside the logic editor only undo / redo / save reach the level (Delete, letters... are the graph's).
      if (this.state?.logic?.open && !e.target.closest?.("#logicEditor:not([hidden]) .lg-canvas")) {
        // The Blueprint editor covers the level: keys with no focused control are the graph's.
        this.logic.onKey(e, down);
        if (e.defaultPrevented) return;
      }
      if (this.state?.logic?.open) {
        const k = e.key.toLowerCase(), mod = e.ctrlKey || e.metaKey;
        if (!(mod && (k === "z" || k === "y" || k === "s")) && e.code !== "F1") return;
      }
      if (e.key === "Alt") e.preventDefault();  // keep the browser's menu bar out of Alt shortcuts
      if (down && e.code === "F1" && !e.shiftKey) { e.preventDefault(); this.engine.command({cmd: "dialog", name: "controls"}); this.flushUi(); return; }
      const r = this.engine.key(e.key, e.code, down, modifiers(e), e.repeat);
      if (r?.handled) e.preventDefault();
      this.flushUi();
    };
    addEventListener("keydown", e => onKey(e, true));
    addEventListener("keyup", e => onKey(e, false));
    addEventListener("blur", () => this.engine.blur());
    document.addEventListener("visibilitychange", () => { if (document.hidden) this.engine.blur(); });
    addEventListener("pointerdown", e => {
      if (!e.target.closest(".menu, #menubar")) this.closeMenus();
    }, true);
    // Right-click belongs to the engine: the browser's own context menu never opens in the editor.
    // Panels with their own menus (viewports, outliner, menus) handle the event first; everything
    // else gets the engine's fallback menu here.
    document.addEventListener("contextmenu", e => {
      if (e.defaultPrevented) return;
      e.preventDefault();
      const entries = this.fallbackMenu(e.target);
      if (entries.length) this.openMenu(entries, e.clientX, e.clientY);
    });
    // Pointer lock while playing: mouse movement looks around.
    // The browser takes the first Escape to end pointer lock (the page never sees that key).
    // Losing the lock while this tab keeps the focus therefore *is* that Escape: stop playing
    // right away, so one press is enough (switching windows only frees the mouse).
    document.addEventListener("pointerlockchange", () => {
      const locked = document.pointerLockElement === this.canvas;
      if (this.locked && !locked) {
        this.engine.command({cmd: "pointerCaptured", value: false});
        const play = this.state?.play?.state;
        if (!this.releasingLock && document.hasFocus() && (play === "playing" || play === "simulating")) this.engine.command({cmd: "stop"});
      }
      this.releasingLock = false;
      this.locked = locked;
    });
    document.addEventListener("mousemove", e => { if (this.locked) this.engine.look(e.movementX, e.movementY); });
    // While the mouse is locked the browser sends every click to the locked canvas, not to the
    // viewport under the (hidden) cursor: route the buttons to the active viewport (fire, aim...).
    const lockedButton = (type, e) => {
      if (document.pointerLockElement !== this.canvas) return;
      const vp = this.state?.viewports?.findIndex?.(v => v.active) ?? 0;
      const v = this.views[Math.max(0, vp)];
      const r = v.el.getBoundingClientRect();
      this.engine.pointer(v.index, type, r.width / 2, r.height / 2, {button: e.button, buttons: e.buttons, mods: modifiers(e)});
      this.flushUi();
      e.preventDefault();
    };
    this.canvas.addEventListener("pointerdown", e => lockedButton(POINTER.down, e));
    this.canvas.addEventListener("pointerup", e => lockedButton(POINTER.up, e));
    this.canvas.addEventListener("mousedown", e => { if (document.pointerLockElement === this.canvas) e.preventDefault(); });
  }

  /**
   * Phones and small windows: the viewport gets the whole main area, and the Outliner, Details and
   * World Settings open as one sheet over it (beside the viewport in landscape, from the bottom in
   * portrait) from the toolbar's panels button, one at a time behind large tabs.
   */
  initCompact() {
    const app = $("#app"), main = $("#main"), side = $("#side");
    const small = matchMedia("(max-width: 900px), (pointer: coarse) and (max-height: 600px)");
    const wide = matchMedia("(orientation: landscape)");
    // Play / Simulate (Pause / Stop while running) and the panels button stay one tap away on
    // phones: in the compact layout they move into a dock pinned to the toolbar's right edge,
    // which never scrolls away with the rest of the toolbar (docs/studio.md, "Handy").
    const dock = document.createElement("div");
    dock.id = "toolDock";
    dock.className = "dock";
    $("#toolbar").append(dock);
    const homes = ["#playGroup", "#panelsBtn"].map(sel => { const el = $(sel), mark = document.createComment(sel); el.before(mark); return [el, mark]; });
    const apply = () => {
      app.classList.toggle("compact", small.matches);
      if (small.matches) dock.append(...homes.map(([el]) => el));
      else for (const [el, mark] of homes) mark.after(el);
      app.classList.toggle("landscape", wide.matches);
      if (!small.matches) main.classList.remove("panels-open");
      $("#panelsBtn").classList.toggle("on", main.classList.contains("panels-open"));
    };
    small.addEventListener?.("change", apply);
    wide.addEventListener?.("change", apply);
    apply();
    this.showPane = pane => {
      side.dataset.pane = pane;
      for (const t of $$("#sideTabs .tab")) t.classList.toggle("active", t.dataset.pane === pane);
      if (pane !== "outliner") $(`#detailsPanel .tab[data-tab="${pane}"]`)?.click();
    };
    this.setPanels = open => {
      main.classList.toggle("panels-open", open);
      $("#panelsBtn").classList.toggle("on", open);
    };
    for (const t of $$("#sideTabs .tab")) t.onclick = () => this.showPane(t.dataset.pane);
    $("#sideClose").onclick = () => this.setPanels(false);
    $("#panelsBtn").onclick = () => {
      const open = !main.classList.contains("panels-open");
      if (open && side.dataset.pane === "details" && !this.state?.selection?.length) this.showPane("outliner");
      this.setPanels(open);
    };
  }

  initSplitters() {
    const drag = (el, onMove) => {
      el.addEventListener("pointerdown", e => {
        el.setPointerCapture(e.pointerId);
        el.classList.add("drag");
        const move = ev => onMove(ev);
        const up = () => { el.classList.remove("drag"); el.removeEventListener("pointermove", move); el.removeEventListener("pointerup", up); this.saveLayout(); };
        el.addEventListener("pointermove", move);
        el.addEventListener("pointerup", up);
        e.preventDefault();
      });
    };
    drag($("#sideSplit"), e => {
      const w = Math.max(240, Math.min(innerWidth * 0.6, innerWidth - e.clientX));
      document.documentElement.style.setProperty("--side", w + "px");
    });
    drag($("#panelSplit"), e => {
      const r = $("#side").getBoundingClientRect();
      const h = Math.max(80, Math.min(r.height - 120, e.clientY - r.top));
      document.documentElement.style.setProperty("--outliner", h + "px");
    });
  }

  saveLayout() {
    const root = document.documentElement.style;
    try {
      localStorage.setItem(LAYOUT_KEY, JSON.stringify({side: root.getPropertyValue("--side"), outliner: root.getPropertyValue("--outliner"), drawer: !$("#drawer").hidden, dtab: this.dtab}));
    } catch { /* ignore */ }
  }

  restoreLayout() {
    let l = null;
    try { l = JSON.parse(localStorage.getItem(LAYOUT_KEY) || "null"); } catch { /* ignore */ }
    this.dtab = "place";
    if (!l) return;
    if (l.side) document.documentElement.style.setProperty("--side", l.side);
    if (l.outliner) document.documentElement.style.setProperty("--outliner", l.outliner);
    if (l.drawer) { $("#drawer").hidden = false; }
    if (l.dtab) this.drawerTab(l.dtab);
  }

  toast(text, actions = [], ms = 5000) {
    const t = $("#toast");
    clearTimeout(this.toastTimer);
    t.innerHTML = `<span>${esc(text)}</span>` + actions.map((a, i) => `<button class="btn${a.primary ? " primary" : ""}" data-a="${i}">${esc(a.label)}</button>`).join("");
    t.hidden = false;
    t.onclick = e => {
      const b = e.target.closest("[data-a]");
      if (!b) return;
      t.hidden = true;
      actions[+b.dataset.a].run();
    };
    this.toastTimer = setTimeout(() => { t.hidden = true; }, ms);
  }
}

// ---------------------------------------------------------------------------
// helpers

function fmtNum(v, integer = false) {
  if (v == null || !isFinite(v)) return "";
  if (integer) return String(Math.round(v));
  const r = Math.round(v * 1e4) / 1e4;
  return String(Object.is(r, -0) ? 0 : r);
}

/** Numbers typed into fields may be simple arithmetic: "2*3", "10/4+1". */
function evalNumber(text) {
  const s = String(text).trim().replace(/,/g, ".");
  if (/^[-+]?(\d+\.?\d*|\.\d+)(e[-+]?\d+)?$/i.test(s)) return +s;
  if (!/^[\d\s.+\-*/()e]+$/i.test(s)) return NaN;
  try { const v = Function(`"use strict";return (${s});`)(); return typeof v === "number" ? v : NaN; } catch { return NaN; }
}

function fmtClock(t) {
  const s = Math.max(0, t || 0), m = Math.floor(s / 60);
  return `${String(m).padStart(2, "0")}:${(s - m * 60).toFixed(1).padStart(4, "0")}`;
}

function download(name, text, type) {
  const url = URL.createObjectURL(new Blob([text], {type}));
  const a = document.createElement("a");
  a.href = url;
  a.download = name;
  document.body.append(a);
  a.click();
  a.remove();
  setTimeout(() => URL.revokeObjectURL(url), 30000);
}

/** The world axes as seen by a camera with this yaw / pitch (degrees). */
function axesSvg(yaw, pitch) {
  const y = yaw * Math.PI / 180, p = pitch * Math.PI / 180;
  const f = [Math.cos(y) * Math.cos(p), Math.sin(y) * Math.cos(p), Math.sin(p)];
  const r = [Math.sin(y), -Math.cos(y), 0];
  const u = [r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0]];
  const axes = [[[1, 0, 0], "#e5484d", "X"], [[0, 1, 0], "#46a758", "Y"], [[0, 0, 1], "#3e7bfa", "Z"]]
    .map(([a, color, label]) => ({x: (a[0] * r[0] + a[1] * r[1] + a[2] * r[2]) * 15, y: -(a[0] * u[0] + a[1] * u[1] + a[2] * u[2]) * 15, depth: a[0] * f[0] + a[1] * f[1] + a[2] * f[2], color, label}))
    .sort((a, b) => b.depth - a.depth);
  return axes.map(a => `<line x1="0" y1="0" x2="${a.x.toFixed(1)}" y2="${a.y.toFixed(1)}" stroke="${a.color}" stroke-width="2" stroke-linecap="round"/>` +
    `<text x="${(a.x * 1.28).toFixed(1)}" y="${(a.y * 1.28 + 3).toFixed(1)}" fill="${a.color}" font-size="8" font-weight="700" text-anchor="middle" font-family="system-ui">${a.label}</text>`).join("");
}
