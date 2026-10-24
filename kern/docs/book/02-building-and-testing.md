# 2 Building and Testing

This chapter describes every way to compile and check Kern: the toolchain, the build script
`kern/build.sh` and its modes, the Studio build, the native Lehrer, the native unit tests with
sanitizers, the logic tests across three execution tiers, the browser test, the determinism
fingerprint, the CI jobs in detail and publishing to GitHub Pages.

All paths are relative to the repository root unless stated otherwise.

---

## 2.1 Overview

| What | Tool | Result |
|---|---|---|
| WebAssembly module (engine + components + game sources) | `kern/build.sh` | `<out>.wasm` |
| Kern Studio as a single file | `tools/build_studio.sh` | `generated/studio.html`, `generated/kern_studio.wasm` |
| Lehrer for the browser | `kern/build.sh --lehrer generated/kern_lehrer.wasm` | `generated/kern_lehrer.wasm` |
| Native Lehrer | `kern/tools/lehrer.sh [out-dir]` | `kern/build/kern_lehrer` |
| Native unit tests | `kern/tests/run.sh` | test run with ASan/UBSan, followed by the logic export test |
| Logic JIT test | `kern/build.sh … logic_wasm_test.cpp` + `node kern/tests/logic_wasm_test.mjs` | interpreter ↔ JIT comparison |
| Lehrer in the WebAssembly module | `node kern/tests/lehrer_wasm_test.mjs generated/kern_lehrer.wasm` | one PPO iteration through the C ABI, fingerprint |
| Browser test | `node kern/tests/e2e/studio_e2e.mjs <base-url>` | Studio, Play, Simulate, export, phone |

The shortest path to a running Studio and green tests (from `README.md`):

```sh
# wasi-sdk 25 (Linux x86_64)
curl -sSL https://github.com/WebAssembly/wasi-sdk/releases/download/wasi-sdk-25/wasi-sdk-25.0-x86_64-linux.tar.gz | tar xz -C /opt
export WASI_SDK=/opt/wasi-sdk-25.0-x86_64-linux

tools/build_studio.sh                 # Kern Studio as a single file -> generated/studio.html
CXX=g++ CC=gcc kern/tests/run.sh      # native unit tests (ASan/UBSan)

# C++ example games
kern/build.sh --app kern/examples/hello/hello.wasm kern/examples/hello/hello.cpp
kern/build.sh --app kern/examples/orbrun/orbrun.wasm kern/examples/orbrun/orbrun.cpp

# Browser test (Studio, Fly Mode, panels, Play, Simulate, exported game)
python3 -m http.server 8771 &
node kern/tests/e2e/studio_e2e.mjs http://127.0.0.1:8771
```

Afterwards, `generated/studio.html` runs offline by double-click. For development, serve the
repository root statically and open `kern/web/studio/index.html`; it loads
`generated/kern_studio.wasm`. `kern/web/studio/play.html?level=orbRun` plays a built-in level without
the editor (further values according to its header comment: `physicsLab`, `shootingRange`,
`microDuck`, `basic`).

---

## 2.2 The Toolchain: wasi-sdk 25

Kern requires [wasi-sdk](https://github.com/WebAssembly/wasi-sdk), **release 25**: clang, wasm-ld,
wasi-libc and libc++ for the target `wasm32-wasip1`. Emscripten is not used.

- The environment variable `WASI_SDK` points to the installation directory. Without it,
  `kern/build.sh` assumes `/opt/wasi-sdk`.
- CI downloads `wasi-sdk-25.0-x86_64-linux.tar.gz` from GitHub and extracts it with
  `--strip-components=1` into `/home/runner/wasi-sdk` (see [2.10](#210-ci-in-detail)).
- For native builds (tests, Lehrer) a normal host compiler is enough (GCC or Clang; Apple clang on
  macOS).
- `tools/build_studio.sh` additionally needs `python3`; the Node tests need Node (CI uses Node 22);
  the browser test needs Playwright with Chromium.

---

## 2.3 `kern/build.sh` – The WebAssembly Module

```
kern/build.sh [--app] [--physics] [--game] [--studio] [--lehrer] <out.wasm> [compiler flags / game sources...]
```

### Components

Without switches, the script compiles every `*.cpp` under `kern/src` except the folders `app`,
`physics`, `game`, `studio` and `lehrer` – that is, `api`, `audio`, `core`, `geo`, `gfx`, `logic`,
`net` and `scene` – plus, always, the deterministic libm from `kern/third_party/musl_math`. The
switches add:

| Switch | Adds | Includes |
|---|---|---|
| `--app` | the app runtime `kern/src/app` for games written against `kern::App`; the host is `kern/web/app.mjs` | – |
| `--physics` | Box3D (`kern/third_party/box3d`, as C17) and `kern/src/physics` | – |
| `--game` | levels, actors, components, game runtime, procedural city (`kern/src/game`) | `--physics` |
| `--studio` | Kern Studio (`kern/src/studio`) and its C ABI | `--game` |
| `--lehrer` | the headless Lehrer (`kern/src/lehrer`) and its C ABI for Web Workers | `--game` |

### Further Arguments

Any number of arguments may follow the output path. If one ends in `.cpp` or `.cc`, it is compiled
as a game source just like the engine; everything else is passed as a compiler option to every C++
compilation. Example from CI (test source plus include path):

```sh
kern/build.sh /tmp/logic_test.wasm kern/tests/logic_wasm_test.cpp -Ikern/tests
```

### Environment Variables

| Variable | Default | Effect |
|---|---|---|
| `WASI_SDK` | `/opt/wasi-sdk` | installation directory of wasi-sdk |
| `KERN_BUILD` | `release` | `debug` compiles with `-O1 -g` without LTO; otherwise `-O3 -flto=thin -DNDEBUG` |
| `KERN_JOBS` | all cores (`nproc`) | number of parallel compiler and ThinLTO jobs |
| `KERN_OBJ_CACHE` | `$TMPDIR/kern-obj` (or `/tmp/kern-obj`) | object cache |

A note on `KERN_BUILD=debug`: the link step passes `-Wl,--strip-debug` in every mode, so the finished
module contains no debug information even in debug mode. The mode affects optimization and LTO.

### Compiler Options

The options are the foundation of robustness and determinism:

```sh
# Conservative feature set: runs on every WebGL2-capable browser (Safari 15+).
features=(-mcpu=mvp -msign-ext -mbulk-memory -mnontrapping-fptoint -mmutable-globals)
target=(--target=wasm32-wasip1 --sysroot="$WASI_SDK/share/wasi-sysroot")
```

```sh
cxxflags=("${target[@]}" -std=c++20 "${opt[@]}" "${features[@]}"
          -fno-exceptions -fno-rtti -fvisibility=hidden -ffunction-sections -fdata-sections -ffp-contract=off
          -Wall -Wextra -Wno-unused-parameter -Werror=return-type
          "${includes[@]}" "${extra_flags[@]}")
```

- **`-ffp-contract=off`** everywhere (Kern, Box3D, musl): no FMA contraction; otherwise `a*b+c`
  rounds differently depending on the target ([Chapter 3](03-foundations.md)).
- **Box3D** is compiled with `-std=c17 -DBOX3D_DISABLE_SIMD -ffp-contract=off -w` (scalar path, as
  in WebAssembly; warnings from the third-party library are off).
- **musl** with `-std=c17 -ffp-contract=off -fno-math-errno -w`.
- **C++** without exceptions and RTTI, with hidden visibility (only what `KERN_EXPORT` marks is
  exported), and one section per function and data item so that `--gc-sections` removes anything
  unused.

### Linking

```sh
"$CXX" "${target[@]}" "${opt[@]}" "${features[@]}" "${lto[@]}" \
  "${cpp_objects[@]}" "${objects[@]}" \
  -mexec-model=reactor \
  -Wl,--gc-sections -Wl,--strip-debug -Wl,-z,stack-size=1048576 -Wl,--export-table -Wl,--growable-table \
  -Wl,--initial-memory=33554432 -Wl,--max-memory=2147483648 \
  ...
```

| Option | Meaning |
|---|---|
| `-mexec-model=reactor` | WASI reactor: no `main`; the host calls `_initialize` and then the exports |
| `-z,stack-size=1048576` | 1 MiB stack |
| `--initial-memory=33554432`, `--max-memory=2147483648` | 32 MiB initial memory, at most 2 GiB |
| `--export-table`, `--growable-table` | The function table is exported and growable. The logic JIT places compiled graphs there (`table.grow`, `table.set` in `kern.mjs`, [Chapter 9](09-logic.md)) |
| `--gc-sections`, `--strip-debug` | remove unused code, remove debug information |

At the end the script reports `built <out> (<n> bytes)`.

### Parallel Compilation and the Object Cache

Each source is compiled into its own object, in parallel (`KERN_JOBS`), and ThinLTO links them. So
that a rebuild only compiles what has changed, and so that the Studio and the example games share the
engine's objects, there is a cache:

- **Box3D and musl:** one cache folder each, under a key derived from the options and the compiler
  version (`box3d-<key>`, `musl-math-<key>`); an object is recompiled if it is missing or older than
  its source or the newest header.
- **C++:** folder `cpp-<flags_key>`; each object is named `<name>-<key>.o`, where the key is built
  from the hash of **all** header contents (under `include`, `src` and Box3D's `include`), the source
  path and the source contents. Consequence: **every header change recompiles all C++ sources** –
  simple and safe, as the comment in the script puts it.
- **ThinLTO:** cache in `$KERN_OBJ_CACHE/thinlto`.

CI keeps the cache between runs (see [2.10](#210-ci-in-detail)).

---

## 2.4 `tools/build_studio.sh` – Kern Studio as a Single File

```
tools/build_studio.sh [--skip-wasm] [out.html]     (default: generated/studio.html)
```

1. `kern/build.sh --studio generated/kern_studio.wasm` – engine, Box3D, game runtime, Studio.
   With `--skip-wasm` this step is skipped; the module must then already exist, otherwise the script
   exits with `missing …/kern_studio.wasm (run without --skip-wasm)`.
2. A Python part assembles the page:
   - `kern/web/studio/studio.css` replaces the `<link>` as a `<style>`.
   - `kern.mjs` and `game.mjs` are additionally embedded as text
     (`<script type="text/plain" id="kern-src-kern">` and `kern-src-game` respectively). The Studio
     writes exported games from them.
   - The module is embedded base64-encoded (`<script type="text/plain" id="kern-wasm">`).
   - `kern.mjs`, `game.mjs`, `logic.mjs` and `studio.mjs` run as **one** inline module: the
     `import { … } from "…";` lines and the `export` keywords are removed and the sources are
     concatenated. This way the page needs no `blob:` imports and also works under strict Content
     Security Policies.
   - A boot block sets `window.KERN_SOURCES` and calls `startStudio({base64: …})`; if startup fails,
     the message appears in the `engineNotice` element.

If one of the sources contains the string `</script`, the build aborts with
`<name> source contains '</script'; cannot inline it` – it would terminate the inline script.

Output: `generated/studio.html: <size> bytes (wasm <size> bytes)`.

---

## 2.5 The Native Lehrer: `kern/tools/lehrer.sh`

```
kern/tools/lehrer.sh [out-dir]      -> <out-dir>/kern_lehrer (default: kern/build)
```

The script builds `kern_lehrer` for Linux and macOS, x86 and ARM, optimized (`-O3 -DNDEBUG
-ffp-contract=off`) and on all cores:

- Box3D with `-DBOX3D_DISABLE_SIMD`, musl with `-fno-math-errno`.
- All sources under `kern/src` **except** `studio/` and `app/`, plus `kern/tests/gl_stub.cpp` (the
  WebGL stub; the Lehrer does not render) and `kern/tools/kern_lehrer.cpp`.
- Linked with `-lm -lpthread`.
- `KERN_NATIVE=1` (default) tries `-march=native`, then `-mcpu=native`, and uses the first option the
  compiler accepts. These options only let the compiler choose instructions; they change no results,
  because `-ffp-contract=off` applies.
- `CC`/`CXX` select the compilers, `KERN_JOBS` the parallelism. The job pool is portable down to
  macOS's bash 3.2.

The full command line of `kern_lehrer` is in [Chapter 17](17-reference.md); the training itself is
described in [Chapter 14](14-lehrer.md).

---

## 2.6 Native Unit Tests: `kern/tests/run.sh`

```sh
CXX=g++ CC=gcc kern/tests/run.sh            # all tests
kern/tests/run.sh micro_duck                 # only tests whose name contains "micro_duck"
```

### What the Script Does

1. **Box3D** is compiled once **without** sanitizers (`-O2 -g -DBOX3D_DISABLE_SIMD
   -ffp-contract=off`) and cached – third-party code on the scalar path, as in WebAssembly.
2. **musl** is likewise compiled once (`-O2 -g -ffp-contract=off -fno-math-errno`).
3. **All of Kern's C++ code** (`kern/src/**/*.cpp`, `gl_stub.cpp`, `test_main.cpp`) is compiled with
   **AddressSanitizer and UndefinedBehaviorSanitizer**:

   ```sh
   flags=(-std=c++20 -O1 -g -ffp-contract=off -fno-exceptions -fno-rtti -fsanitize=address,undefined -fno-sanitize-recover=undefined
          -Wall -Wextra -Wno-unused-parameter -Werror -I"$root/include" -I"$root/third_party/box3d/include" -I"$here")
   ```

   Unlike `build.sh`, **`-Werror`** applies here: any warning aborts the test build.
   `-fno-sanitize-recover=undefined` turns undefined behavior into an abort.
4. The test program `kern_tests` runs in the `kern/tests` folder with the script's arguments.
5. **Only when run without arguments** does the logic export test follow (see
   [2.7](#27-logic-tests-three-tiers-one-result)).

Environment variables: `CXX`/`CC` (default `c++`/`cc`), `KERN_TEST_OUT` (output folder, default
`$TMPDIR/kern-tests`), `KERN_JOBS`.

### The Test Program

`kern/tests/test_main.cpp` contains its own small test harness without any third-party library:
`TEST(name)` registers a function, `CHECK(cond)` and `CHECK_NEAR(a, b, eps)` count checks and, on
failure, print the file, line, expression and `last_error()`. `main` runs every test whose name
contains the filter (first argument), prints `ok` or `FAIL` per test and, at the end,
`<n> checks, <m> failures`. The exit code is 1 if there is at least one failure.

The tests cover the whole engine, including (a selection; names as in the code):

| Area | Examples |
|---|---|
| Math, scene | `math_matrices`, `math_camera`, `bvh_matches_brute_force` |
| Geometry | `earcut_shapes`, `weld_and_hash`, `building_builder`, `road_and_terrain_builders`, `primitives_closed` |
| Renderer against the GL stub | `renderer_meshes`, `renderer_frame`, `renderer_context_loss`, `renderer_id_ownership`, `stream_guards`, `renderer_mirrored_instances` |
| Physics | `physics_world_bodies_and_events`, `physics_joints_and_grab`, `physics_scale_is_a_unit_system` |
| JSON | `json_numbers_match_javascript`, `json_parse_and_stringify` |
| Game runtime | `game_level_round_trip_and_validation`, `game_world_orb_run_play`, `game_world_rules` |
| Logic | `logic_flow_variables_and_latent_nodes`, `logic_state_machine_timers_events_conditions`, `logic_tier_cases_interpreter` |
| Audio | `audio_mixer_spatial`, `audio_builtin_library`, `audio_collider_impact_sounds` |
| Networking | `net_serialization`, `net_reliable_channel_under_bad_network`, `net_client_server`, `net_game_server_and_clients` |
| Studio | `studio_starts_with_orb_run`, `studio_gizmo_translate_and_undo`, `studio_undo_redo_is_exact_and_deterministic`, `studio_save_load_and_textures` |
| Micro Duck | `micro_duck_policy_matches_the_onnx_reference`, `micro_duck_stands_walks_and_turns_on_its_policy`, `micro_duck_runs_every_move_of_its_set` |
| Lehrer | `lehrer_mlp_gradient_matches_finite_differences`, `lehrer_ppo_learns_to_reach`, `lehrer_exports_onnx_like_pollens_networks`, `lehrer_calibrates_a_servo_from_bench_logs` |
| Determinism | `determinism_is_bit_identical_across_builds_and_platforms`, `lehrer_runs_the_same_on_any_number_of_workers` |

### The GL Stub

There is no WebGL natively. `kern/tests/gl_stub.cpp` provides the functions from
`include/kern/gfx/gl.h` as a recording stub. With it, the renderer tests check validation, instance
capacity, the uniform cache, shadow-map reuse, context loss, lines and planes – without a GPU. The
same stub is linked into the native Lehrer.

---

## 2.7 Logic Tests: Three Tiers, One Result

A logic graph can run in three tiers ([Chapter 9](09-logic.md)): in the **interpreter**, as a **JIT**
(a WebAssembly module generated at runtime) and as **exported C++**. All three must produce
bit-identical results. The shared test cases live in `kern/tests/logic_cases.h` (graphs as JSON,
e.g. `math_loops`, `gates_and_sequences`).

| Tier | Test | Where |
|---|---|---|
| Interpreter | `logic_tier_cases_interpreter` in `test_main.cpp` | `run.sh` |
| Exported C++ | `logic_native_test.cpp` | `run.sh` (second phase, only without arguments) |
| JIT (WebAssembly) | `logic_wasm_test.cpp` + `logic_wasm_test.mjs` | CI job `build` |

**Exported C++.** `kern_tests --emit-logic <dir>` writes, for each case, the generated C++ source
(`logic_<name>.cpp`) and the interpreter's results (`logic_expected.txt`). `run.sh` compiles the
generated sources together with the objects of `src/logic` and `src/core` and musl into
`logic_native_test` and compares. Every graph must run in the `native C++` tier.

**JIT.** CI builds `logic_wasm_test.cpp` as a separate module and runs it under Node:

```sh
kern/build.sh /tmp/logic_test.wasm kern/tests/logic_wasm_test.cpp -Ikern/tests
node kern/tests/logic_wasm_test.mjs /tmp/logic_test.wasm
```

Each case runs once in the interpreter and once JIT-compiled; V8 compiles the generated module and
links it to the engine's function table – exactly as in the browser. The tier must be reported as
`compiled (WebAssembly)` and the results must be identical. Finally, a benchmark measures one million
loop iterations in both tiers and prints the speed-up factor.

---

## 2.8 The Browser Test (e2e)

```
node kern/tests/e2e/studio_e2e.mjs [base-url]     (default http://127.0.0.1:8771)
```

Beforehand, the repository root must be served statically (`python3 -m http.server 8771`), and
`generated/studio.html` together with `generated/kern_studio.wasm` must exist. Playwright is looked up
via `import`; for a global installation the test falls back to `require()` (ESM ignores
`NODE_PATH`).

The test launches Chromium with software WebGL (`--use-gl=angle --use-angle=swiftshader
--enable-unsafe-swiftshader --ignore-gpu-blocklist`). Because SwiftShader renders the city at about
one frame per second, every check waits for its effect rather than for a fixed time
(`until(fn, ms)`). Whether the viewport has actually drawn is measured by `ink()`: the number of
distinct colors in the canvas.

Among other things, the test checks:

- **Single-file build** (`/generated/studio.html`): startup with Orb Run, menus, Fly Mode (right mouse
  button + W), tools, Outliner and Details, editing the X position, focus (F), Content Drawer (click
  and drag), selecting and moving with the left mouse button, sound via Web Audio, context menus,
  deterministic undo, Play, Simulate with physics debug, four viewports, a new level from a template,
  Shooting Range, the logic editor, the Micro Duck with head camera and ToF, help, multiplayer in the
  editor and across two tabs (BroadcastChannel), packaging a game and playing the packaged game, the
  game on a phone.
- **Module build** (`/kern/web/studio/index.html`): startup, color picker.
- **Restore** after a reload, **settings** (defaults, saving, resetting), **narrow window**,
  **phone** (compact layout, menus via touch, Micro Duck, Play/Simulate/Pause/Stop via tap),
  **Android file picker**.

At the end the test prints `<n> checks, <m> failures (screenshots: <folder>)`; the exit code is 1 on
failures. In GitHub Actions, each failure is additionally emitted as an `::error::` annotation.

### Screenshots

Screenshots are slow with software WebGL. The test therefore takes them **only if `KERN_E2E_SHOTS`
names a folder**:

```sh
KERN_E2E_SHOTS=artifacts/e2e node kern/tests/e2e/studio_e2e.mjs http://127.0.0.1:8771
```

This produces `studio.png`, `studio-selected.png`, `studio-play.png`, `studio-simulate.png`,
`studio-quad.png`, `studio-physics-lab.png`, `studio-multiplayer.png`, `game.png`,
`game-phone.png`, `studio-narrow.png` and, per phone size, `phone-play-<width>.png` and
`phone-dock-<width>.png`. Some of them are also used in this book.

![Simulate with the physics debug view, as checked by the browser test](../buch/bilder/studio-simulate.png)

---

## 2.9 The Determinism Fingerprint

The fingerprint proves that every build variant simulates the Micro Duck bit for bit identically.
`lehrer::determinism_fingerprint(ticks)` (`src/lehrer/fingerprint.cpp`) makes the duck walk and turn
with Pollen's `velstand` network in the Studio physics – 150 ticks forward (command 0.15), then
turning on the spot (0.8) – and computes, for each control tick, an FNV-1a hash over position,
rotation, velocity, angular velocity, all joint angles and joint velocities.

The reference for 400 ticks lives in `kern/tests/determinism_fingerprint.txt`, one line per tick:

```
0 7588b1a57dfadf95
1 46e1d239a7ed5cfe
2 4770483abd640417
...
```

It is checked in three places:

| Platform | Check |
|---|---|
| GCC on x86 with sanitizers | test `determinism_is_bit_identical_across_builds_and_platforms` (`run.sh`); on a mismatch it prints the first differing tick |
| WebAssembly in Node | `kern/tests/lehrer_wasm_test.mjs` via `kern_lehrer_fingerprint(400)` |
| Apple clang on ARM | CI job `determinism` on `macos-14`: `kern_lehrer --fingerprint 400 \| diff - kern/tests/determinism_fingerprint.txt` |

By hand:

```sh
kern/tools/lehrer.sh
kern/build/kern_lehrer --fingerprint 400 | diff - kern/tests/determinism_fingerprint.txt
```

If the fingerprint differs, either the simulation was changed on purpose (then the reference file
must be regenerated and the change justified) or some piece of code computes in a platform-dependent
way – for instance a `std::sin` instead of `kern::dm::sin` in code that feeds the simulation, a
missing `-ffp-contract=off` option, or a sum whose order depends on the number of threads
([Chapter 3](03-foundations.md), [Chapter 14](14-lehrer.md)).

The test `lehrer_runs_the_same_on_any_number_of_workers` complements this: it trains with 1, 2 and 4
workers (8 ducks in total) and compares the networks byte for byte.

---

## 2.10 CI in Detail

`.github/workflows/ci.yml` is a single pipeline. It runs on every push to `main`, on pull requests
and manually (`workflow_dispatch`). The site is built once and published as soon as it is built;
native and browser tests run in parallel and do not hold up publishing.

```
          ┌──────────┐   ┌──────────────┐
 push ──▶ │ native   │   │ determinism  │      (independent, parallel)
          └──────────┘   └──────────────┘
          ┌──────────┐
          │ build    │──┬──▶ deploy  (main only, no pull request)
          └──────────┘  └──▶ e2e
```

**Concurrency:** the group is `ci-<ref>-pr` for pull requests and `ci-<ref>-<sha>` otherwise; only
older runs of a pull request are cancelled. On `main`, every push runs to completion so that every
green state is published (according to the comment, a run takes about 17 minutes).
Permissions: `contents: write` (for the push to `gh-pages`).

### Job `native` (ubuntu-latest, 30 min)

```yaml
- name: Native unit tests (ASan/UBSan)
  run: CXX=g++ CC=gcc kern/tests/run.sh
```

All unit tests with sanitizers, followed by the logic export test.

### Job `determinism` (macos-14, 30 min)

```yaml
- name: Build the Lehrer (Apple clang, arm64)
  run: KERN_NATIVE=1 kern/tools/lehrer.sh build-mac
- name: Same hashes as x86 and wasm
  run: build-mac/kern_lehrer --fingerprint 400 | diff - kern/tests/determinism_fingerprint.txt
```

The Lehrer, built with Apple clang on ARM and `-mcpu=native`, must move the duck exactly as GCC on
x86 and WebAssembly do.

### Job `build` (ubuntu-latest, 30 min)

1. Checkout, Node 22.
2. **wasi-sdk 25** from the cache (`wasi-sdk-25.0`), otherwise downloaded to `/home/runner/wasi-sdk`.
3. **Object cache** `/home/runner/kern-obj` (`KERN_OBJ_CACHE`), key `kern-obj-<sha>`, restored with
   the prefix `kern-obj-` – that is, from the most recent earlier run.
4. **Build:**

   ```sh
   tools/build_studio.sh
   kern/build.sh --app kern/examples/hello/hello.wasm kern/examples/hello/hello.cpp
   kern/build.sh --app kern/examples/orbrun/orbrun.wasm kern/examples/orbrun/orbrun.cpp
   kern/build.sh --lehrer generated/kern_lehrer.wasm
   for f in kern/web/kern.mjs kern/web/app.mjs kern/web/studio/*.mjs kern/web/lehrer/*.mjs tools/net_relay.mjs; do node --check "$f"; done
   ```

   `node --check` verifies the syntax of all JavaScript files.
5. **Lehrer in WebAssembly:** `node kern/tests/lehrer_wasm_test.mjs generated/kern_lehrer.wasm`. Via
   the C ABI, the test creates two ducks as collectors and one learner for `jump` (setting
   `{"steps":8}`), checks the recipe's imitation rounds (8 rounds of 96 episodes each), runs part of a
   DAgger round and one imitation round, collects a rollout, learns one PPO iteration, exports ONNX
   and compares the fingerprint with the reference file.
6. **Logic JIT:** as in [2.7](#27-logic-tests-three-tiers-one-result).
7. **Assemble the site** in `_site/`:

   | Target | Source |
   |---|---|
   | `index.html` | `site/index.html` |
   | `LICENSE.txt`, `LICENSES.txt` | `LICENSE` (root) and `kern/LICENSES.md`; the start page's footer links both |
   | `editor/index.html` | `generated/studio.html`, prefixed with the comment `<!--BUILD_SHA:<sha>-->` |
   | `editor/build.json` | `{"sha":"<sha>"}` for the update check of the installable editor |
   | `editor/` | `site/editor/` (manifest, service worker, icons) |
   | `editor/microduck/` | `kern/third_party/microduck/*.onnx` (Pollen's policy set, Apache-2.0) |
   | `editor/policies/` | `kern/policies/*.onnx` (Kern's own networks) |
   | `generated/` | `kern_studio.wasm`, `kern_lehrer.wasm` |
   | `lehrer/index.html`, `kern/web/lehrer/*.mjs` | the Lehrer page and its workers |
   | `train/index.html` | redirect from the old address `/train/` to `../lehrer/` |
   | `kern/web/` | `kern.mjs`, `app.mjs`, `studio/*.mjs`, `studio/*.html`, `studio/*.css` |
   | `kern/examples/hello/`, `kern/examples/orbrun/` | `index.html` and `.wasm` of the examples |
   | `.nojekyll` | GitHub Pages without Jekyll |

8. Upload as the artifact `site` (7 days, including hidden files).

### Job `deploy` (`main` only, no pull request; needs `build`)

Downloads the `site` artifact, creates a fresh Git repository in it with the branch `gh-pages`,
commits everything as `Pages: <sha>` and force-pushes (`-f`) to `gh-pages`. The `pages` group does
not cancel a push in progress; only the most recent waiting one follows. The site appears at
<https://christianhohlfeld.github.io/kern/>.

Note: `README.md` still mentions a separate `pages.yml`. The repository contains only `ci.yml`;
publishing is done there by the `deploy` job.

### Job `e2e` (ubuntu-latest, 30 min; needs `build`)

1. Downloads the `site` artifact and restores from it `generated/studio.html`,
   `generated/kern_studio.wasm` and the app files (`manifest.webmanifest`, `sw.js`, `build.json`,
   `icons`, `microduck`) – exactly as they are published.
2. Playwright 1.56.0 from the cache or via `npm install` into `/tmp/e2e`, then
   `playwright install --with-deps chromium`.
3. Starts `python3 -m http.server 8771 --bind 127.0.0.1` and runs
   `NODE_PATH=/tmp/e2e/node_modules node kern/tests/e2e/studio_e2e.mjs http://127.0.0.1:8771`.
4. Uploads `artifacts/e2e/` as the artifact `e2e-screenshots` (also on failure; if the folder is
   missing or empty, this is ignored). The step does not set `KERN_E2E_SHOTS`; screenshots are only
   produced when the variable is set ([2.8](#28-the-browser-test-e2e)).

---

## 2.11 Running Locally Like CI

A local sequence that matches CI:

```sh
export WASI_SDK=/opt/wasi-sdk-25.0-x86_64-linux

# native tests
CXX=g++ CC=gcc kern/tests/run.sh

# determinism with the native Lehrer
kern/tools/lehrer.sh
kern/build/kern_lehrer --fingerprint 400 | diff - kern/tests/determinism_fingerprint.txt

# modules
tools/build_studio.sh
kern/build.sh --lehrer generated/kern_lehrer.wasm
node kern/tests/lehrer_wasm_test.mjs generated/kern_lehrer.wasm
kern/build.sh /tmp/logic_test.wasm kern/tests/logic_wasm_test.cpp -Ikern/tests
node kern/tests/logic_wasm_test.mjs /tmp/logic_test.wasm

# browser
python3 -m http.server 8771 &
node kern/tests/e2e/studio_e2e.mjs http://127.0.0.1:8771
```

The development pages load their files relatively: `kern/web/studio/index.html` and `play.html` load
`../../../generated/kern_studio.wasm`; the Micro Duck's policies come from
`kern/third_party/microduck/` and `kern/policies/` on the development page, from
`../kern/third_party/microduck/` and `../kern/policies/` in the single-file build under
`/generated/`, and from `microduck/` and `policies/` next to the editor on the published site
(`loadPolicies` in `game.mjs`). The Lehrer page `kern/web/lehrer/index.html` includes
`../kern/web/lehrer/lehrer.mjs` and is therefore laid out for the structure of the published site
(`/lehrer/`).

---

## 2.12 Common Errors

| Message / symptom | Cause | Remedy |
|---|---|---|
| `wasi-sdk clang++ not found at …/bin/clang++ (set WASI_SDK)` | `WASI_SDK` is missing or points to the wrong place | `export WASI_SDK=…` pointing to the extracted wasi-sdk 25 |
| `missing …/generated/kern_studio.wasm (run without --skip-wasm)` | `build_studio.sh --skip-wasm` without an existing module | build once without `--skip-wasm` |
| `<name> source contains '</script'; cannot inline it` | a JS source contains `</script` | avoid the string in the source (e.g. split it) |
| Test build aborts on a warning | `run.sh` compiles with `-Werror` | fix the warning; `build.sh` is more tolerant, the tests are not |
| Abort with `runtime error:` / `AddressSanitizer` | UBSan/ASan found a bug | the report names file and line; reproduce it in isolation with a filter (`run.sh <name>`) |
| `first differing tick: N`, or the `diff` against the fingerprint fails | the simulation computes in a platform-dependent way or was changed | see [2.9](#29-the-determinism-fingerprint) |
| Logic test `runs in "interpreter"` instead of `compiled (WebAssembly)` | JIT unavailable or module not compilable | check the JIT output; the engine falls back to the interpreter (`logic JIT failed …` in the console) |
| Every small header edit rebuilds everything | intended: header hash in the cache key | – |
| Browser test hangs at `openStudio` | page not served or module missing; the test waits up to 90 s for `KernStudio.frames() > 5` | start the server in the repository root, build `generated/` |
| Playwright import fails | package not found | `npm install playwright` (CI: 1.56.0) and set `NODE_PATH`, as in CI |
| Lehrer page locally without a module | paths are laid out for the published structure | assemble the site as in [2.10](#job-build-ubuntu-latest-30-min) |

---

© 2026 Christian Heinrich Hohlfeld · All rights reserved · [Imprint and rights](00-imprint.md)
