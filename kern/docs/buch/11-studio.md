# 11 Kern Studio

Kern Studio ist der Editor der Kern-Engine. Er arbeitet wie die Unreal Engine: Ein Level besteht aus
Actors, jeder Actor trägt Komponenten, man bearbeitet ihn in Viewports mit Unreal-Navigation und
Transform-Gizmos, in Outliner, Details und World Settings. **Play** und **Simulate** laufen direkt im
Viewport, und ein fertiges Level wird mit einem Klick zu einer einzigen HTML-Datei, die überall läuft.

Das Besondere an Studio steckt unter der Oberfläche: Die gesamte Editorlogik ist C++ und läuft im selben
WebAssembly-Modul wie Renderer, Box3D-Physik und Spiel-Laufzeit. Was man im Viewport sieht, rendert und
simuliert genau der Code, der später das exportierte Spiel ausführt. Die Browserseite zeichnet nur Panels
und reicht Eingaben weiter (Abschnitt [11.19](#1119-architektur)).

Dieses Kapitel ist das Benutzerhandbuch. Wie man daraus ein eigenes Spiel macht, zeigt
[Kapitel 12](12-eigene-spiele.md); die Logik-Graphen beschreibt [Kapitel 9](09-logik.md), die Physik
[Kapitel 6](06-physik.md), den Ton [Kapitel 7](07-audio.md), die Spiel-Laufzeit hinter Play
[Kapitel 8](08-spiel-laufzeit.md) und den Roboter Plumo [Kapitel 13](13-plumo.md).

![Kern Studio mit dem Level Orb Run: Menüleiste, Toolbar, Viewport, Outliner und Details](bilder/studio.png)

Pfade wie `kern/src/studio/ui.cpp` beziehen sich in diesem Kapitel auf das Wurzelverzeichnis des
Repositorys, in dem die Engine im Ordner `kern/` liegt.

---

## 11.1 Studio starten

Es gibt drei Wege, Studio zu öffnen:

| Weg | Was man tut | Wofür |
|---|---|---|
| Live | https://christianhohlfeld.github.io/kern/editor/ öffnen | sofort loslegen, auch auf Telefon und Tablet |
| Eine Datei | `tools/build_studio.sh` bauen, dann `generated/studio.html` öffnen | offline, ohne Server; die Datei enthält alles |
| Entwicklung | Repository statisch ausliefern, `kern/web/studio/index.html` öffnen | am Studio selbst arbeiten (lädt `generated/kern_studio.wasm`) |

Die Startseite des Projekts (https://christianhohlfeld.github.io/kern/) verlinkt Studio, den Lehrer, die
beiden Beispielspiele und zwei Levels zum direkten Spielen (`kern/web/studio/play.html?level=orbRun` bzw.
`?level=physicsLab`). Ihre Fußzeile nennt den Rechteinhaber („© 2026 Christian Heinrich Hohlfeld“) und verlinkt die Lizenz
(`LICENSING.txt`, Apache 2.0), die Bestandteile Dritter (`LICENSES.txt`), den Quellcode, die Studio-Doku,
die Engine-Doku, das Impressum und die Datenschutzerklärung.

**Was beim Start passiert:**

1. Ein Splash-Bildschirm „Starting the engine" erscheint, während das WebAssembly-Modul lädt.
2. Beim allerersten Start ist **Orb Run** geladen: eine prozedurale Stadt mit Verkehr, einem
   Box3D-Hovercar, 24 Energie-Orbs und einem Tag, der in die Nacht übergeht. Das Output Log meldet
   „Kern Studio ready - Orb Run loaded".
3. Bei jedem weiteren Start öffnet Studio das Level, an dem man zuletzt gearbeitet hat, in seinem
   gespeicherten bzw. geöffneten Stand. Eine Meldung „Reopened "…", where you worked last time" bietet
   **Start fresh** an, um stattdessen mit Orb Run neu zu beginnen.
4. Hatte die letzte Sitzung ungespeicherte Änderungen, fragt Studio
   **Restore "…" from your last session?** mit **Restore** und **Dismiss** (Abschnitt
   [11.15](#1115-speichern-laden-autosave-und-einstellungen)).

Mit **Play** (Alt+P) fährt man in Orb Run sofort los.

### Als App installieren

Unter `/editor/` ist Studio eine installierbare Web-App:

- Chrome und Edge (Android, Windows, macOS, Linux) zeigen in der Titelleiste von Studio einen Knopf
  **⤓ Install**. Auf iPhone und iPad erklärt derselbe Knopf die zwei Schritte: **Teilen**, dann
  **Zum Home-Bildschirm**.
- Installiert startet Studio im Vollbild; Notch und Statusleiste bleiben frei.
- Ein Service Worker hält die zuletzt geladene Version für den Offline-Betrieb vor, holt die Seite aber
  immer zuerst aus dem Netz.
- Jeder Deploy schreibt seine Build-Kennung in die Seite und in `build.json`. Studio fragt beim Start,
  bei jeder Rückkehr zur App, wenn das Netz zurückkommt und alle 60 s nach. Ist ein neuer Build da und
  kann nichts verloren gehen (Editiermodus, nichts ungespeichert), lädt Studio sofort neu. Sonst
  erscheint **New version available** mit **Update** und **Later**; beim Update wandert das Level über
  den Autosave in die neue Version.
- Unten rechts in der Statusleiste steht die laufende Version, mit Hinweis, wenn online eine neuere
  bereitliegt.

---

## 11.2 Die Oberfläche im Überblick

| Bereich | Lage | Inhalt |
|---|---|---|
| Titelleiste | ganz oben | Kern-Logo, Menüleiste (File, Edit, Window, Play, Help), Dokument-Tab mit Levelname und Punkt für ungespeicherte Änderungen, ggf. Install-Knopf, Bilder pro Sekunde |
| Toolbar | darunter | Speichern, Undo/Redo, **Add**, Werkzeuge, Koordinatensystem, Raster, Kamerageschwindigkeit, Play-Gruppe, Layout, **Logic**, Package Game, Panels-Knopf |
| Viewports | Mitte | 1, 2 oder 4 Ansichten mit eigener Leiste |
| Outliner | rechts oben | alle Actors als Baum mit Ordnern, Suche, Sichtbarkeit |
| Details / World Settings | rechts unten | Eigenschaften der Auswahl bzw. der Welt, als zwei Tabs |
| Content Drawer | unten, ein- und ausklappbar | Place Actors, Textures, Sounds, Output Log |
| Statusleiste | ganz unten | Content Drawer, Output Log, Konsole, Statusmeldung, Physik-Statistik, Ton, Pegel, Auswahl, Version |

Die Trennlinie zwischen Viewport und Seitenleiste lässt sich ziehen (mindestens 240 px, höchstens 60 %
der Fensterbreite), ebenso die zwischen Outliner und Details. Studio merkt sich beide Größen und ob der
Content Drawer offen war.

### 11.2.1 Menüleiste

Ein Klick öffnet ein Menü; ist eines offen, wechselt das Überfahren eines anderen Menütitels direkt dorthin.
Mit der Maus klappen Untermenüs beim Überfahren **neben** dem Menü auf, nie darüber. Passt daneben
kein Platz oder wird getippt (Touch, Stift), ersetzt das Untermenü das Menü an Ort und Stelle, mit einer
Zurück-Zeile oben. So liegt nie ein Menü unter dem Finger, der es geöffnet hat. Auf Touch-Geräten sind
Menüzeilen 40 px hoch. Esc schließt jedes Menü.

**File**

| Eintrag | Kürzel | Wirkung |
|---|---|---|
| New Level... | Ctrl+N | Dialog mit sechs Vorlagen (siehe unten) |
| Open Level... | Ctrl+O | `.klevel`-Datei öffnen |
| Save | Ctrl+S | speichern (beim ersten Mal mit Dateidialog) |
| Save As... | Ctrl+Shift+S | unter neuem Namen speichern |
| Import Texture... | | Bild (PNG, JPEG, WebP) als Textur ins Level holen |
| Package Game (HTML)... | | Spiel als eine HTML-Datei exportieren ([11.16](#1116-spiel-exportieren)) |
| Example Levels ▸ | | Orb Run, Physics Lab, Shooting Range, Plumo, Basic, Empty |

Die Vorlagen im Dialog **New Level** (ein Doppelklick auf eine Karte erstellt sofort):

| Vorlage | Beschreibung im Dialog |
|---|---|
| Orb Run | prozedurale Nachtstadt, Box3D-Hovercar, 24 Orbs zum Einsammeln |
| Physics Lab | jedes Box3D-Gelenk: Scharnier, Schiene, Rad, Seilbrücke, Motor, Stapel |
| Shooting Range | Ego-Shooter-Soldat (Desktop, Gamepad, Touch), Ziele, Deckung, mehrspielerfähig |
| Plumo | Kerns Roboter-Ente auf dem trainierten Netz von Pollen Robotics' Open-Source-Roboter, läuft selbstständig mit ToF und Kamera |
| Basic | Boden, Sonne und Himmel, ein sauberer Anfang |
| Empty | gar nichts |

Hat das aktuelle Level ungespeicherte Änderungen, weist der Dialog darauf hin, dass sie im Autosave
bleiben, bis man das neue Level bearbeitet.

**Edit**

| Eintrag | Kürzel | Wirkung |
|---|---|---|
| Undo *Schritt* | Ctrl+Z | letzten Schritt zurücknehmen (der Name des Schritts steht im Eintrag) |
| Redo *Schritt* | Ctrl+Y | wiederholen |
| History... | | alle Schritte als Liste; ein Klick springt zu diesem Stand |
| Cut / Copy / Paste | Ctrl+X / C / V | Ausschneiden, Kopieren, Einfügen |
| Duplicate | Ctrl+D | Auswahl verdoppeln |
| Delete | Entf | Auswahl löschen |
| Select All | Ctrl+A | alles auswählen |
| Deselect | Esc | Auswahl aufheben |
| Group | Ctrl+G | Auswahl unter einen neuen Actor „Group" hängen |
| Snap to Floor | End | Auswahl auf den Boden setzen |
| Preferences... | | Kamera, Maus, Raster (Tabelle in [11.15](#1115-speichern-laden-autosave-und-einstellungen)) |
| Reset Settings to Defaults... | | alle Editor-Einstellungen zurücksetzen |

**Window**

| Eintrag | Kürzel | Wirkung |
|---|---|---|
| Viewport Layout ▸ | | 1, 2 oder 4 Viewports |
| Viewport Type ▸ | Alt+G, Alt+J, Alt+K, Alt+H | Perspective, Top, Bottom, Left, Right, Front, Back (für den aktiven Viewport) |
| View Mode ▸ | Alt+4, Alt+3, Alt+2, Alt+5 | Lit, Unlit, Wireframe, Player Collision |
| Show ▸ | | Grid, Icons, Selection Outline, Collision Shapes, Constraints, Contact Points, Bounds, Fog, Sky, Stats, Robot Camera |
| Game View | G | alle Editor-Helfer im aktiven Viewport aus |
| Focus Selection | F | Auswahl ins Bild holen |
| Content Drawer | Ctrl+Space | Content Drawer ein/aus |
| Output Log | | Content Drawer mit dem Log öffnen |
| Fullscreen | F11 | Browser-Vollbild |

**Play**

| Eintrag | Kürzel | Wirkung |
|---|---|---|
| Play | Alt+P | spielen ([11.11](#1111-play-und-simulate)) |
| Simulate | Alt+S | Physik laufen lassen, Kamera frei |
| Pause / Resume | Pause | anhalten / fortsetzen |
| Advance One Frame | . | einen Schritt weiter (nur pausiert) |
| Stop | Esc | zurück zum Bearbeiten |
| Eject from Player / Possess Player | F8 | Spieler verlassen / übernehmen |
| Restart Game | Ctrl+R | Spiel neu starten |
| Keep Simulation Changes | K | simulierte Posen der Auswahl ins Level übernehmen |
| Number of Players ▸ | | 1 Player bis 4 Players (listen server) ([11.17](#1117-multiplayer)) |
| Network Emulation ▸ | | Perfect, LAN (5 ms), Average (50 ms, 1% loss), Bad (150 ms, 5% loss), Terrible (300 ms, 20% loss) |
| Host Online Game... | | Spiel über das Netz anbieten |
| Join Online Game... | | einem angebotenen Spiel beitreten |
| Leave Online Game | | Online-Spiel verlassen |

**Help**

| Eintrag | Kürzel | Wirkung |
|---|---|---|
| Controls... | F1 | Tabelle der Bedienung |
| About Kern Studio | | kurze Beschreibung |

Einträge, die im aktuellen Zustand nichts tun (etwa **Delete** ohne Auswahl oder **New Level** während
Play), sind ausgegraut.

### 11.2.2 Toolbar

Von links nach rechts:

| Element | Kürzel | Wirkung |
|---|---|---|
| Diskette | Ctrl+S | speichern |
| Undo / Redo | Ctrl+Z / Ctrl+Y | der Tooltip nennt den Schritt |
| **+ Add ▾** | | Menü mit allen Katalog-Gruppen ([11.8](#118-content-drawer-und-eingebaute-inhalte)); während Play gesperrt |
| Select / Move / Rotate / Scale | Q / W / E / R | Werkzeug wählen |
| Globus bzw. Würfel | Ctrl+` | Gizmo in Welt- oder lokalen Achsen |
| Raster-Knopf + Auswahl | [ / ] | Grid-Snap an/aus, Rastergröße 1 cm bis 100 m |
| Winkel-Knopf + Auswahl | | Rotations-Snap an/aus, 1°, 5°, 10°, 15°, 30°, 45°, 90° |
| Skalierungs-Knopf + Auswahl | | Skalierungs-Snap an/aus, 0,0625 bis 1 |
| Kamera + Auswahl | Mausrad beim Fliegen | Kamerageschwindigkeit 1 bis 8 |
| Play | Alt+P | spielen; während Simulate übernimmt er den Spieler |
| Simulate | Alt+S | simulieren |
| Pause | Pause | anhalten / fortsetzen |
| Frame | . | einen Schritt weiter (nur pausiert) |
| Stop | Esc | anhalten und zurück zum Bearbeiten |
| Eject | F8 | Spieler verlassen / übernehmen |
| Layout | | 1, 2 oder 4 Viewports |
| **Logic** | | Logik-Graph des Levels öffnen ([11.10](#1110-der-logik-editor)) |
| Paket | | Package Game |
| Panels | | nur auf schmalen Bildschirmen: Outliner, Details, World als Blatt ([11.18](#1118-schmale-bildschirme-und-telefone)) |

### 11.2.3 Viewports

Jeder Viewport hat oben eine eigene Leiste:

| Element | Inhalt |
|---|---|
| ≡ (Viewport options) | Game View (G), Field of View (50°, 70°, 90°, 110°), Focus Selection (F), Bookmarks (anspringen 1–9, setzen Ctrl+1–9), Layout |
| Ansichtstyp | Perspective oder orthografisch: Top, Bottom, Left, Right, Front, Back |
| View Mode | Lit, Unlit, Wireframe, Player Collision |
| Show | Grid, Icons (lights, empties), Selection outline, Collision shapes, Constraints, Contact points, Bounds, Fog, Sky, Stats; die Liste bleibt beim Umschalten offen |
| Maximieren | wechselt zwischen vier Viewports und diesem allein |

Der Klick auf ein Element der Leiste macht den Viewport zum aktiven. Wechselt man von der Perspektive in
eine orthografische Ansicht, schaltet Studio von Lit auf Wireframe, zurück in die Perspektive wieder auf
Lit. **Constraints** ist standardmäßig aus: Sonst zeichnete jede Kette, Tür und jedes Seil seine
Debug-Linien.

Im Viewport selbst zeigt Studio:

- **Achsenkreuz** unten links (X rot, Y grün, Z blau) in der aktiven Perspektive. Z zeigt nach oben.
- **Infozeile**: in der Perspektive „Speed *n*" und die Kameraposition, in orthografischen Ansichten die
  sichtbare Breite in Metern oder Kilometern. Mit dem Show-Flag **Stats** zusätzlich Bilder pro Sekunde
  und während der Simulation Bodies, Kontakte und Schrittzahl.
- **Hinweiszeile** je nach Zustand, etwa beim Fliegen
  „Flying · WASD move · Q/E down/up · wheel speed … · Shift faster · Z/C zoom".
- **Gizmo-Anzeige** beim Ziehen: Verschiebung (Δ in m), Winkel (°) oder Skalierungsfaktoren.

![Vier Viewports: Perspektive und drei orthografische Ansichten](bilder/studio-quad.png)

### 11.2.4 Outliner

Der Outliner listet alle Actors des Levels in den Spalten Sichtbarkeit (Auge), **Label** und **Type**.
Der Typ folgt den Komponenten: Procedural City, Player, Trigger, Pickup, Audio Source, Constrained Mesh,
Physics Mesh, Static Mesh, Point Light, Physics Body oder Empty.

| Aktion | Wirkung |
|---|---|
| Klick | auswählen |
| Shift+Klick | alles zwischen dem zuletzt gewählten und diesem Actor hinzufügen |
| Ctrl+Klick | einzeln hinzufügen oder entfernen |
| Doppelklick | Actor im Viewport fokussieren; bei einem Ordner: umbenennen |
| F2 | umbenennen (direkt in der Zeile, Enter bestätigt, Esc bricht ab) |
| Auge | ein-/ausblenden, bei Ordnern alle Actors darin |
| Pfeil | Kinder bzw. Ordnerinhalt auf- und zuklappen |
| Ziehen auf einen Actor | als Kind anhängen (Weltlage bleibt) |
| Ziehen auf einen Ordner | in den Ordner verschieben |
| Ziehen ins Leere | vom Eltern-Actor lösen und aus dem Ordner nehmen |
| Ordner-Knopf im Panelkopf | neuen Ordner „New Folder" für die Auswahl anlegen |
| Rechtsklick | Kontextmenü (wie im Viewport); bei Ordnern zusätzlich **Rename Folder** und **Remove Folder (keep actors)** |
| Suchfeld | filtert nach Name, Typ und Tags |

Der Actor, den Play steuert, trägt das Abzeichen **PLAYER**; gesperrte Actors ein Schloss. Die Fußzeile
zählt Actors und Auswahl. Ordner gelten nur für Actors der obersten Ebene.

### 11.2.5 Details

Details zeigt die Auswahl: oben das Symbol, der Name (bei Mehrfachauswahl die Anzahl) und die Knöpfe
**Logic** (Graph dieses Actors) und **+ Add** (Komponente hinzufügen). Darunter ein Suchfeld und die
Abschnitte **Actor**, **Transform** und eine Sektion je Komponente. Jede Sektion klappt per Klick auf
ihren Kopf zu; Komponenten haben dort einen Papierkorb zum Entfernen.

Bei Mehrfachauswahl erscheinen nur Komponenten, die alle ausgewählten Actors haben. Unterschiedliche
Werte zeigen „Multiple"; eine Eingabe setzt sie für alle.

Bearbeiten:

| Feldart | Bedienung |
|---|---|
| Zahl | tippen und Enter; Rechenausdrücke wie `2*3` oder `10/4+1` sind erlaubt, Komma gilt als Punkt |
| Zahl ziehen | das Label (oder ein nicht fokussiertes Zahlenfeld) seitwärts ziehen; Shift ×10, Ctrl ×0,1 |
| Zahl mit Pfeiltasten | ↑/↓ im Feld; Shift ×10, Ctrl ×0,1 |
| Regler | Felder mit Bereich 0 bis 1 haben zusätzlich einen Schieberegler |
| Vektor | drei Felder X, Y, Z; bei **Scale** ein Schloss für proportionales Skalieren |
| Farbe | Farbwähler oder Hex-Wert (`#rrggbb`); der Wähler bleibt offen, solange man darin zieht, jede Farbe wirkt sofort |
| Auswahl | Liste (Form, Typ, Modus, Sound, Textur, Ziel-Actor …) |
| Tags | durch Komma getrennt |
| Zurücksetzen | Pfeil rechts neben dem Feld, sichtbar wenn der Wert vom Standard abweicht |
| Rechtsklick auf ein Feld | **Reset to Default**, **Copy Value**, **Paste Value**; in Textfeldern zusätzlich Ausschneiden, Kopieren, Einfügen, Alles auswählen |

**Warum ein Zieh-Vorgang nur ein Undo-Schritt ist:** Studio gibt jedem zusammenhängenden Vorgang
(Ziehen am Label, Schieberegler, Farbwähler) ein Transaktionskennzeichen. Die Engine fasst alle
Änderungen derselben Eigenschaft mit demselben Kennzeichen zu einem Schritt zusammen. Getippte Werte und
Klicks sind eigene Schritte.

Jede Änderung geht über die JSON-Form des Actors, wie sie auch beim Laden einer Datei gelesen wird. Werte
werden deshalb genauso geprüft und begrenzt, als kämen sie aus einer `.klevel`-Datei.

![Ausgewählter Actor mit Move-Gizmo und seinen Komponenten in Details](bilder/studio-selected.png)

### 11.2.6 Statusleiste und Konsole

| Element | Wirkung |
|---|---|
| Content Drawer | Drawer ein/aus (Ctrl+Space) |
| Output Log | Drawer mit dem Log |
| **Cmd** | Konsole; ↑/↓ blättern in früheren Eingaben, Esc verlässt das Feld |
| Statusmeldung | letzte Meldung des Editors |
| Statistik | während Simulate/Play: Bodies, wache Bodies, Gelenke (davon gebrochen), Kontakte; online zusätzlich Rolle, Spieler, Ping, Verlust, Datenrate |
| Lautsprecher | Ton an/aus |
| Pegel | Ausgangspegel links/rechts |
| Auswahl | „*n* of *m* selected" oder Anzahl der Actors |
| Version | laufender Build |

Konsolenbefehle (`help` listet sie):

| Befehl | Wirkung |
|---|---|
| `play`, `simulate`, `stop` | wie die Knöpfe |
| `spawn <item>` | Katalog-Element platzieren, z. B. `spawn physics_cube` (IDs in [11.8](#118-content-drawer-und-eingebaute-inhalte)) |
| `select <name>` | alle Actors auswählen, deren Name den Text enthält |
| `tod <0..1>` | Tageszeit setzen |
| `gravity <x y z>` | Schwerkraft setzen (ohne Werte: 0 0 −9,80665) |
| `camspeed <1-8>` | Kamerageschwindigkeit |
| `stat` | Bilder pro Sekunde und Zählerstände ins Log |
| `clear` | Log leeren |

### 11.2.7 Kontextmenü

Rechtsklick ohne Ziehen in einem Viewport wählt den Actor unter dem Cursor und öffnet das Kontextmenü.
Auf Touch-Geräten öffnet langes Halten (mindestens 0,5 s ohne Bewegung) dasselbe Menü. Studio lässt das
Kontextmenü des Browsers nirgends zu: Überall, wo kein eigenes Menü passt, erscheint ein Studio-Menü.

| Situation | Einträge |
|---|---|
| Bearbeiten, Auswahl vorhanden | Focus, Rename, Cut, Copy, Paste, Duplicate, Delete, **Edit Logic** bzw. **Add Logic (Blueprint)** (ein Actor), **Make Player (control it in Play)** (Actor mit Player Controller), Snap to Floor, Group, **Attach to …** und **Connect with Constraint ▸** (ab zwei Actors), **Detach** (wenn angehängt), Hide, Lock/Unlock, Select Children, **Place Actor ▸** |
| Bearbeiten, nichts ausgewählt | Paste, Select All, Show All, Place Actor ▸ |
| Simulate/Play, Auswahl vorhanden | Focus, Keep Simulation Changes |

**Place Actor ▸** setzt Cube, Sphere, Physics Cube, Point Light, Orb oder Empty Actor an die Stelle,
an der man geklickt hat.

---

## 11.3 Navigation im Viewport

Die Steuerung folgt der Unreal Engine. Alle Kamerabewegungen gelten für den Viewport unter dem Zeiger;
ein Klick macht ihn zum aktiven.

### 11.3.1 Maus und Tastatur

| Eingabe | Wirkung |
|---|---|
| Rechte Maustaste halten + W A S D | Fly Mode: vor, links, zurück, rechts |
| … + Q / E | runter / hoch (auch Bild↓ / Bild↑) |
| … + Pfeiltasten | wie W A S D |
| … + Shift | 2,5-mal schneller |
| … + Mausrad | Kamerageschwindigkeit 1–8 ändern |
| … + Z / C | heranzoomen / wegzoomen (Sichtfeld) |
| … + Maus bewegen | umsehen |
| Rechte + linke Maustaste | hoch/runter und seitwärts verschieben |
| Mittlere Maustaste ziehen | schwenken |
| Alt + linke Maustaste | um die Auswahl kreisen (bzw. um den Punkt unter dem Cursor) |
| Alt + rechte Maustaste | Dolly: näher/weiter zum Drehpunkt |
| Alt + mittlere Maustaste | schwenken |
| Mausrad | vor/zurück; in orthografischen Ansichten zum Cursor zoomen |
| Rechte oder mittlere Maustaste in orthografischen Ansichten | verschieben |
| F | Auswahl einrahmen (ohne Auswahl: das ganze Level) |
| Doppelklick in den Viewport | Auswahl fokussieren |
| Ctrl+1 … Ctrl+9 | Kamera-Lesezeichen setzen |
| 1 … 9 | Lesezeichen anspringen |

Ob Alt+links um die Auswahl kreist, regelt die Einstellung **Orbit around selection** (standardmäßig an).
Ohne Auswahl kreist die Kamera um den Punkt unter dem Cursor, sonst um einen Punkt am Boden vor ihr.

Während man fliegt, gehören W, E, R, Q den Bewegungstasten; Werkzeuge schalten dann nicht um. Bewegungen
lesen die physische Tastenposition (WASD bleibt auf AZERTY und QWERTZ an derselben Stelle), Kürzel wie
Ctrl+Z dagegen die beschriftete Taste.

Die Kamerageschwindigkeit in Metern pro Sekunde:

| Stufe | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 |
|---|---|---|---|---|---|---|---|---|
| m/s | 1 | 2,5 | 5 | 10 | 20 | 40 | 80 | 160 |

Stufe 4 ist der Standard.

### 11.3.2 Finger

Im Editor und in Simulate steuert man die Kamera wie in einem 3D-Betrachter am Telefon:

| Geste | Wirkung |
|---|---|
| Ein Finger ziehen | um den Punkt unter dem Finger kreisen (mit „Orbit around selection" um die Auswahl); in orthografischen Ansichten verschieben |
| Zwei Finger | zum Punkt zwischen ihnen zoomen (auseinander: näher) und mit ihnen verschieben |
| Tippen | auswählen |
| Zweimal tippen | Auswahl einrahmen (wie F) |
| Lange halten | Kontextmenü |
| Finger auf einem Gizmo-Griff | verschieben, drehen, skalieren wie mit der Maus |
| Finger in einem anderen Viewport | macht ihn zum aktiven |

Beim Zoomen mit zwei Fingern bleibt, was unter den Fingern liegt, unter ihnen. Der Maßstab ist der
Abstand zu diesem Punkt; deshalb fühlt sich die Geste an einer Socke so an wie an einem Häuserblock.
Nach einem Pinch kreist der nächste Ein-Finger-Zug um den Punkt, zu dem man gezoomt hat, solange nichts
anderes die Kamera bewegt oder die Auswahl ändert. Die Seite selbst zoomt beim Pinch nie mit.

---

## 11.4 Tastenkürzel

Die vollständige Belegung, wie sie `Studio::key` in `kern/src/studio/ui.cpp` umsetzt. **F1** zeigt die
wichtigsten Kürzel auch in Studio.

### 11.4.1 Bearbeiten

| Taste | Wirkung |
|---|---|
| Q / W / E / R | Werkzeug Select / Move / Rotate / Scale |
| Leertaste | Move → Rotate → Scale → Move |
| Ctrl+` | Welt- / lokale Achsen |
| [ / ] | Grid-Raster kleiner / größer (0,01; 0,05; 0,1; 0,25; 0,5; 1; 2; 5; 10; 50; 100 m) |
| Entf, Rücktaste | Auswahl löschen |
| F | Auswahl fokussieren |
| End | Auswahl auf den Boden setzen |
| H | Auswahl ausblenden |
| Shift+H | alle anderen ausblenden |
| Ctrl+H | alles einblenden |
| G | Game View im aktiven Viewport |
| Esc | Gizmo-Zug abbrechen, sonst Auswahl aufheben |
| F2 | primären Actor umbenennen |
| F11 | Vollbild |
| F1 | Controls-Dialog |
| Ctrl+Z / Ctrl+Shift+Z / Ctrl+Y | Undo / Redo / Redo |
| Ctrl+C / X / V / D | Kopieren / Ausschneiden / Einfügen / Duplizieren |
| Ctrl+A | alles auswählen |
| Ctrl+G | gruppieren |
| Ctrl+S / Ctrl+Shift+S | Speichern / Speichern unter |
| Ctrl+O | Level öffnen |
| Ctrl+N | New Level |
| Ctrl+Space | Content Drawer |
| Ctrl+1 … 9 / 1 … 9 | Kamera-Lesezeichen setzen / anspringen |
| Alt+G / J / H / K | Perspektive / Top / Front / Side (Right) |
| Alt+2 / 3 / 4 / 5 | Wireframe / Unlit / Lit / Player Collision |
| Alt+P / Alt+S | Play / Simulate |
| Alt + Gizmo ziehen | Kopie ziehen |

Kopierte Actors landen als Text (`{"format": "kern-actors", "actors": […]}`) in der Zwischenablage des
Systems. Ein Ctrl+V in einem anderen Tab oder einer anderen Sitzung fügt sie dort ein.

### 11.4.2 Play und Simulate

| Taste | Wirkung |
|---|---|
| Esc | Stop (bricht zuerst einen laufenden Gizmo-Zug ab) |
| F8 | Eject (Spieler verlassen) bzw. Possess (Spieler übernehmen) |
| Shift+F1 | Maus freigeben, ohne anzuhalten |
| Pause oder Alt+Shift+P | Pause / Resume |
| Alt+P | Play beenden |
| Alt+S | auf Simulate umschalten |
| Ctrl+R | Spiel neu starten (nicht beim Soldaten: dort ist Ctrl Ducken, R Nachladen) |
| . | einen Schritt weiter, wenn pausiert (Simulate oder nach Eject) |
| K | Keep Simulation Changes (Simulate) |
| F | Auswahl fokussieren (Simulate oder nach Eject) |
| G | Game View (Simulate oder nach Eject) |

Wenn der Browser die Maus gefangen hat (Pointer-Lock), schluckt er das erste Esc, um sie freizugeben.
Studio wertet diesen Verlust des Pointer-Locks als Esc und stoppt sofort, sodass ein Druck genügt. Ein
Fensterwechsel gibt die Maus nur frei.

Die Spieltasten selbst:

| Eingabe | Wirkung |
|---|---|
| W A S D, Pfeiltasten | bewegen, lenken, Gas |
| Maus (nach Klick in den Viewport) | umsehen |
| Leertaste | springen; beim Flyer hoch (auch E) |
| Q, Shift links, Ctrl links | beim Flyer runter |
| Shift | Boost bzw. Sprint |
| Linke Maustaste | feuern (Soldat); der Klick, der die Maus fängt, feuert nicht |
| Rechte Maustaste | zielen (Soldat) |
| R | nachladen |
| C oder Ctrl links | ducken |
| F | benutzen (Interact) |
| Mausrad | Spielkamera zoomen |
| 1–7, V | Bewegungen von Plumo ([11.13](#1113-die-plumo-steuern)) |

Gamepads und Touch-Steuerung beschreibt [11.12](#1112-mobil-touch-und-gamepad).

### 11.4.3 Logik-Editor

| Taste | Wirkung |
|---|---|
| F7 | Compile |
| Ctrl+F | Suchen |
| F9 | Breakpoint setzen/entfernen |
| Q | Verbindungen geraderichten |
| Shift+W / A / S / D | ausrichten |
| B / S / D / N / O / G / L + Klick | Branch / Sequence / Delay / Do N / Do Once / Gate / For Loop einfügen |
| C | Kommentar um die Auswahl |
| Ctrl+C / X / V / D | Kopieren / Ausschneiden / Einfügen / Duplizieren |
| Entf | löschen |
| F / Home | Auswahl / alles einrahmen |
| Ctrl+Z | Rückgängig |
| Esc | zurück zum Viewport |

Solange der Logik-Editor offen ist, gehören Buchstaben und Entf dem Graphen; nur Ctrl+Z, Ctrl+Y, Ctrl+S
und F1 erreichen das Level.

---

## 11.5 Auswählen, Gizmos und Snapping

### Auswählen

| Eingabe | Wirkung |
|---|---|
| Linksklick | auswählen; Shift ergänzt, Ctrl schaltet um |
| Klick ins Leere | Auswahl aufheben (ohne Shift/Ctrl) |
| Linke Maustaste ziehen | Rahmenauswahl; Shift ergänzt, Ctrl schaltet um |
| Outliner | siehe [11.2.4](#1124-outliner) |

Linkes Ziehen bewegt nie die Kamera und nie einen Actor, auch nicht, wenn man auf einem Actor anfängt;
es zeichnet immer ein Auswahlrechteck (der Actor, auf dem man begann, gehört dann dazu). Actors bewegt
wie in Unreal nur der Gizmo. Die Rahmenauswahl nimmt jeden sichtbaren, nicht gesperrten Actor, dessen
Ursprung im Rechteck liegt; die prozedurale Stadt bleibt außen vor.

Lichter, Empties und andere Actors ohne Mesh wählt man über ihr Symbol im Viewport (Show › Icons).
Gesperrte Actors (Lock) lassen sich im Viewport nicht wählen: Der Klick geht durch sie hindurch zu dem,
was dahinter liegt.

Die **primäre** Auswahl ist der zuletzt gewählte Actor. An ihm sitzt der Gizmo, und Details zeigt seinen
Namen.

### Gizmos

| Werkzeug | Griffe |
|---|---|
| Move (W) | drei Achsenpfeile, drei Flächen-Quadrate für je zwei Achsen, Mittelpunkt für die Bildschirmebene |
| Rotate (E) | drei Ringe |
| Scale (R) | drei Achsen und ein Mittelgriff für gleichmäßiges Skalieren |
| Select (Q) | kein Gizmo |

Achsen sind rot (X), grün (Y) und blau (Z); der Griff unter dem Zeiger leuchtet gelb. Mit dem Globus- bzw.
Würfel-Knopf (Ctrl+`) richtet sich der Gizmo nach der Welt oder nach dem primären Actor. Bewegt werden
alle ausgewählten Actors, deren Vorfahren nicht selbst ausgewählt sind; Kinder folgen ihren Eltern.

**Alt + Gizmo ziehen** zieht eine Kopie und lässt das Original stehen. Kopie und Bewegung sind ein
einziger Undo-Schritt. **Esc** während des Ziehens bricht ab und stellt den Ausgangszustand her.

### Snapping

| Raster | Standard | Wirkung |
|---|---|---|
| Grid | an, 0,5 m | Move rastet auf Vielfache der Rastergröße ein (in Weltachsen auf absolute Positionen, in lokalen Achsen auf Schritte) |
| Rotation | an, 15° | Rotate dreht in Schritten |
| Scale | aus, 0,25 | Scale ändert Faktoren in Schritten |

Grid-Snap gilt auch beim Platzieren neuer Actors: Ihre X- und Y-Position wird auf das Raster gelegt.
Eine Auswahl in der Toolbar schaltet das jeweilige Raster gleich mit ein. Der perspektivische Viewport
zeichnet ein Bodenraster, orthografische Ansichten ihr eigenes.

---

## 11.6 Levels, Actors und Komponenten

Ein Level besteht aus **World Settings**, **Actors**, Texturen, Sounds, dem Logik-Graphen des Levels und
der Editor-Kamera. Einheiten sind Meter, Sekunden und Kilogramm, Z zeigt nach oben. Rotationen stehen als
Euler-Winkel in Grad (x Roll, y Pitch, z Yaw), Farben als sRGB.

Ein Actor hat Name, Eltern-Actor, Outliner-Ordner, Tags, Hidden, Locked, Transform und Komponenten.

### 11.6.1 Actors anlegen

1. Content Drawer öffnen (Ctrl+Space) und ein Element anklicken: Es erscheint dort, wohin die aktive
   Kamera blickt (auf dem getroffenen Objekt, sonst auf dem Boden vor der Kamera, sonst 10 m voraus).
2. Oder ein Element in einen Viewport **ziehen**: Es landet genau dort, wo man loslässt.
3. Oder **+ Add** in der Toolbar, das Kontextmenü **Place Actor ▸** oder die Konsole (`spawn <id>`).

Neue Actors bekommen eindeutige Namen („Cube", „Cube 2", …) und sind danach ausgewählt. Wer einen Actor
aus der Gruppe **Players** platziert, macht ihn zum Spieler; das Log meldet
„… is the player now - press Play (Alt+P) to control it".

### 11.6.2 Hierarchie und Ordner

- **Anhängen:** zwei oder mehr Actors wählen, den künftigen Eltern-Actor zuletzt, dann Kontextmenü
  **Attach to …**; oder im Outliner auf einen Actor ziehen. Die Weltlage bleibt erhalten.
- **Lösen:** **Detach** im Kontextmenü oder im Outliner ins Leere ziehen.
- **Gruppieren:** Ctrl+G legt einen leeren Actor „Group" in die Mitte der Auswahl und hängt sie daran.
- **Ordner:** Feld **Outliner folder** in Details, Ordner-Knopf im Outliner oder Ziehen auf einen Ordner.
- **Kinder wählen:** **Select Children** im Kontextmenü.

Kind-Actors ohne eigenen Rigid Body werden beim Simulieren als Compound in den Body des nächsten
Vorfahren geschweißt. Meshes mit Kollision, Pickups und Trigger ohne Body bekommen einen impliziten
statischen Body, bei Animation durch Rotator oder Mover einen kinematischen ([Kapitel 6](06-physik.md)).

### 11.6.3 Komponenten

**+ Add** in Details bietet nur an, was die Auswahl noch nicht hat; Collider und Constraints sind Listen
und können mehrfach vorkommen. Procedural City gibt es nur für einen einzelnen Actor ohne Eltern.

| Komponente | Wichtigste Felder |
|---|---|
| **Static Mesh** | Shape (Box, Sphere, Cylinder, Cone, Capsule, Plane, Torus, Ramp, Mesh), Size bzw. Radius/Height/Segments (nur was die Form nutzt); *Physics:* Generate collision, Friction, Restitution, Rolling resistance; *Material:* Base color, Roughness, Metallic, Emissive color und strength, Opacity, Unlit, Cast shadow, Two sided, Texture, Texture tiling |
| **Point Light** | Light color, Intensity, Attenuation radius |
| **Rigid Body (Box3D)** | Mobility (Static, Kinematic, Dynamic), Mass (0 = aus der Dichte), Linear/Angular damping, Gravity scale, Continuous (bullet), Start awake, Allow sleep; *Constraints:* Lock position, Lock rotation (je X/Y/Z); *Initial state:* Initial velocity, Initial spin; *Sound:* Impact sound, Impact threshold |
| **Collider** (mehrere) | Shape (Fit to mesh, Box, Sphere, Capsule, Convex hull), Offset, Rotation, Box size, Radius, Capsule length; *Physical material:* Density, Friction, Restitution, Rolling resistance; *Collision:* Sensor, Channel bits, Collides with bits, Group index; *Sound:* Impact sound (Automatic = der des Bodys) |
| **Constraint** (mehrere) | Type (Hinge, Slider, Ball, Fixed, Distance/rope, Wheel, Motor, Parallel, No collision), Connected actor (World = fester Punkt), Anchor und Axis (lokal), Collide connected; *Limits*, *Motor* (Speed, Max force/torque), *Spring* (Frequency, Damping ratio), Rest length; *Breaking:* Break force, Break torque, Break sound. Eine Info-Zeile nennt die Einheiten (Grad oder Meter) |
| **Player Controller** | Movement (Hover car, Rolling ball, Character, Flyer, Vehicle, Soldier, Walker), Max speed, Acceleration, Turn rate, Jump speed, Boost, Boost speed, Hover height; *Camera:* Chase, First person, Top down mit Distance, Height, Field of view; *Soldier:* Crouch speed, Eye height, Look sensitivity, Head bob; *Walker:* Autonomous, Walking network, Actions to servo goals, Kern extras; *Sound:* Engine loop, Jump, Boost |
| **Weapon** | Type (Rifle, Pistol, Shotgun, Sniper rifle), Damage, Headshot multiplier, Fire rate, Automatic, Pellets, Range, Impulse; *Ammo:* Magazine, Reserve, Reload time; *Handling:* Hip/Aimed spread, Recoil, Aim field of view; *Sound:* Fire, Reload |
| **Health** | Health, Armor, Regenerate after, Regeneration, Respawn, Respawn delay, Points for a kill; *Sound:* Hit, Down; *Explosion:* radius, damage |
| **Pickup** | Points, Burst color, Respawn after, Sound, Heals, Ammo |
| **Trigger** | Action (Goal, Hazard, Checkpoint, Message, Jump pad, Teleport, Damage zone, Logic event only), Message, Only once, Strength, Teleport target, Sound (Automatic = passend zur Aktion) |
| **Rotator** | Rotation rate (°/s je Achse) |
| **Mover** | Offset, Period, Phase, Motion (Sine, Ping-pong, Loop) |
| **Procedural City** | Seed, Blocks per side, Block pitch, Street width, Tallest building, Trees, Traffic cars, Lit windows at night |
| **Audio Source** | Sound, Volume, Pitch, Loop, Play on start, Start delay, Bus; *Attenuation:* Spatial (3D), Inner radius, Falloff distance, Roll-off, Doppler, Pitch by speed |

Was die Felder physikalisch bedeuten und welche Werte sich bewähren, steht in [Kapitel 6](06-physik.md)
(Body, Collider, Constraint), [Kapitel 7](07-audio.md) (Audio Source, Sounds) und
[Kapitel 8](08-spiel-laufzeit.md) (Controller, Pickup, Trigger, Weapon, Health).

### 11.6.4 Gelenke mit „Connect"

Schneller als ein Constraint von Hand:

1. Den Actor wählen, der am Gelenk hängen soll (Body B).
2. Mit Shift oder Ctrl den Actor dazu wählen, an dem er hängt (Body A), **als Letztes**.
3. Rechtsklick › **Connect with Constraint ▸** › Hinge, Slider, Ball, Fixed (weld), Distance / rope, Wheel
   oder Motor.

Studio setzt den Anker auf halbem Weg zwischen den beiden Actors (außer bei Distance und Wheel) und gibt
dem gehängten Actor einen Rigid Body, falls er keinen hat. Mehrere gehängte Actors bekommen je ein
Gelenk zum zuletzt gewählten.

### 11.6.5 Der Spieler

Play steuert einen Actor mit **Player Controller**. Festlegen lässt er sich auf drei Arten: einen Actor
aus **Players** platzieren, Rechtsklick › **Make Player (control it in Play)** oder World Settings ›
**Player**. Hat das Level keinen Actor mit Controller, startet Play stattdessen eine Simulation und meldet
„Nothing to control yet".

### 11.6.6 Fehler und Warnungen

Vor Play und Simulate prüft Studio das Level (`game::validate`). **Fehler** wie verwaiste Referenzen oder
ungültige Werte verhindern den Start: „Fix the errors above to play". **Warnungen** erscheinen nur im
Output Log.

---

## 11.7 Undo und History

Jeder Schritt speichert das vollständige Dokument nach der Änderung. Undo und Redo sind dadurch exakt:
Es gibt keine Umkehroperationen, die auseinanderlaufen könnten, und das Ergebnis hängt nur von der Folge
der Änderungen ab, nie vom Timing. Studio hält bis zu 500 Schritte.

- **Edit › History...** zeigt alle Schritte; ein Klick springt zu diesem Stand.
- Aufeinanderfolgende Änderungen derselben Eigenschaft innerhalb eines Ziehvorgangs sind ein Schritt
  ([11.2.5](#1125-details)).
- Eine Änderung, die nichts ändert, erzeugt keinen Schritt.
- Während Play und Simulate ist das Level gesperrt: „Stop playing to edit the level".

---

## 11.8 Content Drawer und eingebaute Inhalte

Der Content Drawer (Ctrl+Space, Statusleiste **Content Drawer**) hat vier Tabs und ein Suchfeld. Er
öffnet sich unter den Viewports.

### 11.8.1 Place Actors

Ein Klick platziert, Ziehen in einen Viewport platziert an der Ablagestelle. Der Tooltip jeder Kachel
beschreibt das Element. In der Konsole und in Befehlen heißen die Elemente mit ihrer ID.

| Gruppe | Element | ID | Beschreibung |
|---|---|---|---|
| Players | Soldier | `soldier` | Ego-Shooter: Gewehr, Gesundheit, Respawn; Maus, Gamepad, Touch; online-fähig |
| Players | Character | `character` | Third-Person-Läufer: gehen, sprinten, springen, Stufen steigen |
| Players | Hover Car | `hover_car` | schnelles Box3D-Hovercar mit Boost |
| Players | Rolling Ball | `rolling_ball` | Physik-Kugel zum Rollen und Springen |
| Players | Drone | `drone` | Flyer: Leertaste/E hoch, Q/Ctrl runter |
| Players | Plumo | `plumo` | die Roboter-Ente auf Pollen Robotics' trainierten Netzen |
| Players | Plumo (Rollers) | `plumo_rollers` | Plumo auf Rollschuhen: `roller.onnx` fährt, 2 duckt sich |
| Gameplay | Orb | `orb` | leuchtendes Pickup, 1 Punkt |
| Gameplay | Star | `star` | drehendes Pickup, 10 Punkte |
| Gameplay | Health Pack | `health_pack` | heilt 50; bleibt liegen bei voller Gesundheit; kommt nach 15 s wieder |
| Gameplay | Ammo Box | `ammo_box` | 60 Schuss für Soldaten; kommt nach 20 s wieder |
| Gameplay | Goal | `goal` | Trigger, der das Spiel gewinnt |
| Gameplay | Hazard | `hazard` | Trigger, der das Spiel verliert |
| Gameplay | Damage Zone | `damage_zone` | verletzt, wer hineinläuft (25 je Eintritt) |
| Gameplay | Checkpoint | `checkpoint` | Respawn-Punkt |
| Gameplay | Jump Pad | `jump_pad` | schleudert Spieler hoch und nach vorn |
| Gameplay | Teleporter | `teleporter` | zwei verbundene Felder: auf A treten, bei B ankommen |
| Gameplay | Message Trigger | `message` | zeigt bei Berührung einen Text |
| Gameplay | Moving Platform | `moving_platform` | kinematische Plattform mit Mover |
| Gameplay | Spinner | `spinner` | drehender kinematischer Balken |
| Gameplay | Target Dummy | `target_dummy` | beschießbares Ziel mit Gesundheit und Kopf; kommt wieder |
| Gameplay | Explosive Barrel | `explosive_barrel` | explodiert bei Beschuss: stößt und verletzt, was nah ist |
| Shapes | Cube | `cube` | 1-m-Würfel mit Kollision |
| Shapes | Sphere | `sphere` | 1-m-Kugel |
| Shapes | Cylinder | `cylinder` | stehender Zylinder |
| Shapes | Cone | `cone` | stehender Kegel |
| Shapes | Capsule | `capsule` | Kapsel entlang Z |
| Shapes | Plane | `plane` | 10 × 10 m Boden |
| Shapes | Torus | `torus` | Ring |
| Shapes | Ramp | `ramp` | Keil zum Hinauffahren oder -rollen |
| Shapes | Empty Actor | `empty` | nur Transform: Eltern oder Drehpunkt |
| Physics | Physics Cube | `physics_cube` | dynamische Box3D-Kiste |
| Physics | Physics Sphere | `physics_sphere` | dynamische Kugel |
| Physics | Physics Barrel | `physics_barrel` | dynamischer Zylinder (konvexe Hülle) |
| Physics | Crate Stack | `crate_stack` | Pyramide aus 10 Kisten |
| Physics | Pendulum | `pendulum` | Kugel an einem Distance-Gelenk |
| Physics | Hinged Door | `hinged_door` | Tür an einem gefederten Scharnier mit Grenzen |
| Physics | Chain | `chain` | 8 Glieder an Ball-Gelenken |
| Physics | See-Saw | `see_saw` | Brett auf einem Scharnier |
| Physics | Sock | `plumo_sock` | zerknüllte Socke im Maßstab von Plumo (weiche Klumpen an Federn); die Ente hebt sie mit 2 oder von selbst auf |
| Lights | Point Light | `point_light` | farbiges Licht mit Radius |
| Audio | Audio Source | `audio_source` | 3D-Klangquelle (Maschinenbrummen) |
| Audio | Ambient Wind | `ambient_wind` | Wind in Schleife, überall hörbar |
| Audio | City Ambience | `city_ambience` | Stadtgeräusch in Schleife mit Abfall |
| Audio | Radio | `radio` | spielt den ruhigen Musik-Loop in 3D |
| World | Procedural City | `city` | Blöcke, Straßen, Bäume und Verkehr |

Beim Einfügen von Plumo stellt Studio die Physik auf 100 Hz mit 16 Sub-Steps (World Settings ›
Step rate und Sub-steps) und meldet das im Log: Servogetriebene Roboter sind lange Gelenkketten, die
ohne genug Sub-Steps gedehnt würden, und ihr Netz braucht seinen festen Takt.

### 11.8.2 Textures

Der Tab zeigt alle Texturen des Levels mit Vorschau und Größe.

- **Import image** (oder File › Import Texture..., oder ein Bild auf die Seite ziehen) holt PNG, JPEG
  oder WebP ins Level. Bilder über 2048 px Kantenlänge verkleinert Studio. Ist beim Import etwas
  ausgewählt, bekommt jedes ausgewählte Mesh die Textur, und seine Farbe wird weiß.
- **Doppelklick** auf eine Textur setzt sie auf die Auswahl.
- Der Papierkorb entfernt sie; Meshes, die sie nutzten, verlieren sie.

Texturen liegen als Data-URL im Level; die `.klevel`-Datei ist damit in sich vollständig.

### 11.8.3 Sounds

Der Tab listet die importierten Sounds und die eingebauten, nach Kategorien.

- **Klick** spielt einen Sound an (der Knopf oben „Playing preview" stoppt ihn).
- **Doppelklick** setzt ihn als Sound einer Audio Source auf die Auswahl (die Komponente kommt dazu,
  falls sie fehlt).
- **Ziehen in einen Viewport** platziert eine Audio Source mit diesem Sound.
- **Import audio** (oder Dateien auf die Seite ziehen) holt WAV, MP3, OGG, M4A, FLAC und alles, was der
  Browser dekodieren kann, bis 24 MB je Datei. Undo nimmt einen Import zurück.

Jedes Sound-Feld in Details hat eine Vorhör-Taste. Ton beginnt erst nach dem ersten Klick oder
Tastendruck (Browser-Regel); ohne Web Audio arbeitet Studio stumm. Die Sound-Engine beschreibt
[Kapitel 7](07-audio.md).

### 11.8.4 Output Log

Alle Meldungen mit Uhrzeit, Warnungen und Fehler farbig. Das Log zeigt die letzten 200 Einträge.
Während Play und Simulate erscheinen hier auch Spielereignisse; `clear` in der Konsole leert es.

---

## 11.9 World Settings

Der Tab **World Settings** neben Details (auf schmalen Bildschirmen der Tab **World**) gilt für das ganze
Level.

| Feld | Bedeutung |
|---|---|
| Level name | Name des Levels (auch Dateiname beim Speichern) |
| *Game* | |
| Game title | Titel im HUD und im exportierten Spiel |
| Player (controlled in Play) | welcher Actor gesteuert wird |
| Observer camera | Play startet mit freier Kamera: fliegen, zuschauen, Körper ziehen ([11.11.4](#11114-observer-camera)) |
| Win condition | None (sandbox), Collect all pickups, Reach a goal trigger, Survive the time limit, Reach a target score |
| Time limit (0 = none) | Sekunden |
| Target score | Punkte für „Reach a target score" |
| Kill Z (respawn below) | unter dieser Höhe wird der Spieler zurückgesetzt |
| *Sky and light* | |
| Time of day | 0 bis knapp 1 |
| Day length (0 = frozen) | Sekunden für einen ganzen Tag |
| Sun azimuth | Grad |
| Fog density | Nebeldichte |
| Exposure | Belichtung |
| Ambient occlusion radius (0 = off) | Meter |
| Shadow distance (smaller: sharper) | Meter der Sicht, die die Schattenkarte der Sonne abdeckt (2–2000, Standard 80; das Plumo-Level 10) |
| Clouds | Wolken an/aus |
| Endless ground | unendlicher Boden |
| Ground color | Bodenfarbe |
| *Physics (Box3D)* | |
| Gravity | m/s² als Vektor |
| Sub-steps | 1 bis 16 |
| Ground friction | Reibung des Bodens |
| Ground bounciness | Restitution des Bodens |
| Contact stiffness | Hz |
| Contact damping ratio | Dämpfung der Kontakte |
| Contact push-out speed | m/s |
| Step rate | 15 bis 480 Hz |
| *Audio* | |
| Master volume | Gesamtlautstärke |
| Music, Music volume | Musikstück und Lautstärke |
| Ambience, Ambience volume | Hintergrundklang und Lautstärke |
| Reverb | Hallanteil |
| Doppler | Stärke des Doppler-Effekts |

Die Physikwerte erklärt [Kapitel 6](06-physik.md), Himmel und Licht [Kapitel 4](04-grafik.md).

---

## 11.10 Der Logik-Editor

Jeder Actor und das Level selbst können einen Logik-Graphen tragen, nach dem Vorbild der Blueprints der
Unreal Engine. Geöffnet wird er über **Logic** in der Toolbar (Graph des Levels), über **Logic** in
Details oder über **Edit Logic** / **Add Logic (Blueprint)** im Kontextmenü (Graph des Actors). Der
Editor deckt dann die Viewports ab.

| Bereich | Inhalt |
|---|---|
| Toolbar | **Viewport** (zurück, Esc), **Compile** (F7, mit Statusanzeige OK / Warnung / Fehler), **Save**, **Find** (Ctrl+F), Auswahl des Blueprints (Level oder Actor), **Templates**, **C++** (Export), Rechteckauswahl (Touch), Hilfe |
| My Blueprint | Graphen (Event Graph, State Machines), Variablen mit Typfarbe und Live-Wert beim Spielen, Events |
| Graph | Tabs, Zoomstufen wie in Unreal, Knoten und Drähte |
| Details | ausgewählter Knoten, Variable, Kommentar, State oder Transition |
| Compiler Results / Find Results | Probleme und Suchtreffer; ein Klick springt zum Knoten |

Ein leerer Graph bietet Startknöpfe an: **Begin Play**, **On Interact**, **On Begin Overlap**,
**Event Tick** und **Templates…**. Die Vorlagen fügen fertige Graphen ein:

| Vorlage | Was sie tut |
|---|---|
| Door that opens on Interact | Interact schaltet zwischen Öffnen und Schließen, mit Ton |
| Score zone (overlap +10) | Betreten gibt 10 Punkte, zeigt „+10" und spielt einen Ton |
| Spawner (a cube every 3 s) | alle 3 s eine Physics Cube, höchstens 10 |
| Traffic light (state machine) | Ampel als State Machine (Grün, Gelb, Rot) |
| Robot: walk and turn away with its ToF | Plumo läuft und dreht ab, wenn ihr ToF näher als 1,6 m etwas sieht |

Ein Beispiel, wie es der Browser-Test bedient: Basic-Level, **Logic**, **Begin Play**, vom Ausgang
„then" einen Draht ins Leere ziehen, „show message" tippen, Enter, Text eintragen, **Viewport**, einen
Rolling Ball platzieren, **Play**: Das HUD zeigt den Text.

Beim Start von Play oder Simulate kompiliert Kern jeden Graphen zu WebAssembly-Maschinencode; Graphen mit
Fehlern lassen Play nicht starten. **C++** lädt den Graphen als lesbaren C++-Quelltext herunter, der in
ein Spiel gebaut als nativer Code läuft ([Kapitel 12](12-eigene-spiele.md#1243-logik-graphen-als-c)).
Mit F9 gesetzte Breakpoints halten das Spiel an, sobald der Knoten läuft. Alle Knoten, Variablen, State
Machines, die Bedienung im Detail und die drei Ausführungsstufen beschreibt [Kapitel 9](09-logik.md).

---

## 11.11 Play und Simulate

Studio kennt drei Zustände: **Editing**, **Simulating** und **Playing**. In beiden laufenden Zuständen
ist das Level gesperrt; Änderungen in der Simulation verschwinden beim Stop, außer man übernimmt sie mit
**Keep Simulation Changes**.

### 11.11.1 Play

**Play** (Alt+P, Play-Knopf) startet das Spiel im aktiven Viewport: Die Spiel-Laufzeit baut die
Box3D-Welt, der Spieler bekommt die Steuerung, die Kamera folgt ihm, und das HUD zeigt Titel, eingesammelte
Pickups, Punkte, verbleibende oder vergangene Zeit, Bestzeit, Tempo in km/h mit Boost-Balken (beim Soldaten
Fadenkreuz, Gesundheit, Munition). Leiste und Infozeile des Viewports verschwinden.

1. Auf dem Desktop fängt Studio die Maus (Pointer-Lock). Ist sie frei, steht im Viewport
   „Click the viewport to control the game · Esc stops · F8 ejects"; ein Klick fängt sie wieder.
2. Bei Sieg oder Niederlage erscheint ein Banner „Victory" bzw. „Game over" mit **Play again**.
3. **Ctrl+R** oder Play › Restart Game startet neu.
4. **Esc** oder **Stop** beendet das Spiel.

![Play im Viewport: das Hovercar in Orb Run mit HUD](bilder/studio-play.png)

### 11.11.2 Eject und Possess

**F8** verlässt den Spieler: Das Spiel läuft weiter, die Kamera ist frei, und man kann wie beim
Bearbeiten fliegen, auswählen und Körper greifen. Studio wechselt dabei in den Zustand Simulating. Noch
einmal **F8** (oder der Play-Knopf) übernimmt den Spieler wieder.

### 11.11.3 Simulate

**Simulate** (Alt+S) lässt die Physik und die Spielregeln laufen, ohne einen Spieler zu steuern. Die
Kamera bleibt die des Editors.

- **Körper greifen:** Linke Maustaste auf einem dynamischen Körper (oder ein Finger) greift ihn mit dem
  Mouse Joint. Ziehen bewegt ihn, den Zeiger nach oben ziehen hebt ihn hoch, das Mausrad bzw. ein zweiter
  Finger (auseinander/zusammen) holt ihn näher oder schiebt ihn weg, Loslassen wirft ihn mit seinem
  Schwung. Ctrl ist dafür nicht nötig. Ein kurzer Klick wählt den Körper aus. Der Hinweis im Viewport
  lautet „Simulating · drag grabs bodies · K keeps poses · Esc stops“, der Controls-Dialog (F1) nennt
  „Drag (Simulate) – Grab and throw physics bodies“.
- **Eine Plumo am Rumpf** hält der Griff zusätzlich in ihrer Ausrichtung, wie eine Hand den
  Roboter hält, statt sie am Punkt pendeln zu lassen (`begin_grab` mit `hold_rotation`). So
  hochgehoben, pausiert sie wie der echte Roboter ([Kapitel 13](13-plumo.md)).
- **Warum der Körper physikalisch bleibt:** Ein Motor-Gelenk mit Feder (7,5 Hz, kritisch gedämpft) zieht
  den gegriffenen Punkt zum Zeiger. Der Körper stößt an, kippt und schwingt wie jeder andere.
- **K** übernimmt die simulierten Posen der ausgewählten Actors ins Level (ein Undo-Schritt) und beendet
  die Simulation.
- **Pause**, dann **.** geht Schritt für Schritt.

Das Show-Flag **Collision Shapes** zeigt die Box3D-Formen, **Constraints** die Gelenke, **Contact Points**
die Kontakte. Die Statusleiste zählt Bodies, wache Bodies, Gelenke, gebrochene Gelenke und Kontakte.

![Simulate mit eingeblendeten Kollisionsformen](bilder/studio-simulate.png)

![Physics Lab: alle Gelenktypen in der Simulation](bilder/studio-physics-lab.png)

### 11.11.4 Observer camera

Mit World Settings › **Observer camera** startet Play nicht hinter dem Spieler, sondern mit freier
Kamera: Das Spiel läuft, man schaut zu und greift ein. Das Level Plumo nutzt das.

| Gerät | Bedienung |
|---|---|
| Desktop | W A S D fliegen, Q / E runter / hoch, Shift schneller, ohne gehaltene Maustaste; Ziehen ins Leere schaut sich um; rechte Maustaste und Mausrad wie im Editor |
| Telefon | ein Finger ins Leere schaut sich um; zwei Finger zoomen und schwenken |
| Beide | einen Körper ziehen, heben und werfen wie in Simulate |
| F8 | den Spieler übernehmen (etwa die Ente per Gamepad), noch einmal F8 schaut wieder zu |

---

## 11.12 Mobil, Touch und Gamepad

### 11.12.1 Welches Gerät gerade steuert

Die Engine erkennt Touch, Maus mit Tastatur und Gamepad selbst und schaltet auf das zuletzt benutzte um
(`InputArbiter` in `kern/src/game/touch.cpp`, nach den Regeln von SpawnCity):

- Ein Finger schaltet auf Touch.
- Eine Maus schaltet nur auf Geräten mit feinem Zeiger auf Desktop; eine Maus am Tablet behält das
  Touch-Layout.
- Jeder echte Tastendruck schaltet auf Desktop, auch am Telefon.
- Ein Gamepad zählt, sobald ein Knopf gedrückt oder ein Stick über 0,22 ausgelenkt wird.
- Pointer-Lock gibt es nur mit Maus.

### 11.12.2 Play auf Touch-Geräten

Play auf einem Touch-Gerät füllt den ganzen Bildschirm und schaltet auf einen Viewport. Oben links stehen
**⟵ EXIT** (Stop) und **‖ PAUSE** bzw. **▶ RESUME**.

Die Engine legt die Bedienelemente selbst an (`TouchControls`), die Seite zeichnet sie nur:

- **Zwei Sticks** von 168 px in den unteren Ecken, bündig mit den sicheren Rändern; kleiner (mindestens
  72 px) nur, wenn zwei nicht passen. Links bewegen (Beschriftung **MOVE**, bei Fahrzeugen
  **STEER / GAS**, beim Flyer **FLY**), rechts schauen (**LOOK**) mit Drehen am Rand: Zieht man den
  rechten Stick über seinen Ring hinaus, dreht die Kamera weiter.
- **Sprint:** den linken Stick ganz nach vorn drücken (Character und Soldat); der Ring leuchtet.
- **Bildschirm ziehen:** schaut sich um. Beim Soldaten mit Waffe feuert ein Finger auf dem Bildschirm und
  zielt weiter, solange er liegt.
- **Knöpfe** links neben dem rechten Stick und darüber, je nach Controller:

| Controller | Knöpfe |
|---|---|
| Soldier | JUMP, RELOAD (mit Waffe), AIM (mit Waffe, Umschalter), CROUCH (Umschalter), USE |
| Character | JUMP (wenn Jump speed > 0), USE |
| Rolling ball | JUMP (wenn Jump speed > 0), BOOST (wenn Boost an) |
| Flyer | UP, DOWN, BOOST (wenn Boost an) |
| Hover car, Vehicle | BOOST (wenn Boost an), HOP (wenn Jump speed > 0), USE |
| Walker (Plumo) | QUACK, PICK, SIT, KICK L, KICK R, ROLL, BEAK, WHEEE, HOP |

Hebt sich der letzte Finger vom Bildschirm oder geht die Seite in den Hintergrund, lässt die Engine
alles los; nichts bleibt gedrückt hängen.

![Play am Telefon im Hochformat: EXIT, PAUSE und die Touch-Sticks](bilder/phone-play-390.png)

### 11.12.3 Gamepad

Gamepads mit Xbox-Belegung (Standard-Mapping des Browsers) funktionieren im Editor-Play und im exportierten
Spiel, mit Dead-Zones und Kurven wie in SpawnCity:

| Eingabe | Wirkung |
|---|---|
| Linker Stick | bewegen |
| Rechter Stick | schauen |
| RT | feuern |
| LT | zielen |
| A | springen |
| B | ducken |
| X | nachladen |
| Y | benutzen |
| L3 oder LB | Sprint/Boost |
| RB / LB | beim Flyer hoch / runter |
| Start | Pause |

---

## 11.13 Plumo steuern

Plumo ist Kerns zweibeinige Roboter-Ente. Sie läuft auf den echten, trainierten Netzen von Pollen Robotics' Open-Source-Roboter.
Wie Modell, Servos und Netze funktionieren, beschreibt [Kapitel 13](13-plumo.md); hier geht es um die
Bedienung im Studio.

![Das Beispiel-Level Plumo: ein Zimmer im Maßstab der Ente](bilder/duck-level.png)

**Das Beispiel-Level** (File › Example Levels › Plumo): ein Zimmer in ihrem Maßstab mit Möbeln, einem
Ball und zwei Socken. Die Ente läuft autonom. Schon ohne Zusätze spielt sie Ball wie in Pollens
ursprünglicher Runtime (BallPlay: hinlaufen, ausrichten, mit links kicken); mit eingeschaltetem
**Kern extras** (im Beispiel-Level an) spielt stattdessen Kerns Spielschicht mit Ball und Socken. Der Level-Blueprint lässt sie quaken, sobald ihre Kopfkamera den Ball sieht. Play startet mit
der Observer camera: Man schaut zu und kann den Ball mit Maus oder Finger anheben; F8 übernimmt die Ente.

**Ohne Eingabe** läuft die Ente selbstständig (Details › Player Controller › Walker › **Autonomous**): Sie
erkundet den Raum mit Kopf-ToF und Kamera, weicht Hindernissen aus und steht nach einem Sturz selbst
wieder auf. Hebt man sie am Rumpf hoch, pausiert sie (Robot State „Held“) und läuft abgesetzt weiter.

**Tastatur** (im Play, den Spieler übernommen):

| Taste | Bewegung |
|---|---|
| W / S | vorwärts / rückwärts laufen |
| A / D | drehen |
| Leertaste | quaken |
| 1 | hinsetzen / aufstehen |
| 2 | aufheben (Pick) |
| 3 | mit links kicken |
| 4 | mit rechts kicken |
| 5 | rollen (gehalten: rollt weiter) |
| 6 | Schnabel öffnen / schließen |
| **7** | **springen** – Kerns eigene Bewegung aus `jump.onnx` |
| V | „Wheee"-Fahrt, solange gehalten |

**Touch:** die Knöpfe aus [11.12.2](#11122-play-auf-touch-geräten). **HOP** ist der Sprung (Taste 7),
**QUACK** quakt, **WHEEE** startet die Fahrt, die übrigen entsprechen den Tasten 1–6.

**Gamepad:** RT quakt, LT startet die Wheee-Fahrt (beide öffnen dabei den Schnabel), B setzt sich hin bzw.
steht auf, Y hebt auf, X rollt.

Logik steuert die Ente mit den Knoten der Kategorie **Robots** (Walk, Look, Robot ToF, Robot Camera Sees,
Robot Move mit der Bewegung „jump" und weitere, [Kapitel 9](09-logik.md)).

**Kopfkamera und ToF:** Während des Spiels zeigt Studio oben rechts im aktiven Viewport, was die Ente
sieht (Window › Show › **Robot Camera**, standardmäßig an):

- **HEAD CAMERA** – das Bild ihrer Kopfkamera, dazu ihr Zustand bzw. „Fallen".
- **TOF 8×8** – das 8×8-Gitter ihres Abstandssensors: rot ein Hindernis (heller, je näher), grün Boden,
  gelb zu nah, dunkel nichts; dazu der kleinste Abstand in echten Metern oder „clear".

![Plumo im Play mit Kopfkamera und ToF-Gitter](bilder/duck-play.png)

---

## 11.14 Policies laden (jump.onnx, play.onnx)

Die Bewegungen der Ente sind neuronale Netze im ONNX-Format. Das Laufnetz `velstand.onnx` ist fest im
Modul eingebaut; alle anderen lädt die Seite nach, sobald ein Level eine Ente enthält:

| Netz | Herkunft | Ordner auf dem Server | Wofür |
|---|---|---|---|
| Pollens Satz (`roller.onnx`, `roulade.onnx`, `ball_kick_left.onnx`, `ball_kick_right.onnx`, `alpha_*.onnx`, `roller_crouch.onnx` …, laut Manifest) | Pollen Robotics, `kern/third_party/pollen/` | `pollen/` | Sitzen, Aufheben, Kicken, Rollen, Rollschuhe, ältere Gangart |
| `jump.onnx` | Kern Lehrer, `kern/policies/` | `policies/` | der Sprung (Taste 7, HOP, Robot Move „jump") |
| `play.onnx` | Kern Lehrer, `kern/policies/` | `policies/` | die Spielschicht: Ball kicken, Socke aufheben, wegtragen, ausspucken (nur mit **Kern extras**) |

So läuft das Laden:

1. Die Engine meldet über `kern_studio_pending_policies()`, welche Dateien das Level braucht und noch
   fehlen.
2. Die Seite holt jede Datei: Kerns eigene Netze aus `policies/`, Pollens aus `pollen/`, jeweils neben
   der Seite. Die Entwicklungsseite liest die Kopien im Repository (`kern/third_party/pollen/`,
   `kern/policies/`), ebenso die Single-File-Version in `generated/`.
3. `kern_studio_load_policy(file, bytes)` übergibt die Bytes; die Engine prüft, ob es ein Netz ist, das sie
   ausführen kann, und registriert es unter seinem Dateinamen.
4. Scheitert eine Datei, versucht die Seite es nach 5 s, 10 s, 20 s … erneut, höchstens jede Minute.
   Bis dahin steht die Bewegung nicht zur Verfügung; die Ente bleibt stehen und sagt, warum.

**Selbst trainierte Netze:** Was der Kern Lehrer im Browser trainiert (Seite `/lehrer/`,
[Kapitel 14](14-lehrer.md)), speichert er im selben Browser (IndexedDB `kern-lehrer`, Speicher
`policies`, Schlüssel = Dateiname wie `jump.onnx`). Studio lädt diese Netze beim Start und erneut, sobald
der Lehrer ein neueres meldet (BroadcastChannel `kern-lehrer`). Ein auf demselben Gerät trainiertes Netz
ersetzt damit das mitgelieferte, ohne dass man eine Datei kopiert.

Beim Veröffentlichen kopiert die CI die Netze nach `editor/pollen/` und `editor/policies/`. Wer ein
exportiertes Spiel mit Ente selbst hostet, legt diese beiden Ordner neben die HTML-Datei
([11.16](#1116-spiel-exportieren)).

---

## 11.15 Speichern, Laden, Autosave und Einstellungen

### 11.15.1 Das Format `.klevel`

Ein Level ist eine JSON-Datei mit `"format": "kern-level"` und `"version": 1`. Sie enthält `name`,
`world`, die Editor-Kamera (`camera`), `nextId`, `actors`, `textures`, `sounds` und gegebenenfalls `logic`.
Texturen und importierte Sounds stehen als Data-URL darin; eine Datei ist damit vollständig. Eine Datei
einer neueren Studio-Version lehnt Studio mit „level was written by a newer Kern Studio" ab. Das Format
im Einzelnen beschreibt [Kapitel 8](08-spiel-laufzeit.md).

### 11.15.2 Speichern und Öffnen

| Aktion | Was passiert |
|---|---|
| Save (Ctrl+S) | mit File System Access (Chrome, Edge) beim ersten Mal ein Speicherdialog, danach direkt in dieselbe Datei; sonst ein Download `<Levelname>.klevel` |
| Save As (Ctrl+Shift+S) | immer mit Dialog bzw. Download |
| Open (Ctrl+O) | Dateidialog für `.klevel` und `.json` |
| Datei auf die Seite ziehen | `.klevel`/`.json` öffnet, Bilder werden Texturen, Audiodateien Sounds |

Der Dateiname ist der Levelname, in dem alles außer Buchstaben, Ziffern, `-` und `_` zu `_` wird.

**Android** hat kein `showOpenFilePicker`; Studio öffnet dort den normalen Dateidialog. Der kennt die Endung
`.klevel` nicht und würde mit einem Filter alle gespeicherten Levels ausgrauen. Auf Android filtert Studio
deshalb nicht; ist die gewählte Datei kein Level, erscheint „… is not a Kern level", und der Editor läuft
weiter.

### 11.15.3 Autosave und Wiederherstellen

Studio schützt ungespeicherte Arbeit, ohne bei jedem Neuladen zu fragen:

- Die Engine schreibt spätestens 2 s nach jeder Änderung eine Kopie, aber nur, solange das Dokument
  ungespeicherte Änderungen hat. Die Seite hält höchstens eine Kopie (`localStorage`,
  `kern-studio.autosave.v1`).
- Nach dem Speichern, oder wenn man bis zum gespeicherten Stand zurück-undoed, löscht die Seite die
  Kopie wieder.
- Beim Verlassen der Seite, beim Wechsel in den Hintergrund und vor einem Update schreibt sie eine fällige
  Kopie sofort. Hat das Dokument ungespeicherte Änderungen, fragt der Browser beim Verlassen nach.
- Während Play und Simulate wird nichts geschrieben; eine fällige Kopie folgt nach Stop.
- Beim nächsten Start bietet Studio **Restore** oder **Dismiss** an. Restore lädt die Kopie, das Level
  bleibt aber ungespeichert (Punkt am Tab), denn sein Inhalt steht in keiner Datei. Bis zum Speichern wird
  es bei jedem Start erneut angeboten. Dismiss löscht die Kopie.
- Öffnet man ein anderes Level, bleibt die Kopie des vorigen erhalten, bis das neue eigene Änderungen
  bekommt.

Getrennt davon merkt sich Studio das zuletzt bearbeitete Level in seinem sauberen Stand
(`kern-studio.last.v1`) und öffnet es beim nächsten Start wieder.

### 11.15.4 Einstellungen

Studio merkt sich im Browser (`kern-studio.settings.v1`) **nur die Einstellungen, die man selbst geändert
hat**: Preferences, Viewport-Layout, Raster, Welt-/lokale Achsen, Ton an/aus und pro Viewport Ansichtstyp,
View Mode, Sichtfeld und Show-Flags. Was man nie angefasst hat, folgt den Standardwerten der jeweiligen
Version, auch wenn diese sich ändern. Panelgrößen und Content Drawer speichert die Seite eigens
(`kern-studio.layout.v1`). Dass Multiplayer-Play von selbst auf vier Viewports schaltet, gilt nicht als
eigene Wahl.

**Edit › Preferences...**:

| Feld | Bereich | Standard |
|---|---|---|
| Camera speed (1-8) | 1–8 | 4 |
| Mouse sensitivity | 0,1–5 | 1 |
| Invert mouse Y | an/aus | aus |
| Orbit around selection (Alt+LMB) | an/aus | an |
| Grid snapping, Grid size | 0,001–1000 m | an, 0,5 m |
| Rotation snapping, Rotation step | 0,1–180° | an, 15° |
| Scale snapping, Scale step | 0,001–100 | aus, 0,25 |

**Edit › Reset Settings to Defaults...** fragt in einem Studio-Dialog nach und setzt alle Einstellungen,
Panelgrößen und den Content Drawer zurück. Levels bleiben unberührt.

---

## 11.16 Spiel exportieren

**File › Package Game (HTML)...** oder der Paket-Knopf in der Toolbar öffnet den Dialog **Package Game**.

1. **Game title** eintragen (vorbelegt mit World Settings › Game title bzw. dem Levelnamen).
2. **Package** klicken.
3. Studio lädt `<Titel>.html` herunter und meldet „Packaged … (*n* MB)" mit **Play it**, das die Datei in
   einem neuen Tab startet.

Die Datei ist ein vollständiges Spiel: Sie enthält das Engine-Modul als Base64, `kern.mjs` und `game.mjs`
als ein Inline-Modul und das Level als JSON. Sie braucht keinen Server und kein Netz und läuft auch von
`file://`. Beim Start lädt sie das Modul im Spieler-Modus (`kern_studio_init(…, player_only = 1)`): kein
Editor, nur das Level, sofort im Play.

![Das exportierte Orb Run als eigenständige HTML-Datei](bilder/game.png)

**Bedienung im exportierten Spiel:**

| Eingabe | Wirkung |
|---|---|
| W A S D / Pfeile, Leertaste, Shift, Maus | wie in Play ([11.4.2](#1142-play-und-simulate)) |
| Klick | fängt die Maus für den Maus-Look |
| Esc | gibt die Maus frei und pausiert („Paused – Click to continue") |
| P | Pause |
| R oder Enter nach Sieg/Niederlage, **Play again** | neu starten |
| Ctrl+R | neu starten |
| F8 | bei Observer camera: Spieler übernehmen und zurück |
| Gamepad | wie in [11.12.3](#11123-gamepad) |
| Touch | Sticks und Knöpfe wie in Play, oben links ein ↻ zum Neustart |

Unten steht ein Hinweis mit der Tastenbelegung, auf Touch-Geräten ausgeblendet.

![Das exportierte Spiel am Telefon mit Touch-Sticks](bilder/game-phone.png)

**Online spielen mit der exportierten Datei:** Die Datei versteht drei Parameter in der Adresse:
`?host=<raum>` bietet ein Spiel an, `?join=<raum>` tritt bei, `&relay=ws://…` nutzt ein Relay für andere
Rechner ([11.17](#1117-multiplayer)). Scheitert der Verbindungsaufbau, spielt man allein weiter.

**Was nicht in der Datei steckt:** Die nachladbaren Netze von Plumo ([11.14](#1114-policies-laden-jumponnx-playonnx)).
Das Laufen funktioniert ohne sie, weil `velstand.onnx` eingebaut ist. Für Sitzen, Kicken, Springen und die
Spielschicht gehören die Ordner `pollen/` und `policies/` neben die HTML-Datei.

---

## 11.17 Multiplayer

### 11.17.1 Mehrere Spieler im Editor

**Play › Number of Players** (wie Unreals „Number of Players") startet Play mit 2, 3 oder 4 Spielern: Die
Welt des Levels ist ein Listen-Server, jeder weitere Spieler ein Netzwerk-Client mit eigener Welt und
eigenem Viewport, verbunden über ein Netz im selben Prozess.

1. Play › Number of Players › **2 Players (listen server)**.
2. **Play**. Studio schaltet auf vier Viewports, falls nur einer offen war: Viewport 1 zeigt den Server,
   Viewport 2 den ersten Client usw. Jeder zeigt sein eigenes HUD.
3. Die Eingabe bekommt der Spieler des aktiven Viewports; die anderen stehen still. Ein Klick in einen
   anderen Viewport wechselt den Spieler.
4. **Play › Network Emulation** belastet die Verbindung zu den lokalen Clients mit Latenz, Jitter und
   Paketverlust, von „Perfect" bis „Terrible (300 ms, 20% loss)".
5. Die Statusleiste zeigt Rolle, Spielerzahl, Ping, Verlust und Datenrate.

![Zwei Spieler im Editor: Listen-Server und lokaler Client in eigenen Viewports](bilder/studio-multiplayer.png)

### 11.17.2 Online zwischen Browsern

| Schritt | Host | Gast |
|---|---|---|
| 1 | dasselbe Level öffnen | dasselbe Level öffnen |
| 2 | Play › **Host Online Game...** | Play › **Join Online Game...** |
| 3 | **Room** wählen (Standard „kern"), **Relay** leer lassen oder eintragen | denselben Room, ggf. dasselbe Relay |
| 4 | **Host** | **Join** |

- **Ohne Relay** verbinden sich Tabs desselben Browsers auf demselben Rechner über einen
  BroadcastChannel; ein Server ist nicht nötig. Gibt es im Raum keinen Host, meldet Join das nach 4 s.
- **Mit Relay** spielen Rechner im Netz miteinander: `node tools/net_relay.mjs [port]` starten (Standardport
  8790, braucht das npm-Paket `ws`) und seine Adresse eintragen, etwa `ws://host:8790`.
- **Leave Online Game** verlässt das Spiel. Der Host bemerkt, wenn ein Gast geht.
- Ein Host im Hintergrund-Tab läuft über einen Worker-Takt weiter, damit seine Spieler nicht in einen
  Timeout laufen.

Das Relay ist absichtlich dumm: Es leitet nur Datagramme weiter. Bestätigungen, Wiederholungen, Kanäle,
Verbindungsaufbau und Timeouts erledigt Kerns eigener Netzwerk-Stack in C++ ([Kapitel 10](10-netzwerk.md)).
Logik läuft nur auf der Autorität; geänderte Eigenschaften und erzeugte Actors werden repliziert, auch an
Spieler, die später dazukommen.

---

## 11.18 Schmale Bildschirme und Telefone

Bis 900 px Fensterbreite oder auf Touch-Geräten mit höchstens 600 px Höhe schaltet Studio auf ein
kompaktes Layout:

- **Der Viewport bekommt die ganze Fläche.** Outliner, Details und World Settings öffnen sich über den
  **Panels**-Knopf als ein Blatt mit großen Tabs (**Outliner**, **Details**, **World**): im Querformat
  rechts neben dem Viewport, im Hochformat von unten (64 % der Höhe). Ohne Auswahl öffnet es mit dem
  Outliner.
- **Play und Simulate sind immer mit einem Tipp erreichbar.** Sie liegen zusammen mit dem Panels-Knopf in
  einem Dock am rechten Rand der Toolbar. Der Rest der Toolbar scrollt darunter seitwärts durch, das Dock
  bleibt stehen, auch bei offenem Blatt, Content Drawer oder Logik-Editor. Während Simulate und Play
  stehen dort Pause, Stop und Eject bzw. Play zum Übernehmen; Knöpfe, die gerade nichts tun, sind
  ausgeblendet, die übrigen haben Touch-Größe (mindestens 38 px).
- Bis 900 px Breite blendet die Toolbar Raster-, Winkel- und Skalierungsauswahl sowie den Achsen-Knopf aus,
  die Statusleiste Konsole und Physik-Statistik.
- Der Content Drawer belegt höchstens knapp die halbe Höhe.
- Menüs öffnen Untermenüs an Ort und Stelle mit Zurück-Zeile ([11.2.1](#1121-menüleiste)).
- Play füllt den ganzen Bildschirm mit EXIT und PAUSE ([11.12.2](#11122-play-auf-touch-geräten)).
- Die Seite scrollt nie seitwärts.

![Studio in einem 820 px breiten Fenster](bilder/studio-narrow.png)

![Telefon im Hochformat: das Dock mit Play, Simulate und Panels](bilder/phone-dock-390.png)

---

## 11.19 Architektur

### 11.19.1 Die Editorlogik ist C++

Studio ist die Klasse `kern::studio::Studio` (`kern/include/kern/studio/studio.h`). Sie besitzt das
Dokument, die Historie, die Auswahl, vier Viewports mit Kameras und Navigation, die Gizmos, Overlays,
das Details-Schema, den Katalog, Menüs, Dialoge, Einstellungen, Play/Simulate und das Netzwerkspiel. Die
Seite enthält keine Editorlogik.

| Datei | Inhalt |
|---|---|
| `kern/src/studio/studio.cpp` | Dokument, Historie, Auswahl, Actor-Operationen, Play, Netzwerk |
| `kern/src/studio/viewport.cpp` | Kameras, Navigation, Finger, Picking, Rendern, Spieleingabe |
| `kern/src/studio/gizmo.cpp` | Move, Rotate, Scale, Snapping, Alt-Duplikat |
| `kern/src/studio/overlays.cpp` | Raster, Auswahl-Umriss, Symbole, Debug-Linien |
| `kern/src/studio/details.cpp` | Property-Schema, Details, World Settings |
| `kern/src/studio/catalog.cpp` | Place Actors |
| `kern/src/studio/ui.cpp` | Tastatur, Menüs, Dialoge, Befehle, Panel-JSON |
| `kern/src/studio/logic_edit.cpp` | Logik-Editor |
| `kern/src/studio/settings.cpp` | Einstellungen |
| `kern/src/studio/api.cpp` | `kern_studio_*` C-ABI |

**Warum so?** Erstens läuft im Editor exakt der Code, der auch das exportierte Spiel ausführt; es gibt
keine zweite Implementierung, die abweichen könnte. Zweitens lässt sich der ganze Editor ohne Browser
testen: Die Unit-Tests (`kern/tests/run.sh studio`) bedienen Navigation, Gizmos, Katalog, Befehle,
Outliner, Play, Speichern, Autosave und Einstellungen direkt in C++.

### 11.19.2 Die C-ABI `kern_studio_*`

Die Seite spricht Studio über flache C-Funktionen an, die das WebAssembly-Modul exportiert. Zeichenketten
gehen als Zeiger und Länge in den linearen Speicher; JSON-Antworten kommen als Zeiger auf einen Puffer
zurück, der bis zum nächsten Aufruf gilt (`kern_studio_result_length()` nennt die Länge).

| Funktion | Zweck |
|---|---|
| `kern_studio_init(w, h, player_only)` | Renderer und Studio; `player_only` für exportierte Spiele |
| `kern_studio_frame(now_ms)` | Kameras, Simulation, Overlays, Rendern aller sichtbaren Viewports |
| `kern_studio_resize(w, h)` | Größe des Drawing Buffers |
| `kern_studio_set_viewport(i, x, y, w, h, css_w, css_h, dpr, visible)` | Rechteck eines Viewports (Ursprung unten links) |
| `kern_studio_pointer(i, type, x, y, button, buttons, mods, wheel)` | Zeiger: 0 down, 1 move, 2 up, 3 cancel, 4 wheel, 5 leave |
| `kern_studio_touch(type, id, x, y, time)` | Finger; 1, wenn die Touch-Steuerung ihn genommen hat |
| `kern_studio_key(key, code, down, mods, repeat)` | Tastatur; Antwort `{"handled": bool}` |
| `kern_studio_look(dx, dy)` | Maus-Look bei Pointer-Lock |
| `kern_studio_blur()` | Fokus verloren: alle Tasten los |
| `kern_studio_command(json)` | Befehl, Antwort `{"ok": …}` |
| `kern_studio_state()` / `kern_studio_status()` | Panel-Zustand bzw. Werte pro Frame |
| `kern_studio_revision()` | ändert sich, wenn `state()` neu gelesen werden muss |
| `kern_studio_take_ui()` | Aufträge an die Seite (Pointer-Lock, Zwischenablage, Menüs, Dialoge, Dateien …) |
| `kern_studio_pending_textures()`, `…_texture_source(id)`, `…_texture_pixels(…)` | Texturen dekodiert die Seite |
| `kern_studio_pending_sounds()`, `…_sound_source(id)`, `…_sound_pcm(…)`, `…_sound_failed(…)` | importierte Sounds dekodiert die Seite |
| `kern_studio_audio_init(rate)`, `…_audio_render(frames)`, `…_audio_prepare(ms)` | Mixer starten, Stereo-PCM ziehen, eingebaute Sounds vorbereiten |
| `kern_studio_pending_policies()`, `kern_studio_load_policy(file, bytes)` | Netze von Plumo |
| `kern_studio_net_push(from, bytes)`, `…_net_take()`, `…_net_out()`, `…_net_out_to()` | Datagramme für den Netzwerk-Stack |
| `kern_studio_take_autosave(now)`, `…_take_autosave_discard()`, `kern_studio_dirty()` | Autosave |
| `kern_studio_ao_set(…)` | Ambient Occlusion |
| `kern_studio_shadow_filter(taps)` | Filter des Sonnenschattens: 1, 4 (Telefone) oder 9 (Desktop) PCF-Taps |
| `kern_studio_context_lost()`, `…_context_restored()` | WebGL-Kontextverlust |

Alles, was man in Studio tut, ist ein Befehl als JSON-Objekt `{"cmd": …}`, auch jeder Menüeintrag: Ein
Menüeintrag im JSON von `state()` trägt den Befehl, den er ausführt. Die wichtigsten Gruppen:

| Gruppe | Befehle |
|---|---|
| Auswahl | `select`, `selectRange`, `selectAll`, `deselect`, `selectChildren`, `selectFolder` |
| Actors | `spawn`, `delete`, `duplicate`, `copy`, `cut`, `paste`, `pasteText`, `rename`, `attach`, `moveTo`, `setFolder`, `renameFolder`, `group`, `snapToFloor`, `hide`, `show`, `showAll`, `lock`, `focus`, `connect`, `setPlayer` |
| Eigenschaften | `set {path, value}`, `setWorld`, `addComponent`, `removeComponent`, `importTexture`, `removeTexture` |
| Historie | `undo`, `redo`, `historyJump` |
| Play | `play {mode}`, `stop`, `pause`, `stepFrame`, `eject`, `restart`, `keep`, `gameInput`, `gamepad`, `device`, `touchRelease`, `pointerCaptured` |
| Netz | `netPlayers`, `netSimulate`, `netAddress`, `netHost`, `netJoin`, `netStop` |
| Ansichten | `tool`, `space`, `snap`, `cameraSpeed`, `layout`, `maximize`, `activeViewport`, `viewType`, `viewMode`, `showFlag`, `gameView`, `viewCamera`, `fov`, `bookmark` |
| Panels | `filter`, `collapse`, `section`, `menu`, `dialog`, `submitDialog`, `catalog`, `clearLog`, `ui` |
| Dokumente | `new {template}`, `open`, `restore`, `save`, `saved`, `packageText`, `validate`, `console` |
| Ton | `previewSound`, `stopSound`, `importSound`, `removeSound`, `renameSound`, `mute` |
| Einstellungen | `settings`, `applySettings`, `takeSettings`, `resetSettings` |
| Hilfen | `project {id oder point}` liefert die Bildschirmposition |
| Logik | alle Befehle, die mit `logic` beginnen ([Kapitel 9](09-logik.md)) |

Eigenschaftspfade folgen der JSON-Form des Actors, zum Beispiel `transform.position`,
`components.mesh.material.color` oder `components.joints.0.lower`.

### 11.19.3 `studio.mjs` als dünne Schicht

`kern/web/studio/studio.mjs` lädt das Modul, legt die DOM-Panels an und führt eine einfache Schleife aus:

1. Pro Bildschirm-Frame Viewport-Rechtecke melden, Gamepad abfragen, `kern_studio_frame` aufrufen,
   Ton nachfüllen, Netzwerkpakete senden.
2. `take_ui()` abholen und ausführen: Pointer-Lock, Zwischenablage, Kontextmenü, Dialog, Speichern,
   Öffnen, Paketieren, Netz öffnen …
3. Hat sich `revision()` geändert, `state()` lesen und die Panels neu zeichnen; jedes Panel nur, wenn sich
   sein JSON geändert hat.
4. `status()` lesen und HUD, Statistik, Gizmo-Anzeige, Rahmenauswahl, Robot-Kamera und Touch-Steuerung
   aktualisieren.
5. Alle 200 ms Texturen, Sounds und Netze nachladen, alle 500 ms Autosave und Einstellungen prüfen.

Die Seite erledigt nur, was ein Browser kann und C++ nicht: Dateidialoge, Downloads, Zwischenablage,
Pointer-Lock, Bild- und Audio-Dekodierung, `localStorage`, Web-Audio-Ausgabe, Datagramme über
BroadcastChannel oder WebSocket und das Schreiben exportierter Spiele. `kern/web/studio/game.mjs` enthält
die gemeinsamen Teile (JS-Hülle `StudioEngine` um die C-ABI, Tonausgabe `KernAudio`, Netz `KernNet`,
Gamepad, Touch-Overlay, HUD) und ist zugleich der Spiel-Host exportierter Spiele.

Die Seite stellt außerdem `window.KernStudio` bereit: `command(c)`, `state`, `status`, `key(…)`,
`pointer(…)`, `viewportRect(i)` und weitere. Darüber bedient der Browser-Test
`kern/tests/e2e/studio_e2e.mjs` Studio mit Playwright: Menüs, Fly Mode, Werkzeuge, Outliner, Details mit
Undo, Content Drawer, Kontextmenü, Play, Simulate, Layouts, Vorlagen, Logik, Plumo, Multiplayer,
Export, Wiederherstellen, Einstellungen, Telefon-Layouts und Android-Dateidialog. Die Bilder dieses Kapitels
stammen zum großen Teil aus diesem Test.

Wie man die Bausteine selbst nutzt, zeigt [Kapitel 12](12-eigene-spiele.md); alle Befehle und Funktionen
sammelt [Kapitel 17](17-referenz.md).

---

© 2026 Christian Heinrich Hohlfeld · Alle Rechte vorbehalten · [Impressum und Rechte](00-impressum.md)
