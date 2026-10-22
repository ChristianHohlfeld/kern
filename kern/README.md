# Kern – die SpawnCity-Engine

Kern ist eine eigenständige 3D-Engine in **C++20**, die mit wasi-sdk nach **WebAssembly** kompiliert wird und im Browser über **WebGL2** zeichnet. Kein three.js, kein Emscripten, keine Engine-Logik in JavaScript. Im Browser bleibt nur eine dünne Plattformschicht (`kern/web/`) übrig, weil WebGL2, Canvas, Eingabe und `requestAnimationFrame` dort nur über JavaScript erreichbar sind.

SpawnCity läuft auf Kern (`games/spawncity/` + `sim/kern_spawncity*.mjs`). Die Engine kennt SpawnCity nicht. Zwei Beispielspiele zeigen, wie man ein Spiel nur gegen die Engine-API schreibt:

- `kern/examples/hello`: das kleinste Spiel, rund 90 Zeilen
- `kern/examples/orbrun`: ein vollständiges Spiel in C++ mit prozeduraler Stadt, Fahrphysik, Verkehr, Tag/Nacht und einer eigenen Shader-Erweiterung

Dazu kommen die **Spiel-Laufzeit** (`kern::game`: Levels aus Actors und Komponenten, auf Box3D gespielt) und **Kern Studio** (`kern/src/studio`, `kern/web/studio`), der Engine-Editor im Stil der Unreal Engine: Viewports mit Fly Mode und Unreal-Navigation, Gizmos, Outliner, Details, World Settings, Content Drawer, Play und Simulate im Viewport sowie Spiele als eigenständige HTML-Datei. Beim Start ist Orb Run als Level geladen. Ausführliche Dokumentation: [`docs/studio.md`](docs/studio.md).

## Aufbau

```
kern/
  include/kern/          öffentliche Header (kern/kern.h bindet alles ein)
    core/base.h          Typen, Fehlerkanal (fail/last_error), Logging, Span, Hashes
    math/math.h          Vec2/3/4, Quat, Mat4 (spaltenweise, Z oben), Sphere, Aabb, Camera
    gfx/gl.h             WebGL2-Einsprungpunkte (Import-Modul "kern_gl")
    gfx/program.h        Programme und UniformCache (sendet nur geänderte Werte)
    gfx/shader.h         Standard-Shader und ShaderExtension (Hooks für Spiele)
    gfx/mesh.h           Mesh, Material, MeshInput
    gfx/passes.h         Sonnenschatten, Himmel, Bloom
    gfx/renderer.h       Renderer
    scene/               Frustum-Culling, BVH, Picking
    geo/                 Earcut, Primitive, Welding, Gebäude, Straßen, Terrain
    app/app.h            App/Engine/Input für Spiele, die ganz in C++ laufen
    api/c_api.h          flache C-ABI für JavaScript-Hosts
    core/json.h          JSON-Wert, Parser, Formatierer (Schlüsselreihenfolge bleibt)
    physics/physics.h    Box3D-Schicht: Welt, Körper, Shapes, Gelenke, Raycast, MouseGrab, Kontaktpunkte, Gelenkkräfte
    game/                Levels (.klevel), Geometrie und prozedurale Stadt, GameWorld (Darstellung, Box3D, Controller, Regeln), Demo-Levels
    studio/studio.h      Kern Studio (komplette Editorlogik, ohne Browser testbar)
  src/                   Implementierung (gleiche Gliederung; src/studio/api.cpp = kern_studio_* C-ABI)
  third_party/box3d/     Box3D (Erin Catto, MIT), als C17 mitkompiliert
  web/kern.mjs           Browser-Plattformschicht + KernRenderer (JS-Fassade der C-ABI)
  web/app.mjs            Host für C++-Spiele (Frame-Loop, Größe, Eingabe, Kontextverlust)
  web/studio/            Studio-Seite (index.html, studio.css, studio.mjs), Spiel-Host (game.mjs, play.html)
  docs/studio.md         Dokumentation von Studio, Komponenten, Format und Protokoll
  docs/physics.md        Box3D richtig benutzen: Regeln, No-Gos und Messungen (vor Physik-Änderungen lesen)
  docs/walker.md         Micro Duck: Pollen Robotics' trainierte Laufpolicy auf Box3D (Modell, Servo, Takt, Messungen)
  examples/              Beispielspiele
  tests/                 C++-Unit-Tests (Host-Build mit ASan/UBSan), tests/e2e: Browser-Test von Studio
  build.sh               WebAssembly-Build (--app, --physics, --game, --studio)
```

## Bauen

Man braucht [wasi-sdk](https://github.com/WebAssembly/wasi-sdk) (Release 25). Die CI lädt es von GitHub.

```sh
export WASI_SDK=/opt/wasi-sdk-25.0-x86_64-linux

# SpawnCity-Modul (Engine + games/spawncity) -> generated/spawncity_kern.mjs (base64, für den Single-File-Build)
tools/build_kern_wasm.sh

# ein C++-Spiel (mit App-Laufzeit)
kern/build.sh --app kern/examples/orbrun/orbrun.wasm kern/examples/orbrun/orbrun.cpp

# Kern Studio als eine HTML-Datei -> generated/studio.html (Modul: generated/kern_studio.wasm)
tools/build_studio.sh

# Unit-Tests (nativ, mit Sanitizern)
CXX=g++ kern/tests/run.sh
```

Das Modul ist ein WASI-"Reactor": Der Host ruft einmal `_initialize` auf und danach die exportierten Funktionen. Der Feature-Satz ist bewusst konservativ (`mvp` plus sign-ext, bulk-memory, nontrapping-fptoint, mutable-globals), damit es auf jedem WebGL2-fähigen Browser läuft, ab Safari 15.

## Ein Spiel in C++ schreiben

```cpp
#include "kern/kern.h"
using namespace kern;

class MyGame final : public App {
 public:
  RendererOptions configure() override { RendererOptions o; o.shadows = true; return o; }

  bool start(Engine& engine) override {
    Renderer& r = engine.renderer;
    box_ = r.allocate_id();
    MeshData box = primitives::box_on_ground({2, 2, 2}, {0.9f, 0.6f, 0.3f});
    MeshInput in = box.as_input();
    in.material.roughness = 0.5f;
    return r.upsert(box_, in);
  }

  void update(Engine& engine, const Frame& frame) override {
    if (engine.input.down(Key::left)) yaw_ += frame.dt;
    engine.renderer.camera.eye = {std::cos(yaw_) * 12, std::sin(yaw_) * 12, 6};
    engine.renderer.camera.target = {0, 0, 1};
    engine.set_status("Hallo Kern");
  }

 private:
  u32 box_ = 0;
  f32 yaw_ = 0;
};

App* kern_create_app() { static MyGame game; return &game; }
```

Im Browser:

```html
<canvas id="game" style="width:100%;height:100%"></canvas>
<script type="module">
  import {runKernApp} from "./kern/web/app.mjs";
  runKernApp({canvas: document.getElementById("game"), url: "./mygame.wasm", onStatus: t => console.log(t)});
</script>
```

`update()` läuft einmal pro Bildschirm-Frame, danach rendert die Engine. Die Engine übernimmt Drawing-Buffer-Größe und DPR, Tastatur, Maus, Touch, Fokusverlust, Kontextverlust und Wiederherstellung, Pointer-Lock (`engine.request_pointer_lock`), Startparameter (`engine.arg("time")` aus der Query) und den Statuskanal (`engine.set_status`).

## Den Renderer aus JavaScript nutzen (wie SpawnCity)

```js
import {loadKern, KernRenderer} from "./kern/web/kern.mjs";
const kern = await loadKern({url: "./module.wasm"});        // oder {base64}, {bytes}
const r = new KernRenderer(canvas, {shadows: {resolution: 512, extent: 96}}, kern);
r.upsert("car", {positions, normals, colors, uvs, indices, instances: {transforms, tints}, material: {roughness: .4}});
r.updateInstances("car", {transforms});   // false = unverändert, kein Upload
r.setCamera({eye, target, fov});
const stats = r.render();                 // drawCalls, triangles, culledMeshes, shadowReused, cpuMs, gpuMs, bloom
const hit = r.pick(x, y).hit;             // exaktes Dreieck-Picking (BVH für statische Meshes)
```

Die Fassade kopiert Typed Arrays in einen wiederverwendeten Staging-Bereich im WebAssembly-Speicher und ruft die C-ABI auf. Im Frame selbst entstehen keine Allokationen. Die Wortoffsets der Deskriptoren stehen in `kern/include/kern/api/c_api.h` und müssen mit `kern/web/kern.mjs` übereinstimmen.

## Renderer

- **Ein Programm für alles:** Der Standard-Shader macht Instancing (Modell- und Normalenmatrix plus Tint pro Instanz), GPU-Skinning (32 Knochen), PBR im Stil von MeshStandardMaterial (Lambert-Sonne, Hemisphäre, Himmels-Irradianz, Schlick/GGX), Glas-Himmelsspiegelung, 6 Punktlichter, Alpha-Mask und -Blend, Textur, Unlit, Emissive, ACES-Tonemapping, Farbgrading und FogExp2 nach sRGB.
- **Frame:** Sonnenschatten → Opak (vorne nach hinten) → Himmel (nur unbedeckte Pixel) → Transparent (hinten nach vorne nach Weltbounds) → Viewmodel-Ebene nach einem Depth-Clear → Bloom-Composite.
- **Sonnenschatten:** Der Fokus wird auf das Texelraster im Lichtraum eingerastet. Dadurch flimmert nichts, und die Map wird wiederverwendet, bis sich ein Schattenwerfer, die Sonne oder die gerasterte Fokuszelle ändert.
- **Zustandsfilter:** `UniformCache` lässt unveränderte `uniform*`-Aufrufe aus. Textur- und Cull-Zustand werden pro Frame verfolgt.
- **Instanzpuffer:** Sie wachsen geometrisch. Unveränderte Instanzen werden erkannt und nicht hochgeladen.
- **Robustheit:** Ein ungültiges `upsert` lässt das vorhandene Mesh unangetastet, weil vor jeder Änderung validiert wird. Bei Kontextverlust pausiert der Renderer. Bei der Wiederherstellung baut er alle Programme neu und lädt Meshes und Engine-Texturen aus den CPU-Kopien neu hoch. Host-Texturen stellt die Plattformschicht im selben Handle-Slot wieder her. Scheitert die Wiederherstellung, bleibt der Renderer pausiert, bis ein späterer Versuch gelingt.
- **Eingaben aus dem Host:** Alle f64-Ströme der Geometrie-Jobs werden geprüft (Zähler endlich, ganzzahlig, im Rahmen der echten Länge), Optionsvektoren gegen ihre übergebene Länge. Mesh-Ids gehören dem, der sie mit `allocate_id()` geholt hat; `upsert` auf fremde oder freigegebene Ids schlägt fehl.
- **Instanzen:** Leere Transforms bei `update_instances` bedeuten „Instanzen behalten“ (reines Tint-Update, nur ein Puffer wird hochgeladen, die Schattenkarte bleibt gültig). Gespiegelte Instanzen (negative Determinante) werden mit `cullFace(FRONT)` gezeichnet, gemischte ohne Culling, auch im Schattenpass.

### Shader-Erweiterungen

Spiele fügen eigene Looks hinzu, ohne die Engine anzufassen. Eine `ShaderExtension` liefert GLSL-Schnipsel für feste Hook-Punkte (`vertex`, `fragment_discard`, `fragment_input`, `fragment_surface` …), optional bis zu zwei eigene Vertex-Streams (Locations 14/15) und ganzzahlige Materialparameter. Beispiele:

- `games/spawncity/spawncity_look.cpp`: SpawnCity-Fassaden, Fensterraster, Dachziegel, Bodenrauschen, Lackierung, Innenraum-Cutouts
- `NightWindows` in `kern/examples/orbrun/orbrun.cpp`: beleuchtete Fenster in der Nacht aus den Wand-Records des Gebäudebauers

## Geometrie

- `Earcut`: C++-Port von Mapbox Earcut v3.0.2 mit identischen Ergebnissen (Paritätstest gegen die JS-Version)
- `BuildingBuilder`: Grundrisse (mit Innenhöfen) → Dächer, Wände, Gesimse, in Chunks gruppiert, inkrementell mit Zeitbudget
- `RoadBuilder`: Straßenbänder mit Bordstein und gestrichelter Mittellinie, über eine `HeightSampler`-Funktion auf das Terrain gelegt
- `build_terrain_tile`: Terrain-Kachel aus Höhenknoten
- `weld`: verlustfreies Indexieren über Bitmuster (Hash-Tabelle statt String-Schlüssel)
- `primitives`: box, sphere, cylinder, cone, plane, append

Auf dem echten Konstanz-Snapshot (1300 Gebäude) braucht Kern für Bau und Welding etwa 45 ms. Die vorherige JavaScript-Variante brauchte etwa 1260 ms, also rund 28-mal länger.

## Physik, Spiel-Laufzeit und Studio

- **`kern::physics`** kapselt Box3D: `World` mit Körpern, Shapes (Box, Kugel, Kapsel, konvexe Hülle), allen Gelenktypen mit Limits, Motoren, Federn und Bruchschwellen, Kontakt-, Sensor- und Gelenk-Ereignissen, Kontaktpunkten, Achsensperren, Kräften und Posen; `cast_ray` mit Maske und ignoriertem Körper (Sensoren zählen nicht); `MouseGrab` (Box3D-MotorJoint); `hull_mesh`.
- **`kern::game`** ist die Spiel-Laufzeit: `Level` aus Actors mit Komponenten (Static Mesh, Point Light, Rigid Body, Collider, Constraint, Player Controller, Pickup, Trigger, Rotator, Mover, Procedural City), gespeichert als `.klevel`. `plan_bodies` macht daraus Box3D-Körper; Kinder ohne eigenen Body werden als Compound angeschweißt. `GameWorld` stellt das Level dar und spielt es: Controller (Hovercar, Rolling, Character, Flyer, Vehicle), Chase-Kamera, Pickups, Trigger, Respawn, Zeitlimit, Siegbedingungen, Tag und Nacht, Verkehr. Demo-Levels: Orb Run, Physics Lab.
- **`kern::studio::Studio`** ist der Editor: Dokument und Historie, Auswahl, Viewports mit Unreal-Navigation, Gizmos, Overlays, Details-Schema, Katalog, Menüs, Dialoge, Play/Simulate. Die Seite spricht ihn über `kern_studio_*` mit JSON an. Steuerung, Befehlsreferenz und Format: [`docs/studio.md`](docs/studio.md).

Der Renderer hat dafür Linien-Materialien (`Material.lines`), Ebenenmasken (`MeshInput.layers`, `RenderParams.layers`, `PickQuery.layers`), `RenderParams.sky`, Viewport-Rechtecke, orthografische Kameras (`Camera.orthographic`), die Viewmodel-Ebene für Gizmos, `TextureDesc.nearest` und die Primitive `capsule` und `torus` bekommen.

## Tests

- `kern/tests/run.sh`: Unit-Tests für Mathe, BVH gegen Brute Force, Earcut, Welding, Builder, Primitive, Renderer gegen eine aufzeichnende GL-Attrappe (Validierung, Instanz-Kapazität, Uniform-Cache, Schatten-Wiederverwendung, Kontextverlust, Linien und Ebenen), App-Eingabe, Box3D-Schicht, JSON, Levels, Body-Planung, Stadt, GameWorld mit Orb Run und Physics Lab, Spielregeln sowie Kern Studio (Navigation, Gizmos, Katalog, Befehle, Outliner, Play, Speichern)
- `kern/tests/e2e/studio_e2e.mjs`: Browser-Test (Playwright) von Studio (Single-File und Modul), Fly Mode, Panels, Content Drawer, Play, Simulate, Layouts, Vorlagen und dem exportierten Spiel
- `tests/kern_renderer_facade_test.mjs`: das echte Modul unter Node mit WebGL2-Attrappe
- `tests/kern_geo_parity_test.mjs`: C++-Geometrie gegen die frühere JS-Implementierung. Gebäude sind bitgenau gleich, Straßen weichen um 0 ulp ab, Terrain um höchstens 1 ulp.
- `tests/native_*_proof*.mjs`: echte Browser-Proofs (Szene, komponierte Welt mit Kontextverlust)

## Lizenzen

Siehe `kern/LICENSES.md` (Earcut: ISC, Mapbox; Box3D: MIT, Erin Catto).
