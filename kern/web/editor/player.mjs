// Kern player: the runtime shell for games made with the Kern editor.
//
// The same engine module as the editor (generated/kern_editor.wasm) starts in
// player-only mode, loads one .e3d scene and runs it full screen with the HUD,
// keyboard controls and an on-screen joystick for touch devices. Everything
// that moves is computed in C++ (kern::world::SceneRuntime on Box3D); this file
// only sizes the canvas, forwards input and draws the HUD text.
//
// Also exported from here: EditorEngine (the JS wrapper over the editor C ABI,
// shared with editor.mjs) and buildGameHtml (writes a standalone game page).
import {loadKern} from "../kern.mjs";

export const POINTER = {down: 0, move: 1, up: 2, cancel: 3, wheel: 4, leave: 5};

/** Thin wrapper over the editor's C ABI (kern/src/editor/editor_api.cpp). */
export class EditorEngine {
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
  text(ptr) { return ptr ? this.kern.string(ptr, this.e.kern_editor_result_length()) : null; }
  result(ptr) { const t = this.text(ptr); return t == null ? null : JSON.parse(t); }
  command(obj) { return this.withStrings([JSON.stringify(obj)], (p, n) => this.result(this.e.kern_editor_command(p, n))); }
  state() { return this.result(this.e.kern_editor_state()); }
  status() { return this.result(this.e.kern_editor_status()); }
  key(key, code, down, mods, repeat) {
    return this.withStrings([key, code], (kp, kn, cp, cn) => this.result(this.e.kern_editor_key(kp, kn, cp, cn, down ? 1 : 0, mods, repeat ? 1 : 0)));
  }
  pointer(view, type, x, y, {button = 0, buttons = 0, id = 1, kind = 0, mods = 0, wheel = 0} = {}) {
    return this.result(this.e.kern_editor_pointer(view, type, x, y, button, buttons, id, kind, mods, wheel));
  }
  imageSource(key) { return this.withStrings([key], (p, n) => this.text(this.e.kern_editor_image_source(p, n))); }
  imagePixels(key, width, height, rgba) {
    const ptr = this.e.kern_alloc(Math.max(4, rgba.length));
    try {
      this.kern.u8.set(rgba, ptr);
      return this.withStrings([key], (p, n) => this.e.kern_editor_image_pixels(p, n, width, height, ptr));
    } finally {
      this.e.kern_free(ptr);
    }
  }
  /** Decodes pending image planes (data URLs) into RGBA for the engine. */
  pollImages(maxSide = 2048) {
    const keys = this.result(this.e.kern_editor_pending_images()) || [];
    for (const key of keys) {
      const img = new Image();
      img.onload = () => {
        const scale = Math.min(1, maxSide / Math.max(img.naturalWidth, img.naturalHeight));
        const c = document.createElement("canvas");
        c.width = Math.max(1, Math.round(img.naturalWidth * scale));
        c.height = Math.max(1, Math.round(img.naturalHeight * scale));
        const ctx = c.getContext("2d", {willReadFrequently: true});
        ctx.drawImage(img, 0, 0, c.width, c.height);
        this.imagePixels(key, c.width, c.height, ctx.getImageData(0, 0, c.width, c.height).data);
      };
      img.onerror = () => this.imagePixels(key, 0, 0, new Uint8Array(4));
      img.src = this.imageSource(key);
    }
  }
}

export const toBase64 = bytes => {
  let s = "";
  for (let i = 0; i < bytes.length; i += 0x8000) s += String.fromCharCode.apply(null, bytes.subarray(i, i + 0x8000));
  return btoa(s);
};

const escHtml = s => String(s ?? "").replace(/[&<>"']/g, c => ({"&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;"}[c]));

export const PLAYER_PAGE_STYLE = `html,body{margin:0;height:100%;overflow:hidden;background:#11161c;color:#eef3f8;font:14px system-ui,-apple-system,Segoe UI,sans-serif;touch-action:none;user-select:none;-webkit-user-select:none}
#gl{position:fixed;inset:0;width:100%;height:100%;display:block;outline:none}
#hud{position:fixed;left:12px;top:10px;padding:6px 10px;border-radius:6px;background:rgba(10,14,20,.62);pointer-events:none;font-variant-numeric:tabular-nums}
#hud:empty{display:none}
#message{position:fixed;left:50%;top:40%;transform:translate(-50%,-50%);padding:18px 26px;border-radius:10px;background:rgba(10,14,20,.8);font-size:28px;font-weight:700;text-align:center}
#message small{display:block;margin-top:6px;font-size:14px;font-weight:400;opacity:.8}
#message button{margin-top:10px;font:inherit;font-size:15px;padding:6px 16px;border-radius:6px;border:0;background:#3a8ee6;color:#fff}
#hint{position:fixed;left:0;right:0;bottom:8px;text-align:center;opacity:.65;font-size:12px;pointer-events:none}
#error{position:fixed;inset:auto 12px 40px 12px;padding:10px;border-radius:6px;background:#7a1f1f;display:none}
#stick{position:fixed;left:22px;bottom:34px;width:130px;height:130px;border-radius:50%;background:rgba(255,255,255,.1);border:2px solid rgba(255,255,255,.25);touch-action:none}
#stick div{position:absolute;left:40px;top:40px;width:50px;height:50px;border-radius:50%;background:rgba(255,255,255,.45)}
#jump,#restart{position:fixed;border-radius:50%;border:2px solid rgba(255,255,255,.3);background:rgba(255,255,255,.14);color:#fff;font:600 15px system-ui;touch-action:none}
#jump{right:28px;bottom:44px;width:84px;height:84px}
#restart{right:28px;top:12px;width:44px;height:44px;font-size:20px}
[hidden]{display:none!important}`;

const PLAYER_BODY = `<canvas id="gl" tabindex="0" aria-label="Game view"></canvas><div id="hud"></div><div id="message" hidden></div>
<div id="hint">WASD / arrows move · Space jump · Shift/Q down · R restart · P pause · drag to look</div>
<div id="stick" hidden aria-label="Move joystick"><div></div></div><button id="jump" hidden>Jump</button><button id="restart" hidden aria-label="Restart">↻</button><div id="error" role="alert"></div>`;

/**
 * Joins ES module sources into one inline module: import lines and export
 * keywords are removed (the sources import each other only by name).
 */
export function inlineModules(...texts) {
  return texts.map(t => t.replace(/^import \{[^}]*\} from "[^"]+";\n/gm, "").replace(/^export (?=(const|let|function|async function|class) )/gm, "")).join("\n");
}

/**
 * Writes a standalone HTML game: engine module (base64), kern.mjs and this file
 * as one inline module, and the scene. No network access is needed to play it.
 */
export function buildGameHtml({sources, wasmBase64, sceneText, title}) {
  const code = inlineModules(sources.kern, sources.player).replace(/<\/script/gi, "<\\/script");
  const boot = `\n{const t=id=>document.getElementById(id).textContent;` +
    `startPlayer({base64:t("kern-wasm").trim(),sceneText:t("kern-scene")}).catch(e=>{const b=document.getElementById("error");b.style.display="block";b.textContent="Game failed to start: "+e.message;console.error(e);});}`;
  return `<!doctype html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">` +
    `<meta name="generator" content="Kern editor"><title>${escHtml(title || "Kern game")}</title><style>${PLAYER_PAGE_STYLE}</style></head><body>${PLAYER_BODY}` +
    `<script type="application/json" id="kern-scene">${sceneText.replace(/</g, "\\u003c")}<\/script>` +
    `<script type="text/plain" id="kern-wasm">${wasmBase64}<\/script>` +
    `<script type="module">${code}${boot}<\/script></body></html>`;
}

/**
 * Runs one scene full screen. `sceneText` is .e3d JSON; the wasm comes from
 * `bytes`, `base64` or `url`.
 */
export async function startPlayer({sceneText, bytes = null, base64 = null, url = null, canvas = document.getElementById("gl")} = {}) {
  const kern = await loadKern({bytes, base64, url});
  const engine = new EditorEngine(kern);
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
  if (!engine.e.kern_editor_init(canvas.width, canvas.height, 1)) throw Error(kern.lastError());
  engine.e.kern_editor_set_mobile(coarse ? 1 : 0);
  const load = sceneText === "__demo__" ? engine.command({cmd: "loadDemo", kind: "game"}) : engine.command({cmd: "loadDocument", text: sceneText, name: "game.e3d"});
  if (!load?.ok) throw Error(load?.error || "scene could not be loaded");
  const editorView = engine.state().views.find(v => v.type === "editor")?.id;
  const opened = engine.command({cmd: "openPlayer"});
  const player = engine.state().views.find(v => v.type === "player");
  if (!opened?.ok || !player) {
    const log = engine.result(engine.e.kern_editor_log(0)) || [];
    throw Error(log.filter(l => l.type === "error").map(l => l.message).join("; ") || "scene has errors");
  }
  if (editorView != null) engine.command({cmd: "closeView", view: editorView, force: true});
  const view = player.id;
  const $ = id => document.getElementById(id);
  const hud = $("hud"), message = $("message"), stick = $("stick"), knob = stick.firstElementChild, jump = $("jump"), restart = $("restart");
  if (coarse) { stick.hidden = jump.hidden = restart.hidden = false; $("hint").hidden = true; }
  canvas.addEventListener("webglcontextlost", e => { e.preventDefault(); engine.e.kern_editor_context_lost(); kern.resetTables(); });
  canvas.addEventListener("webglcontextrestored", () => { kern.attachContext(canvas, gl); engine.e.kern_editor_context_restored(); });

  // Pointer: drag orbits the camera (touch: one finger, since grab is off).
  const mods = e => (e.shiftKey ? 1 : 0) | (e.ctrlKey || e.metaKey ? 2 : 0);
  const kind = e => (e.pointerType === "touch" ? 1 : e.pointerType === "pen" ? 2 : 0);
  const send = (type, e) => engine.pointer(view, type, e.clientX, e.clientY, {
    button: e.button === 0 && type === POINTER.down ? 2 : e.button, buttons: e.buttons ? 2 : 0, id: e.pointerId ?? 1, kind: kind(e), mods: mods(e), wheel: e.deltaY ?? 0,
  });
  engine.command({cmd: "setTouchOrbit", view, on: true});
  canvas.addEventListener("pointerdown", e => { canvas.focus(); try { canvas.setPointerCapture(e.pointerId); } catch { /* ignore */ } send(POINTER.down, e); e.preventDefault(); });
  canvas.addEventListener("pointermove", e => send(POINTER.move, e));
  canvas.addEventListener("pointerup", e => send(POINTER.up, e));
  canvas.addEventListener("pointercancel", e => send(POINTER.cancel, e));
  canvas.addEventListener("wheel", e => { e.preventDefault(); send(POINTER.wheel, e); }, {passive: false});
  canvas.addEventListener("contextmenu", e => e.preventDefault());

  // Keyboard: the engine maps WASD/arrows/Space/Shift/R/P (kern::editor::Editor::key).
  const key = (e, down) => {
    const r = engine.key(e.key, e.code, down, mods(e), e.repeat);
    if (r?.handled) e.preventDefault();
  };
  addEventListener("keydown", e => key(e, true));
  addEventListener("keyup", e => key(e, false));
  addEventListener("blur", () => engine.e.kern_editor_blur());

  // Touch joystick and buttons.
  let stickId = null;
  const stickMove = e => {
    const r = stick.getBoundingClientRect(), cx = r.left + r.width / 2, cy = r.top + r.height / 2, max = r.width / 2;
    let dx = (e.clientX - cx) / max, dy = (e.clientY - cy) / max;
    const len = Math.hypot(dx, dy);
    if (len > 1) { dx /= len; dy /= len; }
    knob.style.transform = `translate(${dx * max * 0.6}px,${dy * max * 0.6}px)`;
    engine.command({cmd: "gameInput", view, moveX: dx, moveY: -dy});
  };
  const stickEnd = () => { stickId = null; knob.style.transform = ""; engine.command({cmd: "gameInput", view, moveX: 0, moveY: 0}); };
  stick.addEventListener("pointerdown", e => { stickId = e.pointerId; stick.setPointerCapture(e.pointerId); stickMove(e); e.preventDefault(); });
  stick.addEventListener("pointermove", e => { if (e.pointerId === stickId) stickMove(e); });
  stick.addEventListener("pointerup", stickEnd);
  stick.addEventListener("pointercancel", stickEnd);
  jump.addEventListener("pointerdown", e => { e.preventDefault(); engine.command({cmd: "gameInput", view, jump: true, up: 1}); });
  const jumpEnd = () => engine.command({cmd: "gameInput", view, jump: false, up: 0});
  jump.addEventListener("pointerup", jumpEnd);
  jump.addEventListener("pointercancel", jumpEnd);
  restart.addEventListener("click", () => engine.command({cmd: "restartGame", view}));

  let rectKey = "", hudKey = "", lastImages = 0;
  const frame = time => {
    size();
    const key = `${canvas.width}x${canvas.height}`;
    if (key !== rectKey) {
      rectKey = key;
      engine.e.kern_editor_resize(canvas.width, canvas.height);
      engine.e.kern_editor_set_view_rect(view, 0, 0, canvas.width, canvas.height, canvas.clientWidth, canvas.clientHeight, dpr, 1);
    }
    engine.e.kern_editor_frame(time);
    if (time - lastImages > 250) { lastImages = time; engine.pollImages(); }
    const st = engine.status();
    const h = st?.views?.find(v => v.id === view)?.hud || null;
    const k = JSON.stringify(h);
    if (k !== hudKey) {
      hudKey = k;
      if (!h) { hud.textContent = ""; message.hidden = true; }
      else {
        const parts = [];
        if (h.title) parts.push(`<b>${escHtml(h.title)}</b>`);
        parts.push(`Score ${h.score}`);
        if (h.pickupsTotal) parts.push(`${h.pickups}/${h.pickupsTotal} collected`);
        parts.push(h.remaining != null ? `${h.remaining.toFixed(1)} s left` : `${h.elapsed.toFixed(1)} s`);
        hud.innerHTML = parts.join(" · ");
        if (h.status === "won" || h.status === "lost") {
          message.hidden = false;
          message.innerHTML = `${escHtml(h.message || (h.status === "won" ? "You win!" : "Game over"))}<small>Press R to restart</small><button>Play again</button>`;
          message.querySelector("button").onclick = () => engine.command({cmd: "restartGame", view});
        } else message.hidden = true;
      }
    }
    requestAnimationFrame(frame);
  };
  requestAnimationFrame(frame);
  canvas.focus();
  const api = {engine, view, get status() { return engine.status(); }, command: c => engine.command(c)};
  window.KernPlayer = api;
  return api;
}
