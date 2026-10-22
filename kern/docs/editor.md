# Kern Editor – Box3D-Szenen- und Spiele-Editor

Der Kern Editor ist die Oberfläche von `kurzlernen.de/editor3d.html` (R.U.B.E-artiger 3D-Physik-Editor), neu gebaut auf der Kern-Engine. Alles, was rechnet, ist C++ und läuft als ein WebAssembly-Modul:

- **Szenenmodell** (`.e3d` v2), Undo/Redo, Auswahl, Transformationen, Validierung
- **Physik**: Box3D von Erin Catto, als C17 in dasselbe Modul kompiliert (keine JavaScript-Bindings)
- **Darstellung**: der Kern-Renderer auf WebGL2 (ein Canvas, ein Viewport pro Ansicht)
- **Player** und **Spiel-Laufzeit**: unabhängige Box3D-Welten mit Spielerkontrolle, Pickups, Gefahren und Zielen
- **Picking**, Bildebenen, Sampler, Kantenfilter und das Nachzeichnen von Alpha-Masken in Fixtures

Die Seite (`kern/web/editor/`) zeichnet nur Panels aus JSON, das die Engine liefert, und leitet Eingaben weiter. Sie enthält keine Editierlogik.

## Inhalt

1. [Starten und bauen](#starten-und-bauen)
2. [Bedienung](#bedienung)
3. [Spiele bauen](#spiele-bauen)
4. [Spiele exportieren und in C++ einbetten](#spiele-exportieren-und-in-c-einbetten)
5. [Architektur](#architektur)
6. [Seitenprotokoll (C-ABI und Befehle)](#seitenprotokoll)
7. [Dateiformat `.e3d` v2](#dateiformat-e3d-v2)
8. [C++-API](#c-api)
9. [Tests](#tests)
10. [Grenzen und bewusste Abweichungen](#grenzen-und-bewusste-abweichungen)

## Starten und bauen

```sh
export WASI_SDK=/opt/wasi-sdk-25.0-x86_64-linux

# Engine + Box3D + Editor -> generated/kern_editor.wasm, dann alles in eine Datei
tools/build_editor.sh                  # -> generated/editor3d.html (≈2,6 MB, läuft offline und über file://)
tools/build_editor.sh --skip-wasm      # nur neu verpacken (nach Änderungen an HTML/CSS/JS)

# Nur das Modul
kern/build.sh --editor generated/kern_editor.wasm
```

Entwicklungsseite ohne Verpacken: das Repository statisch ausliefern (zum Beispiel `python3 -m http.server 8771`) und `kern/web/editor/index.html` öffnen. Sie lädt `generated/kern_editor.wasm` und die Module `kern/web/kern.mjs`, `kern/web/editor/player.mjs` und `kern/web/editor/editor.mjs` direkt.

| Datei | Aufgabe |
|---|---|
| `kern/web/editor/index.html` | Markup: Menüleiste, Werkzeugleiste, Docks, `<canvas id="gl">`, Statusleiste |
| `kern/web/editor/editor.css` | Stile der ursprünglichen Editor3D-Oberfläche plus Mobile-Ergänzungen |
| `kern/web/editor/editor.mjs` | Seiten-Host: Panels, Menüs, Dialoge, Eingabe, Dateien, Autosave, Export |
| `kern/web/editor/player.mjs` | `EditorEngine` (JS-Hülle der C-ABI), `startPlayer` (Vollbild-Spiel), `buildGameHtml` |
| `kern/web/editor/player.html` | Entwicklungs-Host für Spiele (`player.html?scene=level.e3d`) |
| `tools/build_editor.sh` | Single-File-Build |

Das Single-File-Build führt die drei Modulquellen als ein einziges Inline-Modul aus (Import-Zeilen und `export` entfernt, keine `blob:`-Importe, daher auch unter strengen Content-Security-Policies lauffähig). `kern.mjs` und `player.mjs` liegen zusätzlich als Text in der Seite; daraus schreibt der Editor beim Spiel-Export das Spiel.

## Bedienung

Die Oberfläche entspricht dem Original: Menüleiste, Werkzeugleiste, links *Items*, *Editor context help* und *Undo stack*, rechts *Properties* und *Help*, unten das *Message log*. Panels lassen sich über ihre Titelleiste herauslösen (▣) und an den linken, rechten oder unteren Rand zurückziehen; die Trenner verändern die Dockbreiten. *Window → Reset panel layout* stellt alles zurück, **F11** blendet alle Panels aus.

### Modi

| Taste | Modus | Auswahl |
|---|---|---|
| B | Body | Körper |
| F | Fixture | Formen eines Körpers (Box, Kugel, Kapsel, konvexe Hülle) |
| V | Vertex | Hüllenpunkte |
| J | Joint | Gelenke |
| I | Image | Bildebenen |
| M | Sampler | Sampler-Volumen |
| W | World | Weltparameter und Spiel-Einstellungen |

Linksklick wählt, Linksziehen zieht ein Auswahlrechteck, **Shift** fügt hinzu, **Ctrl** schaltet um. Rechte Maustaste kreist, mittlere verschiebt, das Mausrad zoomt. Auf Touch-Geräten schaltet der Knopf unten rechts zwischen *Orbit* (ein Finger kreist, zwei Finger zoomen/verschieben) und *Select*/*Grab* um.

### Transformationen

| Taste | Wirkung |
|---|---|
| T oder G | Verschieben |
| R / S | Drehen / Skalieren |
| X / Y / Z | Achse festlegen |
| Zahlen, `.`, `-`, Backspace | exakter Wert (zum Beispiel `G X 2 Enter`) |
| Ctrl / Shift | Rasten / Feinsteuerung |
| Enter oder Klick | bestätigen |
| Esc | abbrechen |
| C | 3D-Cursor verschieben; **C → S** Cursor auf Auswahl, **C → C** exakter Cursor-Dialog |
| E (Vertex-Modus) | Hüllenpunkt am Cursor hinzufügen |
| T / R wiederholt (Joint-Modus) | Anker A → Anker B → beide bzw. Achse |
| L (Joint-Modus) | untere / obere Gelenkgrenze |
| Shift+D | Duplizieren und verschieben |
| Entf | Löschen |

Der erste Mauszug nach dem Start einer Transformation legt den Bezugspunkt fest, weitere Bewegungen wenden sie an. (Im ursprünglichen JavaScript startete die mausgeführte Transformation wegen eines verriegelten `operation.start` nie; hier funktioniert sie.)

### Menüs

- **File**: New, Open, Save, Save as, Export JSON (validiert vorher), Export view as PNG, **Export game (HTML)**, Add image, Beispiele (*Suspension & vehicle*, *Newton cradle*, *3D hulls & stacking*, *Joint laboratory*, *Game: Roll & Collect*), Close view
- **Edit**: Undo, Redo, Copy, Paste, Duplicate, Delete, Select all, Deselect all
- **View**: Fit scene/selection, Anzeigeoptionen (Grid, Labels, Shapes, Joints, Images, Centers, Wireframe, Fixture colors)
- **Scene**: Translate, Rotate, Scale, Move cursor, Set cursor, Edit joint limits, Actions (wie **Leertaste**)
- **Window**: Open player view, Clone view, Tile views, Panels ein/aus, Reset panel layout
- **Tools**: Validate scene, Collision settings, Custom properties, Options
- **Help**: Keyboard reference (**F1**), About / format support, Box3D repository

Das **Aktionsmenü** (Leertaste) enthält *Add* (statische, kinematische, dynamische Körper, Box/Kugel/Kapsel-Fixtures, Hüllen, alle Gelenktypen, Bild, Sampler), *Make static/dynamic*, *Make chain*, *Weld together*, *Create fixtures from alpha*, Sampler-Ausgaben (PNG, Sobel, lokale Maxima, Fixtures), *Box fixture from volume*, *Duplicate*, *Delete*, *Cursor to selection*.

### Properties

Das Properties-Panel zeigt die Felder der Auswahl; bei Mehrfachauswahl erscheinen abweichende Werte als „— multiple —“, eine Änderung gilt für alle. Jede Änderung ist ein Undo-Schritt. Es gibt Felder für:

- **Körper**: Name, Typ, Bullet, Sleep, Awake, Active, Dämpfung, Gravity scale, Position, Rotation (Euler in Grad), Geschwindigkeiten, **Game** (Rolle, Steuerung, Kraft, Sprung, Kamera folgt, Punkte)
- **Fixtures**: Form, Zentrum, Rotation, Größe/Radius/Länge, Dichte, Reibung, Restitution, Rollwiderstand, Sensor, Kollisionsfilter, Farbe
- **Gelenke**: Typ, Körper A/B, Anker, Achse, Grenzen, Motor, Feder, Längen, Kräfte
- **Bilder und Sampler**: Körper, Zentrum, Größe, Deckkraft, Spiegeln, Filter, Tönung, Auflösung, Schwellwert, Toleranz
- **Welt**: Gravitation, Schritte pro Sekunde, Substeps, Sleep, Continuous physics, Restitution-Parameter, **Game** (Titel, Siegbedingung, Zeitlimit, Kameraabstand/-höhe, Absturzgrenze)

Doppelklick auf einen Eintrag im Items-Baum benennt ihn um. Filterfeld und Typauswahl filtern den Baum.

### Ansichten und Player

Jede Szene hat einen Tab. **Ctrl+Shift+C** klont die Ansicht, **Ctrl+T** kachelt alle Ansichten. **Ctrl+R** (oder *▶ Run*) öffnet einen **Player**: eine unabhängige Box3D-Kopie der Szene. Im Player zieht die linke Maustaste dynamische Körper über ein echtes Box3D-MotorJoint; Pause, Reload, Einzelschritt und Geschwindigkeit (0,1× bis 4×) sitzen in der Leiste der Ansicht. Höchstens 31 Ansichten sind gleichzeitig offen (eine Renderer-Ebene pro Ansicht); an der Grenze schließt der Editor die älteste Ansicht, deren Schließen nichts kostet, und meldet das im Log.

### Bilder, Sampler und Nachzeichnen

PNG, JPEG, WebP und GIF (bis 15 MB) lassen sich über *Add image* oder per Drag & Drop laden. Die Seite dekodiert sie (längste Seite höchstens 2048 px) und übergibt RGBA an die Engine. *Create fixtures from alpha* zerlegt die deckenden Pixel in zusammengefasste Rechtecke und legt sie als Box-Fixtures an. Ein **Sampler** ist ein Volumen, das alle Bildebenen in seinem Bereich zu einem Bild komponiert; daraus entstehen das gesampelte PNG, ein Sobel-Kantenbild, lokale Maxima oder Fixtures.

### Dateien und Autosave

*Save* schreibt `.e3d` v2, *Export JSON* eine validierte JSON-Fassung. Geöffnet werden `.e3d`, `.json` und alte Editor3D-Dateien (v1 und die frühere Objektliste werden migriert). Der Arbeitsbereich wird laufend in `localStorage` (`kern-editor.workspace.v1`) gesichert und beim nächsten Start wiederhergestellt; ein Autosave des Originaleditors (`editor3d.workspace.v2`) wird übernommen.

## Spiele bauen

Jede Szene kann ein Spiel sein. Die Spiel-Komponenten sind optionale Felder im `.e3d`-Dokument und werden in den Properties bearbeitet.

**Körper → Game**

| Feld | Werte | Bedeutung |
|---|---|---|
| Role | none, player, goal, pickup, hazard | Spieler, Ziel, Sammelobjekt, Gefahr |
| Control | none, roll, drive, fly, walk | Steuerungsart des Spielers |
| Control force | m/s² je Eingabeeinheit | Stärke der Steuerung |
| Jump speed | m/s (0 = kein Sprung) | Sprung, nur am Boden |
| Camera follows | an/aus | Kamera folgt diesem Körper (Standard: Spieler) |
| Pickup points | Zahl | Punkte beim Einsammeln |

Die Steuerungsarten:

- **roll**: Kraft relativ zur Kamera (Murmel, Kugel)
- **drive**: Vorwärts/rückwärts entlang der lokalen X-Achse, Lenken als Drehmoment um Z (Fahrzeug)
- **fly**: hebt die Schwerkraft auf, bewegt in alle Richtungen, Space/Shift für hoch/runter, gedämpft
- **walk**: setzt die horizontale Geschwindigkeit über Impulse auf das Ziel, ohne Kippen (Figur)

Den Bodenkontakt für Sprünge prüft ein Box3D-Raycast nach unten, dessen Länge sich aus der Reichweite der Fixtures ergibt; Sensoren und der Spieler selbst werden ignoriert.

**Welt → Game**

| Feld | Bedeutung |
|---|---|
| Title | Titel im HUD |
| Win condition | none, reachGoal (Ziel berühren), collectAll (alles einsammeln; gibt es ein Ziel, danach das Ziel erreichen), survive (bis zum Zeitlimit überleben) |
| Time limit | Sekunden, 0 = keins |
| Camera distance / height | Verfolgerkamera |
| Fall limit (z) | unterhalb dieser Höhe ist das Spiel verloren |

Ein Spiel läuft, sobald die Szene einen Körper mit der Rolle *player* hat. Im Player gilt dann: **WASD/Pfeiltasten** bewegen relativ zur Kamera, **Leertaste** springt (bei *fly*: hoch), **Shift/Q** runter, **R** startet neu, **P** pausiert. Pickups und Ziele sind sinnvollerweise Sensoren; eingesammelte Pickups werden aus der Box3D-Welt entfernt. Gefahren beenden das Spiel bei Berührung. Das HUD zeigt Punkte, Fortschritt und Zeit, am Ende eine Meldung mit „Play again“.

Ein Beispiel liegt unter *File → Examples → Game: Roll & Collect*.

## Spiele exportieren und in C++ einbetten

**File → Export game (HTML)** prüft die Szene und schreibt eine einzelne HTML-Datei. Sie enthält das Engine-Modul (base64), `kern.mjs` und `player.mjs` als ein Inline-Modul und die Szene; sie braucht kein Netz und läuft auch über `file://`. Auf Touch-Geräten erscheinen ein Joystick, ein Sprungknopf und ein Neustartknopf.

Dasselbe Spiel lässt sich ohne Editor-Code direkt in C++ spielen: `kern/examples/scenegame/scenegame.cpp` ist ein vollständiges `kern::App`, das eine `.e3d`-Szene mit `world::SceneRuntime` ausführt und mit `world::SceneMeshes` zeichnet:

```sh
kern/build.sh --app --world kern/examples/scenegame/scenegame.wasm kern/examples/scenegame/scenegame.cpp
# kern/examples/scenegame/index.html?file=level.e3d   (ohne ?file: eingebautes Level aus C++-Code)
```

```cpp
world::Scene scene;
std::string error;
if (!world::from_text(text, scene, &error)) return kern::fail("%s", error.c_str());
world::SceneRuntime runtime;
runtime.build(scene);                       // Box3D-Welt + Spiel-Komponenten
world::SceneMeshes meshes;
meshes.build(engine.renderer, scene, {});   // ein Mesh pro Fixture
// pro Frame:
runtime.set_input(input);                   // world::GameInput: move_x/move_y/up/jump/camera_forward
runtime.advance(frame.dt);                  // feste Schritte (Schritte/s aus der Szene)
meshes.update(engine.renderer, runtime);    // Posen aus Box3D übernehmen
const world::GameState& g = runtime.game(); // status, score, pickups, elapsed, message
```

## Architektur

```
Browser-Seite (kern/web/editor)                 WebAssembly-Modul (C++)
───────────────────────────────                 ──────────────────────────────────────────
editor.mjs  Panels, Menüs, Dialoge  ── JSON ──▶ kern_editor_* (src/editor/editor_api.cpp)
            Zeiger, Tastatur, Dateien            └─ kern::editor::Editor
            Bild-Dekodierung, Autosave              ├─ Dokumente, Historie, Auswahl (editor.cpp)
player.mjs  EditorEngine, startPlayer               ├─ Ansichten, Kamera, Picking, Zeiger (views.cpp)
kern.mjs    WebGL2-Importe, Speicher                ├─ Properties (fields.cpp), Menüs/Dialoge/Befehle (ui.cpp)
                                                    ├─ Bilder, Sampler, Nachzeichnen (images.cpp)
                                                    └─ Beispielszenen (demos.cpp)
                                                 kern::world   Szene (.e3d), SceneRuntime, Spiel, SceneMeshes
                                                 kern::physics World, Shapes, Joints, Raycast, MouseGrab
                                                 Box3D (third_party/box3d, C17)
                                                 kern::Renderer (WebGL2, Schatten, Picking, Linien)
```

**Ein Canvas, viele Ansichten.** Alle Ansichten zeichnen in dasselbe `<canvas id="gl">`. Die Seite meldet pro Frame das Rechteck jeder Ansicht in Drawing-Buffer-Pixeln (Ursprung unten links) mit `kern_editor_set_view_rect`; die Engine rendert jede sichtbare Ansicht mit eigenem Viewport. Jede Ansicht besitzt ein Renderer-Ebenenbit (0–30), ihre Meshes tragen nur dieses Bit; Bit 31 ist das gemeinsame Gitter. So teilen sich Editor- und Player-Ansichten einen Renderer, ohne die Meshes der anderen zu sehen.

**Renderer-Ergänzungen für den Editor:** `Material.lines` (GL_LINES, unbeleuchtet, ohne Schatten, nie gepickt) für Gelenke, Gitter und Auswahlrahmen; `MeshInput.layers`, `RenderParams.layers`, `PickQuery.layers`; `RenderParams.sky`; `TextureDesc.nearest`; die Primitive `capsule` und `torus`. Der Schatten-Cache berücksichtigt die Ebenenmaske.

**Physik.** `kern::physics::World` kapselt Box3D (Körper, Shapes inklusive Hüllen, alle Gelenke, Kontakt- und Sensor-Ereignisse, Raycast mit Maske und ignoriertem Körper). `world::SceneRuntime` baut daraus eine Szene, rechnet in festen Schritten (höchstens 240 pro Aufruf) und führt die Spiel-Komponenten aus. Die Gelenk-Abbildung entspricht der `||`/`??`-Semantik des ursprünglichen JavaScript.

**Zahlen.** Das Szenenmodell rechnet in `double` mit three.js-Semantik (`world/dmath.h`), damit gespeicherte Dateien zu denen des Originals passen. JSON-Zahlen werden nach dem ECMAScript-Algorithmus formatiert.

## Seitenprotokoll

### C-ABI (`kern/src/editor/editor_api.cpp`)

Zeiger-Rückgaben zeigen auf UTF-8-JSON (oder Text) im Modulspeicher; gültig bis zum nächsten Aufruf. Die Länge liefert `kern_editor_result_length()`.

| Funktion | Zweck |
|---|---|
| `kern_editor_init(w, h, player_only)` | Renderer (Schatten 2048, ohne Bloom/Grading) und Editor anlegen; `player_only=1` für exportierte Spiele |
| `kern_editor_resize(w, h)` | Drawing-Buffer-Größe |
| `kern_editor_set_view_rect(view, x, y, w, h, css_w, css_h, dpr, visible)` | Rechteck einer Ansicht |
| `kern_editor_set_mobile(on)` | Touch-Standards (Orbit) |
| `kern_editor_frame(now_ms)` | Physik vorrücken, Meshes abgleichen, alle Ansichten rendern |
| `kern_editor_command(json, len)` | Befehl ausführen → `{ok, error?, ui?, …}` |
| `kern_editor_state()` | vollständiger UI-Zustand (Tabs, Ansichten, Baum, Properties, Historie, Kontexthilfe, Optionen, UI-Aktionen) |
| `kern_editor_status()` | flüchtiger Zustand pro Frame (Modus, Koordinaten, Meldung, Hinweise, Labels, Auswahlrechteck, HUD, `grabbing`) |
| `kern_editor_ui_revision()` | ändert sich, wenn `state()` neu gelesen werden muss |
| `kern_editor_pointer(view, type, x, y, button, buttons, id, kind, mods, wheel)` | type 0 down, 1 move, 2 up, 3 cancel, 4 wheel, 5 leave; kind 0 Maus, 1 Touch, 2 Stift; mods 1 Shift, 2 Ctrl, 4 Alt |
| `kern_editor_key(key, len, code, len, down, mods, repeat)` | Tastatur → `{handled, ui?}` |
| `kern_editor_blur()` | Fokusverlust: Operation abbrechen, Griffe und Spieleingaben lösen |
| `kern_editor_pending_images()`, `kern_editor_image_source(key)`, `kern_editor_image_pixels(key, w, h, rgba)` | Bilder dekodieren lassen |
| `kern_editor_take_autosave(now_ms)` | fälliger Autosave-Text oder 0 |
| `kern_editor_any_dirty()` | ungesicherte Änderungen |
| `kern_editor_log(since)` | Log-Zeilen ab Index |
| `kern_editor_pixels()`, `kern_editor_pixels_length()` | RGBA der letzten Sampler-Ausgabe |
| `kern_editor_context_lost()`, `kern_editor_context_restored()` | WebGL-Kontextverlust |

### Befehle (`{"cmd": …}`)

| Gruppe | Befehle |
|---|---|
| Dokumente | `newScene`, `openScene`, `openImage`, `saveScene`, `exportJson`, `exportView`, `exportGame`, `loadDemo {kind}`, `loadDocument {text, name}`, `restoreWorkspace {text}`, `closeView {view, force}` |
| Bearbeiten | `undo`, `redo`, `historyJump {index}`, `copy`, `paste`, `duplicate`, `delete`, `selectAll`, `deselectAll`, `select {ids, mode, shift, toggle}`, `rename {id, name}`, `setProperty {path, value}` |
| Erzeugen | `addBody {type, shape}`, `addFixture {shape}`, `addJoint {type}`, `addSampler`, `addHullVertex`, `addImageData {name, src, width, height}`, `makeStatic`, `makeDynamic`, `weld`, `traceImage`, `samplerAction {action}`, `samplerToFixture` |
| Transformieren | `setMode {mode}`, `beginOperation {kind}`, `setAxis {axis}`, `typeOperation {text}`, `finishOperation {accept}`, `setCursor {x,y,z}`, `cursorToSelection` |
| Ansichten | `activateView`, `cloneView`, `toggleTile`, `fitScene`, `fitSelection`, `fitNewViews`, `toggleOption {name}`, `setTouchOrbit {view, on}`, `treeToggle {id}`, `setTreeFilter {query, kind}` |
| Player/Spiel | `openPlayer`, `reloadPlayer`, `stepPlayer`, `togglePlay`, `setSpeed {speed}`, `gameInput {view, moveX, moveY, up, jump}`, `restartGame {view}` |
| UI | `menu {name}`, `dialog {name, id}`, `dialogSubmit {name, values, button}`, `ui {action, …}`, `report {message, type}`, `clearLog` |

Dialoge: `chain`, `cursor`, `saveAs`, `options`, `custom`, `collision`, `validation`, `keyboard`, `about`, `closeScene`, `rename`. Ein Dialog ist eine Liste von Blöcken (`p`, `field`, `table`, `list`, `properties`, `bits`) plus Knöpfen (`close`, `cmd`, `submit`).

UI-Aktionen, die die Seite ausführt: `openFile`, `download`, `downloadPixels`, `dialog`, `popup`, `clipboard`, `togglePanels`, `togglePanel`, `resetPanels`, `openUrl`, `closeMenu`, `exportView`, `exportGame`.

Im Browser ist das Protokoll als `window.KernEditor` erreichbar (`command`, `state`, `status`, `key`, `pointer`, `exportGameHtml`, `views`), in exportierten Spielen als `window.KernPlayer`.

## Dateiformat `.e3d` v2

```json
{
  "format": "editor3d", "version": 2, "name": "Roll & Collect",
  "id": "…",
  "world": {"gravity": [0, 0, -9.80665], "stepsPerSecond": 60, "subStepCount": 4, "allowSleep": true, "continuousPhysics": true,
            "restitutionThreshold": 1, "restitutionIterations": 2, "restitutionPropagation": false},
  "game": {"title": "…", "goal": "collectAll", "timeLimit": 120, "cameraDistance": 8, "cameraHeight": 4, "fallLimit": -20},
  "cursor": [0, 0, 0],
  "bodies": [{
    "id": "…", "name": "player ball", "type": 2, "position": [0, 0, 1], "rotation": [0, 0, 0, 1],
    "linearVelocity": [0, 0, 0], "angularVelocity": [0, 0, 0], "linearDamping": 0, "angularDamping": 0, "gravityScale": 1,
    "allowSleep": true, "awake": true, "active": true, "bullet": false,
    "game": {"role": "player", "control": "roll", "force": 14, "jump": 6, "cameraFollow": true, "points": 1},
    "fixtures": [{"id": "…", "name": "ball", "shape": "sphere", "center": [0, 0, 0], "rotation": [0, 0, 0, 1],
                  "size": [1, 1, 1], "radius": 0.5, "length": 1, "vertices": [],
                  "density": 1, "friction": 0.9, "restitution": 0.1, "rollingResistance": 0.02, "sensor": false,
                  "categoryBits": 1, "maskBits": 4294967295, "groupIndex": 0, "color": "#e8b04a",
                  "customProperties": [], "extra": {}}],
    "customProperties": [], "extra": {}
  }],
  "joints": [{"type": "revolute", "bodyA": "…", "bodyB": "…", "anchorA": […], "anchorB": […], "axisA": […], …}],
  "images": [{"body": "…", "center": […], "size": [w, h], "src": "data:image/png;base64,…", "opacity": 1, …}],
  "samplers": [{"center": […], "size": […], "resolution": 200, "threshold": 0.5, "tolerance": 1, …}],
  "extra": {}
}
```

- Körpertyp 0 statisch, 1 kinematisch, 2 dynamisch; Rotationen sind Quaternionen `[x, y, z, w]`; Z zeigt nach oben.
- Formen: `box` (`size`), `sphere` (`radius`), `capsule` (`radius`, `length`, entlang Z), `hull` (`vertices`).
- Gelenke: `revolute`, `distance`, `prismatic`, `wheel`, `rope`, `weld`, `friction`, `motor`, `spherical`, `parallel`, `filter` (Box3D-Gelenktypen; `rope` ist ein Distance-Joint mit Maximallänge) mit den Feldern aus der Properties-Liste.
- `game` (Szene und Körper) fehlt, solange es Standardwerte hat; ältere Leser ignorieren es.
- Unbekannte Schlüssel bleiben beim Laden und Speichern erhalten, `extra` und `customProperties` werden durchgereicht.
- Version 1 und die frühere Objektliste werden beim Öffnen migriert (Hinweis im Log).

## C++-API

| Header | Inhalt |
|---|---|
| `kern/physics/physics.h` | `World` (Körper, Shapes, Gelenke, `step`, Ereignisse, Posen, Kräfte), `cast_ray`, `MouseGrab`, `hull_mesh`, `version()` |
| `kern/core/json.h` | `json::Value` (Schlüsselreihenfolge bleibt), `parse` (Tiefenlimit, unsichere Schlüssel abgelehnt), `stringify` |
| `kern/world/scene.h` | Szenenmodell, `to_json`/`from_text`, `normalize`, `validate`, `new_scene/body/fixture`, `make_uid` |
| `kern/world/runtime.h` | `SceneRuntime`, `GameInput`, `GameState`, `joint_settings`, `shape_settings`, `body_settings` |
| `kern/world/scene_meshes.h` | `fixture_geometry`, `SceneMeshes`, `pose_matrix`, Farben |
| `kern/editor/editor.h` | `kern::editor::Editor` – die komplette Editorlogik ohne Browser, direkt testbar |

Build-Komponenten von `kern/build.sh`: `--physics` (Box3D + `kern::physics`), `--world` (zusätzlich Szenen und Laufzeit), `--editor` (zusätzlich Editor und `kern_editor_*`), `--app` (App-Laufzeit für Spiele in C++). Box3D wird als C17 kompiliert (`B3_SIMD_NONE` auf wasm, ein Worker); die Objekte werden in `$KERN_OBJ_CACHE` zwischengespeichert.

## Tests

- `kern/tests/run.sh` (nativ, mit Sanitizern): Physik, JSON, Szenen-Rundlauf und Migration, Laufzeit und Spielregeln (Pickups, Ziel, Gefahr, Sprung, Absturz), Editor gegen die GL-Attrappe (Befehle, Undo, Transformationen, Zeiger und Picking, Tastatur, Properties, Dialoge, Bilder und Sampler, Beispiele, Ansichtslimit, `gameInput`/`restartGame`)
- `kern/tests/e2e/editor_e2e.mjs` (Playwright, Chromium): Single-File-Editor, Menüs, Aktionsmenü, `G X 2 Enter`, Undo, Properties, Dialoge, **alle Menüeinträge** in vier Beispielszenen, alle Dialoge öffnen und absenden, alle Formen und Gelenktypen, Bildimport und Nachzeichnen, Kacheln, Player mit Maus-Griff, Spiel mit Tastatur, Export und Start des exportierten Spiels über `file://` inklusive Joystick, Speichern/Öffnen, Autosave, Mobile-Ansicht, `player.html` und das C++-Beispiel `scenegame` mit einer vom Editor gespeicherten Szene

```sh
python3 -m http.server 8771 &
node kern/tests/e2e/editor_e2e.mjs http://127.0.0.1:8771
```

Die CI (`renderer-compare.yml`, Job `kern-editor`) baut Editor und Beispiel und führt den Browser-Test aus.

## Grenzen und bewusste Abweichungen

- Höchstens 31 offene Ansichten (Renderer-Ebenen), siehe oben.
- Das Wireframe-Overlay zeichnet Kanten als Linien-Mesh; Linienbreite ist in WebGL2 immer 1 Pixel.
- Box3D läuft mit einem Worker und ohne SIMD (`B3_SIMD_NONE`), damit das Modul auf jedem WebGL2-Browser läuft.
- Bilder werden im Browser dekodiert (die Engine bekommt RGBA); die längste Seite ist auf 2048 px begrenzt.
- Der Player ist absichtlich unabhängig vom Editorzustand; Änderungen werden mit *Reload* (Ctrl+R) übernommen.
