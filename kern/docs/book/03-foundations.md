# 3 Foundations

Every layer of Kern rests on four headers:

| Header | Namespace | Contents |
|---|---|---|
| `include/kern/core/base.h` | `kern` | integer and floating-point types, error channel, logging, time, `min_of`/`max_of`/`clamp`, `Span`, hashes |
| `include/kern/core/json.h` | `kern::json` | JSON document model, parser, formatter |
| `include/kern/math/math.h` | `kern` | `Vec2`, `Vec3`, `Vec4`, `Quat`, `Mat4`, `Sphere`, `Aabb`, `Camera` |
| `include/kern/core/dmath.h` | `kern::dm` | deterministic transcendental functions (musl) |

This chapter describes them together with their conventions and the reason behind each decision. The
examples are taken from the repository.

---

## 3.1 `kern::core` – `base.h`

### Types

```cpp
using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i8 = std::int8_t;
using i16 = std::int16_t;
using i32 = std::int32_t;
using i64 = std::int64_t;
using f32 = float;
using f64 = double;
```

Kern always spells out widths. This matters at the boundary to WebAssembly: there a pointer is
32 bits wide, `f64` is what JavaScript knows as `Number`, and the C ABI descriptors are `u32` words
([Chapter 17](17-reference.md)).

### The Error Channel: `fail()` and `last_error()`

Kern is built **without exceptions and without RTTI** (`-fno-exceptions -fno-rtti`). Fallible
operations return `bool` (or 0 or `nullptr`) and first store a readable message:

```cpp
/// Records the last error (printf-style). Always returns false so call sites
/// can write `return fail("...")`.
bool fail(const char* format, ...) __attribute__((format(printf, 1, 2)));
/// The message of the most recent fail(); empty string when none.
const char* last_error();
void clear_error();
```

Because `fail()` always returns `false`, a check becomes a single line. This is how it looks in
`src/scene/picking.cpp`:

```cpp
bool screen_ray(const Camera& camera, f32 x, f32 y, f32 width, f32 height, Ray& out) {
  if (!(width > 0 && height > 0) || !std::isfinite(x) || !std::isfinite(y)) return fail("invalid viewport coordinates");
  if (!camera_valid(camera)) return fail("invalid camera");
  bool ok = false;
  const Mat4 view = Mat4::look_at(camera.eye, camera.target, camera.up, &ok);
  if (!ok) return fail("degenerate camera direction/up");
  const Mat4 vp = projection_matrix(camera, width / height) * view;
  f64 inv[16];
  if (!invert(vp, inv)) return fail("singular view projection");
  // ...
```

Properties you need to know:

- The message lives in **a single** process-wide buffer of 1024 characters (`src/core/base.cpp`); it
  is formatted with `vsnprintf` and truncated if necessary. Every new `fail()` overwrites the
  previous message. The buffer is not thread-local.
- `last_error()` returns the message of the **most recent** `fail()`, not necessarily that of the
  most recent failed call. To be sure, call `clear_error()` beforehand.
- For JavaScript hosts the functions are called `kern_last_error()` and `kern_clear_error()`
  ([Chapter 17](17-reference.md)).
- Checks of the form `!(x > 0)` instead of `x <= 0` are intentional: they also catch `NaN`, because
  every comparison with `NaN` is false.

Some modules additionally take their own error-text parameter (`std::string* error`), for example
`json::parse`, `parse_onnx_policy` or `servo_profile_from_json`. These write their message there and
not into `last_error()`.

### Logging

```cpp
enum class LogLevel : i32 { debug = 0, info = 1, warn = 2, error = 3 };
void log(LogLevel level, const char* format, ...) __attribute__((format(printf, 2, 3)));
```

`log()` formats into a 1024-character buffer and hands the text to the platform:

- **WebAssembly:** to the import `kern_env.log(level, text, length)`. The platform layer
  (`kern/web/kern.mjs`) prints it with the prefix `[kern]` – `console.error` from level 3,
  `console.warn` at 2, otherwise `console.log` – or passes it to a custom `logSink` if the host sets
  one.
- **Native:** level 2 and above to `stderr`, otherwise to `stdout`.

A typical use combines logging with the error channel (`src/gfx/bloom.cpp`):

```cpp
    supported_ = false;
    log(LogLevel::warn, "bloom disabled: %s", last_error());
    return false;
```

### Time

```cpp
/// Monotonic milliseconds (performance.now() in the browser).
f64 now_ms();
```

In the browser, `now_ms()` calls the import `kern_env.now` (`performance.now()`); natively it uses
`std::chrono::steady_clock`. `now_ms()` is meant for time budgets and measurements, for instance when
the sound bank precomputes built-in sounds within a limited time (`src/game/sound.cpp`). **It does
not belong in the simulation**: simulations count fixed steps, not wall-clock time; otherwise they
would not be deterministic ([Chapter 6](06-physics.md), [Chapter 13](13-micro-duck.md)).

### Small Helpers

```cpp
template <class T>
constexpr T min_of(T a, T b) { return b < a ? b : a; }
template <class T>
constexpr T max_of(T a, T b) { return a < b ? b : a; }
template <class T>
constexpr T clamp(T v, T lo, T hi) { return v < lo ? lo : (hi < v ? hi : v); }
```

All three are `constexpr` and take both arguments of the same type; with mixed types you specify the
type explicitly, e.g. `min_of<u32>(d[KERN_MD_ALPHA_MODE], 2)` in `src/api/c_api.cpp`.

### `Span<T>`

```cpp
/// A non-owning view of contiguous elements (std::span without the header weight).
template <class T>
struct Span {
  T* data = nullptr;
  std::size_t size = 0;
  constexpr Span() = default;
  constexpr Span(T* d, std::size_t n) : data(d), size(n) {}
  template <class C>
    requires(!std::is_same_v<std::remove_cv_t<C>, Span> && requires(C& c) { c.data(); c.size(); })
  constexpr Span(C& c) : data(c.data()), size(c.size()) {}
  constexpr T& operator[](std::size_t i) const { return data[i]; }
  constexpr T* begin() const { return data; }
  constexpr T* end() const { return data + size; }
  constexpr bool empty() const { return size == 0; }
};
```

`Span` is a non-owning view of contiguous elements: `std::span` without its header weight. `data` and
`size` are public fields (not methods as in `std::span`). Any container with `data()` and `size()`
converts implicitly to a `Span`.

`Span` is the format in which data flows into the engine – from a C++ game as well as from the
JavaScript host. `MeshInput` consists of spans; the C ABI builds them directly from pointers into
linear memory. In the example game `hello` (`kern/examples/hello/hello.cpp`):

```cpp
    MeshInput b = box_mesh_.as_input();
    b.material.roughness = 0.45f;
    b.transforms = Span<const f32>(transforms_.data(), transforms_.size());
    b.tints = Span<const f32>(tints_.data(), tints_.size());
    return r.upsert(boxes_, b);
```

A span does not outlive the data it points to. Functions that need to keep data copy it (the
renderer keeps CPU copies for picking and for restoring after context loss, [Chapter 4](04-graphics.md);
the C ABI's geometry jobs copy their input stream because the host reuses its staging memory).

### Hashes

```cpp
/// FNV-1a, used for string keys and cache signatures.
constexpr u64 hash_bytes(const void* data, std::size_t n, u64 seed = 1469598103934665603ull);

/// 32-bit integer mix (lowbias32), for spatial hashing.
constexpr u32 hash_u32(u32 x);
```

- `hash_bytes` is 64-bit FNV-1a with a selectable seed. The renderer's uniform cache uses it to detect
  unchanged values (`src/gfx/program.cpp`), and the shadow map uses it for its key
  (`src/gfx/shadow.cpp`). The determinism fingerprint is also an FNV-1a over the duck's state
  ([Chapter 2](02-building-and-testing.md)).
- `hash_u32` is the integer mixer *lowbias32*, used for example for deterministic noise in the game
  runtime (`noise()` in `src/game/runtime_internal.h`).

Both are `constexpr` and purely integer-based: the result depends only on the bytes you feed in.

---

## 3.2 `kern::json` – `json.h`

Kern needs JSON everywhere: levels (`.klevel`), the protocol between the Studio and the page, servo
profiles, the Lehrer's demonstrations, the Micro Duck's policy manifest. `kern::json` is a small
document model of its own with three guarantees (header comment of `json.h`):

1. **Objects keep their key order.** A document written by this formatter or by JavaScript's
   `JSON.stringify` survives `parse` + `stringify` byte for byte. This keeps level files
   diff-friendly.
2. **Numbers are IEEE doubles and are formatted as in JavaScript** (ECMAScript Number-to-String:
   shortest unambiguous digit sequence, same exponent rules). The output is therefore identical to
   `JSON.stringify`.
3. **The parser is built for untrusted files:** it rejects non-finite results, nesting deeper than
   `max_depth` and, by default, the keys `__proto__`, `prototype` and `constructor` (prototype
   pollution on the JavaScript side).

### `json::Value`

| Group | Functions |
|---|---|
| Construction | `Value()` (null), `Value(bool)`, `Value(f64)`, `Value(i32)`, `Value(u32)`, `Value(i64)`, `Value(const char*)`, `Value(std::string)`, `Value(std::string_view)`, `Value::array()`, `Value::object()` |
| Type | `type()`, `is_null()`, `is_bool()`, `is_number()`, `is_string()`, `is_array()`, `is_object()` |
| Reading | `as_bool(fallback)`, `as_number(fallback)`, `as_string()` (empty string for other types), `truthy()` (JavaScript truthiness) |
| Arrays | `size()`, `operator[](index)`, `at(index)`, `push(v)`, `items()` |
| Objects | `find(key)`, `has(key)`, `operator[](key)`, `set(key, v)`, `remove(key)`, `members()` |
| Comparison | `operator==`, `operator!=` |

Important for safe code:

- `operator[]` on a missing key or index returns a **null value** instead of an error. Chains such as
  `v["a"][2]["b"]` are therefore always safe.
- `as_number`/`as_bool` return the fallback value for the wrong type. To distinguish between
  "missing" and "wrong type", check with `find` and `is_number`.
- `set` replaces an existing key **in place** or appends it.
- `push` first turns a non-array into an empty array.

### Parsing and Formatting

```cpp
struct ParseOptions {
  u32 max_depth = 80;
  bool reject_unsafe_keys = true;
};

bool parse(std::string_view text, Value& out, std::string* error = nullptr, const ParseOptions& options = {});
std::string stringify(const Value& value, i32 indent = -1);
void append_number(std::string& out, f64 value);
void append_string(std::string& out, std::string_view text);
```

- Error messages carry the byte offset (`"… at offset 17"`); text after the value yields
  `trailing characters`. A UTF-8 BOM at the start is tolerated.
- `out` is only overwritten on success.
- `stringify(v, indent)` corresponds to `JSON.stringify(value, null, indent)`; `indent <= 0` writes
  compactly. Non-finite numbers become `null`, as in JSON.
- The Studio C ABI allows a depth of 120 for commands (`kern_studio_command` in
  `src/studio/api.cpp`); otherwise 80 applies.

The test `json_numbers_match_javascript` pins down the number formatting (excerpt):

```cpp
  auto num = [](f64 v) { std::string s; json::append_number(s, v); return s; };
  CHECK(num(0.1) == "0.1");
  CHECK(num(-0.0) == "0");
  CHECK(num(1e21) == "1e+21");
  CHECK(num(1e20) == "100000000000000000000");
  CHECK(num(1e-7) == "1e-7");
  CHECK(num(0.30000000000000004) == "0.30000000000000004");
  CHECK(num(NAN) == "null");
```

### Example: Reading and Writing a Servo Profile

`src/game/servo.cpp` shows the typical pattern – look up fields individually, check type and range,
and only commit at the end:

```cpp
bool servo_profile_from_json(std::string_view text, ServoProfile& out, std::string* error) {
  json::Value v;
  if (!json::parse(text, v, error)) return false;
  if (!v.is_object() || (v.has("kind") && v["kind"].as_string() != "servo")) {
    if (error) *error = "not a servo profile";
    return false;
  }
  ServoProfile p;
  if (const json::Value* n = v.find("name"); n && n->is_string()) p.name = n->as_string();
  if (const json::Value* s = v.find("source"); s && s->is_string()) p.source = s->as_string();
  for (const Field& f : kFields) {
    const json::Value* x = v.find(f.key);
    if (!x) continue;
    const f64 d = x->as_number(-1);
    if (!x->is_number() || !(d >= f.lo && d <= f.hi)) {
      if (error) *error = std::string("bad value for ") + f.key;
      return false;
    }
    p.*(f.member) = static_cast<f32>(d);
  }
  // ...
  out = std::move(p);
  return true;
}
```

And in the other direction:

```cpp
std::string to_json(const ServoProfile& p) {
  json::Value o = json::Value::object();
  o.set("kind", std::string("servo"));
  o.set("name", p.name);
  if (!p.source.empty()) o.set("source", p.source);
  for (const Field& f : kFields) o.set(f.key, static_cast<f64>(p.*(f.member)));
  return json::stringify(o, 2) + "\n";
}
```

---

## 3.3 `kern::math` – Vectors, Quaternions, Matrices

`math.h` is header-only and allocation-free so that the compiler can inline the hot paths (instance
matrices, culling, picking).

### Conventions

| Convention | Definition |
|---|---|
| Units | meters (and seconds, kilograms in physics and audio) |
| Handedness | right-handed |
| Up | **+Z** |
| Matrices | 4×4, stored **column-major** (OpenGL layout): element (row r, column c) is `m[c * 4 + r]`; the translation is in `m[12]`, `m[13]`, `m[14]` |
| Multiplication | column vectors: `p' = M * p`; `A * B` applies `B` first, then `A` |
| Quaternions | `(x, y, z, w)`, unit quaternion, default `w = 1` |
| Angles | radians in `math.h`; levels store Euler angles in **degrees** ([Chapter 8](08-game-runtime.md)) |
| Projection | OpenGL clip space (depth −1 … 1); the camera looks along −Z in view coordinates |

Z up applies everywhere: in the renderer, in the physics (`physics.h`: "metres, seconds, kilograms,
Z up, quaternions (x, y, z, w)"), in audio (`audio.h`) and in levels. The primitives in
`geo/primitives.h` are likewise Z up and, unless stated otherwise, centered on the origin
([Chapter 5](05-scene-and-geometry.md)).

### Vectors

`Vec2`, `Vec3` and `Vec4` are simple structs with `f32` fields and `constexpr` operators.

| Function | Meaning |
|---|---|
| `dot`, `cross` | dot and cross product (for `Vec2`, `cross` returns the z component) |
| `length`, `length_sq`, `distance`, `distance_sq` | lengths and distances |
| `normalize(a, fallback = {0, 0, 1})` | unit vector; for length ≤ 1e-9 the fallback instead of `NaN` |
| `lerp`, `vmin`, `vmax` | interpolation, component-wise minimum/maximum |
| `finite(v)` | all components finite |
| `hex_color(0xRRGGBB)` | color 0…1 without color-space conversion |
| `Vec3::load(p)`, `store(p)` | from/to an `f32` array |
| `Vec3 * Vec3` | component-wise |
| `Vec4::xyz()` | the first three components |

`normalize` with a fallback is a robustness decision: a zero vector from user input (say, two
identical points) does not produce `NaN`s that propagate through the scene.

### `Quat`

```cpp
/// Unit quaternion (x, y, z, w).
struct Quat {
  f32 x = 0, y = 0, z = 0, w = 1;
  static Quat axis_angle(Vec3 axis, f32 radians);  // axis is normalized
  Quat operator*(const Quat& b) const;             // Hamilton product: b first, then *this
  Vec3 rotate(Vec3 v) const;
};
```

`axis_angle` computes with `kern::dm::sin` and `kern::dm::cos` ([3.4](#34-deterministic-math-dmathh))
so that rotations that feed into the simulation produce the same bits everywhere.

### `Mat4`

| Function | Meaning |
|---|---|
| `Mat4()` / `identity()` | identity matrix (default value) |
| `load(p)` | take 16 `f32` (column-major) |
| `translation()`, `is_identity()` | read the translation; exact comparison with the identity |
| `operator*` | matrix product |
| `transform_point(p)` | affine: rotates, scales, translates (without perspective division) |
| `transform_vector(v)` | only the 3×3 part (directions) |
| `transform(Vec4)` | full 4×4 multiplication (e.g. into clip space) |
| `translate(t)`, `scale(s)` | basic matrices |
| `trs(t, q, s)` | translation × rotation × scale in one step |
| `perspective(fov, aspect, near, far)` | perspective; `fov` is the **vertical** field of view in radians |
| `ortho(l, r, b, t, n, f)` | orthographic projection |
| `look_at(eye, target, up, &ok)` | view matrix; for a degenerate basis, the identity and `ok = false` |

Free functions:

- `normal_matrix3(m, out)` – inverse transpose of the upper 3×3 block (column-major in `out[9]`);
  `false` and the identity if singular. This keeps normals perpendicular under non-uniform scaling.
- `invert(src, f64 out[16])` – general inverse **in double precision**; `false` if the determinant is
  not finite or smaller than 1e-14 in magnitude. Picking uses it to invert the view-projection so
  that rays stay precise even at long view distances.

The example game `hello` sets instance matrices with `trs` and copies them into the instance stream –
exactly 16 `f32` per instance, column-major:

```cpp
  void layout(f32 time) {
    for (u32 i = 0; i < kCount; ++i) {
      const f32 angle = time * (0.5f + (i % 7) * 0.15f);
      const Vec3 p{(static_cast<f32>(i % kSide) - kSide * 0.5f) * 3, (static_cast<f32>(i / kSide) - kSide * 0.5f) * 3, 0};
      const Mat4 m = Mat4::trs(p, Quat::axis_angle({0, 0, 1}, angle), {1, 1, 1 + (i % 3) * 0.6f});
      std::memcpy(&transforms_[i * 16], m.m, sizeof m.m);
    }
  }
```

The test `math_matrices` checks that `trs` really is `T * R * S`:

```cpp
  const Mat4 t = Mat4::translate({1, 2, 3}), s = Mat4::scale({2, 3, 4});
  const Quat q = Quat::axis_angle({0, 0, 1}, kPi / 2);
  const Mat4 trs = Mat4::trs({1, 2, 3}, q, {2, 3, 4});
  // ...
  CHECK(mat_near(trs, t * r * s));
  const Vec3 p = trs.transform_point({1, 0, 0});
  CHECK_NEAR(p.x, 1.0f, 1e-5f);
  CHECK_NEAR(p.y, 4.0f, 1e-5f);
  CHECK_NEAR(p.z, 3.0f, 1e-5f);
```

The point (1, 0, 0) is scaled by 2, rotated by 90° about Z (→ (0, 2, 0)) and translated by
(1, 2, 3) – giving (1, 4, 3).

### `Sphere` and `Aabb`

- `Sphere { center, radius = -1 }` – a negative radius means "empty" (`valid()`). Frustum culling
  treats invalid spheres as visible: when in doubt, draw.
- `Aabb` starts inverted (`min = +1e30`, `max = −1e30`) so that the first `extend` sets the box;
  `valid()`, `center()`, `extent()`.

### `Camera`

```cpp
/// Camera description shared by the renderer, picking and stereo rigs.
struct Camera {
  Vec3 eye{0, -18, 8}, target{0, 0, 2}, up{0, 0, 1};
  f32 fov = kPi / 3, near = 0.05f, far = 1800;
  bool orthographic = false;  // parallel projection (editor top/front/side views)
  f32 ortho_height = 20;      // orthographic: visible height in metres
};
```

- The camera is a **description** (eye point, target, up vector), not a matrix. The renderer,
  picking and the editor derive the matrices from it. `up` defaults to +Z.
- `fov` is vertical, in radians (default π/3 = 60°).
- Orthographic cameras (top, front and side views in the Studio) specify the visible height in
  meters instead of `fov`.
- `camera_valid(c)` checks the parameters (`far > near`; for perspective `near > 0` and
  `0 < fov < π`; for orthographic `ortho_height > 0`, finite, `near >= 0`).
- `projection_matrix(c, aspect)` chooses `perspective` or `ortho`.

The view-projection matrix is obtained as in `screen_ray` (see
[3.1](#the-error-channel-fail-and-last_error)):

```cpp
  const Mat4 view = Mat4::look_at(camera.eye, camera.target, camera.up, &ok);
  const Mat4 vp = projection_matrix(camera, width / height) * view;
```

From `vp`, `Frustum::from_matrix(vp)` (`scene/frustum.h`) extracts the six planes for culling. The
test `math_camera` checks that the target lands in the center of the image:

```cpp
  const Vec3 eye{10, -5, 4}, target{0, 3, 1};
  bool ok = false;
  const Mat4 view = Mat4::look_at(eye, target, {0, 0, 1}, &ok);
  CHECK(ok);
  const Mat4 vp = Mat4::perspective(1.0f, 1.6f, 0.1f, 500.0f) * view;
  const Vec4 c = vp.transform({target, 1});
  CHECK_NEAR(c.x / c.w, 0.0f, 1e-5f);
  CHECK_NEAR(c.y / c.w, 0.0f, 1e-5f);
```

Through the C ABI, a host sets the camera with twelve `f32` values (eye xyz, target xyz, up xyz, fov,
near, far); `kern_camera_set` rejects invalid values with `fail("invalid camera")`
([Chapter 17](17-reference.md)).

---

## 3.4 Deterministic Math: `dmath.h`

### The Problem

`sqrt`, `fabs` and `floor` are exactly defined by IEEE 754; every platform returns the same bits.
This is **not** true of `sin`, `cos`, `exp`, `atan2` and the other transcendental functions: glibc,
Apple's libm, Android's bionic and wasi-libc round differently in the last bit. In a robot
simulation, a single bit turns into a different gait within seconds (header comment of `dmath.h`).
A network that learned in the browser would then behave differently natively, and a difference from
the real robot could not be told apart from numerical noise.

### The Solution: musl, Compiled the Same Way Everywhere

Kern ships the implementations from **musl** (MIT, `kern/third_party/musl_math`). They are compiled
the same way on every platform: without FMA contraction and without platform-specific paths. The file
`third_party/musl_math/src/kmath.h` replaces what the musl sources would otherwise get from
`<math.h>`:

- The functions are renamed to `kern_musl_*` via `#define` (`#define sin kern_musl_sin` …) so they
  never collide with the platform's libm.
- `__FP_FAST_FMA` is 0 everywhere; `float_t`/`double_t` are `float`/`double`
  (`FLT_EVAL_METHOD 0` on SSE2, ARM64 and wasm).
- Exact operations (`fabs`, `sqrt`, `floor`, `scalbn`) go through compiler builtins.

All three build paths compile musl with `-ffp-contract=off -fno-math-errno` (`kern/build.sh`,
`kern/tests/run.sh`, `kern/tools/lehrer.sh`), and all other code is compiled with
`-ffp-contract=off` as well. Box3D runs on the scalar path (`BOX3D_DISABLE_SIMD`) and has its own
deterministic trigonometric functions (e.g. `b3Atan2` in
`third_party/box3d/src/math_functions.c`).

### The API: `kern::dm::`

`include/kern/core/dmath.h` provides the functions in the `kern::dm` namespace, with the same
overload rules as `std::`:

| Functions | Overloads |
|---|---|
| `sin`, `cos`, `tan`, `asin`, `acos`, `atan`, `exp`, `expm1`, `log`, `log10` | `float` → `kern_musl_<f>f`, `double` → `kern_musl_<f>`, integer → `double` |
| `atan2`, `pow`, `hypot` | both `float` → `float` variant, otherwise `double` |

```cpp
#define KERN_DM_UNARY(name)                                                         \
  inline float name(float x) { return kern_musl_##name##f(x); }                     \
  inline double name(double x) { return kern_musl_##name(x); }                      \
  template <class T, class = std::enable_if_t<std::is_integral_v<T>>>               \
  inline double name(T x) { return kern_musl_##name(static_cast<double>(x)); }
```

It is used just like `std::`. From `math.h`:

```cpp
  static Quat axis_angle(Vec3 axis, f32 radians) {
    const Vec3 a = normalize(axis);
    const f32 s = kern::dm::sin(radians * 0.5f);
    return {a.x * s, a.y * s, a.z * s, kern::dm::cos(radians * 0.5f)};
  }
```

and in `Mat4::perspective`:

```cpp
    const f32 f = 1.0f / kern::dm::tan(fov * 0.5f), nf = 1.0f / (near - far);
```

`sqrt`, `fabs` and `floor` remain `std::`, because they are exact.

### The Rule for New Code

From `docs/lehrer.md`, section 7:

- Everything that **feeds the simulation** uses `kern::dm::sin` etc. instead of `std::sin`.
- **No floating-point ordering that depends on the number of threads** – sums in a fixed order.
- **No `-ffast-math` options.**

Pure presentation may use the platform libm. In the current state, for example, the synthesizer and
mixer (`src/audio/`), the primitives (`src/geo/primitives.cpp`), the particles and parts of the
editor (`gizmo.cpp`, `overlays.cpp`, `studio.cpp`) call `std::sin`/`std::cos`; the example game
`hello` positions its camera with `std::cos`/`std::sin`. As soon as such code produces values that
feed into the physics, a controller or training, it must be switched to `kern::dm::`.

### How to Check

The determinism fingerprint (`kern_lehrer --fingerprint 400`, test
`determinism_is_bit_identical_across_builds_and_platforms`, `kern/tests/lehrer_wasm_test.mjs`, CI job
`determinism` on ARM) reveals any deviation down to the exact control tick
([Chapter 2](02-building-and-testing.md)). According to `docs/lehrer.md`, Kern's own libm costs no
speed: musl's functions are as fast as the platform's, and the physics is dominated by Box3D's joint
solver.

---

## 3.5 Summary

- Kern reports errors through `bool` + `fail()` / `last_error()`, not through exceptions.
- `Span` is the engine's input format; whoever keeps data copies it.
- JSON in Kern behaves like JavaScript's `JSON`: key order, number format – and it is hardened
  against malicious files.
- Math: meters, right-handed, **Z up**, column-major matrices, quaternions `(x, y, z, w)`, `fov`
  vertical in radians.
- Simulation needs `kern::dm::` instead of `std::` for transcendental functions, `-ffp-contract=off`
  and fixed summation orders – otherwise it is no longer bit-for-bit identical.

---

© 2026 Christian Heinrich Hohlfeld · All rights reserved · [Imprint and rights](00-imprint.md)
