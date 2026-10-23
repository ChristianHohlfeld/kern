# Der Lehrer: eigene Netze für den Micro Duck

Kern trainiert Policies für den Micro Duck selbst. Alles läuft in C++ auf Kerns eigener Engine und
Box3D: Simulation, Lernverfahren, Export. Es gibt keine Python- oder PyTorch-Abhängigkeit. Gelernt
wird beim Nutzer: nativ auf allen Kernen (auch headless auf einem Server), im Browser auf Desktop
und Handy und später direkt im Studio.

Die Ente im Lehrer ist dieselbe wie im Studio: dieselbe GameWorld, dieselben Servos (Gerätemodell,
`docs/kalibrierung.md`), dieselben Sensoren, derselbe 50-Hz-Regler, dieselbe Physik (16 Substeps).
Ein Netz, das hier lernt, läuft deshalb unverändert im Studio. Es wird im Format von Pollens Netzen
exportiert (61 Beobachtungen, 14 Aktionen) und kann wie diese auf den Roboter.

## Stand

| Schritt | Stand |
|---|---|
| 1. Headless-Simulation: Ente ohne GPU, extern gesteuert, Episoden | fertig |
| 2. MLP mit Backprop und Adam | fertig |
| 3. PPO (wie rsl_rl: GAE, Clip, adaptive Lernrate, asymmetrischer Critic) | fertig |
| 4. ONNX-Export im Format von Pollens Netzen | fertig |
| 5. Variation (Domain Randomization) wie Pollens Rezepte, nativ auf allen Kernen | fertig |
| 6. Lernen im Browser (Web Worker, Desktop und Handy) | fertig |
| 7. Der Sprung: Vorbild → Nachahmen → Üben; als Bewegung im Studio | fertig |
| 8. Determinismus: Bit für Bit auf jeder Plattform und mit jeder Zahl Worker | fertig |
| 9. Digitaler Zwilling: Gerätemodelle, Kalibrierung, Abgleich (`docs/kalibrierung.md`) | fertig (ohne echte Messdaten) |

## 1. Die Umgebung (`src/lehrer`)

- **`DuckEnv`:** eine Ente auf ebenem Boden in einer eigenen GameWorld.
  - Der Renderer bleibt headless (`Renderer::set_headless`): Meshes bleiben auf der CPU, GL wird nie
    angefasst.
  - `reset()`, dann im Takt `observation()` / `step(action)`.
  - Ein `step` ist ein Regeltakt: 20 ms Roboterzeit, 4 feste Schritte der Welt.
- **Externe Steuerung:** `set_walker_external` und `set_walker_action`. Der Regler nimmt die Aktion
  des Lehrers statt eines Netzes. Die Servos bekommen sie wie trainiert: Grundstellung + Aktion,
  Skalierung 1, kein Tiefpass, mit der Totzeit des Servoprofils.
- **Beobachtung:** `walker_observation` liefert zwischen zwei Takten genau, was der Regler beim
  nächsten Takt sieht (Test `micro_duck_observation_between_ticks_is_what_its_controller_sees`).
  Die Befehlsplätze 48–60 schreibt die Aufgabe (einheitliches Layout, wie Pollens Fertigkeiten).
- **Messwerte für Belohnung und Critic:** `walker_probe`, in Robotereinheiten (nicht in der
  4-fachen Weltgröße): Rumpf-Lage und -Geschwindigkeit, Sohlen am Boden, Fußhöhe, Eigenkontakt,
  Bodenkontakt anderer Teile.
- **Prüfung:** Pollens velstand-Netz hält die Ente über diesen Weg stehend; gleicher Seed, gleiche
  Episode, Bit für Bit (Test `lehrer_duck_env_runs_episodes_headless`).

## 2. Netze, PPO, Nachahmung (`nn.cpp`, `ppo.cpp`)

- **`Mlp`:** ELU zwischen den Schichten, lineare Ausgabe, wie rsl_rls Actor und Critic. Gewichte
  eingangsweise, Initialisierung wie PyTorchs `nn.Linear`. Test: Gradient gegen finite Differenzen.
- **`Adam`** mit PyTorchs Standardwerten, Gradient vorher auf Norm 1 begrenzt.
- **`Normalizer`** wie rsl_rls EmpiricalNormalization; der Export schreibt ihn als Sub/Div ins ONNX.
- **PPO wie rsl_rl:** Gauß-Actor mit lernbarer Standardabweichung, asymmetrischer Critic, GAE
  (γ 0,99, λ 0,95), geclippter Surrogate und Wertverlust, Entropie, adaptive Lernrate über die KL.
  Vor dem Update wird die alte Policy mit den aktuellen Normalisierern neu bewertet (bei einigen
  Dutzend Umgebungen wandern die Normalisierer sonst merklich).
- **Nachahmung (`Ppo::imitate`):** Verhaltensklonen des Actors (mittlerer quadratischer Fehler,
  Adam, Minibatches in fester Reihenfolge).
- **Kritiker zuerst (`set_actor_frozen`):** Nach der Nachahmung lernt in den ersten 30 Iterationen
  nur der Critic. Sonst steuern Vorteile eines Critics, der die Erträge noch nicht kennt, einen Actor,
  der schon gut handelt, und zerstören ihn.
- **Warmstart:** Der Actor beginnt als Pollens velstand-Netz (Gewichte und eingefrorener
  Normalisierer). Die Plätze des Sprung-Phasensignals bekommen Gewicht 0.

## 3. Der Sprung

Pollen hat kein Sprung-Netz; dies ist das erste eigene. Die Aufgabe: auf das Kommando aus dem Stand
mit dem ganzen Körper abheben, landen und wieder stehen.

### Das Phasensignal

Der Sprung ist ein Befehl in den Beobachtungs-Befehlsplätzen, wie Pollens Ground-Pick: cos und sin
der Phase t / 2 s in den Plätzen 55 und 56 (`kJumpPhaseSlot`), die Pollens Netze nie nutzen. Sonst
nichts:

- kein Regler, keine Rückkopplung, kein Skript, keine IK, keine Posen-Vorgabe zur Laufzeit,
- keine Kräfte, Impulse oder Physik-Eingriffe.

Die Bewegung erzeugt allein das Netz über die 14 Servoziele, wie bei jeder anderen Fertigkeit.

### Was als echter Sprung zählt (`JumpMeter`)

- **Flug:** alle Teile der Ente mindestens 60 ms ohne Bodenkontakt.
- **Abheben:** Der Rumpf steigt in der Luft mindestens 5 mm über die Höhe beim Abheben. Nur die
  Füße anziehen ist kein Sprung.
- **Aufrecht:** nie mehr als 30° Neigung.
- **Landen:** kein Sturz (über 60° oder ein anderes Teil als die Sohlen am Boden), am Ende auf beiden
  Sohlen stehend.

Die erste, lockerere Messung (Rumpf irgendwann 15 mm über Standhöhe, 60 ms ohne Bodenkontakt) ließ
eine „Sprung“-Variante durch, bei der die Ente sich aufrichtete und dann die Füße anzog, mit 45°
Kippen. Das war kein Sprung; seitdem gilt die strenge Messung.

### Warum PPO allein nicht reicht

Die ersten Läufe (PPO, auch mit Lehrplan für den Sturz-Malus, Phasensignal, Warmstart von velstand,
Referenzhöhe in der Belohnung, verschiedenen Netzgrößen und Lernraten) fanden den Absprung nicht:

- Der Absprung braucht alle Beinservos gleichzeitig am Anschlag. Die Servos sind weich (0,554 N·m/rad,
  Höchstmoment bei 1,7 rad Abweichung): Das Netz muss Ziele weit hinter den Gelenkgrenzen setzen.
  Zufälliges Erkunden um eine stehende Policy erzeugt das praktisch nie.
- Stehen ist sicher und bringt Belohnung; ein missglückter Versuch kostet den Sturz.
- Mit wenigen Dutzend Enten sind die Minibatches klein und der Gradient verrauscht; die KL-Grenze
  hält die Lernrate unten.

### Vorbild → Nachahmen → Üben (`vorbild.h`)

1. **Vorbild suchen** (`--vorbild-suchen`): CMA-ES sucht in derselben Physik einen Sprung, den die
   Ente kann. Gesucht werden die Servoziele des linken Beins (Hüfte, Knie, Knöchel; das rechte
   spiegelt) zu fünf Zeitpunkten (Hocke, Abdruck, Anziehen, Landen, Abfedern). Vor der Hocke und ab
   der Hälfte der Phase hält Pollens velstand die Balance.
   Bewertet wird Steigen in der Luft, Flugzeit, Neigung, Sturz und Stehen, auf dem sauberen und auf
   variierten Robotern, jede Generation auf anderen. Nichts ist von Hand gesetzt: Die Physik
   entscheidet, was geht. Das Ergebnis ist eingebaut (`builtin_jump_demo`).
2. **Nachahmen** (DAgger, 8 Runden à 96 Episoden): In Runde 0 führt das Vorbild, danach das Netz
   selbst; das Vorbild sagt in jedem Takt, was es getan hätte. Das Netz lernt aus allen Beispielen.
   Die Episoden einer Runde lassen sich auf Threads oder Web Worker verteilen (`dagger_episodes`),
   in Episodenfolge zusammengefügt ergibt das dieselbe Runde.
3. **Üben** (PPO): mit der Sprung-Belohnung, einer Nachahmungs-Belohnung (Nähe zur Aktion des
   Vorbilds), kleinem Aktionsrauschen (0,1), vorsichtiger Lernrate und der vollen Variation, bis der
   Sprung robust ist.

Zur Laufzeit gibt es nur das Netz. Das Vorbild existiert nur, während es lernt.

### Belohnung (Übungsphase)

| Term | Gewicht | Was |
|---|---|---|
| airborne | 20 × aufrecht | im Flugfenster in der Luft und steigend (Rumpf über Abhebehöhe oder Steiggeschwindigkeit > 0) |
| lift | 20 × aufrecht | Steiggeschwindigkeit des Rumpfs im Absprungfenster (0–1 m/s) |
| landed | 6 × aufrecht | nach dem Sprung wieder auf beiden Sohlen |
| extra_hop | −4 | ein zweiter Hüpfer |
| imitate | 10 | exp(−mittlerer quadratischer Abstand zur Aktion des Vorbilds / 0,3²) |
| upright / tilt | 2 / −30 | aufrecht (exp(−g_xy²/0,05)); Neigung g_xy² kostet |
| pose_legs / pose_neck | 1 / 1 | vor und nach dem Sprung in Grundstellung |
| drift | −2 | horizontale Geschwindigkeit |
| action_rate | −0,1 → −0,5 | Änderung der Aktion (Lehrplan ab Iteration 200) |
| body_ang_vel, self_collisions | −0,05, −1 | wie Pollens Rezepte |
| fall | −2 → −20 (einmalig) | Lehrplan über 300 Iterationen; Sturz oder Bodenkontakt eines anderen Teils beendet die Episode |

Ohne Vorbild folgt der Rumpf zusätzlich einer Referenzhöhe (nur in der Belohnung).

### Ergebnis

`kern/policies/jump.onnx` (Lauf mit 32 Enten auf 2 Kernen, Seed 1, Rezept `recipe_for("jump")`):

| | sauberer Roboter (40 Episoden) | volle Variation (40 Episoden) |
|---|---|---|
| echte Sprünge | 39 von 40 | 21 von 40 |
| Steigen in der Luft | 11,3 mm | 7,1 mm |
| Flugzeit | 120 ms | 107 ms |
| Neigung (größte, Mittel) | 9° | 12° |
| Stürze | 0 von 40 (0 %) | 2 von 40 (5 %) |

„Volle Variation“ ist Pollens Kick-Rezept am Ende seiner Lehrpläne: Rumpfmasse ±5 %, Schwerpunkt
von Rumpf und Kopf ±15 / ±10 mm, jeder Servo mit eigener Reibung und Rotorträgheit (±10 %), Totzeit
15–30 ms, IMU 0–20 ms verspätet, Sensorrauschen, Encoder-Versatz, Sohlenreibung 0,7–1,3, ±3°
Startneigung. Die Ente fällt dort in 5 % der Fälle; wenn sie nicht als echter Sprung zählt, ist es
meist ein zu flacher (unter 5 mm Steigen), nicht ein Sturz.

Im Studio (Test `micro_duck_jumps_for_real_in_the_studio`, die Bewegung „jump“ im Level, Welt
Bild für Bild): 3 von 3 echte Sprünge, Steigen 8,7 mm, Flug 120 ms, Neigung 9°, danach steht und
läuft die Ente wieder.

Im Studio springt die Ente mit Taste **7**, dem Touch-Knopf **HOP** oder dem Logik-Knoten Robot Move
(Bewegung „jump“). Das Studio lädt `kern/policies/jump.onnx`; ein auf demselben Gerät im Browser
gelerntes Netz ersetzt es.

### Zeit bis zum ersten echten Sprung

Gemessen auf 2 Kernen (x86, nativ):

| Schritt | Zeit | Entenschritte | Ergebnis |
|---|---|---|---|
| Vorbild suchen (einmalig, eingebaut) | 18 min | rund 2 Mio. | Vorbild: 16 von 20 sauber, 0 Stürze |
| Nachahmen, 8 Runden | 2,3 min | 50 000 | ab Runde 7 echte Sprünge (5 von 5) |
| Üben, 1500 Iterationen | 33 min | 1,15 Mio. | robust (Tabelle oben) |

Das eingebaute Vorbild muss niemand neu suchen. Wer den Sprung selbst lernt, braucht also Nachahmen
(rund 2 min auf 2 Kernen bis zum ersten echten Sprung) und dann Übung für die Robustheit.

## 4. Export (`src/lehrer/onnx_export.cpp`)

- Eingang `obs` [1, 61], Ausgang `actions` [1, 14], Opset 17: Sub(Mittelwert), Div(σ + 0,01), dann
  (Gemm, Elu)* und Gemm, wie Pollens exportierte Netze.
- Metadaten: dieselben Schlüssel wie bei Pollen, dazu `task` und `lehrer`.
- Test `lehrer_exports_onnx_like_pollens_networks`: Kern liest die Datei und sie rechnet bis 1e-4
  genau wie der Actor im Lehrer. ONNX Runtime (die Laufzeit des Roboters) lädt sie ebenfalls.

## 5. Variation (Domain Randomization)

Wie Pollens Velocity- und Kick-Rezepte. Der Critic sieht den sauberen Zustand.

- **Start:** Gier zufällig, ±3° Neigung, Sohlenreibung 0,7–1,3.
- **Bau** (`game::vary_micro_duck`): Rumpfmasse und -trägheit ±5 %, Schwerpunkt von Rumpf und Kopf
  (Lehrplan 3 → 15 mm bzw. 3 → 10 mm ab Iteration 200 über 600 Iterationen).
- **Servos** (Gerätemodell, `docs/kalibrierung.md`): jeder Servo zieht seine Werte aus den Streuungen
  seines Profils (XL330: Reibung und Rotorträgheit ±10 %), der Bus eine Totzeit von 15–30 ms.
- **Sensoren** (was der Actor sieht): Gyro ±0,03, Schwerkraft ±0,01, Gelenkwinkel ±0,001 und je
  Episode ein Encoder-Versatz ±0,015, Gelenkgeschwindigkeit ±0,25, IMU 0–1 Takt verspätet (alle 64
  Takte neu gezogen).
- **Stöße:** alle 3–6 s ±0,3 m/s, ab Iteration 300.

## 6. Lernen nativ und im Browser

### Nativ (`tools/kern_lehrer.cpp`)

```
kern/tools/lehrer.sh                      # baut kern/build/kern_lehrer (Linux und macOS, x86 und ARM)
kern/build/kern_lehrer --task jump --iterations 1500 --out jump.onnx --log jump.csv --checkpoint run.ckpt
kern/build/kern_lehrer --eval jump.onnx --episodes 40      # sauber und mit voller Variation
kern/build/kern_lehrer --trace jump.onnx                   # eine Episode Takt für Takt
kern/build/kern_lehrer --vorbild-suchen demo.json          # ein neues Vorbild (CMA-ES)
```

- Alle Kerne (`--threads`), Fortschritt je Iteration, Checkpoints (`--resume`), Schnappschüsse
  (`--keep-every`). Läuft headless, z. B. auf einem Server.
- `--profil servo.json` lernt mit einem kalibrierten Servoprofil.

### Browser (`kern/web/lehrer/`, Seite `/lehrer/`)

- `kern/build.sh --lehrer` baut `kern_lehrer.wasm`: Engine, Box3D, Spiel-Laufzeit, `src/lehrer` und
  die C-Schnittstelle (`lehrer_api.cpp`).
- **Sammler:** Jeder Web Worker führt seine Enten (globale Indizes) mit der Policy, die er bekommt,
  und gibt den Rollout zurück. In der Nachahmung rechnet jeder seinen Teil jeder Runde.
- **Lerner:** ein eigener Worker; Nachahmung, PPO-Update, Bewertung des Sprungs alle 25 Iterationen.
- **Seite:** Bewegung, Worker (Standard: Kerne − 1), Enten je Worker, Iterationen, Seed; Kennzahlen
  mit echten Sprüngen, Steigen und Flugzeit; das Netz wird für Kern Studio auf dem Gerät gespeichert
  (IndexedDB „kern-lehrer“), offene Studio-Tabs übernehmen es sofort.
- Gleicher Seed und gleich viele Enten ergeben dasselbe Netz wie nativ (Determinismus).

### Geschwindigkeit

- **Nativ, x86, 2 Kerne:** rund 1200 Entenschritte/s beim Üben (Sammeln und Lernen zusammen). Ein
  Schritt sind 4 Weltschritte mit je 16 Box3D-Substeps, wie im Studio. Der Engpass ist der
  Gelenk-Löser von Box3D.

## 7. Determinismus: Bit für Bit, auf jeder Plattform

Eine Simulation, ein Trainingslauf oder ein abgespieltes Roboter-Log kommt überall gleich heraus:
nativ auf x86 und ARM (GCC, Clang, Apple clang) und als WebAssembly in jedem Browser. Ein Fehler
der Simulation ist damit ein fester, reproduzierbarer Fehler und kein Rauschen, das je nach Rechner
anders aussieht.

### Was dafür nötig ist

- **Keine FMA-Zusammenfassung:** Alles (Kern, Box3D, libm) wird mit `-ffp-contract=off` übersetzt.
  Sonst rundet `a*b+c` auf ARM und x86 mit `-march=native` anders als in WebAssembly.
- **Box3D im Skalar-Pfad** (`BOX3D_DISABLE_SIMD`), wie in WebAssembly. Gemessen kostet das nativ
  nichts: Der Löser der Gelenke ist der Engpass, nicht die SIMD-Kontakte.
- **Eigene libm:** `sin`, `cos`, `atan2`, `exp`, `log`, `pow` ... runden in glibc, Apples libm,
  Androids bionic und wasi-libc im letzten Bit unterschiedlich. Ein Bit wird in einer
  Robotersimulation in Sekunden zu einem anderen Gang. Kern nutzt deshalb überall die
  Implementierungen von musl (MIT, `third_party/musl_math`, umbenannt in `kern_musl_*`) über
  `kern::dm::` (`include/kern/core/dmath.h`). `sqrt`, `fabs`, `floor` sind in IEEE exakt und
  bleiben `std::`. Box3D hat eigene deterministische Winkelfunktionen.
- **Lernen unabhängig von der Zahl der Worker:**
  - Jede Ente bekommt ihren Seed aus (Seed, globaler Index), nicht aus dem Worker.
  - Der Lerner ordnet die Rollouts kanonisch (Schritt für Schritt, Ente für Ente), bevor er
    Normalisierer, GAE und Update rechnet.
  - Updates und Nachahmung teilen jede Minibatch in feste 8 Teile und summieren sie in fester
    Reihenfolge, egal wie viele Threads sie rechnen.
  - Gleicher Seed und gleiche Zahl Enten ergeben also dasselbe Netz, ob auf 1 Kern, 16 Kernen oder
    4 Web Workern.

### Prüfung

- **Fingerabdruck:** `kern_lehrer --fingerprint 400` lässt die Ente 400 Regeltakte gehen und drehen
  (Pollens Netz, Studio-Physik) und gibt je Takt einen Hash über Lage, Geschwindigkeiten und alle
  Gelenke aus. Die Referenz liegt in `tests/determinism_fingerprint.txt`.
- **CI prüft sie dreimal:** GCC auf x86 mit Sanitizern (Test
  `determinism_is_bit_identical_across_builds_and_platforms`), WebAssembly in Node
  (`tests/lehrer_wasm_test.mjs`), Apple clang auf ARM (macOS-Job `determinism`, `-mcpu=native`).
- **Worker:** `lehrer_runs_the_same_on_any_number_of_workers` lernt mit 1, 2 und 4 Workern (je 8
  Enten insgesamt) und vergleicht die Netze Byte für Byte.
- **Geschwindigkeit:** Ein Lernschritt kostet gleich viel wie vorher; musls Funktionen sind so
  schnell wie die der Plattform, und die Physik wird von den Gelenken bestimmt.

### Regel für neuen Code

Alles, was die Simulation speist, nutzt `kern::dm::sin` usw. statt `std::sin`. Keine
Gleitkomma-Reihenfolge, die von der Zahl der Threads abhängt (Summen in fester Reihenfolge). Keine
`-ffast-math`-Optionen.

## 8. Die Läufe bis hierher

- **PPO allein** (Läufe 1–9): erst Ausnutzen der Belohnung (auf dem Schwanz sitzen, Hechtsprung,
  sich abstützen; jeweils durch strengere Regeln beseitigt), dann stabiles Stehen ohne Sprung; mit
  großem Rauschen und ohne Warmstart lernte die Ente zu stehen, nicht zu springen.
- **Erstes Vorbild:** Die Ente richtete sich auf und zog die Füße an (45° Kippen) – bestand die
  lockere Messung, ist aber kein Sprung. Daraus entstand die strenge Messung.
- **Vorbild mit strenger Bewertung, Nachahmen, Üben** (Lauf 15): das Netz in `kern/policies/`.
