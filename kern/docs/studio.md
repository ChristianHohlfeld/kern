# Kern Studio

Kern Studio ist der Editor der Kern-Engine. Er arbeitet wie eine Game Engine im Stil der Unreal Engine: Levels aus Actors mit Komponenten, Viewports mit Unreal-Navigation, Transform-Gizmos, Outliner und Details-Panel. Play und Simulate laufen direkt im Viewport. Rendern, Box3D-Physik, Steuerung und Spielregeln laufen im selben C++-Code, mit dem auch exportierte Spiele laufen.

- Live: https://kernengine.de/editor/
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
| Details | Name, alle Komponenten der Auswahl (Mehrfachauswahl zeigt „Multiple“), **+ Add** für Komponenten, Suche, Zurücksetzen. Zahlenfelder lassen sich ziehen (Label oder Feld seitwärts ziehen, Shift ×10, Ctrl ×0,1) und rechnen (`2*3`). Der Farbwähler bleibt offen, solange man darin klickt und zieht: jede Farbe wirkt sofort, die ganze Sitzung ist ein Undo-Schritt |
| World Settings | Spieltitel, Siegbedingung, Zeitlimit, Kill-Z, Tageszeit, Tageslänge, Sonne, Nebel, Belichtung, Wolken, Boden (mit Reibung und Restitution), Schwerkraft, Substeps, Schrittrate, Kontakt-Steifigkeit, -Dämpfung und -Ausstoßgeschwindigkeit |
| Content Drawer (Ctrl+Space) | Place Actors (Klick oder Ziehen in einen Viewport), Texturen, Output Log |
| Statusleiste | Konsole (`help`), Statusmeldung, Physik-Statistik, Auswahl |

Beispiel-Levels (File › Example Levels und File › New Level): Orb Run, Physics Lab, Shooting Range,
Micro Duck, Basic, Empty.

**Menüs:** Mit der Maus klappen Untermenüs beim Überfahren neben dem Menü auf, nie darüber. Passt
daneben kein Platz, oder wird getippt (Touch, Stift), öffnet ein Tipp das Untermenü an Ort und
Stelle, mit einer Zurück-Zeile oben. So liegt nie ein Menü unter dem Finger, der es geöffnet hat.
Lange Menüs scrollen, und auf Touch sind die Zeilen 40 px hoch.

**Handy und kleine Fenster** (bis 900 px Breite, oder Touch mit bis 600 px Höhe):
- Der Viewport bekommt die ganze Fläche.
- Outliner, Details und World Settings öffnen sich über den Panels-Knopf rechts in der Toolbar als
  ein Blatt mit großen Tabs. Im Querformat erscheint es neben dem Viewport, im Hochformat von unten.
- Der Content Drawer belegt höchstens die halbe Höhe.
- **Play und Simulate sind immer mit einem Tipp erreichbar**, nie nur im Menü: Sie liegen zusammen mit
  dem Panels-Knopf in einem Dock am rechten Rand der Toolbar. Der Rest der Toolbar scrollt darunter
  durch, das Dock bleibt stehen – auch bei offenem Outliner/Details/World-Blatt, Content Drawer oder
  Logic-Editor. Während Simulate/Play stehen dort Pause, Stop und Eject (bzw. Play zum Übernehmen des
  Spielers); Knöpfe, die im aktuellen Zustand nichts tun, sind ausgeblendet, die übrigen haben
  Touch-Größe (mind. 38 px).
- Play auf Touch-Geräten füllt den ganzen Bildschirm; oben links stehen **EXIT** (Stop) und
  **PAUSE**/**RESUME**.

## Steuerung

| Eingabe | Wirkung |
|---|---|
| Rechte Maustaste + WASD / QE | Fly Mode; Mausrad währenddessen ändert die Geschwindigkeit (1-8), Shift schneller, Z/C zoomen |
| Linksklick | auswählen (Shift ergänzt, Ctrl schaltet um); Klick ins Leere hebt die Auswahl auf |
| Mit links ziehen (auch über einem Actor) | Rahmenauswahl (Shift ergänzt, Ctrl schaltet um); verschoben wird mit dem Gizmo |
| Rechte + linke Maustaste | im Fly Mode hoch/runter und seitwärts |
| Mittlere Maustaste ziehen | schwenken |
| Alt + links / rechts / Mitte | um die Auswahl kreisen / Dolly / schwenken |
| Mausrad | vor/zurück (orthografisch: Zoom zum Cursor) |
| F / End | Auswahl fokussieren / auf den Boden setzen |
| Q W E R / Leertaste | Select / Move / Rotate / Scale / durchschalten |
| Alt + Gizmo ziehen | beim Bewegen duplizieren |
| Aus „Place Actors“ ziehen | neuer Actor landet genau dort, wo losgelassen wird |
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
| Ziehen auf einem Körper (Simulate, Eject, Beobachten) | ihn mit dem Mouse Joint greifen, ziehen, hochheben und werfen; Mausrad: näher / weiter; ein Klick wählt ihn aus |
| K (Simulate) | simulierte Posen der Auswahl übernehmen |
| Rechtsklick | Kontextmenü: Fokus, Umbenennen, Anhängen, mit Gelenk verbinden, Sperren, Actor platzieren |

## Levels, Actors und Komponenten

Ein Level (`.klevel`, JSON, `"format": "kern-level"`, Version 1) besteht aus World Settings, Actors, Texturen und der Editor-Kamera. Einheiten sind Meter, Sekunden und Kilogramm, Z zeigt nach oben. Rotationen stehen als Euler-Winkel in Grad (x Roll, y Pitch, z Yaw), Farben als sRGB.

Ein Actor hat Name, Eltern-Actor, Outliner-Ordner, Hidden/Locked, Tags, Transform und diese Komponenten:

| Komponente | Bedeutung |
|---|---|
| Static Mesh | Box, Sphere, Cylinder, Cone, Capsule, Plane, Torus, Ramp mit PBR-Material (Farbe, Roughness, Metallic, Emissive, Opacity, Unlit, Schatten, zweiseitig, Textur mit Tiling); „Generate collision“ erzeugt einen passenden Collider |
| Point Light | Farbe, Intensität, Radius |
| Rigid Body (Box3D) | Static, Kinematic, Dynamic; Masse (0 = aus der Dichte), Dämpfung, Gravity Scale, Bullet, Sleep, Achsensperren, Startgeschwindigkeit; Aufprall-Sound mit Schwelle. Importierte Roboter tragen zusätzlich Schwerpunkt und Trägheitstensor (`centerOfMass`, `inertia` im Leveldokument) |
| Collider (mehrere) | Fit to mesh, Box, Sphere, Capsule, Convex Hull; Offset, Rotation, Dichte, Reibung, Restitution, Rollwiderstand, Sensor, Kanäle (Kategorie/Maske/Gruppe), Impact Sound (*auto* = der des Bodys; so klingt eine Sohle anders als ein Gehäuse) |
| Constraint (mehrere) | Hinge, Slider, Ball, Fixed, Distance/Rope, Wheel, Motor, Parallel, No Collision; Anker und Achse lokal, Limits, Motor, Feder, Ruhelänge, Bruchkraft und -moment. Hinge: Referenzwinkel der Baupose (`reference`); Servo mit Ziel, Steifigkeit, Dämpfung, Reibung und Haltemoment (`servo*`, siehe [`physics.md`](physics.md) Abschnitt 5) |
| Player Controller | Hover Car, Rolling Ball, Character, Flyer, Vehicle, Soldier, Walker; Geschwindigkeit, Beschleunigung, Wenderate, Sprung, Boost, Schwebehöhe, Kamera (Chase, First Person, Top Down); Motor-Loop, Sprung- und Boost-Sound |
| Pickup | Punkte, Partikelfarbe, Respawn, Sound |
| Trigger | Goal, Hazard, Checkpoint, Message; Sound (Automatic = passend zur Aktion) |
| Audio Source | Sound, Lautstärke, Pitch, Loop, Autoplay, Startverzögerung, Bus; 3D: innerer Radius, Falloff, Roll-off (Inverse, Linear, Exponential), Doppler, Pitch nach Geschwindigkeit |
| Rotator / Mover | Drehung bzw. Bewegung (Sinus, Ping-Pong, Schleife) |
| Procedural City | Seed, Blöcke, Raster, Straßenbreite, Höhe, Bäume, Verkehr, beleuchtete Fenster in der Nacht |

Constraints haben zusätzlich einen Bruch-Sound. Die World Settings enthalten unter „Audio“ Master-Lautstärke, Musik, Ambience (je mit Lautstärke), Hall und Doppler.

Kind-Actors ohne eigenen Body werden beim Simulieren als Compound in den Body des nächsten Vorfahren geschweißt. Meshes mit Kollision, Pickups und Trigger ohne Body bekommen einen impliziten statischen Body, bei Animation durch Rotator oder Mover einen kinematischen. Gelenke verbinden den Actor (Body B) mit dem Ziel-Actor (Body A, 0 = Welt). Ausnahme sind Wheel-Gelenke: Dort ist das Chassis Body A.

Fehler wie verwaiste Referenzen oder ungültige Werte verhindern Play; Warnungen erscheinen im Output Log.

## Place Actors

Shapes: Cube, Sphere, Cylinder, Cone, Capsule, Plane, Torus, Ramp, Empty Actor · Lights: Point Light · Physics: Physics Cube, Physics Sphere, Physics Barrel, Crate Stack, Pendulum, Hinged Door, Chain, See-Saw, Sock · Players: Soldier, Character, Hover Car, Rolling Ball, Drone, Micro Duck, Micro Duck (Rollers) · Gameplay: Orb, Star, Health Pack, Ammo Box, Goal, Hazard, Damage Zone, Checkpoint, Jump Pad, Teleporter, Message Trigger, Moving Platform, Spinner, Target Dummy, Explosive Barrel · Audio: Audio Source, Ambient Wind, City Ambience, Radio · World: Procedural City (Liste: `src/studio/catalog.cpp`).

## Sound

Die Sound-Engine ist C++ im Engine-Modul (`kern/src/audio`): ein Software-Mixer mit 64 Stimmen (Hermite-Resampling, Prioritäten, Voice Stealing, virtuelle Stimmen außerhalb der Hörweite), 3D-Klang am Listener der Kamera (Equal-Power-Panning, Roll-off, Luftdämpfung, Tiefpass hinter dem Hörer, Doppler), Bussen (Music, Effects, Ambience, Interface, Voice → Master) mit klickfreien Lautstärkerampen, Tiefpass und Hall-Send, einem Freeverb-Hall, Peak-Limiter und Soft-Clip. Die Ausgabe überschreitet nie ±1.

Eingebaut sind 38 synthetisierte Sounds (`builtin_sounds()`) und die Laute der Ente (Pickup, Jump, Impact, Kunststoff-Tippen, kleine Schritte, Explosion, Motor, Hover, Wind, Stadt, Servo, Quack, UI-Klicks und zwei Musik-Loops). Sie werden deterministisch und in Zeitscheiben gerendert, ein Level mit Musik ruckelt beim Laden nicht. Eigene Dateien (WAV, MP3, OGG, M4A, FLAC – was der Browser dekodieren kann) importiert man im Content Drawer unter **Sounds**, per Drag & Drop auf die Seite oder über das Plus am Sound-Feld; sie liegen als Data-URL im Level und werden per Windowed-Sinc auf die Ausgaberate gebracht.

Bedienung: Ein Klick auf einen Sound im Tab Sounds spielt ihn an, ein Doppelklick setzt ihn auf die Auswahl, Ziehen in den Viewport platziert eine Audio Source. Jedes Sound-Feld hat eine Vorhör-Taste. Ausgewählte Audio Sources zeigen innen den vollen und außen den Falloff-Radius. Die Statusleiste hat Stummschalter und Pegelanzeige.

Während Play und Simulate spielt das Spiel Musik, Ambience und Audio Sources, den Motor-Loop des Spielers (Pitch und Lautstärke folgen Tempo und Gas), Schritte, Landungen, Sprung, Boost, Pickups, Trigger, Sieg/Niederlage, Aufpralle von Bodies und brechende Gelenke. Die Ereignisse entstehen in den festen Physikschritten und sind deterministisch. Pause pausiert alle Stimmen.

Im Browser überträgt `kern/web/studio/game.mjs` (`KernAudio`) das PCM an die Lautsprecher: per AudioWorklet (Blob- oder Data-Modul), sonst per ScriptProcessor. Die erste Berührung, Taste oder der erste Klick startet den Ton (Browser-Regel, iOS eingeschlossen; `navigator.audioSession` steht auf „playback“). Exportierte Spiele haben denselben Ton.

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
| `kern_studio_take_autosave(now)`, `kern_studio_take_autosave_discard()`, `kern_studio_dirty()` | Autosave (siehe [Wiederherstellen](#wiederherstellen-restore-from-last-session)) |
| `kern_studio_audio_init(rate)`, `kern_studio_audio_render(frames)` | Mixer starten, Stereo-PCM ziehen |
| `kern_studio_pending_sounds()`, `…_sound_source(id)`, `…_sound_pcm(id, pcm, frames, channels, rate)`, `…_sound_failed(id, reason)` | importierte Sounds dekodiert die Seite |

Befehle (`{"cmd": …}`):

- Auswahl: `select {ids, add, toggle, focus}`, `selectRange {id}`, `selectAll`, `deselect`, `selectChildren`, `selectFolder {folder}`
- Actors: `spawn {item, at | viewport+x+y, sound}`, `delete`, `duplicate`, `copy`, `cut`, `paste`, `pasteText {text}`, `rename {id, name}`, `attach {ids, parent}`, `moveTo {ids, parent | folder}`, `setFolder {folder}`, `renameFolder {from, to}`, `group`, `snapToFloor`, `hide`/`show {ids | folder}`, `showAll`, `lock {value}`, `focus {ids}`, `connect {kind}`
- Properties: `set {path, value}` (z. B. `transform.position`, `components.mesh.material.color`, `components.joints.0.lower`), `setWorld {path, value}`, `addComponent {type}`, `removeComponent {type, index}`, `importTexture {name, src, width, height}`, `removeTexture {id}`
- Sound: `previewSound {sound, volume, pitch}`, `stopSound`, `importSound {name, src, duration}`, `removeSound {id}`, `renameSound {id, name}`, `mute {muted}`
- Hilfen: `project {id | point, viewport}` liefert die Bildschirmposition
- Historie: `undo`, `redo`, `historyJump {index}` (aufeinanderfolgende Änderungen derselben Eigenschaft werden zu einem Schritt zusammengefasst)
- Play: `play {mode: play|simulate}`, `stop`, `pause`, `stepFrame`, `eject`, `restart`, `keep`, `gameInput {moveX, moveY, up, jump, boost}`, `pointerCaptured {value}`
- Werkzeuge und Ansichten: `tool`, `space`, `snap`, `cameraSpeed`, `layout`, `maximize`, `activeViewport`, `viewType`, `viewMode`, `showFlag`, `gameView`, `fov`, `bookmark {slot, set}`
- Panels: `filter`, `collapse {key}`, `section {id, closed}`, `menu {name}`, `dialog {name}`, `submitDialog {name, values}`, `catalog`, `clearLog`, `ui {action}`
- Dokumente: `new {template}`, `open {text, name}`, `restore {text, name}`, `save`, `saved`, `packageText`, `validate`, `console {text}`

## Einstellungen

Studio merkt sich die Einstellungen des Nutzers im Browser (`localStorage`, `kern-studio.settings.v1`,
Format `{version: 1, values: {…}}`):

- **Was dazugehört:** Preferences (Kamerageschwindigkeit, Mausempfindlichkeit, Invert Y, Orbit um die
  Auswahl), Viewport-Layout, Grid-/Rotations-/Skalierungsraster, Welt-/lokale Achsen, Ton an/aus und pro
  Viewport Ansichtstyp, View Mode, Sichtfeld und alle Show-Flags. Die Seite speichert außerdem
  Panel-Größen und den Content Drawer (`kern-studio.layout.v1`).
- **Nur eigene Entscheidungen:** Die Engine (`kern/src/studio/settings.cpp`) liefert mit
  `take_settings()` nur die Einstellungen, die der Nutzer selbst geändert hat (per Menü, Toolbar,
  Tastenkürzel, Dialog) oder die aus dem Speicher geladen wurden. Was nie angefasst wurde, wird nicht
  gespeichert und folgt den Standardwerten des jeweiligen Builds. Ändert sich ein Standardwert (z. B.
  Show › Constraints ist jetzt **aus**), gilt er für alle, die diese Einstellung nie geändert haben.
- **Wann gespeichert wird:** alle 0,5 s, wenn sich etwas geändert hat, und bei `pagehide` bzw. beim
  Wechsel in den Hintergrund. Unbekannte Schlüssel und ungültige Werte im Speicher werden beim Laden
  übergangen. Dass Multiplayer-Play von selbst auf vier Viewports schaltet, gilt nicht als Wahl.
- **Auf Standard zurücksetzen:** Edit › **Reset Settings to Defaults…** fragt im Studio-eigenen Dialog
  nach (kein Browser-Dialog) und setzt alle Einstellungen, Panel-Größen und den Content Drawer zurück;
  der gespeicherte Stand wird gelöscht. Levels bleiben unberührt.
- Befehle: `settings` (aktuelle Werte und Standardwerte), `applySettings {values}`, `takeSettings`,
  `resetSettings`.

## Wiederherstellen („Restore from last session“)

Beim Start bietet Studio **Restore "…" from your last session? · Restore / Dismiss** nur an, wenn die
letzte Sitzung ungespeicherte Arbeit zurückgelassen hat. Ein normales Neuladen eines Editors ohne
ungespeicherte Änderungen zeigt nichts an.

- **Wann gespeichert wird:** Die Seite hält höchstens eine Kopie in `localStorage`
  (`kern-studio.autosave.v1`). Die Engine entscheidet: `take_autosave(now)` liefert den Level-Text nur,
  wenn eine Änderung fällig ist (`autosave_seconds`, 2 s) und das Dokument ungespeicherte Änderungen hat
  (`dirty()`).
- **Wann sie gelöscht wird:** Gehört die gespeicherte Kopie zum aktuellen Dokument und ist nichts mehr
  ungespeichert, meldet `take_autosave_discard()` das genau einmal, und die Seite löscht die Kopie. Das
  passiert nach dem Speichern und wenn die Änderungen bis zum gespeicherten Stand rückgängig gemacht
  wurden. **Dismiss** löscht die Kopie ebenfalls.
- **Anderes Level öffnen oder neu anlegen:** Die Kopie der ungespeicherten Änderungen des vorigen Levels
  bleibt erhalten, bis das neue Level eigene Änderungen bekommt. Sie wird dann ersetzt.
- **Restore** lädt die Kopie mit dem Befehl `restore`. Das Level bleibt ungespeichert (Punkt am Tab,
  Rückfrage beim Verlassen), denn sein Inhalt steht in keiner Datei. Bis zum Speichern wird es bei
  jedem Start wieder angeboten.
- **Seite verlassen:** Bei `beforeunload`, `pagehide` und beim Wechsel in den Hintergrund
  (`visibilitychange`) schreibt die Seite eine fällige Kopie sofort bzw. löscht eine veraltete. So kommt
  auch auf Mobilgeräten und beim Update-Neuladen (siehe [Als App installieren](#als-app-installieren-android-ios-desktop))
  nichts abhanden.
- **Absturz oder nicht sauber beendet:** Die Kopie wird spätestens 2 s nach jeder Änderung geschrieben.
  Stürzt der Tab ab oder wird er beendet, ohne dass gespeichert wurde, liegt sie beim nächsten Start
  noch da und wird angeboten. War beim Absturz nichts ungespeichert, gibt es auch nichts anzubieten.
- **Nicht gespeichert, solange Play läuft:** Während Play/Simulate wird nichts geschrieben. Eine fällige
  Kopie folgt nach Stop.

## Viewport mit den Fingern

Im Editor und in Simulate steuert man die Kamera wie in einem 3D-Betrachter am Handy
(`Studio::free_touch_pointer`):

- **Ein Finger** dreht die Kamera um den Punkt unter dem Finger (oder um die Auswahl, wenn
  „Orbit around selection" an ist). In Draufsicht und Seitenansichten verschiebt er die Ansicht.
- **Zwei Finger** zoomen zum Punkt zwischen ihnen (auseinander näher, zusammen weiter weg) und
  verschieben die Ansicht mit sich: Was unter den Fingern liegt, bleibt unter ihnen. Der Maßstab ist
  der Abstand zu diesem Punkt, deshalb fühlt sich das an einer Socke wie an einem Häuserblock gleich an.
- **Tippen** wählt aus, **zweimal tippen** holt die Auswahl ins Bild (wie F), **lange halten** öffnet
  das Kontextmenü.
- Ein Finger auf einem **Gizmo**-Griff verschiebt, dreht oder skaliert wie mit der Maus.
- Ein Finger in einem anderen Viewport macht diesen zum aktiven; die Seite zoomt beim Pinch nie mit.
- Test: `studio_touch_navigates_the_editor_viewport`.

## Beobachten und Anfassen (Observer-Kamera)

Mit **World Settings → Observer camera** startet Play nicht hinter dem Spieler, sondern mit einer
freien Kamera: Das Spiel läuft, man fliegt herum und schaut zu. Das Beispiel-Level Micro Duck nutzt
das.

- **Desktop:**
  - WASD fliegt, Q / E fliegt hoch / runter, Shift fliegt schneller (ohne gehaltene rechte Maustaste).
  - Ziehen ins Leere schaut sich um; rechte Maustaste und Mausrad wie im Editor.
- **Handy:**
  - Ein Finger ins Leere schaut sich um.
  - Zwei Finger zoomen und schwenken wie im Editor (siehe [Viewport mit den Fingern](#viewport-mit-den-fingern)).
- **Anfassen:** Jeden Körper (dynamischer Box3D-Body) greift man mit dem Mouse Joint, mit der Maus
  wie mit dem Finger, auch in Simulate und nach Eject (Ctrl ist nicht mehr nötig).
  - Ein Motor-Joint mit Feder (7,5 Hz, kritisch gedämpft) zieht den gegriffenen Punkt zum Zeiger.
    Der Körper bleibt ganz in der Physik: Er stößt an, kippt, schwingt und fliegt beim Loslassen
    mit seinem Schwung weiter.
  - Den Zeiger nach oben ziehen hebt ihn hoch.
  - Mausrad oder ein zweiter Finger (auseinander / zusammen) holt ihn näher oder schiebt ihn weg.
  - Ein kurzer Klick oder Tipp wählt den Körper aus.
- **Spieler übernehmen:** F8 übernimmt den Spieler (z. B. die Ente per Gamepad), F8 noch einmal
  beobachtet wieder. Im exportierten Spiel gibt es beim Beobachten keinen Pointer-Lock.
- **Tests:** `studio_observer_camera_grabs_bodies` (Fliegen, Maus und Finger heben die Kiste, Mausrad,
  Pinch, F8), `studio_simulate_drags_bodies_without_ctrl`, E2E am Handy (der Ball wird mit dem Finger
  angehoben)

## Tests

- `kern/tests/run.sh studio`: Start mit Orb Run, Fly- und Orbit-Navigation, jedes Katalog-Element platzieren und simulieren, Gizmo-Drag mit Undo/Redo und Alt-Duplikat, Tastenkürzel, Befehle, Menüs, Dialoge, Outliner-Hierarchie und Ordner, Play mit F8 und Esc, Keep Simulation Changes, Speichern/Laden, Texturen, Autosave (nur bei ungespeicherten Änderungen, Löschen nach Speichern und Undo, `restore`), Einstellungen (Standardwerte, nur eigene Entscheidungen, Laden, Zurücksetzen), Packaging, Player-Modus
- `kern/tests/e2e/studio_e2e.mjs`: Browser-Test (Playwright) der Single-File-Version: Menüs, Fly Mode mit rechter Maustaste und WASD, Werkzeuge, Outliner, Details-Bearbeitung mit Undo, Content Drawer (Klick und Drag-and-drop), Kontextmenü, Play mit HUD und Fahren, Eject, Simulate, Quad-Layout, neues Level aus Vorlage, Hilfe, exportiertes Spiel, Wiederherstellen (Neuladen ohne Änderung: kein Angebot; mit ungespeicherter Änderung: Angebot, Restore, Dismiss), Einstellungen (Constraints standardmäßig aus, Speichern und Neuladen, Reset im Studio-Dialog), Handy hoch und quer (Play/Simulate/Pause/Stop in jedem Zustand mit einem Tipp erreichbar), Level öffnen auf Android über den Dateidialog, Farbwähler bleibt beim Ziehen offen, Micro Duck am Handy beobachten und den Ball mit dem Finger anheben, Modul-Version und schmales Fenster

## Logic: Blueprints, die C++ sind

Jeder Actor und das Level können einen Logic-Graphen tragen: Events, Flow, Variablen,
State Machines, Mathe und rund 70 Gameplay-Knoten. Geöffnet wird er über **Logic** in der Toolbar
oder in Details.

Der Blueprint-Editor folgt Unreals Blueprint-Editor (`web/studio/logic.mjs`) und deckt das ganze Fenster ab:

| Bereich | Inhalt |
|---|---|
| Toolbar | Viewport (zurück), **Compile** (F7, mit Status ✓ / ⚠ / ✖), Save, Find (Ctrl+F), welcher Blueprint (Level oder Actor), Vorlagen, C++-Export |
| My Blueprint | Graphen (Event Graph, State Machines, +), Variablen (Typfarbe, Live-Wert beim Spielen, Get/Set, ziehen: Ctrl = Get, Alt = Set), Events (Custom Events: Event / Call) |
| Graph | Tabs, Breadcrumb, Zoom-Stufen wie Unreal („Zoom 1:1“, „Zoom −2“), Wasserzeichen EVENT GRAPH |
| Details | ausgewählter Knoten (Hilfe, Einstellungen, alle Eingänge, Kommentar), Variable (Name, Typ, Default, Referenzen), Kommentar (Text, Farbe), State, Transition |
| Compiler Results / Find Results | Probleme und Suchtreffer, ein Klick springt zum Knoten |

Im Graphen:
- **Knoten:**
  - Titelleisten nach Art: Event rot, Funktion blau, pure grün, Flow grau.
  - Eingaben ohne Draht direkt am Pin.
  - Variablen-Getter und Operatoren sind kompakte Knoten: Name bzw. Symbol (+, −, ×, ÷, <, AND …).
- **Drähte:**
  - Unreal-Splines, farbig nach Typ, Exec weiß und dicker.
  - Drähte ausgewählter Knoten werden hervorgehoben.
  - Beim Spielen laufen Impulse über die Exec-Drähte, die gerade feuerten.
- **Verdrahten:**
  - Einen Pin ziehen, passende Pins leuchten auf.
  - Ins Leere fallen lassen: das kontextsensitive Aktionsmenü mit „Add Reroute Node“.
  - Ctrl+Ziehen nimmt die Drähte eines Pins auf und hängt sie um.
  - Alt+Klick trennt.
  - Doppelklick auf einen Draht setzt einen Reroute-Knoten.
- **Rechtsklick (Touch: langer Druck):**
  - Leere Fläche: „All Actions for this Blueprint“ mit aufklappbaren Kategorien und Suche.
  - Knoten: Ausschneiden/Kopieren/Duplizieren, Links trennen, Kommentar, Ausrichten/Verteilen, Breakpoint.
  - Pin: Links trennen, „Promote to Variable“, Default zurücksetzen.
- **Kommentare (C):**
  - Ein Rahmen um die Auswahl; verschieben verschiebt den Inhalt mit.
  - Ecke ziehen ändert die Größe, Doppelklick auf den Titel benennt um, die Farbe kommt aus Details.
- **Tastatur:**

  | Taste | Wirkung |
  |---|---|
  | Q | Verbindungen geraderichten |
  | Shift+W/A/S/D | Ausrichten |
  | B / S / D / N / O / G / L + Klick | Branch / Sequence / Delay / Do N / Do Once / Gate / For Loop |
  | Ctrl+C/X/V/D/W | Kopieren / Ausschneiden / Einfügen / Duplizieren |
  | F | Auswahl einrahmen |
  | Home | alles einrahmen |
  | Ctrl+Z | Rückgängig (jede Änderung ist ein Schritt) |
- **Debugging:**
  - F9 setzt einen Breakpoint (roter Punkt).
  - Läuft der Knoten beim Spielen, hält das Spiel nach diesem Simulationsschritt an.
  - Der Editor springt zum Knoten; Resume, Step und Clear Breakpoints stehen oben im Graphen.
  - Variablen zeigen ihre Live-Werte.
- **Handy:**
  - Ein Finger verschiebt, zwei Finger zoomen, ein langer Druck ist der Rechtsklick.
  - Ein Tipp auf einen Pin zeigt die passenden Knoten.
  - Der +-Knopf öffnet das Aktionsmenü als Sheet.
  - My Blueprint und Details sind Schubladen, ein Kasten-Knopf schaltet die Rechteckauswahl ein.
- **Vorlagen:** Tür, Punktezone, Spawner, Ampel.

Ausgeführt wird in drei Stufen, die dasselbe Ergebnis liefern (`kern/tests/logic_cases.h`):
1. **Interpreter**, zum Bearbeiten.
2. **JIT**: Beim Start von Play/Simulate kompiliert Kern jeden Graphen zu WebAssembly-Maschinencode
   (`src/logic/codegen.cpp`). Das ist etwa 55-mal schneller als der Interpreter. Die Statuszeile zeigt
   „compiled“.
3. **Export als C++**: **Export C++** schreibt einen lesbaren Quelltext, der sich über einen Hash am
   Graphen registriert. Mit `kern/build.sh --studio out.wasm datei.cpp` läuft der Graph dann als
   nativer Code.

Logic läuft nur auf der Autorität (Standalone oder Server). Geänderte Eigenschaften und erzeugte
Actors werden repliziert, auch an Spieler, die später dazukommen.

## Spieler, Soldat, Touch und Gamepad

- **Spieler setzen:** Ein Element aus der Gruppe **Players** in die Welt ziehen; es wird sofort der
  Spieler (Badge PLAYER im Outliner). Alternativ Rechtsklick → *Make Player* oder World Settings →
  *Player*.
- **Soldat:** Ego-Shooter nach SpawnCity.
  - Bewegung: Gehen, Sprinten, Ducken, Springen.
  - Waffen: Maschinenpistole, Pistole, Sniper oder 40-mm-Werfer mit Magazinen, Nachladen, Zielen über Kimme
    und Korn sowie Kopftreffern.
  - Spiel: Respawn und Lag-Kompensation im Online-Spiel.
  - Optik: Andere Spieler sehen die animierte Gelenkfigur mit Waffe, dazu Mündungsfeuer, Tracer,
    Einschusslöcher und Splitter.
- **Micro Duck:** Pollen Robotics' zweibeiniger Roboter mit seinem echten, trainierten Netz
  (velstand v8).
  - Steuerung: W/S laufen, A/D drehen, Leertaste quakt.
  - Ohne Eingabe läuft sie autonom: Sie erkundet den Raum mit Kopf-ToF und Kamera, weicht
    Hindernissen aus und steht nach einem Sturz selbst wieder auf (Details › Walker › Autonomous).
  - Oben rechts im Viewport zeigt Studio ihre Kopfkamera und das ToF-Gitter (Show › Robot Camera).
  - Logic steuert sie mit den Knoten der Kategorie Robots (Walk, Look, Robot ToF, Robot Camera Sees …).
  - Beim Einfügen stellt Studio die Physik auf 100 Hz mit 16 Sub-Steps.
  - Beispiel-Level **Micro Duck**: ein Zimmer in ihrem Maßstab (2,5 × 2,5 m echt) mit Möbeln und
    einem Ball. Die Ente läuft autonom. Der Level-Blueprint lässt sie quaken, sobald ihre
    Kopfkamera den Ball sieht.
  - Details: [`walker.md`](walker.md).
- **Geräteerkennung:** Die Engine erkennt Touch, Maus+Tastatur und Gamepad selbst und schaltet auf
  das zuletzt benutzte um (`src/game/touch.cpp`).
  - Auf Touch erscheinen 168-px-Sticks in den Ecken: links bewegen, rechts schauen mit Edge-Turn.
    Ein Tippen auf den Bildschirm feuert, die Buttons passen sich dem Controller an.
  - Play läuft im Vollbild mit EXIT; Pointer-Lock gibt es nur mit Maus.
  - Gamepad: Xbox-Belegung mit Dead-Zones und Kurven wie in SpawnCity.
- **Viewport:** Actors bewegt nur der Gizmo (W / E / R) wie in Unreal. Linkes Ziehen zeichnet ein
  Auswahlrechteck.

Physik-Regeln für eigene Presets: [`physics.md`](physics.md).

## Als App installieren (Android, iOS, Desktop)

Kern Studio unter `/editor/` ist eine installierbare Web-App:

- **Installieren:**
  - Chrome und Edge (Android, Windows, macOS, Linux) zeigen in der Titelleiste **Install**.
  - Auf dem iPhone und iPad erklärt der Button die zwei Schritte: Teilen, dann *Zum Home-Bildschirm*.
  - Die App startet danach im Vollbild (`display: fullscreen`). Notches und Statusleiste bleiben frei
    (`viewport-fit=cover` mit Safe-Area-Abständen).
- **Offline:** Der Service Worker (`site/editor/sw.js`) holt die Seite immer zuerst aus dem Netz. Nur
  wenn das Netz fehlt, startet die zuletzt geladene Version aus dem Cache. Icons und Manifest kommen
  aus dem Cache und werden im Hintergrund aufgefrischt.
- **Nie eine alte Version:**
  - Jeder Deploy schreibt den Build-SHA als ersten Kommentar in die Seite und als `build.json` daneben.
  - Die Seite fragt `build.json` beim Start, bei jeder Rückkehr zur App, wenn das Netz zurückkommt,
    und alle 60 s ab. Diese Abfrage geht an keinem Cache vorbei.
  - Ist ein neuer Build da, lädt Studio sofort neu, solange nichts verloren gehen kann: im Editier-Modus
    ohne ungespeicherte Änderungen.
  - Sonst erscheint **New version available · Update / Later**. Beim Update wird das Level per Autosave
    mitgenommen und beim nächsten Start angeboten (siehe [Wiederherstellen](#wiederherstellen-restore-from-last-session)).
  - Unten rechts steht die laufende Version, gelb, wenn online eine neuere bereitliegt.
- **Level öffnen auf Android:** Android hat kein `showOpenFilePicker`, Studio öffnet den normalen
  Dateiauswahl-Dialog. Der kennt die Endung `.klevel` nicht und würde mit einem Filter alle
  gespeicherten Levels ausgrauen (heraus käme man nur mit Zurück). Auf Android filtert Studio deshalb
  nicht; ist die gewählte Datei kein Level, erscheint ein Hinweis, und der Editor läuft weiter.
- **Icons:** 192, 512 und maskable 512 im Manifest, dazu ein Apple Touch Icon (180) und ein Favicon
  (`site/editor/icons`).
