# Kalibrierung: der digitale Zwilling

Ein Netz, das im Lehrer lernt, soll auf dem echten Micro Duck laufen. Dafür muss die Simulation den
echten Roboter treffen. Kern erreicht das, ohne Box3D anzufassen: Box3D rechnet die starren Körper,
Gelenke und Kontakte. Was ein echtes Gerät darüber hinaus tut (ein Servo mit seinem Regler, seiner
Reibung, seiner Latenz), ist ein kleines **Gerätemodell** mit Parametern in echten Einheiten, dem
**Profil**. Profile werden an Messungen des echten Geräts angepasst und an anderen Messungen geprüft.

| Teil | Wo | Was |
|---|---|---|
| Gerätemodell Servo | `include/kern/game/servo.h`, `src/game/servo.cpp` | Gesetz des Positionsservos, Profil als JSON |
| Profil XL330 | `game::xl330_profile()` | Pollens Werte (BAM-Modell „m6“ bei 7,4 V) |
| Prüfstand | `src/lehrer/kalibrierung.cpp` | ein Servo mit bekannter Last in Box3D, Anpassung mit CMA-ES |
| Abgleich | `compare_robot_log` | ein Log des ganzen Roboters gegen die Simulation |
| Werkzeug | `kern_lehrer --kalibrieren / --abgleich / --profil` | |

## 1. Das Servomodell

Ein Positionsservo wie der Dynamixel XL330 wird so simuliert (`game::servo_step`, dieselbe Funktion
im Studio, im Lehrer und auf dem Prüfstand):

- Moment = clamp(K × (Ziel − Winkel), −T, T) − D × Geschwindigkeit
- dazu Coulomb-Reibung F im Gelenk
- die Trägheit des Rotors durch das Getriebe (Armature A) auf der Gelenkachse
- das Kommando kommt `delay` Sekunden nach dem Senden an

| Parameter | XL330 (Pollen) | Einheit |
|---|---|---|
| K Steifigkeit | 0,554 | N·m/rad |
| D Dämpfung | 0,053 | N·m·s/rad |
| F Reibung | 0,0048 | N·m |
| T Höchstmoment | 0,963 | N·m |
| A Armature | 0,0018 | kg·m² |
| Verzögerung | 20 (Training: 15–30) | ms |

Das Profil trägt auch, wie sicher die Werte sind (`spread*`, `delayMin/Max`). Das Training zieht
daraus je Episode für jeden Servo eigene Werte und für den Bus eine Verzögerung. So hängt das Netz
nicht an dem, was die Messung nicht festlegen konnte.

Profil als Datei (`kern_lehrer --beispiele DIR` schreibt `xl330.json`):

```json
{
  "kind": "servo",
  "name": "xl330",
  "stiffness": 0.554, "damping": 0.053, "friction": 0.0048, "torque": 0.963,
  "armature": 0.0018, "delay": 0.02,
  "spreadArmature": 0.1, "spreadFriction": 0.1, "delayMin": 0.015, "delayMax": 0.03
}
```

Ein anderer Servo ist ein anderes Profil. Ein Gerät, das sich grundsätzlich anders verhält (ein
Motor mit Momentenregelung, ein Linearaktor), bekommt ein eigenes Modell neben `servo_step`.

## 2. Prüfstand: ein Servo, gemessen und angepasst

### Messprotokoll

1. **Aufbau:** Servo fest eingespannt, Achse waagerecht. Ein Arm mit Gewicht daran.
   - Masse von Arm und Gewicht wiegen (0,1 g genau).
   - Abstand des Schwerpunkts zur Achse messen (Arm auf einer Kante ausbalancieren).
   - Trägheit um den eigenen Schwerpunkt aus der Form rechnen (bei kleinen Gewichten klein gegen
     m·r²).
   - Winkel 0 = Arm hängt senkrecht nach unten.
2. **Servo wie im Roboter betreiben:** gleiche Firmware-Einstellungen (Positionsmodus, P/I/D-Gains,
   Profil), gleiche Spannung (7,4 V), Return Delay Time 0, gleicher Bus-Takt.
3. **Signale** (je Last), mit mindestens 100 Hz Positions-Abfrage:
   - Sprünge zwischen mehreren Winkeln (klein und groß, z. B. ±0,2 und ±0,9 rad)
   - ein Chirp 0,5 → 4 Hz, Amplitude 0,5 rad
   - ein Rechteck ±0,3 rad
4. **Zwei oder drei Lasten** (z. B. 50 g an 5 cm, 120 g an 6 cm). Mindestens ein Log je Servo wird
   **nicht** zur Anpassung benutzt (Prüf-Log).
5. **Mehrere Servos messen.** Die Streuung zwischen den Exemplaren wird zum `spread` im Profil.

### Log-Format (CSV)

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

`time` in Sekunden, `target` = gesendeter Zielwinkel (zum Sendezeitpunkt), `angle` = gemessener
Winkel, beides in Radiant.

### Anpassen und prüfen

```
kern_lehrer --kalibrieren steps.csv,chirp.csv --pruefen rechteck.csv --out xl330_unit3.json
```

- Der Prüfstand baut den Servo mit seiner Last in Box3D, in derselben Skalierung und mit derselben
  Gelenkweichheit wie die Ente, und spielt die gesendeten Ziele ab.
- CMA-ES passt K, D, F, T, A und die Verzögerung an, bis die simulierten Winkel die gemessenen
  treffen (kleinster quadratischer Fehler). Gestartet wird beim Profil `--profil` (sonst XL330).
- Ausgabe: Fehler vorher und nachher, auf den Anpassungs-Logs **und** auf den Prüf-Logs. Nur der
  Prüf-Fehler sagt, ob das Modell den Servo erklärt oder nur die Kurven auswendig kann.

Test `lehrer_calibrates_a_servo_from_bench_logs`: ein Servo, der vom Nennprofil abweicht (K −20 %,
D +30 %, F +50 %, T −10 %, A +20 %, 30 ms Verzögerung). Zwei Logs werden angepasst, ein drittes mit
anderem Signal und anderer Last geprüft. Der Prüf-Fehler sinkt dabei auf unter ein Fünftel und unter
3 mrad.

## 3. Abgleich: der ganze Roboter

Der Roboter zeichnet je Regeltakt (50 Hz) auf, was sein Netz gesehen hat (61 Werte) und was es
gesendet hat (14 Aktionen). Er steht zu Beginn still in Grundstellung.

```
kern_lehrer --abgleich roboter.csv --profil xl330_unit3.json
```

- Die Simulation startet stehend und bekommt Takt für Takt die Aktionen aus dem Log.
- Verglichen wird, was die Sensoren melden: Gyro, Schwerkraftvektor, Gelenkwinkel und
  -geschwindigkeiten (RMS, und je Gelenk).
- Das ist der verbleibende Abstand zur echten Welt. Er wird mit jedem besseren Profil kleiner.
  Bleibt ein Gelenk auffällig, fehlt dort etwas im Modell (z. B. Getriebespiel).

Test `lehrer_replays_robot_logs_against_the_simulation`: Ein Log aus der Simulation, abgespielt mit
denselben Servos, ergibt Abstand **0** (Bit für Bit, docs/lehrer.md, Determinismus). Mit anderen
Servos ergibt es einen messbaren Abstand.

Die Simulation ist deterministisch auf jeder Plattform. Ein Unterschied zum echten Log ist deshalb
ein echter Modellfehler und kein Rauschen der Simulation.

## 4. Training mit dem kalibrierten Profil

```
kern_lehrer --task jump --profil xl330_unit3.json --out jump.onnx
```

Die Enten im Training bekommen die Servos des Profils, mit seinen Streuungen.

## 5. Grenzen und nächste Schritte

- **Noch keine echten Messdaten.** Alles oben ist mit Logs aus der Simulation geprüft. Sobald ein
  Prüfstand misst, gilt dasselbe Werkzeug für echte Logs.
- **Getriebespiel** ist nicht im Modell. Pollens MJCF hat eine Variante mit Spiel
  (`robot_*_backlash.xml`). Der Abgleich zeigt, ob es fehlt; dann bekommt `servo_step` einen
  Spielbereich.
- **Bekannte Lücke:** `kick_right` kippt in Kern nach dem Tritt um (docs/walker.md, Abschnitt 12).
  Kandidaten: Spiel, Kontaktweichheit, Fußreibung. Das ist ein Fall für den Abgleich mit einem
  Roboter-Log.
- **IMU:** Rauschen und Latenz stehen in der Variation des Trainings (`Task::Noise`). Ein eigenes
  IMU-Profil folgt, wenn ein Log zeigt, dass es nötig ist.
- **Skalierung:** Die 4-fache Ente ist der echte Roboter in anderen Einheiten
  (docs/physics.md, Abschnitt 1).
