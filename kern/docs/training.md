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

## 1. Die Umgebung (`src/train`)

- **`DuckEnv`:** eine Ente auf ebenem Boden in einer eigenen GameWorld.
  - Der Renderer bleibt headless (`Renderer::set_headless`): Meshes bleiben auf der CPU, GL wird nie
    angefasst.
  - `reset()`, dann im Takt `observation()` / `step(action)`.
  - Ein `step` ist ein Regeltakt: 20 ms Roboterzeit, 4 feste Schritte der Welt.
- **Externe Steuerung:** `set_walker_external` und `set_walker_action`.
  - Der Regler nimmt die Aktion des Trainers statt eines Netzes.
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

Die Gewichte sind ein erster Entwurf. Sie werden mit dem Training abgestimmt (Schritt 7).

## 2. Netze (`src/train/nn.cpp`)

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

## 3. PPO (`src/train/ppo.cpp`)

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

## 4. Export (`src/train/onnx_export.cpp`)

- **Format:** Der trainierte Actor wird als ONNX geschrieben, aufgebaut wie Pollens exportierte
  Netze:
  - Eingang `obs` [1, 61], Ausgang `actions` [1, 14], Opset 17.
  - Sub(Mittelwert), Div(σ + 0,01), dann (Gemm mit transB, Elu)* und Gemm.
- **Metadaten:** dieselben Schlüssel wie bei Pollen (Gelenknamen, Grundstellung, Layout der
  Beobachtung, `action_scale`). Dazu kommen `task` und `trainer`.
- **Ohne Abhängigkeit:** Der Protobuf-Schreiber ist ein kleines Stück C++.
- **Prüfung:**
  - `train_exports_onnx_like_pollens_networks`: Kern liest die Datei (`parse_onnx_policy`), und sie
    rechnet bis 1e-4 genau dasselbe wie der Actor im Trainer.
  - Eine exportierte Datei lädt und rechnet auch mit ONNX Runtime, der Laufzeit des Roboters (lokal
    geprüft).

## 5. Variation und natives Training (`tools/kern_train.cpp`)

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
kern/tools/train.sh                    # baut kern/build/kern_train (nativ, -O3, alle Kerne)
kern/build/kern_train --task jump --envs 64 --iterations 1500 --out jump.onnx --log jump.csv
kern/build/kern_train --eval jump.onnx --task jump   # sauberer Roboter: Höhe, Flugzeit, Stürze
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

## 6. Training im Browser (`kern/web/train/`, Seite `/train/`)

### Aufbau

- **Build:** `kern/build.sh --train` baut den Trainer als WebAssembly-Modul (`kern_train.wasm`):
  Engine, Box3D, Spiel-Laufzeit, `src/train` und die C-Schnittstelle (`src/train/train_api.cpp`).
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

- **CI:** `kern/tests/train_wasm_test.mjs` führt eine PPO-Iteration durch die C-Schnittstelle der
  Worker aus und prüft den ONNX-Export.
- **Lokal (Playwright):** 2 Worker, 3 Iterationen; die heruntergeladene Datei lädt in ONNX Runtime.

### Hinweis

Die Rollouts fertigt der Sammler mit festen Normalisierern an; der Lerner aktualisiert sie aus allen
Beobachtungen vor dem Update (rsl_rl tut es Schritt für Schritt). Mit der Neubewertung der alten
Policy (Schritt 5) ist das gleichwertig.
