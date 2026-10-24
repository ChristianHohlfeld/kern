# 14 Der Lehrer

Der Lehrer ist Kerns eigenes Trainingssystem für die Micro Duck. Simulation, Lernverfahren und
Export laufen in C++ auf Kerns Engine und Box3D, ohne Python und ohne PyTorch. Er lernt nativ auf
allen Kernen (auch headless auf einem Server) und als WebAssembly im Browser, auf Desktop und Handy.
Ein Lauf ergibt auf allen in der CI geprüften Plattformen Bit für Bit dasselbe Netz.

Die Ente im Lehrer ist dieselbe wie im Studio: dieselbe `GameWorld`, dieselben Servos, Sensoren und
derselbe 50-Hz-Regler, dieselbe Physik mit 16 Sub-Steps ([Kapitel 13](13-micro-duck.md)). Ein Netz,
das hier lernt, läuft deshalb unverändert im Studio. Es wird im Format von Pollens Netzen exportiert.
Auf dem echten Roboter ist es nicht erprobt; wer ein selbst trainiertes Netz auf Hardware lädt, tut das
auf eigenes Risiko ([Kapitel 16](16-kalibrierung.md)).

Mit dem Lehrer sind Kerns eigene Netze entstanden: `policies/jump.onnx` (der Sprung) und
`policies/play.onnx` (die Spielschicht). Dieses Kapitel erklärt, wie er gebaut ist und wie er
rechnet, so nah am Code wie möglich. Die allgemeinen Grundlagen – was ein neuronales Netz ist,
Gradientenabstieg, Backpropagation, überwachtes Lernen und Reinforcement Learning, MDP, Policy und
Wertfunktion – stehen in [Anhang A](A-grundlagen-neuronale-netze.md). Die Bedienung Schritt für
Schritt steht in [Kapitel 15](15-eigene-modelle.md).

## 14.1 Überblick

| Datei | Inhalt |
|---|---|
| `include/kern/lehrer/env.h`, `src/lehrer/duck_env.cpp` | `Rng`, `Env`, `Task`, `DuckEnv`; in `env.h` auch die Deklarationen von `JumpMeter`, `evaluate_jump` und Fingerabdruck |
| `src/lehrer/tasks.cpp` | die Aufgaben `stand` und `jump` |
| `include/kern/lehrer/nn.h`, `src/lehrer/nn.cpp` | `Mlp`, `Adam`, `Normalizer` |
| `include/kern/lehrer/ppo.h`, `src/lehrer/ppo.cpp` | `PpoConfig`, `Policy`, `Rollout`, `Collector`, `Ppo` (PPO und Nachahmung) |
| `include/kern/lehrer/backend.h`, `src/lehrer/backend.cpp` | `Backend`, `ThreadBackend`, `train_iteration` |
| `include/kern/lehrer/recipe.h`, `src/lehrer/recipe.cpp` | `Recipe`: Aufgabe, PPO-Einstellungen, Warmstart, Lehrpläne |
| `include/kern/lehrer/vorbild.h`, `src/lehrer/vorbild.cpp` | Sprung-Vorbild, Vorbildsuche (CMA-ES), DAgger |
| `include/kern/lehrer/cmaes.h`, `src/lehrer/cmaes.cpp` | CMA-ES |
| `include/kern/lehrer/spielen.h`, `src/lehrer/spielen.cpp` | `PlayEnv`, Vorbild der Spielschicht, Bissmessung, Bewertung, DAgger |
| `include/kern/lehrer/onnx_export.h`, `src/lehrer/onnx_export.cpp` | ONNX-Export und Metadaten |
| `include/kern/lehrer/kalibrierung.h`, `src/lehrer/kalibrierung.cpp` | Prüfstand und Abgleich ([Kapitel 16](16-kalibrierung.md)) |
| `src/lehrer/evaluate.cpp`, `fingerprint.cpp` | Sprungmessung, Determinismus-Fingerabdruck |
| `src/lehrer/lehrer_api.cpp` | C-Schnittstelle für den Browser |
| `tools/kern_lehrer.cpp`, `tools/lehrer.sh` | das Kommandozeilenwerkzeug und sein Build |
| `web/lehrer/` | die Lehrer-Seite: `index.html`, `lehrer.mjs`, `worker.mjs` |

Stand laut `docs/lehrer.md`:

| Schritt | Stand |
|---|---|
| Headless-Simulation, MLP mit Backprop und Adam, PPO wie rsl_rl, ONNX-Export | fertig |
| Variation (Domain Randomization), Lernen im Browser, Determinismus | fertig |
| Der Sprung: Vorbild → Nachahmen → Üben | fertig |
| Digitaler Zwilling: Gerätemodelle, Kalibrierung, Abgleich | fertig (ohne echte Messdaten) |
| Spielen: Ball kicken, Socke aufheben, wegtragen, ausspucken | in Überarbeitung (Abschnitt 14.8) |

## 14.2 Architektur

Der Lehrer trennt fünf Rollen, damit sie an verschiedenen Orten laufen können:

```
             Recipe ──────────► Task ──► DuckEnv / PlayEnv  (eine Ente, eine GameWorld, headless)
               │                              ▲
               │                              │ step(action) / observation()
               ▼                              │
   Ppo (Lerner) ──Policy──► Backend ──► Collector ──► Rollout
        ▲                    (Threads | Web Worker)        │
        └──────────────────── learn(rollouts) ◄────────────┘
```

- **`Task`** sagt, was gelernt wird: Startzustand, Befehlsplätze der Beobachtung, Belohnung,
  Episodenende, Lehrpläne, privilegierte Beobachtung für den Kritiker.
- **`Env`** ist die abstrakte Umgebung (`reset`, `observation`, `critic`, `step`). `DuckEnv` führt
  eine Ente mit einem `Task` durch Episoden (61 Beobachtungen → 14 Aktionen); `PlayEnv` führt die
  Spielschicht (23 → 9).
- **`Recipe`** bündelt Aufgabe, PPO-Konfiguration, Warmstart, Servoprofil, Sub-Steps, Nachahmung
  und Lehrpläne. Alle Orte, die trainieren (Werkzeug, Browser), nutzen dieselben Rezepte
  (`recipe_for`), damit derselbe Lauf überall dasselbe Netz ergibt.
- **`Collector`** führt Umgebungen mit einer festen `Policy` (Actor, Normalisierer, Rauschen) und
  liefert einen `Rollout`. **`Ppo`** ist der Lerner: Er nimmt die Rollouts aller Sammler, rechnet
  das Update und gibt die nächste `Policy` heraus. Beide serialisieren sich in flache Puffer
  (`Policy::serialize`, `Rollout::serialize`).
- **`Backend`** ist, wo gerechnet wird. `ThreadBackend` verteilt Gruppen von Umgebungen auf Threads;
  im Browser ist jeder Web Worker ein Sammler, ein weiterer Worker der Lerner.

Die Trainingsschleife ist dadurch kurz:

```cpp
IterationStats train_iteration(Ppo& ppo, Backend& backend) {
  std::vector<Rollout> rollouts;
  backend.collect(ppo.policy(), ppo.config().steps_per_env, ppo.iteration() + 1, rollouts);
  std::vector<const Rollout*> ptrs;
  for (const Rollout& r : rollouts) ptrs.push_back(&r);
  ppo.set_parallel(backend.update_parallel());
  return ppo.learn(ptrs);
}
```

`ThreadBackend(workers, envs_per_worker, seed, make, prepare)` erzeugt je Worker eine Gruppe von
Umgebungen mit globalen Indizes `w · envs_per_worker …` und einen `Collector(seed, first)`. Vor jeder
Iteration ruft es `prepare(env, iteration)` (die Lehrpläne), dann sammelt jede Gruppe auf ihrem
Thread. `update_parallel()` gibt den Thread-Pool für die Rechenarbeit des Updates zurück.

## 14.3 Die Umgebung der Ente

### `DuckEnv`

`DuckEnv(task, seed, substeps)` baut eine Ente auf ebenem Boden (Reibung 1) in einer eigenen
`GameWorld`. Der Renderer bleibt headless (`Renderer::set_headless`): Meshes bleiben auf der CPU, GL
wird nie angefasst. Die Ente ist nicht autonom; `set_walker_external(duck, true)` sorgt dafür, dass
der Regler die Aktion des Lehrers statt eines Netzes nimmt, und zwar wie trainiert: Home + Aktion,
Scale 1, kein Tiefpass, mit der Busverzögerung des Servoprofils. Läuft eine Bewegung (Skill,
Aufheben, Sitzen), hat sie Vorrang.

Ein `step(action)` ist ein Regeltakt (20 ms Roboterzeit):

```cpp
w.set_walker_action(duck_, last_action_);
do { w.step_once(); } while (!w.walker_tick_due(duck_) && ++guard < 64);   // 4 Weltschritte
++step_;
if (task_->randomize && step_ >= next_push_) w.walker_push(duck_, {±p, ±p, 0});
w.walker_probe(duck_, probe_);
r.reward = task_->reward(Tick{probe_, last_action_, previous_action_, step_}, r.terminal);
r.timeout = !r.terminal && step_ >= max_steps_;
observe();
```

`walker_probe` liefert, was Belohnung und Kritiker lesen, in Robotereinheiten (nicht in der
4-fachen Weltgröße): Rumpflage und -geschwindigkeit, Drehrate, Schwerkraft im Rumpf-Frame, welche
Sohle den Boden berührt, Fußhöhen, Eigenkontakt, Bodenkontakt anderer Teile, Gelenkwinkel und
-geschwindigkeiten, letzte Aktion.

`observe()` baut die Beobachtung:

1. `walker_observation` – genau das, was der Regler beim nächsten Takt sieht.
2. `task->command(obs, step)` schreibt die Befehlsplätze 48–60.
3. Der **Kritiker** bekommt diese saubere Beobachtung plus `task->privileged(probe)`.
4. `task->before_action(...)` – hier fragt eine Aufgabe mit Nachahmungsbelohnung ihr Vorbild.
5. Nur der **Actor** bekommt die Sensorfehler (Abschnitt „Variation“).

### `Task`

```cpp
class Task {
 public:
  virtual std::string name() const = 0;
  virtual i32 privileged() const { return 0; }          // Zusatzbeobachtungen nur für den Kritiker
  virtual f32 episode_seconds() const = 0;
  virtual Start reset(Rng& rng) = 0;                    // Start und Variation der Episode
  virtual void command(f32 observation[kObs], i32 step);
  virtual void privileged(const WalkerProbe& p, f32* out);
  virtual void before_action(const f32* observation, const WalkerProbe& probe, i32 step);
  virtual f32 reward(const Tick& t, bool& terminal) = 0;
  virtual void progress(i32 iteration);                 // Lehrpläne
  Noise noise;                                          // Variation
  bool randomize = true;                                // aus: der saubere Roboter
  std::vector<std::pair<const char*, f32>> terms;       // Belohnungsterme des letzten Takts
};
```

Belohnungen folgen der Konvention von mjlab: Summe aus Gewicht × Term × dt (dt = 0,02 s). Die
Hilfsklasse `Terms` in `tasks.cpp` schreibt jeden Term mit Namen in `terms`, damit `--trace` ihn
zeigen kann; einmalige Terme (der Sturz) werden nicht mit dt multipliziert.

Eingebaut sind zwei Aufgaben:

- **`stand`** (Schnelltest der Pipeline): 4 s stehen, Belohnung aus `upright` (2),
  `pose_legs` (2), `pose_neck` (1), `height` (1, Referenz 0,115 m), `action_rate` (−0,1); ein Sturz
  kostet −4 und beendet die Episode.
- **`jump`** (Abschnitt 14.6).

`make_task(name)` in `recipe.cpp` kennt genau diese beiden Namen.

### Asymmetrischer Kritiker

Actor und Kritiker sehen verschiedenes. Der **Actor** sieht nur, was die Sensoren des Roboters
liefern, mit allen Fehlern der Variation; nur er wird exportiert und muss auf dem Roboter laufen. Der
**Kritiker** existiert nur beim Lernen. Er sieht die saubere Beobachtung und dazu Größen, die kein
Sensor liefert. Für den Sprung (`privileged() = 9`):

| Platz | Inhalt |
|---|---|
| 61 | Rumpfhöhe über der Starthöhe |
| 62–64 | Rumpfgeschwindigkeit x, y, z |
| 65, 66 | linke / rechte Sohle am Boden |
| 67, 68 | linke / rechte Fußhöhe |
| 69 | ist schon geflogen |

Damit schätzt der Kritiker den Wert eines Zustands genauer, und die Vorteile, mit denen der Actor
lernt, werden weniger verrauscht.

### Variation (Domain Randomization)

Wie Pollens Velocity- und Kick-Rezepte. Alle Werte je Episode, sofern `randomize` an ist:

| Bereich | Was | Werte (`Task::Noise`, Aufgabe) |
|---|---|---|
| Start | Gier, Neigung, Sohlenreibung | Gier ±180°, Rollen/Nicken ±3°, Sohlenreibung × 0,7 … 1,3 |
| Bau (`vary_micro_duck`) | Rumpfmasse und -trägheit | × 0,95 … 1,05 |
| | Schwerpunkt Rumpf / Kopf je Achse | ±3 mm / ±3 mm; beim Sprung Lehrplan bis ±15 / ±10 mm |
| Servos (Profil) | je Servo eigene Werte innerhalb der Streuungen | XL330: Reibung und Armatur ±10 % |
| | Busverzögerung | 15 … 30 ms |
| Sensoren (nur Actor) | Gyro, Schwerkraft | ±0,03, ±0,01 (gleichverteilt) |
| | Gelenkwinkel, Encoder-Versatz je Episode | ±0,001, ±0,015 |
| | Gelenkgeschwindigkeit | ±0,25 |
| | IMU-Verzögerung | 0 … 1 Takt, alle 64 Takte neu gezogen |
| Stöße | alle 3–6 s ein Geschwindigkeitsstoß x, y | ±0,3 m/s × `push_scale` |

`Recipe::push_scale(iteration)` schaltet die Stöße ein: 0 bis Iteration 300, 0,27 bis 500, danach 1.
Wie die Servo-Streuung aus dem Profil kommt und wie man sie misst, beschreibt
[Kapitel 16](16-kalibrierung.md).

Die Variation zwingt das Netz, sich auf nichts zu verlassen, was die Simulation nicht genau weiß:
Ein Netz, das nur mit einer Rumpfmasse, einer Reibung und einer Latenz stehen kann, steht auf dem
echten Roboter nicht. Die Bewertung misst deshalb immer zweimal, auf dem sauberen Roboter und mit
voller Variation (`progress(100000)`: die Lehrpläne an ihrem Ende).

### Zufall

`Rng` ist splitmix64: klein, schnell und reproduzierbar. `uniform()` nimmt die oberen 24 Bit,
`normal()` ist Box-Muller mit `kern::dm::log` und `kern::dm::cos`. Jede Umgebung hat ihren eigenen
Strom; der Seed der Umgebung k eines Laufs mit Seed s ist `s · 1000003 + k` (`make_env`).

## 14.4 Netze: `Mlp`, `Normalizer`, `Adam`

### Das MLP

`Mlp` ist ein mehrschichtiges Perzeptron wie Actor und Kritiker von rsl_rl: ELU zwischen den
Schichten, lineare Ausgabe. `dims` sind Eingänge, verdeckte Schichten, Ausgänge, also für den
Sprung-Actor `{61, 512, 256, 128, 14}`. Alle Parameter liegen in einem Vektor `params_`; je Schicht
l die Gewichte `w[i · out + o]` (eingangsweise, „input-major“) und dahinter die Biases `b[o]`.

**Initialisierung** wie PyTorchs `nn.Linear`: Gewichte und Biases gleichverteilt in ±1/√fan_in; die
letzte Schicht kann mit `output_gain` skaliert werden (Standard 1).

**Vorwärtsrechnung** für eine Charge von `batch` Eingängen:

```
a⁰ = x
zˡ = aˡ · Wˡ + bˡ                       (Wˡ: in × out, eingangsweise gespeichert)
aˡ⁺¹ = ELU(zˡ)   für verdeckte Schichten,   ELU(z) = z für z > 0, sonst e^z − 1
y  = z^L                                 (letzte Schicht linear)
```

`dense()` rechnet `y[r][o] = b[o] + Σᵢ x[r][i] · w[i][o]`, vier Zeilen der Charge je Durchlauf über
die Gewichte. Die Speicheranordnung macht beide Richtungen zu zusammenhängenden axpy-Schleifen, die
der Compiler vektorisiert, ohne Summen umzuordnen (wichtig für den Determinismus, Abschnitt 14.7).
`expm1` kommt aus Kerns deterministischer libm (`kern::dm::expm1`). Mit einem `Cache` merkt sich die
Vorwärtsrechnung alle Aktivierungen `act[0..L]` für die Rückwärtsrechnung.

**Rückwärtsrechnung** (`backward(cache, dout, grad, dx)`), Schicht für Schicht von hinten, mit
`δ = ∂Verlust/∂z` (am Ausgang `dout`):

```
∂b[o]    += Σ_r δ[r][o]
∂W[i][o] += Σ_r a[r][i] · δ[r][o]
δ_unten  = δ · Wᵀ
δ_unten  ⊙= ELU'(z)    mit ELU'(z) = 1 für z > 0, sonst e^z = a + 1
```

Der letzte Schritt nutzt, dass die Ableitung der ELU im negativen Bereich e^z = (e^z − 1) + 1 = a + 1
ist; Kern braucht dafür nur die gespeicherte Aktivierung. Gradienten werden zu `grad` **addiert**,
damit Teilstücke einer Minibatch sich zu einem Gradienten summieren lassen. Auf Wunsch liefert
`dx` den Gradienten nach der Eingabe. Der Test `lehrer_mlp_gradient_matches_finite_differences`
vergleicht den Gradienten mit finiten Differenzen.

### Normalisierung

`Normalizer` ist rsl_rls `EmpiricalNormalization`: laufender Mittelwert und Varianz je Eingang,

```
y = (x − mean) / (√var + 0,01)
```

Aktualisiert wird mit Chans paralleler Formel für eine Charge der Größe n mit Mittel m und Varianz v
(in f64):

```
total = count + n
δ     = m − mean
mean' = mean + δ · n / total
var'  = (var · count + v · n + δ² · count · n / total) / total
```

Start ist Mittel 0, Varianz 1, Zähler 0. Der Export schreibt den Normalisierer als `Sub` (Mittel)
und `Div` (√var + 0,01) ins ONNX, genau wie in Pollens Netzen. Der Kritiker hat einen eigenen
Normalisierer über seine 61 + privilegierten Eingänge.

Ein **Warmstart** (`Ppo::warm_start`) übernimmt Gewichte und Normalisierer eines fertigen Netzes und
**friert den Normalisierer ein** (`freeze_actor_normalizer`): Die Gewichte wurden mit genau diesen
Werten trainiert. Eingänge, die das alte Netz nie benutzt hat (beim Sprung die Phasenplätze 55
und 56), bekommen Gewicht 0, Mittel 0 und Divisor 1; der Actor handelt dann zunächst genau wie das
alte Netz, bis er lernt, sie zu nutzen.

### Adam

`Adam` mit PyTorchs Standardwerten (β₁ = 0,9, β₂ = 0,999, ε = 10⁻⁸) und vorgeschaltetem Begrenzen
des Gradienten auf die globale Norm `max_norm` (rsl_rl: 1,0):

```
‖g‖ = √(Σ g²)                         über alle Blöcke zusammen (f64)
wenn ‖g‖ > max_norm:  g ← g · max_norm / (‖g‖ + 10⁻⁶)
t ← t + 1
m ← β₁ m + (1 − β₁) g
v ← β₂ v + (1 − β₂) g²
θ ← θ − (lr / (1 − β₁ᵗ)) · m / (√v / √(1 − β₂ᵗ) + ε)
```

Mehrere Parameterblöcke (beim Actor: Netz und Standardabweichung) teilen sich eine Begrenzung
(`step({Block{...}, Block{...}})`). Die Momente und der Schrittzähler gehören in den Checkpoint.

## 14.5 PPO Schritt für Schritt (`ppo.cpp`)

Kern folgt dem PPO von rsl_rl, mit dem Pollen die Netze der Ente trainiert: Gauß-Actor mit lernbarer
Standardabweichung, asymmetrischer Kritiker, GAE, geclippter Surrogat- und Wertverlust, Entropie,
adaptive Lernrate über die KL-Divergenz, Begrenzung des Gradienten je Netz, Adam.

### Konfiguration

| `PpoConfig` | Standard | Sprung (`recipe_for("jump")`) | Spielschicht (`make_play_learner`) |
|---|---|---|---|
| `actor_hidden` | 512, 256, 128 | 512, 256, 128 (wie velstand) | 256, 128 |
| `critic_hidden` | 512, 256, 128 | 256, 128 (alle Rezepte) | 256, 128 |
| `init_std` | 1,0 | 0,3 | 0,2 |
| `steps_per_env` | 24 | 48 | 64 |
| `epochs`, `minibatches` | 5, 4 | 5, 4 | 5, 4 |
| `clip` | 0,2 | 0,2 | 0,2 |
| `gamma`, `lam` | 0,99, 0,95 | 0,99, 0,95 | 0,99, 0,95 |
| `value_coef` | 1,0 | 1,0 | 1,0 |
| `entropy_coef` | 0,01 | 0,005 | 0 |
| `learning_rate` | 10⁻³ | 10⁻⁴ | 10⁻⁴ |
| `desired_kl`, `adaptive_lr` | 0,01, an | 0,01, an | 0,01, an |
| `max_grad_norm` | 1,0 | 1,0 | 1,0 |
| `clipped_value_loss` | an | an | an |

### 1. Sammeln (`Collector::collect`)

Jede Umgebung läuft `steps_per_env` Takte auf einem Thread, nacheinander (eine Episode ist
sequenziell). In jedem Takt:

```
x  = normalize(o)
μ  = actor(x)
a  = μ + σ ⊙ ε,    ε ~ N(0, 1) aus dem eigenen Strom der Umgebung
r, terminal, timeout = env.step(a)
```

Gespeichert werden Beobachtung, Kritiker-Beobachtung, Aktion, Belohnung, `terminal`, `timeout`; am
Ende die Kritiker-Beobachtung, wo jede Umgebung steht (`last_critic`), für das Bootstrapping. Endet
eine Episode, wird die Umgebung zurückgesetzt und ihre Rückgabe und Länge notiert. Die Zeilen liegen
`[Schritt][Umgebung]`.

Der Rauschstrom der Umgebung k hängt nur von (Seed, globalem Index) ab, nie davon, welcher Worker sie
führt.

### 2. Kanonisch ordnen und Normalisierer aktualisieren

`learn(rollouts)` legt alle Zeilen aller Rollouts in **einer** festen Reihenfolge ab: Schritt für
Schritt, darin die Rollouts in der Reihenfolge ihrer globalen Indizes, darin Umgebung für Umgebung.
Das Ergebnis hängt damit nicht davon ab, wie die Umgebungen auf Worker verteilt waren.

Dann lernen die Normalisierer aus allen beobachteten Zeilen auf einmal (rsl_rl aktualisiert sie
Schritt für Schritt beim Sammeln; Kerns Sammler halten die Policy für einen Rollout fest, deshalb
geschieht es hier). Ein eingefrorener Actor-Normalisierer bleibt, der des Kritikers lernt immer.

### 3. Alte Policy neu bewerten

Mit den **aktuellen** Normalisierern rechnet der Lerner für jede Zeile den Mittelwert μ_alt, den Wert
V(s) und die Log-Wahrscheinlichkeit der gesammelten Aktion:

```
log π(a|s) = Σ_j [ −(a_j − μ_j)² / (2 σ_j²) − ln σ_j − ½ ln 2π ]
```

Der Grund steht in `docs/lehrer.md`: Bei einigen Dutzend Umgebungen wandern die Normalisierer
zwischen zwei Iterationen merklich; mit den Werten vom Sammelzeitpunkt wäre das Verhältnis neuer zu
alter Policy verfälscht.

### 4. Vorteil mit GAE

Je Umgebung rückwärts durch die Zeit, mit `live = 0` nach einem Ende (terminal oder Zeitablauf):

```
r'_t = r_t + γ · V(s_t)          wenn Schritt t ein Zeitablauf war (Bootstrapping wie rsl_rl)
δ_t  = r'_t + live_t · γ · V(s_{t+1}) − V(s_t)
A_t  = δ_t + live_t · γ · λ · A_{t+1}
R_t  = A_t + V(s_t)              (Ziel des Kritikers)
```

V(s_T) am Ende des Rollouts ist der Wert von `last_critic`. Ein Zeitablauf ist kein Scheitern:
Die Episode hätte weitergehen können, also wird ihr Wert geschätzt statt auf 0 gesetzt. Ein
`terminal` (Sturz) dagegen hat keinen Wert danach.

Danach werden die Vorteile über die ganze Charge normalisiert: `(A − Mittel) / (Std + 10⁻⁸)`, mit
der Stichproben-Standardabweichung (n − 1).

### 5. Update: Epochen und Minibatches

Eine Permutation aller Zeilen gilt für alle Epochen (wie rsl_rls `mini_batch_generator`).
Minibatch-Größe = Zeilen / `minibatches` (ein Rest fällt weg). Für jede der `epochs × minibatches`
Minibatches:

**a) Vorwärts** für Actor und Kritiker, aufgeteilt in feste 8 Teilstücke (`kLearnChunks`), die
auf den Threads laufen.

**b) Adaptive Lernrate.** Vor dem Gradientenschritt misst der Lerner die KL-Divergenz zwischen der
Policy beim Sammeln (μ_alt, σ_alt = σ zu Beginn des Updates) und der aktuellen (μ, σ), gemittelt über
die Minibatch:

```
KL = mean_r Σ_j [ ln(σ_j / σ_alt,j) + (σ_alt,j² + (μ_alt − μ)²) / (2 σ_j²) − ½ ]
KL > 2 · desired_kl  →  lr ← max(10⁻⁵, lr / 1,5)
KL < desired_kl / 2  →  lr ← min(10⁻², lr · 1,5)
```

Beide Adam-Optimierer bekommen die neue Rate. So bleibt jeder Schritt in einer Vertrauensregion: Ist
die Policy schon weit gewandert, wird der nächste Schritt kleiner.

**c) Surrogatverlust** (geclippt), je Zeile:

```
ρ      = exp(clamp(log π_neu − log π_alt, −20, 20))
L_surr = max(−A · ρ,  −A · clamp(ρ, 1 − ε, 1 + ε))       ε = clip = 0,2
```

Der Gradient fließt nur, wenn der ungeclippte Zweig der größere ist; sonst ist er 0. Mit
`g = ∂L/∂log π = −A · ρ / n_mb`:

```
∂L/∂μ_j = g · (a_j − μ_j) / σ_j²
∂L/∂σ_j += g · ((a_j − μ_j)² / σ_j³ − 1 / σ_j)
```

Die Standardabweichung σ ist ein direkt gelernter Parameter je Aktion (nicht ihr Logarithmus) und
wird nach jedem Schritt auf mindestens 10⁻³ gehalten.

**d) Wertverlust** (geclippt um den Wert beim Sammeln V₀):

```
V_c = V₀ + clamp(V − V₀, −ε, ε)
L_V = max((V − R)², (V_c − R)²)
```

Ist der ungeclippte Term der größere, ist der Gradient `value_coef · 2 (V − R) / n_mb`; sonst
`value_coef · 2 (V_c − R) / n_mb`, aber nur solange |V − V₀| < ε.

**e) Entropie.** Für einen Gauß-Actor ist die Entropie unabhängig vom Zustand:

```
H = Σ_j (½ + ½ ln 2π + ln σ_j)
```

Der Verlust ist −`entropy_coef` · H; sein Gradient −`entropy_coef` / σ_j kommt auf σ. Ein kleiner
Bonus hält das Rauschen davon ab, zu früh zu verschwinden.

**f) Rückwärts und Schritt.** Jedes Teilstück rechnet `actor.backward` und `critic.backward`; die
Gradienten der 8 Teilstücke werden in fester Reihenfolge summiert. Dann macht `actor_adam_` einen
Schritt über Actor und σ zusammen (gemeinsam auf Norm 1 begrenzt), `critic_adam_` einen über den
Kritiker (eigene Begrenzung).

### Kritiker zuerst

`set_actor_frozen(true)` lässt nur den Kritiker lernen; die adaptive Lernrate ruht dann. Nach der
Nachahmung lernt in den ersten `critic_warmup` Iterationen (Rezept: 30; Übungsphase der Spielschicht:
20) nur der Kritiker. Sonst steuern Vorteile eines Kritikers, der die Erträge der neuen Policy noch
nicht kennt, einen Actor, der schon gut handelt – und zerstören ihn.

### Statistik und Checkpoint

`IterationStats` enthält mittlere Belohnung je Schritt, mittlere Rückgabe und Länge der in der
Iteration beendeten Episoden, Wert- und Surrogatverlust, Entropie, Lernrate, mittleres σ, Sammel-
und Lernzeit und die Gesamtzahl der Entenschritte. `Ppo::save()` schreibt den ganzen Lerner
(Magic `KERNPPO1`: Iteration, Schritte, Lernrate, Normalisierer-Status, Policy, Kritiker, beide
Normalisierer, beide Adam-Zustände); `load()` liest ihn zurück.

## 14.6 Nachahmen, Vorbilder und CMA-ES

### Warum PPO allein den Sprung nicht fand

Die ersten Läufe (PPO, mit Lehrplan für den Sturz-Malus, Phasensignal, Warmstart von velstand,
Referenzhöhe in der Belohnung, verschiedenen Netzgrößen und Lernraten) fanden den Absprung nicht.
`docs/lehrer.md` nennt drei Gründe:

- Der Absprung braucht alle Beinservos gleichzeitig am Anschlag. Die Servos sind weich (Höchstmoment
  erst bei 1,7 rad Abweichung): Das Netz muss Ziele weit hinter den Gelenkgrenzen setzen. Zufälliges
  Erkunden um eine stehende Policy erzeugt das praktisch nie.
- Stehen ist sicher und bringt Belohnung; ein missglückter Versuch kostet den Sturz.
- Mit wenigen Dutzend Enten sind die Minibatches klein und der Gradient verrauscht; die KL-Grenze
  hält die Lernrate unten.

Daher lehrt der Lehrer in drei Schritten: **Vorbild suchen → Nachahmen → Üben**. Zur Laufzeit gibt
es nur das Netz; das Vorbild existiert nur beim Lernen.

### Das Sprung-Vorbild

`JumpDemo` beschreibt einen Sprung mit 15 Zahlen: die Servoziele von Hüfte (Nicken), Knie und Knöchel
des linken Beins an fünf Zeitpunkten (Hocke, Abdruck, Anziehen, Landen, Abfedern); das rechte Bein
spiegelt (−v, weil seine Achsen gespiegelt sind).

```cpp
constexpr f32 kTimes[7] = {0.15f, 0.25f, 0.29f, 0.34f, 0.40f, 0.47f, 0.60f};  // Phase; außen 0
// Ziel = amplitude · tanh(lineare Interpolation der Schlüssel), amplitude = 2,5 rad
```

`tanh` mit Amplitude 2,5 rad erlaubt Ziele weit hinter den Gelenkgrenzen: Der Servo drückt dann mit
seinem Höchstmoment. `JumpTeacher::act` lässt vor Phase 0,15 und ab Phase 0,5 Pollens velstand die
Balance halten (mit genullten Phasenplätzen, die velstand nie gesehen hat) und setzt dazwischen die
Ziele des Vorbilds.

### CMA-ES (`cmaes.cpp`)

CMA-ES (Hansens Covariance Matrix Adaptation Evolution Strategy) optimiert ohne Gradienten. Der
Lehrer nutzt es, wo es keine Ableitung gibt: Die Simulation ist die Funktion, ihre Parameter sind
die Eingabe – das Sprung-Vorbild und die Servo-Kalibrierung.

`cmaes_minimise(start, sigma, generations, seed, evaluate_all, population, tolerance, progress)`
folgt Hansens Tutorial (2016):

```
λ = population (Standard 4 + ⌊3 ln n⌋),  μ = λ/2
wᵢ ∝ ln(μ + ½) − ln i, normiert;  μ_eff = 1 / Σ wᵢ²
c_σ = (μ_eff + 2) / (n + μ_eff + 5)
d_σ = 1 + 2 max(0, √((μ_eff − 1)/(n + 1)) − 1) + c_σ
c_c = (4 + μ_eff/n) / (n + 4 + 2 μ_eff/n)
c₁  = 2 / ((n + 1,3)² + μ_eff)
c_μ = min(1 − c₁, 2 (μ_eff − 2 + 1/μ_eff) / ((n + 2)² + μ_eff))

je Generation:
  C = B D² Bᵀ                            (Eigenzerlegung nach Jacobi; n ist klein)
  x_k = m + σ · B D z_k,  z_k ~ N(0, I)   für k = 1 … λ, alle zusammen an evaluate_all
  sortieren nach Kosten; m ← m + σ Σᵢ wᵢ y_(i)
  p_σ ← (1 − c_σ) p_σ + √(c_σ(2 − c_σ) μ_eff) · B z_w
  p_c ← (1 − c_c) p_c + h_σ √(c_c(2 − c_c) μ_eff) · y_w
  C   ← (1 − c₁ − c_μ) C + c₁ (p_c p_cᵀ + (1 − h_σ) c_c(2 − c_c) C) + c_μ Σᵢ wᵢ y_(i) y_(i)ᵀ
  σ   ← σ · exp((c_σ / d_σ)(‖p_σ‖ / χ_n − 1))
```

Der Startpunkt wird zuerst bewertet; das Ergebnis ist nie schlechter. Zurück kommen der beste
bewertete Punkt (`best`) und das Zentrum der Suchverteilung (`mean`). Bei verrauschten Kosten ist das
Zentrum die bessere Antwort, weil der beste Punkt nur Glück gehabt haben kann. Alle Kandidaten einer
Generation gehen gemeinsam an `evaluate_all` und laufen dort parallel. Die Normalverteilung kommt
in doppelter Genauigkeit aus einem eigenen `Rng` mit `kern::dm`-Funktionen, deshalb ist auch eine
Suche reproduzierbar.

### Die Vorbildsuche

`search_jump_demo(generations, seed, threads, start)` sucht die 15 Schlüssel mit Population 16,
σ = 0,8 von der Grundstellung aus (0,3 von einem gegebenen Start). Jeder Kandidat springt 6 Episoden:
die erste auf dem sauberen Roboter, die anderen mit der vollen Variation. Jede Generation trifft
andere Roboter, alle Kandidaten einer Generation dieselben – ein Vorbild, das nur für wenige
funktioniert, lehrt keinen robusten Sprung. Die Bewertung (`score_jump_demo`, höher ist besser):

```
2000 · min(Steigen in der Luft, 0,02 m)
+ 250 · min(längster Flug, 0,12 s)
+ 500 · min(max(Höhe in der Luft über Standhöhe, 0), 0,03 m)
− 300 · max(0, 0,966 − kleinstes Aufrecht)          (Neigung über 15° kostet)
+ Sturz: −40 + 40 · Takte bis zum Sturz / 100        (ein früher Sturz kostet mehr)
+ 30, wenn sie am Ende auf beiden Sohlen steht
+ 20, wenn es ein echter Sprung war (JumpMeter)
```

Zurück gibt die Suche das Zentrum `mean`. Das eingebaute Vorbild (`builtin_jump_demo()`) wurde für
das XL330-Profil in drei Stufen mit zusammen 370 Generationen gefunden.

### Nachahmen mit DAgger

Verhaltensklonen allein – das Netz lernt aus Episoden, die das Vorbild fährt – hat ein bekanntes
Problem: die **Verteilungsverschiebung**. Das Netz sieht beim Lernen nur Zustände, in die das Vorbild
kommt. Macht es selbst einen kleinen Fehler, landet es in einem Zustand, den es nie gesehen hat,
macht dort einen größeren Fehler, und die Fehler wachsen über die Episode.

DAgger („Dataset Aggregation“) behebt das, indem das Netz selbst fährt und das Vorbild beschriftet:

1. **Runde 0:** Das Vorbild fährt. Jede Beobachtung wird mit seiner Aktion beschriftet.
2. **Runde k > 0:** Das Netz fährt (Mittelwert plus Rauschen 0,05 für Abdeckung). In jedem Takt
   sagt das Vorbild, was es getan hätte; diese Aktion ist die Beschriftung.
3. Nach jeder Runde lernt das Netz aus **allen** bisher gesammelten Beispielen.

So enthält der Datensatz genau die Zustände, in die das Netz selbst gerät, mit der richtigen Antwort
dafür. Voraussetzung: Das Vorbild muss in jedem dieser Zustände eine sinnvolle Antwort haben.

In Kern (`dagger_episodes`, `JumpImitation`):

- Die Episoden einer Runde laufen in der vollen Variation (`prepare_env(..., 1000)`), mit Seed
  `seed + 7919 · (Runde + 1)` und globalem Episodenindex.
- Jeder kann einen beliebigen Teil einer Runde rechnen (ein Thread, ein Web Worker); in
  Episodenfolge zusammengefügt ergeben die Teile genau die ganze Runde.
- **Rezept Sprung:** 8 Runden à 96 Episoden.

Das Anpassen ist `Ppo::imitate(observations, targets, epochs, batch, lr, seed)`: mittlerer
quadratischer Fehler zwischen Actor-Mittelwert und Beschriftung, mit eigenem, frischem Adam,
Minibatches in fester, gemischter Reihenfolge (je Epoche neu gemischt mit dem Seed), jede Minibatch
in 8 feste Teile geteilt:

```
L = 1 / (m · A) · Σ (μ − Ziel)²,     ∂L/∂μ = 2 (μ − Ziel) / (m · A)
```

Ein nicht eingefrorener Actor-Normalisierer nimmt vorher die Beobachtungen auf. Sprung: 10 Epochen,
Minibatch 256, Lernrate 10⁻³. Spielschicht: 30 Epochen, 256, 10⁻³.

### Üben

Nach der Nachahmung (`begin_practice`) wird das Aktionsrauschen auf `practice_std` gesetzt (Sprung:
0,1), der Actor wartet `critic_warmup` = 30 Iterationen auf den Kritiker, dann übt PPO mit der
Sprung-Belohnung, einer Nachahmungsbelohnung und der vollen Variation, bis der Sprung robust ist.

### Die Aufgabe `jump`

Das Kommando ist allein das Phasensignal: cos und sin der Phase t / 2 s in den Plätzen 55 und 56,
sonst 0. Kein Regler, keine Rückkopplung, kein Skript, keine IK, keine Kräfte zur Laufzeit; die
Bewegung erzeugt allein das Netz über die 14 Servoziele. Die Episode dauert 2 s.

Belohnung (alle Terme × dt, außer dem Sturz):

| Term | Gewicht | Was |
|---|---|---|
| `track_height` | 4 × aufrecht | nur ohne Vorbild: Rumpf folgt einer Referenzhöhe über die Phase |
| `airborne` | 20 × aufrecht | im Flugfenster (Phase 0,27–0,42) in der Luft und steigend |
| `lift` | 20 × aufrecht | Steiggeschwindigkeit im Absprungfenster (0,22–0,33), 0–1 m/s |
| `landed` | 6 × aufrecht | ab Phase 0,42, nach einem Flug, auf beiden Sohlen |
| `extra_hop` | −4 | ab Phase 0,5 noch einmal in der Luft |
| `imitate` | 10 (Rezept) | exp(−mittlerer quadratischer Abstand zur Vorbild-Aktion / 0,3²) |
| `upright` / `tilt` | 2 / −30 | exp(−g_xy² / 0,05); g_xy² kostet |
| `pose_legs` / `pose_neck` | 1 / 1 | Beine vor 0,15 und nach 0,55 in Grundstellung; Hals immer |
| `drift` | −2 | horizontale Geschwindigkeit² |
| `action_rate` | −0,1 → −0,5 | Lehrplan ab Iteration 200 über 600 |
| `body_ang_vel` | −0,05 | Drehrate um x und y |
| `self_collisions` | −1 | Eigenkontakt |
| `fall` | −2 → −20 (einmalig) | Lehrplan über 300 Iterationen |

„In der Luft“ heißt: keine Sohle und kein anderes Teil am Boden. Ein Sturz (Schwerkraft z über −0,5
oder Rumpf unter 5 cm) und **jeder Bodenkontakt eines anderen Teils als der Sohlen** beenden die
Episode – sonst ist es die sichere Wette, sich auf Schnabel oder Schwanz zu stützen.

### Was als echter Sprung zählt (`JumpMeter`)

- **Flug:** alle Teile mindestens 60 ms ohne Bodenkontakt.
- **Abheben:** Der Rumpf steigt in der Luft mindestens 5 mm über die Höhe beim Abheben. Nur die Füße
  anziehen ist kein Sprung.
- **Aufrecht:** nie mehr als 30° Neigung.
- **Landen:** kein Sturz und am Ende aufrecht auf beiden Sohlen.

`evaluate_jump(act, user, episodes, seed, substeps, randomized)` misst so eine Policy in
Studio-Physik und liefert `JumpReport` (echte Sprünge, Stürze, Mittel von Höhe, Steigen, Flugzeit,
Neigung, Bestwerte).

## 14.7 Determinismus

Eine Simulation, ein Trainingslauf oder ein abgespieltes Roboter-Log kommt auf allen in der CI
geprüften Plattformen (GCC/x86, Apple clang/ARM, WebAssembly) gleich heraus. Ein Fehler der
Simulation ist damit ein fester, reproduzierbarer Fehler und kein Rauschen, das je nach Rechner
anders aussieht.

### Was dafür nötig ist

- **Keine FMA-Zusammenfassung:** Kern, Box3D und libm werden mit `-ffp-contract=off` übersetzt.
  Sonst rundet `a*b+c` auf ARM und x86 mit `-march=native` anders als in WebAssembly.
- **Box3D im Skalar-Pfad** (`BOX3D_DISABLE_SIMD`), wie in WebAssembly. Nativ kostet das nichts: Der
  Gelenklöser ist der Engpass, nicht die SIMD-Kontakte.
- **Eigene libm:** `sin`, `cos`, `atan2`, `exp`, `log`, `pow` … runden in glibc, Apples libm,
  Androids bionic und wasi-libc im letzten Bit unterschiedlich. Ein Bit wird in einer
  Robotersimulation in Sekunden zu einem anderen Gang. Kern nutzt überall die Implementierungen von
  musl (`third_party/musl_math`, umbenannt in `kern_musl_*`) über `kern::dm::`
  (`include/kern/core/dmath.h`). `sqrt`, `fabs`, `floor` sind in IEEE exakt und bleiben `std::`.
- **Lernen unabhängig von der Zahl der Worker:**
  - Jede Ente bekommt ihren Seed aus (Seed, globalem Index), nicht aus dem Worker; ebenso der
    Rauschstrom des Sammlers.
  - Der Lerner ordnet die Rollouts kanonisch (Schritt für Schritt, Ente für Ente), bevor er
    Normalisierer, GAE und Update rechnet.
  - Update und Nachahmung teilen jede Minibatch in feste 8 Teile und summieren sie in fester
    Reihenfolge, egal wie viele Threads sie rechnen.

Gleicher Seed und gleiche Zahl Enten ergeben also dasselbe Netz, auf 1 Kern, 16 Kernen oder in
4 Web Workern.

### Prüfung

- **Fingerabdruck:** `kern_lehrer --fingerprint 400` lässt die saubere Ente mit velstand 400
  Regeltakte gehen (150 Takte vorwärts mit 0,15 m/s, dann Drehen mit 0,8 rad/s) und gibt je Takt
  einen FNV-1a-Hash über die exakten Bits von Rumpflage, -geschwindigkeit, Drehrate und allen
  Gelenkwinkeln und -geschwindigkeiten aus. Die Referenz liegt in `tests/determinism_fingerprint.txt`.
  Der erste abweichende Index sagt, wann zwei Builds auseinanderlaufen.
- **CI** prüft sie dreimal: GCC auf x86 mit Sanitizern (Test
  `determinism_is_bit_identical_across_builds_and_platforms`), WebAssembly in Node
  (`tests/lehrer_wasm_test.mjs`), Apple clang auf ARM (`-mcpu=native`).
- **Worker:** `lehrer_runs_the_same_on_any_number_of_workers` lernt mit 1, 2 und 4 Workern (je
  8 Enten insgesamt) und vergleicht die Netze Byte für Byte.

### Regel für neuen Code

Alles, was die Simulation speist, nutzt `kern::dm::sin` usw. statt `std::sin`. Keine
Gleitkomma-Reihenfolge, die von der Zahl der Threads abhängt (Summen in fester Reihenfolge). Keine
`-ffast-math`-Optionen. Wie Kern gebaut wird, steht in [Kapitel 2](02-bauen-und-testen.md).

## 14.8 Die Spielschicht lernen

Die Spielschicht (`play.onnx`, [Kapitel 13](13-micro-duck.md#1311-die-spielschicht-walker_playcpp))
lernt wie der Sprung: Ein Vorbild zeigt den Weg, das Netz ahmt es aus seinen eigenen Sensoren nach
(DAgger), dann übt es (PPO).

### `PlayEnv`

`PlayEnv(seed, randomize)` ist eine Ente auf einem Boden mit Oberfläche `floor` (Reibung 1), autonom
und mit Kern extras, deren Spielschicht der Lehrer von außen steuert
(`set_walker_play_external`). Unter der Spielschicht laufen Pollens Netze unverändert. Ein `step` ist
eine Entscheidung (2 Regeltakte, 40 ms); die Episode dauert höchstens 60 s Roboterzeit.

| | Größe |
|---|---|
| Beobachtung (Actor) | 23 (`walker_play_observation`, wie im Studio) |
| Kritiker | 23 + 6 privilegiert: Ball vorhanden, wahre Ballposition voraus/links; Socke vorhanden, wahre Sockenposition voraus/links |
| Aktion | 9 |
| Actor | 23 → 256 → 128 → 9 |

**Szenen** (`reset`): Die Ente startet mit zufälligem Kurs (±180°) und ±2° Neigung. Auf dem Boden
liegt mit 30 % ein Ball, mit 30 % eine Socke, mit 40 % beides – wie im Beispiel-Level dicht
beieinander:

- Entfernung: mit 20 % 0,12–0,35 m (sie startet direkt davor), sonst 0,35–1,2 m; mit 80 % innerhalb
  ±0,9 rad vor der Ente (die Kamera sieht es sofort), sonst in jeder Richtung (sie muss sich umsehen).
- Liegen Ball und Socke beide, liegt der Ball mit 40 % auf dem Weg zur Socke (30–80 % der Strecke,
  ±12 cm seitlich), mit 30 % 15–40 cm neben der Socke, sonst unabhängig; nie näher als 12 cm an der
  Socke und nie unter den Füßen.
- Ball: Radius 3,5–6 cm, Masse 15–50 g, Rollwiderstand 0,01–0,05 (Schaumstoff bis Gummi).
- Mit 30 % liegt eine zweite Socke irgendwo, mindestens 30 cm von der ersten entfernt.

**Variation** (wenn `randomize`): Sohlenreibung × 0,7–1,3, Rumpfmasse × 0,95–1,05, Schwerpunkt Rumpf
±5 mm und Kopf ±3 mm je Achse, je Servo Reibung und Armatur innerhalb der Profilstreuung,
Busverzögerung 15–30 ms, Detektor `sigma_abs` 0–4 mm, `sigma_rel` 0–3 %, Aussetzer 0–20 %.

**Belohnung** je Entscheidung:

| Term | Wert |
|---|---|
| Zeit | −0,01 |
| Fortschritt | +2 × (Abstand vorher − Abstand jetzt) zum wahren Ziel (die Socke zuerst, sonst der Ball; nicht während sie hält) |
| aufgehoben | +3 einmalig, wenn die Socke nach dem Aufheben noch im Schnabel ist |
| tragen | +0,5 · 0,04 s · min(getragene Strecke, 0,3 m) je Entscheidung |
| gekickt | +10, wenn der Ball nach einem Kick 0,25 m von seinem Start liegt |
| abgelegt | +10, wenn die Socke mindestens 0,25 m vom Aufhebeort aus dem Schnabel kommt |
| Sturz | −5 und Ende (Schwerkraft z über −0,3) |

Gekickt oder abgelegt beendet die Episode als Zeitablauf (`timeout`), nicht als `terminal`. Ein
Aufheben zählt als Versuch, wenn es startet, und als gehaltener Griff, wenn die Socke am Ende noch
im Schnabel ist.

### Das Vorbild (`PlayEnv::teacher`)

Das Vorbild sieht **dasselbe wie das Netz** – die 23 Beobachtungen, keine wahren Positionen – und hat
kein verstecktes Gedächtnis außer der Stelle, an der der linke Fuß zu Beginn stand (dazu kennt es den
Ballradius und die gemessene Bissposition). Damit kann das
Netz es genau lernen: Jede Antwort folgt aus dem, was es jetzt sieht und wie lange es steht. Es
entscheidet so:

- **Kick läuft:** nichts tun.
- **Aufheben läuft:** Schnabel offen von Phase 0,04 bis 0,30 (`kPlayBeakOpen`, `kPlayBeakClose`),
  sonst zu.
- **Hält etwas:** losgehen (ein fester erster Schritt mit voller Kraft, dann halbe), Kopf leicht
  hoch; nach 0,3 m getragen den Schnabel öffnen (ausspucken).
- **Nichts bekannt:** auf der Stelle drehen (0,8), Kopf in Grundstellung (geneigt oder ausgeschwenkt
  kippt sie beim Drehen).
- **Ziel:** das nähere von Ball und Socke (ein Ball im Weg wird zuerst weggekickt).
- **Weiter als 0,25 m:** hinschauen (der Kopf nickt mit der Nähe stärker, 0,1–0,5 rad, und dreht
  sich höchstens 0,3 rad zum Ziel – sanft, weil weit geneigt beim schnellen Gehen und Drehen die Ente
  kippt), drehen bis höchstens 0,6 rad/s auf der Stelle bzw. 0,4 rad/s im Gehen; gehen mit
  0,15–0,3 m/s, solange das Ziel weniger als 0,6 rad daneben liegt. Dreht es auf der Stelle, steht der
  Kopf in Grundstellung.
- **Nah dran** (für Ball und Socke gleich): Pollens Gang lässt sich nur anhalten, nicht in kleinen
  Schritten setzen, und ein schwingender Fuß schiebt eine Socke weg. Also geht es auf einer Linie
  langsam und gleichmäßig auf das Ziel zu (0,12–0,15 m/s; aus dem Stand ein fester Anstoß mit
  0,3 m/s, weil der Gang sonst nicht ansetzt), lenkt statt seitlich zu treten, sagt „Stopp“ – bei der
  Socke um die Nachlaufstrecke von 0,7 cm vor der Stelle, beim Ball erst 0,5 cm hinter ihr –, steht und
  startet die Bewegung, wenn es lange genug gestanden hat.
  **Stehen** heißt hier: beide Sohlen länger als 0,25 s am Boden (länger, als die Doppelstützphase
  eines Schritts dauert; Beobachtung `still_s`). Steht es an der richtigen Stelle, bleibt es stehen
  und startet die Bewegung, gleich was sein Laufbefehl einen Moment vorher tat: Ein Zucken rückwärts
  ist kein Grund zurückzuweichen.
  Liegt das Ziel nicht richtig (Socke: ±8 mm voraus, ±12 mm seitlich; Ball: ±2 cm), geht es mit
  voller Geschwindigkeit zurück und kommt neu.
- **Ziel für die Socke** ist die gemessene Bissposition (`bite_spot`, unten), für den Ball 9 cm vor
  und 4,2 cm neben dem linken Fuß (`microduck_ball_kick_env_cfg.py`), korrigiert um den Ballradius und
  gemessen vom Fuß aus, weil der Kick aus der Grundstellung trainiert ist.
- **Bewegungswunsch als Rampe:** Der Wunsch nach Kick oder Aufheben steigt über die letzte halbe
  Sekunde des Stillstehens von −1 auf +1 (bei 1,5 s Stillstand ist er voll). Eine Rampe statt eines
  einzelnen Impulses: So kann das Netz ihn aus der Stehzeit lernen.

### Was der Lehrer selbst misst

Was das Vorbild nicht von Hand weiß, misst der Lehrer in der Physik, wie sie jetzt ist:

- **Wo der Schnabel greift** (`measure_bite`, `kern_lehrer --biss-fenster`): Eine Ente geht ein
  Stück und bleibt stehen (die Haltung nach dem Gehen, nicht die beim Erscheinen). Dann wird die
  Socke als Haufen an eine Stelle eines Gitters vor sie gelegt – 11 Stellen seitlich (−2 … +3 cm),
  9 voraus (4,5 … 7,7 cm) –, das Aufheben läuft mit offenem Schnabel im Fenster [0,04, 0,30), und es
  wird geprüft, ob die Socke hochkam und ob sie nach 3 s Wegtragen noch im Schnabel ist. Die beste
  Stelle ist eine gehaltene mit den meisten gehaltenen Stellen in ihrer Umgebung (±2 seitlich,
  ±1 voraus), denn das Hingehen ist nie exakt.
- **`bite_spot()`** misst das einmal pro Prozess, bevor die Episoden es brauchen, mit dem Schnabel-
  fenster, das das Netz gezeigt bekommt. Die Gitterstellen laufen parallel auf den Threads des
  Trainings; nativ sorgt `std::call_once` für die eine Messung, WebAssembly läuft auf einem Thread und
  braucht dafür keinen Mutex. `--spielen` gibt das Ergebnis vor der ersten Runde aus („the bite,
  measured in this physics: …“). Hält nirgends etwas, nimmt es 6,5 cm vor dem Rumpf, mittig.
  Damit steht **keine Bissposition als Zahl im Code**: Ändert sich die Physik (Reibung,
  Sockenmodell, Schnabel), misst der Lehrer neu.
- **Wie weit die Ente nach „Stopp“ noch geht** (`measure_stop`, `--halt-messen`): aus 0,10, 0,12,
  0,15 und 0,20 m/s je vier Läufe, gemessen voraus und seitlich.

### DAgger für die Spielschicht

`imitate_play(learner, rounds, episodes, seed, parallel)` arbeitet wie beim Sprung: Runde 0 fährt das
Vorbild, danach das Netz (plus Rauschen 0,05), jede Beobachtung trägt die Antwort des Vorbilds, nach
jeder Runde lernt das Netz aus allem (30 Epochen). Die Episoden einer Runde sind zur Hälfte auf dem
sauberen Roboter, zur Hälfte mit voller Variation.

### Üben mit PPO (`--spielen-ueben`)

Das nachgeahmte Netz übt danach selbst: `make_play_learner`, Warmstart von der Datei (Gewichte und
eingefrorener Normalisierer, keine neuen Eingänge), Aktionsrauschen 0,1 (`--uebungs-std`), die ersten
20 Iterationen lernt nur der Kritiker. Belohnt wird nur, was wirkt (Tabelle oben): Wo der Biss
wirklich hält und wie es wirklich stehen muss, findet es selbst. Die Umgebungen sind zur Hälfte
sauber, zur Hälfte variiert.

### Ergebnisse der Spielschicht

`kern/policies/play.onnx`: 12 Runden Nachahmen mit je 32 Episoden zu 60 s, halb sauberer Roboter, halb
volle Variation; ausgewählt ist Runde 8 nach Prüfung über 32 Episoden je Zeile (`docs/lehrer.md`,
Abschnitt 8):

| | Ball weggekickt | Socke aufgehoben | aufgehoben, weggetragen, ausgespuckt | Griffe, die hielten | Stürze |
|---|---|---|---|---|---|
| Netz, sauberer Roboter | 9 von 11 (im Mittel nach 17 s) | 4 von 21 | 1 von 21 | 2 von 14 | 2 von 32 |
| Netz, volle Variation | 7 von 11 | 13 von 21 | 0 von 21 | 1 von 15 | 10 von 32 |

Der Ball ist gelernt: Das Netz geht zum näheren Objekt, stellt den linken Fuß hinter den Ball und
startet Pollens Kick; passt es nicht, geht es zurück und kommt neu, statt stehen zu bleiben. Die
Socke steuert es genauso an und beißt – aber mit der gemessenen Reibung der TPU-Polster auf
Baumwolle hält der Biss selten, und beim Hochheben rutscht die Socke oft heraus. Der Engpass ist der
Griff in dieser Physik, nicht die Entscheidung; nächste Schritte sind ein Sockenmodell, das sich als
Stoff in den Schnabel legt, und die Übungsphase mit Belohnung.

## 14.9 Ergebnisse des Sprungs

`policies/jump.onnx` entstand mit Rezept `recipe_for("jump")`, 32 Enten auf 2 Kernen, Seed 1,
3000 Iterationen Üben (aus `docs/lehrer.md`):

| | sauberer Roboter (40 Episoden) | volle Variation (40 Episoden) |
|---|---|---|
| echte Sprünge | 40 von 40 | 36 von 40 |
| Steigen in der Luft | 11,6 mm | 8,2 mm |
| Flugzeit | 120 ms | 109 ms |
| Neigung (größte, Mittel) | 8° | 9° |
| Stürze | 0 von 40 | 0 von 40 |

„Volle Variation“ ist Pollens Kick-Rezept am Ende seiner Lehrpläne. Die Ente fällt dort nicht; zählt
ein Sprung nicht als echter, ist er zu flach (unter 5 mm Steigen).

Der Weg dahin:

| Schritt | Entenschritte | Ergebnis |
|---|---|---|
| Vorbild suchen (einmalig, eingebaut) | rund 2 Mio. (18 min auf 2 Kernen) | 16 von 20 sauber, 0 Stürze |
| Nachahmen, Runde 1–7 | 43 355 | erster echter Sprung des Netzes (5 von 5) |
| Nachahmen, Runde 8 | 49 936 | 5 von 5 |
| Üben, 3000 Iterationen | 2,3 Mio. | robust (Tabelle oben) |

Weil der Lauf überall Bit für Bit gleich ist, gilt die Zahl der Entenschritte für jedes Gerät; nur
die Zeit hängt vom Gerät ab (gemessen auf derselben x86-Maschine mit 2 Kernen):

| Plattform | erster echter Sprung | Nachahmen fertig | Üben (Entenschritte/s) |
|---|---|---|---|
| nativ, 2 Threads | 106 s | 133 s | 1431 |
| nativ, 1 Thread | – | – | 763 |
| Browser (Chromium, 2 Sammler-Worker + Lerner) | 515 s | 661 s | 391 |
| Browser, alles auf 1 Kern | 539 s | 689 s | 336 |

Der Browser lernt dasselbe Netz (gleiche 49 936 Beispiele, gleicher Nachahmungsfehler 0,0108). Er ist
beim Nachahmen rund 5-mal, beim Üben knapp 4-mal langsamer: WebAssembly ohne SIMD und ohne Threads
innerhalb eines Workers.

Die Physik schafft rund 1300 Entenschritte/s je Kern nativ (nur Sammeln); ein Schritt sind 4
Weltschritte mit je 16 Box3D-Sub-Steps. Der Engpass ist Box3Ds Gelenklöser. 8 Sub-Steps wären
1,6-mal schneller, verändern aber Gang und Bewegungen messbar und bleiben deshalb aus.

Im Studio: 3 von 3 echte Sprünge, Steigen 9,0 mm, Flug 120 ms, Neigung 8°, danach steht und läuft
die Ente wieder (Test `micro_duck_jumps_for_real_in_the_studio`).

## 14.10 ONNX-Export und Metadaten

`export_onnx(actor, normalizer, metadata)` schreibt den Actor in der Form von Pollens exportierten
Netzen (IR-Version 8, Opset 17, Producer „kern“):

```
obs [1, n] ─ Sub(obs_mean) ─ Div(obs_div) ─ Gemm(w0, b0, transB) ─ Elu(α=1) ─ … ─ Gemm(wL, bL) ─ actions [1, m]
```

`obs_div` ist √var + 0,01. Die Gewichte werden für `transB` als [out][in] abgelegt. Kern liest die
Datei mit `parse_onnx_policy` (Test `lehrer_exports_onnx_like_pollens_networks`: bis 10⁻⁴ genau wie
der Actor im Lehrer), und ONNX Runtime, die Laufzeit des Roboters, lädt sie ebenfalls.

**Metadaten** für Bewegungsnetze (`micro_duck_metadata(task)`), dieselben Schlüssel wie bei Pollen:

| Schlüssel | Wert |
|---|---|
| `action_scale` | `1.0` |
| `command_names` | `twist` |
| `observation_names` | `base_ang_vel,projected_gravity,joint_pos,joint_vel,actions,command,head_command,body_command` |
| `joint_names` | die 14 Gelenke in Policy-Reihenfolge |
| `default_joint_pos` | die Home-Pose |
| `joint_stiffness`, `joint_damping` | 1,0 bzw. 0,0 je Gelenk |
| `task` | z. B. `jump` |
| `lehrer` | `Kern (C++ PPO on Box3D)` |

Die Spielschicht hat eigene Metadaten (`play_metadata()`): `task` = `play`, `lehrer` = `Kern (C++
DAgger + PPO on Box3D)`, `rate_hz` = `25` und die Namen der 23 Beobachtungen und 9 Aktionen.

## 14.11 Der Lehrer im Browser

![Die Lehrer-Seite im Browser](bilder/lehrer.png)

`kern/build.sh --lehrer` baut `kern_lehrer.wasm`: Engine, Box3D, Spiel-Laufzeit, `src/lehrer` und die
C-Schnittstelle (`lehrer_api.cpp`). Die Seite `/lehrer/` (`web/lehrer/`) lädt sie einmal und gibt das
kompilierte Modul jedem Worker.

**Rollen:**

- **Sammler:** Jeder Worker ist eine Modulinstanz mit Enten `i · envs … (i + 1) · envs − 1` eines
  Laufs (`kern_lehrer_create(task, envs, seed, first)`). Pro Iteration bekommt er die Policy
  (`kern_lehrer_set_policy`), die Lehrpläne (`kern_lehrer_progress`) und gibt einen Rollout zurück
  (`kern_lehrer_collect`). In der Nachahmung rechnet jeder seinen Teil jeder Runde
  (`kern_lehrer_dagger`).
- **Lerner:** ein eigener Worker (`kern_lehrer_learner`) für Nachahmung (`kern_lehrer_imitate`),
  PPO-Update (`kern_lehrer_learn`), Sprungbewertung alle 25 Iterationen (`kern_lehrer_evaluate`, 5
  Episoden) und Export (`kern_lehrer_export_onnx`).

Die Puffer sind dieselben wie nativ (`Policy::serialize`, `Rollout::serialize`); Ergebnisse kommen
als Zeiger mit `kern_lehrer_result_length()` Bytes zurück. Der Lerner akzeptiert JSON-Einstellungen,
die das Rezept überschreiben: `learningRate`, `initStd`, `entropyCoef`, `desiredKl`, `steps`, `epochs`,
`minibatches`, `warmStart` (`velstand` | `none`), `imitate`.

**Die Seite** bietet Bewegung (Sprung oder Stehen als Schnelltest), Worker (Standard: Kerne − 1,
höchstens 8), Enten je Worker (Standard 8), Iterationen (Standard 1500), Seed, Aktionsrauschen am
Anfang und Lernrate (leer = Rezept). Sie zeigt Iteration, Entenschritte, Schritte/s, mittlere
Episoden-Rückgabe und -länge, Aktionsrauschen, echte Sprünge, Sprunghöhe und Flugzeit und zeichnet
Rückgabe und Länge über die Iterationen. Sie hält den Bildschirm wach (Wake Lock).

Alle 25 Iterationen und am Ende speichert sie das Netz für Kern Studio auf dem Gerät (IndexedDB
„kern-lehrer“, Store „policies“, Schlüssel `<Aufgabe>.onnx`, beim Sprung `jump.onnx`) und meldet es über den BroadcastChannel
„kern-lehrer“; offene Studio-Tabs übernehmen es sofort. „.onnx herunterladen“ speichert die Datei.

Gleicher Seed und gleich viele Enten ergeben dasselbe Netz wie nativ. Die Spielschicht lässt sich im
Browser nicht trainieren; die Seite kennt nur die Aufgaben `jump` und `stand`.

Auf dem Handy: Chromes CPU-Drosselung bremst nur den Hauptthread, nicht die Worker, und taugt deshalb
nicht als Handy-Profil; gemessen ist stattdessen der Ein-Kern-Fall (Tabelle in 14.9). Die Seite zeigt
auf dem Gerät selbst die Entenschritte/s.

## 14.12 Tests

| Test | prüft |
|---|---|
| `lehrer_mlp_gradient_matches_finite_differences` | Rückwärtsrechnung des MLP |
| `lehrer_adam_fits_a_curve_and_the_normaliser_tracks` | Adam und Normalisierer |
| `lehrer_ppo_learns_to_reach` | PPO lernt eine einfache Aufgabe |
| `lehrer_ppo_learns_from_collectors_elsewhere` | Sammeln und Lernen getrennt |
| `lehrer_duck_env_runs_episodes_headless` | velstand hält die Ente über den Lehrer-Weg; gleiche Episode Bit für Bit |
| `micro_duck_observation_between_ticks_is_what_its_controller_sees` | Beobachtung zwischen Takten |
| `determinism_is_bit_identical_across_builds_and_platforms` | Fingerabdruck |
| `lehrer_runs_the_same_on_any_number_of_workers` | 1, 2, 4 Worker → dasselbe Netz |
| `lehrer_exports_onnx_like_pollens_networks` | Export |
| `micro_duck_jumps_with_a_network_trained_in_kern`, `micro_duck_jumps_for_real_in_the_studio` | Sprung |
| `lehrer_calibrates_a_servo_from_bench_logs`, `lehrer_replays_robot_logs_against_the_simulation` | Kalibrierung ([Kapitel 16](16-kalibrierung.md)) |

---

© 2026 Christian Heinrich Hohlfeld · Alle Rechte vorbehalten · [Impressum und Rechte](00-impressum.md)
