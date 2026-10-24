# Kapitel 3 – Grundlagen

Jede Schicht von Kern steht auf vier Headern:

| Header | Namensraum | Inhalt |
|---|---|---|
| `include/kern/core/base.h` | `kern` | Ganzzahl- und Gleitkommatypen, Fehlerkanal, Logging, Zeit, `min_of`/`max_of`/`clamp`, `Span`, Hashes |
| `include/kern/core/json.h` | `kern::json` | JSON-Dokumentmodell, Parser, Formatierer |
| `include/kern/math/math.h` | `kern` | `Vec2`, `Vec3`, `Vec4`, `Quat`, `Mat4`, `Sphere`, `Aabb`, `Camera` |
| `include/kern/core/dmath.h` | `kern::dm` | deterministische transzendente Funktionen (musl) |

Dieses Kapitel beschreibt sie mit ihren Konventionen und dem Grund für jede Entscheidung. Die
Beispiele stammen aus dem Repository.

---

## 3.1 `kern::core` – `base.h`

### Typen

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

Kern schreibt Breiten immer aus. Das ist an der Grenze zu WebAssembly wichtig: Dort ist ein Zeiger
32 Bit breit, `f64` ist das, was JavaScript als `Number` kennt, und die Deskriptoren der C-ABI sind
`u32`-Wörter ([Kapitel 17](17-referenz.md)).

### Der Fehlerkanal: `fail()` und `last_error()`

Kern wird **ohne Ausnahmen und ohne RTTI** gebaut (`-fno-exceptions -fno-rtti`). Fehlbare
Operationen geben `bool` zurück (oder 0 bzw. `nullptr`) und hinterlegen vorher eine lesbare Meldung:

```cpp
/// Records the last error (printf-style). Always returns false so call sites
/// can write `return fail("...")`.
bool fail(const char* format, ...) __attribute__((format(printf, 1, 2)));
/// The message of the most recent fail(); empty string when none.
const char* last_error();
void clear_error();
```

Weil `fail()` immer `false` liefert, wird eine Prüfung zu einer Zeile. So sieht das in
`src/scene/picking.cpp` aus:

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

Eigenschaften, die man kennen muss:

- Die Meldung liegt in **einem** prozessweiten Puffer von 1024 Zeichen (`src/core/base.cpp`); sie
  wird mit `vsnprintf` formatiert und bei Bedarf abgeschnitten. Jeder neue `fail()` überschreibt die
  vorige Meldung. Der Puffer ist nicht thread-lokal.
- `last_error()` liefert die Meldung des **letzten** `fail()`, nicht zwingend die des letzten
  fehlgeschlagenen Aufrufs. Wer sicher gehen will, ruft vorher `clear_error()`.
- Für JavaScript-Hosts heißen die Funktionen `kern_last_error()` und `kern_clear_error()`
  ([Kapitel 17](17-referenz.md)).
- Prüfungen der Form `!(x > 0)` statt `x <= 0` sind Absicht: Sie fangen auch `NaN`, weil jeder
  Vergleich mit `NaN` falsch ist.

Manche Module haben zusätzlich einen eigenen Fehlertext-Parameter (`std::string* error`), etwa
`json::parse`, `parse_onnx_policy` oder `servo_profile_from_json`. Diese schreiben ihre Meldung
dorthin und nicht in `last_error()`.

### Logging

```cpp
enum class LogLevel : i32 { debug = 0, info = 1, warn = 2, error = 3 };
void log(LogLevel level, const char* format, ...) __attribute__((format(printf, 2, 3)));
```

`log()` formatiert in einen Puffer von 1024 Zeichen und gibt den Text an die Plattform:

- **WebAssembly:** an den Import `kern_env.log(level, text, length)`. Die Plattformschicht
  (`kern/web/kern.mjs`) gibt ihn mit dem Präfix `[kern]` aus – `console.error` ab Stufe 3,
  `console.warn` bei 2, sonst `console.log` –, oder an einen eigenen `logSink`, wenn der Host einen
  setzt.
- **Nativ:** Stufe 2 und höher nach `stderr`, sonst nach `stdout`.

Ein typischer Einsatz verbindet Logging mit dem Fehlerkanal (`src/gfx/bloom.cpp`):

```cpp
    supported_ = false;
    log(LogLevel::warn, "bloom disabled: %s", last_error());
    return false;
```

### Zeit

```cpp
/// Monotonic milliseconds (performance.now() in the browser).
f64 now_ms();
```

Im Browser ruft `now_ms()` den Import `kern_env.now` (`performance.now()`), nativ
`std::chrono::steady_clock`. `now_ms()` dient Zeitbudgets und Messungen, etwa wenn die Sound-Bank
eingebaute Klänge für eine begrenzte Zeit vorrechnet (`src/game/sound.cpp`). **In die Simulation
gehört sie nicht**: Simulationen zählen feste Schritte, keine Wanduhrzeit, sonst wären sie nicht
deterministisch ([Kapitel 6](06-physik.md), [Kapitel 13](13-micro-duck.md)).

### Kleine Helfer

```cpp
template <class T>
constexpr T min_of(T a, T b) { return b < a ? b : a; }
template <class T>
constexpr T max_of(T a, T b) { return a < b ? b : a; }
template <class T>
constexpr T clamp(T v, T lo, T hi) { return v < lo ? lo : (hi < v ? hi : v); }
```

Alle drei sind `constexpr` und nehmen beide Argumente vom selben Typ; bei gemischten Typen gibt man
den Typ an, z. B. `min_of<u32>(d[KERN_MD_ALPHA_MODE], 2)` in `src/api/c_api.cpp`.

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

`Span` ist eine nicht besitzende Sicht auf zusammenhängende Elemente: `std::span` ohne dessen
Header-Gewicht. `data` und `size` sind öffentliche Felder (keine Methoden wie bei `std::span`).
Jeder Container mit `data()` und `size()` wandelt sich implizit in einen `Span`.

`Span` ist das Format, in dem Daten in die Engine fließen – vom C++-Spiel wie vom JavaScript-Host.
`MeshInput` besteht aus Spans; die C-ABI baut sie direkt aus Zeigern in den linearen Speicher. Im
Beispielspiel `hello` (`kern/examples/hello/hello.cpp`):

```cpp
    MeshInput b = box_mesh_.as_input();
    b.material.roughness = 0.45f;
    b.transforms = Span<const f32>(transforms_.data(), transforms_.size());
    b.tints = Span<const f32>(tints_.data(), tints_.size());
    return r.upsert(boxes_, b);
```

Ein Span lebt nicht länger als die Daten, auf die er zeigt. Funktionen, die Daten behalten müssen,
kopieren sie (der Renderer hält CPU-Kopien für Picking und die Wiederherstellung nach Kontextverlust,
[Kapitel 4](04-grafik.md); die Geometrie-Jobs der C-ABI kopieren ihren Eingabestrom, weil der Host
seinen Staging-Speicher wiederverwendet).

### Hashes

```cpp
/// FNV-1a, used for string keys and cache signatures.
constexpr u64 hash_bytes(const void* data, std::size_t n, u64 seed = 1469598103934665603ull);

/// 32-bit integer mix (lowbias32), for spatial hashing.
constexpr u32 hash_u32(u32 x);
```

- `hash_bytes` ist 64-Bit-FNV-1a mit wählbarem Startwert. Der Uniform-Cache des Renderers erkennt
  damit unveränderte Werte (`src/gfx/program.cpp`), die Schattenkarte ihren Schlüssel
  (`src/gfx/shadow.cpp`). Auch der Determinismus-Fingerabdruck ist ein FNV-1a über den Zustand der
  Ente ([Kapitel 2](02-bauen-und-testen.md)).
- `hash_u32` ist der Integer-Mischer *lowbias32*, etwa für deterministisches Rauschen in der
  Spiel-Laufzeit (`noise()` in `src/game/runtime_internal.h`).

Beide sind `constexpr` und rein ganzzahlig: Das Ergebnis hängt nur von den Bytes ab, die man
hineingibt.

---

## 3.2 `kern::json` – `json.h`

Kern braucht JSON überall: Levels (`.klevel`), das Protokoll zwischen Studio und Seite, Servoprofile,
Demonstrationen des Lehrers, das Policy-Manifest der Micro Duck. `kern::json` ist ein eigenes,
kleines Dokumentmodell mit drei Zusagen (Kopfkommentar von `json.h`):

1. **Objekte behalten ihre Schlüsselreihenfolge.** Ein Dokument, das dieser Formatierer oder
   JavaScripts `JSON.stringify` geschrieben hat, übersteht `parse` + `stringify` Byte für Byte. Das
   hält Level-Dateien diff-freundlich.
2. **Zahlen sind IEEE-Doubles und werden wie in JavaScript formatiert** (ECMAScript
   Number-to-String: kürzeste eindeutige Ziffernfolge, gleiche Exponentenregeln). Die Ausgabe ist
   also identisch mit `JSON.stringify`.
3. **Der Parser ist für nicht vertrauenswürdige Dateien gemacht:** Er lehnt nicht endliche
   Ergebnisse ab, Verschachtelung tiefer als `max_depth` und standardmäßig die Schlüssel
   `__proto__`, `prototype` und `constructor` (Prototype Pollution auf der JavaScript-Seite).

### `json::Value`

| Gruppe | Funktionen |
|---|---|
| Erzeugen | `Value()` (null), `Value(bool)`, `Value(f64)`, `Value(i32)`, `Value(u32)`, `Value(i64)`, `Value(const char*)`, `Value(std::string)`, `Value(std::string_view)`, `Value::array()`, `Value::object()` |
| Typ | `type()`, `is_null()`, `is_bool()`, `is_number()`, `is_string()`, `is_array()`, `is_object()` |
| Lesen | `as_bool(fallback)`, `as_number(fallback)`, `as_string()` (leerer String bei anderem Typ), `truthy()` (JavaScript-Wahrheitswert) |
| Arrays | `size()`, `operator[](index)`, `at(index)`, `push(v)`, `items()` |
| Objekte | `find(key)`, `has(key)`, `operator[](key)`, `set(key, v)`, `remove(key)`, `members()` |
| Vergleich | `operator==`, `operator!=` |

Wichtig für sicheren Code:

- `operator[]` auf einen fehlenden Schlüssel oder Index liefert einen **null-Wert** statt eines
  Fehlers. Ketten wie `v["a"][2]["b"]` sind deshalb immer sicher.
- `as_number`/`as_bool` geben bei falschem Typ den Ersatzwert zurück. Wer zwischen „fehlt“ und
  „falscher Typ“ unterscheiden will, prüft mit `find` und `is_number`.
- `set` ersetzt einen vorhandenen Schlüssel **an seiner Stelle** oder hängt ihn an.
- `push` macht aus einem Nicht-Array zuerst ein leeres Array.

### Parsen und Formatieren

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

- Fehlermeldungen tragen den Byte-Offset (`"… at offset 17"`); Text hinter dem Wert ergibt
  `trailing characters`. Ein UTF-8-BOM am Anfang wird toleriert.
- `out` wird nur bei Erfolg überschrieben.
- `stringify(v, indent)` entspricht `JSON.stringify(value, null, indent)`; `indent <= 0` schreibt
  kompakt. Nicht endliche Zahlen werden zu `null`, wie in JSON.
- Die Studio-C-ABI erlaubt für Befehle eine Tiefe von 120 (`kern_studio_command` in
  `src/studio/api.cpp`); sonst gilt 80.

Der Test `json_numbers_match_javascript` legt die Zahlenformatierung fest (Auszug):

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

### Beispiel: ein Servoprofil lesen und schreiben

`src/game/servo.cpp` zeigt das typische Muster – Felder einzeln suchen, Typ und Bereich prüfen,
erst am Ende übernehmen:

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

Und in die andere Richtung:

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

## 3.3 `kern::math` – Vektoren, Quaternionen, Matrizen

`math.h` ist header-only und allokationsfrei, damit der Compiler die heißen Pfade (Instanzmatrizen,
Culling, Picking) inlinen kann.

### Konventionen

| Konvention | Festlegung |
|---|---|
| Einheiten | Meter (und Sekunden, Kilogramm in Physik und Audio) |
| Händigkeit | rechtshändig |
| Oben | **+Z** |
| Matrizen | 4×4, **spaltenweise** gespeichert (OpenGL-Layout): Element (Zeile r, Spalte c) ist `m[c * 4 + r]`; die Translation steht in `m[12]`, `m[13]`, `m[14]` |
| Multiplikation | Spaltenvektoren: `p' = M * p`; `A * B` wendet erst `B`, dann `A` an |
| Quaternionen | `(x, y, z, w)`, Einheitsquaternion, Standard `w = 1` |
| Winkel | Radiant in `math.h`; Levels speichern Euler-Winkel in **Grad** ([Kapitel 8](08-spiel-laufzeit.md)) |
| Projektion | OpenGL-Clip-Raum (Tiefe −1 … 1), Kamera blickt in Sichtkoordinaten entlang −Z |

Z oben gilt überall: im Renderer, in der Physik (`physics.h`: „metres, seconds, kilograms, Z up,
quaternions (x, y, z, w)“), im Audio (`audio.h`) und in den Levels. Die Primitive in
`geo/primitives.h` sind ebenfalls Z oben und, wenn nicht anders angegeben, um den Ursprung zentriert
([Kapitel 5](05-szene-und-geometrie.md)).

### Vektoren

`Vec2`, `Vec3` und `Vec4` sind einfache Strukturen mit `f32`-Feldern und `constexpr`-Operatoren.

| Funktion | Bedeutung |
|---|---|
| `dot`, `cross` | Skalar- und Kreuzprodukt (für `Vec2` liefert `cross` die z-Komponente) |
| `length`, `length_sq`, `distance`, `distance_sq` | Längen und Abstände |
| `normalize(a, fallback = {0, 0, 1})` | Einheitsvektor; bei Länge ≤ 1e-9 der Ersatzwert statt `NaN` |
| `lerp`, `vmin`, `vmax` | Interpolation, komponentenweises Minimum/Maximum |
| `finite(v)` | alle Komponenten endlich |
| `hex_color(0xRRGGBB)` | Farbe 0…1 ohne Farbraumwandlung |
| `Vec3::load(p)`, `store(p)` | aus/in ein `f32`-Feld |
| `Vec3 * Vec3` | komponentenweise |
| `Vec4::xyz()` | die ersten drei Komponenten |

`normalize` mit Ersatzwert ist eine Robustheitsentscheidung: Ein Nullvektor aus Nutzereingaben
(etwa zwei gleiche Punkte) erzeugt keine `NaN`, die sich durch die Szene fortpflanzen.

### `Quat`

```cpp
/// Unit quaternion (x, y, z, w).
struct Quat {
  f32 x = 0, y = 0, z = 0, w = 1;
  static Quat axis_angle(Vec3 axis, f32 radians);  // Achse wird normalisiert
  Quat operator*(const Quat& b) const;             // Hamilton-Produkt: erst b, dann *this
  Vec3 rotate(Vec3 v) const;
};
```

`axis_angle` rechnet mit `kern::dm::sin` und `kern::dm::cos` ([3.4](#34-deterministische-mathematik-dmathh)),
damit Rotationen, die in die Simulation eingehen, überall dieselben Bits ergeben.

### `Mat4`

| Funktion | Bedeutung |
|---|---|
| `Mat4()` / `identity()` | Einheitsmatrix (Standardwert) |
| `load(p)` | 16 `f32` (spaltenweise) übernehmen |
| `translation()`, `is_identity()` | Translation lesen; exakter Vergleich mit der Einheit |
| `operator*` | Matrixprodukt |
| `transform_point(p)` | affin: rotiert, skaliert, verschiebt (ohne perspektivische Division) |
| `transform_vector(v)` | nur der 3×3-Anteil (Richtungen) |
| `transform(Vec4)` | volle 4×4-Multiplikation (z. B. in den Clip-Raum) |
| `translate(t)`, `scale(s)` | Grundmatrizen |
| `trs(t, q, s)` | Translation × Rotation × Skalierung in einem Schritt |
| `perspective(fov, aspect, near, far)` | Perspektive; `fov` ist der **vertikale** Öffnungswinkel in Radiant |
| `ortho(l, r, b, t, n, f)` | Parallelprojektion |
| `look_at(eye, target, up, &ok)` | Sichtmatrix; bei entarteter Basis Einheitsmatrix und `ok = false` |

Freie Funktionen:

- `normal_matrix3(m, out)` – Inverse-Transponierte des oberen 3×3-Blocks (spaltenweise in `out[9]`);
  `false` und Einheitsmatrix bei Singularität. Damit bleiben Normalen unter ungleichmäßiger
  Skalierung senkrecht.
- `invert(src, f64 out[16])` – allgemeine Inverse **in doppelter Genauigkeit**; `false`, wenn die
  Determinante nicht endlich oder betragsmäßig kleiner als 1e-14 ist. Picking invertiert damit die
  View-Projection, damit Strahlen auch bei großer Sichtweite genau bleiben.

Das Beispielspiel `hello` setzt Instanzmatrizen mit `trs` und kopiert sie in den Instanzstrom –
genau 16 `f32` je Instanz, spaltenweise:

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

Dass `trs` wirklich `T * R * S` ist, prüft der Test `math_matrices`:

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

Der Punkt (1, 0, 0) wird mit 2 skaliert, um 90° um Z gedreht (→ (0, 2, 0)) und um (1, 2, 3)
verschoben – (1, 4, 3).

### `Sphere` und `Aabb`

- `Sphere { center, radius = -1 }` – ein negativer Radius heißt „leer“ (`valid()`).
  Frustum-Culling behandelt ungültige Kugeln als sichtbar: Im Zweifel wird gezeichnet.
- `Aabb` startet umgekehrt (`min = +1e30`, `max = −1e30`), sodass das erste `extend` die Box setzt;
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

- Die Kamera ist eine **Beschreibung** (Augpunkt, Ziel, Oben-Vektor), keine Matrix. Renderer,
  Picking und Editor leiten die Matrizen daraus ab. `up` ist standardmäßig +Z.
- `fov` ist vertikal, in Radiant (Standard π/3 = 60°).
- Orthografische Kameras (Draufsicht, Vorder- und Seitenansicht im Studio) geben statt `fov` die
  sichtbare Höhe in Metern an.
- `camera_valid(c)` prüft die Parameter (`far > near`; perspektivisch `near > 0` und
  `0 < fov < π`; orthografisch `ortho_height > 0`, endlich, `near >= 0`).
- `projection_matrix(c, aspect)` wählt `perspective` oder `ortho`.

Die View-Projection-Matrix ergibt sich wie in `screen_ray` (siehe [3.1](#der-fehlerkanal-fail-und-last_error)):

```cpp
  const Mat4 view = Mat4::look_at(camera.eye, camera.target, camera.up, &ok);
  const Mat4 vp = projection_matrix(camera, width / height) * view;
```

Aus `vp` gewinnt `Frustum::from_matrix(vp)` (`scene/frustum.h`) die sechs Ebenen fürs Culling. Der
Test `math_camera` prüft, dass das Ziel in der Bildmitte landet:

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

Über die C-ABI setzt ein Host die Kamera mit zwölf `f32` (eye xyz, target xyz, up xyz, fov, near,
far); `kern_camera_set` weist ungültige Werte mit `fail("invalid camera")` zurück
([Kapitel 17](17-referenz.md)).

---

## 3.4 Deterministische Mathematik: `dmath.h`

### Das Problem

`sqrt`, `fabs` und `floor` sind in IEEE-754 exakt definiert; jede Plattform liefert dieselben Bits.
Für `sin`, `cos`, `exp`, `atan2` und die anderen transzendenten Funktionen gilt das **nicht**: glibc,
Apples libm, Androids bionic und wasi-libc runden im letzten Bit unterschiedlich. In einer
Robotersimulation wird aus einem Bit binnen Sekunden ein anderer Gang (Kopfkommentar von
`dmath.h`). Dann würde ein Netz, das im Browser gelernt hat, nativ anders laufen, und ein
Unterschied zum echten Roboter wäre nicht von Rechenrauschen zu trennen.

### Die Lösung: musl, überall gleich übersetzt

Kern bringt die Implementierungen aus **musl** mit (MIT, `kern/third_party/musl_math`). Sie werden
auf jeder Plattform gleich übersetzt: ohne FMA-Zusammenfassung und ohne plattformspezifische Pfade.
Die Datei `third_party/musl_math/src/kmath.h` ersetzt, was die musl-Quellen sonst aus `<math.h>`
bekämen:

- Die Funktionen werden per `#define` in `kern_musl_*` umbenannt (`#define sin kern_musl_sin` …),
  damit sie nie mit der libm der Plattform kollidieren.
- `__FP_FAST_FMA` ist überall 0; `float_t`/`double_t` sind `float`/`double`
  (`FLT_EVAL_METHOD 0` auf SSE2, ARM64 und wasm).
- Exakte Operationen (`fabs`, `sqrt`, `floor`, `scalbn`) laufen über Compiler-Builtins.

Alle drei Build-Wege übersetzen musl mit `-ffp-contract=off -fno-math-errno`
(`kern/build.sh`, `kern/tests/run.sh`, `kern/tools/lehrer.sh`), und auch aller übrige Code wird mit
`-ffp-contract=off` übersetzt. Box3D läuft im Skalar-Pfad (`BOX3D_DISABLE_SIMD`) und hat eigene
deterministische Winkelfunktionen (z. B. `b3Atan2` in `third_party/box3d/src/math_functions.c`).

### Die API: `kern::dm::`

`include/kern/core/dmath.h` stellt die Funktionen im Namensraum `kern::dm` bereit, mit denselben
Überladungsregeln wie `std::`:

| Funktionen | Überladungen |
|---|---|
| `sin`, `cos`, `tan`, `asin`, `acos`, `atan`, `exp`, `expm1`, `log`, `log10` | `float` → `kern_musl_<f>f`, `double` → `kern_musl_<f>`, Ganzzahl → `double` |
| `atan2`, `pow`, `hypot` | beide `float` → `float`-Variante, sonst `double` |

```cpp
#define KERN_DM_UNARY(name)                                                         \
  inline float name(float x) { return kern_musl_##name##f(x); }                     \
  inline double name(double x) { return kern_musl_##name(x); }                      \
  template <class T, class = std::enable_if_t<std::is_integral_v<T>>>               \
  inline double name(T x) { return kern_musl_##name(static_cast<double>(x)); }
```

Benutzt wird es wie `std::`. Aus `math.h`:

```cpp
  static Quat axis_angle(Vec3 axis, f32 radians) {
    const Vec3 a = normalize(axis);
    const f32 s = kern::dm::sin(radians * 0.5f);
    return {a.x * s, a.y * s, a.z * s, kern::dm::cos(radians * 0.5f)};
  }
```

und in `Mat4::perspective`:

```cpp
    const f32 f = 1.0f / kern::dm::tan(fov * 0.5f), nf = 1.0f / (near - far);
```

`sqrt`, `fabs` und `floor` bleiben `std::`, weil sie exakt sind.

### Die Regel für neuen Code

Aus `docs/lehrer.md`, Abschnitt 7:

- Alles, was die **Simulation speist**, nutzt `kern::dm::sin` usw. statt `std::sin`.
- **Keine Gleitkomma-Reihenfolge, die von der Zahl der Threads abhängt** – Summen in fester
  Reihenfolge.
- **Keine `-ffast-math`-Optionen.**

Reine Darstellung darf die Plattform-libm benutzen. Im aktuellen Stand rufen etwa der
Synthesizer und Mixer (`src/audio/`), die Primitive (`src/geo/primitives.cpp`), die Partikel und
Teile des Editors (Gizmos, Overlays, Viewport) `std::sin`/`std::cos` auf; das Beispielspiel
`hello` positioniert seine Kamera mit `std::cos`/`std::sin`. Sobald solcher Code Werte erzeugt, die in
die Physik, einen Controller oder das Training eingehen, muss er auf `kern::dm::` umgestellt werden.

### Wie man prüft

Der Determinismus-Fingerabdruck (`kern_lehrer --fingerprint 400`, Test
`determinism_is_bit_identical_across_builds_and_platforms`, `kern/tests/lehrer_wasm_test.mjs`,
CI-Job `determinism` auf ARM) zeigt jede Abweichung bis auf den Regeltakt genau
([Kapitel 2](02-bauen-und-testen.md)). Laut `docs/lehrer.md` kostet die eigene libm keine
Geschwindigkeit: musls Funktionen sind so schnell wie die der Plattform, und die Physik wird von
Box3Ds Gelenk-Löser bestimmt.

---

## 3.5 Zusammenfassung

- Kern meldet Fehler über `bool` + `fail()` / `last_error()`, nicht über Ausnahmen.
- `Span` ist das Eingabeformat der Engine; wer Daten behält, kopiert sie.
- JSON in Kern verhält sich wie JavaScripts `JSON`: Reihenfolge, Zahlenformat, und es ist gegen
  bösartige Dateien gehärtet.
- Mathematik: Meter, rechtshändig, **Z oben**, Matrizen spaltenweise, Quaternionen `(x, y, z, w)`,
  `fov` vertikal in Radiant.
- Simulation braucht `kern::dm::` statt `std::` für transzendente Funktionen, `-ffp-contract=off`
  und feste Summationsreihenfolgen – sonst ist sie nicht mehr Bit für Bit gleich.
