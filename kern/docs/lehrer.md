# Training: eigene Netze für den Micro Duck

Kern trainiert Policies für den Micro Duck selbst. Alles läuft in C++ auf Kerns eigener Engine und
Box3D: Simulation, Lernverfahren (PPO), Export. Es gibt keine Python- oder PyTorch-Abhängigkeit.
Das Training läuft beim Nutzer: nativ, im Browser auf Desktop und Handy und später direkt im Studio.

Die Ente im Training ist dieselbe wie im Studio: dieselbe GameWorld, dieselben Servos (Box3D-Feder,
Haltemoment, Reibung, Totzeit), dieselben Sensoren und derselbe 50-Hz-Regler. Ein Netz, das hier
lernt, läuft deshalb unverändert im Studio. Es wird im Format von Pollens Netzen exportiert (61
Beobachtungen, 14 Aktionen), kann also wie die anderen Netze auch auf den Roboter.

Die Aufgaben sind wie Pollens Trainingsrezepte aufgebaut (microduck_rl, Apache-2.0): eine
Aufgaben-Belohnung, Aufrecht- und Grundstellungs-Terme, dazu die Sim2Real-Regularisierer. Belohnungen
werden wie dort als Gewicht × Term × dt summiert.

## Stand

| Schritt | Stand |
|---|---|
| 1. Headless-Simulation: Ente ohne GPU, extern gesteuert, Episoden, Sprung-Aufgabe | fertig |
| 2. MLP mit Backprop und Adam | fertig |
| 3. PPO (Rezept wie rsl_rl: GAE, Clip, adaptive Lernrate, asymmetrischer Critic) | fertig |
| 4. ONNX-Export im Format von Pollens Netzen | fertig |
| 5. Variation (Domain Randomization), Training parallel auf allen Kernen | fertig (Abstimmung läuft) |
| 6. Training im Browser (Web Worker, Desktop und Handy) | fertig |
| 7. Das Sprung-Netz trainieren und als Fertigkeit einbinden | offen |

## 1. Die Umgebung (`src/lehrer`)

- **`DuckEnv`:** eine Ente auf ebenem Boden in einer eigenen GameWorld.
  - Der Renderer bleibt headless (`Renderer::set_headless`): Meshes bleiben auf der CPU, GL wird nie
    angefasst.
  - `reset()`, dann im Takt `observation()` / `step(action)`.
  - Ein `step` ist ein Regeltakt: 20 ms Roboterzeit, 4 feste Schritte der Welt.
- **Externe Steuerung:** `set_walker_external` und `set_walker_action`.
  - Der Regler nimmt die Aktion des Lehrers statt eines Netzes.
  - Die Servos bekommen sie wie trainiert: Grundstellung + Aktion, Skalierung 1, kein Tiefpass, mit
    der Totzeit.
- **Beobachtung:** `walker_observation` liefert zwischen zwei Takten genau, was der Regler beim
  nächsten Takt sieht.
  - Test `micro_duck_observation_between_ticks_is_what_its_controller_sees`: 100 Takte gehend,
    Abweichung 0.
  - Die Befehlsplätze 48–60 schreibt die Aufgabe (einheitliches Layout, wie Pollens Fertigkeiten).
- **Messwerte für Belohnung und Critic:** `walker_probe` liefert sie in Robotereinheiten (nicht in
  der 4-fachen Weltgröße):
  - Rumpf-Lage und -Geschwindigkeit
  - Sohlen am Boden (Box3D-Kontakte, `World::touching`)
  - Fußhöhe
  - Eigenkontakt
- **Prüfung:** Pollens velstand-Netz hält die Ente über diesen Weg 4 s stehend. Der externe Weg
  steuert die Servos also genau wie die Netze.
  - Gleicher Seed, gleiche Episode, Bit für Bit.
  - Test `train_duck_env_runs_episodes_headless`.
- **Geschwindigkeit:** 593 Regeltakte/s im Sanitizer-Build auf einem Kern (12-fache Echtzeit). Der
  optimierte Build ist schneller.

### Aufgaben

- **stand:** stehen bleiben. Das ist der Rauchtest der Pipeline.
- **jump (Kerns eigene Fertigkeit):** aus dem Stand mit beiden Füßen abheben, möglichst hoch, landen
  und wieder stehen. Pollen hat kein Sprung-Netz; dies ist das erste eigene.
  - Start wie bei Pollens Kick: stehend in Grundstellung, Gier zufällig, ±3° Neigung, Sohlenreibung
    0,7–1,3.
  - Episode 3 s.
  - Der Actor sieht nur die 61 Beobachtungen des Roboters.
  - Der Critic sieht zusätzlich Rumpfhöhe, Geschwindigkeit, Sohlenkontakte und Fußhöhen (8 Werte).

| Term | Gewicht | Was |
|---|---|---|
| airborne | 5 × aufrecht | in der Luft: weder Sohlen noch ein anderes Teil berühren den Boden |
| height | 10 × aufrecht | in der Luft: Rumpf über Standhöhe (0,115 m), je 5 cm 1, höchstens 2 |
| lift | 4 × aufrecht | Steiggeschwindigkeit des Rumpfs (0–1 m/s) |
| landed | 4 × aufrecht | nach einem Sprung wieder auf beiden Sohlen |
| upright | 2 | aufrecht (exp(−g_xy²/0,05), Pollens Rezept) |
| pose_legs / pose_neck | 1 / 1 | Beine / Hals nahe der Grundstellung (std 0,5 / 0,3) |
| action_rate | −0,1 → −1,0 | Änderung der Aktion (steigt wie bei Pollen bis Iteration 1500) |
| body_ang_vel | −0,05 | Drehrate des Rumpfs um x und y |
| self_collisions | −1 | eigene Teile berühren sich |
| fall | −20 (einmalig) | über 60° gekippt, Rumpf unter 5 cm oder ein anderes Teil als die Sohlen am Boden: Episode endet |

Der Critic sieht zusätzlich, ob die Ente in dieser Episode schon gesprungen ist (9. Wert).

Was die ersten Läufe zeigten (je einige Hunderttausend Schritte auf 2 Kernen):

- **Erster Lauf:** Die Ente lernte, sich auf den Schwanz zu setzen und die Füße zu heben. Das zählte
  als „in der Luft“. Seitdem heißt in der Luft: kein Teil berührt den Boden. Andere Teile als die
  Sohlen auf dem Boden kosten.
- **Zweiter Lauf:** Die Ente sprang 91 mm hoch, fiel aber jedes Mal (Hechtsprung). Seitdem zählen
  die Sprungterme nur, soweit sie aufrecht bleibt. Eine Landung auf beiden Sohlen zahlt, und ein
  Sturz kostet −20 statt −4.
- **Dritter und vierter Lauf** (Startrauschen 1,0 bzw. 0,4): Die Ente fiel nicht mehr, stützte sich
  aber auf ein anderes Teil. Seitdem beendet jeder Bodenkontakt außer den Sohlen die Episode wie ein
  Sturz.
- **Fünfter Lauf** (Startrauschen 0,4, 800 Iterationen, rund 900.000 Schritte auf 2 Kernen): Die
  Ente steht in allen 10 Prüf-Episoden stabil, auch gegen Stöße, nur auf den Sohlen. Sie springt
  aber noch nicht (Rumpf höchstens +3 mm): Stehen ist das sichere Optimum, solange ein Sprung beim
  Erkunden meist mit −20 endet.

Nächste Schritte für den Sprung:

1. **Längere Läufe auf mehr Kernen.** Pollens Netze sehen Hunderte Millionen Schritte; hier waren es
   unter einer Million.
2. **Lehrplan (Curriculum):** Der Sturz kostet anfangs wenig und erst später −20. So lohnt sich das
   Erkunden des Absprungs.
3. **Ein Phasen-Signal wie Pollens Ground-Pick:** Ein „jetzt springen“-Befehl in den Befehlsplätzen
   gibt der Policy einen Zeitpunkt. Das Netz bliebe dann mit dem 61er-Layout kompatibel.

Die Gewichte sind ein erster Entwurf. Sie werden mit dem Training abgestimmt (Schritt 7).

## 2. Netze (`src/lehrer/nn.cpp`)

- **`Mlp`:** Schichten mit ELU dazwischen und linearer Ausgabe, wie rsl_rls Actor und Critic
  (512/256/128).
  - Gewichte liegen eingangsweise (`w[i * out + o]`). Vorwärts- und Rückwärtsrechnung sind damit
    zusammenhängende Schleifen, die der Compiler vektorisiert.
  - Initialisierung wie PyTorchs `nn.Linear`.
  - Test: Der Gradient stimmt mit finiten Differenzen überein, für Parameter und Eingaben.
- **`Adam`:** mit PyTorchs Standardwerten; der Gradient wird vorher auf die Gesamtnorm 1 begrenzt
  (rsl_rl `max_grad_norm`).
- **`Normalizer`:** wie rsl_rls EmpiricalNormalization: laufender Mittelwert und laufende Varianz,
  y = (x − Mittel) / (σ + 0,01). Der Export schreibt das als Sub und Div ins ONNX, wie in Pollens
  Netzen.
- **Test:** Adam lernt sin(x) (mittlerer quadratischer Fehler < 0,01). Der Normalisierer stimmt mit
  Mittelwert und Varianz des ganzen Datensatzes überein.

## 3. PPO (`src/lehrer/ppo.cpp`)

PPO folgt rsl_rls Implementierung, mit der Pollen seine Netze trainiert:

- **Actor:** Gauß-Verteilung um die Netzausgabe; die Standardabweichung ist ein lernbarer Parameter
  je Aktion (`std_type: scalar`, Start 1,0).
- **Critic:** sieht zusätzlich die privilegierten Werte der Aufgabe (asymmetrisch).
- **Normalisierer:** je ein laufender Normalisierer für Actor und Critic, aktualisiert nach jedem
  Schritt mit den neuen Beobachtungen.
- **Rollout:** 24 Schritte je Umgebung. Bei Zeitablauf wird die Belohnung mit γ·V gestützt
  (Bootstrapping).
- **Vorteile:** GAE (γ 0,99, λ 0,95), über den ganzen Rollout normiert.
- **Update:** 5 Epochen × 4 Minibatches.
  - Verlust: geclippter Surrogate (0,2) + Wertverlust geclippt (Faktor 1) − 0,01 · Entropie.
  - Adaptive Lernrate über die KL-Divergenz (Ziel 0,01, Faktor 1,5, Grenzen 1e-5 bis 1e-2).
  - Gradient je Netz auf Norm 1 begrenzt, dann Adam.
- **Parallel:** Die Umgebungen laufen über `thread_pool` auf allen Kernen. In WebAssembly läuft das
  der Reihe nach; dort teilen Web Worker die Arbeit auf (Schritt 6).
- **Test `train_ppo_learns_to_reach`:** Ein Punkt lernt, zu einem Ziel zu fahren. Die Rückgabe
  verbessert sich von −5,7 auf −0,8 in 60 Iterationen (9 s im Sanitizer-Build).

## 4. Export (`src/lehrer/onnx_export.cpp`)

- **Format:** Der trainierte Actor wird als ONNX geschrieben, aufgebaut wie Pollens exportierte
  Netze:
  - Eingang `obs` [1, 61], Ausgang `actions` [1, 14], Opset 17.
  - Sub(Mittelwert), Div(σ + 0,01), dann (Gemm mit transB, Elu)* und Gemm.
- **Metadaten:** dieselben Schlüssel wie bei Pollen (Gelenknamen, Grundstellung, Layout der
  Beobachtung, `action_scale`). Dazu kommen `task` und `lehrer`.
- **Ohne Abhängigkeit:** Der Protobuf-Schreiber ist ein kleines Stück C++.
- **Prüfung:**
  - `train_exports_onnx_like_pollens_networks`: Kern liest die Datei (`parse_onnx_policy`), und sie
    rechnet bis 1e-4 genau dasselbe wie der Actor im Lehrer.
  - Eine exportierte Datei lädt und rechnet auch mit ONNX Runtime, der Laufzeit des Roboters (lokal
    geprüft).

## 5. Variation und natives Training (`tools/kern_lehrer.cpp`)

### Variation je Episode

Die Werte sind auf Pollens Velocity- und Kick-Rezepte abgestimmt (`Task::Noise`). Der Critic sieht
den sauberen Zustand.

- **Start:** Gier zufällig, ±3° Neigung, 0–5 mm Fallhöhe.
- **Sohlenreibung:** Faktor 0,7–1,3.
- **Was der Actor sieht:**
  - Gyro ±0,03, Schwerkraft ±0,01
  - Gelenkwinkel ±0,001 und je Episode ein Encoder-Bias von ±0,015 je Gelenk
  - Gelenkgeschwindigkeit ±0,25
- **Stöße:** alle 3–6 s ±0,3 m/s in x und y, auf jedes Teil der Ente (`walker_push`).
  - Die Stöße setzen wie bei Pollen erst ein, wenn die Fertigkeit entsteht: 0 bis Iteration 500,
    27 % bis 1000, dann voll.
- **Action-Rate-Gewicht:** steigt wie bei Pollen von −0,1 auf −1,0 bis Iteration 1500.
- **Noch nicht nachgebildet:** Schwerpunkt-, Massen- und Armature-Variation, Gelenkreibung,
  IMU-Verzögerung. Folgt, wenn das Netz damit auf dem Roboter hakt.

### Lauf

```
kern/tools/lehrer.sh                    # baut kern/build/kern_lehrer (nativ, -O3, alle Kerne)
kern/build/kern_lehrer --task jump --envs 64 --iterations 1500 --out jump.onnx --log jump.csv
kern/build/kern_lehrer --eval jump.onnx --task jump   # sauberer Roboter: Höhe, Flugzeit, Stürze
```

- **Fortschritt:** Jede Iteration schreibt eine Zeile (CSV mit `--log`).
- **Export:** alle 50 Iterationen eine ONNX-Datei.

### Geschwindigkeit

- **Physik:** etwa 750 Regeltakte/s je Kern; das ist der Engpass. Ein Takt sind 4 Weltschritte mit
  je 16 Box3D-Substeps, wie im Studio.
- **Lernen:** 512/256/128-Netze mit 17–22 GFLOP/s auf einem Kern.
- **Ein Detail gegenüber rsl_rl:** Vor dem Update werden Mittelwerte, Log-Wahrscheinlichkeiten und
  Werte der alten Policy mit den aktuellen Normalisierern neu berechnet. rsl_rl verlässt sich darauf,
  dass bei 4096 Umgebungen die Normalisierer im Rollout kaum wandern; bei einigen Dutzend
  Umgebungen verfälscht das sonst KL und Ratio.

## 6. Training im Browser (`kern/web/lehrer/`, Seite `/lehrer/`)

### Aufbau

- **Build:** `kern/build.sh --lehrer` baut den Lehrer als WebAssembly-Modul (`kern_lehrer.wasm`):
  Engine, Box3D, Spiel-Laufzeit, `src/lehrer` und die C-Schnittstelle (`src/lehrer/lehrer_api.cpp`).
- **Sammler:** Jeder Web Worker lädt das Modul einmal. Er führt seine eigenen Enten mit der Policy,
  die er bekommt, und gibt den Rollout als Bytes zurück.
- **Lerner:** ein eigener Worker. Er sammelt die Rollouts aller Sammler ein, macht das PPO-Update und
  gibt die nächste Policy aus (`Collector` / `Ppo::learn`).
- **Seite:**
  - Einstellungen: Bewegung, Zahl der Worker (Standard: Kerne − 1), Enten je Worker, Iterationen.
  - Start/Pause, Kennzahlen und Verlauf als Diagramm.
  - Alle 25 Iterationen ein ONNX-Export zum Herunterladen.
  - Eine Wake-Lock-Sperre hält Handys wach.
- **Geschwindigkeit:** WebAssembly schafft etwa 250 Regeltakte/s je Worker; nativ sind es etwa 750
  je Kern.

### Test

- **CI:** `kern/tests/lehrer_wasm_test.mjs` führt eine PPO-Iteration durch die C-Schnittstelle der
  Worker aus und prüft den ONNX-Export.
- **Lokal (Playwright):** 2 Worker, 3 Iterationen; die heruntergeladene Datei lädt in ONNX Runtime.

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
- **Training unabhängig von der Zahl der Worker:**
  - Jede Ente bekommt ihren Seed aus (Seed, globaler Index), nicht aus dem Worker.
  - Der Lerner ordnet die Rollouts kanonisch (Schritt für Schritt, Ente für Ente nach globalem
    Index), bevor er Normalisierer, GAE und Update rechnet.
  - Das Update teilt jede Minibatch in feste 8 Teile (`kLearnChunks`) und summiert sie in fester
    Reihenfolge, egal wie viele Threads sie rechnen.
  - Gleicher Seed und gleiche Zahl Enten ergeben also dasselbe Netz, ob auf 1 Kern, 16 Kernen oder
    4 Web Workern.

### Prüfung

- **Fingerabdruck:** `kern_lehrer --fingerprint 400` lässt die Ente 400 Regeltakte gehen und drehen
  (Pollens Netz, Studio-Physik) und gibt je Takt einen Hash über Lage, Geschwindigkeiten und alle
  Gelenke aus. Die Referenz liegt in `tests/determinism_fingerprint.txt`.
- **CI prüft sie dreimal:**
  - GCC auf x86 mit Sanitizern (Test `determinism_is_bit_identical_across_builds_and_platforms`),
  - WebAssembly in Node (`tests/lehrer_wasm_test.mjs`),
  - Apple clang auf ARM (macOS-Job `determinism`, mit `-mcpu=native`).
- **Worker:** `train_runs_the_same_on_any_number_of_workers` trainiert mit 1, 2 und 4 Workern
  (je 8 Enten insgesamt) und vergleicht die Netze Byte für Byte.
- **Geschwindigkeit:** Ein Trainingsschritt kostet gleich viel wie vorher (gemessen auf 16 Substeps,
  rund 1100 Regeltakte/s je Kern nativ). musls Funktionen sind so schnell wie die der Plattform, und
  die Physik wird von den Gelenken bestimmt.

### Regel für neuen Code

Alles, was die Simulation speist, nutzt `kern::dm::sin` usw. statt `std::sin`. Keine
Gleitkomma-Reihenfolge, die von der Zahl der Threads abhängt (Summen in fester Reihenfolge). Keine
`-ffast-math`-Optionen.

## Hinweis zum Browser-Lernen

Die Rollouts fertigt der Sammler mit festen Normalisierern an; der Lerner aktualisiert sie aus allen
Beobachtungen vor dem Update (rsl_rl tut es Schritt für Schritt). Mit der Neubewertung der alten
Policy (Schritt 5) ist das gleichwertig.
