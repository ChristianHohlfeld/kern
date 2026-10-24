# Chapter 1 – Introduction

Kern is a 3D engine written in **C++20**. It is compiled to **WebAssembly** with **wasi-sdk** and
draws in the browser through **WebGL2**. The same WebAssembly module also runs the physics
(**Box3D**), a custom **sound engine**, a **game runtime** made of levels, actors and components,
**logic graphs** with a JIT, a **networking stack** and the editor **Kern Studio**. On top of that
there is **Plumo**, a two-legged robot duck built on Pollen Robotics' open-source robot, which runs in Kern with its real
trained networks, and the **Lehrer** (German for "teacher"), which trains new networks for this robot
in C++ – natively and in the browser.

Live: <https://christianhohlfeld.github.io/kern/> – the start page with Kern Studio (`/editor/`), the
Lehrer (`/lehrer/`), playable levels (`kern/web/studio/play.html?level=orbRun` or `physicsLab`) and
the C++ examples `orbrun` and `hello`.

![Kern Studio with the Orb Run level](../buch/bilder/studio.png)

This chapter explains what Kern is, the principles it is built on, how the layers fit together and
how the repository is organized. It ends with a reading guide for the book.

---

## 1.1 What Kern Is

Kern grew out of [SpawnCity](https://spawncity.de), a game that renders through Kern. The repository
contains the engine together with its commit history from SpawnCity, restricted to the engine files.
The engine itself knows nothing about SpawnCity; games are written against the engine API only.

Kern consists of these parts:

| Part | Namespace / location | Summary |
|---|---|---|
| Foundations | `kern` (`core/`), `kern::json`, `kern::dm` | types, error channel, logging, `Span`, hashes, JSON, deterministic math |
| Math | `kern` (`math/math.h`) | `Vec2/3/4`, `Quat`, `Mat4` (column-major), `Sphere`, `Aabb`, `Camera`; meters, right-handed, Z up |
| Renderer | `kern` (`gfx/`) | WebGL2 renderer: one standard shader, instancing, skinning, PBR, sun shadows, sky, bloom, GTAO, particles, shader extensions |
| Scene | `kern` (`scene/`) | frustum culling, BVH, picking |
| Geometry | `kern` (`geo/`) | Earcut, primitives, welding, buildings, roads, terrain |
| App runtime | `kern` (`app/`) | `App`/`Engine`/`Input` for games that run entirely in C++ |
| Physics | `kern::physics` | C++ layer over Box3D: world, bodies, shapes, joints, raycasts, `MouseGrab` |
| Audio | `kern::audio` | software mixer, 3D sound, buses, reverb, limiter, synthesizer, the duck's voice |
| Game runtime | `kern::game` | `Level` (`.klevel`), `GameWorld`, controllers, rules, sound, touch, Walker (Plumo), multiplayer |
| Logic | `kern::logic` | Blueprint-style graphs; interpreter, JIT to WebAssembly, export as C++ |
| Networking | `kern::net` | serialization, reliability, channels, client/server, transports |
| Studio | `kern::studio` | the Unreal-style editor, written entirely in C++ and testable without a browser |
| Lehrer | `kern::lehrer` | environments, MLP, PPO, DAgger, CMA-ES, ONNX export, calibration |
| C ABI | `src/api`, `src/studio/api.cpp`, … | flat `extern "C"` functions for the JavaScript host |
| Platform layer | `kern/web/*.mjs` | loading the module, WebGL2 imports, input, DOM – no engine logic |

---

## 1.2 Goals and Principles

### No three.js, No Emscripten

Kern uses no third-party render engine and no Emscripten runtime. The renderer is Kern's own C++
code, which calls the WebGL2 functions directly as WebAssembly imports
(`include/kern/gfx/gl.h`, import module `kern_gl`). It is compiled with the plain LLVM toolchain of
**wasi-sdk** (clang, wasm-ld, wasi-libc, libc++). The module is a **WASI "reactor"**: the host calls
`_initialize` once and then the exported functions (`kern/build.sh`, linker option
`-mexec-model=reactor`).

The feature set is deliberately conservative: `-mcpu=mvp` plus `sign-ext`, `bulk-memory`,
`nontrapping-fptoint` and `mutable-globals`. With that, the module runs in every WebGL2-capable
browser from Safari 15 onward.

### No Engine Logic in JavaScript

In the browser, WebGL2, the canvas, input events, `requestAnimationFrame`, Web Audio, file dialogs
and the clipboard are reachable only from JavaScript. Exactly that – and only that – is what the
platform layer `kern/web/` takes care of. Its own header comment sums it up (`kern/web/kern.mjs`):

```js
// The engine is C++ compiled to WebAssembly (kern/build.sh). The browser only
// offers WebGL2, canvas events and timing to JavaScript, so this file does
// exactly three things and contains no engine logic:
//   1. instantiate the module with the `kern_gl` (WebGL2) and `kern_env`
//      imports plus a minimal WASI shim,
//   2. own the GPU object handle tables and context-loss plumbing,
//   3. expose `KernRenderer`, a thin typed-array marshalling facade over the
//      engine's C ABI (kern/include/kern/api/c_api.h).
```

The same holds for the editor: `kern/web/studio/studio.mjs` draws the panels from the JSON that the
C++ editor delivers, forwards input and handles browser tasks (menus and dialogs as DOM, file
selection, downloads, clipboard, pointer lock, image decoding, local storage, writing exported
games). The Blueprint editor `logic.mjs` does not hold a graph either: every change is a Studio
command sent to the engine.

The result: whatever a game does lives in C++, can be tested natively with sanitizers
([Chapter 2](02-building-and-testing.md)) and behaves in the exported game exactly as it does in the
editor.

### Determinism

A simulation, a training run or a replayed robot log produces the same result everywhere in Kern:
natively on x86 and ARM (GCC, Clang, Apple clang) and as WebAssembly in every browser – bit for bit.
This is ensured by the following (`docs/lehrer.md`, section 7):

- **No FMA contraction:** Kern, Box3D and the libm are compiled with `-ffp-contract=off`.
- **Box3D on the scalar path** (`BOX3D_DISABLE_SIMD`), as in WebAssembly.
- **Kern's own libm:** `sin`, `cos`, `atan2`, `exp`, `log`, `pow` … come from musl
  (`third_party/musl_math`, renamed to `kern_musl_*`) and are called through `kern::dm::`.
- **Learning independent of the number of workers:** per-environment seeds derived from
  (seed, global index), a canonical order of rollouts, sums in a fixed order.

This is checked with a **fingerprint** (`kern_lehrer --fingerprint 400`), whose reference lives in
`kern/tests/determinism_fingerprint.txt` and which CI compares on three platforms. The details are
in [Chapter 3](03-foundations.md) and [Chapter 2](02-building-and-testing.md).

Determinism is not an end in itself: it turns a simulation bug into a fixed, reproducible bug rather
than noise that looks different on every machine. A difference between a real robot log and the
simulation is a genuine modeling error ([Chapter 16](16-calibration.md)).

### Further Principles

- **No exceptions, no RTTI.** Everything is built with `-fno-exceptions -fno-rtti`. Fallible
  operations return `bool` (or 0 / `nullptr`) and store a readable message with `fail()`; the host
  reads it with `kern_last_error()` ([Chapter 3](03-foundations.md)).
- **Input from the host is validated.** An invalid `upsert` leaves the existing mesh untouched,
  because everything is validated before any change is made; number streams of the geometry jobs are
  checked for finiteness and length; the JSON parser rejects excessive nesting and the keys
  `__proto__`, `prototype` and `constructor`.
- **No per-frame allocations on the JS side.** The `KernRenderer` facade copies typed arrays into a
  reused staging area in WebAssembly memory (`kern/README.md`).
- **Do not bypass the physics.** Box3D is compiled in unmodified. Plumo stands solely by
  means of its servos and contacts – no helper forces, no prescribed velocities; servos are built
  only from Box3D's own joint parts (`docs/walker.md`, section 1;
  [Chapter 6](06-physics.md), [Chapter 13](13-plumo.md)).
- **One implementation for every place.** The editor, the exported game, the Lehrer and the native
  tools all use the same `GameWorld`, the same servos and the same physics. A network that learns in
  the Lehrer therefore runs unchanged in the Studio (`docs/lehrer.md`).
- **Robust against context loss.** When the WebGL context is lost, the renderer pauses; on
  restoration it rebuilds all programs and re-uploads meshes and engine textures from their CPU
  copies ([Chapter 4](04-graphics.md)).

---

## 1.3 Architecture: The Layers

The following diagram shows the layers of a Kern module in the browser, from the platform layer at
the top down to the foundations at the bottom. A downward arrow means "uses".

```
 Browser (JavaScript, kern/web/)  – platform only, no engine logic
 ┌─────────────────────────────────────────────────────────────────────────────┐
 │ kern.mjs        module loading, imports kern_gl / kern_env / WASI shim,      │
 │                 GPU handle tables, context loss, KernRenderer facade         │
 │ app.mjs         host for C++ games (kern::App)                               │
 │ studio/*.mjs    editor page, game host + KernNet (game.mjs), logic editor    │
 │ lehrer/*.mjs    Lehrer page, Web Workers (collectors, learner)               │
 └───────────────▲───────────────────────────────────────────┬─────────────────┘
                 │ imports: kern_gl.*, kern_env.*,           │ exports (C ABI):
                 │ wasi_snapshot_preview1.*                  │ kern_*, kern_app_*, kern_studio_*,
                 │                                           ▼ kern_lehrer_*, kern_logic_*
 WebAssembly module (C++20, wasi-sdk, reactor)
 ┌─────────────────────────────────────────────────────────────────────────────┐
 │ C ABI      src/api/ (renderer, particles, geometry), src/app/app.cpp,        │
 │            src/studio/api.cpp, src/lehrer/lehrer_api.cpp, src/logic/logic_api│
 ├──────────────────────────────┬──────────────────────────────────────────────┤
 │ kern::studio  (--studio)     │ kern::lehrer  (--lehrer)                      │
 │ editor, viewports, gizmos,   │ DuckEnv, MLP, PPO, DAgger, CMA-ES,            │
 │ details, catalog, Play       │ ONNX export, calibration                      │
 ├──────────────────────────────┴──────────────────────────────────────────────┤
 │ kern::game  (--game)                                                         │
 │ Level/.klevel, GameWorld, controllers, rules, city, sound, touch,            │
 │ Walker (Plumo), servo profiles, logic nodes, NetGame                    │
 ├───────────────┬───────────────┬────────────────┬────────────────────────────┤
 │ kern::physics │ kern::logic   │ kern::net      │ kern::audio                 │
 │ (--physics)   │ graphs, JIT,  │ bit packer,    │ mixer, 3D, buses,           │
 │ → Box3D (C17) │ C++ export    │ acks, channels,│ synth                       │
 │               │               │ client/server  │                             │
 ├───────────────┴───────────────┴────────────────┴────────────────────────────┤
 │ kern (gfx, scene, geo, app)                                                  │
 │ renderer, passes (shadows, sky, bloom, GTAO), particles, BVH, picking,       │
 │ Earcut, primitives, buildings, roads, terrain, App/Engine/Input (--app)      │
 ├─────────────────────────────────────────────────────────────────────────────┤
 │ kern::core / kern::math / kern::json / kern::dm                              │
 │ base.h, json.h, math.h, dmath.h → third_party/musl_math                      │
 └─────────────────────────────────────────────────────────────────────────────┘
```

### What Is Always in the Module and What Is Optional

`kern/build.sh` compiles all sources under `kern/src` **except** the five component folders `app`,
`physics`, `game`, `studio` and `lehrer`. These are added only with the matching switch
([Chapter 2](02-building-and-testing.md)):

| Switch | Adds | Also pulls in |
|---|---|---|
| (always) | `src/api`, `src/audio`, `src/core`, `src/geo`, `src/gfx`, `src/logic`, `src/net`, `src/scene`, musl libm | – |
| `--app` | `src/app` | – |
| `--physics` | Box3D (`third_party/box3d`, C17), `src/physics` | – |
| `--game` | `src/game` | `--physics` |
| `--studio` | `src/studio` | `--game`, `--physics` |
| `--lehrer` | `src/lehrer` | `--game`, `--physics` |

### The Boundary Between C++ and JavaScript

Data crosses the boundary exclusively through the module's linear memory:

- **From the host into the engine:** the host reserves memory with `kern_alloc`, writes typed arrays
  and descriptors at fixed word offsets (`KERN_MD_*`, `KERN_ID_*`, `KERN_RP_*` in
  `include/kern/api/c_api.h`) and passes pointers. These offsets must match the constants in
  `kern/web/kern.mjs`.
- **Strings** go in as a (pointer, length) pair.
- **JSON results** from the Studio come back as a pointer to a NUL-terminated buffer that stays valid
  until the next call (`kern_studio_result_length()` returns the length).
- **From the engine to the host,** the C++ code calls import functions: the WebGL2 entry points
  (`kern_gl`), time, logging, height queries, status, pointer lock and the logic-JIT handoff
  (`kern_env`), as well as a few WASI functions served by a minimal shim in `kern.mjs`
  (`fd_write` ends up in the console, `random_get`, `clock_time_get`; unknown calls return
  `ENOSYS`).

The complete list of exported functions is in [Chapter 17](17-reference.md).

---

## 1.4 How the Parts Work Together

Kern ships in four forms. All four consist of the same C++ code, assembled differently.

### A Game Entirely in C++ (`--app`)

A game derives from `kern::App` and defines `kern_create_app()`. The platform layer
`kern/web/app.mjs` (`runKernApp`) drives it: frame loop, drawing-buffer size, input, context loss and
a status channel. The smallest example is `kern/examples/hello/hello.cpp`:

```cpp
class Hello final : public App {
 public:
  RendererOptions configure() override {
    RendererOptions o;
    o.shadows = true;
    o.shadow.resolution = 1024;
    o.shadow.extent = 40;
    return o;
  }

  bool start(Engine& engine) override {
    Renderer& r = engine.renderer;
    ground_ = r.allocate_id();
    MeshInput g = ground_mesh_.as_input();
    g.material.color = {0.42f, 0.47f, 0.40f, 1};
    if (!r.upsert(ground_, g)) return false;
    // ...
  }
  // update(): camera, instances, picking, engine.set_status(...)
};

App* kern_create_app() {
  static Hello app;
  return &app;
}
```

For each display frame the host calls `kern_app_frame(now_ms, page_hidden)`; this calls `update()`
and then renders ([Chapter 12](12-your-own-games.md)). The larger example
`kern/examples/orbrun/orbrun.cpp` shows a complete game with a procedural city, vehicle physics,
traffic, day/night and a custom shader extension.

### The Renderer from JavaScript (Like SpawnCity)

A JavaScript program can use the renderer on its own: `loadKern()` loads the module,
`new KernRenderer(canvas, options, kern)` creates the renderer, and `upsert`, `updateInstances`,
`setCamera`, `render` and `pick` call the C ABI. This is how SpawnCity uses Kern
([Chapter 4](04-graphics.md)).

### Kern Studio and Exported Games (`--studio`)

`tools/build_studio.sh` builds the module `generated/kern_studio.wasm` (engine, Box3D, game runtime,
Studio) and embeds it together with `kern.mjs`, `game.mjs`, `logic.mjs`, `studio.mjs` and
`studio.css` into **one** HTML file: `generated/studio.html`. It runs offline, even when opened by
double-click.

The Studio edits a `Level` (actors with components), displays it with a `GameWorld` and plays it
with **Play** or **Simulate** directly in the viewport. **File → Package Game** writes a game as a
standalone HTML file: engine module (base64), `kern.mjs`, `game.mjs` and the level. The exported game
starts the same module in player mode (`kern_studio_init(w, h, 1)`) and plays the level with the same
C++ classes as the editor ([Chapter 8](08-game-runtime.md), [Chapter 11](11-studio.md)).

![An exported game](../buch/bilder/game.png)

### The Lehrer (`--lehrer` and Native)

The Lehrer trains networks for Plumo. It runs

- **natively** as the command-line program `kern_lehrer` on all cores (`kern/tools/lehrer.sh`), also
  headless on a server, and
- **in the browser** on the `/lehrer/` page: `kern/build.sh --lehrer` builds `kern_lehrer.wasm`; each
  Web Worker loads its own module and acts as a collector or the learner
  (`src/lehrer/lehrer_api.cpp`).

Both use the same recipes (`include/kern/lehrer/recipe.h`) and the same `GameWorld`. The same seed and
the same number of ducks yield the same network – natively as in the browser. The export is ONNX in
the format of Pollen's networks; the Studio loads it just like those ([Chapter 14](14-lehrer.md)).

![The Lehrer page in the browser](../buch/bilder/lehrer.png)

### Plumo: Everything Working Together

Plumo shows how the layers interlock: its model comes from Pollen's MJCF
(`third_party/pollen`), its limbs are Box3D bodies with joints (`kern::physics`), its servos a
device model (`kern::game::ServoProfile`), its networks ONNX files that Kern evaluates itself on the
CPU (`kern::game::WalkerPolicy`), its voice comes from the mixer (`kern::audio`), logic nodes of the
*Robots* category control it (`kern::logic`), and in the Studio it sits as an actor in a level.

![Plumo level in the Studio](../buch/bilder/duck-level.png)

---

## 1.5 Directory Layout

The repository keeps the engine in the subfolder `kern/`; next to it are tools, the start page and
generated files.

```
/                                repository root
  README.md                      overview, quick start, CI and Pages
  LICENSE                        Kern's licence: Apache License 2.0 (with LICENSING.md, NOTICE, TRADEMARKS.md)
  .github/workflows/ci.yml       CI: native tests, determinism (macOS), build, e2e, deploy
  tools/
    build_studio.sh              Kern Studio as a single HTML file -> generated/studio.html
    net_relay.mjs                WebSocket relay for multiplayer across machines
    plumo_import.py         import of Pollen's robot model and walking policy
    mesh_simplify.py, qem_simplify.cpp   mesh simplification for the import
  site/
    index.html                   GitHub Pages start page (footer: owner, license links)
    editor/                      editor app files (manifest, service worker, icons)
  generated/                     build outputs (studio.html, *.wasm, ...)

  kern/                          the engine
    README.md                    engine overview
    LICENSES.md                  third-party components and their licenses (Box3D, Earcut, musl, Plumo ...)
    build.sh                     WebAssembly build (--app, --physics, --game, --studio, --lehrer)
    include/kern/                public headers (kern/kern.h includes app, foundations, geometry,
                                 renderer, math and scene)
      core/                      base.h, json.h, dmath.h
      math/                      math.h
      gfx/                       gl.h, program.h, shader.h, mesh.h, passes.h, renderer.h, particles.h
      scene/                     bvh.h, frustum.h, picking.h
      geo/                       earcut.h, primitives.h, mesh_data.h, buildings.h, roads.h
      app/                       app.h
      api/                       c_api.h
      physics/                   physics.h
      audio/                     audio.h, synth.h, duck_voice.h
      game/                      level.h, runtime.h, geometry.h, demos.h, sound.h, touch.h,
                                 walker.h, walker_policy.h, servo.h, net_game.h, builtin_meshes.h
      logic/                     logic.h
      net/                       serialize.h, reliable.h, connection.h, client_server.h, transport.h
      studio/                    studio.h
      lehrer/                    env.h, nn.h, ppo.h, backend.h, recipe.h, vorbild.h, spielen.h,
                                 cmaes.h, kalibrierung.h, onnx_export.h
    src/                         implementation, same structure as include/
                                 (src/api: C ABI of the renderer, particles and geometry;
                                  src/studio/api.cpp: kern_studio_*; src/lehrer/lehrer_api.cpp:
                                  kern_lehrer_*; src/logic/logic_api.cpp: kern_logic_*)
    web/
      kern.mjs                   browser platform layer + KernRenderer
      app.mjs                    host for C++ games
      studio/                    index.html, studio.css, studio.mjs, logic.mjs, game.mjs, play.html
      lehrer/                    index.html, lehrer.mjs, worker.mjs
    tools/
      kern_lehrer.cpp            the Lehrer as a command-line program
      lehrer.sh                  builds kern_lehrer natively
    tests/
      run.sh                     native unit tests (ASan/UBSan)
      test_main.cpp              the tests
      gl_stub.cpp/.h             recording WebGL stub
      logic_cases.h, logic_*     logic tests across all three execution tiers
      lehrer_wasm_test.mjs       the Lehrer in the WebAssembly module
      determinism_fingerprint.txt  reference hashes for determinism
      e2e/studio_e2e.mjs         browser test (Playwright)
    examples/
      hello/                     the smallest game
      orbrun/                    a complete game in C++
    policies/                    Kern's own networks (jump.onnx, play.onnx)
    third_party/
      box3d/                     Box3D (Erin Catto, MIT), unmodified, as C17
      musl_math/                 deterministic libm from musl (MIT)
      pollen/                    Pollen Robotics: robot model (MJCF) and policy set
    docs/                        standalone documents (studio, physics, walker, lehrer, kalibrierung, net)
      buch/                      this book (German original)
      book/                      this book (English translation)
```

The standalone documents under `kern/docs/` are the sources this book draws on. For changes to the
physics or Plumo, `docs/physics.md` and `docs/walker.md` are authoritative.

`kern/README.md` mentions some SpawnCity files (`games/spawncity`, `sim/kern_*.mjs`,
`tools/build_kern_wasm.sh`, `tests/kern_*_test.mjs`). These live in the SpawnCity repository, not in
this one.

---

## 1.6 Reading Guide

The book is built from the bottom up: first tools and foundations, then the engine layers, then
game, editor and robot, and finally the reference.

| Chapter | Contents |
|---|---|
| [01 Introduction](01-introduction.md) | this chapter: goals, architecture, directories |
| [02 Building and Testing](02-building-and-testing.md) | wasi-sdk, `build.sh`, Studio build, native tests, logic and browser tests, determinism, CI, deployment |
| [03 Foundations](03-foundations.md) | `kern::core`, JSON, math and its conventions, deterministic math |
| [04 Graphics](04-graphics.md) | renderer, standard shader, passes, shader extensions, context loss, JS facade |
| [05 Scene and Geometry](05-scene-and-geometry.md) | culling, BVH, picking, Earcut, primitives, welding, buildings, roads, terrain |
| [06 Physics](06-physics.md) | the Box3D layer and the rules for using it |
| [07 Audio](07-audio.md) | mixer, 3D sound, synthesizer, sound in games |
| [08 Game Runtime](08-game-runtime.md) | levels, actors, components, `.klevel`, `GameWorld`, controllers, rules |
| [09 Logic](09-logic.md) | logic graphs, interpreter, JIT, export as C++ |
| [10 Networking](10-networking.md) | Kern Net and multiplayer |
| [11 Studio](11-studio.md) | Kern Studio: interface, controls, protocol, export |
| [12 Your Own Games](12-your-own-games.md) | building games in C++ and in the Studio |
| [13 Plumo](13-plumo.md) | the robot, its networks, servos and sensors |
| [14 Lehrer](14-lehrer.md) | training: PPO, demonstration and imitation, determinism, browser |
| [15 Your Own Models](15-your-own-models.md) | your own networks and models in Kern |
| [16 Calibration](16-calibration.md) | servo profiles, test bench, matching against the robot |
| [17 Reference](17-reference.md) | C ABI, `kern_lehrer` command line, file formats, glossary |

Three appendices provide the background knowledge the chapters assume:

| Appendix | Contents |
|---|---|
| [A Neural Networks and Reinforcement Learning](A-neural-networks.md) | background for Chapters 14 and 15 |
| [B Real-Time Rendering](B-rendering.md) | background for Chapters 4 and 5 |
| [C Physics Simulation](C-physics.md) | background for Chapter 6 |

**Suggested starting points:**

- **If you want to build and modify Kern,** read Chapters 2 and 3, then the chapter on the layer you
  are changing. Before changing the physics or the robot, also read Chapters 6 and 13.
- **If you want to build a game,** start with Chapter 11 (Studio) or Chapter 12 (C++), with
  Chapters 8 and 9 as background.
- **If you work with Plumo,** read Chapters 13, 14 and 16 in that order.
- **If you embed Kern from JavaScript,** read Chapter 4 and the C ABI in Chapter 17.

Code examples in this book are taken from the repository or match its API exactly. Paths are given
relative to the `kern/` folder unless they begin with `tools/`, `site/` or `generated/` of the
repository root or are explicitly marked otherwise.

---

© 2026 Christian Heinrich Hohlfeld · All rights reserved · [Imprint and rights](00-imprint.md)
