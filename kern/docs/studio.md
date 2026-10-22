# Kern Studio

Kern Studio ist der Editor der Kern-Engine. Er arbeitet wie eine Game Engine im Stil der Unreal Engine: Levels aus Actors mit Komponenten, Viewports mit Unreal-Navigation, Transform-Gizmos, Outliner und Details-Panel. Play und Simulate laufen direkt im Viewport. Rendern, Box3D-Physik, Steuerung und Spielregeln laufen im selben C++-Code, mit dem auch exportierte Spiele laufen.

- Live: https://christianhohlfeld.github.io/kern/editor/
- Bauen: `tools/build_studio.sh` → `generated/studio.html` (eine Datei, läuft offline)
- Entwickeln: Repository statisch ausliefern, `kern/web/studio/index.html` öffnen (lädt `generated/kern_studio.wasm`)

Beim Start ist **Orb Run** geladen. Das ist eine prozedurale Stadt mit Verkehr, einem Box3D-Hovercar, 24 Energie-Orbs und einem Tag, der in die Nacht übergeht. Mit **Play** (Alt+P) fährt man sofort los.

## Aufbau

```
kern/include/kern/game/   Level-Modell, Geometrie, Laufzeit, Demo-Levels
kern/src/game/            level.cpp (Modell + .klevel), geometry.cpp (Formen, Stadt),
                          runtime.cpp (GameWorld: Darstellung, Box3D, Controller, Regeln), demos.cpp
kern/include/kern/studio/ studio.h - der Editor (ohne Browser testbar)
kern/src/studio/          studio.cpp (Dokument, Historie, Auswahl, Actor-Operationen, Play)
                          viewport.cpp (Kameras, Navigation, Picking, Rendern)
                          gizmo.cpp (Move/Rotate/Scale), overlays.cpp (Grid, Outline, Icons, Debug-Linien)
                          details.cpp (Property-Schema, Details, World Settings)
                          catalog.cpp (Place Actors), ui.cpp (Tastatur, Menüs, Dialoge, Befehle)
                          api.cpp (kern_studio_* C-ABI)
kern/web/studio/          index.html, studio.css, studio.mjs (Editor-Seite), game.mjs (Spiel-Host + JS-Wrapper)
```

Die Seite enthält keine Editorlogik. Sie zeichnet die Panels aus dem JSON von `state()` und `status()`, leitet Eingaben weiter und erledigt, was nur ein Browser kann: Dateidialoge, Downloads, Zwischenablage, Pointer-Lock, Bilddekodierung, Autosave in `localStorage` und das Schreiben exportierter Spiele.

## Oberfläche

| Bereich | Inhalt |
|---|---|
| Menüleiste | File, Edit, Window, Play, Help |
| Toolbar | Speichern, Undo/Redo, **+ Add** (Place Actors), Werkzeuge Select/Move/Rotate/Scale, Welt-/lokale Achsen, Grid-, Rotations- und Skalierungsraster, Kamerageschwindigkeit, Play/Simulate/Pause/Frame/Stop/Eject, Layout, Package Game |
| Viewports | 1, 2 oder 4 (Perspektive, Top, Front, Side). Pro Viewport: Optionen, Ansichtstyp, View Mode (Lit, Unlit, Wireframe, Player Collision), Show-Flags, Maximieren |
| Outliner | Ordner, Hierarchie, Suche, Sichtbarkeit (Auge), Sperren, Drag-and-drop zum Anhängen, in Ordner verschieben oder Lösen; Doppelklick fokussiert, F2 benennt um |
| Details | Name, alle Komponenten der Auswahl (Mehrfachauswahl zeigt „Multiple“), **+ Add** für Komponenten, Suche, Zurücksetzen. Zahlenfelder lassen sich ziehen (Label oder Feld seitwärts ziehen, Shift ×10, Ctrl ×0,1) und rechnen (`2*3`) |
| World Settings | Spieltitel, Siegbedingung, Zeitlimit, Kill-Z, Tageszeit, Tageslänge, Sonne, Nebel, Belichtung, Wolken, Boden, Schwerkraft, Substeps, Schrittrate |
| Content Drawer (Ctrl+Space) | Place Actors (Klick oder Ziehen in einen Viewport), Texturen, Output Log |
| Statusleiste | Konsole (`help`), Statusmeldung, Physik-Statistik, Auswahl |

## Steuerung

| Eingabe | Wirkung |
|---|---|
| Rechte Maustaste + WASD / QE | Fly Mode; Mausrad währenddessen ändert die Geschwindigkeit (1-8), Shift schneller, Z/C zoomen |
| Linke Maustaste ziehen | vor/zurück und drehen |
| Linke + rechte Maustaste | hoch/runter und seitwärts |
| Mittlere Maustaste ziehen | schwenken |
| Alt + links / rechts / Mitte | um die Auswahl kreisen / Dolly / schwenken |
| Mausrad | vor/zurück (orthografisch: Zoom zum Cursor) |
| F / End | Auswahl fokussieren / auf den Boden setzen |
| Q W E R / Leertaste | Select / Move / Rotate / Scale / durchschalten |
| Alt + Gizmo ziehen | beim Bewegen duplizieren |
| Ctrl + Alt + ziehen | Rahmenauswahl (in orthografischen Ansichten genügt Ziehen) |
| Ctrl+Z / Ctrl+Y | Rückgängig / Wiederholen (Edit → History zeigt alle Schritte) |
| Ctrl+C / V / X / D, Entf | Kopieren, Einfügen, Ausschneiden, Duplizieren, Löschen |
| H / Shift+H / Ctrl+H | Auswahl verstecken / den Rest verstecken / alles zeigen |
| Ctrl+1..9 / 1..9 | Kamera-Lesezeichen setzen / anspringen |
| Alt+G / J / H / K | Perspektive / Top / Front / Side |
| Alt+2 / 3 / 4 / 5 | Wireframe / Unlit / Lit / Collision |
| G | Game View (Editor-Helfer aus) |
| [ / ] | Grid-Raster kleiner / größer |
| Alt+P / Alt+S | Play / Simulate |
| Esc / F8 / Shift+F1 | Stopp / Eject bzw. Possess / Maus freigeben |
| Ctrl + ziehen (Simulate) | Box3D-Körper greifen und werfen |
| K (Simulate) | simulierte Posen der Auswahl übernehmen |
| Rechtsklick | Kontextmenü: Fokus, Umbenennen, Anhängen, mit Gelenk verbinden, Sperren, Actor platzieren |

## Levels, Actors und Komponenten

Ein Level (`.klevel`, JSON, `"format": "kern-level"`, Version 1) besteht aus World Settings, Actors, Texturen und der Editor-Kamera. Einheiten sind Meter, Sekunden und Kilogramm, Z zeigt nach oben. Rotationen stehen als Euler-Winkel in Grad (x Roll, y Pitch, z Yaw), Farben als sRGB.

Ein Actor hat Name, Eltern-Actor, Outliner-Ordner, Hidden/Locked, Tags, Transform und diese Komponenten:

| Komponente | Bedeutung |
|---|---|
| Static Mesh | Box, Sphere, Cylinder, Cone, Capsule, Plane, Torus, Ramp mit PBR-Material (Farbe, Roughness, Metallic, Emissive, Opacity, Unlit, Schatten, zweiseitig, Textur mit Tiling); „Generate collision“ erzeugt einen passenden Collider |
| Point Light | Farbe, Intensität, Radius |
| Rigid Body (Box3D) | Static, Kinematic, Dynamic; Masse (0 = aus der Dichte), Dämpfung, Gravity Scale, Bullet, Sleep, Achsensperren, Startgeschwindigkeit |
| Collider (mehrere) | Fit to mesh, Box, Sphere, Capsule, Convex Hull; Offset, Rotation, Dichte, Reibung, Restitution, Rollwiderstand, Sensor, Kanäle (Kategorie/Maske/Gruppe) |
| Constraint (mehrere) | Hinge, Slider, Ball, Fixed, Distance/Rope, Wheel, Motor, Parallel, No Collision; Anker und Achse lokal, Limits, Motor, Feder, Ruhelänge, Bruchkraft und -moment |
| Player Controller | Hover Car, Rolling Ball, Character, Flyer, Vehicle; Geschwindigkeit, Beschleunigung, Wenderate, Sprung, Boost, Schwebehöhe, Kamera (Chase, First Person, Top Down) |
| Pickup | Punkte, Partikelfarbe, Respawn |
| Trigger | Goal, Hazard, Checkpoint, Message |
| Rotator / Mover | Drehung bzw. Bewegung (Sinus, Ping-Pong, Schleife) |
| Procedural City | Seed, Blöcke, Raster, Straßenbreite, Höhe, Bäume, Verkehr, beleuchtete Fenster in der Nacht |

Kind-Actors ohne eigenen Body werden beim Simulieren als Compound in den Body des nächsten Vorfahren geschweißt. Meshes mit Kollision, Pickups und Trigger ohne Body bekommen einen impliziten statischen Body, bei Animation durch Rotator oder Mover einen kinematischen. Gelenke verbinden den Actor (Body B) mit dem Ziel-Actor (Body A, 0 = Welt). Ausnahme sind Wheel-Gelenke: Dort ist das Chassis Body A.

Fehler wie verwaiste Referenzen oder ungültige Werte verhindern Play; Warnungen erscheinen im Output Log.

## Place Actors

Basic: Cube, Sphere, Cylinder, Cone, Capsule, Plane, Torus, Ramp, Empty Actor · Lights: Point Light · Physics: Physics Cube, Physics Sphere, Physics Barrel, Crate Stack, Pendulum, Hinged Door, Chain, See-Saw · Gameplay: Hover Car, Rolling Ball, Character, Drone, Orb, Star, Goal, Hazard, Checkpoint, Message Trigger, Moving Platform, Spinner · Procedural: Procedural City.

Vorlagen unter File → New Level: Orb Run, Physics Lab (alle Gelenktypen: Stapel, Pendel, Tür, Seilbrücke mit Bruchstelle, Drehteller mit Motor, Lift, Rover auf Wheel-Gelenken), Basic und Empty.

## Spiele exportieren

File → Package Game schreibt eine eigenständige HTML-Datei. Sie enthält das Engine-Modul (base64), `kern.mjs`, `game.mjs` und das Level. Das Spiel läuft im Vollbild mit HUD, Maus-Look per Pointer-Lock und Touch-Steuerung (Joystick, Sprung, Boost). Es braucht keinen Server und kein Netz.

## Protokoll

Die Seite spricht den Editor über die C-ABI in `kern/src/studio/api.cpp` an:

| Funktion | Zweck |
|---|---|
| `kern_studio_init(w, h, player_only)` | Renderer (Schatten, Bloom, NightWindows) und Studio; `player_only` für exportierte Spiele |
| `kern_studio_frame(now_ms)` | Kameras, Simulation, Overlays, Rendern aller sichtbaren Viewports |
| `kern_studio_set_viewport(i, x, y, w, h, css_w, css_h, dpr, visible)` | Viewport-Rechteck im Drawing Buffer (Ursprung unten links) |
| `kern_studio_pointer(i, type, x, y, button, buttons, mods, wheel)` | Zeiger (0 down, 1 move, 2 up, 3 cancel, 4 wheel, 5 leave) |
| `kern_studio_key(key, code, down, mods, repeat)` | Tastatur, Antwort `{"handled": bool}` |
| `kern_studio_look(dx, dy)` | Maus-Look bei Pointer-Lock |
| `kern_studio_command(json)` | Befehl, Antwort `{"ok": …}` |
| `kern_studio_state()` / `kern_studio_status()` | Panel-Zustand bzw. Werte pro Frame (HUD, Physik, Kamera, Marquee, Gizmo) |
| `kern_studio_revision()` | ändert sich, wenn `state()` neu gelesen werden muss |
| `kern_studio_take_ui()` | Aufträge an die Seite: `capturePointer`, `releasePointer`, `clipboard`, `readClipboard`, `popup`, `dialog`, `save`, `saveAs`, `open`, `importTexture`, `packageGame`, `toggleDrawer`, `toggleLog`, `fullscreen`, `rename` |
| `kern_studio_pending_textures()`, `…_texture_source(id)`, `…_texture_pixels(id, w, h, rgba)` | Texturen dekodiert die Seite |
| `kern_studio_take_autosave(now)`, `kern_studio_dirty()` | Autosave |

Befehle (`{"cmd": …}`):

- Auswahl: `select {ids, add, toggle, focus}`, `selectRange {id}`, `selectAll`, `deselect`, `selectChildren`, `selectFolder {folder}`
- Actors: `spawn {item, at | viewport+x+y}`, `delete`, `duplicate`, `copy`, `cut`, `paste`, `pasteText {text}`, `rename {id, name}`, `attach {ids, parent}`, `moveTo {ids, parent | folder}`, `setFolder {folder}`, `renameFolder {from, to}`, `group`, `snapToFloor`, `hide`/`show {ids | folder}`, `showAll`, `lock {value}`, `focus {ids}`, `connect {kind}`
- Properties: `set {path, value}` (z. B. `transform.position`, `components.mesh.material.color`, `components.joints.0.lower`), `setWorld {path, value}`, `addComponent {type}`, `removeComponent {type, index}`, `importTexture {name, src, width, height}`, `removeTexture {id}`
- Historie: `undo`, `redo`, `historyJump {index}` (aufeinanderfolgende Änderungen derselben Eigenschaft werden zu einem Schritt zusammengefasst)
- Play: `play {mode: play|simulate}`, `stop`, `pause`, `stepFrame`, `eject`, `restart`, `keep`, `gameInput {moveX, moveY, up, jump, boost}`, `pointerCaptured {value}`
- Werkzeuge und Ansichten: `tool`, `space`, `snap`, `cameraSpeed`, `layout`, `maximize`, `activeViewport`, `viewType`, `viewMode`, `showFlag`, `gameView`, `fov`, `bookmark {slot, set}`
- Panels: `filter`, `collapse {key}`, `section {id, closed}`, `menu {name}`, `dialog {name}`, `submitDialog {name, values}`, `catalog`, `clearLog`, `ui {action}`
- Dokumente: `new {template}`, `open {text, name}`, `save`, `saved`, `packageText`, `validate`, `console {text}`

## Tests

- `kern/tests/run.sh studio`: Start mit Orb Run, Fly- und Orbit-Navigation, jedes Katalog-Element platzieren und simulieren, Gizmo-Drag mit Undo/Redo und Alt-Duplikat, Tastenkürzel, Befehle, Menüs, Dialoge, Outliner-Hierarchie und Ordner, Play mit F8 und Esc, Keep Simulation Changes, Speichern/Laden, Texturen, Autosave, Packaging, Player-Modus
- `kern/tests/e2e/studio_e2e.mjs`: Browser-Test (Playwright) der Single-File-Version: Menüs, Fly Mode mit rechter Maustaste und WASD, Werkzeuge, Outliner, Details-Bearbeitung mit Undo, Content Drawer (Klick und Drag-and-drop), Kontextmenü, Play mit HUD und Fahren, Eject, Simulate, Quad-Layout, neues Level aus Vorlage, Hilfe, exportiertes Spiel, Modul-Version und schmales Fenster
