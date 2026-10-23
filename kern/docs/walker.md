# Walker: die Micro Duck mit ihrer echten Policy

Die Micro Duck ist Pollen Robotics' zweibeiniger Roboter: 25 cm groß, 0,74 kg, 14 Servos
(XL330) in der Policy, dazu ein Mundservo. Kern lässt sie mit genau dem neuronalen Netz laufen,
mit dem der echte Roboter läuft: `velstand` v8. Das Netz steht, läuft, dreht und steht nach einem
Sturz wieder auf. Dieses Dokument beschreibt den Vertrag zwischen Netz und Simulation. Wer daran
etwas ändert, liest vorher auch `docs/physics.md`.

Code: `src/game/walker.cpp`. Daten: `src/game/micro_duck_model.inc` und
`src/game/micro_duck_velstand.inc`, beide von `tools/micro_duck_import.py` erzeugt. Quellen und
Lizenzen stehen in `third_party/microduck/README.md`.

## 1. Grundsatz: die Physik nicht umgehen

Die Policy wurde in MuJoCo auf einem physikalischen Modell trainiert. Sie funktioniert nur, wenn
Kern dieselbe Physik rechnet. Deshalb gilt:

- **Keine Hilfskräfte, keine gesetzten Geschwindigkeiten.** Es gibt keinen Gyro, keine
  Rumpfsperre und keine Zusatzmomente. Der Roboter steht allein durch seine Servos und Kontakte,
  wie in der echten Welt.
- **Servos bestehen nur aus Box3Ds eigenen Gelenkteilen.** Die implizit gelöste Gelenkfeder
  erzeugt Steifigkeit und Dämpfung, ein Motor mit Geschwindigkeit 0 die Reibung
  (`docs/physics.md` Abschnitt 5).
- **Fester Zeitschritt nach Glenn Fiedler („Fix Your Timestep").** Die Policy rechnet alle N
  Physikschritte, gezählt in Schritten statt in Wanduhrzeit. Der Lauf ist deterministisch: gleiche
  Eingaben ergeben Bit für Bit denselben Gang (Test
  `micro_duck_stands_walks_and_turns_on_its_policy`).

## 2. Modell

| Training (MuJoCo) | Kern (Box3D) |
|---|---|
| `robot_allcollisions_backlash.xml`: 15 Bodies, 14 Gelenke | dieselben Bodies und Gelenke, aufgebaut in der Home-Pose (STAND-Keyframe) |
| Masse, Schwerpunkt, voller Trägheitstensor pro Body | übernommen: `BodyComponent::center_of_mass` und `inertia`, gesetzt mit `set_mass_data` |
| Gelenkwinkel relativ zur MJCF-Nullstellung | `JointComponent::reference` = Home-Winkel, damit ein Gelenk genau den MJCF-Winkel liest |
| Kollision: zwei Sohlen (condim 3, Reibung 1) und alle XL330-Gehäuse (condim 1, reibungsfrei) | dieselben Teile als orientierte Quader auf den Mesh-Maßen; Sohlen Reibung 1, Gehäuse 0 |
| Spiel im Getriebe ±1° (passives Gelenk) | nicht nachgebildet (siehe Abschnitt 6) |

**Maßstab 4.** Box3D ist für Körper von 0,1 bis 10 m ausgelegt, die Glieder der Ente sind 2 bis
4 cm groß. Kern baut sie deshalb 4-fach groß und rechnet an genau einer Stelle um
(`scaled()`), nach Froude-Ähnlichkeit:

| Größe | Faktor |
|---|---|
| Länge | 4 |
| Masse | 64 |
| Trägheit | 1024 |
| Moment und Servosteifigkeit | 256 |
| Dämpfung | 512 |
| Zeit | 2 |
| Geschwindigkeit | 2 |
| Winkelgeschwindigkeit | 1/2 |

Die Policy sieht immer echte Einheiten: Kern multipliziert Winkelgeschwindigkeiten mit √4 = 2,
bevor sie ins Netz gehen.

## 3. Zeitschritt und Takt

- Training: Physik alle 5 ms (200 Hz), Policy alle 4 Schritte (50 Hz), Kommandolatenz 15–30 ms.
- In Kern läuft die Zeit 2× langsamer: Physik 10 ms (`kWalkerStepHz` = 100 Hz, 16 Sub-Steps),
  Policy alle 4 Physikschritte, Latenz 4 Schritte (entspricht 20 ms echt).
- Fügt Studio eine Ente ein, stellt es die World Settings auf 100 Hz und 16 Sub-Steps und meldet
  das. Bei einer anderen Schrittrate rundet Kern den Takt auf ganze Schritte und meldet die
  Abweichung.
- `GameWorld::step` folgt „Fix Your Timestep":
  - Der Frame liefert Zeit, die Simulation verbraucht sie in festen Schritten.
  - Der Rest bleibt im Akkumulator.
  - Gerendert wird zwischen den letzten beiden Zuständen (alpha = Rest / h).
  - Lange Frames werden begrenzt.
  - Die Simulationszeit ist ein Schrittzähler und keine aufsummierte Gleitkommazahl.

## 4. Servo (XL330)

Gelernt hat die Policy das BAM-Modell „m6" des XL330 bei 7,4 V:

| Parameter | Wert |
|---|---|
| Steifigkeit K = V · kt/R · 0,5755 | 0,554 N·m/rad |
| Dämpfung (Gegen-EMK kt²/R plus viskose Reibung) | 0,053 N·m·s/rad |
| Coulomb-Reibung | 0,0048 N·m |
| Moment bei voller Aussteuerung (V · kt/R) | 0,963 N·m |

In Box3D (`GameWorld::drive_servos`, in jedem Physikschritt):

- **Feder:** Die Gelenkfeder zieht zu `angle + clamp(Ziel − angle, ±Moment/K)`.
  - `set_joint_spring` rechnet N·m/rad mit der axialen effektiven Masse des Gelenks in Box3Ds
    Hertz und Dämpfungsgrad um. So rechnet Box3D seine Feder auch intern.
  - Die Feder ist implizit und bei jeder Steifigkeit stabil.
  - Die Begrenzung des Fehlers ergibt das Haltemoment, wie die sättigende PWM des Servos.
- **Reibung:** ein Motor mit Geschwindigkeit 0, dessen maximales Moment die Reibung ist.

**Strombegrenzung.** Mit der 1,75-A-Grenze der Firmware (0,64 N·m) kommt die Ente nicht mehr vom
Rücken hoch. Das Trainings-Repo schreibt in `scripts/infer_policy.py` ausdrücklich, dass ohne diese
Grenze trainiert wurde. Kern folgt dem.

## 5. Beobachtung und Aktion (velstand, 61 → 14)

| Index | Inhalt |
|---|---|
| 0–2 | Winkelgeschwindigkeit des Rumpfs im Rumpf-Frame (x vorn, y links, z oben), rad/s |
| 3–5 | Schwerkraft im Rumpf-Frame, aufrecht (0, 0, −1) |
| 6–19 | Gelenkwinkel − Home-Pose, in der Policy-Reihenfolge: linkes Bein, Hals/Kopf, rechtes Bein |
| 20–33 | Gelenkgeschwindigkeiten, einen Policy-Takt alt (so im Training) |
| 34–47 | letzte Aktion, roh |
| 48–50 | Kommando vx, vy, ωz: ±0,4 m/s, ±0,3 m/s, ±1 rad/s; EMA 0,2 pro Takt wie auf dem Roboter |
| 51–54 | Kopfkommando (Hals-, Kopfnicken, Kopf gieren, rollen), Delta zur Home-Pose; Quaken nickt |
| 55–60 | Körperpose, 0 |

- **Aktion:** Ziel = Home + Aktion. Skalierung 1 und kein Filter, wie im Training.
- **Netz:** (obs − mean) / Divisor, dann Schichten mit 512, 256 und 128 ELU-Neuronen, dann
  linear 14. Die Gewichte sind exakte float32-Hex-Literale.
  - `micro_duck_policy_matches_the_onnx_reference` prüft das Netz gegen eine float64-Referenz bis
    2·10⁻⁵.
- **Steuerung:**
  - W/S: vx
  - A/D: ωz
  - Leertaste: Quaken
  - Ohne Spieler (Simulate) steht die Ente bei Kommando 0.

## 6. Gemessen (Test `micro_duck_stands_walks_and_turns_on_its_policy`)

| Fall | Kern | Pollen (MuJoCo, v8) |
|---|---|---|
| Kommando 0 | steht 8 s, driftet unter 2 mm | steht |
| vx = 0,4 m/s | ≈ 0,135 m/s echt (ohne Armatur 0,105) | ≈ 0,17 m/s |
| ωz = 1 rad/s im Stand | ≈ 0,61 rad/s echt (ohne Armatur 0,52) | ≈ 0,97 rad/s |
| Zappeln beim Laufen (Effektivwert der Winkelgeschwindigkeit, Test `micro_duck_holds_itself_steady`) | Rumpf 1,0 rad/s, Kopf 1,9 rad/s (ohne Armatur 3,6 und 6,7) | – |
| liegt auf Bauch oder Rücken | steht nach höchstens 12 s wieder | steht auf |

**Was fehlt, und warum:**

- **Armatur (Rotorträgheit, 0,0018 kg·m² pro Gelenk, MJCF `chosen_actuator`).** MuJoCo addiert
  sie in reduzierten Koordinaten auf die Gelenkachse. Box3D rechnet in maximalen Koordinaten und
  kennt keinen Getriebe-Joint.
  - Kern gibt sie dem Glied als Masse-Eigenschaft: eine dünne Scheibe auf der Gelenkachse, die das
    Glied trägt. Das sind 0,0018 um die Achse und je die Hälfte quer dazu. Das ist ein gültiger
    starrer Körper; ein rein axialer Tensor ist keiner, und Box3D gewinnt damit Energie.
  - Es gibt keine Zusatzmomente und keine gesetzten Geschwindigkeiten. Die Trägheit wirkt durch
    Box3Ds Löser.
  - Unterschied zu MuJoCo: Der echte Rotor dreht mit Übersetzung (288:1) relativ zum Gehäuse.
    Kerns Scheibe dreht mit dem Glied, also auch mit der Bewegung des Elternglieds um dieselbe
    Achse. Dreht sich das Elternglied, ist die Trägheit dadurch etwas zu groß.
  - Gemessen: Sie läuft und dreht näher am Training (Tabelle) und zappelt viel weniger.
- **Getriebespiel ±1°:** nicht modelliert. Im Training ist es eine Domain-Randomization, die
  Policy ist dagegen robust.
- **Reibungsmischung:** Box3D mischt die Reibung zweier Flächen als geometrisches Mittel. MuJoCo
  nimmt die Sohle (Priorität).
  - Auf Kerns Boden (0,6) ergibt das 0,77.
  - Das liegt in der Trainings-Randomization von 0,7 bis 1,3.

## 7. Neu importieren

Siehe `third_party/microduck/README.md`. Der Import prüft:

- Gelenknamen und -reihenfolge;
- Home-Pose gegen `default_joint_pos`;
- Beobachtungsreihenfolge;
- `action_scale`;
- die Graphstruktur (Sub, Div, 4× Gemm, ELU).

Weicht etwas ab, bricht er ab.

## 8. Sensoren

Die Ente sieht nur, was der echte Roboter sieht. Alle Sensoren sitzen an den Stellen, die das MJCF
festlegt (Sites `head_camera`, `tof`, `imu`, vom Import übernommen).

### Kopf-ToF

Wie der VL53L8CX des Roboters (Quellen: `kinematics/src/tof.rs`, `tof/` im microduck-Repo):

| Eigenschaft | Wert |
|---|---|
| Zonen | 8 × 8 |
| Sichtfeld | 45° × 45° |
| Reichweite | bis 4 m |
| Bildrate | 15 Hz echt |

- **Messung:** Jede Zone ist ein Box3D-Ray-Cast, der die eigenen Glieder überspringt
  (`World::cast_ray_ignoring`).
- **Klassifikation** wie im Roboter:
  - **Boden:** Ein abwärts gerichteter Strahl, der mindestens 85 % der Sensorhöhe überwindet.
  - **Zu nah:** horizontal unter 10 cm (Rauschband).
  - **Hindernis:** alles andere, mit horizontaler Entfernung, Richtung und der Belegung der linken
    und rechten Hälfte.

### Kopfkamera

- IMX219 hinter M12-Optik: 62° über die lange Seite des 16:9-Bildes (`mediad/src/camera.rs`).
- Eingebaut um eine Vierteldrehung gedreht (`mediad` `rotate = 90`). Aufrecht ist das Bild daher
  hochkant, 9:16: 62° hoch und 37,4° breit (die Kalibrierung mit fy = 1062 ergibt dieselben 37,4°).
  Kern rendert es genau so, und **Robot Camera Sees** prüft gegen dieses Sichtfeld.
- Studio zeigt sie beim Spielen oben rechts im Viewport (Show-Flag **Robot Camera**), daneben das
  ToF-Gitter: rot = Hindernis, heller = näher; grün = Boden; gelb = zu nah.
- Logic fragt sie mit **Robot Camera Sees** ab: im Bild und nicht verdeckt (Ray-Cast).

### IMU

Gravitation im Rumpf-Frame. „Gefallen“ heißt wie im Daemon gz > −0,5, und zwar 200 ms lang
(`robotd-params`: `fall_gravity_z`, `fall_debounce_ms`). Ein kurzes Kippen beim Stolpern zählt
nicht.

## 9. Autonomes Verhalten

Fährt niemand die Ente, entscheidet sie selbst (`src/game/walker_brain.cpp`). Die Zustände geben
dieselben Kommandos wie ein Gamepad: Twist und Kopfpose für die Policy.

**Quellenlage, ehrlich:** Die Verhaltensmaschine des echten Roboters steht in `autonomous.rs` im
Repository `apirrone/microduck_runtime`. Der Zugriff darauf wurde in dieser Sitzung verweigert, und
das öffentliche `pollen-robotics/microduck` hat sie noch nicht portiert (dort „the biggest untracked
gap“). Deshalb gilt:

- **Original:** die Zustandsnamen, die Sensoren und ihre Filter (Abschnitt 8), das Sturzurteil,
  die Glättung und die Grenzen der Kommandos sowie der Blick in die Runde im Stehen (unten,
  portiert aus `robotd/src/idle_head.rs`).
- **Kerns eigene Werte:** Dauern, Wahrscheinlichkeiten, Geschwindigkeiten, Energie,
  Erkundungsraster und Ausweich-Abstände. Sie werden ersetzt, sobald `autonomous.rs` lesbar ist.

| Zustand | Was die Ente tut |
|---|---|
| Chill | steht; nach 2 s Ruhe schaut sie sich um (Idle-Look-around, unten) |
| LookAround | wie Chill, für kürzere Pausen zwischen zwei Erkundungen |
| Wander | läuft mit 0,3 m/s auf die am wenigsten besuchte freie Richtung zu; der Kopf schaut in die Kurve und leicht nach unten |
| TurnInPlace | dreht auf der Stelle zur gewählten Richtung |
| BackOff | etwas zu nah: kurz rückwärts und weg |
| Zoomies | selten, ausgeruht: ein kurzer schneller Lauf |
| GetUp | gestürzt: wartet, bis die Policy aufgestanden ist |

Die Steuerung dahinter:

- **Energie:** sinkt beim Laufen, steigt beim Stehen. Sie entscheidet zwischen Ruhe und Erkunden.
- **Erkundungsgedächtnis:** ein Raster aus Zellen von 0,5 m. Gezählt werden Besuche, die langsam
  vergessen werden, und Zellen, in denen der ToF ein Hindernis sah. Das wählt die nächste Richtung.
- **Hindernisse** liest sie nur aus frischen ToF-Bildern (jünger als 0,4 s, wie `TOF_STALE` im
  Monitor des Roboters):
  - unter 0,7 m voraus: langsamer werden und ausweichen;
  - unter 0,32 m: anhalten und zur freieren Seite drehen;
  - unter 0,16 m: zurücksetzen.

### Blick in die Runde (Idle-Look-around)

Zeile für Zeile portiert aus `robotd/src/idle_head.rs`, mit den Standardwerten aus `[idle]`
(`robotd-params`). Er läuft, wenn die Ente still steht, gleich ob autonom, per Logic oder per
Stick:

- **Start:** Kein Twist ist angefordert, der geglättete Twist ist unter 10⁻³, und seit 2 s hat
  niemand den Kopf gesteuert. Dann noch einmal 2 s (`after_s`). Er blendet über 1,5 s ein und aus.
- **Blicke:** Sie schaut irgendwohin, bis ±0,6 rad Gieren, ±0,2 Nicken und ±0,15 Hals. Mit 25 %
  geht der Blick zur Mitte, mit 25 % kippt sie den Kopf neugierig um 0,3 rad. Ein neuer Blick
  liegt mindestens 0,2 rad vom alten entfernt.
- **Bewegung:** Jeder Blick folgt einer Minimum-Jerk-Kurve und dauert 0,3 s plus 1 s pro rad.
  Gehalten wird er 1–3 s. Währenddessen springt der Blick alle 0,4–1,2 s um bis zu 0,08 / 0,05 rad.
- **Lebendigkeit:** Eine langsame Drift von 0,04 rad bei 0,15 Hz liegt auf allen Achsen.
- **Dösen:** Mit der Zeit wird sie ruhiger (Zeitkonstante 90 s, Untergrenze 0,5). Nach 45 s gähnt
  sie etwa alle 90 s: Der Kopf hebt sich 3 s lang um 0,15 rad.
- **Nicht übernommen:** Schnabel und Atmen. Kerns Ente hat keinen Mund-Servo, und das Atmen gilt
  nur für das Steh-Netz. Beides wird wie im Original berechnet, aber nicht angewendet.

*[gemessen]* Test `micro_duck_looks_around_when_left_alone`: In den ersten Sekunden steht der Kopf
still. Danach schwenkt der Blick der Kamera um über 0,2 rad (gemessen 1,1 rad), und die Ente bleibt
stehen.

### Wer fährt

In dieser Reihenfolge, wie beim echten Roboter:

1. Der Stick des Spielers. Die Autonomie pausiert, bis 2 s lang keine Eingabe kam.
2. Ein Logic-Kommando (**Walk**, **Look**).
3. Das autonome Verhalten (an/aus: Details › Walker › Autonomous oder der Knoten
   **Set Autonomous**).

Zufall kommt aus einem Generator mit dem Actor als Seed, daher läuft die Ente deterministisch.

*[gemessen]* Test `micro_duck_roams_on_its_own_with_its_tof`: In einem 1,5 × 1,5 m großen Raum
(echte Größe) mit einer Kiste erkundet die Ente in 45 s die Fläche. Sie wendet vor den Wänden und
fällt nie.

## 10. Blueprint-Knoten (Kategorie Robots)

| Knoten | Wirkung |
|---|---|
| Walk | Twist wie das Gamepad (vorwärts bis 0,4 m/s, seitwärts 0,3 m/s, drehen 57 °/s); Dauer 0 = bis zum nächsten Walk |
| Look | Kopf gieren / nicken (Kamera und ToF schauen mit) |
| Set Autonomous | eigenes Verhalten an / aus |
| Quack | Quaken mit Nicken |
| Robot ToF | Hindernis gesehen, Entfernung, Richtung, freie linke / rechte Hälfte |
| Robot Camera Sees | ein Actor im Bild: sichtbar, Richtung, Entfernung |
| Robot State | Verhalten (Chill, Wander … Driven, Logic), gefallen, Geschwindigkeit |

Vorlage **Robot: walk and turn away with its ToF** (Templates im Blueprint-Editor): Die Ente
läuft per Logic und dreht weg, sobald der ToF etwas näher als 1,6 m sieht.

## 11. Aussehen und Kollision

- **Meshes:** Jedes Glied zeigt Pollens STL-Teile (`third_party/microduck/micro_duck_meshes.inc`,
  CC BY-NC-SA 4.0). Sie sind vereinfacht, pro Glied zusammengeführt, in die Home-Pose gestellt und
  tragen die MJCF-Materialfarben als Vertexfarben. Für die Vereinfachung gilt:
  - Verfahren: Vertex-Clustering mit Quadrik-Repräsentanten.
  - Bei den gedruckten Schalen bleibt die Zelle unter 1 mm. Sonst verschmelzen Innen- und
    Außenwand, und es entstehen Löcher.
  - Das Ergebnis: rund 85 000 Dreiecke.
- **Mund:** Der Unterschnabel (Kiefer und weiches Polster) ist ein eigenes Glied am Mund-Servo.
  - Bereich wie im Daemon: −5° (gegen das Polster gedrückt) bis 30°.
  - Die Drehachse geht durch die Augen des Kiefer-Meshes.
  - Masse und Trägheit stammen aus den Meshes (PLA 1,24, TPU 1,21 g/cm³) und sind vom Kopf
    abgezogen.
- **Kollision wie in MuJoCo:** Jedes kollidierende Mesh ist seine konvexe Hülle.
  - Sohlen: Reibung 1.
  - XL330-Gehäuse: reibungsfrei, wie im Training.
  - Schnabelpolster: Reibung 1. Sie greifen.
  - Schalen von Rumpf und Kopf: Reibung 0,5. Darauf liegt eine gestürzte Ente und rutscht nicht
    wie auf Eis. Die Schalen berühren die eigene Ente nie.
- *[gemessen]* Test `micro_duck_stands_on_the_floor`: Die Sohlen stehen 0,3–0,6 mm im Boden. Das
  liegt innerhalb von Box3Ds Kontakt-Slop von 5 mm. Früher waren die Sohlen Quader aus den
  Mesh-Maßen und standen 2,8 mm (echt) zu tief, deshalb schwebte die sichtbare Sohle.

## 12. Alle Bewegungen (Policy-Set, `manifest.json`)

Kern lädt dieselben `.onnx`-Dateien wie der Roboter (`third_party/microduck/`, Apache-2.0).
`velstand.onnx` ist eingebaut, die anderen lädt der Editor aus `editor/microduck/`.
`WalkerPolicy` liest das ONNX-Format selbst (rsl_rl-Actor: Normalisierer, dann Gemm und ELU).

Die Ablaufsteuerung folgt `robotd/src/control.rs`:

- Vorrang: Skill > Ground Pick > Sitzen/Aufstehen > Laufen.
- Ein Move zur Zeit, aus dem Sitzen keiner.
- Die Fenster laufen vor der Inferenz ab.
- Die Rückmeldung der letzten Aktion teilen sich alle Netze.

| Move | Netz | Kommando | Taste | Pad | Touch |
|---|---|---|---|---|---|
| Laufen | `velstand.onnx` (oder `alpha_walking` + `alpha_stand` unter Twist 0,05: Details › Walking network) | Twist, Kopf | W A S D | linker Stick | MOVE |
| Sitzen / Aufstehen | `alpha_sitstand.onnx` | Posture-Flag in vx (1 sitzen, 0 stehen); 2 s Sitzen, 1 s Aufstehen | 1 | B | SIT |
| Socke aufheben | `alpha_ground_pick.onnx` | Phase cos/sin(2π·φ), Periode 4 s, Ende bei φ = 0,7 | 2 | X | PICK |
| Kick links / rechts | `ball_kick_left/right.onnx` | null, 0,5 s | 3 / 4 | LT / RT | KICK L / R |
| Rolle | `roulade.onnx` | null, 1 s; gehalten rollt sie weiter (Chain-Fenster 0,15 s) | 5 | Y | ROLL |
| Rollschuh fahren | `roller.onnx` (Action-Scale 0,8) | Twist | W A S D | | |
| Ducken auf Rollen | `roller_crouch.onnx` (0,8) | Phase, Periode 5 s | 2 | X | PICK |
| Schnabel auf / zu | Mund-Servo | | 6 | | BEAK |

- **Schnabel beim Aufheben:** Er öffnet sich beim Ground Pick und schließt sich bei φ = 0,42,
  wenn der Schnabel am Boden ist. Das übernimmt auf dem Roboter der Client, also die App.
- **Action-Scale und Filter:** Der Daemon fährt den echten Roboter mit Scale 0,9 und einem
  Tiefpass auf den Sollwerten (Kopf 0,5, Beine 0,7). Das ist Sim-to-Real-Tuning für die Hardware.
  - Kern simuliert die Trainingsphysik und fährt deshalb jedes Netz wie trainiert: Scale 1, kein
    Filter. Nur das Action-Scale aus dem Set gilt (Rollen 0,8).
  - *[gemessen]* Mit 0,9 und Tiefpass kippen die Kicks um, mit 1,0 ohne Filter stehen sie.
- **Rollschuhe:** Katalogeintrag „Micro Duck (Rollers)“ (Rollen-MJCF
  `robot_groundcontact_rollers_backlash.xml`).
  - Die Knöchel tragen Kufen statt Füßen.
  - Je zwei freie Räder sind Scharniere ohne Antrieb, mit Lagerträgheit 0,0001 kg·m².
  - Die Reifen berühren den Boden als Mesh-Hülle, Reibung 1.
  - Die Ente steht 23,5 mm (echt) höher.
- **Gelenkgrenzen:** Alle 15 Servo-Gelenke haben harte Box3D-Grenzen aus dem MJCF.
  - *[gemessen]* Test `micro_duck_runs_every_move_of_its_set`: Bei allen Moves überschreitet kein
    Gelenk seine Grenze um mehr als 0,004 rad.

*[gemessen]* Test `micro_duck_runs_every_move_of_its_set`:

| Move | Ergebnis |
|---|---|
| Sitzen und Aufstehen | ok |
| Socke aufheben | ok, tiefste Neigung 0,78 |
| Kick links | ok, der Ball fliegt 4 m (echt) |
| Kick rechts | kippt manchmal und steht wieder auf |
| Rolle | dreht sich ganz und steht wieder |
| Ducken auf Rollen | ok |
