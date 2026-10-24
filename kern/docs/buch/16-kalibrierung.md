# 16 Kalibrierung: von der Simulation zum echten Roboter

Ein Netz, das im Lehrer lernt, soll auf der echten Micro Duck laufen. Dafür muss die Simulation den
echten Roboter treffen, und das Netz darf sich auf nichts verlassen, was die Simulation nicht genau
weiß. Kern verfolgt dafür drei Wege:

1. **Gerätemodelle mit Profilen:** Was ein echtes Gerät über die starre Physik hinaus tut (ein Servo
   mit Regler, Reibung, Latenz), ist ein kleines Modell mit Parametern in echten Einheiten. Profile
   werden an Messungen angepasst (`--kalibrieren`) und an anderen Messungen geprüft.
2. **Abgleich des ganzen Roboters** (`--abgleich`): ein Log des echten Roboters, abgespielt in der
   Simulation; der Unterschied ist der verbleibende Abstand zur echten Welt.
3. **Domänen-Randomisierung:** Was die Messung nicht festlegen kann, variiert das Training.

Box3D bleibt dabei unangetastet; es rechnet starre Körper, Gelenke und Kontakte
([Kapitel 6](06-physik.md)). Dazu kommt ein vierter Grundsatz: **in der Physik messen statt
eintragen** – Größen, die aus Modell und Physik folgen, misst der Lehrer selbst, statt sie als Zahl
in den Code zu schreiben.

| Teil | Wo | Was |
|---|---|---|
| Gerätemodell Servo | `include/kern/game/servo.h`, `src/game/servo.cpp` | Gesetz des Positionsservos, Profil als JSON |
| Profil XL330 | `game::xl330_profile()` | Pollens Werte (BAM-Modell „m6“ bei 7,4 V) |
| Prüfstand | `src/lehrer/kalibrierung.cpp` | ein Servo mit bekannter Last in Box3D, Anpassung mit CMA-ES |
| Abgleich | `compare_robot_log` | ein Log des ganzen Roboters gegen die Simulation |
| Werkzeug | `kern_lehrer --kalibrieren / --pruefen / --abgleich / --profil / --beispiele` | |

## 16.1 Das Servomodell

Ein Positionsservo wie der Dynamixel XL330 wird so simuliert, mit derselben Funktion
`game::servo_step` im Studio, im Lehrer und auf dem Prüfstand ([Kapitel 13.4](13-micro-duck.md#134-das-servo-modell)):

```
Moment = clamp(K · (Ziel − Winkel), −T, T) − D · Geschwindigkeit
       + Coulomb-Reibung F im Gelenk
       + Trägheit des Rotors durch das Getriebe (Armatur A) auf der Gelenkachse
das Kommando kommt delay Sekunden nach dem Senden an
```

| Parameter | XL330 (Pollen) | JSON-Schlüssel | Einheit |
|---|---|---|---|
| K Steifigkeit | 0,554 | `stiffness` | N·m/rad |
| D Dämpfung | 0,053 | `damping` | N·m·s/rad |
| F Reibung | 0,0048 | `friction` | N·m |
| T Höchstmoment | 0,963 | `torque` | N·m |
| A Armatur | 0,0018 | `armature` | kg·m² |
| Verzögerung | 0,02 (Training: 0,015–0,030) | `delay` | s |
| Streuungen (relativ ±) | Armatur 0,1, Reibung 0,1, sonst 0 | `spreadStiffness`, `spreadDamping`, `spreadFriction`, `spreadTorque`, `spreadArmature` | – |
| Verzögerungsbereich | 0,015 … 0,030 | `delayMin`, `delayMax` | s |

Das Profil als Datei (`kern_lehrer --beispiele DIR` schreibt `xl330.json`; die Werte stehen dort
als float32 in doppelter Genauigkeit, hier gerundet):

```json
{
  "kind": "servo",
  "name": "xl330",
  "source": "Pollen Robotics microduck_rl: BAM m6 model of the XL330-M288 at 7.4 V, MJCF armature",
  "stiffness": 0.554, "damping": 0.053, "friction": 0.0048, "torque": 0.963,
  "armature": 0.0018, "delay": 0.02,
  "spreadStiffness": 0, "spreadDamping": 0, "spreadFriction": 0.1, "spreadTorque": 0,
  "spreadArmature": 0.1, "delayMin": 0.015, "delayMax": 0.03
}
```

`servo_profile_from_json` prüft: `kind` muss (falls vorhanden) `servo` sein, jeder Wert eine Zahl in
seinem Bereich (Gewinne 0 … 10⁶, Armatur bis 10³, Verzögerung und Streuungen 0 … 1), Steifigkeit und
Moment positiv; ist `delayMax` kleiner als `delayMin`, wird es angeglichen. Fehlende Schlüssel bleiben
0.

Ein anderer Servo ist ein anderes Profil. Ein Gerät, das sich grundsätzlich anders verhält (ein Motor
mit Momentenregelung, ein Linearaktor), bekommt ein eigenes Modell neben `servo_step`.

Die **Streuungen** sagen, wie sicher die Werte sind. Das Training zieht daraus je Episode für jeden
Servo eigene Werte und für den Bus eine Verzögerung (`DuckEnv::reset`):

```cpp
auto spread = [&](f32 value, f32 rel) { return rel > 0 ? value * rng_.uniform(1 - rel, 1 + rel) : value; };
for (i32 link = 1; link <= kActions + 1; ++link) {        // 14 Policy-Servos und der Mund
  game::ServoProfile p = servo;
  p.stiffness = spread(servo.stiffness, servo.spread_stiffness);
  ...
  game::set_micro_duck_servo(l, duck_, link, p);
}
controller->servo_delay = servo.delay_max > servo.delay_min ? rng_.uniform(servo.delay_min, servo.delay_max) : servo.delay;
```

So hängt das Netz nicht an dem, was die Messung nicht festlegen konnte. Ein gut gemessener Servo
bekommt kleine Streuungen, ein schlecht bekannter große.

## 16.2 Prüfstand: ein Servo, gemessen und angepasst

### Messprotokoll

Aus `docs/kalibrierung.md`:

1. **Aufbau:** Servo fest eingespannt, Achse waagerecht, ein Arm mit Gewicht daran. Masse von Arm und
   Gewicht wiegen (0,1 g genau), Abstand des Schwerpunkts zur Achse messen (Arm auf einer Kante
   ausbalancieren), Trägheit um den eigenen Schwerpunkt aus der Form rechnen (bei kleinen Gewichten
   klein gegen m·r²). Winkel 0 = Arm hängt senkrecht nach unten.
2. **Servo wie im Roboter betreiben:** gleiche Firmware-Einstellungen (Positionsmodus, P/I/D-Gains,
   Profil), gleiche Spannung (7,4 V), Return Delay Time 0, gleicher Bus-Takt.
3. **Signale** je Last, mit mindestens 100 Hz Positionsabfrage: Sprünge zwischen mehreren Winkeln
   (klein und groß, z. B. ±0,2 und ±0,9 rad), ein Chirp 0,5 → 4 Hz mit 0,5 rad Amplitude, ein Rechteck
   ±0,3 rad.
4. **Zwei oder drei Lasten** (z. B. 50 g an 5 cm, 120 g an 6 cm). Mindestens ein Log je Servo wird
   nicht zur Anpassung benutzt (Prüf-Log).
5. **Mehrere Servos messen.** Die Streuung zwischen den Exemplaren wird zum `spread` im Profil.

### Log-Format

```
# mass 0.05
# arm 0.05
# inertia 1e-05
# horizontal 1
time,target,angle
0,-0.5,0
0.005,-0.5,-0.0123
...
```

- Kommentarzeilen `# Schlüssel Wert` beschreiben die Last (`ServoLoad`): Masse in kg, Abstand des
  Schwerpunkts zur Achse in m, Trägheit um den eigenen Schwerpunkt in kg·m², `horizontal 1` für eine
  waagerechte Achse (die Schwerkraft belastet den Arm wie ein Pendel), sonst nur Trägheit.
- `time` in Sekunden (nicht fallend), `target` der gesendete Zielwinkel zum Sendezeitpunkt, `angle`
  der gemessene Winkel, beide in Radiant. Zeilen, die nicht mit einer Zahl beginnen (die Kopfzeile),
  werden übersprungen; Trennzeichen sind Komma, Leerzeichen, Tab oder Semikolon.
- Mindestens zwei Zeilen, Masse positiv.

### Der Prüfstand in Box3D

`simulate_servo(profile, log)` baut den Servo mit seiner Last in Box3D, in derselben Umgebung wie die
Ente:

- **dieselbe Skalierung** (Maßstab 4, Froude-Faktoren wie in [Kapitel 13.3](13-micro-duck.md#133-das-modell)),
- **dieselbe Schrittrate** (100 Hz Welt, 16 Sub-Steps; ein Schritt = 5 ms echt),
- **dieselbe Gelenkweichheit** wie die Servogelenke der Ente (Constraint 240 Hz, Dämpfungsgrad 2),
- ein fester Sockel, ein Glied mit Masse und Schwerpunkt der Last, Trägheit der Last plus Armatur wie
  auf den Gliedern der Ente (A um die Achse, A/2 quer),
- Schwerkraft nur bei waagerechter Achse.

Dann spielt er die gesendeten Ziele ab, wie `GameWorld` sie einspeist: Das Ziel, das zum Zeitpunkt n
ankommt, wurde `delay` Schritte früher gesendet und gilt bis zum nächsten Sample. Ergebnis ist der
simulierte Winkel zu jedem Zeitpunkt des Logs; `servo_error` ist die Wurzel des mittleren
quadratischen Fehlers gegen die gemessenen Winkel.

### Anpassen und prüfen

```
kern_lehrer --kalibrieren steps.csv,chirp.csv --pruefen rechteck.csv --out xl330_unit3.json
kern_lehrer --kalibrieren steps.csv,chirp.csv --profil start.json --iterations 120 --threads 4
```

`calibrate_servo(start, fit, holdout, generations, seed, threads)`:

- **Parameter:** sechs Zahlen, x = 0 beim Startprofil (`--profil`, sonst XL330). Die fünf Gewinne
  K, D, F, T, A als **Logarithmus ihres Verhältnisses** zum Start (eine Einheit = Faktor e, begrenzt
  auf ±4), die Verzögerung in Einheiten von 10 ms (0 … 0,2 s). Im Log-Raum sind alle sechs
  Koordinaten gleich skaliert, wie CMA-ES es braucht.
- **Suche:** CMA-ES ([Kapitel 14.6](14-lehrer.md#146-nachahmen-vorbilder-und-cma-es)) mit σ = 0,5,
  Population 12, standardmäßig 80 Generationen (`--iterations` ändert das). Kosten: RMS-Fehler über
  alle Anpassungs-Logs zusammen (jedes Log nach seiner Länge gewichtet). Die Kandidaten einer
  Generation laufen auf `--threads` Threads.
- **Ergebnis:** der beste bewertete Punkt. `source` des Profils sagt „calibrated on n bench log(s)
  from …“.

Ausgabe:

```
calibrating xl330 on 2 bench log(s), 1 held out, 4 threads
fit: <vorher> -> <nachher> mrad rms;  held out: <vorher> -> <nachher> mrad rms (<n> simulations)
  K … N*m/rad  D … N*m*s/rad  F … N*m  T … N*m  armature … kg*m^2  delay … ms
wrote xl330_unit3.json
```

Entscheidend ist der **Prüf-Fehler** („held out“). Nur er sagt, ob das Modell den Servo erklärt oder
nur die Kurven auswendig kann. Deshalb gehört mindestens ein Log mit anderem Signal und anderer Last
in `--pruefen`.

Test `lehrer_calibrates_a_servo_from_bench_logs`: Ein Servo weicht vom Nennprofil ab (K −20 %,
D +30 %, F +50 %, T −10 %, A +20 %, 30 ms Verzögerung). Zwei Logs werden angepasst, ein drittes mit
anderem Signal und anderer Last geprüft. Der Prüf-Fehler sinkt dabei auf unter ein Fünftel und unter
3 mrad.

### Beispiel-Logs

```
kern_lehrer --beispiele beispiele/
```

schreibt Logs im richtigen Format, erzeugt in der Simulation mit dem XL330-Profil:

| Datei | Inhalt |
|---|---|
| `servo_steps.csv` | 50 g an 5 cm, waagerecht; Rechteck ±0,5 rad, 0,4 s je Stufe, 2,4 s bei 200 Hz |
| `servo_chirp.csv` | 120 g an 6 cm, waagerecht; Chirp mit 0,5 rad Amplitude, 2,4 s |
| `robot.csv` | 250 Regeltakte des ganzen Roboters (Abschnitt 16.3) |
| `xl330.json` | das Profil |

Sie zeigen das Format und eignen sich, die Werkzeugkette einmal durchzuspielen.

## 16.3 Abgleich: der ganze Roboter

Der Roboter zeichnet je Regeltakt (50 Hz) auf, was sein Netz gesehen hat (61 Werte) und was es
gesendet hat (14 Aktionen). Er steht zu Beginn still in Grundstellung.

**Format** (`parse_robot_log`): eine Kopfzeile, dann je Takt 75 Zahlen – 61 Beobachtungen, 14
Aktionen. `robot_log_csv` schreibt die Kopfzeile `obs0,…,obs60,act0,…,act13`.

```
kern_lehrer --abgleich roboter.csv --profil xl330_unit3.json
```

`compare_robot_log(log, servo)`:

- Die Simulation startet stehend auf dem sauberen Roboter (keine Variation) mit den Servos des
  Profils und bekommt Takt für Takt die Aktionen aus dem Log, über denselben Weg wie das Training
  (`DuckEnv`, externe Aktion, Scale 1, kein Tiefpass).
- Vor jedem Takt vergleicht sie, was ihre Sensoren melden, mit dem, was das Log sagt: Gyro (0–2),
  Schwerkraftvektor (3–5), Gelenkwinkel (6–19) und Gelenkgeschwindigkeiten (20–33).
- Ausgabe: RMS je Gruppe und der RMS-Fehler des Winkels für jedes der 14 Gelenke einzeln:

```
roboter.csv against the simulation (xl330), <n> ticks (<s> s):
  gyro … rad/s  gravity …  joints … rad  joint speeds … rad/s (rms)
    left_hip_yaw     … rad
    ...
    right_ankle      … rad
```

Das ist der verbleibende Abstand zur echten Welt. Er wird mit jedem besseren Profil kleiner. Bleibt
ein Gelenk auffällig, fehlt dort etwas im Modell, etwa Getriebespiel.

Test `lehrer_replays_robot_logs_against_the_simulation`: Ein Log aus der Simulation
(`simulated_robot_log`, im Test 120 Takte: velstand steht 50 Takte und geht dann mit 0,15 m/s
vorwärts; ab Takt 150 dreht es zusätzlich mit 0,6 rad/s), abgespielt mit denselben Servos, ergibt Abstand **0** – Bit für Bit, weil die
Simulation deterministisch ist ([Kapitel 14.7](14-lehrer.md#147-determinismus)). Mit anderen Servos
ergibt es einen messbaren Abstand.

Weil die Simulation auf jeder Plattform deterministisch ist, ist ein Unterschied zu einem echten Log
ein echter Modellfehler und kein Rauschen der Simulation.

## 16.4 Training mit dem kalibrierten Profil

```
kern_lehrer --task jump --profil xl330_unit3.json --out jump.onnx
```

Die Enten im Training bekommen die Servos des Profils mit seinen Streuungen (`Recipe::servo`,
`DuckEnv::servo`). Auf dem sauberen Roboter (Bewertung, `randomize = false`) gilt das Profil exakt
für alle Servos.

Hinweis: Die Spielschicht (`PlayEnv`) baut ihre Servos derzeit immer aus `xl330_profile()` und
variiert Reibung und Armatur innerhalb seiner Streuungen; `--profil` wirkt auf das PPO-Training der
Bewegungen (`--task`).

## 16.5 Domänen-Randomisierung

Was nicht gemessen ist oder streut, variiert das Training. Die vollständigen Bereiche stehen in
[Kapitel 14.3](14-lehrer.md#143-die-umgebung-der-ente); hier nach ihrer Rolle für Sim-to-Real
geordnet:

| Unsicherheit | Wie das Training sie abdeckt |
|---|---|
| Servo-Exemplare | je Servo eigene Werte innerhalb der Profilstreuung (XL330: Reibung, Armatur ±10 %) |
| Buslatenz | 15–30 ms je Episode |
| Bau (Akku, Kabel, Druck) | Rumpfmasse und -trägheit ±5 %, Schwerpunkt von Rumpf und Kopf (Sprung: Lehrplan bis ±15 / ±10 mm) |
| Boden | Sohlenreibung × 0,7–1,3 |
| IMU | Rauschen (Gyro ±0,03, Schwerkraft ±0,01), 0–1 Takt Verzögerung |
| Encoder | Rauschen ±0,001 rad, Versatz ±0,015 rad je Episode und Gelenk, Geschwindigkeit ±0,25 |
| Störungen | Stöße alle 3–6 s bis ±0,3 m/s (ab Iteration 300) |
| Kamera-Detektor (Spielschicht) | Positionsrauschen 0–4 mm + 0–3 % der Entfernung, 0–20 % Aussetzer |
| Spielzeug (Spielschicht) | Ballradius 3,5–6 cm, Masse 15–50 g, Rollwiderstand 0,01–0,05 |

Der Kritiker sieht dabei immer den sauberen Zustand; nur der Actor, der auf den Roboter kommt, sieht
die verrauschten Sensoren (asymmetrischer Kritiker, [Kapitel 14.3](14-lehrer.md#143-die-umgebung-der-ente)).

Getriebespiel ist nicht im Modell. Pollens Training randomisiert es (die MJCF-Varianten
`robot_*_backlash.xml`), und velstand ist dagegen robust; Kerns eigene Netze lernen ohne.

## 16.6 In der Physik messen statt eintragen

Manche Größen folgen aus Modell und Physik. Würde man sie als Zahl in den Code schreiben, stimmten
sie nach der nächsten Änderung an Reibung, Modell oder Servo nicht mehr. Kern misst sie deshalb in der
Simulation, so wie man sie am echten Roboter messen würde:

- **Die Bissposition** (`bite_spot`, `measure_bite`): Wo muss eine Socke vor der Ente liegen, damit
  Pollens Aufheben sie greift und beim Wegtragen hält? Der Lehrer legt die Socke auf ein Gitter von
  99 Stellen vor eine Ente, die gerade gegangen ist und steht, lässt das Aufheben mit dem
  Schnabelfenster laufen, das das Netz gezeigt bekommt, und nimmt die Stelle, die mit ihren Nachbarn
  am zuverlässigsten hält. Das geschieht einmal pro Prozess vor dem ersten Training; keine
  Bissposition steht als Zahl im Code ([Kapitel 14.8](14-lehrer.md#148-die-spielschicht-lernen)).
- **Der Nachlauf nach „Stopp“** (`--halt-messen`): wie weit die Ente aus einer langsamen Gangart
  noch geht. Hier ist die Regel noch nicht ganz umgesetzt: Das Vorbild der Spielschicht trägt den mit
  `--halt-messen` gemessenen Wert als Konstante ein (`kCoastM`, 0,7 cm aus 0,12 m/s); nach einer
  Änderung der Physik misst man neu.
- **Der Mund-Encoder** als Griffsensor: „hält etwas“ ist nicht eingetragen, sondern das, was der
  Servo meldet – auf „zu“ gestellt und trotzdem mehr als 2° offen.
- **Stillstand** in der Spielschicht: beide Sohlen am Boden, gemessen über die Kontakte, statt „kein
  Laufbefehl“.

Die **Paar-Reibung** der Oberflächen (TPU-Schnabelpolster auf Baumwolle 0,8, Baumwolle auf Baumwolle
0,5, Baumwolle auf Holzboden 0,4) ist dagegen eingetragen: Haftreibungswerte trockener Materialpaare,
im Code mit ihren typischen Bereichen begründet ([Kapitel 13.3](13-micro-duck.md#133-das-modell)).
Sie sind Kandidaten für eine Messung am echten Material.

## 16.7 Grenzen und nächste Schritte

- **Noch keine echten Messdaten.** Alles oben ist mit Logs aus der Simulation geprüft. Sobald ein
  Prüfstand misst, gilt dasselbe Werkzeug für echte Logs.
- **Getriebespiel** fehlt im Servomodell. Der Abgleich zeigt, ob es fehlt; dann bekommt `servo_step`
  einen Spielbereich.
- **Armatur** ist eine mitdrehende Scheibe, kein Rotor mit Übersetzung; dreht sich das Elternglied um
  dieselbe Achse, ist die Trägheit etwas zu groß ([Kapitel 13.3](13-micro-duck.md#133-das-modell)).
- **Bekannte Lücke:** `kick_right` kippt in Kern nach dem Tritt manchmal um. Kandidaten: Spiel,
  Kontaktweichheit, Fußreibung – ein Fall für den Abgleich mit einem Roboter-Log.
- **IMU:** Rauschen und Latenz stehen in der Variation des Trainings (`Task::Noise`). Ein eigenes
  IMU-Profil folgt, wenn ein Log zeigt, dass es nötig ist.
- **Reibungsmischung:** Box3D mischt Flächen ohne eigenes Paar als geometrisches Mittel, MuJoCo nimmt
  die Sohle. Im Beispiel-Level sind Boden und Sohle beide 1,0, die Mischung ist also gleich.
- **Kamera-Detektor:** Das Modell (Rauschen, Aussetzer) ist ein Gerätemodell ohne Messung am echten
  Detektor; seine Bereiche sind Annahmen des Trainings.
- **Skalierung:** Die 4-fache Ente ist der echte Roboter in anderen Einheiten; alle Profile und Logs
  sind in echten Einheiten, die Umrechnung geschieht nur an der Box3D-Grenze.
