# Physik in Kern: Box3D richtig benutzen

Kern simuliert mit [Box3D](https://github.com/erincatto/box3d), Erin Cattos 3D-Nachfolger von Box2D v3
(eingebunden unter `third_party/box3d`). Diese Seite ist verbindlich für jeden Code in Kern, der
Bodies, Shapes, Joints oder Motoren anlegt.

Grundlage sind drei Quellen:
- Erins eigene Dokumentation (`docs/*.md` im Box3D-Repo) und seine Samples;
- seine Blogposts und GDC-Vorträge auf box2d.org;
- Messungen in Kern.

**Lesen, bevor man `src/physics` oder `src/game/runtime.cpp` ändert oder einen Roboter, ein Fahrzeug oder
einen Ragdoll-Preset baut.**

Quellen sind als `box3d:datei:zeile` (Repo im eingebundenen Stand) oder als URL angegeben.
Punkte mit *[gemessen]* sind mit einem Kern-Test belegt.

---

## 1. Einheiten und Größen

- **Kern rechnet durchgehend in MKS: Meter, Kilogramm, Sekunden.** Winkel sind in der Physikschicht
  Radiant, nur in Leveldateien und im Details-Panel Grad.
- **Bewegte Körper gehören zwischen 0,1 m und 10 m, nie unter 1 cm** (`box3d:docs/loose_ends.md:56-58`,
  `docs/overview.md:171-181`). Die Toleranzen des Solvers sind in Metern festgelegt:

  | Toleranz | Wert |
  |---|---|
  | Linear Slop | 5 mm |
  | Speculative Distance | 20 mm |
  | AABB-Margin | bis 50 mm |
  | Contact Recycling | 50 mm |

  Quelle: `box3d:include/box3d/constants.h:47-97`.
- **Einheiten an genau einer Stelle umrechnen, an der Grenze zu Box3D** (`box3d:docs/loose_ends.md:59-61`),
  nie verstreut im Simulationscode.
  - Kerns Regel für kleine reale Modelle: das ganze Modell mit einem Faktor S skalieren. Längen ×S,
    Massen ×S³, Momente ×S⁴, Geschwindigkeiten und Zeiten ×√S (Froude-Ähnlichkeit: Posen und Gangart
    bleiben gleich).
  - Siehe `kDuckScale` in `src/game/walker.cpp`.
- `b3SetLengthUnitsPerMeter` verstellt die Toleranzen für die **ganze** Welt. Kern nutzt das nicht, weil
  in einem Level Autos und Roboter zusammen vorkommen.

*[gemessen]* Micro Duck in echter Größe (2–4 cm große Glieder): Der Rumpf sank innerhalb von 10 Frames
9 cm durch die Beine, selbst mit verschweißten Gelenken. In 4-facher Größe halten die Gelenke.

## 2. Massenverhältnisse

- **Schwere Körper nicht an leichten aufhängen.** Ab etwa 10:1 leidet die Stabilität.
  - Quellen: Box2D v2.4 `docs/loose_ends.md` und `box3d:docs/loose_ends.md:133-134` („extreme mass
    ratios … joint stretching").
  - Erins Lösung für einen Panzer mit 467:1: leichte Teile schwerer machen, schwere leichter
    ([Diskussion #132](https://github.com/erincatto/box3d/discussions/132)).
- **Ein starres Teil ist ein Body mit mehreren Shapes.** Keine Bodies zusammenschweißen: „the
  approximate solver cannot hold many bodies together rigidly" (`box3d:docs/simulation.md:1523-1526`).
  - Kern schweißt Kinder, die nur ein Mesh haben, in den nächsten Vorfahren mit Body ein. Sie werden
    also Shapes, keine Bodies.
- Leichten Gliedern eine Mindestmasse geben, statt den Solver zu verstellen.
  - Die 5-g-Hüftbügel der Ente wiegen in echter Größe mindestens 20 g (×64 bei S = 4).

## 3. Zeitschritt und Sub-Steps

- **Fester Zeitschritt:** Standard 60 Hz (`WorldSettings.step_hz`), nie gröber als 30 Hz. Box3D bekommt
  nie ein variables dt.
- **Akkumulator nach Glenn Fiedler** ([Fix Your Timestep](https://gafferongames.com/post/fix_your_timestep/),
  `GameWorld::step`):
  - Der Frame liefert Zeit, die Simulation verbraucht sie in festen Schritten h.
  - Der Rest bleibt im Akkumulator.
  - Kern rendert zwischen den beiden letzten Physikständen (alpha = Rest / h).
  - Ein langer Frame wird auf 0,25 s begrenzt, höchstens 0,1 s Simulation pro Frame
    („Spiral of Death"). Ist man trotzdem im Rückstand, fallen ganze Schritte weg; die Phase
    bleibt erhalten.
  - Die Simulationszeit zählt Schritte (`step_count_`) statt Gleitkommazeit aufzusummieren.
  - Eingaben gelten pro festem Schritt. Steuerungen, die seltener rechnen (eine Policy mit
    50 Hz), zählen Schritte, nicht Sekunden.
  - Gleiche Eingaben ergeben damit Bit für Bit denselben Ablauf
    ([Deterministic Lockstep](https://gafferongames.com/post/deterministic_lockstep/); gilt für
    gleiche Maschine und gleichen Build).
  - Die Play-Kamera folgt der **interpolierten** Pose (`presented_player_`). Mit der rohen
    Physik-Pose wackelten schnelle Autos und Kugeln im Bild; `studio_fast_player_does_not_shake_on_screen`
    sichert das ab.
- **Sub-Steps:** Standard 4 (`WorldSettings.substeps`). Lange Gelenkketten brauchen mehr: „long joint
  chains will stretch less with more sub-steps" (`box3d:docs/simulation.md:155-159`).
  - *[gemessen]* Hüfte der Ente bei 4 Sub-Steps: Die Gelenklücke wuchs unter Last auf 5 cm und federte
    zurück. Bei 16 Sub-Steps bleibt sie unter 1 mm.
  - Studio stellt beim Einfügen eines Roboters 16 Sub-Steps und dessen Schrittrate ein (Micro Duck:
    100 Hz, `kWalkerStepHz`) und meldet das.
- Die Gelenksteifigkeit ist **durch die Sub-Step-Rate gedeckelt**: `constraintHertz` wird auf 0,25 ×
  Sub-Step-Rate begrenzt, Kontakt-Hertz auf 0,125 × (`box3d:src/joint.c:1484-1486`,
  `src/physics_world.c:1127-1129`).
  - `constraintHertz` zu erhöhen hilft also nur zusammen mit mehr Sub-Steps.
- Federn bleiben **unter der halben Schrittfrequenz** (Nyquist). Kern begrenzt das (`max_hertz` in
  `create_joints`).

## 4. Gelenke

- Jeden Preset mit Gelenken **in seiner Ruhepose bauen**: Der Nullwinkel liegt dort, wo die beiden
  lokalen Frames übereinstimmen, und Limits müssen die Null enthalten (`box3d:docs/simulation.md:1414-1417`).
- Revolute-Gelenke drehen um die **z-Achse** des Frames. Kern baut den Frame aus der Achse im Actor
  (`physics::frame_from_axis`).
- Eigenkollision:
  - Verbundene Bodies kollidieren nicht, solange `collideConnected` nicht gesetzt ist.
  - Den Rest mit einer negativen Kollisionsgruppe pro Roboter filtern, oder mit Filter-Joints für
    einzelne Paare (`box3d:shared/human.c:399-410`).
- Die Standardwerte `constraintHertz = 60` und Dämpfung 2 nicht anfassen (`box3d:src/joint.c:30-31`).
  Kern erhöht sie nur für Servogelenke, zusammen mit der Sub-Step-Grenze oben.

## 5. Motoren, Servos und Reibung

- **Immer ein endliches maximales Motormoment bzw. eine maximale Motorkraft setzen**
  (`box3d:docs/simulation.md:1419-1429`).
- **Positionsservos sind die Gelenkfeder des Revolute-Joints** mit Zielwinkel
  (`b3RevoluteJoint_SetTargetAngle`, Feder implizit gelöst, bei jeder Steifigkeit stabil).
  - In Kern: `JointComponent::servo` → `GameWorld::drive_servos`.
  - Steifigkeit (N·m/rad) und Dämpfung (N·m·s/rad) sind physikalische Servowerte.
    `World::set_joint_spring` rechnet sie mit der axialen effektiven Masse in Box3Ds Hertz und
    Dämpfungsgrad um, genau wie `revolute_joint.c` (eine Umrechnung an der Grenze).
  - Das Haltemoment entsteht durch Begrenzen des Fehlers auf Moment / Steifigkeit, wie die
    sättigende PWM eines Dynamixel.
  - Erins anderes Rezept, `motorSpeed = gain × Fehler` mit Maximalmoment
    (`box3d:docs/simulation.md:1240-1243`), ist eine starre Geschwindigkeitsregelung ohne
    Nachgiebigkeit. Für Roboter mit trainierter Policy passt es nicht, denn die Policy hat einen
    nachgiebigen PD-Servo gelernt.
- **Die Physik nicht umgehen.** Keine Zusatzmomente, die etwas „nachbilden" sollen. Keine gesetzten
  Geschwindigkeiten an Bodies mit Gelenken oder Kontakten. Keine Gyros, Sperren oder Stützkräfte für
  Roboter. In der echten Welt gibt es sie auch nicht.
  - Die Rotorträgheit eines Getriebes (MuJoCo „armature") ist eine Masse-Eigenschaft: eine
    Scheibe auf der Gelenkachse, die das Glied trägt (`docs/walker.md` Abschnitt 6). Was Box3D
    gar nicht abbilden kann, wird dokumentiert und nicht vorgetäuscht.
- **Gelenkreibung** ist ein Motor mit Geschwindigkeit 0 und kleinem Maximalmoment
  (`box3d:docs/simulation.md:1431-1434`). Erins Ragdoll nutzt 5 N·m × einen Faktor pro Gelenk
  (`box3d:shared/human.c:347-393`).
- **Kontaktreibung** ist pro Shape eingestellt (`ColliderComponent::friction`, Standard 0,6).
  - Im Editor: Details › Mesh › **Physics** (Reibung, Restitution, Rollwiderstand des automatisch
    erzeugten Colliders, direkt über dem Render-Material) oder Details › Collider bei eigenen
    Collidern. Der endlose Boden: World Settings › Physics › Ground friction / Ground bounciness.
  - Zwei Materialien kombiniert Box3D als √(a · b) (Reibung) und max (Restitution).
  - Kontakt-Weichheit (Box3D World Definition): World Settings › Contact stiffness (30 Hz),
    Contact damping ratio (10), Contact push-out speed (3 m/s).
  - Rollwiderstand wirkt nur auf Kugeln und Kapseln.
  - Roboterfüße sind breite flache Quader mit hoher Reibung.
- Einen Körper weich aufrecht halten: Erin nimmt dafür einen **Parallel-Joint** (Hertz, Dämpfung,
  Maximalmoment), keine überschriebenen Geschwindigkeiten (`box3d:shared/human.c:526-605`).

## 6. Kräfte, Geschwindigkeiten, Dämpfung

- Dauerlasten als **Kräfte, jeden Schritt** anwenden; Kräfte werden nach jedem Schritt zurückgesetzt
  (`box3d:docs/simulation.md:1966`).
- Geschwindigkeiten direkt zu setzen übergeht den Solver. Ein Sprung ist deshalb ein Impuls
  (`apply_linear_impulse_to_center`, m · Δv), keine gesetzte Geschwindigkeit.
  - Für Charaktere (alle Drehachsen gesperrt) und kinematische Bodies ist das in Ordnung.
  - An Bodies mit Gelenken oder Kontakten kämpft es gegen die Constraints und pumpt Energie hinein.
  - *[gemessen]* Ein „Gyro" auf Geschwindigkeitsebene ließ den Rumpf der Ente springen.
- Die eingebaute Dämpfung nutzen (implizit, `box3d:src/solver.c:93-94`). Selbst gebautes
  `F = −c·v` mit großem c ist explizit und wird an leichten Bodies instabil.

## 7. Bodies, Charaktere, Kinematik

- **Bodies an ihrer Endpose erzeugen.** Am Ursprung erzeugen und danach verschieben kostet doppelt;
  im Spiel nicht per `SetTransform` teleportieren (`box3d:include/box3d/box3d.h:551-554`).
- Den Body-Typ beim Erzeugen festlegen, `SetType` ist teuer.
- Kinematische Bodies bewegt man über die Geschwindigkeit oder `SetTargetTransform`. Ein Ziel, das
  langsamer als die Schlafschwelle ist, wird übersprungen (`box3d:include/box3d/box3d.h:580-584`).
- Kerns Charaktere und Soldaten nutzen Box3Ds experimentellen Character Mover (`box3d:docs/character.md`):
  - die Geschwindigkeit jeden Schritt an den Ebenen beschneiden;
  - einen sinnvollen Kapselradius verwenden;
  - kein schnelles Drehen.
- Bewegungssperren sind weiche Constraints. Kern sperrt Nicken und Rollen nur bei eigenen Controllern
  (Fahrzeuge, Charaktere, Flieger), nie bei Walkern.
- Importierte Roboter bekommen ihre Masseneigenschaften aus der Beschreibung (MJCF/URDF): Masse,
  Schwerpunkt und vollen Trägheitstensor (`BodyComponent::center_of_mass`, `inertia`,
  `World::set_mass_data`). Die Kollisionsformen bestimmen dann nur noch den Kontakt.
- Gelenke, die in einer anderen als ihrer Nullstellung gebaut werden, tragen den Winkel dieser Pose als
  `JointComponent::reference`. Der Joint liest dann den Winkel der Beschreibung.

## 8. Continuous Collision, Events, Schlaf

- Continuous Collision eingeschaltet lassen. Sie deckt nur dynamisch gegen statisch ab, Bullets
  zusätzlich dynamische Bodies. Kern macht schnelle Spielerfahrzeuge zu Bullets.
- Events sind standardmäßig aus und werden pro Shape eingeschaltet. Kern schaltet Kontakt-, Sensor-
  und Hit-Events nur an den Shapes ein, deren Events das Spiel liest (`create_body`).
- Schlaf: Spieler und Roboter schlafen nie. Eine langsame Gangart liegt unter der Schlafschwelle von
  5 cm/s.

## 9. Checkliste für einen neuen gelenkigen Preset (Roboter, Ragdoll, Fahrzeug)

1. Jedes bewegte Teil ist in echter Größe ≥ 10 cm. Sonst das Modell mit einem Faktor skalieren (§1).
2. Ein Body pro starrem Segment; Kinder, die nur ein Mesh haben, werden als Shapes eingeschweißt.
3. Nachbarmassen innerhalb von etwa 10:1 halten und eine Mindestmasse pro Glied setzen (§2).
4. In der Ruhepose bauen, mit Limits um 0 und Achsen im Frame des Actors (§4).
5. Servos als Gelenkfeder mit physikalischer Steifigkeit, Dämpfung und Haltemoment; Gelenkreibung als
   Motor mit Geschwindigkeit 0 (§5). Keine Hilfskräfte.
6. Für das Level 8–16 Sub-Steps einstellen, oder sie beim Einfügen des Presets erhöhen (§3).
7. Eigenkollision mit einer negativen Gruppe filtern (§4).
8. Einen nativen Test schreiben: Der Preset steht, seine Gelenke halten (Lücke < 1 % der Gliedlänge)
   und er erfüllt seine Aufgabe (läuft, fährt, schwingt), deterministisch.
