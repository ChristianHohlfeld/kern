# 12 Eigene Spiele schreiben

Kern bietet drei Wege zu einem eigenen Spiel. Sie schließen sich nicht aus, sondern teilen sich dieselbe
Engine: denselben Renderer, dieselbe Physik, dieselbe Plattformschicht.

| Weg | Werkzeug | Ergebnis | Geeignet für |
|---|---|---|---|
| **A** Im Studio ohne Code | Kern Studio, Logik-Graphen | eine HTML-Datei | Levels mit Spielern, Physik, Pickups, Triggern, Ton, Logik; schnelle Prototypen; Unterricht |
| **B** In C++ mit der App-API | `kern::App`, `kern/build.sh --app`, `kern/web/app.mjs` | ein `.wasm` plus eine kleine HTML-Seite | Spiele mit eigener Simulation, eigener Darstellung, eigenen Shadern |
| **C** Aus JavaScript über die C-ABI | `kern/web/kern.mjs`, `kern/web/studio/game.mjs` | eigene Seite, die das Modul steuert | bestehende JavaScript-Projekte (wie SpawnCity), Studio-Levels in eigene Seiten einbetten |

Dazu kommt eine Mischform: Logik-Graphen aus Studio lassen sich als C++ exportieren und in ein Modul
bauen ([12.4.3](#1243-logik-graphen-als-c)).

Pfade beziehen sich auf das Wurzelverzeichnis des Repositorys; die Engine liegt dort im Ordner `kern/`.

---

## 12.1 Weg A: ein Spiel im Studio, ohne Code

Dieser Abschnitt baut Schritt für Schritt ein kleines Sammelspiel: Eine Spielfigur läuft über eine Fläche
mit Hindernissen, sammelt Orbs ein, muss einer Gefahrenzone ausweichen und gewinnt, wenn alles eingesammelt
ist. Die Bedienung der einzelnen Panels steht in [Kapitel 11](11-studio.md).

### Schritt 1: Ein neues Level

1. Studio öffnen (https://christianhohlfeld.github.io/kern/editor/ oder `generated/studio.html`).
2. **File › New Level...**, Vorlage **Basic**, **Create**. Das Level hat einen Boden (40 × 40 m), Sonne,
   Himmel und einen Würfel.
3. In **World Settings** den **Level name** und den **Game title** setzen, etwa „Orb Garden".

Warum Basic und nicht Empty: Empty hat gar nichts, also auch keinen Boden, auf dem eine Figur stehen kann.
Die World Settings haben zwar einen **Endless ground**; ein sichtbarer Boden mit eigener Farbe und Kollision
ist für den Anfang aber anschaulicher.

### Schritt 2: Die Spielfigur

1. Content Drawer öffnen (**Ctrl+Space**), Gruppe **Players**, **Character** in den Viewport ziehen.
2. Das Log meldet „Character is the player now". Im Outliner trägt der Actor das Abzeichen **PLAYER**.
3. In **Details › Player Controller** lassen sich Tempo (**Max speed**), **Acceleration**, **Jump speed**
   und die Kamera (**Chase**, **First person**, **Top down**) einstellen.

Ein Druck auf **Play** (Alt+P) testet sofort: W A S D läuft, die Maus schaut, die Leertaste springt,
**Esc** kehrt zurück. Jeder andere Eintrag aus **Players** funktioniert genauso; der zuletzt platzierte ist
der Spieler, oder man wählt ihn mit Rechtsklick › **Make Player**.

### Schritt 3: Hindernisse und Physik

1. Aus **Shapes** ein paar **Cube**, eine **Ramp** und eine **Plane** platzieren. Mit **W** (Move),
   **E** (Rotate) und **R** (Scale) und dem Gizmo zurechtrücken; das Grid-Raster (0,5 m) hält die Kanten
   sauber. **End** setzt die Auswahl auf den Boden.
2. Aus **Physics** einen **Crate Stack** und eine **Hinged Door** dazu. Diese Actors sind dynamisch bzw.
   hängen an Gelenken und reagieren auf die Spielfigur.
3. Aus **Gameplay** eine **Moving Platform** und einen **Spinner**: Beide bewegen sich kinematisch.
4. Mit **Simulate** (Alt+S) prüfen, ob alles steht, wie es soll. In der Simulation kann man Körper mit der
   Maus greifen und werfen. **K** übernimmt die simulierte Lage der ausgewählten Actors ins Level, etwa
   für einen Kistenstapel, der natürlich zusammengefallen aussehen soll.

![Ausgewählter Actor mit Gizmo und seinen Komponenten in Details](bilder/studio-selected.png)

Ein Würfel aus **Shapes** hat Kollision, bewegt sich aber nicht. Soll er fallen, bekommt er in Details
mit **+ Add › Rigid Body** und **Mobility = Dynamic** eine Box3D-Physik; oder man nimmt gleich die
**Physics Cube**. Was die Physikfelder bedeuten, erklärt [Kapitel 6](06-physik.md).

### Schritt 4: Etwas zum Einsammeln

1. Aus **Gameplay** mehrere **Orb** (1 Punkt) und einen **Star** (10 Punkte) verteilen. Ein Orb lässt
   sich mit **Alt + Gizmo ziehen** schnell vervielfältigen: Jeder Zug zieht eine Kopie.
2. Alle Orbs auswählen (Rahmenauswahl oder Ctrl+Klick im Outliner) und mit dem Ordner-Knopf im Outliner in
   einen Ordner legen. Das hält den Outliner übersichtlich.
3. In **World Settings › Win condition** **Collect all pickups** wählen. Wer alle Pickups hat, gewinnt.

### Schritt 5: Gefahr, Checkpoint und Zeitlimit

1. Ein **Hazard** (Trigger, der das Spiel verliert) oder eine **Damage Zone** platzieren und mit Scale auf
   die gewünschte Größe ziehen.
2. Einen **Checkpoint** setzen: Fällt die Figur unter **Kill Z** (World Settings) oder verliert sie, startet
   sie hier neu.
3. Optional **Time limit** in World Settings: Das HUD zählt herunter.

Andere Ziele: **Reach a goal trigger** (mit einem **Goal**), **Survive the time limit** oder **Reach a
target score** (mit **Target score**).

### Schritt 6: Logik ohne Programmieren

Viele Spiele brauchen nichts weiter als die Bausteine oben. Für eigenes Verhalten gibt es Logik-Graphen:

1. Einen **Message Trigger** platzieren und in Details › Trigger die **Action** auf
   **Logic event only** stellen.
2. In Details auf **Logic** klicken. Der Logik-Editor öffnet den Graphen dieses Actors.
3. **Templates › Score zone (overlap +10)**: Betritt jemand die Zone, gibt es 10 Punkte, eine Meldung
   „+10" und einen Ton.
4. **Compile** (F7) prüft den Graphen, **Viewport** führt zurück.

Weitere Vorlagen: eine Tür, die auf **Interact** (Taste F) öffnet, ein Spawner, eine Ampel als State
Machine und ein Verhalten für die Micro Duck. Alle Knoten erklärt [Kapitel 9](09-logik.md).

### Schritt 7: Ton

1. In **World Settings › Audio** eine **Music** und eine **Ambience** wählen.
2. Aus der Gruppe **Audio** des Content Drawers etwa **Ambient Wind** platzieren, oder im Tab **Sounds**
   eine eigene Datei importieren und in den Viewport ziehen: Es entsteht eine Audio Source an dieser Stelle.

Die Spieler-Controller, Pickups und Trigger bringen ihre Geräusche bereits mit ([Kapitel 7](07-audio.md)).

### Schritt 8: Testen

- **Play** (Alt+P): spielen. **F8** verlässt die Figur, die Welt läuft weiter, man kann sich umsehen;
  noch einmal F8 übernimmt sie wieder.
- **Play › Number of Players › 2 Players** testet das Level mit zwei Spielern in zwei Viewports.
- Fehler im Level (etwa ein Gelenk, das auf einen gelöschten Actor zeigt) verhindern Play und stehen im
  **Output Log**.

![Play im Viewport mit HUD](bilder/studio-play.png)

### Schritt 9: Speichern

**Ctrl+S** speichert das Level als `.klevel`. Die Datei ist JSON und enthält auch Texturen und importierte
Sounds; man kann sie weitergeben und in jedem Studio wieder öffnen (Ctrl+O oder auf die Seite ziehen).
Ungespeicherte Arbeit sichert Studio zusätzlich im Browser und bietet sie beim nächsten Start an.

### Schritt 10: Exportieren

1. **File › Package Game (HTML)...**
2. Titel prüfen, **Package**.
3. Studio lädt `Orb_Garden.html` herunter und bietet **Play it** an.

Die Datei enthält Engine, Level und Spiel-Host. Sie läuft offline, von `file://`, von jedem Webserver und
auf dem Telefon mit Touch-Sticks. Wie man sie veröffentlicht, steht in [12.6](#126-veröffentlichen).

![Ein exportiertes Spiel im Browser](bilder/game.png)

![Dasselbe Spiel auf dem Telefon: die Engine erkennt Touch und zeichnet Sticks und Knöpfe](bilder/game-phone.png)

---

## 12.2 Weg B: ein Spiel in C++ mit der App-API

Für Spiele, die mehr wollen als Actors und Komponenten, schreibt man das Spiel selbst in C++ gegen die
Engine-API. Das Spiel ist eine Klasse, die von `kern::App` erbt; die Engine ruft sie in jedem Frame auf
und rendert danach. Alles Browserspezifische erledigt die Plattformschicht `kern/web/app.mjs`.

### 12.2.1 Die Bausteine

Die API steht in `kern/include/kern/app/app.h` und wird mit `#include "kern/kern.h"` eingebunden
(zusammen mit Renderer, Geometrie, Partikeln und Mathe).

**`App`** – das Spiel:

| Methode | Wann | Zweck |
|---|---|---|
| `RendererOptions configure()` | einmal, vor `start` | Schatten, Bloom und weitere Renderer-Optionen (Breite und Höhe setzt die Engine) |
| `Span<ShaderExtension* const> extensions()` | einmal, vor `start` | eigene Shader-Erweiterungen; sie müssen die App überleben |
| `bool start(Engine&)` | einmal | Meshes anlegen, Zustand vorbereiten; `false` bricht ab |
| `void update(Engine&, const Frame&)` | jeden Bildschirm-Frame | Eingabe lesen, Spiel fortschreiben, Szene aktualisieren |
| `void resized(Engine&)` | nach Größenänderung | z. B. Layout an neue Breite anpassen |
| `void stop(Engine&)` | beim Beenden | aufräumen |

**`Engine`** – was das Spiel bekommt:

| Mitglied | Zweck |
|---|---|
| `renderer` | der Renderer ([Kapitel 4](04-grafik.md)): Meshes, Kamera, Licht, Himmel, Picking |
| `input` | Tastatur, Maus, Touch dieses Frames |
| `set_status(text)` | Text an die Seite (sie zeigt ihn an); gleiche Texte werden nicht erneut gesendet |
| `request_pointer_lock(bool)` | die Seite fängt die Maus bzw. gibt sie frei |
| `arg(key, fallback)`, `arg_number(key, fallback)` | Startparameter, standardmäßig aus der Query der Seite (`?time=0.6`) |
| `width()`, `height()`, `aspect()` | Größe des Drawing Buffers |
| `stats()` | Statistik des letzten Frames (Draw Calls, Dreiecke …) |
| `clear_color` | Löschfarbe, wo kein Himmel gezeichnet wird |

**`Input`** – Zustand der Eingabe:

| Methode | Bedeutung |
|---|---|
| `down(Key)` | Taste gehalten |
| `pressed(Key)` / `released(Key)` | in diesem Frame gedrückt / losgelassen |
| `pointer()`, `pointer_delta()` | Zeigerposition und Bewegung seit dem letzten Frame, in Drawing-Buffer-Pixeln (auch bei Pointer-Lock) |
| `button(b)`, `button_pressed(b)` | Maustaste gehalten / in diesem Frame gedrückt (Bit 0 links) |
| `wheel()` | Mausrad in diesem Frame |
| `touches()`, `touch_count()` | bis zu 10 Finger mit `id` (< 0 = frei), `start`, `position`, `previous` |
| `focused()` | Seite hat den Fokus; bei Fokusverlust lässt die Engine alle Tasten los |

`Key` kennt die Buchstaben `a` bis `z`, `digit0` bis `digit9`, die Pfeile `left`, `right`, `up`, `down`
und `space`, `enter`, `escape`, `tab`, `backspace`, `shift_left`/`_right`, `control_left`/`_right`,
`alt_left`/`_right`. Die Seite bildet sie über `KeyboardEvent.code` ab, also nach der Tastenposition.

**`Frame`**: `time` (Sekunden seit Start), `dt` (Sekunden seit dem letzten Frame, höchstens 0,1) und
`index`.

### 12.2.2 Der Ablauf eines Frames

```
Seite (app.mjs)                       Modul (src/app/app.cpp)
---------------------------------------------------------------------------
kern_app_set_args("?time=0.6")   ->   Engine::set_args
kern_app_start(w, h)             ->   kern_create_app(), configure(), Renderer::init, start()
Tastatur / Maus / Touch          ->   kern_app_key / _pointer / _wheel / _touch / _focus
requestAnimationFrame:
  kern_app_frame(now, hidden)    ->   update(engine, frame); input.end_frame(); render()
ResizeObserver                   ->   kern_app_resize(w, h) -> resized()
Kontextverlust                   ->   kern_context_lost / kern_context_restored
runKernApp(...).stop()           ->   kern_app_stop() -> stop(), Renderer::shutdown
```

`update` läuft genau einmal pro Bildschirm-Frame, danach rendert die Engine. `pressed`, `released`,
`pointer_delta` und `wheel` gelten nur für diesen einen Frame; `end_frame` setzt sie zurück. Das ist der
Grund, warum man sie in `update` liest und nicht zwischenspeichert.

### 12.2.3 `examples/hello` Zeile für Zeile

Das kleinste Kern-Spiel (`kern/examples/hello/hello.cpp`, 83 Zeilen): ein Feld aus 64 instanzierten,
schattenwerfenden Kisten, die sich drehen, eine Kamera, die man mit der Maus oder den Pfeiltasten um sie
kreisen lässt, und Picking per Klick.

```cpp
#include <cstdio>
#include <vector>

#include "kern/kern.h"

using namespace kern;
```

`kern/kern.h` bindet die ganze öffentliche API ein, auch `kern/app/app.h`.

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
```

`configure` legt den Renderer fest, bevor es ihn gibt: Sonnenschatten mit einer 1024er-Karte, die 40 m
abdeckt. Breite und Höhe trägt die Engine selbst ein.

```cpp
  bool start(Engine& engine) override {
    Renderer& r = engine.renderer;
    ground_ = r.allocate_id();
    MeshInput g = ground_mesh_.as_input();
    g.material.color = {0.42f, 0.47f, 0.40f, 1};
    if (!r.upsert(ground_, g)) return false;
```

Jedes Mesh braucht eine Id, die man mit `allocate_id()` holt; sie gehört dann diesem Spiel. `ground_mesh_`
ist ein `MeshData` (Positionen, Normalen, Farben, UVs, Indizes), `as_input()` macht daraus eine
geliehene Sicht `MeshInput`, an der man das Material setzt. `upsert` legt das Mesh an oder ersetzt es.
Ein ungültiges `upsert` lässt ein vorhandenes Mesh unangetastet und liefert `false`; deshalb bricht
`start` dann ab.

```cpp
    boxes_ = r.allocate_id();
    layout(0);
    MeshInput b = box_mesh_.as_input();
    b.material.roughness = 0.45f;
    b.transforms = Span<const f32>(transforms_.data(), transforms_.size());
    b.tints = Span<const f32>(tints_.data(), tints_.size());
    return r.upsert(boxes_, b);
  }
```

Die 64 Kisten sind **ein** Mesh mit 64 Instanzen: `transforms` enthält 16 Floats (eine 4×4-Matrix) pro
Instanz, `tints` 4 Floats (RGBA) pro Instanz. Ein Draw Call zeichnet alle. `layout(0)` füllt die Matrizen
vorher.

```cpp
  void update(Engine& engine, const Frame& frame) override {
    Input& in = engine.input;
    if (in.button(0)) yaw_ -= in.pointer_delta().x * 0.005f;
    if (in.down(Key::left)) yaw_ += frame.dt * 1.5f;
    if (in.down(Key::right)) yaw_ -= frame.dt * 1.5f;
    distance_ = clamp(distance_ * (1 + in.wheel() * 0.001f), 8.0f, 80.0f);
```

Gehaltene linke Maustaste dreht um die Bewegung des Zeigers, die Pfeiltasten drehen mit 1,5 rad/s
(mit `frame.dt` unabhängig von der Bildrate), das Mausrad ändert den Abstand zwischen 8 und 80 m.

```cpp
    Camera& c = engine.renderer.camera;
    c.target = {0, 0, 1};
    c.eye = {std::cos(yaw_) * distance_, std::sin(yaw_) * distance_, distance_ * 0.6f};
```

Die Kamera ist ein Feld des Renderers: Augpunkt, Ziel (und Sichtfeld, Near, Far). Z zeigt nach oben.

```cpp
    layout(static_cast<f32>(frame.time));
    engine.renderer.update_instances(boxes_, Span<const f32>(transforms_.data(), transforms_.size()),
                                     Span<const f32>(tints_.data(), tints_.size()));
```

Jede Kiste dreht sich mit eigenem Tempo; `update_instances` lädt die neuen Matrizen in einem Rutsch hoch.
Unveränderte Instanzen erkennt der Renderer und lädt sie nicht erneut.

```cpp
    if (in.button_pressed(0)) {
      PickHit hit;
      if (engine.renderer.pick(in.pointer().x, in.pointer().y, {}, hit) && hit.mesh == boxes_) {
        tints_[hit.instance * 4 + 0] = 1.0f; tints_[hit.instance * 4 + 1] = 0.25f; tints_[hit.instance * 4 + 2] = 0.2f;
      }
    }
```

Ein Klick schießt einen Strahl durch den Pixel (`pick` arbeitet in Drawing-Buffer-Pixeln, genau wie
`pointer()`). Trifft er das Kisten-Mesh, nennt `hit.instance` die getroffene Kiste, und ihr Tint wird rot.

```cpp
    char text[96];
    std::snprintf(text, sizeof text, "Kern hello - %u draws, %.0f triangles", engine.stats().draw_calls, engine.stats().triangles);
    engine.set_status(text);
  }
```

Der Statuskanal bringt Text auf die Seite; `hello/index.html` zeigt ihn unten links.

```cpp
 private:
  void layout(f32 time) {
    for (u32 i = 0; i < kCount; ++i) {
      const f32 angle = time * (0.5f + (i % 7) * 0.15f);
      const Vec3 p{(static_cast<f32>(i % kSide) - kSide * 0.5f) * 3, (static_cast<f32>(i / kSide) - kSide * 0.5f) * 3, 0};
      const Mat4 m = Mat4::trs(p, Quat::axis_angle({0, 0, 1}, angle), {1, 1, 1 + (i % 3) * 0.6f});
      std::memcpy(&transforms_[i * 16], m.m, sizeof m.m);
    }
  }
```

Ein 8×8-Raster im Abstand von 3 m. `Mat4::trs` baut die Matrix aus Translation, Rotation (Quaternion um Z)
und Skalierung; jede dritte Kiste ist höher. Die Matrix ist spaltenweise gespeichert und wird direkt in
den Instanzpuffer kopiert.

```cpp
  static constexpr u32 kSide = 8, kCount = kSide * kSide;
  MeshData ground_mesh_ = primitives::plane(60, 60);
  MeshData box_mesh_ = primitives::box_on_ground({1.4f, 1.4f, 1.4f}, {0.95f, 0.62f, 0.32f});
  std::vector<f32> transforms_ = std::vector<f32>(kCount * 16, 0.0f);
  std::vector<f32> tints_ = std::vector<f32>(kCount * 4, 1.0f);
  u32 ground_ = 0, boxes_ = 0;
  f32 yaw_ = 0.6f, distance_ = 30;
};
```

`primitives::plane` und `primitives::box_on_ground` erzeugen Geometrie (die Kiste steht mit der Unterseite
auf z = 0, ihre Vertexfarbe ist orange). Weiße Tints lassen diese Farbe unverändert.

```cpp
App* kern_create_app() {
  static Hello app;
  return &app;
}
```

Diese eine freie Funktion muss jedes Spiel definieren. Die Laufzeit ruft sie beim Start genau einmal auf.
Das statische Objekt lebt so lange wie das Modul.

Die Seite dazu (`kern/examples/hello/index.html`) ist kurz:

```html
<canvas id="game"></canvas>
<div id="status">loading...</div>
<script type="module">
  import {runKernApp} from "../../web/app.mjs";
  const status = document.getElementById("status");
  runKernApp({canvas: document.getElementById("game"), url: "./hello.wasm", onStatus: text => (status.textContent = text)})
    .then(app => (window.__kernApp = app))
    .catch(error => { status.textContent = String(error); window.__kernError = String(error); });
</script>
```

### 12.2.4 Bauen

Man braucht [wasi-sdk](https://github.com/WebAssembly/wasi-sdk) Release 25; die CI lädt es von GitHub.

```sh
export WASI_SDK=/opt/wasi-sdk-25.0-x86_64-linux
kern/build.sh --app kern/examples/hello/hello.wasm kern/examples/hello/hello.cpp
```

`kern/build.sh [Komponenten] <out.wasm> [Quellen und Compiler-Flags]`:

| Option | Was dazukommt |
|---|---|
| (immer) | Renderer, Geometrie, JSON, Kern-Mathe |
| `--app` | die App-Laufzeit (`kern/src/app`) für Spiele gegen `kern::App` |
| `--physics` | Box3D und die Schicht `kern/src/physics` |
| `--game` | Levels, Komponenten, Spiel-Laufzeit, prozedurale Stadt; schließt `--physics` ein |
| `--studio` | Kern Studio und seine C-ABI; schließt `--game` ein |
| `--lehrer` | der Lehrer für den Browser; schließt `--game` ein |

Argumente, die auf `.cpp` oder `.cc` enden, sind Quellen des Spiels; alle anderen gehen als Flags an den
Compiler (etwa `-DMEIN_SCHALTER` oder `-Imein/include`). Mehrere Quelldateien sind erlaubt.

| Umgebungsvariable | Bedeutung |
|---|---|
| `WASI_SDK` | Installationsordner von wasi-sdk (Standard `/opt/wasi-sdk`) |
| `KERN_BUILD=debug` | `-O1 -g` statt `-O3` mit ThinLTO |
| `KERN_JOBS` | parallele Compiler-Aufrufe (Standard: alle Kerne) |
| `KERN_OBJ_CACHE` | Objekt-Cache (Standard `$TMPDIR/kern-obj`) |

Jede Quelle wird zu einem eigenen Objekt kompiliert und unter einem Hash aus Flags, Quelle und allen
Headern zwischengespeichert. Ein erneuter Build übersetzt nur, was sich geändert hat, und Studio und die
Beispielspiele teilen sich die Objekte der Engine. Das Modul ist ein WASI-„Reactor" mit konservativem
Feature-Satz, damit es in jedem WebGL2-fähigen Browser ab Safari 15 läuft. Ausführlich:
[Kapitel 2](02-bauen-und-testen.md).

### 12.2.5 `examples/orbrun`: ein vollständiges Spiel

`kern/examples/orbrun/orbrun.cpp` (726 Zeilen) ist ein komplettes Rennspiel nur gegen die Engine-API: Ein
Hovercar rast durch eine prozedurale Stadt und sammelt 24 Energie-Orbs, während der Tag in die Nacht
übergeht. Es zeigt Gebäude- und Straßenbauer, Instancing, Schatten, Himmel, Bloom, Punktlichter, Partikel,
eine eigene Shader-Erweiterung, Tastatur- und Touch-Eingabe und den Statuskanal.

Nicht verwechseln: Das Level **Orb Run** in Studio ist ein anderes Programm mit derselben Idee. Es besteht
aus Actors und Komponenten (Procedural City, Box3D-Hovercar, Pickups) und läuft auf der Spiel-Laufzeit.
Das C++-Beispiel macht alles selbst, auch Fahrphysik und Kollision.

**Steuerung:** W/S oder Pfeile fahren, A/D lenken, Leertaste Boost, N schnelle Zeit, Enter neue Stadt.
Touch: linke Bildschirmhälfte lenkt und gibt Gas (Ausschlag ab dem Aufsetzpunkt), rechte Hälfte boostet.
`?time=0.6` in der Adresse startet nachts.

**Aufbau der Klasse `OrbRun`:**

```cpp
  void update(Engine& engine, const Frame& frame) override {
    Renderer& r = engine.renderer;
    Input& in = engine.input;
    const f32 dt = frame.dt;
    if (in.pressed(Key::enter)) new_run(engine, rng_.next());
    if (in.pressed(Key::n)) fast_time_ = !fast_time_;
    elapsed_ += dt;
    now_ms_ = frame.time * 1000.0;
    if (!finished_) run_time_ += dt;

    drive(in, dt);
    update_traffic(dt);
    collect(engine);
    update_daylight(r, dt);
    update_camera(r, in, dt);
    upload_dynamic(r);
    update_status(engine);
  }
```

Jeder Frame liest die Eingabe, schreibt die Simulation fort und lädt danach die veränderlichen Daten zur
GPU. Die Teile im Einzelnen:

**Die Stadt** (`class City`, `new_run`). Ein Raster aus 7 × 7 Blöcken mit 80 m Abstand und 16 m breiten
Straßen. Jeder Block bekommt zufällig (aus einem Seed, `Rng` ist ein Xorshift) eine von vier Bebauungen:
ein blockgroßes Gebäude (in 60 % der Fälle mit Innenhof als zweitem Ring), vier Parzellen, ein L-förmiges
Gebäude mit Anbau oder drei Riegel. Zur Mitte hin werden die Häuser höher. Die Grundrisse gehen als
`FootprintStream` an den `BuildingBuilder` der Engine, die Straßen als `RoadStream` an den `RoadBuilder`.
Beide liefern Chunks, die `weld` indexiert und `upload_static` als Meshes anlegt:

```cpp
    BuildingBuilder buildings;
    if (!buildings.begin(footprints.data(), footprints.size(), bo)) return false;
    while (!buildings.step(1e9)) {}
    for (GeoChunk& chunk : buildings.chunks()) {
      weld(chunk.mesh);
      if (!upload_static(r, chunk.mesh, Material{}, 0)) return false;
    }
```

`step(budget)` arbeitet eigentlich in Zeitscheiben; mit `1e9` baut das Beispiel alles auf einmal. Die
Bäume sind zwei instanzierte Meshes (Stamm und Krone) mit einer Matrix pro Baum. Wie die Bauer arbeiten,
steht in [Kapitel 5](05-szene-und-geometrie.md).

**Kollision ohne Physik-Engine.** Das Beispiel nutzt bewusst kein Box3D. Jedes Gebäude ist ein Polygon in
einem 40-m-Gitter; `City::collide` schiebt den Kreis des Autos (Radius 1,7 m) aus allen Polygonen der
Nachbarzellen heraus und spiegelt die Geschwindigkeit an der Wand. Der Stadtrand ist eine feste Grenze.

**Fahrphysik** (`drive`). Gas beschleunigt mit 24 m/s² vorwärts, Rückwärtsgas (Taste S: 0,7 × 30) mit 21 m/s², Boost mit
zusätzlich 42 m/s²; die Höchstgeschwindigkeit ist 36 m/s, mit Boost 60 m/s. Seitliche Geschwindigkeit
klingt mit `exp(-7.5 dt)` ab: Das Auto rutscht kurz und fängt sich. Lenken wirkt erst mit Tempo („Grip"
steigt bis 9 m/s auf voll) und kehrt sich beim Rückwärtsfahren um. Boost leert sich mit 0,45 pro Sekunde
und lädt mit 0,12 nach. Damit das Auto beim Boost nicht durch Wände tunnelt, bewegt es sich in
Teilschritten von höchstens 0,6 m und prüft jeden davon.

**Verkehr** (`update_traffic`). 22 Autos fahren auf den Straßenmittellinien in Spuren 2,8 m neben der
Mitte, mit 9 bis 15 m/s. Steht der Spieler weniger als 10 m vor einem Auto in seiner Spur, hält es an.
Am Stadtrand tauchen die Autos auf der anderen Seite wieder auf. Alle Autos sind ein instanziertes Mesh
mit einer Farbe pro Instanz.

**Orbs und Partikel** (`collect`). Kommt das Auto einem Orb näher als 3,2 m, verschwindet er und sprüht
28 Funken über das Partikelsystem der Engine (`Particles`, sortiert, kameragerichtet, ein Draw Call). Mit
dem letzten Orb endet der Lauf, und die Bestzeit wird festgehalten.

**Tag und Nacht** (`update_daylight`). Die Tageszeit läuft in 480 s einmal herum, mit **N** in 40 s.
Aus ihr folgen Sonnenrichtung, Mondrichtung und ein Nachtanteil; zwischen Tag-, Dämmerungs- und Mondlicht
wird gemischt, ebenso Himmel-, Boden- und Nebelfarbe, Umgebungslicht und Belichtung:

```cpp
    const f32 phase = time_of_day_ * 2 * kPi;
    const Vec3 sun = normalize({std::cos(phase) * 0.62f, -0.38f, std::sin(phase)});
    const Vec3 moon = normalize({-sun.x, 0.3f, max_of(0.35f, -sun.z)});
    const f32 night = smoothstep(0.06f, -0.16f, sun.z);
    ...
    l.sun_direction = sun.z > -0.04f ? sun : moon;
    l.sun_color = mix3(mix3(day_sun, dusk_sun, dusk), moon_light, night);
    ...
    r.sky.sun = sun;
    r.sky.moon = moon;
    r.sky.night = night;
    windows_.night = night;
```

**Lichter.** Scheinwerferglanz vor dem Auto und ein Licht pro Orb, nachts stärker. Der `LightPool` nimmt
alle Wünsche entgegen und gibt dem Renderer die sechs wichtigsten; der Standard-Shader kennt sechs
Punktlichter.

**Kamera** (`update_camera`). Eine Verfolgerkamera, die mit dem Tempo zurückweicht, weich nachgeführt
(`damp3`) und bei Zusammenstößen kurz wackelt; das Sichtfeld öffnet sich über 30 m/s. Das Mausrad ändert
den Abstand.

**Die Shader-Erweiterung `NightWindows`.** Das ist das Lehrstück des Beispiels: Ein Spiel fügt dem
Standard-Shader einen eigenen Look hinzu, ohne die Engine anzufassen. Der Gebäudebauer schreibt für jeden
Wand-Vertex einen Datensatz mit vier Werten (Wandlänge, Höhe, Seed, Geschosshöhe). Die Erweiterung
deklariert dafür einen Vertex-Stream und baut daraus ein Fensterraster, dessen Fenster nachts leuchten:

```cpp
class NightWindows final : public ShaderExtension {
 public:
  f32 night = 0;
  const char* name() const override { return "orbrun-night-windows"; }
  Span<const ExtensionAttribute> attributes() const override { return {kAttributes, 1}; }
  const char* vertex_declarations() const override { return "flat out vec4 vWall;\n"; }
  const char* vertex() const override { return "  vWall = aWall;\n"; }
  const char* fragment_declarations() const override { ... "uniform float uNight;" ... }
  const char* fragment_surface() const override { ... Fensterraster, emissive += ... * uNight ... }
  void bind(const Program& p) override { u_night_ = p.location("uNight"); }
  void frame_uniforms(UniformCache& cache, const Renderer&) override { cache.set_float(u_night_, night); }

 private:
  static constexpr ExtensionAttribute kAttributes[] = {{"wall", 4, "aWall"}};
  u32 u_night_ = 0;
};
```

| Teil | Aufgabe |
|---|---|
| `attributes()` | ein zusätzlicher Vertex-Stream `aWall` mit 4 Floats |
| `vertex_declarations()` / `vertex()` | gibt den Datensatz als `flat` Varying weiter; `flat`, weil er pro Wand konstant ist und Interpolation den Fenster-Hash zerstören würde |
| `fragment_declarations()` | das Varying, das Uniform `uNight` und eine Hash-Funktion |
| `fragment_surface()` | auf senkrechten Wänden (`abs(n.z) < 0.5`): Fensterraster aus UV und Geschosshöhe, dunkles Glas, per Hash beleuchtete Fenster als `emissive`, mal `uNight`; in der Ferne wird das Raster zu einem mittleren Schimmer ausgeblendet, damit nichts flimmert |
| `bind()` | holt die Uniform-Position nach dem Linken des Programms |
| `frame_uniforms()` | setzt `uNight` einmal pro Frame; der `UniformCache` sendet nur Änderungen |

Angemeldet wird die Erweiterung über `extensions()`:

```cpp
  Span<ShaderExtension* const> extensions() override { return {extensions_, 1}; }
  ...
  NightWindows windows_;
  ShaderExtension* extensions_[1] = {&windows_};
```

Welche Meshes den Stream bekommen, entscheidet `MeshData::as_input(extra_slot)`: Gebäude geben ihre
Zusatzdaten in Slot 0 (den Stream `aWall`), Straßen und Grundplatten übergeben `kMaxExtensionAttributes`
und damit keinen. Alle Hook-Punkte und Regeln für Erweiterungen stehen in [Kapitel 4](04-grafik.md).

**Status.** `update_status` schreibt jede Frame eine Zeile wie
`ORBS 5/24 | 01:12.4 | BOOST [######----] | 87 km/h | best --:--.-`; nach dem letzten Orb
`RUN COMPLETE … | Enter: new city`. `set_status` sendet nur, wenn sich der Text geändert hat.

**Startparameter.** `engine.arg_number("time", 0.455)` liest `?time=` aus der Adresse; so startet
`index.html?time=0.6` nachts.

Gebaut wird es wie hello:

```sh
kern/build.sh --app kern/examples/orbrun/orbrun.wasm kern/examples/orbrun/orbrun.cpp
```

### 12.2.6 Ein eigenes Spiel anlegen

1. Einen Ordner anlegen, etwa `meinspiel/`, mit `meinspiel.cpp` (eine Klasse, die von `App` erbt, und
   `kern_create_app()`) und `index.html` (wie bei hello, mit dem Pfad zu `app.mjs`).
2. Bauen: `kern/build.sh --app meinspiel/meinspiel.wasm meinspiel/meinspiel.cpp`. Wer Box3D direkt nutzen
   will, nimmt `--physics` dazu ([Kapitel 6](06-physik.md)); wer Levels und die Spiel-Laufzeit braucht,
   `--game` ([Kapitel 8](08-spiel-laufzeit.md)).
3. Den Ordner zusammen mit `kern/web/kern.mjs` und `kern/web/app.mjs` über einen Webserver ausliefern
   ([12.5](#125-hosten-mit-webappmjs)).

---

## 12.3 Die Plattformschicht `web/app.mjs`

`kern/web/app.mjs` ist der Host für C++-Spiele. Er besitzt nur, was der Browser in JavaScript erzwingt:
den WebGL2-Kontext, den Frame-Callback, die Größe des Drawing Buffers und die Eingabe-Ereignisse, die er an
das Modul weitergibt.

```js
import {runKernApp} from "./kern/web/app.mjs";
const app = await runKernApp({
  canvas: document.getElementById("game"),
  url: "./meinspiel.wasm",
  onStatus: text => (status.textContent = text),
  onError: error => console.error(error),
});
// später: app.stop();
```

| Option | Bedeutung |
|---|---|
| `canvas` | das Canvas (Pflicht) |
| `url`, `bytes`, `base64`, `module` | Quelle des Moduls, eine davon |
| `onStatus(text)` | bekommt den Text aus `Engine::set_status` |
| `onError(error)` | Fehler beim Rendern oder bei der Wiederherstellung des Kontexts |
| `maxPixelRatio` | begrenzt die Dichte des Drawing Buffers (Standard: volle Geräteauflösung) |
| `contextAttributes` | zusätzliche Attribute für `getContext("webgl2", …)` |
| `args` | Startparameter für `Engine::arg` (Standard: `location.search`) |

Zurück kommt `{kern, stop()}`. Was der Host übernimmt:

- **Größe:** Ein `ResizeObserver` hält den Drawing Buffer auf CSS-Größe × Pixelverhältnis und ruft
  `kern_app_resize` auf.
- **Tastatur:** `keydown`/`keyup` über `KeyboardEvent.code` (Tabelle `KEY_CODES` in derselben Reihenfolge
  wie `kern::Key`); Pfeile, Leertaste und Tab verlieren ihr Standardverhalten (kein Scrollen). Wiederholte
  Tastenereignisse zählen nicht.
- **Maus:** Position und Bewegung in Drawing-Buffer-Pixeln, Tasten, Rad (die Seite scrollt nicht); kein
  Kontextmenü auf dem Canvas.
- **Touch:** Finger als `kern_app_touch` mit Phase 0 (Start), 1 (Bewegung), 2 (Ende); das Canvas hat
  `touch-action: none`.
- **Fokus:** Verliert das Fenster den Fokus, lässt die Engine alle Tasten los. So bleibt nach Alt+Tab
  keine Taste hängen.
- **Pointer-Lock:** auf Wunsch des Spiels (`request_pointer_lock`).
- **Kontextverlust:** Bei `webglcontextlost` pausiert der Renderer; bei der Wiederherstellung baut er
  Programme neu und lädt Meshes und Texturen aus seinen CPU-Kopien neu hoch.
- **Hintergrund:** Ist die Seite versteckt, erfährt der Renderer das pro Frame (`page_hidden`).

Ein Modul, das nicht mit `--app` gebaut wurde, lehnt `runKernApp` mit „module was not built with --app" ab.

---

## 12.4 Weg C: aus JavaScript und über die C-ABI

Das Modul exportiert seine Funktionen als flache C-ABI. JavaScript ruft sie direkt auf; Daten gehen über
den linearen Speicher (Zeiger und Längen). Dafür gibt es zwei fertige Fassaden.

### 12.4.1 Den Renderer aus JavaScript nutzen (wie SpawnCity)

`kern/web/kern.mjs` enthält `loadKern` und `KernRenderer`, eine JavaScript-Fassade über die Renderer-ABI
in `kern/include/kern/api/c_api.h`. So nutzt SpawnCity die Engine: Spiellogik in JavaScript, Rendern in
C++.

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

- Meshes haben hier Zeichenketten als Id; die Arrays sind `Float32Array` (Indizes `Uint16Array` oder
  `Uint32Array`).
- Die Fassade kopiert Typed Arrays in einen wiederverwendeten Staging-Bereich im WebAssembly-Speicher und
  ruft die C-ABI auf. Im Frame selbst entstehen keine Allokationen.
- Es gibt einen `KernRenderer` pro Modul.
- Die Wortoffsets der Deskriptoren stehen in `c_api.h` und müssen mit `kern.mjs` übereinstimmen.

Weitere Methoden: `resize`, `setLighting`, `setAmbientOcclusion`, `setPointLights`, `setShadows`,
`createTexture`, `deleteTexture`, `updateBones`, `setVisible`, `remove`, `getScreenRay`, `pickRay`,
`upsertTerrainTile` und die Geometrie-Jobs (`KernGeoJob`). Die Referenz steht in
[Kapitel 17](17-referenz.md), die Renderer-Konzepte in [Kapitel 4](04-grafik.md).

### 12.4.2 Studio-Levels in eine eigene Seite einbetten

Ein Level aus Studio läuft auch ohne Export in einer eigenen Seite. `kern/web/studio/game.mjs` exportiert
`startGame`, denselben Spiel-Host, den exportierte HTML-Dateien enthalten. Das Modul ist das von Studio
(`generated/kern_studio.wasm`, gebaut mit `--studio`).

`kern/web/studio/play.html` zeigt das in wenigen Zeilen; es spielt ein eingebautes Level, gewählt mit
`?level=orbRun` (Standard), `physicsLab`, `shootingRange`, `microDuck` oder `basic`:

```js
import {startGame, PLAYER_STYLE, PLAYER_BODY} from "./game.mjs";
document.getElementById("style").textContent = PLAYER_STYLE;
document.body.insertAdjacentHTML("afterbegin", PLAYER_BODY);
const level = new URLSearchParams(location.search).get("level") || "orbRun";
startGame({template: level, url: new URL("../../../generated/kern_studio.wasm", import.meta.url)});
```

Ein eigenes Level übergibt man als Text der `.klevel`-Datei:

```js
const levelText = await (await fetch("./mein-level.klevel")).text();
const game = await startGame({levelText, url: "./kern_studio.wasm"});
```

`startGame` erwartet ein Canvas mit der Id `gl` (oder `canvas:`) sowie die Elemente aus `PLAYER_BODY`
(HUD, Ladeanzeige, Hinweis, Neustart-Knopf, Fehlerfeld). Es startet das Modul im Spieler-Modus, öffnet das
Level, lädt bei Bedarf die Netze der Micro Duck, startet Play und gibt ein Objekt zurück, das auch als
`window.KernGame` bereitsteht:

| Mitglied | Zweck |
|---|---|
| `command(c)` | ein Studio-Befehl als Objekt, z. B. `{cmd: "restart"}` oder `{cmd: "pause", value: true}` |
| `status` | Werte des aktuellen Frames: HUD (`status`, `score`, `pickups`, `remaining` …), Simulation, Eingabegerät |
| `engine` | die `StudioEngine` mit allen `kern_studio_*`-Funktionen |
| `audio`, `net` | Tonausgabe und Netzwerk-Transport |

So steuert man ein Spiel aus eigenem Code, etwa mit eigenen Bildschirmknöpfen:

```js
game.command({cmd: "gameInput", moveY: 1, jump: true});   // Werte -1..1 bzw. true/false
game.command({cmd: "gameInput", moveY: 0, jump: false});
if (game.status.hud?.status === "won") showMyWinScreen();
```

`gameInput` kennt `moveX`, `moveY`, `up`, `jump`, `boost`, `fire`, `aim`, `reload`, `crouch`, `interact`,
`lookX` und `lookY`. Die Werte addieren sich zu Tastatur, Touch und Gamepad. Alle Befehle stehen in
[Kapitel 11](11-studio.md#11192-die-c-abi-kern_studio_) und [Kapitel 17](17-referenz.md).

Die Studio-C-ABI ist dieselbe, mit der Studio selbst arbeitet. Wer die Fassade nicht will, ruft
`kern_studio_init`, `kern_studio_command`, `kern_studio_frame` usw. direkt auf; `StudioEngine` in
`game.mjs` zeigt, wie Zeichenketten in den Speicher kommen (`kern_alloc`, `kern_free`) und wie JSON-Antworten
gelesen werden (`kern_studio_result_length`).

### 12.4.3 Logik-Graphen als C++

Der Logik-Editor in Studio lädt mit dem Knopf **C++** einen Graphen als lesbaren C++-Quelltext herunter.
Der Quelltext registriert sich über einen Hash am Graphen. Baut man ihn in ein Studio-Modul ein, läuft der
Graph als nativer Code statt über den Interpreter oder den JIT:

```sh
kern/build.sh --studio out.wasm logic.cpp
```

Der Dateiname kommt aus dem Download. Die drei Ausführungsstufen (Interpreter, JIT, C++) liefern dasselbe
Ergebnis; Einzelheiten in [Kapitel 9](09-logik.md).

---

## 12.5 Hosten mit `web/app.mjs`

Ein C++-Spiel besteht aus statischen Dateien:

```
meinspiel/
  index.html          importiert ../kern/web/app.mjs (relativer Pfad wie im Beispiel)
  meinspiel.wasm
kern/web/
  app.mjs
  kern.mjs            von app.mjs importiert
```

`app.mjs` importiert `./kern.mjs` aus demselben Ordner; beide müssen also nebeneinander liegen. Die
Beispiele im Repository erwarten genau diese Lage (`kern/examples/hello/index.html` importiert
`../../web/app.mjs`). Die Seite lädt das Modul per `fetch` und als ES-Modul; deshalb liefert man sie über
einen Webserver aus und öffnet sie nicht als lokale Datei. Für den eigenen Rechner reicht ein beliebiger
statischer Server im Wurzelverzeichnis des Repositorys, danach öffnet man
`/kern/examples/hello/index.html`.

Ein exportiertes Studio-Spiel braucht das alles nicht: Es ist eine Datei, die auch von `file://` läuft.

---

## 12.6 Veröffentlichen

> **Rechte:** Kern ist Open Source unter der Apache License 2.0 (`LICENSE`, `LICENSING.md`). Eigene
> Spiele und Netze dürfen verwendet, weitergegeben und verkauft werden, auch geschlossen. Ein
> exportiertes Spiel enthält die Engine; wer es weitergibt, legt die Lizenz und `NOTICE` bei (im
> exportierten HTML genügt ein Hinweis mit Link) und erhält die Urheberrechtsvermerke. Die Namen
> „Kern" und „Kern Studio" und das Logo gehören nicht zur Lizenz: „Made with Kern" ist erlaubt, ein
> eigenes Produkt „Kern" zu nennen nicht (`TRADEMARKS.md`). Bestandteile Dritter behalten ihre
> eigenen Lizenzen (`kern/LICENSES.md`, [Anhang D](D-lizenzen.md)). Die folgenden Abschnitte
> beschreiben das Technische.

### Studio-Spiele

Die exportierte HTML-Datei lädt man auf einen beliebigen Webspace, legt sie in ein Repository mit GitHub
Pages oder verschickt sie direkt. Sie läuft offline, im Vollbild, mit Tastatur, Maus, Gamepad und auf
Telefonen mit Touch-Steuerung.

- **Mit Micro Duck:** die Ordner `microduck/` (Pollens Netze aus `kern/third_party/microduck/*.onnx`) und
  `policies/` (`kern/policies/*.onnx`) neben die HTML-Datei legen, sonst fehlen alle Bewegungen außer dem
  Laufen ([Kapitel 11](11-studio.md#1114-policies-laden-jumponnx-playonnx)).
- **Online-Spiel:** Die Adresse mit `?host=<raum>` öffnet das Spiel als Host, `?join=<raum>` tritt bei.
  Für Spieler auf verschiedenen Rechnern läuft `node tools/net_relay.mjs` auf einem erreichbaren Server
  (Port 8790, Paket `ws`), und alle hängen `&relay=ws://server:8790` an. Ohne Relay finden sich nur Tabs
  desselben Browsers.

### C++-Spiele

Hochladen, was [12.5](#125-hosten-mit-webappmjs) zeigt: die HTML-Seite, das `.wasm` und `kern/web/kern.mjs`
sowie `kern/web/app.mjs` mit denselben relativen Pfaden. Kern selbst veröffentlicht so auf GitHub Pages;
die CI (`.github/workflows/ci.yml`) zeigt den vollständigen Ablauf:

```sh
tools/build_studio.sh
kern/build.sh --app kern/examples/hello/hello.wasm kern/examples/hello/hello.cpp
kern/build.sh --app kern/examples/orbrun/orbrun.wasm kern/examples/orbrun/orbrun.cpp
...
cp kern/web/kern.mjs kern/web/app.mjs _site/kern/web/
for g in hello orbrun; do
  mkdir -p _site/kern/examples/$g
  cp kern/examples/$g/index.html kern/examples/$g/$g.wasm _site/kern/examples/$g/
done
```

Studio landet dabei als `_site/editor/index.html` mit Manifest, Service Worker, Icons, `build.json` und den
Netzordnern `microduck/` und `policies/`; `kern/web/studio/` mit `play.html` und `generated/kern_studio.wasm`
kommen ebenfalls auf die Seite. Die veröffentlichte Fassung steht unter https://christianhohlfeld.github.io/kern/.

### Prüfliste vor dem Veröffentlichen

| Punkt | Studio-Spiel | C++-Spiel |
|---|---|---|
| Läuft ohne Fehler im Log bzw. in der Konsole | Output Log, Play | Browser-Konsole, `onError` |
| Startet auf dem Telefon | Export auf dem Telefon öffnen; Touch-Sticks erscheinen von selbst | Touch-Eingabe über `input.touches()` selbst auswerten |
| Ton | erster Klick oder Tipp startet ihn (Browser-Regel) | eigene Sache des Spiels |
| Dateien vollständig | eine HTML-Datei (plus Netzordner bei der Ente) | HTML, `.wasm`, `kern.mjs`, `app.mjs` |
| Auslieferung | beliebig, auch `file://` | Webserver bzw. GitHub Pages |

---

© 2026 Christian Heinrich Hohlfeld · Alle Rechte vorbehalten · [Impressum und Rechte](00-impressum.md)
