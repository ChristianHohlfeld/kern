# 13 Die Micro Duck

Die Micro Duck ist der kleine zweibeinige Laufroboter von Pollen Robotics: rund 25 cm groß,
0,74 kg schwer, 14 Servos vom Typ Dynamixel XL330 in den Beinen und im Hals, dazu ein fünfzehnter
Servo für den Unterschnabel. Pollen trainiert ihre Bewegungen als neuronale Netze in MuJoCo
(Repository `microduck_rl`, Framework mjlab, Lernverfahren rsl_rl) und exportiert sie als
ONNX-Dateien, die der echte Roboter lädt.

Kern simuliert diesen Roboter auf Box3D und lässt genau diese Netze laufen. Das Ziel ist Treue:
dasselbe Modell, dieselben Servos, derselbe Regeltakt, dieselbe Beobachtung, dieselbe Aktion wie im
Training und auf dem Roboter. Darauf setzt Kern drei eigene Dinge:

- den **Lehrer**, Kerns eigenes Trainingssystem in C++ ([Kapitel 14](14-lehrer.md)),
- den **Sprung** (`policies/jump.onnx`), die erste eigene Bewegung,
- die **Spielschicht** (`policies/play.onnx`), ein gelerntes Netz, das über Pollens Bewegungen
  entscheidet, was die Ente mit einem Ball oder einer Socke tut.

Dieses Kapitel beschreibt die Ente in der Laufzeit: Modell, Servo, Takt, Netze, Sensoren, Gehirn,
Spielschicht und Steuerung. Wie man selbst Netze trainiert, steht in
[Kapitel 15](15-eigene-modelle.md), wie die Simulation an den echten Roboter angeglichen wird, in
[Kapitel 16](16-kalibrierung.md).

![Das Micro-Duck-Level im Studio](bilder/duck-level.png)

## 13.1 Wo der Code liegt

| Datei | Inhalt |
|---|---|
| `include/kern/game/walker.h` | Konstanten (`kWalkerStepHz`, `kWalkerSubsteps`, `kWalkerScale`, `kJumpSeconds`, `kJumpPhaseSlot`), `micro_duck_actors`, `add_micro_duck`, `vary_micro_duck`, `micro_duck_sock_actors` |
| `include/kern/game/walker_policy.h`, `src/game/walker_policy.cpp` | `WalkerPolicy`: ONNX-Netze lesen und rechnen, Register der geladenen Netze, das eingebaute Manifest; robotds Pick-up-Detektor (`PickupDetector`, `parse_onnx_pickup`, `pickup_detector()`) |
| `include/kern/game/servo.h`, `src/game/servo.cpp` | das Gerätemodell des Positionsservos und seine Profile |
| `src/game/walker.cpp` | Aufbau des Modells, Regler (`run_controller`), Steuerung (`drive_walkers`, darin die Pick-up-Erkennung), Sensoren (`sense_walker`), Kamera, Stimme, Lehrer-Schnittstelle |
| `src/game/walker_brain.cpp` | das autonome Gehirn (Port von Pollens `autod`, dazu BallPlay aus Pollens ursprünglicher Runtime) und der Idle-Blick |
| `src/game/walker_play.cpp` | die Spielschicht: Detektor, Tracker, Beobachtung, Ausführung von `play.onnx` |
| `include/kern/game/runtime.h` | die öffentliche API in `GameWorld` (Abschnitt „walkers“) |
| `src/game/micro_duck_model.inc`, `micro_duck_velstand.inc` | vom Importwerkzeug erzeugte Modelldaten und das eingebaute velstand-Netz |
| `third_party/microduck/` | Pollens Netze, Manifest, MJCF-Dateien, Meshes, Lizenzen |
| `policies/` | Kerns eigene Netze (`jump.onnx`, `play.onnx`) |

## 13.2 Grundsatz: die Physik nicht umgehen

Ein trainiertes Netz funktioniert nur, wenn die Simulation dieselbe Physik rechnet wie das Training.
Deshalb gelten in Kern feste Regeln für die Ente:

- Keine Hilfskräfte, keine gesetzten Geschwindigkeiten, keine Rumpfsperre, kein Teleport, kein
  Überblenden zwischen Netzen. Die Ente steht allein durch ihre Servos und Kontakte.
- Ein Servo besteht nur aus Box3Ds eigenen Gelenkteilen: der implizit gelösten Gelenkfeder und
  einem Motor mit Sollgeschwindigkeit 0 für die Reibung (Abschnitt 13.4).
- Fester Zeitschritt, gezählt in Schritten statt in Wanduhrzeit. Gleiche Eingaben ergeben Bit für
  Bit denselben Gang, auf jeder Plattform (Determinismus, [Kapitel 14](14-lehrer.md#147-determinismus)).

Die Physik selbst (Box3D, Kontakte, Gelenke) beschreibt [Kapitel 6](06-physik.md).

## 13.3 Das Modell

### Import aus dem MJCF

Pollen trainiert auf `robot_allcollisions_backlash.xml` (15 Bodies, 14 Gelenke). Das Werkzeug
`tools/micro_duck_import.py` liest diese Datei und `velstand.onnx` und erzeugt daraus
`src/game/micro_duck_model.inc` und `src/game/micro_duck_velstand.inc`. Aus Pollens STL-Dateien
liest es nur Maße (tiefster Punkt, Masseverteilung des Schnabels); das Aussehen zeichnet danach
`tools/kern_duck.py`, die Kollider schreibt es aus den gemessenen Hüllen:

```
python3 -I tools/micro_duck_import.py kern/third_party/microduck/robot_allcollisions_backlash.xml \
    <microduck_rl>/src/mjlab_microduck/robot/microduck/assets kern/third_party/microduck/velstand.onnx kern/src/game
python3 -I tools/kern_duck.py
```

Der Import prüft Gelenknamen und -reihenfolge, die Home-Pose gegen `default_joint_pos`, die
Reihenfolge der Beobachtung, `action_scale` und die Graphstruktur des Netzes (Sub, Div, 4× Gemm,
ELU). Weicht etwas ab, bricht er ab.

Je Glied enthält `kDuckLinks` (Struktur `DuckLinkDef`): MJCF-Body, Gelenkname, Elternglied,
Gelenkposition und -achse in der Home-Pose, Gelenkgrenzen, Home-Winkel, Masse, Schwerpunkt und den
vollen Trägheitstensor um den Schwerpunkt. Kern baut die Ente direkt in der Home-Pose
(STAND-Keyframe) auf und setzt `JointComponent::reference` auf den Home-Winkel, damit jedes Gelenk
genau den MJCF-Winkel liest.

### Glieder und Gelenke

Die Ente besteht aus 16 Gliedern: Rumpf, den 14 Gliedern der Policy und dem Unterschnabel
(`static_assert(kLinkCount == kJoints + 2)`). Die Reihenfolge der 14 Gelenke ist die der Policy:

| Index | Gelenk | Grenzen (rad) | Home (rad) |
|---|---|---|---|
| 0 | `left_hip_yaw` | −0,436 … 0,524 | 0 |
| 1 | `left_hip_roll` | −0,384 … 0,384 | −0,087 |
| 2 | `left_hip_pitch` | −1,571 … 1,571 | −0,458 |
| 3 | `left_knee` | −1,571 … 1,571 | −0,005 |
| 4 | `left_ankle` | −1,571 … 1,571 | 0,453 |
| 5 | `neck_pitch` | −1,571 … 1,047 | 0,349 |
| 6 | `head_pitch` | −1,571 … 1,571 | 0,349 |
| 7 | `head_yaw` | −2,967 … 2,967 | 0 |
| 8 | `head_roll` | −0,436 … 0,436 | 0 |
| 9 | `right_hip_yaw` | −0,524 … 0,436 | 0 |
| 10 | `right_hip_roll` | −0,384 … 0,384 | 0,087 |
| 11 | `right_hip_pitch` | −1,571 … 1,571 | 0,458 |
| 12 | `right_knee` | −1,571 … 1,571 | 0,005 |
| 13 | `right_ankle` | −1,571 … 1,571 | −0,453 |
| – | `mouth` | 0 … 0,611 | – |

Alle 15 Servogelenke haben harte Box3D-Grenzen aus dem MJCF. Die Glieder heißen in der Szene nach
ihrem Servogelenk, der Rumpf heißt „Micro Duck“. Im Actor-Layout ist der Rumpf der Walker-Controller
(Index 0), die Glieder sind seine Kinder; `ActorId` des Kopfs ist `trunk + 9` (der `head_roll`-Body),
die Knöchel mit den Sohlen sind die Glieder 5 und 14.

### Massen und Armatur

Masse, Schwerpunkt und Trägheitstensor kommen je Body aus dem MJCF und werden mit `set_mass_data`
gesetzt. Der Rumpf wiegt 0,199 kg, der Kopf 0,155 kg; in Summe 0,74 kg. Masse und Trägheit des
Unterschnabels stammen aus den Meshes (PLA 1,24 g/cm³, TPU 1,21 g/cm³) und sind vom Kopf abgezogen.

MuJoCo addiert die Rotorträgheit des Servos (Armatur, 0,0018 kg·m² je Gelenk) in reduzierten
Koordinaten auf die Gelenkachse. Box3D rechnet in maximalen Koordinaten und kennt keinen
Getriebe-Joint. Kern gibt die Armatur deshalb dem Glied als Masse-Eigenschaft (`add_armature`):

```cpp
// a/2 (E + axis axis^T): 'armature' um die Achse, die Hälfte quer dazu
const f32 add[6] = {1 + ax.x * ax.x, 1 + ax.y * ax.y, 1 + ax.z * ax.z, ax.x * ax.y, ax.x * ax.z, ax.y * ax.z};
for (int k = 0; k < 6; ++k) inertia[k] += 0.5f * armature * add[k];
```

Das ist eine dünne Scheibe auf der Gelenkachse und damit ein gültiger starrer Körper. Ein rein
axialer Tensor wäre keiner, und Box3D gewinnt damit Energie. Der Unterschied zu MuJoCo: Der echte
Rotor dreht mit 288:1 relativ zum Gehäuse, Kerns Scheibe dreht mit dem Glied. Dreht sich das
Elternglied um dieselbe Achse, ist die Trägheit dadurch etwas zu groß. Gemessen läuft und dreht die
Ente mit Armatur näher am Training und zappelt deutlich weniger (Abschnitt 13.14).

### Kollision

Was kollidiert, ist wie in MuJoCo jeweils eine konvexe Hülle – in Kern als Messdaten: grobe Hüllen aus 11 bis 40 Punkten, die angeben, wie weit die Teile des echten Roboters reichen (`tools/kern_duck_envelopes.json` → `src/game/duck_colliders.inc`). Eine Gestaltung ist darin nicht zu erkennen; sie sorgen dafür, dass die Ente die Welt genau so berührt wie im Training:

| Teil | Reibung | Oberfläche |
|---|---|---|
| Sohlen | 1 | – (Kollider mit `builtin:plastic_step` als Aufprallgeräusch) |
| XL330-Gehäuse | 0 (wie im Training) | – |
| Schnabelpolster | 1 | `tpu` |
| Schalen von Rumpf und Kopf (`trunk_base`, `jaw_soft`) | 0,5 | – |

Die Schalen umschließen die Servos der Nachbarglieder und berühren die eigene Ente nie (Kern setzt
dafür `no_collision`-Gelenke). Auf ihnen liegt eine gestürzte Ente und rutscht nicht wie auf Eis.

Seit der Überarbeitung der Spielschicht kennt die Physik **Oberflächenmaterialien mit eigener
Paar-Reibung** (`physics::Surface`: `cloth`, `tpu`, `floor`). Zwei Materialien mischen sich nicht
einfach: TPU auf Baumwolle hat einen eigenen gemessenen Wert, nicht das Mittel aus TPU auf TPU und
Baumwolle auf Baumwolle. `physics::surface_friction`
liefert für bestimmte Paare einen eigenen Wert, alle anderen Paare mischen wie Box3D als
geometrisches Mittel √(a·b):

| Paar | Reibung | Begründung im Code |
|---|---|---|
| Baumwollstrick auf Baumwollstrick (`cloth`/`cloth`) | 0,5 | Faden auf Faden 0,3, Strick auf sich 0,4–0,6 |
| TPU auf Baumwolle (`tpu`/`cloth`) | 0,8 | Elastomere auf Textil 0,6–1,0, je weicher desto höher |
| Baumwolle auf Holz- oder Laminatboden (`cloth`/`floor`) | 0,4 | Stoff auf Holz 0,3–0,5 |

Die Oberfläche trägt ein Kollider (`ColliderComponent::surface`), ein Mesh (`MeshComponent::surface`)
oder der Boden der Welt (`WorldSettings::ground_surface`, im Level-JSON `groundSurface`). Im
Micro-Duck-Level und in den Trainingsszenen ist der Boden `floor`.

Box3Ds Kontakt-Slop ist 5 mm; im Test `micro_duck_stands_on_the_floor` stehen die Sohlen 0,3–0,6 mm
im Boden.

### Maßstab 4

Box3D ist für Körper von 0,1 bis 10 m ausgelegt, die Glieder der Ente sind 2 bis 4 cm groß. Kern
baut sie deshalb 4-fach groß (`kWalkerScale = 4`) und rechnet an genau einer Stelle um
(`scaled()`), nach Froude-Ähnlichkeit:

| Größe | Faktor | bei S = 4 |
|---|---|---|
| Länge | S | 4 |
| Masse | S³ | 64 |
| Trägheit | S⁵ | 1024 |
| Moment, Servosteifigkeit | S⁴ | 256 |
| Dämpfung | S⁴·⁵ | 512 |
| Zeit | √S | 2 |
| Geschwindigkeit | √S | 2 |
| Winkelgeschwindigkeit | 1/√S | 1/2 |

Die Policy sieht immer echte Einheiten: Kern multipliziert Winkelgeschwindigkeiten mit √4 = 2,
bevor sie ins Netz gehen. Alle API-Werte für Lehrer, Logik und Sensoren sind ebenfalls in
Robotereinheiten (echte Meter, echte Sekunden), sofern nicht anders gesagt.

### Aussehen

Die Ente in Kern ist eine eigene Gestaltung (`tools/kern_duck.py` → `src/game/duck_meshes.inc`): ein
weißer Rumpf mit blauem Kern-Abzeichen, ein Kopf mit orangem Schnabel, Augen, Kamera und
Abstandssensor, dunkle Servoblöcke an den Gelenken, helle Streben dazwischen und orange Füße. Alles
ist aus abgerundeten Quadern (Superellipsoiden), Streben und Scheiben gebaut, in der Home-Pose und
mit Vertexfarben. Pollens 3D-Modelle (CC BY-NC-SA 4.0, nicht kommerziell) sind nicht enthalten;
übernommen sind nur Messdaten (die Gliedrahmen und die groben Kollisionshüllen), damit Sohlen,
Servos und Schnabel dort sitzen, wo sie beim echten Roboter sitzen.

### Rollschuhe

`micro_duck_actors(true)` baut die Rollen-Variante aus `robot_groundcontact_rollers_backlash.xml`
(Katalogeintrag „Micro Duck (Rollers)“): Die Knöchel tragen Kufen statt Sohlen, je zwei freie Räder
sind Scharniere ohne Antrieb mit Lagerträgheit 0,0001 kg·m², die Reifenhüllen berühren den Boden
mit Reibung 1, und die Ente steht 23,5 mm (echt) höher.

### Eine Ente in eine Szene setzen

```cpp
game::Level level;
const game::ActorId duck = game::add_micro_duck(level, {0, 0, 0});  // Rumpf auf z = 0
// add_micro_duck setzt level.world.step_hz = 100 und level.world.substeps = 16
```

`add_micro_duck` hängt die Actors an, verdrahtet Eltern und Gelenkziele und stellt die
Schrittrate der Welt ein. Das Studio fügt die Ente aus dem Katalog genauso ein und meldet die
geänderten World Settings.

## 13.4 Das Servo-Modell

Pollen trainiert mit dem BAM-Modell „m6“ des XL330-M288 bei 7,4 V. Kern bildet es als
**Gerätemodell** ab: ein kleines Gesetz in echten Einheiten über Box3D, dessen Parameter in einem
Profil (`ServoProfile`) stehen.

| Parameter | Wert (`xl330_profile()`) | Bedeutung |
|---|---|---|
| `stiffness` K | 0,554 N·m/rad | V · kt/R · 0,5755 |
| `damping` D | 0,053 N·m·s/rad | Gegen-EMK kt²/R plus viskose Reibung |
| `friction` F | 0,0048 N·m | Coulomb-Reibung im Gelenk |
| `torque` T | 0,963 N·m | Moment bei voller Aussteuerung V · kt/R |
| `armature` A | 0,0018 kg·m² | Rotorträgheit durch das Getriebe |
| `delay` | 20 ms | Kommando → Servo |
| `spread_armature`, `spread_friction` | ±10 % | Streuung im Training |
| `delay_min` … `delay_max` | 15 … 30 ms | Latenzbereich im Training |

Das Gesetz:

```
Moment = clamp(K · (Ziel − Winkel), −T, T) − D · Geschwindigkeit
+ Coulomb-Reibung F
```

Eine Funktion setzt es in jedem Physikschritt für jedes Servogelenk um, im Studio, im Lehrer und
auf dem Kalibrier-Prüfstand gleichermaßen (`game::servo_step`, aufgerufen aus
`GameWorld::drive_servos`):

```cpp
const f32 q = world.joint_angle(joint);
const f32 k = max_of(1e-6f, stiffness);
const f32 reach = torque > 0 ? torque / k : 1e9f;
world.set_joint_spring(joint, q + clamp(target - q, -reach, reach), k, max_of(0.0f, damping));
world.set_joint_motor(joint, friction > 0, 0, max_of(0.0f, friction));
```

- **Feder:** Box3Ds Gelenkfeder zieht zu `Winkel + clamp(Ziel − Winkel, ±T/K)`. Die Feder ist
  implizit gelöst und bei jeder Steifigkeit stabil. `set_joint_spring` rechnet N·m/rad mit der
  axialen effektiven Masse des Gelenks in Box3Ds Hertz und Dämpfungsgrad um, wie Box3D seine Feder
  auch intern rechnet.
- **Sättigung:** Die Begrenzung des Fehlers auf T/K ergibt das Haltemoment, so wie die sättigende
  PWM des Dynamixel. Bei K = 0,554 N·m/rad ist das volle Moment erst bei 1,7 rad Abweichung erreicht;
  die Servos sind weich.
- **Reibung:** ein Motor mit Sollgeschwindigkeit 0, dessen Höchstmoment die Reibung ist.
- **Strombegrenzung:** Mit der 1,75-A-Grenze der Firmware (0,64 N·m) kommt die Ente nicht mehr vom
  Rücken hoch. Pollen trainiert ausdrücklich ohne diese Grenze (`scripts/infer_policy.py`); Kern
  folgt dem.

`set_micro_duck_servos(level, trunk, profile)` gibt allen Gelenken einer Ente ein Profil (mit der
Armatur auf jedem Glied und der Busverzögerung), `set_micro_duck_servo(level, trunk, link, profile)`
einem einzelnen (Glied 1–14 in Policy-Reihenfolge, 15 der Mund). Profile als JSON und ihre
Kalibrierung beschreibt [Kapitel 16](16-kalibrierung.md).

## 13.5 Takt und Decimation

| | Training (MuJoCo) | Kern |
|---|---|---|
| Physikschritt | 5 ms (200 Hz) | 10 ms Weltzeit (`kWalkerStepHz` = 100 Hz), 16 Sub-Steps (`kWalkerSubsteps`) |
| Regeltakt | alle 4 Schritte (50 Hz) | alle 4 Schritte (Decimation 4) = 20 ms Roboterzeit |
| Kommandolatenz | 15–30 ms | 4 Physikschritte = 20 ms Roboterzeit (Profil `delay`) |

Weil die Zeit in der 4-fach großen Welt √4 = 2-mal langsamer läuft, entsprechen 10 ms Weltzeit genau
5 ms Roboterzeit. Kern berechnet Decimation und Verzögerung beim Start jeder Ente aus der
Schrittrate der Welt:

```cpp
w->decimation = max_of(1, lround(kControlPeriod * slow * hz));   // 0,02 · 2 · 100 = 4
w->delay      = max_of(0, lround(controller.servo_delay * slow * hz));
```

Passt die Schrittrate nicht, rundet Kern auf ganze Schritte und meldet die Abweichung („set World
Settings > Step Rate to 100“). Die Verzögerung ist eine Warteschlange `pending`: In jedem
Physikschritt bekommen die Servos das Ziel, das `delay` Schritte früher gesendet wurde.

`GameWorld::step` folgt „Fix Your Timestep“ von Glenn Fiedler: Der Frame liefert Zeit, die Simulation
verbraucht sie in festen Schritten, der Rest bleibt im Akkumulator, gerendert wird zwischen den
beiden letzten Zuständen, lange Frames werden begrenzt, und die Simulationszeit ist ein Schrittzähler
(siehe [Kapitel 8](08-spiel-laufzeit.md)).

Die Spielschicht entscheidet in jedem zweiten Regeltakt (`kPlayDecisionTicks = 2`), also mit 25 Hz.

## 13.6 Der Policy-Satz

Kern lädt dieselben ONNX-Dateien wie der Roboter. `velstand.onnx` ist in die Engine eingebaut
(`builtin_velstand_policy()`), alle anderen lädt die Seite (Studio) oder das Werkzeug zur Laufzeit
und meldet sie mit `register_walker_policy` an. `WalkerPolicy` liest das ONNX-Format selbst:

```cpp
struct WalkerPolicy {
  std::string file;                       // "velstand.onnx"
  std::vector<i32> dims;                  // 61, 512, 256, 128, 14
  std::vector<f32> mean, div;             // (obs - mean) / div
  std::vector<std::vector<f32>> weights;  // je Schicht [out][in] (Gemm, transB)
  std::vector<std::vector<f32>> biases;
  std::vector<std::pair<std::string, std::string>> metadata;
  void run(const f32* observation, f32* action) const;  // ELU zwischen den Schichten, linear am Ende
};
```

`parse_onnx_policy` akzeptiert genau die Form eines rsl_rl-Actors: Sub (Mittelwert), Div (Divisor),
dann Gemm mit Elu dazwischen und ein lineares Gemm am Ende, float32-Tensoren. Alles andere lehnt es
mit Begründung ab („not an rsl_rl actor“, „expected Elu after a hidden Gemm“, „unsupported ONNX
operator“ …). Das eingebaute velstand-Netz trägt seine Gewichte als exakte float32-Hex-Literale; der
Test `micro_duck_policy_matches_the_onnx_reference` prüft es gegen eine float64-Referenz bis 2·10⁻⁵.

### Die Netze

Das Manifest (`third_party/microduck/manifest.json`, Schema 2, `obs_len` 61, `action_len` 14,
`control_hz` 50) ist eingebaut (`micro_duck_manifest()`); `policy_set()` in `walker.cpp` liest daraus
Kommando-Kodierung, Dauer, Phase und `action_scale`.

| Bewegung | Netz | Art | Kommando | Taste | Pad | Touch |
|---|---|---|---|---|---|---|
| Laufen und Stehen | `velstand.onnx` (v8) | dauernd | Twist, Kopf, Körperpose | W A S D | linker Stick | MOVE |
| Laufen, ältere Gangart | `alpha_walking.onnx` + `alpha_stand.onnx` (unter Twist 0,05) | dauernd | Twist | W A S D | linker Stick | MOVE |
| Sitzen / Aufstehen | `alpha_sitstand.onnx` | skriptgesteuert | Posture-Flag in vx (1 sitzen, 0 stehen); 2 s Sitzen, 1 s Aufstehen | 1 | B | SIT |
| Socke aufheben | `alpha_ground_pick.onnx` | episodisch | Phase cos/sin(2π·φ), Periode 4 s, Ende bei φ = 0,7 | 2 | Y | PICK |
| Kick links / rechts | `ball_kick_left.onnx` / `ball_kick_right.onnx` | episodisch, 0,5 s | null | 3 / 4 | – | KICK L / KICK R |
| Rolle | `roulade.onnx` | episodisch, 1 s, Kette | null; gehalten rollt sie weiter (Fenster 0,15 s) | 5 (halten) | X | ROLL |
| Rollschuh fahren | `roller.onnx`, Action-Scale 0,8 | dauernd | Twist | W A S D | linker Stick | MOVE |
| Ducken auf Rollen | `roller_crouch.onnx`, Action-Scale 0,8 | episodisch | Phase | 2 | Y | PICK |
| **Sprung** (Kern) | `policies/jump.onnx` | episodisch, 2 s | Phase cos/sin in Plätzen 55/56 | 7 | – | HOP |
| Schnabel auf / zu | Mund-Servo (kein Netz) | | | 6 | – | BEAK |

Die Gangart wählt das Feld **Walking network** im Details-Panel (`gait`: `velstand` oder `alpha`).

### Ablaufsteuerung wie robotd

`run_controller` folgt `robotd/src/control.rs` in derselben Reihenfolge:

1. **Fenster zuerst:** Eine abgelaufene Bewegung beendet sich, wickelt ab (`unwind_s`) oder läuft
   bei gehaltener Kette weiter. Ein Aufstehen, dessen Zeit um ist, ist fertig.
2. **Netz wählen und Kommando umkodieren:** Vorrang hat eine laufende Bewegung (Skill), dann das
   Aufheben, dann Sitzen/Aufstehen, dann Rollen, dann Stehen (nur Gangart `alpha` unter Twist 0,05),
   sonst Laufen. Jede Bewegung bekommt die Befehlsplätze, mit denen sie trainiert wurde; Kopf- und
   Körperplätze werden für Skills genullt („zero-padded, as trained“).
3. **Inferenz**, dann Skalierung und Tiefpass.
4. **Fenster weiterschalten** nach dem Takt, der sie benutzt hat.

`walker_skill(id, "kick_left")` startet eine Bewegung wie der Daemon. Es gibt `""` zurück, wenn sie
startet, sonst den Grund: „a ground pick is running“, „a scripted move is already running“, „the robot
is sitting - stand it up first“, „… is not loaded“. Ein Move zur Zeit, aus dem Sitzen keiner. Die
Rückmeldung der letzten Aktion (Beobachtung 34–47) teilen sich alle Netze.

Ist das Netz einer Bewegung noch nicht geladen, springt kein anderes ein: Die Servos halten die
Home-Pose, und die Ente sagt einmal, welche Datei fehlt.

### Aktion → Servoziel

```cpp
f32 t = clamp(home[j] + scale * action[j], -kActuatorTravel, kActuatorTravel);   // ±π, safety.rs
if (has_previous) t = alpha * t + (1 - alpha) * previous_target[j];               // Tiefpass
```

- Kern fährt jedes Netz **wie trainiert**: Scale 1, kein Tiefpass. Nur das Action-Scale aus dem
  Manifest gilt (Rollen und Ducken 0,8).
- Der Daemon fährt den echten Roboter mit Scale 0,9 und einem Tiefpass (Kopf 0,5, Beine 0,7). Das
  ist Sim-to-Real-Tuning für die Hardware. Im Details-Panel wählt **Actions to servo goals**
  (`actions`) zwischen `training` (Standard) und `robot` (Daemon-Profil). Gemessen kippen die Kicks
  mit 0,9 und Tiefpass um und stehen mit 1,0 ohne Filter; laufen und stehen kann die Ente mit beiden
  (Test `micro_duck_walks_with_the_robot_daemons_action_profile`).
- Das Ziel bleibt im Verfahrweg des XL330 (±π), nicht im Gelenkbereich: Ein Ziel jenseits der Grenze
  drückt das Gelenk in Box3Ds harten Anschlag, wie in MuJoCo.

## 13.7 Beobachtung und Aktion (61 → 14)

Alle Pollen-Netze und Kerns Sprung haben dasselbe Layout:

| Index | Inhalt |
|---|---|
| 0–2 | Winkelgeschwindigkeit des Rumpfs im Rumpf-Frame (x vorn, y links, z oben), echte rad/s |
| 3–5 | Schwerkraft im Rumpf-Frame, aufrecht (0, 0, −1) |
| 6–19 | Gelenkwinkel − Home-Pose, in Policy-Reihenfolge |
| 20–33 | Gelenkgeschwindigkeiten, einen Regeltakt alt (so im Training) |
| 34–47 | letzte Aktion, roh |
| 48–50 | Kommando vx, vy, ωz: ±0,4 m/s, ±0,3 m/s, ±1 rad/s; EMA 0,2 pro Takt wie auf dem Roboter |
| 51–54 | Kopfkommando (Hals-Nicken, Kopf-Nicken, Gieren, Rollen) als Delta zur Home-Pose; Grenzen ±1,10 / ±1,10 / ±1,40 / ±0,31 rad |
| 55–60 | Körperpose; 57–59 tragen z, Rollen, Nicken. Plätze 55/56 nutzt Kerns Sprung für sein Phasensignal |

- **Aktion:** 14 Werte, Ziel = Home + Scale · Aktion.
- **Netz velstand:** (obs − mean) / Divisor, dann 512, 256 und 128 ELU-Neuronen, dann linear 14.
- `walker_observation(id, obs)` liefert zwischen zwei Takten genau, was der Regler beim nächsten Takt
  sieht (Test `micro_duck_observation_between_ticks_is_what_its_controller_sees`).
  `walker_last_observation` liefert die des letzten Takts, nachdem die Befehlsplätze geschrieben
  wurden.

## 13.8 Sensoren

Die Ente sieht nur, was der echte Roboter sieht. Alle Sensoren sitzen an den Sites, die das MJCF
festlegt (`head_camera`, `tof`, `imu`).

### Kopf-ToF (VL53L8CX)

| Eigenschaft | Wert |
|---|---|
| Zonen | 8 × 8 |
| Sichtfeld | 45° × 45° |
| Reichweite | 4 m |
| Bildrate | 15 Hz echt |

Jede Zone ist ein Box3D-Ray-Cast, der die eigenen Glieder überspringt (`cast_ray_ignoring`). Die
Klassifikation folgt `kinematics/src/tof.rs`:

- **Boden** (`zone = 2`): ein abwärts gerichteter Strahl, der mindestens 85 % der Sensorhöhe
  überwindet.
- **Zu nah** (`zone = 1`): horizontal unter 10 cm.
- **Hindernis** (`zone = 3`): alles andere; dazu die nächste horizontale Entfernung, ihre Richtung und
  die Belegung der linken und rechten Hälfte (`free_left`, `free_right`, blockiert unter 1 m).

`WalkerSensors` enthält außerdem die Treffer für das Hindernismodell des Gehirns (bis 0,35 m hoch,
bis 1,5 m weit), wie viele Strahlen nahe der Laufrichtung etwas gemeldet haben, die Schwerkraft, den
Sturz-Befund und die Geschwindigkeit.

### Kopfkamera (IMX219)

62° über die lange Seite des 16:9-Bildes; eingebaut um eine Vierteldrehung gedreht. Aufrecht ist das
Bild hochkant, 62° hoch und 37,4° breit. `walker_camera(id, cam)` liefert die Kamera,
`walker_sees(id, target, visible, bearing, distance)` prüft, ob die Mitte eines Actors im Bild liegt
und der erste Treffer eines Strahls dorthin der Actor selbst ist. Das Studio zeigt das Kamerabild
beim Spielen oben rechts im Viewport (Show-Flag **Robot Camera**), daneben das ToF-Gitter.

Blickt die Ente nach unten (Nacken −1,1, Kopf 1,1), schaut die Kamera 87° nach unten und sieht 6–15 cm
vor dem Rumpf (Test `micro_duck_camera_sees_the_floor_in_front_when_looking_down`).

### IMU und Sturz

Schwerkraft im Rumpf-Frame. „Gefallen“ heißt wie im Daemon: gz > −0,5, 200 ms lang
(`fall_gravity_z`, `fall_debounce_ms`). Ein kurzes Kippen beim Stolpern zählt nicht. Ist eine autonome Ente
gefallen, meldet `walker_state` „GetUp“; velstand v8 steht von selbst wieder auf.

### Mund-Encoder

`walker_holding(id)`: Ist der Mund-Servo auf „zu“ gestellt und steht trotzdem mehr als 2° offen,
steckt etwas im Schnabel. Auf nichts geschlossen ruht der Schnabel bei 0,0° an seinen Polstern, eine
Socke hält ihn bei 3,6–7,8° offen. Der Mund fährt −5° (gegen die Polster gedrückt) bis 30°.

## 13.9 Wer fährt

In jedem Regeltakt entscheidet `drive_walkers`, wessen Kommando gilt, in dieser Reihenfolge:

1. **Hand:** Der Stick des Spielers (über 5 % Ausschlag) oder eine Robot-Taste setzt `manual_left`
   auf 2 s. Solange es läuft, gilt die Hand, und Autonomie und Spielschicht pausieren.
2. **Logik:** ein Kommando von **Walk** bzw. **Look** (`walker_command`, `walker_look`), für die
   angegebene Dauer oder bis zum nächsten Kommando.
3. **Autonomie:** das Gehirn (`think_walker`) und darüber die Spielschicht (`play_walker`), wenn das
   Feld **Autonomous** (`autonomous`) an ist.

Twist, Kopf und Körperpose werden danach begrenzt und mit EMA geglättet (Twist 0,2, Kopf mit der Rate
des Absenders). Steht die Ente still (kein Twist, keine Bewegung, aufrecht) und hat seit 2 s niemand
den Kopf gesteuert, legt sich der **Idle-Blick** darauf (`idle_walker`, Port von
`robotd/src/idle_head.rs`): Blicke auf Minimum-Jerk-Kurven, kleine Sprünge, langsame Drift, Dösen
mit der Zeit und gelegentliches Gähnen.

### In der Hand: robotds Pick-up-Erkennung

Wie robotd (`[pickup]`, seit dem v2-Modell standardmäßig an; duck-control und robotd `pickup.rs`)
merkt die Ente, wenn eine Hand sie hochhebt, und tritt dann nicht in der Luft weiter. Pollens
Klassifikator `pickup_detector.onnx` (ein temporales CNN mit rund 8000 Parametern, als Bytes
eingebaut in `third_party/microduck/pickup_detector.inc`) liest in jedem Regeltakt die letzte
Sekunde: 50 Zeilen à 63 Merkmale (`kPickupWindow`, `kPickupFeatures`) – Gyro, Schwerkraft, Gelenke
minus Home, Gelenkgeschwindigkeiten, den Sollwert des vorigen Takts minus Home, Servoströme und ein
Pause-Flag. Er antwortet mit p(gehalten). Kern rechnet das Netz selbst und deterministisch
(`parse_onnx_pickup`: normieren und maskieren, per Gather die gelesenen Spalten, drei Conv1d mit ELU,
dann Gemm, ELU, Gemm, Sigmoid).

| Schritt | Regel (`drive_walkers`, nach duck-control `pickup.rs`) |
|---|---|
| Pausieren | p > 0,8 in 5 Takten hintereinander |
| Gehalten | kein Netz läuft; die Sollwerte rampen in 300 ms vom letzten Sollwert auf `PAUSE_POSE` (die mittlere Standhaltung der Lauf-Policy); `walker_driving` meldet `held`, `walker_state` „Held“ |
| Weiter | p < 0,35 in 4 Takten hintereinander, frühestens nach 300 ms Pause; die Netze starten frisch (letzte Aktion 0, Tiefpass ohne Anker), wie robotd nach einer Pause |

- Ausgewertet wird nur ein volles Fenster; ein aufgefülltes läge außerhalb der Trainingsverteilung.
- Beobachtet wird nur, solange Lauf- oder Stehnetz die Ente haben. Eine Bewegung, Sitzen,
  Rollschuhe (das Modell kennt keine Räder) oder der Lernende des Lehrers beenden eine Pause und
  leeren das Fenster.
- Ströme: Kern hat keine Servoströme. Das ausgelieferte Modell liest diese Spalten nicht (der Graph
  verwirft sie per Gather), Kern füllt 0.
- Im Studio hebt man die Ente in **Simulate** am Rumpf hoch (oder beim Zuschauen mit der Observer
  camera, wie im Beispiel-Level). Dieser Griff hält den Rumpf in seiner
  Ausrichtung wie eine Hand, statt ihn am Punkt pendeln zu lassen (`MouseGrab::begin` mit
  `hold_rotation`, [Kapitel 6](06-physik.md)).

*Gemessen:* Test `micro_duck_pickup_detector_matches_onnxruntime`: Kerns Rechnung weicht auf Pollens
zwei Fixture-Fenstern um weniger als 10⁻⁵ von onnxruntime ab. Test
`micro_duck_pauses_in_the_hand_and_walks_when_put_down`: 20 s autonom ohne falsche Pause; 15 cm
(echt) hochgehoben, pausiert sie und bleibt pausiert; abgesetzt läuft sie weiter und fällt nicht.

## 13.10 Das Gehirn: Autonomie

Fährt niemand die Ente, entscheidet sie selbst (`src/game/walker_brain.cpp`). Quelle ist Pollens
Daemon `autod` (Branch `autonomous-mode`, Commit `df9a750` vom 01.10.2026, Apache-2.0), mit seinen
Zuständen, Dauern, Übergangsgewichten und Zahlen. Die Zustände geben dieselben Kommandos wie ein
Gamepad: Twist, Kopfpose mit eigener Glättungsrate, Körperpose, Mund und Laute.

| Zustand | Was die Ente tut |
|---|---|
| Chill | steht, kleine Kopfdrifts, atmender Schnabel (3–7 s) |
| LookAround | große Blicke, schief gelegter Kopf (3,5–7 s) |
| Wander | läuft zu einem Ziel 1–3 m entfernt in einer von 16 Richtungen, bevorzugt wenig besuchte; Ende bei 0,25 m oder ohne 10 cm Fortschritt in 8 s (höchstens 90 s) |
| TurnInPlace | dreht mit 80–100 % von 1,5 rad/s, der Kopf voraus |
| Zoomies | volle Geschwindigkeit, ruckige Kurven, „Wheee“ (nur mit frischem ToF und über 1,2 m frei) |
| Stretch | streckt sich, gähnt, gurrt, schüttelt den Kopf |
| Ruffle | Federschütteln mit 4,5 Hz |
| Dance | Groove mit der Körperpose |
| Sneeze | einatmen, stocken, Niesen, benommen umschauen |
| GroundPick | die Aufheben-Bewegung, danach ein Quak |
| Nap | setzt sich und döst oder ruht (12–40 s) |
| Petted | gestreichelt (`walker_pet`) |
| BallPlay | sieht einen Ball, läuft an und richtet ihn vor dem linken Fuß aus (aus Pollens ursprünglicher Runtime nachgebaut, unten) |
| Kick | Pollens linker Kick, danach eine Quak-Salve und ein Blick in die Runde |

Darüber liegen wie im Original eine **Energie** (0–1), die den nächsten Zustand gewichtet, ein
**Neuheitsraster** (0,5-m-Zellen), **Quak-Salven**, **Hören** (`walker_hear`: eine Stimme bekommt
eine Antwort, ein Geräusch weckt kurz) und das **Hindernismodell** aus `obstacle.rs`: bestätigte
ToF-Treffer im Korridor voraus (halbe Breite 0,12 m, gemessen ab dem Schnabel 0,18 m vor dem Rumpf);
unter 0,45 m bremst und weicht sie aus, unter 0,15 m dreht sie weg und sperrt die Stelle für 5 min.

Die Odometrie ist die Rumpfpose. Kern hat kein Mikrofon und keinen Berührungssensor; Hören und
Streicheln kommen über die API. Zufall kommt aus einem Generator mit dem Actor als Seed, deshalb
läuft auch die Autonomie deterministisch. Solange eine Bewegung läuft, die Ente sich setzt oder
aufsteht oder gefallen ist, hält das Gehirn still.

### BallPlay: aus Pollens ursprünglicher Runtime, standardmäßig an

Pollens Runtime (`apirrone/microduck_runtime`, `autonomous.rs`) hatte einen Zustand BallPlay:
„approach / line up / kick“. `autod` hat ihn nur weggelassen, weil der Daemon noch keinen
Balldetektor hat (`docs/design/autonomous.md` §4). Der Code der Runtime ist nicht öffentlich. Kern
baut BallPlay deshalb nach dieser Beschreibung nach, mit Pollens eigenem Kick-Netz
(`ball_kick_left.onnx`) und der Ballposition aus dessen Training. BallPlay ist **standardmäßig an**:
Es gehört zu Pollens Software in Kern, nicht zu Kern extras.

Es läuft, wenn die Ente autonom ist, keine Rollschuhe trägt, `kick_left` geladen ist, die
Spielschicht nicht verfügbar ist (Abschnitt 13.11) und das Gehirn gerade nicht kickt, döst,
gestreichelt wird, aufhebt oder sitzt.

- **Sehen:** Ein Ball (dynamischer Kugel-Actor mit Tag `ball` oder Namen „Ball“, ohne Elternteil,
  auf dem Boden, also höchstens 8 cm hoch) im Bild der Kopfkamera (`walker_sees`: im Sichtfeld der
  IMX219-Optik und nicht verdeckt) startet BallPlay. Sieht sie mehrere, nimmt sie den nächsten und
  bleibt bei ihm.
- **Anlaufen:** mit gesenktem Kopf (nur der Kopf nickt, nicht der Nacken: Beugen verlagert das
  Gewicht nach vorn, und der Gang leidet), langsamer je näher, bis 35 cm. Ist der Ball mehr als
  0,6 rad seitlich, dreht sie erst auf der Stelle. Ein Hindernis-Tor gibt es dabei nicht: Was das ToF
  voraus sieht, ist der Ball.
- **Ausrichten:** in kleinen Schritten (0,2–0,25 m/s, nie unter dem, womit der Gang aus dem Stand
  anläuft), den Kopf auf dem Ball (−1,1 rad), bis er dort liegt, wo das Kick-Training ihn hinlegt
  (`microduck_ball_kick_env_cfg.py`: 9 cm vor, 4,2 cm neben dem linken Fuß, ±1,5 cm, gemessen vom
  Fuß). Verliert sie ihn dicht vor sich aus dem Bild, tritt sie mit gesenktem Kopf zurück, bis sie
  ihn wieder sieht; dort, wo er war, kickt sie nicht.
- **Kicken:** Kopf in Grundstellung (der Kick wurde so trainiert), 3 s ruhig, bis sie langsamer als
  1 cm/s ist, dann der Kick. Danach eine Quak-Salve, und dieser Ball ist 2–5 s uninteressant.
- **Aufgeben:** Einen Ball, den sie 6 s nicht gesehen hat, gibt sie auf, solange er weit weg ist
  (näher als 0,4 m geht sie nach der letzten Position). Nach 45 s ohne Kick lässt sie den Ball 20 s
  in Ruhe. In beiden Fällen folgt LookAround.

Abweichungen vom Original, soweit bekannt: Der Detektor ist ein geometrischer Sichttest statt eines
neuronalen Detektors im Kamerabild. Gekickt wird nur links, weil der rechte Kick die Ente in Kern
umkippt (Abschnitt 13.15). Anlauf- und Ausrichtwerte sind Kerns, weil die Werte der Runtime nicht
lesbar sind.

**Spielschicht statt BallPlay:** Mit Kern extras und geladenem `play.onnx` (`walker_play_available`)
startet das Gehirn kein BallPlay; dann spielt Kerns gelerntes Netz mit Ball und Socke
(Abschnitt 13.11). Während die Spielschicht eingreift, startet das Gehirn weder Aufheben noch
Sitzen.

## 13.11 Die Spielschicht (`walker_play.cpp`)

Pollens Netze sind die Bewegungen der Ente; keines davon sieht. Was die Ente mit einem Ball oder
einer Socke tut – hinschauen, hingehen, kicken, aufheben, wegtragen, ausspucken –, entscheidet ein
eigenes Netz: `policies/play.onnx`. Es ist Kern-eigen (nur mit Kern extras) und übernimmt dann statt
Pollens BallPlay (Abschnitt 13.10); Kerns früherer handgeschriebener Code für Fetch und Carry ist
entfernt. Die Spielschicht selbst enthält keine handgeschriebene Spiellogik; um das Netz herum gibt
es nur Wahrnehmung.

![Die Ente spielt](bilder/duck-play.png)

### Wo sie sitzt

Die Spielschicht sitzt an der Stelle eines Gamepads. In jedem zweiten Regeltakt (25 Hz) liest sie,
was der Detektor der Kamera meldet und was die Ente spürt, und antwortet mit Laufbefehl, Kopf,
Schnabel und welche Bewegung sie starten will. Sie ersetzt dann die Kommandos des Gehirns
(`auto_cmd`, `auto_head`, `auto_mouth`), und `walker_state` meldet „Play“.

Verfügbar ist sie, wenn

- **Kern extras** an ist (`kernExtras`, Details › Controller › „Kern extras (ball play, sock
  fetching, beak in the pick)“),
- die Ente keine Rollschuhe trägt und einen Mund-Servo hat,
- `play.onnx` geladen ist (oder der Lehrer sie extern steuert),
- **Autonomous** an ist: Laufbefehl, Kopf und Schnabel der Spielschicht laufen über den Kanal der
  Autonomie und wirken nur dann.

**Eingeschaltet** („engaged“) ist sie, solange sie ein Objekt kennt, etwas im Schnabel hat oder eine
selbst gestartete Bewegung läuft, und niemand von Hand oder per Logik fährt:

```cpp
const bool driven = w.manual_left > 0 || w.drive_left != 0;
p.engaged = p.external || (!driven && (p.track[0].known || p.track[1].known || holding || p.started_move));
```

Sonst läuft Pollens Gehirn wie bisher. **Handsteuerung pausiert die Spielschicht:** Ein Stick,
eine Robot-Taste oder ein Logik-Kommando schaltet sie aus, bis die 2 s Handzeit abgelaufen bzw. das
Logik-Kommando vorbei ist.

### Der Detektor: ein Gerätemodell

Der Detektor (`sense_play`) ist wie der Servo ein Gerätemodell mit Parametern, die der Lehrer
variiert:

- Er sucht je Klasse (Ball, Socke) das **nächste Objekt**, das im Kamerabild liegt und nicht verdeckt
  ist. Ein Ball ist ein dynamischer Kugel-Actor mit Tag `ball` oder dem Namen „Ball“, eine Socke ein
  Actor mit Tag `sock` (gesehen, wenn einer ihrer Teile sichtbar ist).
- Seine Position ist die Mitte des Objekts **auf dem Boden**: Ein Ball höher als 8 cm, eine Socke
  höher als 3 cm (echt) zählt nicht, etwa eine Socke im Schnabel.
- **Rauschen:** Standardabweichung = `sigma_abs` + `sigma_rel` · Entfernung (monokulare Schätzung,
  der Fehler wächst mit der Entfernung).
- **Aussetzer:** Mit Wahrscheinlichkeit `dropout` meldet ein Bild nichts.

```cpp
struct DetectorModel { f32 sigma_abs = 0, sigma_rel = 0, dropout = 0; u64 seed = 1; };
world.set_walker_detector(duck, {0.002f, 0.01f, 0.1f, 42});
```

Im Studio ist der Detektor rauschfrei; der Lehrer zieht seine Werte je Episode
([Kapitel 14](14-lehrer.md#148-die-spielschicht-lernen)). Das Rauschen kommt aus einem eigenen,
deterministischen Generator.

### Der Tracker

- Der Tracker behält die letzte Schätzung, wenn das Objekt aus dem Bild ist, mit ihrem **Alter**.
  Nach 3 s ohne Meldung vergisst er es, bei einem Objekt näher als 0,35 m (unter der Kamera, es
  bewegt sich nicht von selbst) erst nach 30 s.
- **Gewöhnung:** Einen eben gekickten Ball meldet er 15 s lang nicht, eine weggetragene und abgelegte
  Socke 40 s lang. Als abgelegt gilt eine Socke, die mindestens 0,25 m weit getragen wurde; ist sie
  beim Hochheben verloren gegangen, bleibt sie ein Ziel.
- **Tragen:** Vom Moment des Zugreifens an zählt er, wie weit und wie lange die Ente etwas trägt.

### Beobachtung (`kPlayObs` = 23)

Alle Positionen in echten Metern im **Kurs-Frame** der Ente (Rumpfrichtung, nur Gierwinkel): x voraus,
y links. Keine wahren Positionen.

| Index | Name (`observation_names`) | Inhalt |
|---|---|---|
| 0 | `ball_known` | 1, wenn der Tracker einen Ball kennt |
| 1, 2 | `ball_ahead`, `ball_left` | geschätzte Ballposition |
| 3 | `ball_age` | Sekunden seit der letzten Meldung (bis 3) |
| 4 | `sock_known` | 1, wenn der Tracker eine Socke kennt |
| 5, 6 | `sock_ahead`, `sock_left` | geschätzte Sockenposition |
| 7 | `sock_age` | Sekunden seit der letzten Meldung (bis 3) |
| 8, 9 | `left_foot_ahead`, `left_foot_left` | linker Knöchel relativ zum Rumpf |
| 10 | `holding` | Mund-Encoder: etwas im Schnabel |
| 11 | `beak_open` | Schnabelwinkel, 0 = zu (−5°), 1 = offen (30°), begrenzt auf −0,5 … 1,5 |
| 12 | `walking` | 1, wenn das Lauf- oder Stehnetz fährt |
| 13 | `picking` | 1, während das Aufheben läuft |
| 14 | `kicking` | 1, während `kick_left` läuft |
| 15 | `pick_phase` | Phase des Aufhebens (0 … 0,7) |
| 16–18 | `gravity_x/y/z` | Schwerkraft im Rumpf-Frame |
| 19 | `carried_m` | so weit getragen (bis 2 m), nur während sie hält |
| 20 | `carried_s` | so lange getragen (bis 10 s), nur während sie hält |
| 21 | `still_s` | so lange steht sie still (bis 4 s) |
| 22 | `walking_s` | so lange geht sie vorwärts (+) bzw. rückwärts (−) (±4 s) |

**Stillstand** ist neu definiert als das, was die Beine tun, nicht was befohlen wurde: Beide Sohlen
berühren etwas, das nicht die Ente selbst ist, die Spielschicht ist eingeschaltet, und keine Bewegung
läuft. Der Grund steht im Code: Auf „ein wenig vorwärts“ setzt Pollens Gang unter Umständen gar
nicht an – das ist Stehen, wo immer die Schwelle in dieser Physik oder auf der echten Ente liegt.

`walker_play_observation(id, out)` liefert die Beobachtung nebenwirkungsfrei und zwischen zwei
Takten exakt.

### Aktionen (`kPlayActions` = 9)

| Index | Name (`action_names`) | Wirkung |
|---|---|---|
| 0 | `forward` | vx = Wert · 0,3 m/s |
| 1 | `strafe` | vy = Wert · 0,2 m/s |
| 2 | `turn` | ωz = Wert · 1,0 rad/s |
| 3 | `head_yaw` | Kopf gieren, begrenzt auf ±1, · 1,1 rad |
| 4 | `head_pitch_down` | Kopf nicken, begrenzt auf −0,5 … 1, · 1,1 rad (+ schaut nach unten) |
| 5 | `beak_open` | > 0: Schnabel auf |
| 6 | `kick_left` | > 0,5: Pollens linken Kick starten |
| 7 | `ground_pick` | > 0,5: Pollens Aufheben starten |
| 8 | `neck_bow` | Nacken beugen: max(0, Wert), begrenzt auf 1, · 1,1 rad nach unten |

- Alle Werte werden zuerst auf ±1,5 begrenzt, nicht-endliche auf 0.
- Die drei Laufbefehle haben eine **Totzone wie ein Gamepad-Stick**: unter 20 % des Bereichs
  (0,06 m/s, 0,04 m/s, 0,2 rad/s) steht die Ente. Ein Netz, das 0,1 statt 0 antwortet, bleibt so
  stehen.
- **Rückwärts** fährt die Spielschicht den Gang an wie Pollens `autod` (`walkable` in
  `walker_brain.cpp`): die ersten 0,6 s mit voller Geschwindigkeit (0,3 m/s), danach nie langsamer
  als 0,15 m/s. Aus dem Stand setzt ein langsamerer Schritt zurück gar nicht an. Vorwärts gibt das
  Netz das Tempo selbst vor; es hat gelernt, dem Gang aus dem Stand einen kräftigen Anstoß zu geben.
- Der Kopf folgt mit Glättungsrate 0,3; Körperpose und „Wheee“ sind aus.
- Eine Bewegung startet nur, wenn keine läuft und der Schnabel leer ist; der Daemon lehnt sonst ab.
  Startet ein Kick, merkt sich der Tracker den Ball 15 s lang nicht.
- Während eines Aufhebens gehört der Schnabel dem Netz: Es hat gelernt, wann es öffnet und beißt.
  Ohne Spielschicht öffnet Kern extras den Schnabel bei Phase 0,16 und beißt bei 0,24.

### Socke und Ball

**Socke** (`micro_duck_sock_actors()`): sieben Stoffklumpen von 12,5 mm Durchmesser, lose gehäuft
3,4 cm hoch (vier am Boden, zwei darauf, einer oben), 25 g zusammen, jeder mit seinen Nachbarn durch
weiche Federn verbunden (Box3D-Distanzgelenke, 15 Hz, Dämpfung 0,7). Sie gibt unter dem Schnabel
nach, und was der Schnabel greift, zieht den Rest mit. Jeder Klumpen hat Oberfläche `cloth` und
Rollwiderstand 0,5: Runde Stoffklumpen würden sonst rollen, und der Haufen kröche über den Boden.

```cpp
std::vector<game::Actor> sock = game::micro_duck_sock_actors();  // Index 0 ist die Wurzel "Sock"
// Ids, Eltern und Gelenkziele wie bei add_micro_duck umrechnen, dann level.add(...)
```

**Ball:** ein dynamischer Kugel-Actor mit Tag `ball` (oder Name „Ball“). Im Beispiel-Level ein 12-cm-
Spielball von 50 g mit Rollwiderstand 0,03 (Gummi auf Holz); ohne Rollwiderstand rollte er ewig.

### Was im Beispiel-Level passiert

Das Micro-Duck-Level (`game::micro_duck_level()`) ist ein Raum von 2,5 × 2,5 m (echt) mit Holzboden
(Reibung 1, Oberfläche `floor`), Schuhkarton, Bücherstapel, Kiste, Blumentopf, einem Ball und zwei
Socken. Die Ente ist autonom und hat Kern extras an. Ein Level-Blueprint zeigt die Robots-Knoten:
Er lässt die Ente einmal quaken, wenn der Ball ins Kamerabild kommt. Das ist eine Vorführung des
Blueprints, nicht Verhalten der Ente.

### Ergebnisse

Gemessen über 32 Episoden je Zeile (`docs/lehrer.md`, Abschnitt 8; Einzelheiten in
[Kapitel 14](14-lehrer.md)):

| | Ball weggekickt | Socke aufgehoben | aufgehoben, weggetragen, ausgespuckt | Griffe, die hielten | Stürze |
|---|---|---|---|---|---|
| Netz, sauberer Roboter | 9 von 11 (im Mittel nach 17 s) | 4 von 21 | 1 von 21 | 2 von 14 | 2 von 32 |
| Netz, volle Variation | 7 von 11 | 13 von 21 | 0 von 21 | 1 von 15 | 10 von 32 |

Die Laufzeittests `micro_duck_plays_with_a_ball_on_its_own` (Ball 30 cm vor der Ente: gekickt, rollt
mehr als 25 cm weg, kein Sturz) und `micro_duck_goes_for_a_sock_and_bites` (Socke 30 cm vor der Ente:
hingehen, stehen, aufheben, zubeißen) prüfen in Studio-Physik, dass nur das Netz entscheidet.

## 13.12 Steuerung im Studio

![Telefonsteuerung](bilder/phone-play-390.png)

Die Eingaben landen in `PlayInput::robot`, einem Bitfeld der gehaltenen Robot-Tasten:

| Bit | Bewegung | Tastatur | Gamepad | Touch |
|---|---|---|---|---|
| 1 | Sitzen / Aufstehen | 1 | B | SIT |
| 2 | Aufheben (auf Rollen: Ducken) | 2 | Y | PICK |
| 4 | Kick links | 3 | – | KICK L |
| 8 | Kick rechts | 4 | – | KICK R |
| 16 | Rolle (gehalten: weiter) | 5 | X | ROLL |
| 32 | Schnabel auf/zu | 6 | – | BEAK |
| 64 | Quaken (Chirp) | Leertaste | RT | QUACK |
| 128 | „Wheee“ (gehalten) | V | LT | WHEEE |
| 256 | **Sprung** (Kern) | **7** | – | **HOP** |

Laufen: W/S oder linker Stick vorwärts/rückwärts (bis 0,4 m/s), A/D bzw. Stick quer drehen (bis
1 rad/s). Am Gamepad gilt eine radiale Totzone von 0,10 mit einer Potenzkurve 1,28. Auf dem Telefon
gibt es den MOVE-Stick und die Tasten oben; QUACK und PICK liegen in der unteren Reihe am Daumen.
Jede Robot-Taste und jeder Stickausschlag setzt die Handzeit auf 2 s und pausiert Autonomie und
Spielschicht.

Die Stimme folgt `robotd/src/sound.rs`: „greet“, wenn die Ente lebendig wird und wenn das Gamepad
zurückkommt, „inquire“, wenn es weg ist, „chirp“ bei einem neuen Druck auf Quaken, „wheee“ solange
gehalten, „peck“ beim Hinsetzen. Die Stimme ist eine bitgenaue Portierung von Pollens Synthesizer
(`src/audio/duck_voice.cpp`, [Kapitel 7](07-audio.md)), mit einer eigenen Stimme je Ente.

### Der Sprung

Taste **7**, Touch **HOP** oder der Logik-Knoten **Robot Move** mit „jump“ starten Kerns Sprung.
Er läuft wie Pollens Kicks: harter Wechsel auf `jump.onnx`, nach 2 s (`kJumpSeconds`) zurück. Das Netz
sieht die Phase t / 2 s als cos und sin in den Beobachtungsplätzen 55 und 56 (`kJumpPhaseSlot`), die
Pollens Netze nie nutzen; der Rest des Kommandos ist 0. Im Studio (Test
`micro_duck_jumps_for_real_in_the_studio`): 3 von 3 echte Sprünge, Steigen 9,0 mm, Flug 120 ms,
Neigung 8°, danach steht und läuft die Ente wieder. Wie das Netz entstand, steht in
[Kapitel 14](14-lehrer.md#149-ergebnisse-des-sprungs).

### Netze laden

Das Studio (`web/studio/game.mjs`) fragt die Engine nach fehlenden Netzen
(`kern_studio_pending_policies`) und lädt sie: Pollens Netze aus `microduck/`, Kerns eigene
(`jump.onnx`, `play.onnx`) aus `policies/` – auf der Entwicklerseite aus `third_party/microduck/` und
`kern/policies/`. Eine Datei, die nicht lädt, wird nach 5, 10, 20 s … (höchstens einmal pro Minute)
erneut angefragt. Netze, die der Lehrer im Browser auf demselben Gerät gespeichert hat (IndexedDB
„kern-lehrer“), lädt das Studio ebenfalls, und offene Studio-Tabs übernehmen ein neues sofort
(BroadcastChannel „kern-lehrer“).

### Details-Panel (Walker)

| Feld | JSON-Schlüssel | Bedeutung |
|---|---|---|
| Autonomous (camera, ToF) | `autonomous` | eigenes Verhalten, wenn niemand fährt |
| Walking network | `gait` | `velstand` (v8) oder `alpha` (alpha_walking + alpha_stand) |
| Actions to servo goals | `actions` | `training` (Scale 1, kein Filter) oder `robot` (0,9, Tiefpass 0,5 / 0,7) |
| Kern extras | `kernExtras` | Spielschicht (übernimmt dann statt BallPlay), Schnabel beim Aufheben |
| – | `rollers` | Rollschuh-Modell |
| – | `servoDelay` | Busverzögerung in Sekunden (Standard 0,02) |

### Blueprint-Knoten (Kategorie Robots)

| Knoten | Wirkung |
|---|---|
| Walk | Twist wie das Gamepad; Dauer 0 = bis zum nächsten Walk |
| Look | Kopf gieren / nicken |
| Set Autonomous | eigenes Verhalten an / aus |
| Chirp | Quaken mit offenem Schnabel |
| Robot Move | `sit`, `pick`, `kick_left`, `kick_right`, `roulade`, `jump` |
| Robot Beak | Schnabel auf (1) / zu (0) |
| Pet Robot | eine Hand auf der Ente für n Sekunden |
| Robot Hears | Stimme oder Geräusch |
| Robot ToF | Hindernis, Entfernung, Richtung, freie Hälften |
| Robot Camera Sees | ein Actor im Bild: sichtbar, Richtung, Entfernung |
| Robot State | Verhalten (Chill, Wander, …, BallPlay, Kick, Play, Held, Driven, Logic), gefallen, Geschwindigkeit |
| Robot Holds Something | der Mund-Encoder |

Die Logik selbst beschreibt [Kapitel 9](09-logik.md), das Studio [Kapitel 11](11-studio.md).

## 13.13 Die API in C++

```cpp
game::GameWorld& w = ...;                       // laufende Welt mit einer Ente
const game::ActorId duck = w.walkers().front();

w.walker_command(duck, 0.2f, 0, 0, 3.0f);       // 0,2 m/s vorwärts, 3 s lang
w.walker_look(duck, 0.5f, 0.2f, 2.0f);           // Kopf gieren / nicken
std::string why = w.walker_skill(duck, "pick");  // "" = gestartet
bool holds = w.walker_holding(duck);

game::GameWorld::WalkerSensors s;
w.walker_sensors(duck, s);                       // ToF, Schwerkraft, Sturz
bool visible; f32 bearing, distance;
w.walker_sees(duck, ball, visible, bearing, distance);

std::string net = w.walker_driving(duck);        // "walk", "stand", "ground_pick", "kick_left", "jump", "held", ...
std::string state = w.walker_state(duck);        // "Wander", "BallPlay", "Play", "Held", "Driven", "Logic", "GetUp", ...
```

Für den Lehrer gibt es zusätzlich `set_walker_external` / `set_walker_action` (eine lernende Policy
statt der Netze), `walker_tick_due`, `walker_probe` (Messwerte für Belohnung und Kritiker in echten
Einheiten), `walker_push` (Stoß) und für die Spielschicht `set_walker_play_external`,
`set_walker_play_action`, `walker_play_due` und `set_walker_detector`. Sie sind in
[Kapitel 14](14-lehrer.md) erklärt.

## 13.14 Gemessen

Test `micro_duck_stands_walks_and_turns_on_its_policy` und weitere:

| Fall | Kern | Pollen (MuJoCo, v8) |
|---|---|---|
| Kommando 0 | steht 8 s, driftet unter 2 mm | steht |
| vx = 0,4 m/s | ≈ 0,135 m/s echt (ohne Armatur 0,105) | ≈ 0,17 m/s |
| ωz = 1 rad/s im Stand | ≈ 0,61 rad/s echt (ohne Armatur 0,52) | ≈ 0,97 rad/s |
| Zappeln beim Laufen (Effektivwert der Winkelgeschwindigkeit) | Rumpf 1,0 rad/s, Kopf 1,9 rad/s (ohne Armatur 3,6 und 6,7) | – |
| auf Bauch oder Rücken | steht nach höchstens 12 s wieder | steht auf |

Test `micro_duck_runs_every_move_of_its_set`: Sitzen und Aufstehen, Aufheben (tiefste Neigung 0,78),
Kick links (Ball fliegt 4 m echt), Rolle und Ducken auf Rollen gelingen; Kick rechts kippt manchmal
und steht wieder auf. Kein Gelenk überschreitet seine Grenze um mehr als 0,004 rad.

Test `micro_duck_plays_ball_as_pollens_brain_does` (nur Pollens Software, ohne Kern extras): Die
Ente sieht den Ball, läuft an, richtet ihn aus und kickt mit Pollens Kick-Netz, ohne zu fallen; der
Ball rollt mehr als 25 cm (echt) weg. Die Pick-up-Erkennung prüfen die Tests in Abschnitt 13.9.

## 13.15 Was fehlt, und warum

- **Getriebespiel ±1°:** nicht modelliert. Im Training ist es Randomisierung, die Policy ist dagegen
  robust. Kandidat für den Abgleich ([Kapitel 16](16-kalibrierung.md)).
- **Armatur:** als mitdrehende Scheibe, nicht als Rotor mit Übersetzung (Abschnitt 13.3).
- **Reibungsmischung:** Box3D mischt zwei Flächen geometrisch, MuJoCo nimmt die Sohle. Auf einem
  Boden mit 0,6 ergäbe das 0,77, innerhalb der Trainingsvariation 0,7–1,3; im Beispiel-Level ist der
  Boden 1,0 und die Sohle greift mit 1,0.
- **Kick rechts:** kippt in Kern nach dem Tritt manchmal um. Die Ursache ist offen (Spiel,
  Kontaktweichheit, Fußreibung).
- **BallPlay:** nach der Beschreibung der Runtime nachgebaut, deren Code nicht öffentlich ist;
  Detektor als geometrischer Sichttest, nur der linke Kick (Abschnitt 13.10).
- **Servoströme:** Kern hat keine. Der Pick-up-Detektor bekommt 0; das ausgelieferte Modell liest
  diese Spalten nicht (Abschnitt 13.9).
- **Kern-eigen, nicht im Original:** die Spielschicht, das Schnabel-Timing beim Aufheben (beide nur mit
  Kern extras) und der Sprung. Immer an, weil reiner Klang: Kunststoff- und Schrittgeräusche, das
  Servo-Geräusch aus der Last.

Die vollständige Abweichungstabelle zu Pollens Runtime steht in `docs/walker.md`, Abschnitt 13.

---

© 2026 Christian Heinrich Hohlfeld · Alle Rechte vorbehalten · [Impressum und Rechte](00-impressum.md)
