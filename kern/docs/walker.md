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
dieselben Kommandos wie ein Gamepad: Twist, Kopfpose (mit eigener Glättungsrate), Körperpose, Mund
und Laute.

**Quelle:** Pollens eigener Port von `autonomous.rs`, der Daemon `autod`
(github.com/pollen-robotics/microduck, Branch `autonomous-mode`, Commit `df9a750` vom 01.10.2026,
Apache-2.0; `autod/src/brain.rs`, `obstacle.rs`, `docs/design/autonomous.md`). Kern übernimmt ihn
originalgetreu, mit seinen Zahlen:

| Zustand | Was die Ente tut (Dauer) |
|---|---|
| Chill | steht, kleine langsame Kopfdrifts, atmender Schnabel (3–7 s) |
| LookAround | große Blicke, schief gelegter Kopf (3,5–7 s) |
| Wander | läuft zu einem Ziel 1–3 m entfernt (16 Richtungen), dorthin, wo sie am wenigsten war; dreht erst, wenn das Ziel über 0,7 rad daneben liegt; bremst unter 0,4 m; Ende bei 0,25 m oder nach 8 s ohne 10 cm Fortschritt (höchstens 90 s); kurze Seitenblicke |
| TurnInPlace | dreht mit 80–100 % von 1,5 rad/s, meist zur unbesuchtesten Richtung, der Kopf voraus |
| Zoomies | volle Geschwindigkeit, ruckige Kurven, das Wheee (2,5–5 s); nur mit frischem ToF und über 1,2 m frei |
| Stretch | streckt sich nach oben und hinten, gähnt, gurrt, schüttelt den Kopf (5,5–6,5 s) |
| Ruffle | schnelles Federschütteln mit 4,5 Hz (1,4–2 s) |
| Dance | Groove mit der Körperpose, der Kopf tanzt mit (5–8 s) |
| Sneeze | einatmen, stocken, HATSCHI mit Chirp, benommen umschauen (2,6–3,2 s) |
| GroundPick | die Ground-Pick-Bewegung, danach ein fröhlicher Quak (Gewicht 5 %) |
| Nap | setzt sich: döst (Kopf sinkt, schreckt hoch, Traumzucken) oder ruht sitzend (12–40 s), steht danach meist mit Stretch auf |
| Petted | gestreichelt: zerfließt, wackelt aufgeregt mit Chirps oder schmilzt schläfrig bis zum Nap, je nach Energie |

Darüber, wie im Original:

- **Energie 0–1:** Laufen und Zoomies verbrauchen, Chill, Nap und Streicheln füllen auf. Sie gewichtet
  den nächsten Zustand, das Tempo und wie gesprächig die Ente ist.
- **Neuheitsraster:** Verweildauer je 0,5-m-Zelle der Odometrie, nach etwa 15 min vergessen.
- **Quak-Salven:** 1–4 Chirps (45/30/18/7 %), alle 6–20 s × (1,8 − Energie), mit Schnabel und
  Kopfwurf. Beim Laufen wippt der Kopf.
- **Hören:** Eine Stimme bekommt ein aufmerksames Aufhorchen und nach 0,35–0,8 s eine Antwort. Ein
  Geräusch weckt eine dösende Ente kurz, ohne sie aufzuwecken. Eigene Laute zählen 1,5 s lang nicht.
- **Hindernisse** (`obstacle.rs`): bestätigte ToF-Treffer (ein zweiter innerhalb 8 cm), bis 0,35 m
  hoch und 1,5 m weit, im Odometrie-Rahmen 1,5 s gemerkt. Gefragt wird der Korridor voraus
  (halbe Breite 0,12 m), gemessen ab dem Schnabel (0,18 m vor dem Rumpf):

  | Weg voraus | Folge |
  |---|---|
  | frisch gesehen (≤ 0,9 s) | volle Geschwindigkeit |
  | älter, unter 1,5 s | halbe Geschwindigkeit |
  | nicht gesehen | nicht vorwärts, nur drehen |
  | Treffer unter 0,45 m | langsamer, ausweichen |
  | Treffer unter 0,15 m | wegdrehen; die Stelle ist 5 min für Ziele gesperrt |
  | drei harte Stopps kurz hintereinander | eingekesselt: Quak-Salve und schnelle Drehung |

- **Mindesttempo:** nie unter 0,15 m/s vorwärts, jeder Start 0,6 s mit vollem Tempo. Darunter
  bleibt die Gangart stehen.

**Was Kern anders hat:** Kern hat kein Mikrofon und keinen Berührungssensor. Hören und Streicheln
kommen deshalb über `walker_hear` und `walker_pet`. Die Odometrie ist die Rumpfpose; `autod` hält
seine Odometrie ebenfalls für perfekt.

**BallPlay, aus Pollens ursprünglicher Runtime, standardmäßig an.** Pollens Runtime
(`apirrone/microduck_runtime`, `autonomous.rs`) hatte 16 Zustände, darunter BallPlay: „approach /
line up / kick“ (`docs/ideas/autonomous_behavior.md`). `autod` hat es nur weggelassen, weil der Daemon
noch keinen Balldetektor hat (`docs/design/autonomous.md` §4; Pollen plant die Ballerkennung im
`mediad`, M5). Der Code der Runtime ist nicht öffentlich (der öffentliche Spiegel
`TommyZihao/microduck_runtime` stammt von März 2026, vor der Autonomie). Kern baut BallPlay deshalb
nach dieser Beschreibung nach, mit Pollens eigenem Kick-Netz (`ball_kick_left.onnx`) und der
Ballposition aus dessen Training:

- **Sehen:** Ein Ball im Bild der Kopfkamera (`walker_sees`: im Sichtfeld der IMX219-Optik und nicht
  verdeckt) startet BallPlay.
- **Anlaufen:** mit gesenktem Kopf, langsamer je näher, bis 35 cm.
- **Ausrichten:** in kleinen Schritten, den Kopf auf dem Ball, bis er dort liegt, wo das Kick-Training
  ihn hinlegt (`microduck_ball_kick_env_cfg.py`: 9 cm vor, 4,2 cm neben dem linken Fuß, ±1,5 cm,
  gemessen vom Fuß). Verliert sie ihn aus dem Bild, tritt sie zurück, bis sie ihn wieder sieht.
- **Kicken:** Kopf hoch (der Kick wurde mit dem Kopf in Grundstellung trainiert), 3 s ruhig, dann der
  Kick; danach eine Quak-Salve, und der Ball ist 2–5 s uninteressant.

Abweichungen vom Original, soweit bekannt: Der Detektor ist ein geometrischer Sichttest statt eines
neuronalen Detektors im Kamerabild. Gekickt wird nur links, weil der rechte Kick sie in Kern umkippt
(Abschnitt 12). Anlauf- und Ausrichtwerte sind Kerns, weil die Runtime-Werte nicht lesbar sind.

**Kern-Verhalten, nicht im Original** (Details › Controller › „Kern extras“, `kernExtras`): Kerns
gelerntes Spielnetz (`kern/policies/play.onnx`, 25 Hz, 23 Beobachtungen, 9 Aktionen, vom Lehrer
trainiert: `docs/lehrer.md`, „Spielen“) spielt mit Ball und Socke und übernimmt dann statt BallPlay
(`src/game/walker_play.cpp`). Es übernimmt, solange die Ente ein Objekt kennt, etwas im Schnabel hat
oder eine von ihm gestartete Bewegung läuft: Es gibt Twist, Kopf und Schnabel vor und startet Pollens
Kick und Ground Pick. Steuert jemand die Ente (Stick, Telefon, Taste) oder ein Logic-Kommando, wartet
das Netz. Das Beispiel-Level schaltet es ein.

*[gemessen]* Tests `micro_duck_roams_on_its_own_with_its_tof`, `micro_duck_plays_ball_as_pollens_brain_does`
(nur Pollens Software: sieht, läuft an, richtet aus, kickt; der Ball rollt weg),
`micro_duck_plays_with_a_ball_on_its_own` und `micro_duck_goes_for_a_sock_and_bites` (Spielnetz).

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
   **Set Autonomous**). Wie `autod` hält es still, solange eine Bewegung läuft, die Ente sich
   setzt oder aufsteht oder gestürzt ist.

Zufall kommt aus einem Generator mit dem Actor als Seed, daher läuft die Ente deterministisch.

### In der Hand (robotd: Pick-up-Erkennung)

Wie robotd (`[pickup]`, seit dem v2-Modell standardmäßig an; duck-control `pickup.rs`, robotd
`pickup.rs`, robotd-design §2.4.2). Pollens Klassifikator `pickup_detector.onnx` (temporales CNN,
8k Parameter) liest jede Regelperiode die letzte Sekunde (50 Zeilen à 63 Merkmale: Gyro,
Schwerkraft, Gelenke minus Home, Gelenkgeschwindigkeiten, Sollwert des vorigen Ticks minus Home,
Ströme, Pause-Flag) und gibt p(gehalten). Kern rechnet das Netz selbst (`parse_onnx_pickup`,
deterministisch, `kern::dm`); Abweichung zu onnxruntime auf Pollens Fixture-Fenstern unter 1e-5
(Test `micro_duck_pickup_detector_matches_onnxruntime`).

- Pause: p > 0,8 für 5 Ticks. Dann läuft kein Netz, die Sollwerte rampen in 300 ms vom letzten
  Sollwert auf `PAUSE_POSE` (die mittlere Standhaltung der Lauf-Policy). `walker_driving` meldet
  `held`.
- Weiter: p < 0,35 für 4 Ticks, frühestens nach 300 ms Pause. Die Netze starten frisch (letzte
  Aktion 0, Tiefpass ohne Anker), wie robotd nach einer Pause.
- Beobachtet wird nur, solange Lauf- oder Stand-Netz die Ente haben: Eine Bewegung, Sitzen,
  Rollschuhe (das Modell kennt keine Räder) oder der Lernende des Lehrers beenden eine Pause und
  leeren das Fenster.
- Ströme: Kern hat keine Servoströme; das ausgelieferte Modell liest diese Spalten nicht (der
  Graph verwirft sie per Gather), Kern füllt 0.

Hochheben in Kern Studio (Play, Ente am Rumpf greifen): Der Griff hält den Rumpf in seiner
Ausrichtung wie eine Hand, statt ihn am Punkt pendeln zu lassen.

*[gemessen]* Test `micro_duck_pauses_in_the_hand_and_walks_when_put_down`: 20 s autonom ohne eine
falsche Pause; 15 cm hochgehoben pausiert sie nach etwa 0,5 s und bleibt pausiert; abgesetzt
läuft sie weiter und fällt nicht.

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
| Robot Move | eine Bewegung des Sets: sit, pick, kick_left, kick_right, roulade |
| Robot Beak | Schnabel auf (1) / zu (0) |
| Pet Robot | eine Hand auf der Ente für n Sekunden (Petted) |
| Robot Hears | eine Stimme (sie antwortet) oder ein Geräusch (sie schreckt aus dem Dösen) |
| Robot Holds Something | der Mund-Encoder: etwas steckt im Schnabel |

Vorlage **Robot: walk and turn away with its ToF** (Templates im Blueprint-Editor): Die Ente
läuft per Logic und dreht weg, sobald der ToF etwas näher als 1,6 m sieht.

## 11. Aussehen und Kollision

- **Meshes:** Pollens STL-Modelle aus microduck_rl, von `tools/micro_duck_import.py` vereinfacht,
  pro Glied zusammengeführt und quantisiert (`third_party/microduck/micro_duck_meshes.inc`,
  CC BY-NC-SA 4.0, nicht kommerziell).
- **Mund:** Der Unterschnabel (Kiefer und weiches Polster) ist ein eigenes Glied am Mund-Servo.
  - Bereich wie im Daemon: −5° (gegen das Polster gedrückt) bis 30°.
  - Die Drehachse geht durch die Augen des Kiefer-Meshes.
  - Masse und Trägheit stammen aus den Meshes (PLA 1,24, TPU 1,21 g/cm³) und sind vom Kopf
    abgezogen.
- **Kollision wie in MuJoCo:** jedes kollidierende Teil eine konvexe Hülle, aus dem Import
  (`src/game/micro_duck_model.inc`).
  - Sohlen: Reibung 1.
  - XL330-Gehäuse: reibungsfrei, wie im Training.
  - Schnabelpolster: Reibung 1. Sie greifen.
  - Schalen von Rumpf und Kopf: Reibung 0,5. Darauf liegt eine gestürzte Ente und rutscht nicht
    wie auf Eis. Die Schalen berühren die Ente selbst nie.
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
| Socke aufheben | `alpha_ground_pick.onnx` | Phase cos/sin(2π·φ), Periode 4 s, Ende bei φ = 0,7 | 2 | Y | PICK |
| Kick links / rechts | `ball_kick_left/right.onnx` | null, 0,5 s | 3 / 4 | | KICK L / R |
| Rolle | `roulade.onnx` | null, 1 s; gehalten rollt sie weiter (Chain-Fenster 0,15 s) | 5 | X | ROLL |
| Sprung (Kern) | `kern/policies/jump.onnx` (vom Lehrer trainiert) | | 7 | | HOP |
| Rollschuh fahren | `roller.onnx` (Action-Scale 0,8) | Twist | W A S D | | |
| Ducken auf Rollen | `roller_crouch.onnx` (0,8) | Phase, Periode 5 s | 2 | Y | PICK |
| Schnabel auf / zu | Mund-Servo | | 6 | | BEAK |

- **Schnabel beim Aufheben:** Auf dem Roboter öffnet ihn der Mensch mit dem Trigger. Mit Kern extras
  und Spiel-Netz entscheidet das Netz (es hat es gelernt); ohne Netz öffnet Kern ihn beim Absenken
  bei φ = 0,16 und beißt bei φ = 0,24 zu.
- **Action-Scale und Filter:** Der Daemon fährt den echten Roboter mit Scale 0,9 und einem
  Tiefpass auf den Sollwerten (Kopf 0,5, Beine 0,7). Das ist Sim-to-Real-Tuning für die Hardware.
  - Kern simuliert die Trainingsphysik und fährt deshalb jedes Netz wie trainiert: Scale 1, kein
    Filter. Nur das Action-Scale aus dem Set gilt (Rollen 0,8).
  - *[gemessen]* Mit 0,9 und Tiefpass kippen die Kicks um, mit 1,0 ohne Filter stehen sie.
  - Umschaltbar pro Ente: Details › Controller › „Actions to servo goals“ (`actions`:
    `training` oder `robot` = Scale 0,9, Tiefpass Kopf 0,5 / Beine 0,7 wie `robotd-params`).
    Test `micro_duck_walks_with_the_robot_daemons_action_profile`: auch so steht und geht sie.
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

## 13. Abweichungen von Pollens Runtime (geprüft 23.10.2026)

Verglichen mit `pollen-robotics/microduck` (robotd, duck-control, robotd-params, autod im Branch
`autonomous-mode`). Kern ist eine eigene Engine auf Box3D; Pollens MJCF und Trainingswerte dienen
nur als Referenz für die Werte.

| Bereich | Original | Kern |
|---|---|---|
| Observation (61) | duck-control obs.rs | gleich; Inferenz = onnxruntime bis 1e-6 |
| Gelenkreihenfolge, Default-Pose | model.rs | gleich |
| Gelenkgrenzen | MJCF `range` | gleich, als harte Box3D-Grenzen |
| Sollwert-Klemmung | safety.rs ±π | gleich |
| Regeltakt | 50 Hz, Befehl 1 Tick verzögert | gleich (Physik 4× größer, Zeit √4 langsamer) |
| Action-Scale / Tiefpass | 0,9 und 0,5 / 0,7 | Standard wie trainiert (1,0, kein Filter); `robot` wählbar |
| Rollen, Ground Pick | Scale 0,8 / Phasen aus dem Manifest | gleich |
| Kopf-Kommando | ±1,10 / ±1,10 / ±1,40 / ±0,31 rad | gleich |
| Twist | robot.move unbegrenzt, Pad 0,4 / 0,3 / 1,0 | auf 0,4 / 0,3 / 1,0 begrenzt |
| Sturz | Schwerkraft z > −0,5 für 200 ms | gleich |
| In der Hand | robotd `[pickup]`: pickup_detector.onnx, Latch 0,8×5 / 0,35×4, 300 ms | gleich; Ströme 0 (vom Modell nicht gelesen) |
| Mund | −5° … 30°, nicht Teil der Policy | gleich |
| Idle-Look-around, Stimme | idle_head.rs, sounds | portiert |
| Autonomie | autod (12 Zustände); Runtime: dazu BallPlay | autod portiert; BallPlay nach Beschreibung nachgebaut (Abschnitt 9); ToF über Kerns Raycast, Hören/Streicheln per API |
| Policy umgangen? | – | nein: kein Pose-Setzen, kein Teleport, keine Zusatzkräfte, kein Blending |

**Kern-eigen (nicht im Original, nur mit „Kern extras“):** das Spielnetz für Ball und Socke
(Abschnitt 9), Schnabel-Timing beim Ground Pick. Immer an, weil reiner Klang: Kunststoff- und Schritt-Geräusche, Servo-Geräusch aus der Last.

**Kopf nach unten:** Nacken negativ und Kopf-Pitch positiv neigen nach unten. Nur Kopf 1,1: Kamera
56° nach unten, sieht 10–30 cm vor dem Rumpf. Nacken −1,1 und Kopf 1,1: 87°, sieht 6–15 cm, also
die Socke an der Greifstelle (Test `micro_duck_camera_sees_the_floor_in_front_when_looking_down`).
Beim Anlauf auf die Socke senkt sie den Nacken nicht: Das verlagert das Gewicht nach vorn, die
Füße setzen nach, und die Greifstelle ist verloren.

## 14. Stimme

Die Stimme der Ente ist der Synthesizer des Roboters (`src/audio/duck_voice.cpp`). Er ist eine
Portierung von `sounds/` aus pollen-robotics/microduck (Apache-2.0), das selbst
apirrone/microduck_sounds portiert. Es gibt keine Samples und kein Netz.

- **Eigene Stimme pro Ente:** Seed = die ersten 4 Bytes von sha256(Kennung). Der Roboter nimmt dafür
  seine Seriennummer, Kern den Namen und die Actor-Id. Aus dem Seed folgt die Persönlichkeit:
  Tonlage, Obertöne, Nasalität, Vibrato, Atem, Quakigkeit und Tempo.
- **Laute:** greet, chirp (je 12 Varianten), wheee (6, in Start, Schleife und Ende), alarm, inquire,
  peck und coo (je 10). Gespielt wird eine zufällige Variante.
  - Die Synthese ist bitgenau zum Original: Ein Vergleich aller 328 Puffer gegen das Rust-Crate
    ergibt dieselbe Länge und eine maximale Abweichung von 0.
  - Wie auf dem Roboter liegt jeder Laut als 48-kHz-Puffer mit 16 Bit vor (`to_wav`: × 32767,
    begrenzt, abgeschnitten). Erst danach wird er kubisch auf die Rate des Mixers gebracht.
  - Zu hören ist sie durch den Lautsprecher des Roboters, also mit einem Hochpass bei 300 Hz
    (`SPEAKER_ROLLOFF_HZ`). robotd spielt die WAVs trocken über aplay, und den Abfall macht dort der
    kleine Lautsprecher selbst; Kern bildet ihn mit diesem Filter nach.
  - Ein neuer Laut schneidet den laufenden ab. Während wheee werden andere Laute verworfen.
- **Wann sie ruft** (`robotd/src/sound.rs`, Pad-Belegung des Roboters):

| Anlass | Laut |
|---|---|
| Sie wird lebendig (Play, Simulate) und das Gamepad kommt zurück | greet |
| Das Gamepad ist weg | inquire |
| Neuer Druck auf RT, Leertaste, QUACK oder den Knoten **Chirp** | chirp, der Schnabel öffnet sich |
| LT, V oder WHEEE gehalten | wheee: Start, Schleife solange gehalten, Ende |
| Sie setzt sich | peck |

- **Wann nicht:** Im Leerlauf, beim Gehen, beim Umschauen und beim Anblick des Balls ruft sie
  nicht. Im Beispiel-Level ruft sie beim Ball nur, weil das der Level-Blueprint ausdrücklich so
  vorführt.
- **Servos:** Der Roboter und sein Training modellieren keine Servogeräusche, es gibt also keine
  Referenz. Kern leitet den Servo-Loop aus der Physik ab (`walker_servo_load`, jeden Regeltakt):
  - Drehmoment jedes Servos nach dem Servo-Gesetz, das Box3D löst (Steifigkeit × Fehler bis zum
    Haltemoment, minus Dämpfung × Drehzahl), als Anteil am Haltemoment: die Last, gegen die er arbeitet.
  - Lautstärke folgt der Arbeit (Last × Drehzahl) und leise der Haltelast, die Tonhöhe der Drehzahl
    der Motoren und etwas der Last. Gemessen: stehend Last 0,02 und Arbeit 0, gehend Last 0,06 und
    Arbeit 1,2 (Test `micro_duck_servo_load_drives_its_sound`). Leise, unter Schritten und Stimme.
- **Berührungen:** Die Gehäuse und Servo-Housings klingen bei jeder Berührung kurz und massiv nach Kunststoff
  (`builtin:plastic`), mit dem Boden wie miteinander. Die Sohlen klingen als leiser, kleiner
  Schritt (`builtin:plastic_step`, als Impact Sound ihrer Collider): dumpf und leise, ein Schritt
  pro Fuß höchstens alle 0,5 s (Simulationszeit), weil eine Sohle von der Ferse zur Spitze abrollt
  und dabei mehrere Treffer meldet - das waren die hörbaren Doppelschritte.
  - Schon ab 0,12 m/s Annäherung gibt es einen Ton (`kDuckImpactThreshold`), weil die Ente klein
    und leicht ist.
  - Beim Gehen sind es etwa zwei bis drei Schritte pro Sekunde. Ein Tappen kommt erst, wenn sie
    fällt, rollt oder sich selbst berührt.
  - Gemessen im Test `micro_duck_steps_and_taps_sound`.
