# Kapitel 4 – Grafik

Dieses Kapitel beschreibt den Renderer von Kern vollständig: wie der C++-Code im WebAssembly-Modul WebGL2 erreicht, wie Meshes und Materialien aufgebaut sind, was der eine Standard-Shader rechnet, in welcher Reihenfolge ein Frame entsteht und wie die Zusatzpasses (Sonnenschatten, Himmel, GTAO, Bloom) im Detail arbeiten. Am Ende steht ein vollständiges Beispiel einer eigenen Shader-Erweiterung aus dem Spiel Orb Run.

Allgemeine Grundlagen – Rasterisierung, Koordinatenräume und Matrizen, die Shader-Pipeline, der Tiefenpuffer, die Grundidee von PBR, Gamma und Tonemapping – setzt das Kapitel voraus. Sie stehen in [Anhang B](B-grundlagen-rendering.md). Hier geht es darum, was Kern konkret daraus macht.

![Kern Studio mit Orb Run: prozedurale Stadt, Sonnenschatten, Himmel und Bloom](bilder/studio.png)

## 4.1 Überblick

Der Renderer besteht aus wenigen Teilen:

| Teil | Datei | Aufgabe |
|---|---|---|
| WebGL2-Importe | `include/kern/gfx/gl.h`, `web/kern.mjs` | genau die GL-Funktionen, die die Engine braucht, als WebAssembly-Importe |
| Programm und Uniform-Filter | `gfx/program.h`, `src/gfx/program.cpp` | Shader kompilieren und linken, redundante `uniform*`-Aufrufe vermeiden |
| Standard-Shader | `gfx/shader.h`, `src/gfx/standard_shader.cpp` | ein Programm für alle Meshes, erweiterbar über `ShaderExtension` |
| Mesh und Material | `gfx/mesh.h`, `src/gfx/mesh.cpp` | CPU-Kopie der Geometrie, GPU-Puffer, Material |
| Renderer | `gfx/renderer.h`, `src/gfx/renderer.cpp` | Meshes verwalten, Frame zeichnen, Picking, Kontextverlust |
| Passes | `gfx/passes.h`, `src/gfx/shadow.cpp`, `sky.cpp`, `bloom.cpp`, `gtao.cpp` | Sonnenschatten, Himmel, Bloom/Composite, Ambient Occlusion |
| Partikel und Lichter | `gfx/particles.h`, `src/gfx/particles.cpp` | Billboard-Partikel als ein instanziertes Mesh, Auswahl der Punktlichter |

Drei Grundsätze prägen den Aufbau:

1. **Ein Programm für alles.** Jedes Mesh der Szene wird mit demselben Standard-Shader gezeichnet. Innerhalb des Szenenpasses gibt es keine Programmwechsel. Spiele ändern das Aussehen über Shader-Erweiterungen, die in dieses eine Programm eingefügt werden, nicht über eigene Programme.
2. **Hochladen einmal, danach nur Bewegung.** Ein Mesh wird mit `upsert` einmal hochgeladen. Danach ändern sich normalerweise nur seine Instanzströme (`update_instances`). Die CPU-Kosten pro Frame folgen dem, was sich bewegt, nicht der Szenengröße.
3. **Die CPU-Kopie ist die Wahrheit.** Der Renderer behält Positionen, Normalen, Farben, UVs, Indizes und Instanzen jedes Meshes im Speicher. Daraus entstehen Bounds für das Culling, die BVH für das Picking und nach einem Kontextverlust die komplette Neuauflage auf der GPU.

## 4.2 Die WebGL2-Anbindung

### Importe statt Emscripten

Kern wird mit wasi-sdk nach WebAssembly übersetzt, ohne Emscripten und ohne generierten JavaScript-Kleber. Die GL-Funktionen sind deshalb gewöhnliche WebAssembly-Importe aus dem Modul `kern_gl`. `gl.h` deklariert sie mit einem Makro:

```cpp
#if defined(__wasm__)
#define KERN_GL(name) __attribute__((import_module("kern_gl"), import_name(#name)))
#else
#define KERN_GL(name)
#endif

KERN_GL(drawElementsInstanced) void drawElementsInstanced(u32 mode, i32 count, u32 type, i32 offset, i32 instances);
```

Außerhalb von WebAssembly (native Tests) bleibt das Makro leer, und die Tests linken eine aufzeichnende GL-Attrappe (`tests/gl_stub.cpp`).

Die Liste in `gl.h` ist laut Kommentar „genau das, was die Engine benutzt“. Wer einen neuen GL-Aufruf braucht, ergänzt ihn an zwei Stellen: in `gl.h` und im `kern_gl`-Importobjekt von `web/kern.mjs`. Auch die Enum-Werte (`gl::TRIANGLES`, `gl::RGBA16F`, `gl::TEXTURE_COMPARE_MODE` …) stehen als WebGL2-Zahlen in `gl.h`.

### Handles, Zeiger und Längen

Über die Grenze zwischen WebAssembly und JavaScript gehen nur Zahlen. Daraus folgen zwei Regeln:

- **GPU-Objekte sind `u32`-Handles**, `0` bedeutet null. `KernModule` in `kern.mjs` hält dafür eine Tabelle `objects` mit Freiliste: `createBuffer()` legt das echte `WebGLBuffer` in einen Slot und gibt die Slotnummer zurück, `deleteBuffer()` gibt den Slot frei. Uniform-Locations bekommen eine eigene Tabelle `locations`; `getUniformLocation` gibt `0` zurück, wenn das Uniform nicht aktiv ist.
- **Daten sind Zeiger in den linearen Speicher plus Bytezahl.** `bufferData(target, ptr, bytes, usage)` ruft auf der JavaScript-Seite `gl.bufferData(target, u8, usage, ptr, bytes)` mit einer `Uint8Array`-Sicht auf den Modulspeicher auf, ohne die Daten zu kopieren. `ptr == 0` reserviert nur Speicher. Bei `texImage2D` wählt die Plattformschicht die passende Sicht nach dem GL-Typ (`f32` für `FLOAT`, `u16` für `HALF_FLOAT`/`UNSIGNED_SHORT`, `u32` für `UNSIGNED_INT`, sonst `u8`).

Die Sichten auf den Speicher (`u8`, `u32`, `f32` …) erneuert `refresh()`, sobald `memory.buffer` sich durch Speicherwachstum ändert. Strings (Shader-Quelltext, Uniform-Namen) kommen als Zeiger und Länge und werden mit `TextDecoder` gelesen.

Zwei Kleinigkeiten zeigen, wie dünn die Schicht bewusst ist: `drawBuffers` cacht die JavaScript-Arrays pro Wertekombination, damit der Aufruf pro Frame keine Allokation erzeugt. `getExtension("EXT_disjoint_timer_query_webgl2")` merkt sich das Erweiterungsobjekt, damit `gpuDisjoint()` später `GPU_DISJOINT_EXT` abfragen kann.

### Was `kern.mjs` sonst noch tut

`web/kern.mjs` erfüllt laut Kopfkommentar genau drei Aufgaben und enthält keine Engine-Logik:

1. das Modul mit den Importen `kern_gl`, `kern_env` und einem minimalen WASI-Shim instanziieren,
2. die Handle-Tabellen und die Kontextverlust-Mechanik besitzen,
3. `KernRenderer` anbieten, eine Fassade, die typisierte Arrays in die C-ABI (`include/kern/api/c_api.h`) übersetzt.

`kern_env` liefert unter anderem `now` (`performance.now()`), `terrain_height` (ruft `kern.heightSampler(x, y)` auf, falls die Seite einen gesetzt hat), `status`, `pointer_lock`, `log` sowie den JIT-Weg der Logik-Graphen (siehe [Kapitel 9](09-logik.md)). Der WASI-Shim kennt `fd_write` (Ausgabe auf die Konsole), `random_get`, `clock_time_get`, `proc_exit`, `fd_close` und `fd_seek`; alles andere antwortet mit `ENOSYS`. Die Engine fasst keine Dateien an.

Den WebGL2-Kontext erzeugt die Seite, nicht die Engine. `KernRenderer` und `web/app.mjs` fordern ihn mit denselben Attributen an:

```js
canvas.getContext("webgl2", { alpha: false, antialias: false, depth: true,
                              powerPreference: "high-performance", ... });
```

`antialias: false` ist gewollt: Der Renderer zeichnet die Szene ohnehin in eigene Framebuffer (siehe 4.7), in denen Multisampling des Standard-Framebuffers nichts bewirken würde. Kern Studio und exportierte Spiele erzeugen ihren Kontext selbst und fordern dabei `antialias: true` an (`web/studio/studio.mjs`, `web/studio/game.mjs`).

### Zwei Arten, den Renderer zu benutzen

- **C++-Spiele** schreiben eine `kern::App` (`include/kern/app/app.h`). `web/app.mjs` (`runKernApp`) erzeugt den Kontext, leitet Eingaben weiter und ruft jeden Frame `kern_app_frame(now, hidden)` auf. Darin läuft zuerst `App::update()`, danach `Renderer::render()`. Das Beispiel `examples/hello` und Orb Run arbeiten so; Details in [Kapitel 12](12-eigene-spiele.md).
- **JavaScript-Hosts** (etwa SpawnCity) benutzen `KernRenderer` aus `kern.mjs`. Die Fassade schreibt Kamera, Licht und Mesh-Deskriptoren in feste Wortlayouts (`KERN_MD_*`, `KERN_LI_*`, `KERN_RP_*` aus `c_api.h`) und ruft die exportierten C-Funktionen auf. Pro Modul gibt es genau einen `KernRenderer` („one KernRenderer per module“).

Kern Studio und exportierte Spiele nutzen eine dritte, eigene C-ABI (`kern_studio_*`, siehe [Kapitel 11](11-studio.md)); intern ist es derselbe `Renderer`.

## 4.3 Programme und der Uniform-Cache

### `Program`

`Program::build(vertex, fragment, label)` kompiliert beide Stufen, linkt und löscht die Shader-Objekte gleich danach. Schlägt etwas fehl, landet das Treiber-Log (bis 767 Zeichen) über `fail()` als letzter Fehler im Modul, mit dem `label` davor, etwa `"standard fragment shader: ..."`. `location(name)` liefert `0` für ein Uniform, das im gelinkten Programm nicht aktiv ist – und alle Setter ignorieren Location `0`. Ein Uniform, das der Compiler wegoptimiert hat, kostet deshalb nichts und erzeugt keinen Fehler.

`destroy(contextLost)` unterscheidet zwei Fälle: Bei lebendem Kontext wird `deleteProgram` gerufen; nach einem Kontextverlust sind die Handles bereits tot und werden nur vergessen.

### `UniformCache`

In GL gehören Uniform-Werte zum Programmzustand. Ein einmal gesetzter Wert bleibt also stehen, auch über Frames und Programmwechsel hinweg. `UniformCache` nutzt das: Er merkt sich pro Location den zuletzt gesendeten Wert und überspringt den `gl.uniform*`-Aufruf, wenn sich nichts ändert.

```cpp
void UniformCache::set_float(u32 location, f32 v) {
  if (!location) return;
  Entry& e = entry(location);
  if (e.kind == 2 && e.count == 1 && e.f[0] == v) return;
  e.kind = 2; e.count = 1; e.f[0] = v;
  gl::uniform1f(location, v);
}
```

Skalare und Vektoren vergleicht er direkt. Arrays und Matrizen (`set_vec3_array`, `set_vec4_array`, `set_mat4`) vergleicht er über einen 64-Bit-Hash des Inhalts (`hash_bytes`), damit nicht jeder Eintrag das ganze Array speichern muss. Knochenpaletten gehen über `set_mat4_array` absichtlich ungecacht: Sie ändern sich bei animierten Figuren ohnehin jeden Frame.

Warum lohnt sich das? Jeder `uniform*`-Aufruf ist im Browser ein Übergang von WebAssembly nach JavaScript und dann in die WebGL-Implementierung, die den Aufruf validiert. Bei hunderten Meshes mit oft identischem Material spart der Cache den größten Teil dieser Aufrufe. `reset()` ist nur nötig, wenn das Programm neu gelinkt wurde.

## 4.4 Mesh, Material und MeshInput

### Feste Vertex-Attribute

Der Standard-Shader liest seine Vertex-Ströme an festen Locations (`namespace attr` in `mesh.h`):

| Location | Name im Shader | Inhalt | Pro |
|---|---|---|---|
| 0 | `aPosition` | Position xyz | Vertex |
| 1 | `aNormal` | Normale xyz | Vertex |
| 2 | `aColor` | Farbe rgb | Vertex |
| 3 | `aUV` | Texturkoordinate uv | Vertex |
| 4–7 | `iModel0..3` | Spalten der Modellmatrix | Instanz |
| 8 | `iTint` | Tint rgba | Instanz |
| 9–11 | `iNormal0..2` | Spalten der Normalenmatrix (3×3) | Instanz |
| 12 | `aJoints` | 4 Knochenindizes | Vertex |
| 13 | `aWeights` | 4 Gewichte | Vertex |
| 14, 15 | frei | Ströme von Shader-Erweiterungen | Vertex |

Die Instanz-Attribute haben `vertexAttribDivisor = 1`. Jedes Mesh wird deshalb mit genau einem `drawElementsInstanced` oder `drawArraysInstanced` gezeichnet, auch wenn es nur eine Instanz hat. Die Normalenmatrix (Inverse-Transponierte des 3×3-Teils, siehe [Anhang B](B-grundlagen-rendering.md)) rechnet der Renderer pro Instanz auf der CPU aus (`normal_matrix3`). Eine singuläre Matrix wird abgelehnt (`"singular world transform"`).

Weitere Grenzen aus `mesh.h`: höchstens 32 Knochen (`kMaxBones`), 6 Punktlichter (`kMaxPointLights`), 2 Erweiterungs-Ströme (`kMaxExtensionAttributes`), 8 ganzzahlige Materialparameter für Erweiterungen (`kMaxMaterialParams`).

### Material

```cpp
struct Material {
  Vec4 color{1, 1, 1, 1};
  AlphaMode alpha = AlphaMode::opaque;   // opaque, mask, blend
  f32 alpha_cutoff = 0.5f;
  f32 roughness = 1.0f;
  f32 metalness = 0.0f;
  Vec3 emissive{0, 0, 0};      // lineares HDR, nach der Beleuchtung addiert (speist Bloom)
  bool unlit = false;
  bool double_sided = false;
  bool cast_shadow = true;
  bool frustum_culled = true;
  bool viewmodel = false;      // Ego-Ebene: zuletzt, nach einem Depth-Clear
  bool linear_colors = false;  // Vertexfarben sind linear (kein sRGB-Decode)
  bool lines = false;          // Linienliste, immer unlit, kein Schatten, nicht pickbar
  i32 params[kMaxMaterialParams] = {};  // gehören Shader-Erweiterungen
  bool casts_shadow() const { return cast_shadow && alpha == AlphaMode::opaque && !lines; }
};
```

Einige Felder brauchen eine Erklärung:

- **Farben sind sRGB.** Der Shader multipliziert Vertexfarbe, Instanz-Tint, `color.rgb` und Texel und wandelt das Produkt dann von sRGB nach linear um (`srgbToLinear`). Nur wenn `linear_colors` gesetzt ist, entfällt die Umwandlung. `emissive` dagegen ist bereits linear.
- **Nur opake Materialien werfen Schatten** (`casts_shadow()`). Alpha-Mask- und Blend-Materialien landen nie in der Schattenkarte.
- **`viewmodel`** gehört zur Ego-Ebene: Diese Meshes werden zuletzt gezeichnet, nach einem Löschen des Tiefenpuffers. Eine Wand vor der Kamera schneidet dann keine Waffe ab. Kern Studio zeichnet seine Gizmos ebenfalls auf dieser Ebene.
- **`lines`** macht aus dem Mesh eine Linienliste (`gl::LINES`, Paare von Vertices). Der Renderer setzt dann `unlit = true` und `cast_shadow = false` selbst; Linien-Meshes lassen sich nicht skinnen und werden beim Picking übersprungen.

Die Face-Culling-Regel steht in `Mesh::cull_mode()`: aus bei `double_sided`, `lines` oder gemischt gespiegelten Instanzen; Rückseiten-Culling im Normalfall; Vorderseiten-Culling, wenn alle Instanzen gespiegelt sind (negative Determinante). Eine Spiegelung kehrt den Umlaufsinn um; ein gemischtes Mesh lässt sich in einem Draw nicht korrekt cullen.

### MeshInput und `upsert`

`MeshInput` ist eine geliehene Sicht auf Daten des Aufrufers. Die Spans müssen nur während des Aufrufs gültig sein; der Renderer kopiert alles.

| Feld | Pflicht | Bedeutung |
|---|---|---|
| `positions` | ja | xyz pro Vertex |
| `normals` | nein | ohne Angabe: flache Flächennormalen bei Triangle-Soup, sonst +Z |
| `colors` | nein | rgb pro Vertex, Standard weiß |
| `uvs` | nein | uv pro Vertex, Standard 0 |
| `indices` | nein | Dreiecksliste (bzw. Linienpaare) |
| `joints`, `weights`, `bones` | nein | Skinning: 4 pro Vertex, Palette mit 16 Floats pro Knochen |
| `extension[2]` | nein | Ströme der Shader-Erweiterungen in Deklarationsreihenfolge |
| `transforms` | nein | Instanzmatrizen, 16 Floats; Standard eine Einheitsmatrix |
| `tints` | nein | rgba pro Instanz, Standard weiß |
| `material`, `texture`, `visible`, `layers` | – | Material, Textur-Handle, Sichtbarkeit, Ebenenmaske |

`upsert(id, input)` legt Mesh `id` an oder ersetzt es **atomar**. Zuerst wird alles geprüft, bevor sich etwas ändert:

- Positionen sind Tripel, alle Werte endlich, ohne Indizes ist die Vertexzahl durch 3 (bzw. 2 bei Linien) teilbar.
- Jeder Index liegt innerhalb der Vertexzahl.
- `normals`/`colors` haben so viele Floats wie `positions`, `uvs` zwei pro Vertex, Erweiterungs-Ströme `size` pro Vertex.
- Beim Skinning: genau 4 Joints und 4 Gewichte pro Vertex, höchstens 32 Knochen, ganzzahlige Joint-Indizes innerhalb der Palette, Gewichte in [0, 1] mit Summe 1 (Toleranz 0,001).
- Instanzmatrizen endlich und nicht singulär.

Erst dann baut der Renderer das neue Mesh vollständig auf (Bounds, eventuell BVH, GPU-Puffer) und tauscht es gegen das alte. Scheitert die Prüfung, bleibt das bisherige Mesh unverändert sichtbar. Das ist wichtig für Editoren: Ein fehlerhafter Zwischenstand löscht nichts.

Mesh-Ids kommen aus `allocate_id()`; `upsert` akzeptiert nur so vergebene Ids. Zwei Besitzer können sich deshalb nie einen Slot teilen. Schlägt das erste `upsert` fehl, gibt `release_id()` die Id zurück. `remove()` gibt sie frei und recycelt sie.

### Was beim Hochladen passiert

`Renderer::upload()` legt pro Mesh ein Vertex Array Object an und füllt:

- statische Puffer (`STATIC_DRAW`) für Positionen, Normalen, Farben, UVs, gegebenenfalls Joints, Gewichte und Erweiterungs-Ströme,
- drei dynamische Instanzpuffer (`DYNAMIC_DRAW`) für Modellmatrizen, Tints und Normalenmatrizen,
- einen Indexpuffer. **Hat das Mesh höchstens 65 536 Vertices, werden die Indizes als 16 Bit hochgeladen**, sonst als 32 Bit. Das halbiert die Index-Bandbreite im häufigen Fall.

Bei statischen Weltmeshes baut der Renderer zusätzlich eine BVH für das Picking: wenn das Mesh keine Linien enthält, genau eine Instanz mit Einheitsmatrix hat, nicht geskinnt ist und mindestens 36 Dreiecke hat (`kBvhMinTriangles`). Mehr dazu in [Kapitel 5](05-szene-und-geometrie.md).

### Instanzen aktualisieren

`update_instances(id, transforms, tints, &changed)` ist der Weg für alles, was sich bewegt:

- Leere `transforms` bedeuten „Instanzen behalten“: ein reines Tint-Update. Dann wird nur der Tint-Puffer hochgeladen; Bounds, BVH und Schattenkarte bleiben gültig.
- Haben `tints` nicht genau 4 Floats pro Instanz, gelten alle Instanzen als weiß.
- Sind Transforms und Tints bytegleich mit dem Stand im Renderer (`memcmp`), passiert gar nichts, und `changed` bleibt `false`.
- Wächst die Instanzzahl über die Pufferkapazität, wird diese **geometrisch** vergrößert (mindestens verdoppelt). Schwankt die Zahl der Instanzen, entstehen so keine wiederholten Neuallokationen. Danach lädt `bufferSubData` nur die belegten Bytes hoch.
- Die Scratch-Vektoren tauschen mit denen des Meshes (`swap`). Im eingeschwungenen Zustand allokiert ein Update nicht.

Weitere Setter: `update_bones` (gleiche Knochenzahl wie beim Anlegen), `set_visible`, `set_material` (der Primitivtyp Linie/Dreieck darf sich nicht ändern), `set_layers`. Jede Änderung, die einen Schattenwerfer betrifft, erhöht die Schatten-Revision; dazu mehr in 4.8.

### Ebenen (Layers)

Jedes Mesh hat eine Bitmaske `layers` (Standard 1). `RenderParams::layers` bestimmt, welche Bits gezeichnet werden – und welche Meshes Schatten werfen. `PickQuery::layers` filtert genauso beim Picking. Kern Studio nutzt das für Raster, Icons, Gizmos pro Viewport, Collider-Darstellung und die Welten mehrerer Netzwerk-Clients (`layer` in `include/kern/studio/studio.h`, gesetzt in `src/studio/viewport.cpp`).

### Texturen

- `create_texture(TextureDesc)` erzeugt eine engine-eigene RGBA8-Textur. Der Renderer behält die Pixel als CPU-Kopie und lädt sie nach einem Kontextverlust neu hoch. Optionen: `mipmaps` (Standard an, dann `LINEAR_MIPMAP_LINEAR`), `repeat` (sonst `CLAMP_TO_EDGE`), `nearest` (Pixel-Look, ohne Mipmaps).
- `register_external_texture(gl_handle)` meldet eine Textur an, deren GPU-Objekt der Host besitzt. So arbeitet `KernRenderer.createTexture(source)`: Es lädt ein Bild, Canvas oder `ImageData` direkt über WebGL hoch und hinterlegt eine Wiederherstellungsfunktion, die nach einem Kontextverlust dieselbe Handle-Nummer neu füllt (`kern.pinned`).

Ein Mesh hat höchstens eine Albedo-Textur (`MeshInput::texture`). Sie wird auf Einheit 0 gebunden; die Schattenkarte liegt auf Einheit 1.

## 4.5 Der Standard-Shader

`build_standard_shader(extensions)` setzt Vertex- und Fragment-Shader aus festen GLSL-Bausteinen zusammen und fügt an definierten Stellen die Schnipsel der Erweiterungen ein. Es entsteht genau ein Programm, das der Renderer bei `init()` linkt.

### Aufbau des Vertex-Shaders

1. `kVertexHead`: `#version 300 es`, die festen Attribute 0–13.
2. Pro Erweiterungsattribut eine Zeile `layout(location=N) in vecK aName;`.
3. `kVertexUniforms`: `uSkinEnabled`, `uBones[32]`, `uViewProjection`, die Ausgänge `vWorld`, `vNormal`, `vColor`, `vUV`, `vOpacity`.
4. Hook `vertex_declarations`.
5. `kVertexMainHead`: Modellmatrix aus den Instanzspalten, Skinning, Weltposition, Normale.
6. Hook `vertex`.
7. `gl_Position = uViewProjection * p;`

Das Skinning ist klassisches Linear Blend Skinning:

```glsl
if (uSkinEnabled == 1) {
  mat4 skin = aWeights.x * uBones[int(aJoints.x)] + aWeights.y * uBones[int(aJoints.y)]
            + aWeights.z * uBones[int(aJoints.z)] + aWeights.w * uBones[int(aJoints.w)];
  local = skin * local;
  localNormal = mat3(skin) * localNormal;
}
vec4 p = model * local;
vWorld = p.xyz;
vNormal = normalize(mat3(iNormal0, iNormal1, iNormal2) * localNormal);
vColor = aColor * iTint.rgb;
vOpacity = iTint.a;
```

### Aufbau des Fragment-Shaders

1. `kFragmentHead`: Eingänge, alle Uniforms und **drei Ausgänge**:
   - `location 0` `outColor`: das fertige LDR-Bild (tonemapped, sRGB, mit Nebel),
   - `location 1` `outHdr`: die lineare HDR-Farbe vor dem Tonemapping (für Bloom),
   - `location 2` `outAo`: ein Datensatz für die Ambient Occlusion (siehe 4.8.4).
2. `kGlslToneMap`: `linearToSrgb`, `srgbToLinear`, ACES-Filmic, Farbgrading.
3. `kFragmentFunctions`: `envIrradiance`, `skyReflection`.
4. Hook `fragment_declarations`.
5. Textur lesen, Alpha berechnen, Alpha-Test.
6. Hook `fragment_discard`.
7. `bool linearInput = uLinearColor == 1;` und Hook `fragment_input`.
8. `kFragmentBase`: Grundfarbe linearisieren, Normale, Sonnenrichtung, `surfaceGlass = 0`, `emissive = uEmissive`.
9. Hook `fragment_surface`.
10. `kFragmentLighting`: Beleuchtung, Tonemapping, Nebel, Ausgänge.

Alpha entsteht als `clamp(vOpacity * uMaterial.a * texel.a, 0, 1)`. Für `AlphaMode::mask` setzt der Renderer `uAlphaCutoff` auf `alpha_cutoff`, sonst auf 0; darunter wird verworfen.

### Das Licht- und Materialmodell

Das Modell lehnt sich an `MeshStandardMaterial` von three.js an; der Code verweist an mehreren Stellen ausdrücklich auf three.js (ACES-Anpassung r16x, FogExp2 „as three.js applies it“, „unchanged look“ bei abgeschalteter Atmosphäre). Alle Größen sind linear; `base` ist die linearisierte Grundfarbe, `n` die Normale, `sun` die normierte Sonnenrichtung (zur Sonne hin), `rough = clamp(roughness, 0.04, 1)`, `metal = clamp(metalness, 0, 1)`.

**Direktes Sonnenlicht (Lambert)**

```
ndl    = max(dot(n, sun), 0)
shadow = PCF-Ergebnis der Schattenkarte (1 = beleuchtet)
```

**Hemisphärenlicht.** Mischung aus Boden- und Himmelsfarbe nach der Neigung der Normale (Z ist oben):

```
hemis = mix(uGroundColor, uSkyColor, clamp(n.z * 0.5 + 0.5, 0, 1))
```

**Himmels-Irradianz.** Eine Näherung an die vorgefilterte Umgebung eines Kuppel-Gradienten über einer Bodenscheibe:

```glsl
vec3 envIrradiance(vec3 n) {
  vec3 sky = mix(uEnvHorizon, uEnvZenith, 0.35);
  float w = clamp(n.z * 0.5 + 0.5, 0.0, 1.0);
  return mix(uEnvGround, sky, smoothstep(0.0, 1.0, w));
}
```

**Punktlichter.** Bis zu 6, mit glattem Abfall auf null am Radius:

```
d2          = max(|L - P|², 0.04)
radius      = max(0.1, range)
attenuation = (max(0, 1 - d2/radius²))² / (1 + d2/radius²)
point      += color * (intensity * 0.12) * attenuation * max(dot(n, normalize(L - P)), 0)
```

**Zusammensetzung des diffusen Anteils.** Indirektes Licht wird getrennt geführt, weil die Ambient Occlusion später nur diesen Anteil abdunkeln darf; Sonne und Punktlichter haben ihre eigene Verschattung:

```
indirect = base * (1 - metal) * (hemis / π + envIrradiance(n) * uEnvIntensity)
rgb      = base * (1 - metal) * (sunColor * ndl * shadow + point * π) / π + indirect
```

Bei `unlit` ist `rgb = base` und `indirect = 0`.

**Spiegelnder Anteil.** Nur wenn `metal > 0` oder `rough < 0.95`:

```
V   = normalize(eye - P),  R = reflect(-V, n),  NdV = clamp(dot(n, V), 0, 1)
F0  = mix(0.04, base, metal)
F   = F0 + (max(1 - rough, F0) - F0) * (1 - NdV)^5          // Schlick mit Rauheit
envR = mix(envIrradiance(R), skyReflection(R, 1), 1 - rough)
rgb      += F * envR * uEnvIntensity
indirect += F * envR * uEnvIntensity

H   = normalize(sun + V),  a2 = rough⁴,  NdH = clamp(dot(n, H), 0, 1)
D   = a2 / (π * (NdH² * (a2 - 1) + 1)²)                    // GGX-Verteilung
rgb += F * sunColor * ndl * shadow * min(D, 40) * 0.25
```

`skyReflection(r, s)` ist schlicht `mix(uEnvHorizon, uEnvZenith, clamp(r.z, 0, 1) * s)`. Die Begrenzung `min(D, 40)` verhindert extrem helle Glanzpunkte bei sehr glatten Oberflächen, die sonst als einzelne flackernde Pixel im Bloom auftauchen würden.

**Glas.** Eine Erweiterung kann `surfaceGlass` (0..1) setzen. Dann spiegelt die Fläche den Himmel mit einem Fresnel-Term, damit Scheiben hell wirken statt schwarz:

```
fr      = (1 - |dot(V, n)|)³
skyRefl = skyReflection(reflect(-V, n), 0.6) * uEnvIntensity
g       = surfaceGlass * (0.45 + 0.4 * fr)
add     = surfaceGlass * mix((0.05, 0.07, 0.09), (0.32, 0.4, 0.5), fr)
rgb     = mix(rgb, skyRefl, g) + add      // ebenso für indirect
```

**Emission.** `rgb += emissive` – nach der Beleuchtung, linear und ohne Obergrenze. Werte über 1 werden später zu Bloom.

### Vom HDR-Wert zum Pixel

```glsl
outHdr = vec4(max(rgb, vec3(0.0)), alpha);
rgb = acesFilmic(rgb);                       // Belichtung: c *= uExposure / 0.6
if (uGrade == 1) rgb = gradeColor(rgb);
rgb = linearToSrgb(clamp(rgb, 0.0, 1.0));
```

`acesFilmic` ist die Anpassung von three.js (r16x) an die ACES-RRT/ODT-Kurve, einschließlich des Faktors `uExposure / 0.6`. `gradeColor` ist Kerns Produktions-Grading auf dem bereits tonemappten Bild: Sättigung ×1,14, eine weiche S-Kurve (22 %), wärmere Lichter und kühlere Schatten. Im Szenenshader schaltet `RendererOptions::grade = false` es ab; im Bloom-Composite entscheidet `BloomOptions::grade` (die C-ABI setzt beide gleich, `kern_renderer_init`).

### Nebel

Wie in three.js wird der Nebel **nach** der sRGB-Umwandlung angewendet, als `FogExp2`:

```
dist    = |P - eye|
optical = uFogDensity * dist
fog     = 1 - exp(-optical²)
```

Drei optionale Erweiterungen in `Lighting` ändern das; sind alle 0, sieht der Nebel aus wie in three.js:

- **Höhenabfall** (`fog_height_falloff = k`): Die optische Dichte wird mit dem Mittelwert von `exp(-k z)` entlang des Sichtstrahls multipliziert,
  `optical *= (exp(-k z0) - exp(-k z1)) / (k (z1 - z0))` mit `z0 = max(eye.z, 0)`, `z1 = max(P.z, 0)`.
  Der Dunst liegt so dicht am Boden und wird über den Dächern klar.
- **Sonnenstreuung** (`fog_sun_scatter`): In Blickrichtung zur Sonne färbt sich der Nebel zur Sonnenfarbe, gewichtet mit `max(dot(Blickrichtung, sun), 0)^8`.
- **Horizont** (`fog_horizon`): wirkt im Himmelspass, siehe 4.8.2.

### Der AO-Datensatz

Im Szenenpass schreibt der Shader (wenn `uAoOut == 1`) in den dritten Ausgang:

```glsl
outAo = vec4(clamp(indirect / max(outHdr.rgb, 1e-4), 0.0, 1.0) * (1.0 - fog),
             dot(base, vec3(0.2126, 0.7152, 0.0722)));
```

Also pro Farbkanal den Anteil indirekten Lichts am Gesamtlicht, bereits um den Nebel verringert, und in Alpha die Luminanz der Albedo. Transparente Meshes und die Ego-Ebene schreiben `(0, 0, 0, alpha)`, gelten also als unverdeckt. Wie dieser Datensatz verwendet wird, steht in 4.8.4.

## 4.6 Shader-Erweiterungen

Eine `ShaderExtension` (`gfx/shader.h`) liefert GLSL-Schnipsel für feste Einfügestellen und optional eigene Vertex-Ströme, Uniforms und Materialparameter. Die Engine bleibt unverändert; das Spiel reicht seine Erweiterungen bei `Renderer::init()` herein (in einer `App` über `App::extensions()`, in der C-ABI über `kern_game_extensions`).

### Die Einfügestellen

| Hook | Stufe | Was dort sichtbar ist und geändert werden darf |
|---|---|---|
| `vertex_declarations` | Vertex | eigene Uniforms und `out`-Variablen |
| `vertex` | Vertex, nach den Standard-Varyings | darf `vColor`, `vOpacity`, `vUV` ändern; sieht `model`, `local`, `aColor`, `aUV`, `aNormal`, `iTint`, `iModel*` |
| `fragment_declarations` | Fragment | eigene Uniforms, `in`-Variablen, Funktionen |
| `fragment_discard` | Fragment, früh | darf `discard`; sieht `vWorld`, `alpha`, `texel` |
| `fragment_input` | Fragment | darf `linearInput` ändern |
| `fragment_surface` | Fragment, vor der Beleuchtung | darf `base`, `alpha`, `surfaceGlass`, `emissive` ändern oder verwerfen; sieht `n`, `sun`, `uEye`, `vWorld`, `vUV` |

Jeder eingefügte Schnipsel bekommt eine Kommentarzeile `// --- <name>` voran, damit Compilerfehler im Log der richtigen Erweiterung zuzuordnen sind.

### Vertex-Ströme

`attributes()` gibt eine Liste von `ExtensionAttribute { key, size, glsl }` zurück: `key` ist der Name, unter dem Hosts den Strom liefern (etwa `"wall"`), `size` 1 bis 4 Floats pro Vertex, `glsl` der Attributname im Shader. Über alle Erweiterungen zusammen sind höchstens zwei Ströme erlaubt; sie belegen die Locations 14 und 15 in Deklarationsreihenfolge. Der Renderer setzt dafür `attribute_base`.

Hosts liefern die Ströme so:

- **C++:** `MeshInput::extension[i]`, wobei `i` der Index in `Renderer::extension_attributes()` ist. `MeshData::as_input(extra_slot)` legt den Zusatzstrom eines `MeshData` in Slot `extra_slot`; ein Slot ≥ `kMaxExtensionAttributes` bedeutet „nicht übergeben“.
- **JavaScript:** `KernRenderer.upsert(id, { ..., [key]: Float32Array })` – die Fassade liest die Schlüssel aus `kern_extension_attribute_key`.

Ein Mesh ohne den Strom ist gültig; das Attribut ist dann deaktiviert und liefert im Shader den GL-Standardwert (0, 0, 0, 1).

### Materialparameter und Uniforms

- `material_keys()` benennt ganzzahlige Materialparameter. Der Renderer verteilt sie auf `Material::params` (insgesamt höchstens 8) und setzt `material_base`. Pro Draw ruft er `material_uniforms(cache, material, material.params + material_base)` auf.
- `bind(program)` läuft nach jedem (Neu-)Linken des Standardprogramms, also auch nach einem Kontextverlust. Hier löst die Erweiterung ihre Uniform-Locations auf.
- `frame_uniforms(cache, renderer)` läuft einmal pro Szenenpass, nach den Frame-Uniforms der Engine.

Erweiterungen sollten ihre Uniforms über den übergebenen `UniformCache` setzen; dann gelten dieselben Einsparungen wie für die Engine selbst.

## 4.7 Der Ablauf eines Frames

`Renderer::render(params, stats)` erzeugt einen Frame. Die Schritte im Code:

**0. Prüfen.** Bevor irgendein GL-Zustand angefasst wird, prüft der Renderer Viewport-Rechteck, Kamera (`camera_valid`) und Blickrichtung. Ein abgelehnter Frame hinterlässt keinen gebundenen Framebuffer, keinen aktiven Scissor und keine offene Timer-Query. Danach entstehen View-, Projektions- und View-Projektionsmatrix und daraus das Frustum (siehe [Kapitel 5](05-szene-und-geometrie.md)).

**1. GPU-Zeit einsammeln.** Wenn `EXT_disjoint_timer_query_webgl2` vorhanden ist, umschließt eine `TIME_ELAPSED`-Query den Frame. Die Ergebnisse kommen asynchron einige Frames später; höchstens 4 Queries sind gleichzeitig unterwegs, und Query-Objekte werden wiederverwendet. Ist das Ergebnis durch `GPU_DISJOINT` ungültig, wird es verworfen. `RenderStats::gpu_ms` ist −1, solange keine Messung vorliegt.

**2. Sonnenschatten.** Falls aktiviert, wird die Schattenkarte nur neu gezeichnet, wenn sich ihr Schlüssel geändert hat (4.8.1); sonst setzt der Renderer `stats.shadow_reused`.

**3. Ziel binden.** Ohne Viewport-Rechteck (`has_viewport == false`) bindet `BloomPass::begin()` das Szenen-Target, sofern Bloom eingeschaltet (`BloomOptions::enabled`) und unterstützt ist; sonst zeichnet der Renderer direkt in den Standard-Framebuffer. Mit Rechteck wird mit Scissor direkt in den Standard-Framebuffer gezeichnet – ohne Bloom und ohne Ambient Occlusion. Danach Viewport setzen, Farbe und Tiefe löschen.

**4. Einsortieren.** Jedes lebende Mesh wird geprüft: unsichtbar, ohne GPU-Daten oder außerhalb der Ebenenmaske → übersprungen. Nicht geskinnte Meshes mit `frustum_culled` werden gegen das Frustum getestet (`culled_meshes`). Geskinnte Meshes werden nie über Bounds verworfen, weil ihre Pose die Bind-Pose-Bounds verlassen kann. Dann drei Listen: Ego-Ebene, transparent (`AlphaMode::blend`), opak. Sortierschlüssel ist der quadrierte Abstand des Weltbounds-Mittelpunkts zur Kamera.
   - Opak: **vorne nach hinten**. Nahe Flächen füllen den Tiefenpuffer zuerst, verdeckte Fragmente scheitern am frühen Tiefentest und kosten keine Shader-Zeit.
   - Transparent: **hinten nach vorne**, damit die Alpha-Mischung richtig übereinanderliegt.

**5. Opake Meshes.** Tiefentest `LEQUAL`, Tiefenschreiben an, kein Blending, Standardprogramm, Frame-Uniforms einmal (`set_frame_uniforms`), `uAoOut = 1`, dann ein Draw pro Mesh (`draw_mesh`). Pro Draw werden nur Material-Uniforms gesetzt (über den Cache), die Textur nur bei Wechsel gebunden und der Cull-Zustand nur bei Wechsel umgeschaltet.

**6. Himmel.** Wenn `params.sky` und die Kamera perspektivisch ist: ein bildschirmfüllendes Dreieck in der Fernebene, das nur dort Fragmente erzeugt, wo noch nichts gezeichnet wurde (4.8.2).

**7. Transparente Meshes.** Blending `SRC_ALPHA, ONE_MINUS_SRC_ALPHA`, Tiefentest an, Tiefenschreiben aus, `uAoOut = 0`.

**8. Ambient Occlusion.** GTAO rechnet aus der Tiefentextur des Szenen-Targets (4.8.4). Gibt es Ego-Meshes, geschieht das *vor* ihnen, weil deren Depth-Clear die Welttiefe löschen würde; danach wird das Szenen-Target wieder gebunden.

**9. Ego-Ebene.** `clear(DEPTH_BUFFER_BIT)`, dann die Viewmodel-Meshes.

**10. Composite.** `BloomPass::end()` extrahiert die hellen Anteile, weichzeichnet sie, wendet die Ambient Occlusion auf den indirekten Anteil an und schreibt das Ergebnis in den Standard-Framebuffer (4.8.3).

**11. Statistik.** `RenderStats` füllt Drawcalls, Schatten-Drawcalls, Dreiecke, Instanzen, verworfene und sichtbare Meshes, CPU-Zeit, GPU-Zeit, Bloom-Zustand und ob AO angewendet wurde.

### Framebuffer und Texturen eines Frames

| Ziel | Format | Größe | Inhalt |
|---|---|---|---|
| Schattenkarte | `DEPTH_COMPONENT24`, Vergleichsmodus `LEQUAL` | `shadow.resolution`² | Tiefe aus Sicht der Sonne |
| Szenen-Target, Attachment 0 | `RGBA8` | volle Größe | LDR-Bild (tonemapped, sRGB, Nebel) |
| Szenen-Target, Attachment 1 | `RGBA16F` | volle Größe | lineares HDR vor dem Tonemapping |
| Szenen-Target, Attachment 2 | `RGBA8`, `NEAREST` | volle Größe | AO-Datensatz: indirekter Anteil rgb, Albedo-Luminanz |
| Szenen-Target, Tiefe | `DEPTH_COMPONENT24`-Textur, `NEAREST` | volle Größe | Szenentiefe, von GTAO und Composite gelesen |
| Bloom a1, b1 | `RGBA16F`, `LINEAR` | 1/4 | Helligkeitsauszug, Blur-Pingpong |
| Bloom a2, b2 | `RGBA16F`, `LINEAR` | 1/8 | zweite, breitere Blur-Stufe |
| GTAO a, b | `RG16F`, `NEAREST` | halb oder voll | (Sichtbarkeit, Sichttiefe) roh und gefiltert |

Die drei Farb-Attachments des Szenen-Targets belegen zusammen 16 Byte pro Pixel (4 + 8 + 4). Der Code kommentiert das als Absicht: Das ist das Tile-Budget kachelbasierter Mobil-GPUs. Deshalb ist der AO-Datensatz RGBA8 und nicht Fließkomma.

Weil der Himmel an dieselben drei Ausgänge schreiben muss („WebGL needs every draw buffer written“), schreibt auch sein Shader `outHdr` und `outAo = 0`.

![Vier Viewports in Kern Studio: Teilansichten zeichnen mit Scissor direkt in den Standard-Framebuffer](bilder/studio-quad.png)

Kern Studio rendert jeden Viewport mit einem eigenen `render()`-Aufruf. Ein einzelner, bildschirmfüllender Viewport rendert ohne Rechteck, damit Bloom und AO laufen; in der Vier-Ansicht hat jeder Viewport ein Rechteck und damit weder Bloom noch AO.

## 4.8 Die Passes im Detail

### 4.8.1 Sonnenschatten

`SunShadowMap` (`src/gfx/shadow.cpp`) ist eine einzelne orthografische Tiefenkarte mit Hardware-PCF.

**Optionen und Grenzen** (`ShadowOptions`):

| Feld | Standard | Gültig | Bedeutung |
|---|---|---|---|
| `resolution` | 512 | Zweierpotenz 128–4096 | Kantenlänge der Karte in Texeln |
| `extent` | 120 | 10–2000 | halbe Kantenlänge des abgedeckten Quadrats in Metern |
| `bias` | 0,0012 | 0 ≤ bias < 0,04 | Tiefenversatz beim Vergleich |

**Lichtbasis.** Aus der Sonnenrichtung `dir` entsteht eine Orthonormalbasis `right`, `up` (Hilfsvektor +Z, bei fast senkrechter Sonne +Y).

**Einrasten auf das Texelraster.** Der Fokus (`camera.target`) wird in Lichtkoordinaten zerlegt und auf ganze Texel gerundet:

```cpp
const f32 texel = 2 * options_.extent / static_cast<f32>(options_.resolution);
const f64 a = std::round(dot(focus, b.right) / texel);
const f64 c = std::round(dot(focus, b.up) / texel);
const f64 d = std::round(dot(focus, b.dir) / 8.0f);   // grob entlang des Lichts
```

Warum? Bewegt sich die Karte um Bruchteile eines Texels mit der Kamera, fallen Schattenkanten in jedem Frame anders auf das Raster – die Kanten „schwimmen“ und flimmern. Eingerastet verschiebt sich die Karte nur in ganzen Texeln, und eine Kante liegt immer gleich. Entlang der Lichtrichtung reicht ein grober 8-m-Schritt; diese Bewegung verschiebt keine Texel, sie muss nur den Tiefenbereich gültig halten.

**Wiederverwendung.** Aus den gerundeten Koordinaten und der (auf 10⁻⁴ gerundeten) Sonnenrichtung entsteht ein Hash. Der Renderer verknüpft ihn mit der Schatten-Revision und der Ebenenmaske des Frames. Nur wenn sich dieser Schlüssel ändert, wird die Karte neu gezeichnet. Die Schatten-Revision steigt, wenn ein Schattenwerfer erscheint, verschwindet, sich bewegt, sein Material ändert oder `invalidate(true)` gerufen wird. Bei einer stehenden Kamera über einer statischen Stadt kostet der Schatten danach keinen einzigen Drawcall.

**Zeichnen.** Matrix: `ortho(-e, e, -e, e, 0.2, 4e) * look_at(focus + dir * 2e, focus, up)`. Das Lichtfrustum verwirft Meshes, deren Bounds außerhalb liegen (außer geskinnten). Farbschreiben aus, Tiefentest `LESS`, `polygonOffset(2, 3)` gegen Schattenakne, eigener schlanker Shader (nur Position, Instanzmatrix, Skinning; der Fragment-Shader ist leer). Gezeichnet werden nur Meshes mit `casts_shadow()` in den aktiven Ebenen, mit ihrem eigenen Cull-Modus.

**Abtasten.** Die Tiefentextur hat `TEXTURE_COMPARE_MODE = COMPARE_REF_TO_TEXTURE` und lineare Filterung. Ein `texture(sampler2DShadow, ...)` vergleicht dann vier benachbarte Texel in der Hardware und interpoliert die Ergebnisse bilinear – das ist das „Hardware-PCF“. Im Standard-Shader:

```glsl
if (uSunShadowEnabled == 1 && ndl > 0.0) {
  vec4 clip = uSunShadowProjection * vec4(vWorld, 1.0);
  vec3 uvz = clip.xyz / clip.w * 0.5 + 0.5;
  if (uvz.x > 0.0 && uvz.x < 1.0 && uvz.y > 0.0 && uvz.y < 1.0 && uvz.z > 0.0)
    shadow = texture(uSunShadowMap, vec3(uvz.xy, uvz.z - uSunShadowBias));
}
```

Außerhalb der Karte gilt eine Fläche als beleuchtet. Flächen, die von der Sonne weg zeigen (`ndl == 0`), lesen die Karte gar nicht.

Daraus folgt der wichtigste Abstimmungshinweis: `extent` und `resolution` bestimmen zusammen die Texelgröße. Orb Run und Kern Studio nutzen 2048 Texel auf ±110 m, also gut 10 cm pro Texel; `examples/hello` 1024 Texel auf ±40 m.

### 4.8.2 Himmel

`SkyPass` (`src/gfx/sky.cpp`) zeichnet ein einziges bildschirmfüllendes Dreieck, das der Vertex-Shader aus `gl_VertexID` erzeugt, auf die Fernebene (`gl_Position.z = w`). Er läuft **nach** den opaken Meshes mit Tiefentest `LEQUAL` und ausgeschaltetem Tiefenschreiben. Nur Pixel, die noch auf dem gelöschten Tiefenwert 1 stehen, bestehen den Test. Der aufwendige Himmelsshader läuft also nur dort, wo wirklich Himmel zu sehen ist.

Die Blickrichtung pro Pixel entsteht aus Vorwärts-, Rechts- und Hochvektor der Kamera und `tan(fov/2)`. Der Fragment-Shader setzt den Himmel aus diesen Teilen zusammen:

- **Gradient** zwischen Horizont- und Zenitfarbe mit `pow(h, 0.45)` (h = Höhe der Richtung), nachts zu dunklen Blautönen gemischt (`SkyState::night`).
- **Goldene Stunde:** `dusk = clamp(1 - |sun.z| * 4, 0, 1) * (1 - night * 0.6)` färbt den Horizont orange und den Zenit violett, plus ein Glühen am Horizont auf der Seite der Sonne.
- **Sonne:** Mie-artiges Glühen `pow(mu, 8) * 0.22 + pow(mu, 64) * 0.5` und eine Scheibe `smoothstep(0.9993, 0.99975, mu) * 12` mit `mu = dot(Richtung, Sonne)`. Steht die Sonne unter −0,05, fällt beides weg.
- **Nacht:** Mondschein, Mondscheibe und funkelnde Sterne aus einem Hash-Raster.
- **Wolken:** eine Schicht aus 5-Oktaven-fBm (Value Noise), projiziert über `d.xy / (d.z + 0.12)`, verschoben mit `SkyState::wind * Zeit`. `SkyState::blast_age > 0` drückt die Wolken von einer Explosionsrichtung weg (Schockwelle).
- **Unter dem Horizont** blendet die Farbe zur Horizontfarbe über.
- **Horizontnebel:** Mit `fog_horizon > 0` löst sich der Horizont in die Nebelfarbe der Szene auf (gleiche Farbe, gleicher Sonnenschein). Der Renderer skaliert den Wert mit `1 - exp(-(fog_density * min(far, 2000))²)`: Dünner Nebel ergibt nur einen dünnen Dunst am Horizont.

Wie die Szene schreibt der Himmel HDR in `outHdr` und das tonemappte Ergebnis in `outColor`. Bei orthografischen Kameras gibt es keinen Himmel; dann bleibt die Löschfarbe stehen.

### 4.8.3 Bloom und Composite

`BloomPass` (`src/gfx/bloom.cpp`) braucht Rendern in Fließkomma-Texturen (`EXT_color_buffer_float` oder `EXT_color_buffer_half_float`). Fehlt das oder scheitert ein Programm, meldet `stats.bloom` den Zustand `unsupported`, und der Renderer zeichnet direkt in den Standard-Framebuffer.

**Ablauf in `end()`** (alle Schritte mit einem bildschirmfüllenden Dreieck):

1. **Helligkeitsauszug** aus dem HDR-Attachment in a1 (1/4 Auflösung) mit weichem Knie:
   `o = c * smoothstep(0.88, 0.88 + 0.35, max(c.r, c.g, c.b))`.
2. **Blur Stufe 1:** horizontal a1 → b1, vertikal b1 → a1. Der Kern hat 9 Taps, wird aber mit 5 Texturzugriffen gelesen: Die äußeren Paare liegen bei Offsets 1,3846 und 3,2308 Texel, die lineare Filterung mischt jeweils zwei Nachbartexel in einem Zugriff. Gewichte 0,2270 / 0,3162 / 0,0703.
3. **Kopie** a1 → a2 (1/8 Auflösung), **Blur Stufe 2** a2 ↔ b2 – die breitere Glüh-Stufe.
4. **Composite** in den Standard-Framebuffer.

Der Composite-Shader rechnet so:

```glsl
vec3 scene = texture(tScene, vUv).rgb;                       // fertiges LDR-Bild
vec3 b = texture(tB1, vUv).rgb * uS1 + texture(tB2, vUv).rgb * uS2;   // 0.13, 0.08
...
vec3 h = max(texture(tHdr, vUv).rgb, 0.0), h2 = h;
if (occluded) h2 *= 1.0 - share.rgb * (1.0 - aoMultiBounce(aoVisibility(px), vec3(share.a)));
o = vec4(clamp(scene + present(h2 + b) - present(h), 0.0, 1.0), 1.0);
```

`present()` ist dieselbe Kette wie im Szenenshader (ACES, optional Grading, sRGB). Der Composite addiert also zum fertigen Bild nur die **Differenz** zwischen „HDR mit Glühen und AO“ und „HDR ohne“, beide durch dieselbe Tonemapping-Kurve. Warum so umständlich? Weil das LDR-Bild den Nebel bereits enthält, der *nach* sRGB angewendet wurde und im HDR-Attachment fehlt. Würde der Composite das HDR-Bild neu tonemappen, ginge der Nebel verloren. Pixel ohne Glühen und ohne Verdeckung kopiert der Shader unverändert (`if (!glow && !occluded)`), was auch Rechenzeit spart.

**Frame-Zeit-Wächter.** `begin()` führt einen gleitenden Mittelwert des Frame-Abstands (Faktor 0,05, Ausreißer über 200 ms zählen nicht). Liegt er länger als 3,5 s über 27 ms, schaltet sich das Glühen für die Sitzung ab (`BloomState::disabled_slow_device`), außer `BloomOptions::forced` ist gesetzt. Der Composite läuft danach weiter, denn er wendet auch die AO an. Bei verborgener Seite (`page_hidden`) wird der Wächter zurückgesetzt, damit gedrosselte Hintergrund-Frames das Glühen nicht abschalten.

### 4.8.4 Ambient Occlusion (GTAO)

Kern implementiert Ground-Truth Ambient Occlusion nach Jimenez, Wu, Pesce und Jarabo (2016) in der Form von Intels XeGTAO (`src/gfx/gtao.cpp`). Die Besonderheit liegt weniger im Verfahren als in der Einbettung: **Es gibt keinen Depth-Prepass und keinen zusätzlichen Geometriepass.**

**Die Idee in drei Schritten:**

1. Der normale Vorwärtspass schreibt neben dem Bild in Attachment 2, welcher Anteil jedes Pixels aus indirektem Licht stammt (4.5, „Der AO-Datensatz“).
2. GTAO rechnet auf der fertigen Szenentiefe, wie stark jeder Punkt von seiner Umgebung verdeckt ist.
3. Der Bloom-Composite dunkelt genau den indirekten Anteil ab. Direktes Sonnenlicht hat bereits seinen Schatten, Punktlichter ebenso; sie bleiben unberührt.

Damit ist AO physikalisch an der richtigen Stelle (Verdeckung betrifft Umgebungslicht), ohne dass ein zweites Mal Geometrie gezeichnet werden muss. Der README begründet den Verzicht auf den Prepass damit, dass die Hidden-Surface-Removal-Hardware von Apple und Mali-FPK Überzeichnung ohnehin verwirft.

**Hauptpass** (ein AO-Texel pro Fragment, Ausgabe RG16F = Sichtbarkeit, Sichttiefe):

- *Sichttiefe aus dem Tiefenpuffer:* `w = P[3][2] / ((d * 2 - 1) + P[2][2])`, Position im Sichtraum `(ndc.x * w / P[0][0], ndc.y * w / P[1][1], -w)`.
- *Normale aus der Tiefe:* Pro Achse wird der Nachbar genommen, dessen Tiefensprung kleiner ist (er liegt eher auf derselben Fläche); `N = normalize(cross(dx, dy))`, zur Kamera gedreht.
- *Radius:* `uRadius` (Weltmeter) wird in AO-Pixel umgerechnet und auf 20 % der Bildhöhe begrenzt. Unter 1,5 Pixeln gilt der Punkt als unverdeckt.
- *Slices:* Für jede von `uSlices` Richtungen `phi` in der Bildebene wird die Normale in die Slice-Ebene projiziert (`projN`, Winkel `n`). Dann sucht der Shader auf beiden Seiten in `uSteps` Schritten den höchsten Horizont: der größte Kosinus `dot(Δ, V) / |Δ|` der Abtastpunkte. Die Abtastverteilung ist quadratisch (`t = t² + minS`), dichter nahe am Punkt. Ein Abfall über die letzten 61,5 % des Radius blendet ferne Punkte zum Ausgangshorizont zurück, damit entfernte Geometrie nicht verdeckt.
- *Integral:* Die Horizontwinkel `h0`, `h1` werden auf ±π/2 um `n` begrenzt, dann wird die kosinusgewichtete Sichtbarkeit analytisch integriert:
  ```
  arc(h) = (cos n + 2 h sin n - cos(2h - n)) / 4
  visibility += |projN| * (arc(h0) + arc(h1))
  ```
  Am Ende `visibility = pow(visibility / slices, uPower)`, mindestens 0,03.
- *Rauschen:* Die Slice-Rotation folgt einem geordneten 4×4-Bayer-Muster; jeder 4×4-Block enthält jede Rotation genau einmal. `acos` wird über `fastAcos` (Lagarde, max. Fehler 0,0068 rad) genähert.

**Entrauschen.** Ein tiefenbewusster 4×4-Filter mittelt genau über dieses Muster. Nachbarn mit anderer Tiefe (Toleranz `w * 0.04 + 0.05`) bekommen kein Gewicht, damit an Kanten nichts verläuft.

**Anwenden im Composite.** `aoVisibility` liest die AO-Textur mit einem tiefenbewussten bilinearen Upsampling (Toleranz `w * 0.03 + 0.03`). Dann der Multi-Bounce-Fit aus dem Paper – helle Albedo gewinnt Licht zurück, das zwischen den Verdeckern hin- und herspringt:

```glsl
vec3 aoMultiBounce(float v, vec3 albedo) {
  vec3 a = 2.0404 * albedo - 0.3324, b = -4.7951 * albedo + 0.6417, c = 2.7552 * albedo + 0.6903;
  return max(vec3(v), ((v * a + b) * v + c) * v);
}
```

und `h2 *= 1 - share.rgb * (1 - aoMultiBounce(v, share.a))`.

**Qualitätsstufen** wie XeGTAO (Slices × Schritte pro Seite):

| `AoQuality` | Slices | Schritte |
|---|---|---|
| `low` | 1 | 2 |
| `medium` | 2 | 2 |
| `high` | 3 | 3 |
| `ultra` | 9 | 3 |

**Optionen** (`AoOptions`): `enabled` (an), `quality` (`medium`), `half_resolution` (an), `radius` (1,6 m), `power` (2,2), `adaptive` (an). Mit `adaptive` stuft ein Frame-Zeit-Wächter (gleiche Schwellen wie Bloom: 27 ms für 3,5 s) zuerst die Qualität bis `low` herunter und schaltet dann auf halbe Auflösung.

`set_ambient_occlusion_radius(r)` ändert nur den Radius – eine Eigenschaft der Szene, nicht des Geräts; 0 schaltet AO für diese Szene ab. Die Spiel-Laufzeit setzt ihn aus `WorldSettings::ao_radius` ([Kapitel 8](08-spiel-laufzeit.md)).

**Wann AO läuft:** nur mit Szenen-Target (also nicht in Teil-Viewports und nicht, wenn Bloom abgeschaltet oder nicht unterstützt ist), nur bei perspektivischer Kamera und nur wenn aktiviert. Das Studio und exportierte Spiele wählen auf Geräten mit grobem Zeiger (Telefone) `medium` in halber Auflösung, sonst `high` in voller; die URL-Parameter `?ao=off|low|medium|high|ultra` und `?aoRes=half|full` überschreiben das (`web/studio/game.mjs`). Aus JavaScript: `KernRenderer.setAmbientOcclusion({enabled, quality, fullResolution, radius})`.

## 4.9 Partikel und Punktlichter

### `Particles`

`Particles` (`gfx/particles.h`) zeichnet kamerazugewandte Quads – Funken, Rauch, Mündungsfeuer – als **ein** instanziertes, transparentes Mesh. Es ist rein visuell: keine Kollision, keine Physik.

```cpp
struct ParticleSpec {
  Vec3 position, velocity;
  Vec4 color{1, 1, 1, 1};  // alpha blendet über die Lebenszeit aus
  f64 start_ms = 0;        // Startzeit (dieselbe Uhr wie tick())
  f32 life_ms = 350;       // 16 .. 10000
  f32 size = 0.35f;        // Kantenlänge in Metern bei Geburt
  f32 gravity = 0;         // m/s² nach unten (negativ hebt)
  f32 grow = 0;            // Größe * (1 + grow * t)
  f32 fade_extra = 0;      // alpha *= 1 - fade_extra * t
};
```

- `init(renderer, capacity, mesh_id)` – Budget 1 bis 4096; ohne `mesh_id` holt sich das System selbst eine Id.
- `emit(spec)` – prüft alle Werte (endlich, Farben in [0, 1], Lebenszeit, Größe) und gibt eine Seriennummer zurück, oder 0, wenn das Budget voll ist (`stats.dropped`).
- `tick(now_ms, eye)` – Zeit läuft nie rückwärts (sonst würden ausgeblendete Partikel nach einer Pause wieder auftauchen). Abgelaufene Partikel fallen weg. Position: `p0 + v t − (0, 0, ½ g t²)`. Die Quads werden um Z zur Kamera gedreht, **hinten nach vorne sortiert** (innerhalb eines Draws mischt die GPU in Reihenfolge der Instanzen) und über `update_instances` hochgeladen. Alpha: `color.a * (1 − t) * (1 − fade_extra * t)`.

Das Mesh ist unlit, zweiseitig, wirft keinen Schatten, wird nicht frustum-gecullt und ist unsichtbar, solange kein Partikel lebt. Die Spiel-Laufzeit nutzt ein System mit 512 Partikeln; aus JavaScript steht dieselbe Klasse über `kern_fx_*` zur Verfügung.

### `LightPool`

Der Shader kennt 6 Punktlichter. `LightPool` wählt pro Frame die wichtigsten aus beliebig vielen Anfragen aus:

```
score = intensity / (1 + distance_to_eye / max(4, range))
```

Bei Gleichstand gewinnt die frühere Anfrage. `request()` begrenzt Intensität auf 100 und Reichweite auf 1000; `resolve(eye)` sortiert, füllt eine `PointLights`-Struktur und leert die Warteschlange. Orb Run fordert pro Frame ein Scheinwerferlicht vor dem Auto und ein Licht pro Orb an und übergibt das Ergebnis an `set_point_lights`.

## 4.10 Einstellungen

### `RendererOptions`

| Feld | Standard | Bedeutung |
|---|---|---|
| `width`, `height` | 960 × 540 | Größe des Drawing Buffers (später `resize`) |
| `shadows` | `false` | Sonnenschatten anlegen |
| `shadow` | 512 / 120 / 0,0012 | `ShadowOptions` (4.8.1) |
| `bloom` | `enabled`, nicht `forced`, `grade` | `BloomOptions` (4.8.3) |
| `ao` | siehe 4.8.4 | `AoOptions` |
| `grade` | `true` | Produktions-Grading nach dem Tonemapping |

Zur Laufzeit: `set_shadows(&options)` bzw. `set_shadows(nullptr)`, `set_ambient_occlusion`, `set_ambient_occlusion_radius`, `set_point_lights`, `invalidate(shadows)`.

### `Lighting` und `SkyState`

`Renderer::lighting` hält alle Lichtfarben linear und mit Intensität multipliziert. Die Standardwerte heißen im Code „production day“:

| Feld | Standard | Wirkung |
|---|---|---|
| `sun_direction` | (−0,45, −0,52, 0,72) | Richtung *zur* Sonne |
| `sun_color` | (2,8, 2,47, 2,11) | Sonnenlicht |
| `sky_color`, `ground_color` | (0,469, 0,609, 0,85), (0,125, 0,094, 0,054) | Hemisphärenlicht |
| `fog_color`, `fog_density` | (0,474, 0,584, 0,716), 0,00085 | FogExp2 (sRGB-Farbe) |
| `exposure` | 1 | Belichtung vor ACES |
| `env_intensity` | 0,9 | Stärke von Himmels-Irradianz und Reflexion |
| `env_zenith`, `env_horizon`, `env_ground` | Blau, Hellblau, Grünbraun | Umgebungskuppel |
| `fog_height_falloff`, `fog_sun_scatter`, `fog_horizon` | 0 | Atmosphäre (4.5, 4.8.2) |

`Renderer::sky` (`SkyState`) steuert den Himmel getrennt: Sonnen- und Mondrichtung, `night` (0..1), `clouds`, `wind`, Explosionsrichtung und -alter.

Die Spiel-Laufzeit berechnet all das aus der Tageszeit (`GameWorld::apply_lighting`): Sie setzt ein Verhältnis von Sonne zu Himmelsirradianz von etwa 5:1, nutzt das Hemisphärenlicht nur noch als Nachtaufhellung, leitet `env_ground` aus dem Licht ab, das eine Straße mit Albedo 0,25 zurückwirft, und schaltet Höhennebel, Sonnenstreuung und Horizontnebel ein. Einzelheiten stehen in [Kapitel 8](08-spiel-laufzeit.md).

### `RenderParams`

| Feld | Standard | Bedeutung |
|---|---|---|
| `clear_color` | (0,045, 0,07, 0,12, 1) | Löschfarbe, sichtbar wo kein Himmel ist |
| `has_viewport`, `viewport` | aus | Teilrechteck mit Scissor; ohne Bloom und AO |
| `page_hidden` | `false` | setzt die Frame-Zeit-Wächter aus |
| `layers` | alle | Ebenenmaske für Zeichnen und Schatten |
| `sky` | `true` | Himmel zeichnen |

### Kamera

`Renderer::camera` ist eine `Camera` aus `math/math.h`: `eye`, `target`, `up` (Standard +Z), `fov` (Standard π/3), `near` 0,05, `far` 1800, optional `orthographic` mit `ortho_height`. Die Welt ist Z-oben, Einheiten sind Meter.

## 4.11 Kontextverlust

Ein Browser darf den WebGL-Kontext jederzeit wegnehmen – bei Speicherdruck, Treiber-Resets oder im Hintergrund auf Mobilgeräten. Kern behandelt das vollständig:

1. **Verlust** (`webglcontextlost`): Die Plattformschicht ruft `preventDefault()` (sonst gibt es keine Wiederherstellung), dann `kern_context_lost()` und `kern.resetTables()`. `Renderer::context_lost()` vergisst alle GPU-Handles, ohne `delete*` aufzurufen (die Objekte sind schon tot), und zerstört Programme und Passes im Modus „contextLost“. `render()` verweigert ab jetzt mit `"GL context lost; waiting for restoration"`. Meshes und Texturen bleiben als CPU-Kopie erhalten; `upsert` funktioniert weiter, lädt aber nichts hoch.
2. **Wiederherstellung** (`webglcontextrestored`): Die Seite füllt zuerst die host-eigenen Texturen in ihren reservierten Slots neu (`kern.pinned`), dann ruft sie `kern_context_restored()`. Der Renderer baut alle Programme und Passes neu (`init_gpu`, inklusive `bind()` aller Erweiterungen), lädt alle engine-eigenen Texturen und alle Meshes aus den CPU-Kopien hoch und invalidiert Szene und Schatten.
3. **Gescheiterte Wiederherstellung:** Der Renderer bleibt im verlorenen Zustand und gibt frei, was er schon angelegt hatte. Ein späterer Versuch beginnt sauber von vorn.

`set_headless()` nutzt denselben Mechanismus für Simulationen ganz ohne GPU, etwa im Lehrer ([Kapitel 14](14-lehrer.md)): Meshes bleiben auf der CPU, GL wird nie angefasst.

## 4.12 Performance-Hinweise

Was der Code selbst nahelegt:

- **Instanzieren statt duplizieren.** Viele gleiche Objekte (Bäume, Autos, Kisten) sind ein Mesh mit vielen Instanzen und damit ein Drawcall. Die Stadt der Laufzeit zeichnet alle Baumstämme als ein Mesh und alle Kronen als ein zweites.
- **Statische Geometrie zusammenführen.** `primitives::append` und die Chunks der Gebäude- und Straßenbauer fassen viele kleine Teile zu wenigen großen Meshes zusammen. Weniger Meshes bedeuten weniger Drawcalls und weniger Sortierarbeit.
- **Nur hochladen, was sich ändert.** `update_instances` vergleicht bytegenau und tut bei gleichen Daten nichts; ein reines Tint-Update lädt nur einen Puffer hoch und lässt den Schatten gültig. Wer Instanzen jeden Frame neu schreibt, sollte nur die bewegten Meshes anfassen.
- **Schatten wiederverwenden.** Jede Bewegung eines Schattenwerfers zeichnet die ganze Schattenkarte neu. Bewegte Dinge, die keinen sichtbaren Schatten brauchen (Partikel, Straßen, Bodenplatten), sollten `cast_shadow = false` haben – Orb Run und die Laufzeit tun das für Straßen und den Boden.
- **16-Bit-Indizes.** Meshes bis 65 536 Vertices bekommen automatisch 16-Bit-Indizes. `weld` (Kapitel 5) macht aus Triangle-Soups indizierte Meshes und spart dabei Speicher.
- **Frustum-Culling nicht abschalten** ohne Grund. `frustum_culled = false` ist für Dinge gedacht, deren Bounds nicht stimmen (Partikel).
- **AO und Bloom passen sich an.** Beide haben Frame-Zeit-Wächter. Für Telefone ist AO in halber Auflösung der Standard.
- **Messen:** `RenderStats` liefert Drawcalls, Schatten-Drawcalls, `shadow_reused`, Dreiecke, Instanzen, verworfene und sichtbare Meshes, CPU- und GPU-Zeit. In JavaScript gibt `KernRenderer.render()` dieselben Werte als Objekt zurück.

## 4.13 Beispiel: eine eigene Shader-Erweiterung (Orb Run)

Orb Run (`examples/orbrun/orbrun.cpp`) lässt die Fenster der Stadt nachts leuchten. Die Gebäude haben keine Fenster in der Geometrie. Stattdessen trägt jeder Vertex der Gebäude-Meshes einen „Wand-Datensatz“ mit vier Floats, den der Gebäudebauer erzeugt (Kapitel 5): Wandlänge (−1 für Dach, −2 für Gesims), Wandhöhe, ein Gebäude-Seed und die Stockwerkshöhe. Die UVs der Wände sind Meter entlang der Wand (u) und Meter über dem Fuß (v).

```cpp
class NightWindows final : public ShaderExtension {
 public:
  f32 night = 0;
  const char* name() const override { return "orbrun-night-windows"; }
  Span<const ExtensionAttribute> attributes() const override { return {kAttributes, 1}; }
  const char* vertex_declarations() const override { return "flat out vec4 vWall;\n"; }
  const char* vertex() const override { return "  vWall = aWall;\n"; }
  const char* fragment_declarations() const override {
    return R"GLSL(
// flat: the record is constant per wall; interpolation noise would scramble the window hash.
flat in vec4 vWall;
uniform float uNight;
float windowHash(float n) { return fract(sin(n * 91.7) * 43758.5453); }
)GLSL";
  }
  const char* fragment_surface() const override {
    return R"GLSL(
  if (vWall.x > 0.0 && abs(n.z) < 0.5) {
    float u = vUV.x, z = vUV.y, storey = vWall.w;
    float column = floor(u / 3.0), floorIndex = floor(z / storey);
    vec2 cell = vec2(fract(u / 3.0), fract(z / storey));
    // Fade the grid out where it would alias; distant facades keep an average glow instead.
    vec2 fw = fwidth(vec2(u / 3.0, z / storey));
    float farFade = 1.0 - smoothstep(0.08, 0.3, max(fw.x, fw.y));
    float win = step(0.22, cell.x) * step(cell.x, 0.78) * step(0.28, cell.y) * step(cell.y, 0.78)
              * step(storey, z) * step(z, vWall.y - 1.0) * farFade;
    float lit = step(0.42, windowHash(column * 7.31 + floorIndex * 17.0 + floor(vWall.z * 997.0 + 0.5)));
    base = mix(base, vec3(0.05, 0.06, 0.08), win * 0.85);
    surfaceGlass = max(surfaceGlass, win * (1.0 - lit * uNight) * 0.7);
    float tall = step(storey, z) * step(z, vWall.y - 1.0);
    emissive += (win * lit + (1.0 - farFade) * tall * 0.13) * uNight * vec3(1.0, 0.74, 0.42) * 1.6;
  }
)GLSL";
  }
  void bind(const Program& p) override { u_night_ = p.location("uNight"); }
  void frame_uniforms(UniformCache& cache, const Renderer&) override { cache.set_float(u_night_, night); }

 private:
  static constexpr ExtensionAttribute kAttributes[] = {{"wall", 4, "aWall"}};
  u32 u_night_ = 0;
};
```

Was hier passiert, Schritt für Schritt:

1. **Ein Vertex-Strom** `wall` mit 4 Floats, im Shader `aWall`. Er landet auf Location 14.
2. **Vertex-Hook:** Der Datensatz wird als `flat` weitergereicht. Er ist pro Wand konstant; Interpolation würde nur Rundungsrauschen erzeugen, das den Fenster-Hash verwürfelt.
3. **Fragment-Hook `fragment_surface`:** Nur auf Wänden (`vWall.x > 0`) mit annähernd waagerechter Normale. Das Raster hat 3 m breite Spalten und Stockwerkshöhe; ein Fenster belegt den Mittelteil jeder Zelle, ab dem ersten Stock bis 1 m unter der Traufe. Ein Hash aus Spalte, Stockwerk und Gebäude-Seed entscheidet, ob ein Fenster beleuchtet ist (etwa 58 %).
4. **Gegen Aliasing:** `fwidth` misst, wie schnell sich die Rasterkoordinate pro Pixel ändert. Wird das Raster feiner als etwa ein Pixel, blendet `farFade` die Fenster aus und ersetzt sie durch ein mittleres Glühen der Fassade.
5. **Wirkung auf das Material:** Fensterflächen werden dunkel (`base`), unbeleuchtete Fenster werden zu Glas (`surfaceGlass` – sie spiegeln den Himmel), beleuchtete bekommen warmes `emissive`. Weil `emissive` linear und unbegrenzt ist, glühen die Fenster über den Bloom-Pass.
6. **Uniform:** `uNight` wird in `bind()` aufgelöst und einmal pro Frame über den Cache gesetzt. Das Spiel schreibt `windows_.night` aus seinem Tag-Nacht-Zyklus.

Eingebunden wird die Erweiterung über `App::extensions()`:

```cpp
Span<ShaderExtension* const> extensions() override { return {extensions_, 1}; }
...
ShaderExtension* extensions_[1] = {&windows_};
```

Beim Hochladen der Gebäude-Chunks gibt Orb Run den Zusatzstrom in Slot 0 weiter (`mesh.as_input(0)`); Straßen und Bodenplatten bekommen `kMaxExtensionAttributes` als Slot, also keinen Strom – dort ist `vWall.x` 0, und der Hook tut nichts.

Die Spiel-Laufzeit hat eine Klasse `kern::game::NightWindows` mit identischem GLSL (deklariert in `include/kern/game/runtime.h`, implementiert in `src/game/runtime.cpp`), die Kern Studio und exportierte Spiele immer registrieren. Die Komponente „Procedural City“ entscheidet mit `night_windows`, ob ihre Gebäude den Strom bekommen ([Kapitel 5](05-szene-und-geometrie.md)).

---

© 2026 Christian Heinrich Hohlfeld · Alle Rechte vorbehalten · [Impressum und Rechte](00-impressum.md)
