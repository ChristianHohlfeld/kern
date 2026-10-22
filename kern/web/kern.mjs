// Kern browser platform layer.
//
// The engine is C++ compiled to WebAssembly (kern/build.sh). The browser only
// offers WebGL2, canvas events and timing to JavaScript, so this file does
// exactly three things and contains no engine logic:
//   1. instantiate the module with the `kern_gl` (WebGL2) and `kern_env`
//      imports plus a minimal WASI shim,
//   2. own the GPU object handle tables and context-loss plumbing,
//   3. expose `KernRenderer`, a thin typed-array marshalling facade over the
//      engine's C ABI (kern/include/kern/api/c_api.h).
//
// Usage:
//   const kern = await loadKern({ bytes });      // once per page
//   const renderer = new KernRenderer(canvas, { shadows: {...} }, kern);

export const KERN_WEB_VERSION = "kern-web-1";

// Word offsets: keep in sync with kern/include/kern/api/c_api.h.
const MD = Object.freeze({
  POSITIONS: 0, NORMALS: 2, COLORS: 4, UVS: 6, INDICES: 8, INDEX_BYTES: 10, JOINTS: 11, WEIGHTS: 13, BONES: 15,
  EXT0: 17, EXT1: 19, TRANSFORMS: 21, TINTS: 23, TEXTURE: 25, FLAGS: 26, ALPHA_MODE: 27, COLOR: 28,
  ALPHA_CUTOFF: 32, ROUGHNESS: 33, METALNESS: 34, PARAMS: 35, EMISSIVE: 43, WORDS: 46,
});
const MF = Object.freeze({ VISIBLE: 1, UNLIT: 2, DOUBLE_SIDED: 4, CAST_SHADOW: 8, FRUSTUM_CULLED: 16, VIEWMODEL: 32, LINEAR_COLORS: 64, WELD: 128 });
const ID = Object.freeze({ WIDTH: 0, HEIGHT: 1, SHADOWS: 2, SHADOW_RESOLUTION: 3, SHADOW_EXTENT: 4, SHADOW_BIAS: 5, BLOOM: 6, BLOOM_FORCED: 7, GRADE: 8, WORDS: 9 });
const RP = Object.freeze({ CLEAR: 0, HAS_VIEWPORT: 4, X: 5, Y: 6, W: 7, H: 8, PAGE_HIDDEN: 9, WORDS: 10 });
const RS = Object.freeze({ DRAW_CALLS: 0, SHADOW_DRAW_CALLS: 1, SHADOW_RESOLUTION: 2, SHADOW_REUSED: 3, TRIANGLES: 4, INSTANCES: 5, CULLED: 6, CPU_MS: 7, GPU_MS: 8, BLOOM: 9, VISIBLE: 10, SLOTS: 11 });
const LI_SLOTS = 27, SK_SLOTS = 14, PK_SLOTS = 15;
const BLOOM_STATES = ["off", "active", "glow-off-slow-device", "unsupported"];
const MAX_POINT_LIGHTS = 6;

// ---------------------------------------------------------------------------
// Linear colour tagging (vertex colours baked from linear sources skip the
// sRGB decode). Kept as a WeakSet so callers can tag shared arrays once.
const LINEAR_COLOR_ARRAYS = new WeakSet();
export function markLinearColors(...arrays) {
  for (const a of arrays) if (a && typeof a === "object") LINEAR_COLOR_ARRAYS.add(a);
  return arrays[0];
}

// ---------------------------------------------------------------------------
// Module loading

function decodeBase64(text) {
  if (typeof atob === "function") {
    const bin = atob(text), out = new Uint8Array(bin.length);
    for (let i = 0; i < bin.length; i++) out[i] = bin.charCodeAt(i);
    return out;
  }
  return Uint8Array.from(globalThis.Buffer.from(text, "base64"));
}

/**
 * Instantiates a Kern module. Pass one of `bytes` (ArrayBuffer/Uint8Array),
 * `base64`, `url` or an already compiled `module`.
 */
export async function loadKern({ bytes = null, base64 = null, url = null, module = null, log = null } = {}) {
  if (!module) {
    if (base64) bytes = decodeBase64(base64);
    if (!bytes && url) bytes = await (await fetch(url)).arrayBuffer();
    if (!bytes) throw Error("loadKern needs bytes, base64, url or module");
    module = await WebAssembly.compile(bytes);
  }
  const kern = new KernModule(log);
  const instance = await WebAssembly.instantiate(module, kern.imports());
  kern.attach(instance);
  return kern;
}

const PENDING = Symbol("pending");

export class KernModule {
  constructor(log) {
    this.instance = null;
    this.exports = null;
    this.memory = null;
    this.gl = null;
    this.canvas = null;
    this.logSink = log;
    this.decoder = new TextDecoder();
    this.encoder = new TextEncoder();
    this.views = { buffer: null, u8: null, u16: null, u32: null, i32: null, f32: null, f64: null };
    // GPU object handle tables (0 = null).
    this.objects = [null];
    this.freeObjects = [];
    this.locations = [null];
    this.pinned = new Map(); // object slot -> restore function (host-owned textures)
    this.timer = null;
    this.drawBufferLists = new Map();
    this.heightSampler = null;  // (x, y) => ground height, used by the geometry builders
    this.statusSink = null;     // (text) => void, kern::Engine::set_status
    this.pointerLockSink = null;  // (lock) => void, kern::Engine::request_pointer_lock
  }

  // ---- linear memory views (refreshed after memory growth) ----
  refresh() {
    const buffer = this.memory.buffer;
    if (this.views.buffer === buffer) return this.views;
    this.views = {
      buffer, u8: new Uint8Array(buffer), u16: new Uint16Array(buffer), u32: new Uint32Array(buffer),
      i32: new Int32Array(buffer), f32: new Float32Array(buffer), f64: new Float64Array(buffer),
    };
    return this.views;
  }
  get u8() { return this.refresh().u8; }
  get u32() { return this.refresh().u32; }
  get f32() { return this.refresh().f32; }
  get f64() { return this.refresh().f64; }

  cstring(ptr) {
    const u8 = this.u8;
    let end = ptr;
    while (u8[end]) end++;
    return this.decoder.decode(u8.subarray(ptr, end));
  }
  string(ptr, length) { return this.decoder.decode(this.u8.subarray(ptr, ptr + length)); }
  lastError() { return this.cstring(this.exports.kern_last_error()) || "unknown engine error"; }
  fail(context) { const message = this.lastError(); this.exports.kern_clear_error(); return new Error(`${context}: ${message}`); }

  // ---- handle tables ----
  addObject(object) {
    if (!object) return 0;
    let slot = this.freeObjects.pop();
    if (slot === undefined) slot = this.objects.push(object) - 1;
    else this.objects[slot] = object;
    return slot;
  }
  releaseObject(slot) {
    if (!slot || this.pinned.has(slot)) return;
    this.objects[slot] = null;
    this.freeObjects.push(slot);
  }
  /** After context loss every GPU object is dead; pinned slots stay reserved. */
  resetTables() {
    const max = Math.max(0, ...this.pinned.keys());
    this.objects = [null];
    this.freeObjects = [];
    for (let slot = 1; slot <= max; slot++) {
      this.objects[slot] = this.pinned.has(slot) ? PENDING : null;
      if (!this.pinned.has(slot)) this.freeObjects.push(slot);
    }
    this.freeObjects.reverse();
    this.locations = [null];
    this.timer = null;
  }

  attachContext(canvas, gl) {
    this.canvas = canvas;
    this.gl = gl;
    this.timer = null;
  }

  attach(instance) {
    this.instance = instance;
    this.exports = instance.exports;
    this.memory = instance.exports.memory;
    this.refresh();
    this.exports._initialize?.();
  }

  imports() {
    const self = this;
    const gl = () => self.gl;
    const obj = slot => self.objects[slot] ?? null;
    const loc = slot => self.locations[slot] ?? null;
    const env = {
      now: () => performance.now(),
      terrain_height: (x, y) => (self.heightSampler ? Number(self.heightSampler(x, y)) : 0),
      status: (ptr, length) => self.statusSink?.(self.string(ptr, length)),
      pointer_lock: lock => self.pointerLockSink?.(lock !== 0),
      log: (level, ptr, length) => {
        const text = self.string(ptr, length);
        if (self.logSink) self.logSink(level, text);
        else (level >= 3 ? console.error : level === 2 ? console.warn : console.log)(`[kern] ${text}`);
      },
    };
    const kern_gl = {
      createBuffer: () => self.addObject(gl().createBuffer()),
      deleteBuffer: b => { gl().deleteBuffer(obj(b)); self.releaseObject(b); },
      bindBuffer: (target, b) => gl().bindBuffer(target, obj(b)),
      bufferData: (target, ptr, bytes, usage) => {
        if (!ptr) gl().bufferData(target, bytes, usage);
        else gl().bufferData(target, self.u8, usage, ptr, bytes);
      },
      bufferSubData: (target, offset, ptr, bytes) => gl().bufferSubData(target, offset, self.u8, ptr, bytes),
      createVertexArray: () => self.addObject(gl().createVertexArray()),
      deleteVertexArray: v => { gl().deleteVertexArray(obj(v)); self.releaseObject(v); },
      bindVertexArray: v => gl().bindVertexArray(obj(v)),
      enableVertexAttribArray: i => gl().enableVertexAttribArray(i),
      vertexAttribPointer: (i, size, type, normalized, stride, offset) => gl().vertexAttribPointer(i, size, type, !!normalized, stride, offset),
      vertexAttribDivisor: (i, d) => gl().vertexAttribDivisor(i, d),

      createTexture: () => self.addObject(gl().createTexture()),
      deleteTexture: t => { gl().deleteTexture(obj(t)); self.releaseObject(t); },
      bindTexture: (target, t) => gl().bindTexture(target, obj(t)),
      activeTexture: unit => gl().activeTexture(unit),
      texImage2D: (target, level, internal, w, h, format, type, ptr, bytes) => {
        const g = gl();
        if (!ptr) { g.texImage2D(target, level, internal, w, h, 0, format, type, null); return; }
        const v = self.refresh();
        const view = type === g.FLOAT ? v.f32 : type === g.HALF_FLOAT || type === g.UNSIGNED_SHORT ? v.u16 : type === g.UNSIGNED_INT ? v.u32 : v.u8;
        const shift = view.BYTES_PER_ELEMENT === 4 ? 2 : view.BYTES_PER_ELEMENT === 2 ? 1 : 0;
        g.texImage2D(target, level, internal, w, h, 0, format, type, view, ptr >> shift);
      },
      texParameteri: (target, pname, value) => gl().texParameteri(target, pname, value),
      generateMipmap: target => gl().generateMipmap(target),

      createFramebuffer: () => self.addObject(gl().createFramebuffer()),
      deleteFramebuffer: f => { gl().deleteFramebuffer(obj(f)); self.releaseObject(f); },
      bindFramebuffer: (target, f) => gl().bindFramebuffer(target, obj(f)),
      framebufferTexture2D: (target, attachment, textarget, t, level) => gl().framebufferTexture2D(target, attachment, textarget, obj(t), level),
      checkFramebufferStatus: target => gl().checkFramebufferStatus(target),
      drawBuffers: (ptr, count) => {
        const u32 = self.u32, values = [];
        for (let i = 0; i < count; i++) values.push(u32[(ptr >> 2) + i]);
        const key = values.join(",");
        let list = self.drawBufferLists.get(key);
        if (!list) self.drawBufferLists.set(key, list = values);
        gl().drawBuffers(list);
      },
      readBuffer: src => gl().readBuffer(src),
      createRenderbuffer: () => self.addObject(gl().createRenderbuffer()),
      deleteRenderbuffer: r => { gl().deleteRenderbuffer(obj(r)); self.releaseObject(r); },
      bindRenderbuffer: (target, r) => gl().bindRenderbuffer(target, obj(r)),
      renderbufferStorage: (target, internal, w, h) => gl().renderbufferStorage(target, internal, w, h),
      framebufferRenderbuffer: (target, attachment, rbTarget, r) => gl().framebufferRenderbuffer(target, attachment, rbTarget, obj(r)),

      createShader: type => self.addObject(gl().createShader(type)),
      shaderSource: (s, ptr, length) => gl().shaderSource(obj(s), self.string(ptr, length)),
      compileShader: s => gl().compileShader(obj(s)),
      getShaderParameter: (s, pname) => Number(gl().getShaderParameter(obj(s), pname)),
      getShaderInfoLog: (s, out, capacity) => self.writeString(gl().getShaderInfoLog(obj(s)) || "", out, capacity),
      deleteShader: s => { gl().deleteShader(obj(s)); self.releaseObject(s); },
      createProgram: () => self.addObject(gl().createProgram()),
      attachShader: (p, s) => gl().attachShader(obj(p), obj(s)),
      linkProgram: p => gl().linkProgram(obj(p)),
      getProgramParameter: (p, pname) => Number(gl().getProgramParameter(obj(p), pname)),
      getProgramInfoLog: (p, out, capacity) => self.writeString(gl().getProgramInfoLog(obj(p)) || "", out, capacity),
      deleteProgram: p => { gl().deleteProgram(obj(p)); self.releaseObject(p); },
      useProgram: p => gl().useProgram(obj(p)),
      getUniformLocation: (p, ptr, length) => {
        const location = gl().getUniformLocation(obj(p), self.string(ptr, length));
        return location ? self.locations.push(location) - 1 : 0;
      },

      uniform1i: (l, v) => gl().uniform1i(loc(l), v),
      uniform1f: (l, v) => gl().uniform1f(loc(l), v),
      uniform2f: (l, x, y) => gl().uniform2f(loc(l), x, y),
      uniform3f: (l, x, y, z) => gl().uniform3f(loc(l), x, y, z),
      uniform4f: (l, x, y, z, w) => gl().uniform4f(loc(l), x, y, z, w),
      uniform3fv: (l, ptr, count) => gl().uniform3fv(loc(l), self.f32, ptr >> 2, count * 3),
      uniform4fv: (l, ptr, count) => gl().uniform4fv(loc(l), self.f32, ptr >> 2, count * 4),
      uniformMatrix4fv: (l, ptr, count) => gl().uniformMatrix4fv(loc(l), false, self.f32, ptr >> 2, count * 16),

      enable: cap => gl().enable(cap),
      disable: cap => gl().disable(cap),
      depthFunc: f => gl().depthFunc(f),
      depthMask: flag => gl().depthMask(!!flag),
      blendFunc: (s, d) => gl().blendFunc(s, d),
      cullFace: mode => gl().cullFace(mode),
      colorMask: (r, g, b, a) => gl().colorMask(!!r, !!g, !!b, !!a),
      clearColor: (r, g, b, a) => gl().clearColor(r, g, b, a),
      clearDepth: d => gl().clearDepth(d),
      clear: mask => gl().clear(mask),
      viewport: (x, y, w, h) => gl().viewport(x, y, w, h),
      scissor: (x, y, w, h) => gl().scissor(x, y, w, h),
      polygonOffset: (f, u) => gl().polygonOffset(f, u),
      drawArrays: (mode, first, count) => gl().drawArrays(mode, first, count),
      drawArraysInstanced: (mode, first, count, n) => gl().drawArraysInstanced(mode, first, count, n),
      drawElementsInstanced: (mode, count, type, offset, n) => gl().drawElementsInstanced(mode, count, type, offset, n),

      getExtension: (ptr, length) => {
        const name = self.string(ptr, length), ext = gl().getExtension(name);
        if (name === "EXT_disjoint_timer_query_webgl2") self.timer = ext;
        return ext ? 1 : 0;
      },
      createQuery: () => self.addObject(gl().createQuery()),
      deleteQuery: q => { gl().deleteQuery(obj(q)); self.releaseObject(q); },
      beginQuery: (target, q) => gl().beginQuery(target, obj(q)),
      endQuery: target => gl().endQuery(target),
      queryAvailable: q => (gl().getQueryParameter(obj(q), gl().QUERY_RESULT_AVAILABLE) ? 1 : 0),
      queryResultMs: q => Number(gl().getQueryParameter(obj(q), gl().QUERY_RESULT)) / 1e6,
      gpuDisjoint: () => (self.timer && gl().getParameter(self.timer.GPU_DISJOINT_EXT) ? 1 : 0),
    };
    // Minimal WASI: the engine never touches files; stdio goes to the console.
    const wasi = new Proxy({
      fd_write: (fd, iovs, count, written) => {
        const u32 = self.u32;
        let text = "", total = 0;
        for (let i = 0; i < count; i++) {
          const ptr = u32[(iovs >> 2) + i * 2], len = u32[(iovs >> 2) + i * 2 + 1];
          text += self.string(ptr, len);
          total += len;
        }
        u32[written >> 2] = total;
        if (text.trim()) (fd === 2 ? console.error : console.log)(`[kern] ${text.trimEnd()}`);
        return 0;
      },
      random_get: (ptr, length) => { crypto.getRandomValues(self.u8.subarray(ptr, ptr + length)); return 0; },
      clock_time_get: (id, precision, out) => {
        const ns = BigInt(Math.round(performance.now() * 1e6));
        new BigUint64Array(self.memory.buffer, out, 1)[0] = ns;
        return 0;
      },
      proc_exit: code => { throw Error(`kern module exited with ${code}`); },
      fd_close: () => 0,
      fd_seek: () => 70,   // ESPIPE
    }, { get: (target, name) => target[name] ?? (() => 52) });  // ENOSYS for anything else
    return { kern_env: env, kern_gl, wasi_snapshot_preview1: wasi };
  }

  writeString(text, out, capacity) {
    const bytes = this.encoder.encode(text);
    const n = Math.min(bytes.length, capacity);
    this.u8.set(bytes.subarray(0, n), out);
    return n;
  }
}

// ---------------------------------------------------------------------------
// Staging: a growable scratch area in linear memory for marshalling arrays.

class Staging {
  constructor(kern) { this.kern = kern; this.ptr = 0; this.capacity = 0; this.offset = 0; }
  reset(bytes) {
    const need = bytes + 64;
    if (need > this.capacity) {
      if (this.ptr) this.kern.exports.kern_free(this.ptr);
      this.capacity = Math.max(need, this.capacity * 2, 4096);
      this.ptr = this.kern.exports.kern_alloc(this.capacity);
      if (!this.ptr) throw Error("kern: out of memory");
    }
    this.offset = 0;
  }
  /** Copies a typed array; returns its address (8-byte aligned). */
  put(array) {
    if (!array?.length) return 0;
    const bytes = array.byteLength;
    const at = this.ptr + this.offset;
    this.kern.u8.set(new Uint8Array(array.buffer, array.byteOffset, bytes), at);
    this.offset += (bytes + 7) & ~7;
    return at;
  }
  static size(array) { return array?.length ? (array.byteLength + 7) & ~7 : 0; }
}

function asFloat32(value) {
  if (value == null) return null;
  if (value instanceof Float32Array) return value;
  return Float32Array.from(value);
}

/** Instance matrices as one Float32Array (accepts flat arrays or arrays of 16-element rows). */
function asMatrices(data) {
  if (data == null) return null;
  if (data instanceof Float32Array) return data.length && data.length % 16 === 0 ? data : null;
  if (data.length && data[0]?.length === 16) {
    const out = new Float32Array(data.length * 16);
    for (let i = 0; i < data.length; i++) if (data[i]?.length === 16) out.set(data[i], i * 16);
    return out;
  }
  if (data.length && data.length % 16 === 0) return Float32Array.from(data);
  return null;
}

// ---------------------------------------------------------------------------
// Renderer facade

export class KernRenderer {
  /**
   * @param {HTMLCanvasElement} canvas
   * @param {object} options width, height, shadows ({resolution, extent, bias}|null),
   *   bloom (true|false|{forced}), grade, contextAttributes
   * @param {KernModule} kern a loaded module (loadKern)
   */
  constructor(canvas, { width = canvas?.width ?? 960, height = canvas?.height ?? 540, shadows = null, bloom = true, grade = true,
    contextAttributes = {} } = {}, kern = KernRenderer.defaultModule) {
    if (!canvas?.getContext) throw Error("Canvas required");
    if (!kern?.exports) throw Error("Kern module not loaded (await loadKern() first)");
    if (kern.renderer && !kern.renderer.disposed) throw Error("one KernRenderer per module");
    const gl = canvas.getContext("webgl2", { alpha: false, antialias: false, depth: true, powerPreference: "high-performance",
      preserveDrawingBuffer: false, ...contextAttributes });
    if (!gl) throw Error("WebGL2 required");
    this.kern = kern;
    this.canvas = canvas;
    this.gl = gl;
    kern.attachContext(canvas, gl);
    kern.renderer = this;
    this.meshes = new Map();       // id -> record
    this.textures = new Set();
    this.staging = new Staging(kern);
    this.lost = false;
    this.disposed = false;
    this.stats = null;
    this.ready = false;

    // Camera (pushed to the engine before each render / query).
    this.eye = [0, -18, 8]; this.target = [0, 0, 2]; this.up = [0, 0, 1];
    this.fov = Math.PI / 3; this.near = .05; this.far = 1800;
    // Lighting (linear colours x intensity). Defaults: production day.
    this.sunDirection = [-.45, -.52, .72];
    this.sunColor = [2.8, 2.47, 2.11];
    this.skyColor = [.469, .609, .85];
    this.groundColor = [.125, .094, .054];
    this.fogColor = [.474, .584, .716];
    this.fogDensity = .00085;
    this.exposure = 1;
    this.envIntensity = .9;
    this.envZenith = [.05, .2, .62];
    this.envHorizon = [.52, .66, .86];
    this.envGround = [.105, .127, .084];
    this.skySun = null; this.skyMoon = null; this.night = 0;
    this.sky = { clouds: true, blastDir: [0, 0, -1], blastAge: -1, wind: [.012, .004] };
    this.pointLights = { count: 0 };  // mirror of the engine state (count of active lights)

    const e = kern.exports;
    const scratchBytes = 64 * 4 + 16 * 8 + LI_SLOTS * 4 + SK_SLOTS * 4 + PK_SLOTS * 8 + 64;
    this.scratch = e.kern_alloc(scratchBytes);   // fixed scratch for descriptors and results
    this.descPtr = this.scratch;                 // 64 words
    this.resultPtr = this.scratch + 256;         // 16 f64
    this.lightPtr = this.resultPtr + 128;        // 27 f32
    this.skyPtr = this.lightPtr + LI_SLOTS * 4;  // 14 f32
    this.pickPtr = (this.skyPtr + SK_SLOTS * 4 + 7) & ~7;  // 15 f64
    this.pointPtr = e.kern_alloc(MAX_POINT_LIGHTS * 4 * 4 * 2);

    const u32 = kern.u32, f32 = kern.f32, d = this.descPtr >> 2;
    u32.fill(0, d, d + ID.WORDS);
    u32[d + ID.WIDTH] = Math.max(1, Math.round(width));
    u32[d + ID.HEIGHT] = Math.max(1, Math.round(height));
    const shadow = shadows && shadows !== false ? shadows : null;
    this.shadowsEnabled = Boolean(shadow);
    u32[d + ID.SHADOWS] = shadow ? 1 : 0;
    u32[d + ID.SHADOW_RESOLUTION] = shadow?.resolution ?? 512;
    f32[d + ID.SHADOW_EXTENT] = shadow?.extent ?? 120;
    f32[d + ID.SHADOW_BIAS] = shadow?.bias ?? .0012;
    const bloomOptions = bloom === false ? { disabled: true } : bloom === true ? {} : { ...bloom };
    u32[d + ID.BLOOM] = bloomOptions.disabled ? 0 : 1;
    u32[d + ID.BLOOM_FORCED] = bloomOptions.forced ? 1 : 0;
    u32[d + ID.GRADE] = grade === false ? 0 : 1;
    if (!e.kern_renderer_init(this.descPtr)) throw kern.fail("renderer init");
    this.materialKeys = Array.from({ length: e.kern_material_key_count() }, (_, i) => kern.cstring(e.kern_material_key(i)));
    this.extensionAttributes = Array.from({ length: e.kern_extension_attribute_count() },
      (_, i) => ({ key: kern.cstring(e.kern_extension_attribute_key(i)), size: e.kern_extension_attribute_size(i) }));
    this.width = 0; this.height = 0;
    this.resize(width, height);
    this.ready = true;

    this.onLost = event => {
      event.preventDefault();
      if (this.lost) return;
      this.lost = true;
      this.ready = false;
      e.kern_context_lost();
      kern.resetTables();
    };
    this.onRestored = () => {
      if (!this.lost) return;
      this.lost = false;
      kern.attachContext(canvas, gl);
      for (const restore of kern.pinned.values()) restore();
      if (!e.kern_context_restored()) { this.error = kern.fail("context restore"); console.error(this.error); return; }
      this.ready = true;
    };
    canvas.addEventListener?.("webglcontextlost", this.onLost, false);
    canvas.addEventListener?.("webglcontextrestored", this.onRestored, false);
  }

  resize(width, height) {
    if (!(width > 0 && height > 0)) throw Error("invalid drawing dimensions");
    const w = Math.round(width), h = Math.round(height);
    if (w === this.width && h === this.height) return false;
    this.width = w; this.height = h;
    this.canvas.width = w; this.canvas.height = h;
    if (!this.kern.exports.kern_renderer_resize(w, h)) throw this.kern.fail("resize");
    return true;
  }

  // ---- camera & lighting ----
  setCamera({ eye, target, up, fov, near, far } = {}) {
    if (eye) this.eye = [eye[0], eye[1], eye[2]];
    if (target) this.target = [target[0], target[1], target[2]];
    if (up) this.up = [up[0], up[1], up[2]];
    if (fov !== undefined) this.fov = fov;
    if (near !== undefined) this.near = near;
    if (far !== undefined) this.far = far;
  }
  get camera() { return { eye: this.eye, target: this.target, up: this.up, fov: this.fov, near: this.near, far: this.far }; }

  setLighting(values = {}) {
    for (const key of ["sunDirection", "sunColor", "skyColor", "groundColor", "fogColor", "fogDensity", "exposure", "envIntensity",
      "envZenith", "envHorizon", "envGround", "night", "skySun", "skyMoon"])
      if (values[key] !== undefined) this[key] = Array.isArray(values[key]) ? [...values[key]] : values[key];
  }

  pushCamera() {
    const f32 = this.kern.f32, o = this.descPtr >> 2;
    f32.set(this.eye, o); f32.set(this.target, o + 3); f32.set(this.up, o + 6);
    f32[o + 9] = this.fov; f32[o + 10] = this.near; f32[o + 11] = this.far;
    if (!this.kern.exports.kern_camera_set(this.descPtr)) throw this.kern.fail("camera");
  }

  pushLighting() {
    const f32 = this.kern.f32, o = this.lightPtr >> 2;
    f32.set(this.sunDirection, o); f32.set(this.sunColor, o + 3); f32.set(this.skyColor, o + 6); f32.set(this.groundColor, o + 9);
    f32.set(this.fogColor, o + 12);
    f32[o + 15] = this.fogDensity; f32[o + 16] = this.exposure;
    f32[o + 17] = Number.isFinite(this.envIntensity) ? this.envIntensity : .9;
    f32.set(this.envZenith, o + 18); f32.set(this.envHorizon, o + 21); f32.set(this.envGround, o + 24);
    this.kern.exports.kern_lighting_set(this.lightPtr);
    const s = this.skyPtr >> 2, sky = this.sky;
    f32.set(this.skySun || this.sunDirection, s);
    f32.set(this.skyMoon || [.4, .5, .76], s + 3);
    f32[s + 6] = this.night || 0;
    f32[s + 7] = sky.clouds === false ? 0 : 1;
    f32.set(sky.blastDir || [0, 0, -1], s + 8);
    f32[s + 11] = Number.isFinite(sky.blastAge) ? sky.blastAge : -1;
    f32.set(sky.wind || [.012, .004], s + 12);
    this.kern.exports.kern_sky_set(this.skyPtr);
  }

  setPointLights(frame) {
    if (this.disposed) throw Error("renderer disposed");
    const n = MAX_POINT_LIGHTS * 4;
    if (!frame || !Number.isInteger(frame.count) || frame.count < 0 || frame.count > MAX_POINT_LIGHTS
      || !(frame.posRange instanceof Float32Array) || frame.posRange.length !== n
      || !(frame.colorIntensity instanceof Float32Array) || frame.colorIntensity.length !== n)
      throw Error("invalid point light data");
    const f32 = this.kern.f32, o = this.pointPtr >> 2;
    f32.set(frame.posRange, o);
    f32.set(frame.colorIntensity, o + n);
    if (!this.kern.exports.kern_point_lights_set(frame.count, this.pointPtr, this.pointPtr + n * 4)) throw this.kern.fail("point lights");
    this.pointLights.count = frame.count;
  }

  setShadows(options = null) {
    if (this.disposed) throw Error("renderer disposed");
    const o = options && options !== false ? options : null;
    if (!this.kern.exports.kern_shadows_set(o ? 1 : 0, o?.resolution ?? 512, o?.extent ?? 120, o?.bias ?? .0012)) throw this.kern.fail("shadows");
    this.shadowsEnabled = Boolean(o);
  }

  invalidate({ shadows = false } = {}) { this.kern.exports.kern_invalidate(shadows ? 1 : 0); }

  // ---- textures ----
  /** GPU texture from an image source (canvas, image, ImageData, bitmap); survives context loss. */
  createTexture(source, { mipmaps = true, wrap = "clamp" } = {}) {
    if (this.disposed) throw Error("renderer disposed");
    const kern = this.kern;
    const upload = () => {
      const gl = this.gl, texture = gl.createTexture();
      gl.bindTexture(gl.TEXTURE_2D, texture);
      gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, source);
      if (mipmaps) gl.generateMipmap(gl.TEXTURE_2D);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, mipmaps ? gl.LINEAR_MIPMAP_LINEAR : gl.LINEAR);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
      const w = wrap === "repeat" ? gl.REPEAT : gl.CLAMP_TO_EDGE;
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, w);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, w);
      gl.bindTexture(gl.TEXTURE_2D, null);
      return texture;
    };
    const slot = kern.addObject(upload());
    kern.pinned.set(slot, () => { kern.objects[slot] = upload(); });
    const id = kern.exports.kern_texture_external(slot);
    const handle = { id, slot, kernTexture: true };
    this.textures.add(handle);
    return handle;
  }

  deleteTexture(handle) {
    if (!this.textures.delete(handle)) return false;
    const kern = this.kern;
    kern.exports.kern_texture_delete(handle.id);
    kern.pinned.delete(handle.slot);
    if (!this.lost) this.gl.deleteTexture(kern.objects[handle.slot]);
    kern.releaseObject(handle.slot);
    return true;
  }

  // ---- meshes ----
  materialParams(material) {
    const params = new Int32Array(8);
    this.materialKeys.forEach((key, i) => {
      const v = material?.[key];
      params[i] = v === true ? 1 : Number.isFinite(v) ? v | 0 : 0;
    });
    return params;
  }

  /**
   * Creates or replaces mesh `id`.
   * input: {positions, normals?, colors?, uvs?, indices?, skin?: {joints, weights, bones},
   *         instances?: {transforms, tints?}, texture?, material?, visible?, ...extension streams}
   */
  upsert(id, input) {
    if (this.disposed) throw Error("renderer disposed");
    if (typeof id !== "string" || !id.trim()) throw Error("mesh needs ID");
    const kern = this.kern, e = kern.exports;
    const positions = input.positions;
    if (!(positions instanceof Float32Array)) throw Error("positions requires Float32Array / 3");
    for (const key of ["normals", "colors", "uvs"])
      if (input[key] != null && !(input[key] instanceof Float32Array)) throw Error(`${key} requires Float32Array`);
    const indices = input.indices ?? null;
    if (indices && !(indices instanceof Uint16Array || indices instanceof Uint32Array)) throw Error("invalid indices");
    const skin = input.skin ?? null;
    const transforms = asMatrices(input.instances?.transforms);
    if (input.instances?.transforms != null && !transforms) throw Error("empty instances");
    const tints = input.instances?.tints instanceof Float32Array ? input.instances.tints : null;
    const ext = this.extensionAttributes.map(a => input[a.key] ?? null);
    for (let i = 0; i < ext.length; i++) if (ext[i] && !(ext[i] instanceof Float32Array)) throw Error(`${this.extensionAttributes[i].key} requires Float32Array`);
    const joints = skin ? asFloat32(skin.joints) : null, weights = skin ? asFloat32(skin.weights) : null, bones = skin ? asFloat32(skin.bones) : null;
    const arrays = [positions, input.normals, input.colors, input.uvs, indices, joints, weights, bones, ext[0], ext[1], transforms, tints];
    this.staging.reset(arrays.reduce((n, a) => n + Staging.size(a), 0));
    const ptrs = arrays.map(a => this.staging.put(a));

    const u32 = kern.u32, f32 = kern.f32, i32 = kern.refresh().i32, d = this.descPtr >> 2;
    u32.fill(0, d, d + MD.WORDS);
    const span = (slot, k, a) => { u32[d + slot] = ptrs[k]; u32[d + slot + 1] = a?.length ?? 0; };
    span(MD.POSITIONS, 0, positions); span(MD.NORMALS, 1, input.normals); span(MD.COLORS, 2, input.colors); span(MD.UVS, 3, input.uvs);
    span(MD.INDICES, 4, indices); u32[d + MD.INDEX_BYTES] = indices?.BYTES_PER_ELEMENT ?? 4;
    span(MD.JOINTS, 5, joints); span(MD.WEIGHTS, 6, weights); span(MD.BONES, 7, bones);
    span(MD.EXT0, 8, ext[0]); span(MD.EXT1, 9, ext[1]); span(MD.TRANSFORMS, 10, transforms); span(MD.TINTS, 11, tints);
    const material = this.writeMaterial(d, input);
    if (input.weld === true) u32[d + MD.FLAGS] |= MF.WELD;

    return this.commitMesh(id, kid => e.kern_mesh_upsert(kid, this.descPtr), {
      material, visible: input.visible !== false, skinned: Boolean(bones?.length), instanceCount: transforms ? transforms.length / 16 : 1,
    });
  }

  /**
   * Writes the material, flags and texture words of a mesh descriptor at word
   * offset `d` (the geometry words are left untouched). Returns the material.
   */
  writeMaterial(d, input) {
    const u32 = this.kern.u32, f32 = this.kern.f32, i32 = this.kern.refresh().i32;
    const texture = input.texture;
    u32[d + MD.TEXTURE] = texture?.kernTexture ? texture.id : Number.isInteger(texture) ? texture : 0;
    const material = { color: [1, 1, 1, 1], alphaMode: "opaque", unlit: false, doubleSided: false, ...input.material };
    let flags = 0;
    if (input.visible !== false) flags |= MF.VISIBLE;
    if (material.unlit) flags |= MF.UNLIT;
    if (material.doubleSided) flags |= MF.DOUBLE_SIDED;
    if (material.castShadow !== false) flags |= MF.CAST_SHADOW;
    if (material.frustumCulled !== false) flags |= MF.FRUSTUM_CULLED;
    if (material.viewmodel === true) flags |= MF.VIEWMODEL;
    if (material.linearColors || LINEAR_COLOR_ARRAYS.has(input.colors)) flags |= MF.LINEAR_COLORS;
    u32[d + MD.FLAGS] = flags;
    u32[d + MD.ALPHA_MODE] = material.alphaMode === "blend" ? 2 : material.alphaMode === "mask" ? 1 : 0;
    const color = material.color ?? [1, 1, 1, 1];
    f32[d + MD.COLOR] = color[0] ?? 1; f32[d + MD.COLOR + 1] = color[1] ?? 1; f32[d + MD.COLOR + 2] = color[2] ?? 1; f32[d + MD.COLOR + 3] = color[3] ?? 1;
    f32[d + MD.ALPHA_CUTOFF] = Number.isFinite(material.alphaCutoff) ? material.alphaCutoff : .5;
    f32[d + MD.ROUGHNESS] = Number.isFinite(material.roughness) ? material.roughness : 1;
    f32[d + MD.METALNESS] = Number.isFinite(material.metalness) ? material.metalness : 0;
    i32.set(this.materialParams(material), d + MD.PARAMS);
    const emissive = material.emissive ?? [0, 0, 0];
    f32[d + MD.EMISSIVE] = emissive[0] || 0; f32[d + MD.EMISSIVE + 1] = emissive[1] || 0; f32[d + MD.EMISSIVE + 2] = emissive[2] || 0;
    return material;
  }

  /** Runs `create(kid)` for mesh `id` (new or replaced) and keeps the id bookkeeping consistent. */
  commitMesh(id, create, info) {
    const e = this.kern.exports;
    const existing = this.meshes.get(id);
    const kid = existing ? existing.kid : e.kern_mesh_alloc_id();
    if (!create(kid)) {
      if (!existing) e.kern_mesh_release_id(kid);
      throw this.kern.fail(`mesh ${id}`);
    }
    const record = existing ?? { id, kid };
    Object.assign(record, info);
    if (!existing) { this.meshes.set(id, record); this.byKid ??= new Map(); this.byKid.set(kid, record); }
    return record;
  }

  /** Material-only descriptor (for meshes built inside the engine). Returns the descriptor address. */
  materialDescriptor(input) {
    const d = this.descPtr >> 2;
    this.kern.u32.fill(0, d, d + MD.WORDS);
    this.writeMaterial(d, input);
    return this.descPtr;
  }

  // ---- geometry builders (run inside the engine) ----

  /** Starts an incremental footprint extrusion job (see kern/include/kern/geo/buildings.h). */
  beginBuildings(stream, options) { return this.beginGeoJob("kern_geo_buildings_begin", stream, options); }
  /** Starts an incremental road ribbon job (see kern/include/kern/geo/roads.h). */
  beginRoads(stream, options) { return this.beginGeoJob("kern_geo_roads_begin", stream, options); }

  beginGeoJob(entry, stream, options) {
    if (this.disposed) throw Error("renderer disposed");
    if (!(stream instanceof Float64Array) || !(options instanceof Float64Array)) throw Error("geometry job needs Float64Array stream and options");
    this.staging.reset(Staging.size(stream) + Staging.size(options));
    const sp = this.staging.put(stream), op = this.staging.put(options);
    const id = this.kern.exports[entry](sp, stream.length, op);
    if (!id) throw this.kern.fail("geometry job");
    return new KernGeoJob(this, id);
  }

  /**
   * Builds a terrain tile from the module's height sampler (kern.heightSampler)
   * and uploads it as mesh `id`. Returns {minHeight, maxHeight, bytes}.
   */
  upsertTerrainTile(id, { x0, y0, step = 5, cells = 32, albedo = [.369, .478, .243], material = {} } = {}) {
    const desc = this.materialDescriptor({ material });
    const out = this.resultPtr;
    this.commitMesh(id, kid => this.kern.exports.kern_geo_terrain_tile(kid, x0, y0, step, cells, albedo[0], albedo[1], albedo[2], desc, out),
      { material, visible: true, skinned: false, instanceCount: 1 });
    const s = this.kern.f64, o = out >> 3;
    return { minHeight: s[o], maxHeight: s[o + 1], bytes: s[o + 2] };
  }

  /** Replaces the instance streams; returns false when nothing changed. */
  updateInstances(id, instances) {
    const record = this.meshes.get(id);
    if (!record) throw Error("unknown mesh " + id);
    const transforms = asMatrices(instances?.transforms) ?? null;
    if (instances?.transforms != null && !transforms) throw Error("empty instances");
    const tints = instances?.tints instanceof Float32Array ? instances.tints : null;
    this.staging.reset(Staging.size(transforms) + Staging.size(tints));
    const tp = this.staging.put(transforms), cp = this.staging.put(tints);
    const r = this.kern.exports.kern_mesh_instances(record.kid, tp, transforms?.length ?? 0, cp, tints?.length ?? 0);
    if (r < 0) throw this.kern.fail(`instances ${id}`);
    if (transforms) record.instanceCount = transforms.length / 16;
    return r === 1;
  }

  updateBones(id, bones) {
    const record = this.meshes.get(id);
    if (!record?.skinned) throw Error("not a skinned mesh " + id);
    if (!(bones instanceof Float32Array) || bones.length % 16) throw Error("skin bones requires Float32Array / 16");
    this.staging.reset(Staging.size(bones));
    const p = this.staging.put(bones);
    if (!this.kern.exports.kern_mesh_bones(record.kid, p, bones.length)) throw this.kern.fail(`bones ${id}`);
  }

  setVisible(id, visible) {
    const record = this.meshes.get(id);
    if (!record) return false;
    const changed = Boolean(this.kern.exports.kern_mesh_visible(record.kid, visible !== false ? 1 : 0));
    record.visible = visible !== false;
    return changed;
  }

  remove(id) {
    const record = this.meshes.get(id);
    if (!record) return false;
    this.kern.exports.kern_mesh_remove(record.kid);
    this.meshes.delete(id);
    this.byKid?.delete(record.kid);
    return true;
  }

  // ---- frame ----
  render({ clearColor = [.045, .07, .12, 1], viewport = null, pageHidden = globalThis.document?.hidden === true } = {}) {
    if (this.disposed) throw Error("renderer disposed");
    if (this.lost) throw Error("GL context lost; waiting for restoration");
    const kern = this.kern;
    this.pushCamera();
    this.pushLighting();
    const u32 = kern.u32, f32 = kern.f32, p = this.descPtr >> 2;
    f32[p] = clearColor[0]; f32[p + 1] = clearColor[1]; f32[p + 2] = clearColor[2]; f32[p + 3] = clearColor[3];
    if (viewport) {
      if (![viewport.x, viewport.y, viewport.width, viewport.height].every(Number.isInteger)) throw Error("viewport must fit inside the drawing buffer");
      u32[p + RP.HAS_VIEWPORT] = 1; u32[p + RP.X] = viewport.x; u32[p + RP.Y] = viewport.y; u32[p + RP.W] = viewport.width; u32[p + RP.H] = viewport.height;
    } else u32[p + RP.HAS_VIEWPORT] = 0;
    u32[p + RP.PAGE_HIDDEN] = pageHidden ? 1 : 0;
    if (!kern.exports.kern_render(this.descPtr, this.resultPtr)) throw kern.fail("render");
    const s = kern.f64, o = this.resultPtr >> 3;
    const gpu = s[o + RS.GPU_MS];
    this.stats = {
      drawCalls: s[o + RS.DRAW_CALLS], shadowDrawCalls: s[o + RS.SHADOW_DRAW_CALLS], shadowResolution: s[o + RS.SHADOW_RESOLUTION],
      shadowReused: s[o + RS.SHADOW_REUSED] === 1, triangles: s[o + RS.TRIANGLES], instances: s[o + RS.INSTANCES],
      culledMeshes: s[o + RS.CULLED], visibleMeshes: s[o + RS.VISIBLE], cpuMs: +s[o + RS.CPU_MS].toFixed(3),
      gpuMs: gpu >= 0 ? gpu : null, bloom: BLOOM_STATES[s[o + RS.BLOOM]] ?? "off",
    };
    return this.stats;
  }

  // ---- queries ----
  getScreenRay(x, y) {
    this.pushCamera();
    if (!this.kern.exports.kern_screen_ray(x, y, this.resultPtr)) throw this.kern.fail("screen ray");
    const s = this.kern.f64, o = this.resultPtr >> 3;
    return { origin: [s[o], s[o + 1], s[o + 2]], direction: [s[o + 3], s[o + 4], s[o + 5]], near: s[o + 6], far: s[o + 7] };
  }

  /** Screen ray of an arbitrary camera for a width x height viewport (stereo eyes). */
  screenRayFor(camera, x, y, width, height) {
    if (!(width > 0 && height > 0) || !Number.isFinite(x) || !Number.isFinite(y)) throw Error("invalid viewport coordinates");
    const f32 = this.kern.f32, o = this.descPtr >> 2, up = camera.up || [0, 0, 1];
    f32.set(camera.eye, o); f32.set(camera.target, o + 3); f32.set(up, o + 6);
    f32[o + 9] = camera.fov ?? Math.PI / 3; f32[o + 10] = camera.near ?? .1; f32[o + 11] = camera.far ?? 1500;
    if (!this.kern.exports.kern_camera_screen_ray(this.descPtr, x, y, width, height, this.resultPtr)) throw this.kern.fail("screen ray");
    const s = this.kern.f64, r = this.resultPtr >> 3;
    return { origin: [s[r], s[r + 1], s[r + 2]], direction: [s[r + 3], s[r + 4], s[r + 5]], near: s[r + 6], far: s[r + 7] };
  }

  readPick(found) {
    const s = this.kern.f64, o = this.pickPtr >> 3;
    const result = { hit: null, trianglesExamined: s[o + 13], boundsRejected: s[o + 14], skippedSkinned: 0 };
    if (!found) return result;
    const record = this.byKid?.get(s[o]);
    result.hit = {
      id: record?.id ?? null, instance: s[o + 1], triangle: s[o + 2], distance: s[o + 3],
      point: [s[o + 4], s[o + 5], s[o + 6]], normal: [s[o + 7], s[o + 8], s[o + 9]], barycentric: [s[o + 10], s[o + 11], s[o + 12]],
    };
    return result;
  }

  pick(x, y, { maxDistance = this.far, ignoreId = null } = {}) {
    if (!Number.isFinite(x) || !Number.isFinite(y)) throw Error("invalid viewport coordinates");
    this.pushCamera();
    const ignore = ignoreId != null ? this.meshes.get(ignoreId)?.kid ?? 0 : 0;
    return this.readPick(this.kern.exports.kern_pick(x, y, maxDistance, ignore, this.pickPtr));
  }

  /** Picks along an explicit world ray ({origin, direction}). */
  pickRay(ray, { maxDistance = 1500, ignoreId = null } = {}) {
    if (ray?.origin?.length !== 3 || ray?.direction?.length !== 3 || !(maxDistance > 0)) throw Error("invalid pick ray");
    const f32 = this.kern.f32, o = this.descPtr >> 2;
    f32[o] = ray.origin[0]; f32[o + 1] = ray.origin[1]; f32[o + 2] = ray.origin[2];
    f32[o + 3] = ray.direction[0]; f32[o + 4] = ray.direction[1]; f32[o + 5] = ray.direction[2];
    const ignore = ignoreId != null ? this.meshes.get(ignoreId)?.kid ?? 0 : 0;
    return this.readPick(this.kern.exports.kern_pick_ray(this.descPtr, maxDistance, ignore, this.pickPtr));
  }

  meshInfo(id) {
    const record = this.meshes.get(id);
    if (!record || !this.kern.exports.kern_mesh_info(record.kid, this.resultPtr)) return null;
    const s = this.kern.f64, o = this.resultPtr >> 3;
    return { vertices: s[o], triangles: s[o + 1], instances: s[o + 2], visible: s[o + 3] === 1,
      worldBounds: { center: [s[o + 4], s[o + 5], s[o + 6]], radius: s[o + 7] } };
  }

  /** Copy of a mesh's CPU stream ("positions", "normals", "colors", "uvs", "transforms", "tints", "indices"). */
  meshStream(id, name) {
    const record = this.meshes.get(id);
    const which = ["positions", "normals", "colors", "uvs", "transforms", "tints", "indices"].indexOf(name);
    if (!record || which < 0) return null;
    const e = this.kern.exports, countPtr = this.resultPtr;
    const ptr = e.kern_mesh_stream(record.kid, which, countPtr);
    const count = this.kern.u32[countPtr >> 2];
    const Type = which === 6 ? Uint32Array : Float32Array;
    return new Type(this.kern.memory.buffer, ptr, count).slice();
  }

  dispose() {
    if (this.disposed) return;
    const kern = this.kern, e = kern.exports;
    for (const handle of [...this.textures]) this.deleteTexture(handle);
    e.kern_renderer_shutdown();
    e.kern_free(this.scratch);
    e.kern_free(this.pointPtr);
    if (this.staging.ptr) e.kern_free(this.staging.ptr);
    this.meshes.clear();
    this.byKid?.clear();
    this.disposed = true;
    this.ready = false;
    kern.renderer = null;
    this.canvas.removeEventListener?.("webglcontextlost", this.onLost);
    this.canvas.removeEventListener?.("webglcontextrestored", this.onRestored);
  }
}

/** An incremental geometry job running inside the engine. */
export class KernGeoJob {
  constructor(renderer, id) { this.renderer = renderer; this.kern = renderer.kern; this.id = id; this.freed = false; }

  /** Advances the job for at most `budgetMs`. Returns true when finished; throws on failure. */
  step(budgetMs = 4) {
    const state = this.kern.exports.kern_geo_step(this.id, budgetMs);
    if (state < 0) throw this.kern.fail("geometry job");
    return state === 1;
  }

  /** Runs to completion, yielding to the page between slices. Returns false when cancelled. */
  async run({ yieldFrame = null, budgetMs = 4, cancelled = () => false } = {}) {
    while (!this.step(budgetMs)) {
      if (yieldFrame) await yieldFrame();
      if (cancelled()) return false;
    }
    return true;
  }

  stats() {
    const out = this.renderer.resultPtr;
    this.kern.exports.kern_geo_stats(this.id, out);
    const s = this.kern.f64, o = out >> 3;
    return { processed: s[o], built: s[o + 1], skipped: s[o + 2], triangles: s[o + 3], chunks: s[o + 4] };
  }

  chunk(index) {
    const out = this.renderer.resultPtr;
    if (!this.kern.exports.kern_geo_chunk(this.id, index, out)) return null;
    const s = this.kern.f64, o = out >> 3;
    return { x: s[o], y: s[o + 1], items: s[o + 2], triangles: s[o + 3], hash: s[o + 4] };
  }

  /**
   * Welds chunk `index` and uploads it as mesh `id`. `extraKey` names the
   * extension stream that receives the builder's per-vertex record (e.g. "facade").
   * Returns {bytesBefore, bytesAfter}.
   */
  commit(index, id, { material = {}, extraKey = null, visible = true } = {}) {
    const r = this.renderer;
    const slot = extraKey ? r.extensionAttributes.findIndex(a => a.key === extraKey) : 0;
    const desc = r.materialDescriptor({ material, visible });
    const out = r.resultPtr + 64;
    r.commitMesh(id, kid => this.kern.exports.kern_geo_commit(this.id, index, kid, desc, Math.max(0, slot), out),
      { material, visible, skinned: false, instanceCount: 1 });
    const s = this.kern.f64, o = out >> 3;
    return { bytesBefore: s[o], bytesAfter: s[o + 1] };
  }

  free() {
    if (this.freed) return;
    this.freed = true;
    this.kern.exports.kern_geo_free(this.id);
  }
}

/** Module used by `new KernRenderer(canvas, options)` when none is passed. */
KernRenderer.defaultModule = null;
