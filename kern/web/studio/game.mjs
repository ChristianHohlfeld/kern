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
  /** A touch / pen finger in css px of the active viewport; true when the engine's touch controls took it. */
  touch(type, id, x, y, time) { return this.e.kern_studio_touch(type, id, x, y, time) === 1; }
  blur() { this.e.kern_studio_blur(); }
  setViewport(i, x, y, w, h, cssW, cssH, dpr, visible) { this.e.kern_studio_set_viewport(i, x, y, w, h, cssW, cssH, dpr, visible ? 1 : 0); }
  takeAutosave(now) { return this.text(this.e.kern_studio_take_autosave(now)); }
  takeAutosaveDiscard() { return this.e.kern_studio_take_autosave_discard() === 1; }
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
  /** Loads the robot's policy set (microduck/<file>.onnx next to the page; the dev page reads the
   *  repository's copy, so does the repository's single-file build in generated/). A file that fails is fetched again later; until then the move it
   *  drives is unavailable (the duck stands and says why). */
  async loadPolicies(bases = /\/kern\/web\/studio\//.test(location.pathname) ? ["../../third_party/microduck/"] : /\/generated\/[^/]*$/.test(location.pathname) ? ["../kern/third_party/microduck/"] : ["microduck/"]) {
    if (this.policiesLoading) return this.policiesLoading;
    // A file that failed is asked for again later (5 s, 10 s, 20 s ... at most every minute).
    const retry = this.policyRetry ??= new Map(), now = Date.now();
    const files = (this.result(this.e.kern_studio_pending_policies()) || []).filter(f => !retry.has(f) || retry.get(f).at <= now);
    if (!files.length) return;
    for (const f of files) retry.set(f, {at: Infinity, n: retry.get(f)?.n ?? 0});
    this.policiesLoading = Promise.all(files.map(async file => {
      for (const base of bases) {
        try {
          const r = await fetch(base + file);
          if (!r.ok) continue;
          const bytes = new Uint8Array(await r.arrayBuffer());
          const ptr = this.e.kern_alloc(Math.max(4, bytes.length));
          try {
            this.kern.u8.set(bytes, ptr);
            if (this.withStrings([file], (p, n) => this.e.kern_studio_load_policy(p, n, ptr, bytes.length)) === 1) return true;
          } finally {
            this.e.kern_free(ptr);
          }
        } catch { /* next base */ }
      }
      const n = retry.get(file).n + 1;
      retry.set(file, {at: Date.now() + Math.min(60000, 5000 * 2 ** (n - 1)), n});
      console.warn("Kern: robot policy " + file + " not loaded, trying again in " + Math.round(Math.min(60000, 5000 * 2 ** (n - 1)) / 1000) + " s");
      return false;
    })).finally(() => { this.policiesLoading = null; });
    return this.policiesLoading;
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
  // ---- audio ----
  audioInit(rate) { return this.e.kern_studio_audio_init(rate) === 1; }
  /** Renders queued built-in sounds for about `ms`; true when none is left. */
  audioPrepare(ms) { return this.e.kern_studio_audio_prepare(ms) === 1; }
  /** Interleaved stereo PCM of the next `frames` frames (a copy). */
  audioRender(frames) {
    const ptr = this.e.kern_studio_audio_render(frames);
    if (!ptr) return new Float32Array(frames * 2);
    return new Float32Array(this.kern.memory.buffer, ptr, frames * 2).slice();
  }
  soundSource(id) { return this.withStrings([id], (p, n) => this.text(this.e.kern_studio_sound_source(p, n))); }
  soundPcm(id, pcm, frames, channels, rate) {
    const ptr = this.e.kern_alloc(Math.max(4, pcm.length * 4));
    try {
      new Float32Array(this.kern.memory.buffer, ptr, pcm.length).set(pcm);
      return this.withStrings([id], (p, n) => this.e.kern_studio_sound_pcm(p, n, ptr, frames, channels, rate)) === 1;
    } finally {
      this.e.kern_free(ptr);
    }
  }
  soundFailed(id, reason) { this.withStrings([id, reason || ""], (p, n, rp, rn) => this.e.kern_studio_sound_failed(p, n, rp, rn)); }
  /** Decodes the level's imported sounds (data URLs) with Web Audio for the engine's mixer. */
  pollSounds(audio) {
    if (!audio?.context) return;
    this.soundDecoding ??= new Set();
    const ids = this.result(this.e.kern_studio_pending_sounds()) || [];
    for (const id of ids) {
      if (this.soundDecoding.has(id)) continue;
      this.soundDecoding.add(id);
      const src = this.soundSource(id) || "";
      audio.decode(src).then(d => {
        this.soundDecoding.delete(id);
        if (!this.soundPcm(id, d.pcm, d.frames, d.channels, d.rate)) this.soundFailed(id, "the decoded audio was rejected");
      }, error => {
        this.soundDecoding.delete(id);
        this.soundFailed(id, error?.message || "the browser could not decode it");
      });
    }
  }
  // ---- network ----
  netPush(from, bytes) {
    const ptr = this.e.kern_alloc(Math.max(4, bytes.length));
    try {
      this.kern.u8.set(bytes, ptr);
      this.e.kern_studio_net_push(from, ptr, bytes.length);
    } finally {
      this.e.kern_free(ptr);
    }
  }
  /** The next datagram the engine wants sent ({to, bytes}) or null. */
  netTake() {
    const n = this.e.kern_studio_net_take();
    if (!n) return null;
    const bytes = new Uint8Array(this.kern.memory.buffer, this.e.kern_studio_net_out(), n).slice();
    return {to: this.e.kern_studio_net_out_to(), bytes};
  }
  contextLost() { this.e.kern_studio_context_lost(); }
  contextRestored() { return this.e.kern_studio_context_restored() === 1; }
}

// ---------------------------------------------------------------------------
// Audio output: the engine's C++ mixer renders PCM, this moves it to the speakers.

/** AudioWorklet processor: plays queued chunks, reports what it consumed (frames). */
const WORKLET_SOURCE = `
class KernOutput extends AudioWorkletProcessor {
  constructor() {
    super();
    this.queue = [];
    this.offset = 0;
    this.consumed = 0;
    this.underruns = 0;
    this.blocks = 0;
    this.port.onmessage = e => { if (e.data === "flush") { this.queue.length = 0; this.offset = 0; } else this.queue.push(e.data); };
  }
  process(inputs, outputs) {
    const out = outputs[0], l = out[0], r = out[1] || out[0], n = l.length;
    let i = 0;
    while (i < n && this.queue.length) {
      const chunk = this.queue[0], frames = chunk.length >> 1;
      const take = Math.min(n - i, frames - this.offset);
      for (let k = 0; k < take; k++) { l[i + k] = chunk[(this.offset + k) * 2]; r[i + k] = chunk[(this.offset + k) * 2 + 1]; }
      i += take;
      this.offset += take;
      if (this.offset >= frames) { this.queue.shift(); this.offset = 0; }
    }
    if (i < n) { this.underruns++; for (; i < n; i++) { l[i] = 0; r[i] = 0; } }
    this.consumed += n;
    if (++this.blocks % 4 === 0) this.port.postMessage([this.consumed, this.underruns]);
    return true;
  }
}
registerProcessor("kern-output", KernOutput);
`;

/**
 * Speaker output for the engine mixer. Works everywhere Web Audio does: an
 * AudioWorklet where available (Blob or data: module), a ScriptProcessor
 * otherwise. Browsers start audio only after a user gesture, so the first
 * pointer / key / touch event resumes the context (iOS included; the audio
 * session is set to "playback" so the ring switch does not mute games).
 */
export class KernAudio {
  constructor(render, {latency = 0.07} = {}) {
    this.render = render;        // frames -> Float32Array (interleaved stereo)
    this.latency = latency;      // seconds kept queued ahead of the device
    this.context = null;
    this.node = null;
    this.mode = "none";
    this.written = 0;
    this.consumed = 0;
    this.underruns = 0;
    this.started = false;
  }
  static supported() { return typeof (globalThis.AudioContext || globalThis.webkitAudioContext) === "function"; }
  get rate() { return this.context?.sampleRate || 0; }
  get running() { return this.context?.state === "running"; }
  /** Creates the context (suspended until a gesture) and returns its sample rate. */
  create() {
    if (this.context) return this.rate;
    const Ctx = globalThis.AudioContext || globalThis.webkitAudioContext;
    if (!Ctx) return 0;
    try { if (navigator.audioSession) navigator.audioSession.type = "playback"; } catch { /* not supported */ }
    try { this.context = new Ctx({latencyHint: "interactive"}); } catch { try { this.context = new Ctx(); } catch { return 0; } }
    const unlock = () => {
      if (!this.context) return;
      if (this.context.state !== "running") this.context.resume().catch(() => {});
      if (!this.node) this.connect();
    };
    for (const type of ["pointerdown", "keydown", "touchend", "mousedown"]) addEventListener(type, unlock, {capture: true, passive: true});
    this.unlock = unlock;
    document.addEventListener("visibilitychange", () => { if (!document.hidden && this.context?.state === "interrupted") this.context.resume().catch(() => {}); });
    return this.rate;
  }
  async connect() {
    if (this.node || this.connecting || !this.context) return;
    this.connecting = true;
    const ctx = this.context;
    if (ctx.audioWorklet && globalThis.AudioWorkletNode) {
      const urls = [];
      try { urls.push(URL.createObjectURL(new Blob([WORKLET_SOURCE], {type: "text/javascript"}))); } catch { /* no blobs */ }
      urls.push("data:text/javascript;base64," + btoa(WORKLET_SOURCE));
      for (const url of urls) {
        try {
          await ctx.audioWorklet.addModule(url);
          const node = new AudioWorkletNode(ctx, "kern-output", {numberOfInputs: 0, numberOfOutputs: 1, outputChannelCount: [2]});
          node.port.onmessage = e => { this.consumed = e.data[0]; this.underruns = e.data[1]; };
          node.connect(ctx.destination);
          this.node = node;
          this.mode = "worklet";
          break;
        } catch { /* blocked by a content security policy: try the next way */ }
      }
    }
    if (!this.node && ctx.createScriptProcessor) {
      const node = ctx.createScriptProcessor(1024, 0, 2);
      node.onaudioprocess = e => {
        const l = e.outputBuffer.getChannelData(0), r = e.outputBuffer.getChannelData(1), n = l.length;
        const pcm = this.render(n);
        for (let i = 0; i < n; i++) { l[i] = pcm[i * 2]; r[i] = pcm[i * 2 + 1]; }
      };
      node.connect(ctx.destination);
      this.node = node;
      this.mode = "script";
    }
    this.connecting = false;
    this.started = !!this.node;
  }
  /** Keeps the worklet's queue `latency` seconds ahead; call every frame (and from a timer). */
  pump() {
    if (this.mode !== "worklet" || !this.running) return;
    const rate = this.rate, target = Math.ceil(this.latency * rate / 128) * 128;
    let queued = this.written - this.consumed;
    if (queued < -rate) { this.written = this.consumed; queued = 0; }  // after a long stall: start over
    let guard = 0;
    while (queued < target && guard++ < 16) {
      const frames = Math.min(2048, Math.max(128, target - queued));
      const pcm = this.render(frames);
      this.node.port.postMessage(pcm, [pcm.buffer]);
      this.written += frames;
      queued += frames;
    }
  }
  /** Decodes a data: URL (any format the browser plays) to interleaved PCM (1 or 2 channels). */
  async decode(src) {
    const comma = src.indexOf(",");
    if (!src.startsWith("data:") || comma < 0) throw Error("not a data URL");
    const meta = src.slice(5, comma), body = src.slice(comma + 1);
    let bytes;
    if (/;base64$/i.test(meta)) {
      const bin = atob(body);
      bytes = new Uint8Array(bin.length);
      for (let i = 0; i < bin.length; i++) bytes[i] = bin.charCodeAt(i);
    } else bytes = new TextEncoder().encode(decodeURIComponent(body));
    const buffer = await new Promise((resolve, reject) => {
      const p = this.context.decodeAudioData(bytes.buffer, resolve, reject);
      if (p?.then) p.then(resolve, reject);
    });
    const channels = Math.min(2, buffer.numberOfChannels), frames = buffer.length;
    const pcm = new Float32Array(frames * channels);
    if (buffer.numberOfChannels > 2) {
      // Down-mix surround to stereo (even channels left, odd right).
      const l = new Float32Array(frames), r = new Float32Array(frames);
      for (let c = 0; c < buffer.numberOfChannels; c++) {
        const d = buffer.getChannelData(c), t = c % 2 ? r : l;
        for (let i = 0; i < frames; i++) t[i] += d[i] / Math.ceil(buffer.numberOfChannels / 2);
      }
      for (let i = 0; i < frames; i++) { pcm[i * 2] = l[i]; pcm[i * 2 + 1] = r[i]; }
    } else {
      for (let c = 0; c < channels; c++) {
        const d = buffer.getChannelData(c);
        for (let i = 0; i < frames; i++) pcm[i * channels + c] = d[i];
      }
    }
    return {pcm, frames, channels, rate: buffer.sampleRate, duration: buffer.duration};
  }
}

/** Connects the engine's mixer to the speakers; returns the KernAudio (or null without Web Audio). */
export function startAudio(engine) {
  if (!KernAudio.supported()) return null;
  const audio = new KernAudio(frames => engine.audioRender(frames));
  const rate = audio.create();
  if (!rate || !engine.audioInit(rate)) return null;
  // Timer pump: keeps sound flowing when frames are slow (and while the tab is in the background),
  // and renders the level's built-in sounds between frames.
  setInterval(() => { audio.pump(); engine.audioPrepare(5); }, 20);
  return audio;
}

// ---------------------------------------------------------------------------
// Network transport: moves Kern Net datagrams (kern/src/net) between browsers.
// Kern's C++ stack does acks, resends, channels, the handshake and timeouts;
// this only delivers bytes, like UDP would.
//
//   "local"  BroadcastChannel: tabs of this browser on this machine, no server
//   "relay"  WebSocket to tools/net_relay.mjs: players on different machines

const randomPeerId = () => {
  const a = new Uint32Array(2);
  crypto.getRandomValues(a);
  return (a[0] & 0x1fffff) * 4294967296 + a[1] || 1;  // 53-bit, never 0
};

/** Host-side datagram transport for one engine. */
export class KernNet {
  constructor(engine) {
    this.engine = engine;
    this.kind = null;
    this.room = "";
    this.self = 0;
    this.host = 0;
    this.channel = null;
    this.socket = null;
    this.timer = 0;
    this.onstatus = null;
    this.sent = 0;
    this.received = 0;
  }
  get active() { return !!this.kind; }

  /** Opens the transport; resolves with {self, host} (host 0 = this peer hosts). */
  async open({kind = "local", room = "kern", hosting = false, url = ""} = {}) {
    this.close();
    this.kind = kind;
    this.room = room;
    if (kind === "local") {
      this.self = randomPeerId();
      this.channel = new BroadcastChannel("kern-net-" + room);
      this.channel.onmessage = e => this.onLocal(e.data);
      if (hosting) {
        this.host = this.self;
        this.announce = setInterval(() => this.channel?.postMessage({hello: this.self}), 500);
        this.channel.postMessage({hello: this.self});
      } else {
        this.host = await new Promise((resolve, reject) => {
          const done = id => { clearTimeout(t); this.channel.removeEventListener("message", listen); resolve(id); };
          const listen = e => { if (e.data?.hello) done(e.data.hello); };
          const t = setTimeout(() => { this.channel?.removeEventListener("message", listen); reject(Error(`no game is hosted in room "${room}" in this browser`)); }, 4000);
          this.channel.addEventListener("message", listen);
          this.channel.postMessage({who: this.self});
        });
      }
    } else {
      const ws = new WebSocket(url);
      ws.binaryType = "arraybuffer";
      this.socket = ws;
      const hello = await new Promise((resolve, reject) => {
        ws.onerror = () => reject(Error("the relay " + url + " is not reachable"));
        ws.onopen = () => ws.send(JSON.stringify(hosting ? {host: room} : {join: room}));
        ws.onmessage = e => { if (typeof e.data === "string") resolve(JSON.parse(e.data)); };
      });
      if (!hosting && !hello.host) throw Error(`nobody hosts room "${room}" on the relay`);
      this.self = hello.id;
      this.host = hello.host;
      ws.onmessage = e => {
        if (typeof e.data === "string" || e.data.byteLength < 9) return;
        const view = new DataView(e.data);
        const from = view.getUint32(0, true) + view.getUint32(4, true) * 4294967296;
        this.deliver(from, new Uint8Array(e.data, 8));
      };
      ws.onclose = () => this.onstatus?.("The relay closed the connection");
    }
    this.timer = setInterval(() => this.flush(), 8);
    return {self: this.self, host: hosting ? 0 : this.host};
  }

  onLocal(m) {
    if (!m) return;
    if (m.who && this.host === this.self) this.channel.postMessage({hello: this.self});
    if (m.to === this.self && m.data) this.deliver(m.from, new Uint8Array(m.data));
  }

  deliver(from, bytes) {
    this.received++;
    this.engine.netPush(from, bytes);
  }

  /** Sends what the engine queued (called from a timer and every frame). */
  flush() {
    if (!this.kind) return;
    for (let guard = 0; guard < 256; guard++) {
      const d = this.engine.netTake();
      if (!d) break;
      this.sent++;
      if (this.channel) this.channel.postMessage({from: this.self, to: d.to, data: d.bytes.buffer}, [d.bytes.buffer]);
      else if (this.socket?.readyState === 1) {
        const out = new Uint8Array(8 + d.bytes.length);
        const view = new DataView(out.buffer);
        view.setUint32(0, d.to % 4294967296, true);
        view.setUint32(4, Math.floor(d.to / 4294967296), true);
        out.set(d.bytes, 8);
        this.socket.send(out);
      }
    }
  }

  close() {
    clearInterval(this.timer);
    clearInterval(this.announce);
    this.channel?.close();
    this.socket?.close();
    this.channel = this.socket = null;
    this.kind = null;
    this.host = this.self = 0;
  }
}

/** Opens the network for an engine and hosts or joins (shared by the editor and packaged games). */
export async function openNetwork(engine, net, {host, room = "kern", relay = ""}) {
  const r = await net.open({kind: relay ? "relay" : "local", room, hosting: host, url: relay});
  engine.command({cmd: "netAddress", self: r.self});
  return host ? engine.command({cmd: "netHost"}) : engine.command({cmd: "netJoin", server: r.host});
}

export const modifiers = e => (e.shiftKey ? 1 : 0) | (e.ctrlKey || e.metaKey ? 2 : 0) | (e.altKey ? 4 : 0);

export const toBase64 = bytes => {
  let s = "";
  for (let i = 0; i < bytes.length; i += 0x8000) s += String.fromCharCode.apply(null, bytes.subarray(i, i + 0x8000));
  return btoa(s);
};

export const escHtml = s => String(s ?? "").replace(/[&<>"']/g, c => ({"&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;"}[c]));

/**
 * Gamepad (Xbox layout, browser "standard" mapping): sticks, triggers and buttons
 * go to the engine every frame while a pad is connected. Kern applies dead zones,
 * response curves and the soldier's aim rates (SpawnCity mapping):
 * left stick move, right stick look, RT fire, LT aim, A jump, B crouch,
 * X reload, Y interact, L3 / LB sprint, Start pause.
 */
export class GamepadInput {
  constructor(engine) { this.engine = engine; this.connected = false; this.last = ""; }
  poll() {
    const pads = navigator.getGamepads ? Array.from(navigator.getGamepads()).filter(p => p && p.connected) : [];
    const p = pads.find(g => g.mapping === "standard") || pads[0];
    if (!p) {
      if (this.connected) { this.connected = false; this.last = ""; this.engine.command({cmd: "gamepad", connected: false}); }
      return null;
    }
    const b = i => !!p.buttons[i]?.pressed, v = i => p.buttons[i]?.value ?? 0, ax = i => p.axes[i] ?? 0;
    const s = {cmd: "gamepad", connected: true, lx: ax(0), ly: -ax(1), rx: ax(2), ry: ax(3), lt: v(6), rt: v(7),
      a: b(0), b: b(1), x: b(2), y: b(3), lb: b(4), rb: b(5), l3: b(10), start: b(9)};
    // Any pressed button or a stick past .22 marks the pad as the device in use (SpawnCity pad-input).
    s.active = p.buttons.slice(0, 17).some(x => x?.pressed) || p.axes.slice(0, 4).some(a => Math.abs(a) > 0.22);
    const key = JSON.stringify(s);
    // Sticks at rest and nothing pressed: send once, then stay quiet.
    if (key !== this.last || Math.abs(s.rx) > 0.05 || Math.abs(s.ry) > 0.05) { this.last = key; this.engine.command(s); }
    this.connected = true;
    return p;
  }
}

/** Styles of the engine-drawn touch controls (see TouchOverlay). */
export const TOUCH_STYLE = `.k-touch{position:absolute;inset:0;pointer-events:none;z-index:6;font:700 10px Inter,system-ui,sans-serif;letter-spacing:.05em;text-transform:uppercase;color:#f2f4f7;user-select:none;-webkit-user-select:none}
.k-touch .ts{position:absolute;border-radius:50%;border:1.5px solid rgba(255,255,255,.2);box-sizing:border-box}
.k-touch .ts.live{border-color:rgba(255,255,255,.5)}.k-touch .ts.accent{border-color:#f5b301}
.k-touch .ts i{position:absolute;width:31%;height:31%;border-radius:50%;background:rgba(255,255,255,.16);border:1.5px solid rgba(255,255,255,.42);transform:translate(-50%,-50%);box-sizing:border-box}
.k-touch .ts.live i{background:rgba(255,255,255,.34);border-color:rgba(255,255,255,.75)}
.k-touch .ts span{position:absolute;left:0;right:0;top:-15px;text-align:center;opacity:.5;font-size:9px;white-space:nowrap}
.k-touch .tb{position:absolute;display:grid;place-items:center;border:2px solid rgba(255,255,255,.7);box-sizing:border-box;border-radius:50%;font-size:10px;font-weight:900;text-shadow:0 1px 2px #000;transition:transform .06s}
.k-touch .tb.sq{border-radius:10px}.k-touch .tb.pressed{transform:scale(.94);border-color:#fff;background:#ff7a1acc!important}
.k-touch .tb.on{border-color:#f5b301;color:#f5b301}
.k-touch-on .k-hud-top{top:58px}
.k-touch-on .k-hp,.k-touch-on .k-ammo,.k-touch-on .k-hud-speed{left:auto;right:max(14px,env(safe-area-inset-right));bottom:auto}
.k-touch-on .k-ammo,.k-touch-on .k-hud-speed{top:10px}.k-touch-on .k-hp{top:66px}.k-touch-on .k-ammo b{font-size:30px}
.k-touch .tf{position:absolute;width:46px;height:46px;margin:-23px 0 0 -23px;border-radius:50%;border:2px solid rgba(255,138,106,.8);box-shadow:0 0 12px rgba(255,90,60,.5)}`;

/**
 * Draws the touch controls the engine lays out and drives (status.touch, see
 * kern/game/touch.h): sticks, buttons and the firing finger. Purely visual; the
 * fingers go to the engine (StudioEngine.touch).
 */
export class TouchOverlay {
  constructor(parent) {
    this.el = document.createElement("div");
    this.el.className = "k-touch";
    this.el.hidden = true;
    this.key = "";
    parent.appendChild(this.el);
  }
  attach(parent) {
    if (this.el.parentNode === parent) return;
    this.el.parentNode?.classList.remove("k-touch-on");
    parent.appendChild(this.el);
  }
  update(t) {
    // The parent knows the corners belong to the thumbs (the HUD moves up, see TOUCH_STYLE).
    this.el.parentNode?.classList.toggle("k-touch-on", !!t);
    if (!t) { if (!this.el.hidden) { this.el.hidden = true; this.key = ""; } return; }
    this.el.hidden = false;
    const key = JSON.stringify(t);
    if (key === this.key) return;
    this.key = key;
    const px = v => `${Math.round(v)}px`;
    const stick = (s, id) => `<div class="ts${s.live ? " live" : ""}${s.accent ? " accent" : ""}" data-stick="${id}" style="left:${px(s.x)};top:${px(s.y)};width:${px(s.size)};height:${px(s.size)}">` +
      `<span>${escHtml(s.label)}</span><i style="left:${50 + 31 * s.kx}%;top:${50 + 31 * s.ky}%"></i></div>`;
    let html = stick(t.move, "move") + stick(t.look, "look");
    for (const b of t.buttons)
      html += `<div class="tb${b.round ? "" : " sq"}${b.pressed ? " pressed" : ""}${b.on ? " on" : ""}" data-touch="${escHtml(b.id)}" style="left:${px(b.x)};top:${px(b.y)};width:${px(b.w)};height:${px(b.h)};background:${b.color}b8">${escHtml(b.label)}</div>`;
    if (t.fire) html += `<div class="tf" style="left:${px(t.fireX)};top:${px(t.fireY)}"></div>`;
    this.el.innerHTML = html;
  }
}

/**
 * Tells the engine what device this is and which one the player uses right now
 * (SpawnCity's rules, decided in C++: touch switches to the touch layout, a mouse
 * only on fine-pointer devices, any key to desktop), plus the safe-area insets.
 */
export function watchInputDevice(engine) {
  const mq = q => matchMedia(q).matches;
  const probe = document.createElement("div");
  probe.style.cssText = "position:fixed;left:0;top:0;visibility:hidden;pointer-events:none;padding:env(safe-area-inset-top,0px) env(safe-area-inset-right,0px) env(safe-area-inset-bottom,0px) env(safe-area-inset-left,0px)";
  document.body.appendChild(probe);
  const safe = () => { const c = getComputedStyle(probe); return [c.paddingLeft, c.paddingTop, c.paddingRight, c.paddingBottom].map(v => parseFloat(v) || 0); };
  const platform = () => engine.command({cmd: "device", safe: safe(),
    platform: {fine: mq("(hover: hover) and (pointer: fine)"), coarse: mq("(pointer: coarse)"), touchPoints: navigator.maxTouchPoints || 0}});
  platform();
  addEventListener("resize", platform);
  addEventListener("orientationchange", platform);
  let touch = mq("(pointer: coarse)");
  addEventListener("pointerdown", e => {
    touch = e.pointerType !== "mouse";
    engine.command({cmd: "device", event: touch ? "touch" : "mouse"});
  }, true);
  addEventListener("pointermove", e => {
    if (touch && e.pointerType === "mouse" && !document.pointerLockElement && (e.movementX || e.movementY)) {
      engine.command({cmd: "device", event: "mousemove"});
      touch = engine.status()?.input?.device === "touch";
    }
  }, true);
  addEventListener("keydown", () => { touch = false; }, true);
}

/**
 * Routes a viewport's touch / pen pointers to the engine's touch controls.
 * `pos(e)` gives css px in the viewport; returns true when the event was used.
 */
export function routeTouch(engine, type, e, pos) {
  if (e.pointerType === "mouse") return false;
  if (type === POINTER.move) {
    let used = false;
    for (const c of e.getCoalescedEvents?.() || [e]) { const [x, y] = pos(c); used = engine.touch(type, c.pointerId, x, y, c.timeStamp) || used; }
    return used;
  }
  const [x, y] = pos(e);
  return engine.touch(type, e.pointerId, x, y, e.timeStamp);
}

/**
 * Keeps a networked game running while its tab is in the background: browsers
 * pause requestAnimationFrame there (and throttle timers), which would freeze
 * a host and time out its players. A worker's timer keeps ticking; `tick` runs
 * only while the page is hidden.
 */
export function backgroundTicker(tick, ms = 16) {
  let worker = null;
  try {
    const src = `setInterval(() => postMessage(0), ${ms});`;
    worker = new Worker(URL.createObjectURL(new Blob([src], {type: "text/javascript"})));
    worker.onmessage = () => { if (document.hidden) tick(performance.now()); };
  } catch { /* workers unavailable: background tabs pause */ }
  return worker;
}

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
  if (h.soldier) html += soldierHtml(h.soldier, h.status === "playing");
  else if (h.player) {
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

/** First-person shooter HUD: crosshair sized by the spread, hit marker, health, ammo, damage direction. */
function soldierHtml(f, playing) {
  let html = "";
  const tanHalf = Math.tan((f.fov || 60) * Math.PI / 360);
  // Crosshair gap: the spread cone's radius on screen (vh), plus a little room.
  const gap = Math.min(18, 50 * Math.tan(f.spread * Math.PI / 180) / tanHalf + 0.6).toFixed(2);
  if (f.respawnIn < 0 && playing) {
    html += f.aiming ? `<div class="k-xh k-xh-dot"></div>`
      : `<div class="k-xh" style="--g:${gap}vh"><i class="k-xh-t"></i><i class="k-xh-b"></i><i class="k-xh-l"></i><i class="k-xh-r"></i><i class="k-xh-c"></i></div>`;
    if (f.hitAge < 0.28) html += `<div class="k-hit${f.hitKill ? " k-kill" : f.hitHead ? " k-head" : ""}" style="opacity:${(1 - f.hitAge / 0.28).toFixed(2)}"><i></i><i></i><i></i><i></i></div>`;
  }
  if (f.damageAge < 1.2) {
    const a = Math.max(0, 1 - f.damageAge / 1.2);
    html += `<div class="k-dmg" style="opacity:${(a * 0.85).toFixed(2)}"></div>`;
    html += `<div class="k-dmg-dir" style="transform:translate(-50%,-50%) rotate(${(-f.damageYaw).toFixed(0)}deg);opacity:${a.toFixed(2)}"><i></i></div>`;
  }
  const hp = Math.max(0, f.health), max = Math.max(1, f.maxHealth);
  html += `<div class="k-hp${hp / max < 0.3 ? " k-low" : ""}"><b>${Math.ceil(hp)}</b><div class="k-hp-bar"><div style="width:${Math.round(100 * hp / max)}%"></div>` +
    (f.armor > 0 ? `<div class="k-armor" style="width:${Math.round(100 * Math.min(1, f.armor / max))}%"></div>` : "") + `</div></div>`;
  if (f.ammo >= 0) {
    html += `<div class="k-ammo${f.ammo === 0 ? " k-empty" : ""}"><b>${f.ammo}</b><span>/ ${f.reserve}</span>` +
      (f.reload >= 0 ? `<div class="k-reload"><div style="width:${Math.round(f.reload * 100)}%"></div></div><em>Reloading</em>`
        : f.ammo === 0 ? `<em>${f.reserve > 0 ? "R / X to reload" : "No ammo"}</em>` : "") + `</div>`;
  }
  if (f.respawnIn >= 0) html += `<div class="k-respawn"><div>Down</div><small>Respawn in ${Math.ceil(f.respawnIn)}</small></div>`;
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
.k-hud-banner.k-paused{pointer-events:none}
.k-hud-banner small{display:block;margin:6px 0 14px;font-size:14px;opacity:.85}
.k-hud-banner button{font:600 14px system-ui,sans-serif;padding:8px 20px;border:0;border-radius:6px;background:#2f7de1;color:#fff;cursor:pointer}
.k-hud-banner em{display:block;margin-top:8px;font-size:11px;font-style:normal;opacity:.55}
.k-xh{position:absolute;left:50%;top:50%;width:0;height:0;--g:1vh;filter:drop-shadow(0 0 1px rgba(0,0,0,.9))}
.k-xh i{position:absolute;background:rgba(255,255,255,.92);border-radius:1px}
.k-xh-t,.k-xh-b{width:2px;height:9px;left:-1px}.k-xh-t{bottom:var(--g)}.k-xh-b{top:var(--g)}
.k-xh-l,.k-xh-r{height:2px;width:9px;top:-1px}.k-xh-l{right:var(--g)}.k-xh-r{left:var(--g)}
.k-xh-c{width:2px;height:2px;left:-1px;top:-1px}
.k-xh-dot{width:4px;height:4px;margin:-2px 0 0 -2px;border-radius:50%;background:rgba(255,255,255,.85)}
.k-hit{position:absolute;left:50%;top:50%;width:0;height:0}
.k-hit i{position:absolute;width:2px;height:10px;background:#fff;left:-1px;top:-5px}
.k-hit i:nth-child(1){transform:rotate(45deg) translateY(-12px)}.k-hit i:nth-child(2){transform:rotate(135deg) translateY(-12px)}
.k-hit i:nth-child(3){transform:rotate(225deg) translateY(-12px)}.k-hit i:nth-child(4){transform:rotate(315deg) translateY(-12px)}
.k-hit.k-head i{background:#ffd34d}.k-hit.k-kill i{background:#ff4d4d;height:14px}
.k-dmg{position:absolute;inset:0;background:radial-gradient(ellipse at center,transparent 55%,rgba(200,20,20,.55) 100%)}
.k-dmg-dir{position:absolute;left:50%;top:50%;width:min(46vh,46vw);height:min(46vh,46vw)}
.k-dmg-dir i{position:absolute;left:50%;top:0;width:60px;height:14px;margin-left:-30px;border-radius:50% 50% 0 0;background:rgba(255,60,50,.85)}
.k-hp{position:absolute;left:20px;bottom:20px;display:flex;align-items:center;gap:10px}
.k-hp b{font-size:30px;font-weight:700;min-width:52px}
.k-hp-bar{position:relative;width:180px;height:8px;border-radius:4px;background:rgba(255,255,255,.18);overflow:hidden}
.k-hp-bar div{position:absolute;left:0;top:0;bottom:0;background:linear-gradient(90deg,#5fe08a,#b5f5c9)}
.k-hp-bar .k-armor{background:#69b7ff;height:3px;top:auto}
.k-hp.k-low b{color:#ff8a7a}.k-hp.k-low .k-hp-bar div:first-child{background:#ff6b5b}
.k-ammo{position:absolute;right:22px;bottom:18px;text-align:right}
.k-ammo b{font-size:38px;font-weight:700}.k-ammo span{margin-left:6px;font-size:16px;opacity:.65}
.k-ammo em{display:block;font-size:11px;font-style:normal;letter-spacing:.1em;text-transform:uppercase;opacity:.8}
.k-ammo.k-empty b{color:#ff8a7a}
.k-reload{width:120px;height:3px;margin:4px 0 2px auto;background:rgba(255,255,255,.2)}.k-reload div{height:100%;background:#fff}
.k-respawn{position:absolute;left:50%;top:40%;transform:translate(-50%,-50%);text-align:center}
.k-respawn div{font-size:34px;font-weight:800;color:#ff8a7a}.k-respawn small{font-size:15px;opacity:.85}`;

export const PLAYER_STYLE = `html,body{margin:0;height:100%;overflow:hidden;background:#0d1015;color:#eef3f8;font:14px system-ui,-apple-system,"Segoe UI",sans-serif;touch-action:none;user-select:none;-webkit-user-select:none}
#gl{position:fixed;inset:0;width:100%;height:100%;display:block;outline:none}
#hud{position:fixed;inset:0;pointer-events:none}
#hint{position:fixed;left:0;right:0;bottom:10px;text-align:center;opacity:.6;font-size:12px;pointer-events:none;text-shadow:0 1px 2px #000}
#error{position:fixed;inset:auto 12px 40px 12px;padding:10px 14px;border-radius:6px;background:#7a1f1f;display:none}
#loading{position:fixed;inset:0;display:grid;place-items:center;font-size:13px;letter-spacing:.2em;text-transform:uppercase;opacity:.6}
#restart{position:fixed;left:max(8px,env(safe-area-inset-left));top:max(8px,env(safe-area-inset-top));width:44px;height:40px;border-radius:10px;border:1.5px solid rgba(255,255,255,.3);background:rgba(10,13,18,.56);color:#fff;font:600 18px system-ui;touch-action:none;z-index:7}
[hidden]{display:none!important}
${HUD_STYLE}
${TOUCH_STYLE}`;

export const PLAYER_BODY = `<canvas id="gl" tabindex="0" aria-label="Game view"></canvas><div id="hud" class="k-hud"></div><div id="loading">Loading</div>
<div id="hint">WASD / arrows move · Space jump · Shift boost · mouse look (click to capture, Esc frees) · P pause · R restart · gamepad supported</div>
<button id="restart" hidden aria-label="Restart">↻</button><div id="error" role="alert"></div>`;

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
  const audio = startAudio(engine);
  const opened = template ? engine.command({cmd: "new", template}) : engine.command({cmd: "open", text: levelText, name: "game.klevel"});
  engine.loadPolicies().catch(() => {});
  if (!opened?.ok) throw Error(engine.state()?.statusText || "the level could not be loaded");
  const params = new URLSearchParams(location.search);
  const online = params.get("host") ?? params.get("join");
  let net = null;
  if (online != null) {
    net = new KernNet(engine);
    net.flushEveryFrame = true;
    try {
      await openNetwork(engine, net, {host: params.has("host"), room: online || "kern", relay: params.get("relay") || ""});
    } catch (error) {
      net.close();
      net = null;
      document.getElementById("loading").textContent = "Online play failed: " + error.message + " - playing alone";
      engine.command({cmd: "play", mode: "play"});
    }
  } else {
    const started = engine.command({cmd: "play", mode: "play"});
    if (!started?.ok) throw Error(engine.state()?.statusText || "the level has errors");
  }
  const $ = id => document.getElementById(id);
  $("loading").hidden = true;
  const hud = $("hud");
  const overlay = new TouchOverlay(document.body);
  watchInputDevice(engine);
  canvas.addEventListener("webglcontextlost", e => { e.preventDefault(); engine.contextLost(); kern.resetTables(); });
  canvas.addEventListener("webglcontextrestored", () => { kern.attachContext(canvas, gl); engine.contextRestored(); });

  // Mouse: click captures the pointer for mouse look; touch drags orbit the camera.
  // Touch drags look around; only mouse buttons fire and aim (touch has its own buttons).
  // Touch and pen fingers go to the engine's touch controls; the mouse looks (pointer lock) and fires.
  const send = (type, e) => engine.pointer(0, type, e.clientX, e.clientY, {button: e.pointerType === "mouse" ? e.button : -1, buttons: e.buttons, mods: modifiers(e), wheel: e.deltaY ?? 0});
  const at = e => [e.clientX, e.clientY];
  canvas.addEventListener("pointerdown", e => {
    canvas.focus();
    try { canvas.setPointerCapture(e.pointerId); } catch { /* ignore */ }
    e.preventDefault();
    if (routeTouch(engine, POINTER.down, e, at)) return;
    // The mouse looks through pointer lock - not while watching (observer camera: it drags bodies).
    if (e.pointerType === "mouse" && document.pointerLockElement !== canvas && !engine.state()?.play?.observing) canvas.requestPointerLock?.()?.catch?.(() => {});
    send(POINTER.down, e);
  });
  canvas.addEventListener("pointermove", e => { if (!routeTouch(engine, POINTER.move, e, at) && document.pointerLockElement !== canvas) send(POINTER.move, e); });
  canvas.addEventListener("pointerup", e => { if (!routeTouch(engine, POINTER.up, e, at)) send(POINTER.up, e); });
  canvas.addEventListener("pointercancel", e => { if (!routeTouch(engine, POINTER.cancel, e, at)) send(POINTER.cancel, e); });
  // No finger left on the screen: nothing can still be held (SpawnCity's ground truth).
  addEventListener("touchend", e => { if (!e.touches.length) engine.command({cmd: "touchRelease"}); });
  addEventListener("visibilitychange", () => { if (document.hidden) engine.command({cmd: "touchRelease"}); });
  canvas.addEventListener("wheel", e => { e.preventDefault(); send(POINTER.wheel, e); }, {passive: false});
  canvas.addEventListener("contextmenu", e => e.preventDefault());
  document.addEventListener("mousemove", e => { if (document.pointerLockElement === canvas) engine.look(e.movementX, e.movementY); });
  // The browser's Escape ends pointer lock without telling the page: pause right away (one press),
  // and the next click into the game resumes and captures the mouse again.
  let wasLocked = false;
  document.addEventListener("pointerlockchange", () => {
    const locked = document.pointerLockElement === canvas;
    if (wasLocked && !locked && document.hasFocus() && engine.status()?.hud?.status === "playing") engine.command({cmd: "pause", value: true});
    if (locked) engine.command({cmd: "pause", value: false});
    wasLocked = locked;
  });

  const key = (e, down) => {
    const r = engine.key(e.key, e.code, down, modifiers(e), e.repeat);
    if (r?.handled && !(e.code === "Escape")) e.preventDefault();
  };
  addEventListener("keydown", e => key(e, true));
  addEventListener("keyup", e => key(e, false));
  addEventListener("blur", () => engine.blur());

  const gamepad = new GamepadInput(engine);
  let touchShown = false;
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
    audio?.pump();
    net?.flush();
    if (time - lastTextures > 250) { lastTextures = time; engine.pollTextures(); engine.pollSounds(audio); engine.loadPolicies().catch(() => {}); }
    gamepad.poll();
    const st = engine.status();
    const html = hudHtml(st?.hud) + (st?.paused ? `<div class="k-hud-banner k-paused"><div>Paused</div><small>Click to continue</small></div>` : "");
    if (html !== hudKey) { hudKey = html; hud.innerHTML = html; }
    overlay.update(st?.touch);
    const onTouch = st?.input?.device === "touch";
    if (onTouch !== touchShown) { touchShown = onTouch; $("restart").hidden = !onTouch; $("hint").hidden = onTouch; }
    requestAnimationFrame(frame);
  };
  requestAnimationFrame(frame);
  if (net) backgroundTicker(time => { engine.frame(time); engine.takeUi(); audio?.pump(); net.flush(); });
  canvas.focus();
  const api = {engine, audio, net, get status() { return engine.status(); }, command: c => engine.command(c)};
  window.KernGame = api;
  return api;
}
