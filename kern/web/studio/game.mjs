// Kern game shell: the runtime page of games packaged with Kern Studio, plus
// the JavaScript wrapper over the Studio C ABI that the editor page shares.
//
// The engine module is the same one the editor runs (generated/kern_studio.wasm).
// A packaged game starts it in player-only mode, opens its level and plays it
// full screen: rendering, Box3D physics, controllers and game rules all run in
// C++ (kern::game::GameWorld). This file sizes the canvas, forwards input and
// draws the HUD text - nothing else.
import {loadKern} from "../kern.mjs";

export const POINTER = {down: 0, move: 1, up: 2, cancel: 3, wheel: 4, leave: 5};

/** Thin wrapper over kern/src/studio/api.cpp. */
export class StudioEngine {
  constructor(kern) {
    this.kern = kern;
    this.e = kern.exports;
  }
  withStrings(strings, fn) {
    const enc = this.kern.encoder, ptrs = [], args = [];
    try {
      for (const s of strings) {
        const bytes = enc.encode(s), p = bytes.length ? this.e.kern_alloc(bytes.length) : 0;
        if (bytes.length) this.kern.u8.set(bytes, p);
        ptrs.push(p);
        args.push(p, bytes.length);
      }
      return fn(...args);
    } finally {
      for (const p of ptrs) if (p) this.e.kern_free(p);
    }
  }
  text(ptr) { return ptr ? this.kern.string(ptr, this.e.kern_studio_result_length()) : null; }
  result(ptr) { const t = this.text(ptr); return t == null ? null : JSON.parse(t); }
  init(width, height, playerOnly) { return this.e.kern_studio_init(width, height, playerOnly ? 1 : 0) === 1; }
  resize(width, height) { this.e.kern_studio_resize(width, height); }
  frame(time) { this.e.kern_studio_frame(time); }
  revision() { return this.e.kern_studio_revision(); }
  command(obj) { return this.withStrings([JSON.stringify(obj)], (p, n) => this.result(this.e.kern_studio_command(p, n))); }
  state() { return this.result(this.e.kern_studio_state()); }
  status() { return this.result(this.e.kern_studio_status()); }
  takeUi() { return this.result(this.e.kern_studio_take_ui()) || []; }
  key(key, code, down, mods, repeat) {
    return this.withStrings([key, code], (kp, kn, cp, cn) => this.result(this.e.kern_studio_key(kp, kn, cp, cn, down ? 1 : 0, mods, repeat ? 1 : 0)));
  }
  pointer(viewport, type, x, y, {button = 0, buttons = 0, mods = 0, wheel = 0} = {}) {
    return this.result(this.e.kern_studio_pointer(viewport, type, x, y, button, buttons, mods, wheel));
  }
  look(dx, dy) { this.e.kern_studio_look(dx, dy); }
  blur() { this.e.kern_studio_blur(); }
  setViewport(i, x, y, w, h, cssW, cssH, dpr, visible) { this.e.kern_studio_set_viewport(i, x, y, w, h, cssW, cssH, dpr, visible ? 1 : 0); }
  takeAutosave(now) { return this.text(this.e.kern_studio_take_autosave(now)); }
  dirty() { return this.e.kern_studio_dirty() === 1; }
  textureSource(id) { return this.withStrings([id], (p, n) => this.text(this.e.kern_studio_texture_source(p, n))); }
  texturePixels(id, width, height, rgba) {
    const ptr = this.e.kern_alloc(Math.max(4, rgba.length));
    try {
      this.kern.u8.set(rgba, ptr);
      return this.withStrings([id], (p, n) => this.e.kern_studio_texture_pixels(p, n, width, height, ptr));
    } finally {
      this.e.kern_free(ptr);
    }
  }
  /** Decodes the level's texture images (data URLs) into RGBA for the engine. */
  pollTextures(maxSide = 2048) {
    this.decoding ??= new Set();
    const ids = this.result(this.e.kern_studio_pending_textures()) || [];
    for (const id of ids) {
      if (this.decoding.has(id)) continue;
      this.decoding.add(id);
      const img = new Image();
      img.onload = () => {
        this.decoding.delete(id);
        const scale = Math.min(1, maxSide / Math.max(img.naturalWidth, img.naturalHeight));
        const c = document.createElement("canvas");
        c.width = Math.max(1, Math.round(img.naturalWidth * scale));
        c.height = Math.max(1, Math.round(img.naturalHeight * scale));
        const ctx = c.getContext("2d", {willReadFrequently: true});
        ctx.drawImage(img, 0, 0, c.width, c.height);
        this.texturePixels(id, c.width, c.height, ctx.getImageData(0, 0, c.width, c.height).data);
      };
      img.onerror = () => { this.decoding.delete(id); this.texturePixels(id, 0, 0, new Uint8Array(4)); };
      img.src = this.textureSource(id);
    }
  }
  contextLost() { this.e.kern_studio_context_lost(); }
  contextRestored() { return this.e.kern_studio_context_restored() === 1; }
}

export const modifiers = e => (e.shiftKey ? 1 : 0) | (e.ctrlKey || e.metaKey ? 2 : 0) | (e.altKey ? 4 : 0);

export const toBase64 = bytes => {
  let s = "";
  for (let i = 0; i < bytes.length; i += 0x8000) s += String.fromCharCode.apply(null, bytes.subarray(i, i + 0x8000));
  return btoa(s);
};

export const escHtml = s => String(s ?? "").replace(/[&<>"']/g, c => ({"&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;"}[c]));

/** "83.4" -> "1:23.4" */
export function formatTime(seconds) {
  if (!(seconds >= 0)) return "-";
  const m = Math.floor(seconds / 60), s = seconds - m * 60;
  return m ? `${m}:${s.toFixed(1).padStart(4, "0")}` : `${s.toFixed(1)}`;
}

/**
 * HUD markup shared by the editor's Play mode and packaged games:
 * score, pickups, timer, speed and boost, the latest message, and the
 * win / lose banner.
 */
export function hudHtml(h) {
  if (!h) return "";
  const parts = [];
  if (h.title) parts.push(`<div class="k-hud-title">${escHtml(h.title)}</div>`);
  const stats = [];
  if (h.pickupsTotal) stats.push(`<div class="k-hud-stat"><b>${h.pickups}<i>/${h.pickupsTotal}</i></b><span>collected</span></div>`);
  stats.push(`<div class="k-hud-stat"><b>${Math.round(h.score)}</b><span>score</span></div>`);
  stats.push(h.remaining >= 0
    ? `<div class="k-hud-stat${h.remaining < 10 ? " k-warn" : ""}"><b>${formatTime(h.remaining)}</b><span>left</span></div>`
    : `<div class="k-hud-stat"><b>${formatTime(h.elapsed)}</b><span>time</span></div>`);
  if (h.best > 0) stats.push(`<div class="k-hud-stat"><b>${formatTime(h.best)}</b><span>best</span></div>`);
  parts.push(`<div class="k-hud-stats">${stats.join("")}</div>`);
  let html = `<div class="k-hud-top">${parts.join("")}</div>`;
  if (h.player) {
    const kmh = Math.round(h.speed * 3.6);
    html += `<div class="k-hud-speed"><b>${kmh}</b><span>km/h</span>` +
      (h.boost >= 0 ? `<div class="k-hud-boost"><div style="width:${Math.round(h.boost * 100)}%"></div></div>` : "") + `</div>`;
  }
  if (h.message && h.messageAge < 3.5 && h.status === "playing")
    html += `<div class="k-hud-message" style="opacity:${Math.min(1, (3.5 - h.messageAge) * 1.5).toFixed(2)}">${escHtml(h.message)}</div>`;
  if (h.status === "won" || h.status === "lost")
    html += `<div class="k-hud-banner k-${h.status}"><div>${h.status === "won" ? "Victory" : "Game over"}</div>` +
      `<small>${escHtml(h.message || "")}</small><button type="button" data-hud="restart">Play again</button><em>R to restart</em></div>`;
  return html;
}

export const HUD_STYLE = `.k-hud{position:absolute;inset:0;pointer-events:none;color:#f4f7fb;font:500 13px/1.25 system-ui,-apple-system,"Segoe UI",sans-serif;text-shadow:0 1px 3px rgba(0,0,0,.55);font-variant-numeric:tabular-nums}
.k-hud-top{position:absolute;left:18px;top:16px;display:flex;flex-direction:column;gap:8px}
.k-hud-title{font-size:12px;letter-spacing:.14em;text-transform:uppercase;opacity:.8}
.k-hud-stats{display:flex;gap:18px}
.k-hud-stat b{display:block;font-size:26px;font-weight:700;letter-spacing:-.01em}
.k-hud-stat b i{font-style:normal;opacity:.55;font-size:17px}
.k-hud-stat span{font-size:10px;letter-spacing:.12em;text-transform:uppercase;opacity:.65}
.k-hud-stat.k-warn b{color:#ff8f6b}
.k-hud-speed{position:absolute;right:20px;bottom:18px;text-align:right}
.k-hud-speed b{font-size:34px;font-weight:700}
.k-hud-speed span{margin-left:4px;font-size:11px;opacity:.7;letter-spacing:.1em;text-transform:uppercase}
.k-hud-boost{width:150px;height:4px;margin-top:6px;margin-left:auto;background:rgba(255,255,255,.18);border-radius:2px;overflow:hidden}
.k-hud-boost div{height:100%;background:linear-gradient(90deg,#4fe3c8,#9ff7e8)}
.k-hud-message{position:absolute;left:50%;top:22%;transform:translateX(-50%);padding:8px 16px;border-radius:999px;background:rgba(8,12,18,.55);font-size:15px;white-space:nowrap}
.k-hud-banner{position:absolute;left:50%;top:42%;transform:translate(-50%,-50%);min-width:280px;padding:22px 30px 18px;border-radius:12px;background:rgba(10,13,19,.82);text-align:center;pointer-events:auto;box-shadow:0 12px 40px rgba(0,0,0,.45)}
.k-hud-banner div{font-size:32px;font-weight:800;letter-spacing:-.01em}
.k-hud-banner.k-won div{color:#7ef0c4}
.k-hud-banner.k-lost div{color:#ff9a7a}
.k-hud-banner small{display:block;margin:6px 0 14px;font-size:14px;opacity:.85}
.k-hud-banner button{font:600 14px system-ui,sans-serif;padding:8px 20px;border:0;border-radius:6px;background:#2f7de1;color:#fff;cursor:pointer}
.k-hud-banner em{display:block;margin-top:8px;font-size:11px;font-style:normal;opacity:.55}`;

export const PLAYER_STYLE = `html,body{margin:0;height:100%;overflow:hidden;background:#0d1015;color:#eef3f8;font:14px system-ui,-apple-system,"Segoe UI",sans-serif;touch-action:none;user-select:none;-webkit-user-select:none}
#gl{position:fixed;inset:0;width:100%;height:100%;display:block;outline:none}
#hud{position:fixed;inset:0;pointer-events:none}
#hint{position:fixed;left:0;right:0;bottom:10px;text-align:center;opacity:.6;font-size:12px;pointer-events:none;text-shadow:0 1px 2px #000}
#error{position:fixed;inset:auto 12px 40px 12px;padding:10px 14px;border-radius:6px;background:#7a1f1f;display:none}
#loading{position:fixed;inset:0;display:grid;place-items:center;font-size:13px;letter-spacing:.2em;text-transform:uppercase;opacity:.6}
#stick{position:fixed;left:24px;bottom:36px;width:136px;height:136px;border-radius:50%;background:rgba(255,255,255,.08);border:2px solid rgba(255,255,255,.22);touch-action:none}
#stick div{position:absolute;left:43px;top:43px;width:50px;height:50px;border-radius:50%;background:rgba(255,255,255,.42)}
.tbtn{position:fixed;border-radius:50%;border:2px solid rgba(255,255,255,.28);background:rgba(255,255,255,.12);color:#fff;font:600 14px system-ui;touch-action:none}
#jump{right:28px;bottom:48px;width:86px;height:86px}
#boost{right:128px;bottom:36px;width:64px;height:64px}
#restart{right:20px;top:16px;width:44px;height:44px;font-size:20px}
[hidden]{display:none!important}
${HUD_STYLE}`;

export const PLAYER_BODY = `<canvas id="gl" tabindex="0" aria-label="Game view"></canvas><div id="hud" class="k-hud"></div><div id="loading">Loading</div>
<div id="hint">WASD / arrows move · Space jump · Shift boost · mouse look (click to capture, Esc frees) · P pause · R restart</div>
<div id="stick" hidden aria-label="Move"><div></div></div><button class="tbtn" id="jump" hidden>Jump</button><button class="tbtn" id="boost" hidden>Boost</button>
<button class="tbtn" id="restart" hidden aria-label="Restart">↻</button><div id="error" role="alert"></div>`;

/**
 * Joins ES module sources into one inline module: import lines and export
 * keywords are removed (the sources import each other only by name).
 */
export function inlineModules(...texts) {
  return texts.map(t => t.replace(/^import \{[^}]*\} from "[^"]+";\n/gm, "").replace(/^export (?=(const|let|function|async function|class) )/gm, "")).join("\n");
}

/**
 * Writes a standalone HTML game: engine module (base64), kern.mjs and this
 * file as one inline module, and the level. Plays offline from any host.
 */
export function buildGameHtml({sources, wasmBase64, levelText, title}) {
  const code = inlineModules(sources.kern, sources.game).replace(/<\/script/gi, "<\\/script");
  const boot = `\n{const t=id=>document.getElementById(id).textContent;` +
    `startGame({base64:t("kern-wasm").trim(),levelText:t("kern-level")}).catch(e=>{const b=document.getElementById("error");b.style.display="block";b.textContent="Game failed to start: "+e.message;console.error(e);});}`;
  return `<!doctype html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">` +
    `<meta name="generator" content="Kern Studio"><title>${escHtml(title || "Kern game")}</title><style>${PLAYER_STYLE}</style></head><body>${PLAYER_BODY}` +
    `<script type="application/json" id="kern-level">${levelText.replace(/</g, "\\u003c")}<\/script>` +
    `<script type="text/plain" id="kern-wasm">${wasmBase64}<\/script>` +
    `<script type="module">${code}${boot}<\/script></body></html>`;
}

/**
 * Plays one level full screen. `levelText` is .klevel JSON (or `template` names
 * a built-in level: orbRun, physicsLab, basic); the wasm comes from `bytes`,
 * `base64` or `url`.
 */
export async function startGame({levelText = null, template = null, bytes = null, base64 = null, url = null, canvas = document.getElementById("gl")} = {}) {
  const kern = await loadKern({bytes, base64, url});
  const engine = new StudioEngine(kern);
  const gl = canvas.getContext("webgl2", {antialias: true, alpha: false, powerPreference: "high-performance"});
  if (!gl) throw Error("WebGL2 is not available in this browser");
  kern.attachContext(canvas, gl);
  const coarse = matchMedia("(pointer:coarse)").matches;
  let dpr = 1;
  const size = () => {
    dpr = Math.min(devicePixelRatio || 1, coarse ? 1.5 : 2);
    const w = Math.max(1, Math.round(canvas.clientWidth * dpr)), h = Math.max(1, Math.round(canvas.clientHeight * dpr));
    if (canvas.width !== w || canvas.height !== h) { canvas.width = w; canvas.height = h; return true; }
    return false;
  };
  size();
  if (!engine.init(canvas.width, canvas.height, true)) throw Error(kern.lastError());
  const opened = template ? engine.command({cmd: "new", template}) : engine.command({cmd: "open", text: levelText, name: "game.klevel"});
  if (!opened?.ok) throw Error(engine.state()?.statusText || "the level could not be loaded");
  const started = engine.command({cmd: "play", mode: "play"});
  if (!started?.ok) throw Error(engine.state()?.statusText || "the level has errors");
  const $ = id => document.getElementById(id);
  $("loading").hidden = true;
  const hud = $("hud"), stick = $("stick"), knob = stick.firstElementChild;
  if (coarse) { stick.hidden = $("jump").hidden = $("boost").hidden = $("restart").hidden = false; $("hint").hidden = true; }
  canvas.addEventListener("webglcontextlost", e => { e.preventDefault(); engine.contextLost(); kern.resetTables(); });
  canvas.addEventListener("webglcontextrestored", () => { kern.attachContext(canvas, gl); engine.contextRestored(); });

  // Mouse: click captures the pointer for mouse look; touch drags orbit the camera.
  const send = (type, e) => engine.pointer(0, type, e.clientX, e.clientY, {button: e.button, buttons: e.buttons, mods: modifiers(e), wheel: e.deltaY ?? 0});
  canvas.addEventListener("pointerdown", e => {
    canvas.focus();
    try { canvas.setPointerCapture(e.pointerId); } catch { /* ignore */ }
    if (e.pointerType === "mouse" && document.pointerLockElement !== canvas) canvas.requestPointerLock?.()?.catch?.(() => {});
    send(POINTER.down, e);
    e.preventDefault();
  });
  canvas.addEventListener("pointermove", e => { if (document.pointerLockElement !== canvas) send(POINTER.move, e); });
  canvas.addEventListener("pointerup", e => send(POINTER.up, e));
  canvas.addEventListener("pointercancel", e => send(POINTER.cancel, e));
  canvas.addEventListener("wheel", e => { e.preventDefault(); send(POINTER.wheel, e); }, {passive: false});
  canvas.addEventListener("contextmenu", e => e.preventDefault());
  document.addEventListener("mousemove", e => { if (document.pointerLockElement === canvas) engine.look(e.movementX, e.movementY); });

  const key = (e, down) => {
    const r = engine.key(e.key, e.code, down, modifiers(e), e.repeat);
    if (r?.handled && !(e.code === "Escape")) e.preventDefault();
  };
  addEventListener("keydown", e => key(e, true));
  addEventListener("keyup", e => key(e, false));
  addEventListener("blur", () => engine.blur());

  // Touch joystick and buttons.
  let stickId = null;
  const stickMove = e => {
    const r = stick.getBoundingClientRect(), cx = r.left + r.width / 2, cy = r.top + r.height / 2, max = r.width / 2;
    let dx = (e.clientX - cx) / max, dy = (e.clientY - cy) / max;
    const len = Math.hypot(dx, dy);
    if (len > 1) { dx /= len; dy /= len; }
    knob.style.transform = `translate(${dx * max * 0.6}px,${dy * max * 0.6}px)`;
    engine.command({cmd: "gameInput", moveX: dx, moveY: -dy});
  };
  const stickEnd = () => { stickId = null; knob.style.transform = ""; engine.command({cmd: "gameInput", moveX: 0, moveY: 0}); };
  stick.addEventListener("pointerdown", e => { stickId = e.pointerId; stick.setPointerCapture(e.pointerId); stickMove(e); e.preventDefault(); });
  stick.addEventListener("pointermove", e => { if (e.pointerId === stickId) stickMove(e); });
  stick.addEventListener("pointerup", stickEnd);
  stick.addEventListener("pointercancel", stickEnd);
  const hold = (id, on, off) => {
    const b = $(id);
    b.addEventListener("pointerdown", e => { e.preventDefault(); engine.command(on); });
    b.addEventListener("pointerup", () => engine.command(off));
    b.addEventListener("pointercancel", () => engine.command(off));
  };
  hold("jump", {cmd: "gameInput", jump: true, up: 1}, {cmd: "gameInput", jump: false, up: 0});
  hold("boost", {cmd: "gameInput", boost: true}, {cmd: "gameInput", boost: false});
  $("restart").addEventListener("click", () => engine.command({cmd: "restart"}));
  hud.addEventListener("click", e => { if (e.target.closest("[data-hud=restart]")) engine.command({cmd: "restart"}); });

  let rectKey = "", hudKey = "", lastTextures = 0;
  const frame = time => {
    size();
    const k = `${canvas.width}x${canvas.height}`;
    if (k !== rectKey) {
      rectKey = k;
      engine.resize(canvas.width, canvas.height);
      engine.setViewport(0, 0, 0, canvas.width, canvas.height, canvas.clientWidth, canvas.clientHeight, dpr, true);
    }
    engine.frame(time);
    engine.takeUi();
    if (time - lastTextures > 250) { lastTextures = time; engine.pollTextures(); }
    const st = engine.status();
    const html = hudHtml(st?.hud);
    if (html !== hudKey) { hudKey = html; hud.innerHTML = html; }
    requestAnimationFrame(frame);
  };
  requestAnimationFrame(frame);
  canvas.focus();
  const api = {engine, get status() { return engine.status(); }, command: c => engine.command(c)};
  window.KernGame = api;
  return api;
}
