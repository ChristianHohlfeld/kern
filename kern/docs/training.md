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
| 2. MLP mit Backprop und Adam | offen |
| 3. PPO (Rezept wie rsl_rl: GAE, Clip, adaptive Lernrate, asymmetrischer Critic) | offen |
| 4. ONNX-Export im Format von Pollens Netzen | offen |
| 5. Variation (Domain Randomization), Training parallel auf allen Kernen | offen |
| 6. Training im Browser (Web Worker, Desktop und Handy) | offen |
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
| airborne | 5 | beide Sohlen in der Luft |
| height | 10 | in der Luft: Rumpf über Standhöhe (0,115 m), je 5 cm 1, höchstens 2 |
| lift | 4 | Steiggeschwindigkeit des Rumpfs (0–1 m/s) |
| upright | 2 | aufrecht (exp(−g_xy²/0,05), Pollens Rezept) |
| pose_legs / pose_neck | 1 / 1 | Beine / Hals nahe der Grundstellung (std 0,5 / 0,3) |
| action_rate | −0,1 | Änderung der Aktion |
| body_ang_vel | −0,05 | Drehrate des Rumpfs um x und y |
| self_collisions | −1 | eigene Teile berühren sich |
| fall | −4 (einmalig) | über 60° gekippt oder Rumpf unter 5 cm: Episode endet |

Die Gewichte sind ein erster Entwurf. Sie werden mit dem Training abgestimmt (Schritt 7).
