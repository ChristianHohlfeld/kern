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
| vx = 0,4 m/s | ≈ 0,1–0,16 m/s echt | ≈ 0,17 m/s |
| ωz = 1 rad/s im Stand | ≈ 0,4–0,7 rad/s echt | ≈ 0,97 rad/s |
| liegt auf Bauch oder Rücken | steht nach höchstens 12 s wieder | steht auf |

**Was fehlt, und warum:**

- **Armatur (Rotorträgheit, 0,0018 kg·m² pro Gelenk).** MuJoCo addiert sie in reduzierten
  Koordinaten auf die Gelenkachse. Box3D rechnet in maximalen Koordinaten, und ein Getriebe-Joint
  existiert nicht.
  - Eine Nachbildung über Zusatzmomente oder Geschwindigkeiten würde die Physik umgehen
    (Abschnitt 1).
  - Die Gelenke der Ente reagieren deshalb schneller als im Training. Das ist der
    wahrscheinlichste Grund für das langsamere Drehen.
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
