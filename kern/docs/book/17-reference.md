# 17 Reference

This chapter is for looking things up. It lists the C ABI of the WebAssembly module (imports and
exports, grouped), the command line of `kern_lehrer`, the file formats and a glossary.
Explanations are in the respective chapters; here you find signatures, meanings and references.

---

## 17.1 The C ABI

### Basic rules

- Every exported function is `extern "C"` and is exported from the module under **the same name**.
  The macro is in `include/kern/api/c_api.h`:

  ```cpp
  #if defined(__wasm__)
  #define KERN_EXPORT(name) extern "C" __attribute__((export_name(#name), used))
  #else
  #define KERN_EXPORT(name) extern "C"
  #endif
  ```

  Natively (tests, Lehrer) the same functions are ordinary C functions.
- The module is a WASI reactor: the host calls **`_initialize` once**, then the exports (`kern.mjs`
  does this in `attach()`).
- Data passes through linear memory: the host allocates with `kern_alloc`, writes and passes
  pointers. Pointers are 32-bit.
- **Return value 1/0** means success/failure; on 0 the message is in `kern_last_error()`.
- **Strings** going in: (pointer, length) in bytes, UTF-8.
- **Result buffers** (JSON texts, byte buffers) belong to the module and remain valid until the next
  call in the same group; the length is returned by `kern_studio_result_length()` or
  `kern_lehrer_result_length()`.
- Which functions a module contains depends on the build switches
  ([Chapter 2](02-building-and-testing.md)): renderer, particles, geometry and logic are always
  present, `kern_app_*` only with `--app`, `kern_studio_*` only with `--studio`, `kern_lehrer_*`
  only with `--lehrer`.

### Imports: what the module needs from the host

| Module | Function | Purpose | Source |
|---|---|---|---|
| `kern_gl` | WebGL2 entry points (`createBuffer`, `bindBuffer`, `bufferData`, …, `drawElementsInstanced`, queries) | GPU objects are `u32` handles (0 = null), data as (pointer, bytes) | `include/kern/gfx/gl.h` |
| `kern_env` | `now()` | milliseconds, monotonic | `src/core/base.cpp` |
| `kern_env` | `log(level, ptr, length)` | log output | `src/core/base.cpp` |
| `kern_env` | `terrain_height(x, y)` → `f64` | height query for the geometry jobs | `src/api/geo_api.cpp` |
| `kern_env` | `status(ptr, length)` | status text of a C++ game | `src/app/app.cpp` |
| `kern_env` | `pointer_lock(lock)` | request/release pointer lock | `src/app/app.cpp` |
| `kern_env` | `logic_jit(ptr, length, id)` → `i32` | have the generated WebAssembly module of a logic graph compiled | `src/logic/logic_api.cpp` |
| `kern_env` | `logic_jit_release(slot)` | release a table slot | `src/logic/logic_api.cpp` |
| `wasi_snapshot_preview1` | `fd_write`, `random_get`, `clock_time_get`, `proc_exit`, `fd_close`, `fd_seek`, otherwise `ENOSYS` | minimal shim; the engine does not touch files | `kern/web/kern.mjs` |

The list of `kern_gl` functions is exactly what the engine uses. A new entry point must be added in
`gl.h` **and** in the `kern_gl` import object of `kern.mjs`.

### Weak and game-side symbols

| Symbol | Meaning |
|---|---|
| `extern "C" u32 kern_game_extensions(ShaderExtension*** out)` | A game in the module can supply its shader extensions; weak default version: none (`src/api/c_api.cpp`). Queried by `kern_renderer_init`. |
| `kern::App* kern_create_app()` | Must be defined by a game built with `--app` (`include/kern/app/app.h`); `kern_app_start` calls it. |
| `extern "C" void kern_set_native_height_sampler(f64 (*)(f64, f64))` | **native only**: replaces the `terrain_height` import (tests, tools). |

---

### Memory and errors (`src/api/c_api.cpp`)

| Function | Description |
|---|---|
| `void* kern_alloc(u32 bytes)` | `malloc` (at least 1 byte) |
| `void kern_free(void* p)` | `free` |
| `const char* kern_last_error()` | message of the last `fail()` |
| `void kern_clear_error()` | clear the message |

### Renderer (`src/api/c_api.cpp`)

All functions act on the process-wide renderer `c_api_renderer()` (used by `KernRenderer` in
`kern.mjs` and by the app runtime).

| Function | Description |
|---|---|
| `i32 kern_renderer_init(const u32* d)` | initialisation from a `KERN_ID_*` descriptor; queries `kern_game_extensions` |
| `void kern_renderer_shutdown()` | tear down the renderer |
| `i32 kern_renderer_resize(i32 w, i32 h)` | size of the drawing buffer |
| `u32 kern_material_key_count()`, `const char* kern_material_key(u32 i)` | names of the extensions' material parameters |
| `u32 kern_extension_attribute_count()`, `const char* kern_extension_attribute_key(u32 i)`, `u32 kern_extension_attribute_size(u32 i)` | the extensions' own vertex streams (name, components) |

**Meshes**

| Function | Description |
|---|---|
| `u32 kern_mesh_alloc_id()` | new mesh id; it belongs to the caller |
| `void kern_mesh_release_id(u32 id)` | release an id |
| `i32 kern_mesh_upsert(u32 id, const u32* d)` | create/replace a mesh from a `KERN_MD_*` descriptor; with `KERN_MF_WELD` a matching triangle soup is indexed first. Invalid input leaves the existing mesh unchanged |
| `i32 kern_mesh_instances(u32 id, const f32* t, u32 t_count, const f32* tint, u32 tint_count)` | set instances: **−1** error, **0** unchanged, **1** changed. Empty transforms mean "keep instances" (pure tint update) |
| `i32 kern_mesh_bones(u32 id, const f32* bones, u32 count)` | bone matrices for skinning |
| `i32 kern_mesh_visible(u32 id, i32 visible)` | visibility |
| `i32 kern_mesh_remove(u32 id)` | remove a mesh |
| `i32 kern_mesh_exists(u32 id)`, `u32 kern_mesh_count()` | queries |
| `i32 kern_mesh_info(u32 id, f64* out)` | `out[8]`: vertices, triangles, instances, visible, bounds centre xyz, bounds radius |
| `const void* kern_mesh_stream(u32 id, u32 stream, u32* count)` | read access to CPU streams: 0 positions, 1 normals, 2 colors, 3 uvs, 4 transforms, 5 tints (`f32`), 6 indices (`u32`) |

**Textures**

| Function | Description |
|---|---|
| `u32 kern_texture_external(u32 handle)` | register a texture whose GPU object is owned by the host (and restored by it in the same slot after a context loss) |
| `u32 kern_texture_create(const u8* rgba, i32 w, i32 h, i32 mipmaps, i32 repeat)` | engine texture from RGBA8 |
| `i32 kern_texture_delete(u32 id)` | delete |

**Scene state**

| Function | Description |
|---|---|
| `i32 kern_camera_set(const f32* c)` | 12 `f32`: eye xyz, target xyz, up xyz, fov, near, far; checks finiteness and ranges (`invalid camera`) |
| `void kern_lighting_set(const f32* f)`, `void kern_lighting_get(f32* f)` | lighting, `KERN_LI_*` (27 slots) |
| `void kern_atmosphere_set(f32 height_falloff, f32 sun_scatter, f32 horizon)` | height fog, sun scattering, horizon; all 0 = plain FogExp2 |
| `void kern_ao_set(i32 enabled, i32 quality, i32 half, f32 radius)` | GTAO: quality 0 low … 3 ultra, half resolution, radius (0 = default) |
| `void kern_sky_set(const f32* f)` | sky, `KERN_SK_*` (14 slots) |
| `i32 kern_point_lights_set(u32 count, const f32* pos_range, const f32* color_intensity)` | up to 6 point lights; 4 `f32` each (xyz + range, rgb + intensity) |
| `i32 kern_shadows_set(i32 enabled, i32 resolution, f32 extent, f32 bias)` | sun shadows on/off and parameters |
| `void kern_invalidate(i32 shadows)` | mark the scene as changed; with `shadows` the cached shadow map is invalidated too |

**Frame, queries, context**

| Function | Description |
|---|---|
| `i32 kern_render(const u32* p, f64* out)` | one frame; parameters `KERN_RP_*`, statistics `KERN_RS_*` |
| `i32 kern_screen_ray(f32 x, f32 y, f64* out)` | ray through a pixel of the current camera; `out[8]`: origin xyz, direction xyz, near, far |
| `i32 kern_camera_screen_ray(const f32* c, f32 x, f32 y, f32 w, f32 h, f64* out)` | the same for an explicit camera (12 `f32`) and viewport size |
| `i32 kern_pick(f32 x, f32 y, f32 max_distance, u32 ignore_id, f64* out)` | exact triangle picking under the pixel; result `KERN_PK_*` |
| `i32 kern_pick_ray(const f32* ray, f32 max_distance, u32 ignore_id, f64* out)` | picking along a ray (origin xyz, direction xyz) |
| `void kern_context_lost()`, `i32 kern_context_restored()` | report WebGL context loss / restoration (rebuilds programs, reloads meshes) |

### Descriptors (`include/kern/api/c_api.h`)

The offsets must match the constants in `kern/web/kern.mjs` (`MD`, `MF`, `ID`, `RP`, `RS`,
`LI_SLOTS`, `SK_SLOTS`, `PK_SLOTS`).

**Mesh descriptor `KERN_MD_*`** (46 `u32` words; streams as a (pointer, count) pair)

| Offset | Field | Offset | Field |
|---|---|---|---|
| 0 | `POSITIONS` (ptr, number of floats) | 21 | `TRANSFORMS` |
| 2 | `NORMALS` | 23 | `TINTS` |
| 4 | `COLORS` | 25 | `TEXTURE` |
| 6 | `UVS` | 26 | `FLAGS` (`KERN_MF_*`) |
| 8 | `INDICES` (ptr, number of indices) | 27 | `ALPHA_MODE` (0 opaque, 1 mask, 2 blend) |
| 10 | `INDEX_BYTES` (2 or 4) | 28 | `COLOR` (4 × `f32`) |
| 11 | `JOINTS` | 32 | `ALPHA_CUTOFF` (`f32`) |
| 13 | `WEIGHTS` | 33 | `ROUGHNESS` (`f32`) |
| 15 | `BONES` | 34 | `METALNESS` (`f32`) |
| 17 | `EXT0` | 35 | `PARAMS` (8 × `i32`, material parameters of the extensions) |
| 19 | `EXT1` | 43 | `EMISSIVE` (3 × `f32`, linear) |

**Mesh flags `KERN_MF_*`**

| Bit | Name | Meaning |
|---|---|---|
| 1 << 0 | `VISIBLE` | visible |
| 1 << 1 | `UNLIT` | without lighting |
| 1 << 2 | `DOUBLE_SIDED` | double-sided |
| 1 << 3 | `CAST_SHADOW` | casts shadows |
| 1 << 4 | `FRUSTUM_CULLED` | is culled |
| 1 << 5 | `VIEWMODEL` | viewmodel layer (after a depth clear, e.g. gizmos) |
| 1 << 6 | `LINEAR_COLORS` | vertex colours are linear (no sRGB decoding) |
| 1 << 7 | `WELD` | index a triangle soup before uploading |
| 1 << 8 | `LINES` | line list instead of triangles (always unlit, no shadow, not pickable) |

**Init descriptor `KERN_ID_*`** (9 words): 0 `WIDTH`, 1 `HEIGHT`, 2 `SHADOWS`,
3 `SHADOW_RESOLUTION`, 4 `SHADOW_EXTENT` (`f32`), 5 `SHADOW_BIAS` (`f32`), 6 `BLOOM`,
7 `BLOOM_FORCED`, 8 `GRADE`.

**Render parameters `KERN_RP_*`** (10 words): 0 `CLEAR` (4 × `f32`), 4 `HAS_VIEWPORT`, 5 `X`,
6 `Y`, 7 `W`, 8 `H`, 9 `PAGE_HIDDEN`.

**Render statistics `KERN_RS_*`** (11 `f64`): `DRAW_CALLS`, `SHADOW_DRAW_CALLS`,
`SHADOW_RESOLUTION`, `SHADOW_REUSED`, `TRIANGLES`, `INSTANCES`, `CULLED`, `CPU_MS`, `GPU_MS`,
`BLOOM` (state; in `kern.mjs`: `off`, `active`, `glow-off-slow-device`, `unsupported`),
`VISIBLE`.

**Lighting `KERN_LI_*`** (27 `f32`): 0 `SUN_DIRECTION`, 3 `SUN_COLOR`, 6 `SKY_COLOR`,
9 `GROUND_COLOR`, 12 `FOG_COLOR`, 15 `FOG_DENSITY`, 16 `EXPOSURE`, 17 `ENV_INTENSITY`,
18 `ENV_ZENITH`, 21 `ENV_HORIZON`, 24 `ENV_GROUND`.

**Sky `KERN_SK_*`** (14 `f32`): 0 `SUN`, 3 `MOON`, 6 `NIGHT`, 7 `CLOUDS` (> 0.5 = on),
8 `BLAST_DIRECTION`, 11 `BLAST_AGE` (< 0: no blast wave), 12 `WIND` (2 values).

**Pick result `KERN_PK_*`** (15 `f64`): 0 `MESH`, 1 `INSTANCE`, 2 `TRIANGLE`, 3 `DISTANCE`,
4 `POINT` (xyz), 7 `NORMAL` (xyz), 10 `BARYCENTRIC` (3), 13 `TRIANGLES_EXAMINED`,
14 `BOUNDS_REJECTED`.

### Particles (`src/api/fx_api.cpp`)

One particle system per module; it draws into a mesh that the host has reserved beforehand
([Chapter 4](04-graphics.md)).

| Function | Description |
|---|---|
| `i32 kern_fx_init(u32 mesh_id, u32 capacity)` | create (or re-create) the system |
| `u32 kern_fx_emit(const f64* s)` | one particle; 16 `f64`: position xyz, velocity xyz, rgba, start_ms, life_ms, size, gravity, grow, fade_extra. Returns: id, 0 on budget overflow or if invalid |
| `void kern_fx_tick(f64 now_ms, f32 eye_x, f32 eye_y, f32 eye_z, u32* out)` | advance and sort; `out[5]`: active, dropped, uploaded, expired, mesh visible |
| `u32 kern_fx_active()`, `u32 kern_fx_dropped()` | counters |
| `void kern_fx_clear()` | remove all |

### Geometry (`src/api/geo_api.cpp`)

Incremental jobs for buildings and roads plus synchronous terrain tiles; results go welded straight
into the renderer ([Chapter 5](05-scene-and-geometry.md)).

| Function | Description |
|---|---|
| `u32 kern_geo_buildings_begin(const f64* stream, u32 length, const f64* o, u32 option_count)` | building job; options: chunk_size, max_buildings, cornice, wall_records, trim rgb, trim_mix, wall_count, roof_count, walls rgb…, roofs rgb…. Returns the job id, 0 on error |
| `u32 kern_geo_roads_begin(const f64* stream, u32 length, const f64* o, u32 option_count)` | road job; options: center x/y, radius, chunk_size, segment_length, curb_extra, curb_z, surface_z, marking_z, marking_width, dash_start, dash_length, dash_period, uv_scale, curb rgb, palette_count, palette rgb… |
| `i32 kern_geo_step(u32 id, f64 budget_ms)` | works for at most `budget_ms`: 1 done, 0 continue, −1 error |
| `i32 kern_geo_stats(u32 id, f64* out)` | processed, built, skipped, triangles, number of chunks |
| `i32 kern_geo_chunk(u32 id, u32 index, f64* out)` | grid x, y, elements, triangles, content hash (never 0) |
| `i32 kern_geo_commit(u32 id, u32 index, u32 mesh_id, const u32* desc, u32 extra_slot, f64* out)` | weld a chunk and upload it as a mesh (material from `KERN_MD_*`); `out`: bytes before/after welding; the chunk memory is freed afterwards |
| `void kern_geo_free(u32 id)` | release a job |
| `i32 kern_geo_terrain_tile(u32 mesh_id, f64 x0, f64 y0, f64 step, u32 cells, f64 r, f64 g, f64 b, const u32* desc, f64* out)` | terrain tile from the height query; `out`: min, max height, bytes |
| `const void* kern_geo_chunk_stream(u32 id, u32 index, u32 stream, u32* count)` | streams of a chunk (0 positions, 1 normals, 2 colors, 3 uvs, 4 extra, 5 indices) – tools and tests |
| `i32 kern_geo_weld(u32 id, u32 index)` | weld a chunk without uploading – tools and tests |
| `i32 kern_geo_terrain_probe(f64 x0, f64 y0, f64 step, u32 cells, f64 r, f64 g, f64 b)`, `const void* kern_geo_terrain_probe_stream(u32 stream, u32* count)` | terrain tile into a probe buffer – tools and tests |

### App runtime (`src/app/app.cpp`, only with `--app`)

Driven by the host `kern/web/app.mjs` ([Chapter 12](12-your-own-games.md)).

| Function | Description |
|---|---|
| `void kern_app_set_args(const char* text, u32 length)` | start parameters before the start (`k=v&k2=v2`, URL-encoded) |
| `i32 kern_app_start(i32 width, i32 height)` | `kern_create_app()`, `configure()`, renderer init, `start()` |
| `i32 kern_app_frame(f64 now_ms, i32 page_hidden)` | one frame: `update()` (dt clamped to 0…0.1 s), then rendering; 0 if rendering fails |
| `i32 kern_app_resize(i32 width, i32 height)` | size; calls `resized()` |
| `void kern_app_key(u32 key, i32 down)` | key (`kern::Key`) |
| `void kern_app_pointer(f32 x, f32 y, f32 dx, f32 dy, u32 buttons)` | pointer position, movement, button bits |
| `void kern_app_wheel(f32 dy)` | mouse wheel |
| `void kern_app_touch(i32 id, i32 phase, f32 x, f32 y)` | finger; phase 0 begins, 2 ends, otherwise movement |
| `void kern_app_focus(i32 focused)` | focus; on loss all keys are released |
| `void kern_app_stop()` | `stop()` and tear down the renderer |

### Kern Studio (`src/studio/api.cpp`, only with `--studio`)

The page `kern/web/studio/` and exported games (`game.mjs`) talk to the editor through these
functions. JSON responses come back as pointers (valid until the next call, length via
`kern_studio_result_length()`); before a successful initialisation almost all functions return
`nullptr` or 0 (exception: `kern_studio_load_policy` registers policies even beforehand). The
commands for `kern_studio_command` are described in [Chapter 11](11-studio.md) (source:
`docs/studio.md`, section "Protokoll").

**Lifecycle and rendering**

| Function | Description |
|---|---|
| `i32 kern_studio_init(i32 width, i32 height, i32 player_only)` | renderer (shadows 2048 px, extent 110, bias 0.0014, bloom, extension `NightWindows`) and Studio; `player_only = 1` starts the runtime of exported games without the editor |
| `void kern_studio_ao_set(i32 enabled, i32 quality, i32 half, f32 radius)` | GTAO like `kern_ao_set` |
| `void kern_studio_shadow_filter(i32 taps)` | PCF taps of the sun shadow: 1, 4 (phones) or 9 (desktop); the page chooses by device, `?shadowFilter=1\|4\|9` overrides |
| `i32 kern_studio_resize(i32 width, i32 height)` | drawing buffer |
| `void kern_studio_frame(f64 now_ms)` | cameras, simulation, overlays, rendering of all visible viewports (not while the context is lost) |
| `void kern_studio_set_viewport(u32 index, i32 x, i32 y, i32 w, i32 h, f32 css_w, f32 css_h, f32 dpr, i32 visible)` | rectangle of a viewport in the drawing buffer (origin bottom left) |
| `void kern_studio_context_lost()`, `i32 kern_studio_context_restored()` | context loss |

**Protocol**

| Function | Description |
|---|---|
| `const char* kern_studio_command(const char* json, u32 length)` | a command `{"cmd": …}`; response `{"ok": …}`; unreadable JSON yields `{"ok":false,"error":"invalid command: …"}` (depth up to 120) |
| `const char* kern_studio_state()` | full panel state (outliner, details, menus …) |
| `const char* kern_studio_status()` | per-frame values (HUD, statistics, camera, overlays) |
| `f64 kern_studio_revision()` | changes when `state()` needs to be read again |
| `const char* kern_studio_take_ui()` | requests to the page (capture pointer, clipboard, dialogs, save, open, package game …) |
| `u32 kern_studio_result_length()` | length of the last result |

**Input**

| Function | Description |
|---|---|
| `const char* kern_studio_pointer(u32 viewport, u32 type, f32 x, f32 y, i32 button, i32 buttons, u32 modifiers, f32 wheel)` | pointer; `type` 0 down, 1 move, 2 up, 3 cancel, 4 wheel, 5 leave; modifiers 1 Shift, 2 Ctrl/Meta, 4 Alt |
| `i32 kern_studio_touch(u32 type, i32 id, f32 x, f32 y, f64 time_ms)` | finger/pen in the active viewport (0 down, 1 move, 2 up, 3 cancel); 1 if the touch controls used it |
| `void kern_studio_look(f32 dx, f32 dy)` | mouse look with pointer lock |
| `const char* kern_studio_key(const char* key, u32 key_length, const char* code, u32 code_length, i32 down, u32 modifiers, i32 repeat)` | keyboard (`KeyboardEvent.key`/`.code`); response `{"handled": bool}` |
| `void kern_studio_blur()` | the window has lost focus |

**Textures, policies, sound**

| Function | Description |
|---|---|
| `const char* kern_studio_pending_textures()` | textures that need decoded pixels: `["tex1", …]` |
| `const char* kern_studio_texture_source(const char* id, u32 length)` | source of a texture |
| `i32 kern_studio_texture_pixels(const char* id, u32 length, i32 width, i32 height, const u8* rgba)` | hand over decoded pixels |
| `const char* kern_studio_pending_policies()` | Micro Duck policy files still missing (from `manifest.json`, plus `jump.onnx` and `play.onnx`) – only if the level contains a walker |
| `i32 kern_studio_load_policy(const char* file, u32 length, const u8* data, u32 bytes)` | an `.onnx` file; 1 if it is an rsl_rl actor that Kern can evaluate |
| `i32 kern_studio_audio_init(u32 sample_rate)` | start the mixer at the output device's rate |
| `const f32* kern_studio_audio_render(u32 frames)` | `frames` (at most 16384) stereo frames, interleaved |
| `i32 kern_studio_audio_prepare(f64 budget_ms)` | pre-compute built-in sounds for about `budget_ms` (default 2 ms) |
| `const char* kern_studio_pending_sounds()`, `const char* kern_studio_sound_source(const char* id, u32 length)` | imported sounds that need PCM |
| `i32 kern_studio_sound_pcm(const char* id, u32 length, const f32* samples, u32 frames, u32 channels, u32 rate)` | decoded PCM (1 or 2 channels, any rate) |
| `void kern_studio_sound_failed(const char* id, u32 length, const char* reason, u32 reason_length)` | decoding failed |

**Networking** (the page only moves datagrams; `KernNet` in `game.mjs`, [Chapter 10](10-networking.md))

| Function | Description |
|---|---|
| `void kern_studio_net_push(f64 from, const u8* data, u32 bytes)` | incoming datagram from peer `from` (53-bit id of the page, > 0) |
| `u32 kern_studio_net_take()` | next outgoing datagram: size (0 = none) |
| `const u8* kern_studio_net_out()`, `f64 kern_studio_net_out_to()` | its bytes and destination |

**Autosave** (`docs/studio.md`, "Wiederherstellen" – restoring)

| Function | Description |
|---|---|
| `const char* kern_studio_take_autosave(f64 now_ms)` | level text if a backup is due and there are unsaved changes; otherwise `nullptr` |
| `i32 kern_studio_take_autosave_discard()` | 1 (once) if the stored backup no longer contains unsaved work and should be deleted |
| `i32 kern_studio_dirty()` | the document has unsaved changes |

### Lehrer (`src/lehrer/lehrer_api.cpp`, only with `--lehrer`)

One module per web worker; a worker is a collector and/or learner ([Chapter 14](14-lehrer.md)).
Results are pointers with `kern_lehrer_result_length()` bytes, valid until the next call.

| Function | Description |
|---|---|
| `u32 kern_lehrer_result_length()` | bytes of the last result |
| `i32 kern_lehrer_create(const char* task, u32 task_length, i32 envs, u32 seed, i32 first)` | environments `first … first+envs−1` of a run (`jump`, `stand`) with its recipe; worker w of k gets `first = w × envs` |
| `void kern_lehrer_progress(i32 iteration)` | before iteration `iteration` (from 1): curricula and pushes according to the recipe |
| `i32 kern_lehrer_set_policy(const f32* data, u32 count)` | policy to act with (`Policy::serialize`, `count` floats) |
| `const u8* kern_lehrer_collect(i32 steps)` | `steps` steps of each environment; returns a serialised rollout |
| `i32 kern_lehrer_learner(const char* task, u32 task_length, const char* config, u32 config_length, u32 seed)` | learner with recipe; JSON overrides `learningRate`, `initStd`, `entropyCoef`, `desiredKl`, `steps`, `epochs`, `minibatches`, `warmStart` (`"velstand"` \| `"none"`), `imitate` |
| `i32 kern_lehrer_steps()` | steps per environment and iteration of the recipe |
| `const char* kern_lehrer_evaluate(i32 episodes)` | `jump` only: JSON `jumps`, `episodes`, `heightMm`, `riseMm`, `tiltDeg`, `flightS`, `falls` |
| `const u8* kern_lehrer_dagger(i32 round, i32 first, i32 count)` | a collector's share of an imitation round: `u32` ticks, then ticks × 61 observations and ticks × 14 teacher actions (`f32`) |
| `i32 kern_lehrer_imitation_episodes()`, `i32 kern_lehrer_imitation_left()` | episodes per round; remaining rounds |
| `const char* kern_lehrer_imitate(const u8* data, u32 bytes, u32 count)` | one imitation round from `count` shares (each preceded by its `u32` size; `count = 0`: the learner computes the round itself); after the last one, practice begins. JSON: `round`, `rounds`, `samples`, `error` and the jump values |
| `const f32* kern_lehrer_policy()` | the learner's current policy (length in bytes) |
| `const char* kern_lehrer_learn(const u8* data, u32 bytes, u32 count)` | learns from `count` rollouts (each preceded by its `u32` size); JSON: `iteration`, `steps`, `meanReward`, `meanReturn`, `meanLength`, `episodes`, `valueLoss`, `surrogateLoss`, `entropy`, `learningRate`, `meanStd`, `collectSeconds`, `learnSeconds` |
| `const u8* kern_lehrer_export_onnx()` | actor as ONNX in the format of Pollen's networks |
| `const u64* kern_lehrer_fingerprint(i32 ticks)` | determinism fingerprint, one `u64` per control tick |

### Logic (`src/logic/logic_api.cpp`)

These functions are called by **generated code**, not by the host: a JIT-compiled graph imports them
as `env.call_node`, `env.read_pin` and `env.trace` (wasm-to-wasm calls), and the host reports the
finished table slot with `kern_logic_jit_ready` ([Chapter 9](09-logic.md)).

| Function | Description |
|---|---|
| `i32 kern_logic_call_node(i32 id, i32 node, i32 entry)` | run the C++ function of a node of instance `id` |
| `f64 kern_logic_read_pin(i32 id, i32 node, i32 pin)` | read a data pin |
| `void kern_logic_trace(i32 id, i32 node)` | report the execution of a node (debugger) |
| `void kern_logic_jit_ready(i32 id, i32 function)` | host: the module for instance `id` is in table slot `function`; −1 = failed, the graph stays in the interpreter |

---

## 17.2 Command line of `kern_lehrer`

`kern_lehrer` is the Lehrer (Kern's trainer) as a native program (`kern/tools/kern_lehrer.cpp`,
built with `kern/tools/lehrer.sh` into `kern/build/kern_lehrer`;
[Chapter 2](02-building-and-testing.md), [Chapter 14](14-lehrer.md)). Most of its options keep
their German names; their meaning is given below.

### Flow and modes of operation

After reading all options, the program loads Pollen's policies (`--policies`) and picks the
**first** applicable mode in this order:

| # | Condition | Mode |
|---|---|---|
| 1 | `--fingerprint N` | determinism fingerprint: N lines `<tick> <hash>`, then exit |
| – | (check) | `--task` must be `jump` or `stand`, otherwise `unknown task …`, exit code 2 |
| 2 | `--eval FILE` | jump evaluation of a network |
| 3 | `--trace FILE` | one episode tick by tick |
| 4 | `--kalibrieren LIST` | servo calibration |
| 5 | `--abgleich FILE` | matching against a robot log |
| 6 | `--beispiele DIR` | write example logs |
| 7 | `--vorbild-suchen FILE` | search for a jump demonstration (CMA-ES) |
| 8 | `--vorbild-pruefen X` | evaluate a demonstration |
| 9 | `--spielen-pruefen X` | evaluate a play network or demonstration |
| 10 | `--spielen-ueben FILE` | practise a play network (PPO) |
| 11 | `--spielen-spur` | one play episode, decision by decision |
| 12 | `--biss-fenster` | measure where the beak grips a sock |
| 13 | `--halt-messen` | measure how far the duck keeps walking after "stop" |
| 14 | `--demo-spur` | play the Studio's Micro Duck example level |
| 15 | `--spielen` | play network through imitation (DAgger) |
| 16 | (otherwise) | **training** of `--task` |

`--spielen`, `--spielen-spur`, `--biss-fenster`, `--halt-messen` and `--demo-spur` set the same
internal switch; the last one on the command line wins. An unknown option prints the short help and
exits with code 2.

### Options

**General**

| Option | Default | Meaning |
|---|---|---|
| `--task jump\|stand` | `jump` | task (training, `--trace`) |
| `--threads N` | hardware threads | compute threads |
| `--envs N` | 16 × threads | ducks in total; rounded down to a multiple of the threads (at least 1 per thread). The result depends on the number of ducks, not the number of threads |
| `--seed N` | 1 | seed. With `--trace` and `--spielen-spur` the episode seed (internally 1000 + N); with `--demo-spur` the **duration in seconds** (values above 1; without `--seed`, or with 0 or 1, it is 120 s) |
| `--iterations N` | 600 | training: last iteration. With `--kalibrieren` generations (default 80), `--vorbild-suchen` generations (default 120), `--spielen-ueben` iterations (default 300) – in each case unless a value other than 600 is given |
| `--episodes N` | 10 | episodes for `--eval`, `--vorbild-pruefen`, `--spielen-pruefen` |
| `--out FILE` | `<task>.onnx` | output. `--kalibrieren`: `<profile>_calibrated.json`; `--spielen`: `play.onnx`; `--spielen-ueben`: `play_geuebt.onnx` |
| `--policies DIR` | `kern/third_party/microduck/` (relative to the source path at build time) | Pollen's ONNX files |
| `--profil FILE` | XL330 | servo profile (JSON) for training, start value of calibration, matching |

**Training (PPO) – overrides the recipe** (`recipe.h`; [Chapter 14](14-lehrer.md))

| Option | Recipe `jump` | Recipe `stand` | Meaning |
|---|---|---|---|
| `--steps N` | 48 | 24 | steps per duck and iteration |
| `--init-std X` | 0.3 | 1.0 | initial action standard deviation |
| `--lr X` | 1e-4 | 1e-3 | learning rate |
| `--entropy X` | 0.005 | 0.01 | entropy coefficient |
| `--critic A,B,…` | 256,128 | 256,128 | critic layers |
| `--actor A,B,…` | 512,256,128 | 512,256,128 | actor layers |
| `--epochs N` | 5 | 5 | epochs per update |
| `--minibatches N` | 4 | 4 | minibatches per epoch |
| `--adaptive-lr 0\|1` | 1 | 1 | adapt the learning rate from the KL divergence |
| `--kl X` | 0.01 | 0.01 | target KL of the adaptive learning rate |
| `--substeps N` | 16 | 16 | Box3D sub-steps (`game::kWalkerSubsteps`) |
| `--warm-start NAME` | `velstand` | `none` | `velstand`: the actor starts as Pollen's `velstand` network; any other value means no warm start (`make_learner` in `src/lehrer/recipe.cpp`) |

**Demonstration (Vorbild) and imitation in training**

| Option | Default (`jump`) | Meaning |
|---|---|---|
| `--vorbild builtin\|demo.json\|none` | `builtin` | demonstration to imitate; `none` switches imitation off |
| `--nachahmen N` | 8 | DAgger rounds ("imitate"; with `--spielen`: default 8) |
| `--episoden N` | 96 | episodes per round (with `--spielen`: default 64) |
| `--uebungs-std X` | 0.1 | action noise during practice after imitation (with `--spielen-ueben`: default 0.1) |
| `--nachahmungs-lohn X` | 10 | weight of the reward for acting like the demonstration during practice ("imitation reward") |

**Output and resuming in training**

| Option | Default | Meaning |
|---|---|---|
| `--log FILE` | – | CSV per iteration: `iteration,steps,seconds,mean_reward,mean_return,mean_length,episodes,value_loss,entropy,mean_std,learning_rate,steps_per_second,jumps,height_mm,flight_s,falls` (appended to when resuming) |
| `--checkpoint FILE` | – | the whole learner every `--save-every` iterations and at the end; after imitation additionally `FILE.nachgeahmt` ("imitated") |
| `--resume FILE` | – | resume from the checkpoint (without repeating imitation) |
| `--save-every N` | 25 | interval for ONNX and checkpoint |
| `--keep-every N` | 0 (off) | snapshots `<stem>_itNNNNN.onnx` |
| `--eval-every N` | 10 | for `jump`: 5 evaluation episodes every N iterations; at 4 of 5 real jumps or more the program reports `FIRST REAL JUMP` and writes `--out` immediately |
| `--stop-after-jump` | off | stop after the first real jump (the file is not overwritten afterwards) |

With imitation, training additionally writes `<stem>_nachgeahmt.onnx` (the network after imitation,
before practice).

**Evaluating and tracing**

| Option | Meaning |
|---|---|
| `--fingerprint N` | fingerprint over N control ticks (reference: `kern/tests/determinism_fingerprint.txt` with 400) |
| `--eval FILE` | jump of a network: clean robot and full variation (build, servos, sensors, start) |
| `--trace FILE\|builtin\|demo.json` | one clean episode tick by tick: height, foot contacts, tilt, reward terms; with `builtin` or `.json` the demonstration drives |
| `--randomized` | for `--trace` and `--spielen-spur`: with variation |

**Calibration** ([Chapter 16](16-calibration.md))

| Option | Meaning |
|---|---|
| `--kalibrieren a.csv,b.csv` | test-bench logs to fit to ("calibrate") |
| `--pruefen c.csv[,…]` | held-out check logs ("check") |
| `--abgleich robot.csv` | robot log against the simulation (gyro, gravity, joints, per joint) ("matching") |
| `--beispiele DIR` | writes `servo_steps.csv`, `servo_chirp.csv`, `robot.csv` and `xl330.json` (generated in the simulation) ("examples") |

**Demonstration**

| Option | Meaning |
|---|---|
| `--vorbild-suchen demo.json` | new jump demonstration with CMA-ES; optionally starting from `--vorbild` ("search demonstration") |
| `--vorbild-pruefen builtin\|demo.json` | evaluate a demonstration clean and with variation, plus a score over 10 episodes ("check demonstration") |

**Play** (ball and sock; [Chapter 14](14-lehrer.md))

| Option | Meaning |
|---|---|
| `--spielen` | play network through imitation of the demonstration (DAgger); `--out` is written after each round ("play") |
| `--spielen-pruefen vorbild\|play.onnx` | evaluate the demonstration or a network (clean and with variation) ("check play") |
| `--spielen-ueben play.onnx` | practise a network with PPO (half the ducks clean, half with variation; the critic warms up for 20 iterations); every 20 iterations an evaluation and `<stem>_itNNNN.onnx` ("practise play") |
| `--spielen-spur` | one episode, decision by decision; with `--netz play.onnx` the network drives and the line shows both answers ("play trace") |
| `--netz play.onnx` | play network for `--spielen-spur` and `--demo-spur` ("network") |
| `--biss-fenster` | where the beak gets hold of a sock ("bite window") |
| `--halt-messen` | how far the duck keeps walking after "stop" ("measure stopping") |
| `--demo-spur` | the Studio's Micro Duck example level with 60 Hz frames, one line per second with state, observation and the play network's answer; networks from `kern/policies/`, `--seed` is the duration in seconds (default 120). The environment variable `KERN_JUMP_AT=<s>` triggers the jump in second s (key 7) ("demo trace") |

### Examples

```sh
kern/tools/lehrer.sh
kern/build/kern_lehrer --task jump --iterations 3000 --out jump.onnx --log jump.csv --checkpoint run.ckpt
kern/build/kern_lehrer --eval jump.onnx --episodes 40
kern/build/kern_lehrer --trace jump.onnx
kern/build/kern_lehrer --vorbild-suchen demo.json
kern/build/kern_lehrer --kalibrieren steps.csv,chirp.csv --pruefen square.csv --out xl330_unit3.json
kern/build/kern_lehrer --abgleich robot.csv --profil xl330_unit3.json
kern/build/kern_lehrer --task jump --profil xl330_unit3.json --out jump.onnx
kern/build/kern_lehrer --fingerprint 400 | diff - kern/tests/determinism_fingerprint.txt
```

---

## 17.3 File formats

### `.klevel` – levels

A level is JSON with `"format": "kern-level"` and `"version": 1`. The writer
(`to_json(const Level&)` in `src/game/level.cpp`) creates these top-level keys:

| Key | Contents |
|---|---|
| `format`, `version` | `"kern-level"`, `1` |
| `name` | name of the level |
| `world` | World Settings |
| `camera` | editor camera: `position`, `yaw`, `pitch` |
| `nextId` | next free actor id |
| `actors` | actors with transform and components |
| `textures` | embedded textures: `id`, `name`, `width`, `height`, `src` |
| `sounds` | imported sounds: `id`, `name`, `duration`, `src` |
| `logic` | level graph (only if present) |

Units: metres, seconds, kilograms, Z up; rotations as Euler angles in degrees (x roll, y pitch,
z yaw; applied as yaw × pitch × roll); colours sRGB 0…1. A different `format` is rejected ("not a
Kern level"), as is a higher `version` ("level was written by a newer Kern Studio"). Components and
fields are described in [Chapter 8](08-game-runtime.md).

### ONNX policies

Kern reads and writes **rsl_rl actors** in the format Pollen Robotics exports for the Micro Duck
(`include/kern/game/walker_policy.h`, `include/kern/lehrer/onnx_export.h`).

**Graph** (opset 17, IR version 8):

```
obs [1, n] → Sub(obs_mean) → Div(obs_div) → (Gemm, Elu(alpha 1))* → Gemm → actions [1, m]
```

- Input `obs`, output `actions`, `float32`. The Gemm weights are stored as `[out][in]` with
  `transB = 1`.
- The normaliser is part of the graph: `(obs − mean) / div` (according to `docs/lehrer.md`,
  `div` = σ + 0.01).
- Motion networks have 61 observations and 14 actions (Pollen's `velstand`: 61 → 512 → 256 → 128 →
  14). The play network `play.onnx` has 23 → 9 (25 Hz).

**Reading:** `parse_onnx_policy` accepts exactly this shape and otherwise reports a reason, e.g.
`not an ONNX file (protobuf)`, `not an rsl_rl actor (Sub, Div, Gemm...)`, `ONNX tensor is not
float32`, `expected Elu after a hidden Gemm`, `unsupported ONNX operator`, `the output layer is not
linear`. The metadata is read along with it (`WalkerPolicy::metadata`, `meta(key)`); the runtime
parameters of a motion (kind, duration, command encoding, `action_scale` …) come from the policy
manifest `third_party/microduck/manifest.json`, which is built in (`micro_duck_manifest()`).

**Metadata on export** (`micro_duck_metadata(task)`):

| Key | Value |
|---|---|
| `action_scale` | `1.0` |
| `command_names` | `twist` |
| `observation_names` | `base_ang_vel,projected_gravity,joint_pos,joint_vel,actions,command,head_command,body_command` |
| `joint_names` | `left_hip_yaw,left_hip_roll,left_hip_pitch,left_knee,left_ankle,neck_pitch,head_pitch,head_yaw,head_roll,right_hip_yaw,right_hip_roll,right_hip_pitch,right_knee,right_ankle` |
| `default_joint_pos` | `0.000,-0.087,-0.458,-0.005,0.453,0.349,0.349,0.000,0.000,0.000,0.087,0.458,0.005,-0.453` |
| `joint_stiffness` | fourteen times `1.000` |
| `joint_damping` | fourteen times `0.000` |
| `task` | the task, e.g. `jump` |
| `lehrer` | `Kern (C++ PPO on Box3D)` |

For the play network (`play_metadata()`): `task` = `play`, `lehrer` = `Kern (C++ DAgger + PPO on
Box3D)`, `rate_hz` = `25`, `observation_names` (23 names from `ball_known` to `walking_s`),
`action_names` = `forward,strafe,turn,head_yaw,head_pitch_down,beak_open,kick_left,ground_pick,neck_bow`.

**The pick-up detector** (`pickup_detector.onnx`, robotd) has a different shape: input a window of
50 × 63 features, normalise and mask, a Gather of the columns it reads, three Conv1d with ELU, then
Gemm, ELU, Gemm, Sigmoid; output p(held). It is read by `parse_onnx_pickup` (`PickupDetector`); it
is built in as bytes (`third_party/microduck/pickup_detector.inc`, `pickup_detector()`),
[Chapter 13](13-micro-duck.md).

**Where the files live:** Pollen's set in `kern/third_party/microduck/` (`velstand.onnx` and
`pickup_detector.onnx` are also built in), Kern's own networks in `kern/policies/` (`jump.onnx`, `play.onnx`). On the published site
they are in `editor/microduck/` and `editor/policies/`. A network trained in the browser is stored by
the Lehrer page in IndexedDB ("kern-lehrer"); on the same device it replaces the bundled one
([Chapter 14](14-lehrer.md), [Chapter 15](15-your-own-models.md)).

### Servo profile (JSON)

A servo profile describes a servo type in the device model `game::servo_step`
(`include/kern/game/servo.h`, [Chapter 16](16-calibration.md)). The XL330 profile as shown in
`docs/kalibrierung.md`:

```json
{
  "kind": "servo",
  "name": "xl330",
  "stiffness": 0.554, "damping": 0.053, "friction": 0.0048, "torque": 0.963,
  "armature": 0.0018, "delay": 0.02,
  "spreadArmature": 0.1, "spreadFriction": 0.1, "delayMin": 0.015, "delayMax": 0.03
}
```

The writer (`game::to_json(const ServoProfile&)`, e.g. for `kern_lehrer --beispiele DIR` →
`xl330.json` or as the result of `--kalibrieren`) always outputs **all** fields with two-space
indentation, in the order `kind`, `name`, `source` (only if set), `stiffness`, `damping`,
`friction`, `torque`, `armature`, `delay`, `spreadStiffness`, `spreadDamping`, `spreadFriction`,
`spreadTorque`, `spreadArmature`, `delayMin`, `delayMax`. Internally the values are `f32` and are
formatted as `f64`; written files may therefore contain longer decimal representations than above.

| Field | Unit | Valid | Meaning |
|---|---|---|---|
| `kind` | – | `"servo"` (optional) | kind of profile; any other value is rejected |
| `name`, `source` | – | text | name, origin of the values |
| `stiffness` | N·m/rad | 0 … 1e6, **> 0** | K |
| `damping` | N·m·s/rad | 0 … 1e6 | D (back-EMF + viscous) |
| `friction` | N·m | 0 … 1e6 | F, Coulomb friction in the joint |
| `torque` | N·m | 0 … 1e6, **> 0** | T, maximum torque |
| `armature` | kg·m² | 0 … 1e3 | A, rotor inertia through the gearbox |
| `delay` | s | 0 … 1 | dead time command → servo |
| `spreadStiffness`, `spreadDamping`, `spreadFriction`, `spreadTorque`, `spreadArmature` | relative (±) | 0 … 1 | spread from which the training draws per servo |
| `delayMin`, `delayMax` | s | 0 … 1 | range of the bus dead time in training; `delayMax < delayMin` is set to `delayMin` |

Missing fields keep the default values of `ServoProfile` (0, name `xl330`); since `stiffness` and
`torque` must be positive, a valid file needs at least these two. Messages: `not a servo profile`,
`bad value for <field>`, `stiffness and torque must be positive`.

The servo model: torque = clamp(K × (target − angle), −T, T) − D × velocity, plus Coulomb friction F,
rotor inertia A on the joint axis and the command delayed by `delay` seconds.

### Test-bench and robot logs (CSV)

| Format | Structure | Source |
|---|---|---|
| Test-bench log | comment lines `# mass`, `# arm`, `# inertia`, `# horizontal` (load), header line, then `time,target,angle` (s, rad, rad) | `parse_servo_log` |
| Robot log | header line, then per control tick (50 Hz) 61 observations and 14 actions; at the start the robot stands still in the default pose | `parse_robot_log` |

Details in [Chapter 16](16-calibration.md) and `docs/kalibrierung.md`.

---

## 17.4 Glossary

| Term | Meaning |
|---|---|
| **Actor** | object of a level with a transform (relative to its parent) and optional components ([Chapter 8](08-game-runtime.md)) |
| **Armature** | rotor inertia of a servo, transferred through the gearbox onto the joint axis (kg·m²) |
| **Box3D** | 3D physics engine by Erin Catto, compiled in unmodified as C17 ([Chapter 6](06-physics.md)) |
| **C ABI** | the flat `extern "C"` functions of the module, through which JavaScript talks to the engine |
| **CMA-ES** | derivative-free optimisation method; used in the Lehrer for calibration and demonstration search |
| **Collector** | runs environments with a given policy and delivers rollouts (German: *Sammler*) |
| **Content Drawer** | Studio panel for placing actors |
| **Critic** | value function in PPO; in the Lehrer it sees the clean state (asymmetric) |
| **DAgger** | imitation learning: the network drives, the demonstration states the correct action on each tick |
| **Determinism fingerprint** | hash per control tick over the state of the walking duck; reference in `kern/tests/determinism_fingerprint.txt` |
| **`dm`** | `kern::dm`, deterministic transcendental functions from musl |
| **Domain randomisation / variation** | training with random deviations in build, servos, sensors and start |
| **DuckEnv** | Lehrer environment: one Micro Duck in a `GameWorld` of its own |
| **Error channel** | `fail()` / `last_error()` instead of exceptions (German: *Fehlerkanal*) |
| **GameWorld** | displays a level with the renderer and plays it on Box3D |
| **GTAO** | Ground-Truth Ambient Occlusion (after XeGTAO) in the renderer |
| **HostTransport** | transport whose datagrams are moved by the browser page (BroadcastChannel, WebSocket relay) |
| **JIT (logic)** | compilation of a logic graph into a WebAssembly module at runtime |
| **`.klevel`** | file format of a level (JSON, `"format": "kern-level"`) |
| **Lehrer** | Kern's training system for Micro Duck networks; German for "teacher" ([Chapter 14](14-lehrer.md)) |
| **Learner** | the part of PPO that learns from rollouts (German: *Lerner*) |
| **Logic graph** | blueprint-style program of nodes and connections; runs in the interpreter, as JIT or as exported C++ |
| **LoopbackNetwork** | in-process network for tests and multiplayer in the editor |
| **Micro Duck** | bipedal robot by Pollen Robotics; simulated in Kern with its real networks |
| **NetGame** | multiplayer over Kern Net: authoritative server, snapshots, interpolation |
| **NightWindows** | shader extension for lit windows at night |
| **ONNX** | exchange format for the networks; Kern reads and writes the rsl_rl actor shape |
| **Outliner** | tree view of the actors in the Studio |
| **Package Game** | export of a level as a self-contained HTML file |
| **Play / Simulate** | the run modes in the viewport (`game::SimMode::play` and `simulate`): Play plays the level, Simulate runs physics and rules while the editor remains usable (grab bodies, adopt simulated poses with K) |
| **Player-only** | starting the Studio module without the editor, for exported games (`kern_studio_init(…, 1)`) |
| **Policy** | trained network that maps observations to actions |
| **PPO** | Proximal Policy Optimization, the Lehrer's learning algorithm (after rsl_rl) |
| **Reactor (WASI)** | WebAssembly module without `main`: `_initialize`, then exports |
| **Recipe** | training settings per task (`recipe.h`), the same natively and in the browser (German: *Rezept*) |
| **Rollout** | collected transitions (observation, action, reward …) of several environments |
| **rsl_rl** | reinforcement learning library with which Pollen trains the networks; the model for Kern's PPO and export format |
| **Servo profile** | parameters of a servo type (K, D, F, T, A, dead time, spreads) as JSON (German: *Servoprofil*) |
| **ShaderExtension** | GLSL snippet at fixed hook points of the standard shader, with which games add their own looks ([Chapter 4](04-graphics.md)) |
| **Span** | non-owning view of contiguous elements (`kern::Span`) |
| **Substeps** | sub-steps of the Box3D solver per world step; 16 for the Micro Duck |
| **Tier (logic)** | execution tier of a graph: `interpreter`, `compiling`, `compiled (WebAssembly)`, `native C++` |
| **velstand** | Pollen's walking and standing network for the Micro Duck; built into Kern |
| **Viewmodel layer** | render layer after a depth clear, e.g. for gizmos |
| **Vorbild** | demonstration that the network imitates first (jump, play); German for "role model" |
| **Walker** | controller type for articulated robots driven by a policy |
| **wasi-sdk** | LLVM toolchain for WebAssembly with wasi-libc and libc++; Kern uses release 25 |
| **Welding** | lossless indexing of a triangle soup |

---

© 2026 Christian Heinrich Hohlfeld · All rights reserved · [Imprint and rights](00-imprint.md)
