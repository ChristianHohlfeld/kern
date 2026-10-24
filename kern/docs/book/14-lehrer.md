# 14 The Lehrer

The Lehrer ("teacher") is Kern's own training system for the Micro Duck. Simulation, learning
algorithm and export run in C++ on Kern's engine and Box3D, without Python and without PyTorch. It
learns natively on all cores (headless on a server, too) and as WebAssembly in the browser, on
desktop and phone. A run produces the same network bit for bit everywhere.

The duck in the Lehrer is the same as in the Studio: the same `GameWorld`, the same servos, sensors
and 50 Hz controller, the same physics with 16 sub-steps ([Chapter 13](13-micro-duck.md)). A network
that learns here therefore runs unchanged in the Studio. It is exported in the format of Pollen's
networks and, like them, can go onto the robot.

Kern's own networks were created with the Lehrer: `policies/jump.onnx` (the jump) and
`policies/play.onnx` (the play layer). This chapter explains how it is built and how it computes,
staying as close to the code as possible. The general foundations – what a neural network is,
gradient descent, backpropagation, supervised learning and reinforcement learning, MDP, policy and
value function – are covered in [Appendix A](A-neural-networks.md). Step-by-step usage is described
in [Chapter 15](15-your-own-models.md).

## 14.1 Overview

| File | Contents |
|---|---|
| `include/kern/lehrer/env.h`, `src/lehrer/duck_env.cpp` | `Rng`, `Env`, `Task`, `DuckEnv`; `env.h` also holds the declarations of `JumpMeter`, `evaluate_jump` and the fingerprint |
| `src/lehrer/tasks.cpp` | the tasks `stand` and `jump` |
| `include/kern/lehrer/nn.h`, `src/lehrer/nn.cpp` | `Mlp`, `Adam`, `Normalizer` |
| `include/kern/lehrer/ppo.h`, `src/lehrer/ppo.cpp` | `PpoConfig`, `Policy`, `Rollout`, `Collector`, `Ppo` (PPO and imitation) |
| `include/kern/lehrer/backend.h`, `src/lehrer/backend.cpp` | `Backend`, `ThreadBackend`, `train_iteration` |
| `include/kern/lehrer/recipe.h`, `src/lehrer/recipe.cpp` | `Recipe`: task, PPO settings, warm start, curricula |
| `include/kern/lehrer/vorbild.h`, `src/lehrer/vorbild.cpp` | jump demonstration (Vorbild), demonstration search (CMA-ES), DAgger |
| `include/kern/lehrer/cmaes.h`, `src/lehrer/cmaes.cpp` | CMA-ES |
| `include/kern/lehrer/spielen.h`, `src/lehrer/spielen.cpp` | `PlayEnv`, play-layer demonstration, bite measurement, evaluation, DAgger |
| `include/kern/lehrer/onnx_export.h`, `src/lehrer/onnx_export.cpp` | ONNX export and metadata |
| `include/kern/lehrer/kalibrierung.h`, `src/lehrer/kalibrierung.cpp` | test bench and matching ([Chapter 16](16-calibration.md)) |
| `src/lehrer/evaluate.cpp`, `fingerprint.cpp` | jump measurement, determinism fingerprint |
| `src/lehrer/lehrer_api.cpp` | C interface for the browser |
| `tools/kern_lehrer.cpp`, `tools/lehrer.sh` | the command-line tool and its build |
| `web/lehrer/` | the Lehrer page: `index.html`, `lehrer.mjs`, `worker.mjs` |

Status according to `docs/lehrer.md`:

| Step | Status |
|---|---|
| Headless simulation, MLP with backprop and Adam, PPO like rsl_rl, ONNX export | done |
| Variation (domain randomisation), learning in the browser, determinism | done |
| The jump: demonstration → imitation → practice | done |
| Digital twin: device models, calibration, matching | done (without real measurement data) |
| Play: kick a ball, pick up a sock, carry it off, spit it out | being reworked (Section 14.8) |

## 14.2 Architecture

The Lehrer separates five roles so that they can run in different places:

```
             Recipe ──────────► Task ──► DuckEnv / PlayEnv  (one duck, one GameWorld, headless)
               │                              ▲
               │                              │ step(action) / observation()
               ▼                              │
   Ppo (learner) ──Policy──► Backend ──► Collector ──► Rollout
        ▲                    (threads | web workers)       │
        └──────────────────── learn(rollouts) ◄────────────┘
```

- **`Task`** says what is learned: start state, command slots of the observation, reward, episode
  end, curricula, privileged observation for the critic.
- **`Env`** is the abstract environment (`reset`, `observation`, `critic`, `step`). `DuckEnv` runs a
  duck with a `Task` through episodes (61 observations → 14 actions); `PlayEnv` runs the play layer
  (23 → 9).
- **`Recipe`** bundles task, PPO configuration, warm start, servo profile, sub-steps, imitation and
  curricula. Every place that trains (tool, browser) uses the same recipes (`recipe_for`), so that
  the same run produces the same network everywhere.
- **`Collector`** runs environments with a fixed `Policy` (actor, normaliser, noise) and delivers a
  `Rollout`. **`Ppo`** is the learner: it takes the rollouts of all collectors, computes the update
  and hands out the next `Policy`. Both serialise into flat buffers (`Policy::serialize`,
  `Rollout::serialize`).
- **`Backend`** is where the computation happens. `ThreadBackend` distributes groups of environments
  across threads; in the browser each web worker is a collector and one further worker is the
  learner.

This keeps the training loop short:

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

`ThreadBackend(workers, envs_per_worker, seed, make, prepare)` creates, for each worker, a group of
environments with global indices `w · envs_per_worker …` and a `Collector(seed, first)`. Before each
iteration it calls `prepare(env, iteration)` (the curricula), then each group collects on its own
thread. `update_parallel()` returns the thread pool for the computational work of the update.

## 14.3 The duck's environment

### `DuckEnv`

`DuckEnv(task, seed, substeps)` builds a duck on level ground (friction 1) in a `GameWorld` of its
own. The renderer stays headless (`Renderer::set_headless`): meshes stay on the CPU, GL is never
touched. The duck is not autonomous; `set_walker_external(duck, true)` makes the controller take the
Lehrer's action instead of a network's, and does so as trained: home + action, scale 1, no low-pass,
with the bus delay of the servo profile. If a motion is running (skill, pick, sitting), it takes
priority.

One `step(action)` is one control tick (20 ms robot time):

```cpp
w.set_walker_action(duck_, last_action_);
do { w.step_once(); } while (!w.walker_tick_due(duck_) && ++guard < 64);   // 4 world steps
++step_;
if (task_->randomize && step_ >= next_push_) w.walker_push(duck_, {±p, ±p, 0});
w.walker_probe(duck_, probe_);
r.reward = task_->reward(Tick{probe_, last_action_, previous_action_, step_}, r.terminal);
r.timeout = !r.terminal && step_ >= max_steps_;
observe();
```

`walker_probe` provides what reward and critic read, in robot units (not in the four-times world
size): trunk position and velocity, angular rate, gravity in the trunk frame, which sole is touching
the ground, foot heights, self-contact, ground contact of other parts, joint angles and velocities,
last action.

`observe()` builds the observation:

1. `walker_observation` – exactly what the controller sees on the next tick.
2. `task->command(obs, step)` writes the command slots 48–60.
3. The **critic** receives this clean observation plus `task->privileged(probe)`.
4. `task->before_action(...)` – this is where a task with an imitation reward asks its
   demonstration.
5. Only the **actor** receives the sensor errors (section "Variation").

### `Task`

```cpp
class Task {
 public:
  virtual std::string name() const = 0;
  virtual i32 privileged() const { return 0; }          // extra observations for the critic only
  virtual f32 episode_seconds() const = 0;
  virtual Start reset(Rng& rng) = 0;                    // start and variation of the episode
  virtual void command(f32 observation[kObs], i32 step);
  virtual void privileged(const WalkerProbe& p, f32* out);
  virtual void before_action(const f32* observation, const WalkerProbe& probe, i32 step);
  virtual f32 reward(const Tick& t, bool& terminal) = 0;
  virtual void progress(i32 iteration);                 // curricula
  Noise noise;                                          // variation
  bool randomize = true;                                // off: the clean robot
  std::vector<std::pair<const char*, f32>> terms;       // reward terms of the last tick
};
```

Rewards follow mjlab's convention: sum of weight × term × dt (dt = 0.02 s). The helper class `Terms`
in `tasks.cpp` writes each term with its name into `terms` so that `--trace` can show it; one-off
terms (the fall) are not multiplied by dt.

Two tasks are built in:

- **`stand`** (quick test of the pipeline): stand for 4 s, reward from `upright` (2),
  `pose_legs` (2), `pose_neck` (1), `height` (1, reference 0.115 m), `action_rate` (−0.1); a fall
  costs −4 and ends the episode.
- **`jump`** (Section 14.6).

`make_task(name)` in `recipe.cpp` knows exactly these two names.

### Asymmetric critic

Actor and critic see different things. The **actor** sees only what the robot's sensors deliver,
with all the errors of the variation; only the actor is exported and has to run on the robot. The
**critic** exists only during learning. It sees the clean observation plus quantities that no sensor
provides. For the jump (`privileged() = 9`):

| Slot | Contents |
|---|---|
| 61 | trunk height above the start height |
| 62–64 | trunk velocity x, y, z |
| 65, 66 | left / right sole on the ground |
| 67, 68 | left / right foot height |
| 69 | has already been airborne |

This lets the critic estimate the value of a state more accurately, and the advantages from which
the actor learns become less noisy.

### Variation (domain randomisation)

Like Pollen's velocity and kick recipes. All values per episode, as long as `randomize` is on:

| Area | What | Values (`Task::Noise`, task) |
|---|---|---|
| Start | yaw, tilt, sole friction | yaw ±180°, roll/pitch ±3°, sole friction × 0.7 … 1.3 |
| Build (`vary_micro_duck`) | trunk mass and inertia | × 0.95 … 1.05 |
| | centre of mass of trunk / head per axis | ±3 mm / ±3 mm; for the jump, curriculum up to ±15 / ±10 mm |
| Servos (profile) | separate values per servo within the spreads | XL330: friction and armature ±10 % |
| | bus delay | 15 … 30 ms |
| Sensors (actor only) | gyro, gravity | ±0.03, ±0.01 (uniform) |
| | joint angle, encoder offset per episode | ±0.001, ±0.015 |
| | joint velocity | ±0.25 |
| | IMU delay | 0 … 1 tick, redrawn every 64 ticks |
| Pushes | a velocity push in x, y every 3–6 s | ±0.3 m/s × `push_scale` |

`Recipe::push_scale(iteration)` switches the pushes on: 0 until iteration 300, 0.27 until 500, then 1.
How the servo spread comes from the profile and how to measure it is described in
[Chapter 16](16-calibration.md).

The variation forces the network not to rely on anything the simulation does not know precisely: a
network that can only stand with one trunk mass, one friction and one latency will not stand on the
real robot. The evaluation therefore always measures twice, on the clean robot and with full
variation (`progress(100000)`: the curricula at their end).

### Randomness

`Rng` is splitmix64: small, fast and reproducible. `uniform()` takes the upper 24 bits, `normal()` is
Box–Muller with `kern::dm::log` and `kern::dm::cos`. Each environment has its own stream; the seed of
environment k in a run with seed s is `s · 1000003 + k` (`make_env`).

## 14.4 Networks: `Mlp`, `Normalizer`, `Adam`

### The MLP

`Mlp` is a multilayer perceptron like rsl_rl's actor and critic: ELU between the layers, linear
output. `dims` are inputs, hidden layers, outputs, so for the jump actor `{61, 512, 256, 128, 14}`.
All parameters live in one vector `params_`; for each layer l the weights `w[i · out + o]`
(input-major) followed by the biases `b[o]`.

**Initialisation** as in PyTorch's `nn.Linear`: weights and biases uniform in ±1/√fan_in; the last
layer can be scaled with `output_gain` (default 1).

**Forward pass** for a batch of `batch` inputs:

```
a⁰ = x
zˡ = aˡ · Wˡ + bˡ                       (Wˡ: in × out, stored input-major)
aˡ⁺¹ = ELU(zˡ)   for hidden layers,   ELU(z) = z for z > 0, otherwise e^z − 1
y  = z^L                                 (last layer linear)
```

`dense()` computes `y[r][o] = b[o] + Σᵢ x[r][i] · w[i][o]`, four rows of the batch per pass over the
weights. The memory layout turns both directions into contiguous axpy loops that the compiler
vectorises without reordering sums (important for determinism, Section 14.7). `expm1` comes from
Kern's deterministic libm (`kern::dm::expm1`). With a `Cache`, the forward pass remembers all
activations `act[0..L]` for the backward pass.

**Backward pass** (`backward(cache, dout, grad, dx)`), layer by layer from the back, with
`δ = ∂loss/∂z` (at the output, `dout`):

```
∂b[o]    += Σ_r δ[r][o]
∂W[i][o] += Σ_r a[r][i] · δ[r][o]
δ_below  = δ · Wᵀ
δ_below  ⊙= ELU'(z)    with ELU'(z) = 1 for z > 0, otherwise e^z = a + 1
```

The last step uses the fact that the derivative of the ELU in the negative range is
e^z = (e^z − 1) + 1 = a + 1; Kern only needs the stored activation for it. Gradients are **added**
to `grad`, so that the pieces of a minibatch can be summed into one gradient. On request, `dx`
returns the gradient with respect to the input. The test
`lehrer_mlp_gradient_matches_finite_differences` compares the gradient with finite differences.

### Normalisation

`Normalizer` is rsl_rl's `EmpiricalNormalization`: running mean and variance per input,

```
y = (x − mean) / (√var + 0.01)
```

It is updated with Chan's parallel formula for a batch of size n with mean m and variance v
(in f64):

```
total = count + n
δ     = m − mean
mean' = mean + δ · n / total
var'  = (var · count + v · n + δ² · count · n / total) / total
```

It starts at mean 0, variance 1, count 0. The export writes the normaliser into the ONNX as `Sub`
(mean) and `Div` (√var + 0.01), exactly as in Pollen's networks. The critic has a normaliser of its
own over its 61 + privileged inputs.

A **warm start** (`Ppo::warm_start`) takes over the weights and normaliser of a finished network and
**freezes the normaliser** (`freeze_actor_normalizer`): the weights were trained with exactly these
values. Inputs that the old network never used (for the jump, the phase slots 55 and 56) get weight
0, mean 0 and divisor 1; the actor then initially acts exactly like the old network until it learns
to use them.

### Adam

`Adam` with PyTorch's default values (β₁ = 0.9, β₂ = 0.999, ε = 10⁻⁸) and, before it, clipping of
the gradient to the global norm `max_norm` (rsl_rl: 1.0):

```
‖g‖ = √(Σ g²)                         over all blocks together (f64)
if ‖g‖ > max_norm:  g ← g · max_norm / (‖g‖ + 10⁻⁶)
t ← t + 1
m ← β₁ m + (1 − β₁) g
v ← β₂ v + (1 − β₂) g²
θ ← θ − (lr / (1 − β₁ᵗ)) · m / (√v / √(1 − β₂ᵗ) + ε)
```

Several parameter blocks (for the actor: network and standard deviation) share one clipping
(`step({Block{...}, Block{...}})`). The moments and the step counter belong in the checkpoint.

## 14.5 PPO step by step (`ppo.cpp`)

Kern follows the PPO of rsl_rl, with which Pollen trains the duck's networks: Gaussian actor with a
learnable standard deviation, asymmetric critic, GAE, clipped surrogate and value loss, entropy,
adaptive learning rate via the KL divergence, gradient clipping per network, Adam.

### Configuration

| `PpoConfig` | Default | Jump (`recipe_for("jump")`) | Play layer (`make_play_learner`) |
|---|---|---|---|
| `actor_hidden` | 512, 256, 128 | 512, 256, 128 (like velstand) | 256, 128 |
| `critic_hidden` | 512, 256, 128 | 256, 128 (all recipes) | 256, 128 |
| `init_std` | 1.0 | 0.3 | 0.2 |
| `steps_per_env` | 24 | 48 | 64 |
| `epochs`, `minibatches` | 5, 4 | 5, 4 | 5, 4 |
| `clip` | 0.2 | 0.2 | 0.2 |
| `gamma`, `lam` | 0.99, 0.95 | 0.99, 0.95 | 0.99, 0.95 |
| `value_coef` | 1.0 | 1.0 | 1.0 |
| `entropy_coef` | 0.01 | 0.005 | 0 |
| `learning_rate` | 10⁻³ | 10⁻⁴ | 10⁻⁴ |
| `desired_kl`, `adaptive_lr` | 0.01, on | 0.01, on | 0.01, on |
| `max_grad_norm` | 1.0 | 1.0 | 1.0 |
| `clipped_value_loss` | on | on | on |

### 1. Collecting (`Collector::collect`)

Each environment runs `steps_per_env` ticks on one thread, one after the other (an episode is
sequential). On each tick:

```
x  = normalize(o)
μ  = actor(x)
a  = μ + σ ⊙ ε,    ε ~ N(0, 1) from the environment's own stream
r, terminal, timeout = env.step(a)
```

Stored are observation, critic observation, action, reward, `terminal`, `timeout`; at the end, the
critic observation of where each environment stands (`last_critic`), for bootstrapping. When an
episode ends, the environment is reset and its return and length are recorded. The rows are laid
out `[step][environment]`.

The noise stream of environment k depends only on (seed, global index), never on which worker runs
it.

### 2. Canonical ordering and normaliser update

`learn(rollouts)` stores all rows of all rollouts in **one** fixed order: step by step, within that
the rollouts in the order of their global indices, within that environment by environment. The
result therefore does not depend on how the environments were distributed across workers.

Then the normalisers learn from all observed rows at once (rsl_rl updates them step by step during
collection; Kern's collectors keep the policy fixed for a rollout, so it happens here). A frozen
actor normaliser stays frozen; the critic's always learns.

### 3. Re-evaluating the old policy

Using the **current** normalisers, the learner computes for each row the mean μ_old, the value V(s)
and the log probability of the collected action:

```
log π(a|s) = Σ_j [ −(a_j − μ_j)² / (2 σ_j²) − ln σ_j − ½ ln 2π ]
```

The reason is given in `docs/lehrer.md`: with a few dozen environments the normalisers drift
noticeably between two iterations; with the values from collection time the ratio of new to old
policy would be distorted.

### 4. Advantage with GAE

Per environment, backwards through time, with `live = 0` after an end (terminal or timeout):

```
r'_t = r_t + γ · V(s_t)          if step t was a timeout (bootstrapping as in rsl_rl)
δ_t  = r'_t + live_t · γ · V(s_{t+1}) − V(s_t)
A_t  = δ_t + live_t · γ · λ · A_{t+1}
R_t  = A_t + V(s_t)              (the critic's target)
```

V(s_T) at the end of the rollout is the value of `last_critic`. A timeout is not a failure: the
episode could have continued, so its value is estimated instead of set to 0. A `terminal` (fall), on
the other hand, has no value afterwards.

The advantages are then normalised over the whole batch: `(A − mean) / (std + 10⁻⁸)`, with the
sample standard deviation (n − 1).

### 5. Update: epochs and minibatches

One permutation of all rows applies to all epochs (like rsl_rl's `mini_batch_generator`). Minibatch
size = rows / `minibatches` (any remainder is dropped). For each of the `epochs × minibatches`
minibatches:

**a) Forward** for actor and critic, split into a fixed 8 chunks (`kLearnChunks`) that run on the
threads.

**b) Adaptive learning rate.** Before the gradient step, the learner measures the KL divergence
between the policy at collection time (μ_old, σ_old = σ at the start of the update) and the current
one (μ, σ), averaged over the minibatch:

```
KL = mean_r Σ_j [ ln(σ_j / σ_old,j) + (σ_old,j² + (μ_old − μ)²) / (2 σ_j²) − ½ ]
KL > 2 · desired_kl  →  lr ← max(10⁻⁵, lr / 1.5)
KL < desired_kl / 2  →  lr ← min(10⁻², lr · 1.5)
```

Both Adam optimisers receive the new rate. This keeps every step within a trust region: if the
policy has already moved far, the next step becomes smaller.

**c) Surrogate loss** (clipped), per row:

```
ρ      = exp(clamp(log π_new − log π_old, −20, 20))
L_surr = max(−A · ρ,  −A · clamp(ρ, 1 − ε, 1 + ε))       ε = clip = 0.2
```

The gradient only flows when the unclipped branch is the larger one; otherwise it is 0. With
`g = ∂L/∂log π = −A · ρ / n_mb`:

```
∂L/∂μ_j = g · (a_j − μ_j) / σ_j²
∂L/∂σ_j += g · ((a_j − μ_j)² / σ_j³ − 1 / σ_j)
```

The standard deviation σ is a directly learned parameter per action (not its logarithm) and is kept
at no less than 10⁻³ after each step.

**d) Value loss** (clipped around the value at collection time V₀):

```
V_c = V₀ + clamp(V − V₀, −ε, ε)
L_V = max((V − R)², (V_c − R)²)
```

If the unclipped term is the larger one, the gradient is `value_coef · 2 (V − R) / n_mb`; otherwise
`value_coef · 2 (V_c − R) / n_mb`, but only as long as |V − V₀| < ε.

**e) Entropy.** For a Gaussian actor, the entropy is independent of the state:

```
H = Σ_j (½ + ½ ln 2π + ln σ_j)
```

The loss is −`entropy_coef` · H; its gradient −`entropy_coef` / σ_j goes to σ. A small bonus keeps
the noise from vanishing too early.

**f) Backward and step.** Each chunk computes `actor.backward` and `critic.backward`; the gradients
of the 8 chunks are summed in a fixed order. Then `actor_adam_` takes a step over actor and σ
together (jointly clipped to norm 1), and `critic_adam_` one over the critic (with its own
clipping).

### Critic first

`set_actor_frozen(true)` lets only the critic learn; the adaptive learning rate rests in that case.
After imitation, only the critic learns during the first `critic_warmup` iterations (recipe: 30;
practice phase of the play layer: 20). Otherwise the advantages of a critic that does not yet know
the returns of the new policy steer an actor that already acts well – and destroy it.

### Statistics and checkpoint

`IterationStats` contains the mean reward per step, mean return and length of the episodes that
ended in the iteration, value and surrogate loss, entropy, learning rate, mean σ, collection and
learning time, and the total number of duck steps. `Ppo::save()` writes the whole learner (magic
`KERNPPO1`: iteration, steps, learning rate, normaliser status, policy, critic, both normalisers,
both Adam states); `load()` reads it back.

## 14.6 Imitation, demonstrations and CMA-ES

### Why PPO alone did not find the jump

The first runs (PPO, with a curriculum for the fall penalty, phase signal, warm start from velstand,
reference height in the reward, various network sizes and learning rates) did not find the take-off.
`docs/lehrer.md` gives three reasons:

- The take-off needs all leg servos at their limit at the same time. The servos are soft (maximum
  torque only at 1.7 rad deviation): the network has to set targets far beyond the joint limits.
  Random exploration around a standing policy practically never produces that.
- Standing is safe and earns reward; a failed attempt costs a fall.
- With only a few dozen ducks the minibatches are small and the gradient is noisy; the KL limit
  keeps the learning rate down.

That is why the Lehrer teaches in three steps: **find a demonstration → imitate → practise**. At
runtime there is only the network; the demonstration (the *Vorbild*, "role model") exists only
during learning.

### The jump demonstration

`JumpDemo` describes a jump with 15 numbers: the servo targets of hip (pitch), knee and ankle of the
left leg at five points in time (crouch, push-off, tuck, landing, absorbing); the right leg mirrors
them (−v, because its axes are mirrored).

```cpp
constexpr f32 kTimes[7] = {0.15f, 0.25f, 0.29f, 0.34f, 0.40f, 0.47f, 0.60f};  // phase; 0 outside
// target = amplitude · tanh(linear interpolation of the keys), amplitude = 2.5 rad
```

`tanh` with an amplitude of 2.5 rad allows targets far beyond the joint limits: the servo then pushes
with its maximum torque. `JumpTeacher::act` lets Pollen's velstand keep the balance before phase 0.15
and from phase 0.5 onwards (with zeroed phase slots, which velstand has never seen), and sets the
demonstration's targets in between.

### CMA-ES (`cmaes.cpp`)

CMA-ES (Hansen's Covariance Matrix Adaptation Evolution Strategy) optimises without gradients. The
Lehrer uses it where there is no derivative: the simulation is the function, its parameters are the
input – the jump demonstration and the servo calibration.

`cmaes_minimise(start, sigma, generations, seed, evaluate_all, population, tolerance, progress)`
follows Hansen's tutorial (2016):

```
λ = population (default 4 + ⌊3 ln n⌋),  μ = λ/2
wᵢ ∝ ln(μ + ½) − ln i, normalised;  μ_eff = 1 / Σ wᵢ²
c_σ = (μ_eff + 2) / (n + μ_eff + 5)
d_σ = 1 + 2 max(0, √((μ_eff − 1)/(n + 1)) − 1) + c_σ
c_c = (4 + μ_eff/n) / (n + 4 + 2 μ_eff/n)
c₁  = 2 / ((n + 1.3)² + μ_eff)
c_μ = min(1 − c₁, 2 (μ_eff − 2 + 1/μ_eff) / ((n + 2)² + μ_eff))

per generation:
  C = B D² Bᵀ                            (Jacobi eigendecomposition; n is small)
  x_k = m + σ · B D z_k,  z_k ~ N(0, I)   for k = 1 … λ, all passed together to evaluate_all
  sort by cost; m ← m + σ Σᵢ wᵢ y_(i)
  p_σ ← (1 − c_σ) p_σ + √(c_σ(2 − c_σ) μ_eff) · B z_w
  p_c ← (1 − c_c) p_c + h_σ √(c_c(2 − c_c) μ_eff) · y_w
  C   ← (1 − c₁ − c_μ) C + c₁ (p_c p_cᵀ + (1 − h_σ) c_c(2 − c_c) C) + c_μ Σᵢ wᵢ y_(i) y_(i)ᵀ
  σ   ← σ · exp((c_σ / d_σ)(‖p_σ‖ / χ_n − 1))
```

The starting point is evaluated first; the result is never worse. Returned are the best evaluated
point (`best`) and the centre of the search distribution (`mean`). With noisy costs the centre is
the better answer, because the best point may just have been lucky. All candidates of a generation
go to `evaluate_all` together and run in parallel there. The normal distribution comes in double
precision from a separate `Rng` with `kern::dm` functions, so a search is reproducible as well.

### The demonstration search

`search_jump_demo(generations, seed, threads, start)` searches for the 15 keys with population 16,
σ = 0.8 from the default pose (0.3 from a given start). Each candidate jumps 6 episodes: the first on
the clean robot, the others with full variation. Each generation meets different robots, all
candidates of a generation the same ones – a demonstration that only works for a few does not teach
a robust jump. The score (`score_jump_demo`, higher is better):

```
2000 · min(rise in the air, 0.02 m)
+ 250 · min(longest flight, 0.12 s)
+ 500 · min(max(height in the air above standing height, 0), 0.03 m)
− 300 · max(0, 0.966 − smallest upright value)        (tilt beyond 15° costs)
+ fall: −40 + 40 · ticks until the fall / 100          (an early fall costs more)
+ 30 if it ends up standing on both soles
+ 20 if it was a real jump (JumpMeter)
```

The search returns the centre `mean`. The built-in demonstration (`builtin_jump_demo()`) was found
for the XL330 profile in three stages with a total of 370 generations.

### Imitation with DAgger

Behaviour cloning on its own – the network learns from episodes driven by the demonstration – has a
well-known problem: **distribution shift**. During learning the network only sees states the
demonstration gets into. If it makes a small mistake itself, it ends up in a state it has never
seen, makes a bigger mistake there, and the errors grow over the episode.

DAgger ("Dataset Aggregation") fixes this by letting the network drive itself while the
demonstration provides the labels:

1. **Round 0:** the demonstration drives. Each observation is labelled with its action.
2. **Round k > 0:** the network drives (mean plus noise 0.05 for coverage). On every tick the
   demonstration says what it would have done; that action is the label.
3. After each round the network learns from **all** examples collected so far.

The dataset thus contains exactly the states the network itself gets into, with the right answer
for them. Prerequisite: the demonstration must have a sensible answer in each of these states.

In Kern (`dagger_episodes`, `JumpImitation`):

- The episodes of a round run with full variation (`prepare_env(..., 1000)`), with seed
  `seed + 7919 · (round + 1)` and a global episode index.
- Anyone can compute any part of a round (a thread, a web worker); joined in episode order, the parts
  make up exactly the whole round.
- **Jump recipe:** 8 rounds of 96 episodes each.

The fitting is `Ppo::imitate(observations, targets, epochs, batch, lr, seed)`: mean squared error
between actor mean and label, with its own fresh Adam, minibatches in a fixed shuffled order
(reshuffled each epoch with the seed), each minibatch split into 8 fixed parts:

```
L = 1 / (m · A) · Σ (μ − target)²,     ∂L/∂μ = 2 (μ − target) / (m · A)
```

A non-frozen actor normaliser takes in the observations beforehand. Jump: 10 epochs, minibatch 256,
learning rate 10⁻³. Play layer: 30 epochs, 256, 10⁻³.

### Practice

After imitation (`begin_practice`) the action noise is set to `practice_std` (jump: 0.1), the actor
waits `critic_warmup` = 30 iterations for the critic, and then PPO practises with the jump reward,
an imitation reward and full variation until the jump is robust.

### The `jump` task

The command is the phase signal alone: cos and sin of the phase t / 2 s in slots 55 and 56,
otherwise 0. No controller, no feedback, no script, no IK, no forces at runtime; the motion is
produced solely by the network through the 14 servo targets. The episode lasts 2 s.

Reward (all terms × dt, except the fall):

| Term | Weight | What |
|---|---|---|
| `track_height` | 4 × upright | only without a demonstration: trunk follows a reference height over the phase |
| `airborne` | 20 × upright | in the flight window (phase 0.27–0.42), in the air and rising |
| `lift` | 20 × upright | rising speed in the take-off window (0.22–0.33), 0–1 m/s |
| `landed` | 6 × upright | from phase 0.42, after a flight, on both soles |
| `extra_hop` | −4 | in the air again from phase 0.5 |
| `imitate` | 10 (recipe) | exp(−mean squared distance to the demonstration's action / 0.3²) |
| `upright` / `tilt` | 2 / −30 | exp(−g_xy² / 0.05); g_xy² costs |
| `pose_legs` / `pose_neck` | 1 / 1 | legs in default pose before 0.15 and after 0.55; neck always |
| `drift` | −2 | horizontal velocity² |
| `action_rate` | −0.1 → −0.5 | curriculum from iteration 200 over 600 |
| `body_ang_vel` | −0.05 | angular rate about x and y |
| `self_collisions` | −1 | self-contact |
| `fall` | −2 → −20 (one-off) | curriculum over 300 iterations |

"In the air" means: no sole and no other part on the ground. A fall (gravity z above −0.5 or trunk
below 5 cm) and **any ground contact of a part other than the soles** end the episode – otherwise
leaning on the beak or tail becomes the safe bet.

### What counts as a real jump (`JumpMeter`)

- **Flight:** all parts without ground contact for at least 60 ms.
- **Lift-off:** in the air, the trunk rises at least 5 mm above its height at lift-off. Merely
  tucking the feet is not a jump.
- **Upright:** never more than 30° of tilt.
- **Landing:** no fall, and upright on both soles at the end.

`evaluate_jump(act, user, episodes, seed, substeps, randomized)` measures a policy this way in Studio
physics and returns a `JumpReport` (real jumps, falls, means of height, rise, flight time, tilt,
best values).

## 14.7 Determinism

A simulation, a training run or a replayed robot log comes out the same everywhere: natively on x86
and ARM (GCC, Clang, Apple clang) and as WebAssembly in every browser. A simulation error is thus a
fixed, reproducible error and not noise that looks different from machine to machine.

### What it takes

- **No FMA contraction:** Kern, Box3D and libm are compiled with `-ffp-contract=off`. Otherwise
  `a*b+c` rounds differently on ARM and x86 with `-march=native` than in WebAssembly.
- **Box3D on the scalar path** (`BOX3D_DISABLE_SIMD`), as in WebAssembly. Natively this costs
  nothing: the joint solver is the bottleneck, not the SIMD contacts.
- **Own libm:** `sin`, `cos`, `atan2`, `exp`, `log`, `pow` … round differently in the last bit in
  glibc, Apple's libm, Android's bionic and wasi-libc. In a robot simulation one bit turns into a
  different gait within seconds. Kern uses musl's implementations everywhere
  (`third_party/musl_math`, renamed to `kern_musl_*`) via `kern::dm::`
  (`include/kern/core/dmath.h`). `sqrt`, `fabs`, `floor` are exact in IEEE and stay `std::`.
- **Learning independent of the number of workers:**
  - Each duck gets its seed from (seed, global index), not from the worker; the same goes for the
    collector's noise stream.
  - The learner orders the rollouts canonically (step by step, duck by duck) before computing
    normalisers, GAE and update.
  - Update and imitation split every minibatch into a fixed 8 parts and sum them in a fixed order,
    regardless of how many threads compute them.

The same seed and the same number of ducks therefore produce the same network, on 1 core, 16 cores
or in 4 web workers.

### Verification

- **Fingerprint:** `kern_lehrer --fingerprint 400` lets the clean duck walk with velstand for 400
  control ticks (150 ticks forward at 0.15 m/s, then turning at 0.8 rad/s) and prints for each tick
  an FNV-1a hash over the exact bits of trunk position, velocity, angular rate and all joint angles
  and velocities. The reference is in `tests/determinism_fingerprint.txt`. The first differing index
  tells you when two builds diverge.
- **CI** checks it three times: GCC on x86 with sanitizers (test
  `determinism_is_bit_identical_across_builds_and_platforms`), WebAssembly in Node
  (`tests/lehrer_wasm_test.mjs`), Apple clang on ARM (`-mcpu=native`).
- **Workers:** `lehrer_runs_the_same_on_any_number_of_workers` learns with 1, 2 and 4 workers
  (8 ducks in total each time) and compares the networks byte for byte.

### Rule for new code

Everything that feeds the simulation uses `kern::dm::sin` etc. instead of `std::sin`. No
floating-point ordering that depends on the number of threads (sums in a fixed order). No
`-ffast-math` options. How Kern is built is described in [Chapter 2](02-building-and-testing.md).

## 14.8 Learning the play layer

> **Work in progress:** the play layer is currently being reworked. What is described here is the
> code in the working tree.

The play layer (`play.onnx`, [Chapter 13](13-micro-duck.md#1311-the-play-layer-walker_playcpp))
learns like the jump: a demonstration shows the way, the network imitates it from its own sensors
(DAgger), and then it practises (PPO).

### `PlayEnv`

`PlayEnv(seed, randomize)` is a duck on a floor with surface `floor` (friction 1), autonomous and
with Kern extras, whose play layer the Lehrer drives from outside (`set_walker_play_external`).
Underneath the play layer, Pollen's networks run unchanged. One `step` is one decision (2 control
ticks, 40 ms); an episode lasts at most 60 s of robot time.

| | Size |
|---|---|
| Observation (actor) | 23 (`walker_play_observation`, as in the Studio) |
| Critic | 23 + 6 privileged: ball present, true ball position ahead/left; sock present, true sock position ahead/left |
| Action | 9 |
| Actor | 23 → 256 → 128 → 9 |

**Scenes** (`reset`): the duck starts with a random heading (±180°) and ±2° tilt. On the floor there
is a ball with 30 % probability, a sock with 30 %, both with 40 % – close together, as in the
example level:

- Distance: with 20 % 0.12–0.35 m (it starts right in front of it), otherwise 0.35–1.2 m; with 80 %
  within ±0.9 rad in front of the duck (the camera sees it immediately), otherwise in any direction
  (it has to look around).
- If both ball and sock are present, with 40 % the ball lies on the way to the sock (30–80 % of the
  distance, ±12 cm to the side), with 30 % 15–40 cm beside the sock, otherwise independently; never
  closer than 12 cm to the sock and never under the feet.
- Ball: radius 3.5–6 cm, mass 15–50 g, rolling resistance 0.01–0.05 (foam to rubber).
- With 30 % a second sock lies somewhere, at least 30 cm from the first.

**Variation** (if `randomize`): sole friction × 0.7–1.3, trunk mass × 0.95–1.05, centre of mass of
the trunk ±5 mm and of the head ±3 mm per axis, friction and armature per servo within the profile
spread, bus delay 15–30 ms, detector `sigma_abs` 0–4 mm, `sigma_rel` 0–3 %, dropouts 0–20 %.

**Reward** per decision:

| Term | Value |
|---|---|
| Time | −0.01 |
| Progress | +2 × (distance before − distance now) to the true target (the sock first, otherwise the ball; not while holding) |
| Picked up | +3 once, if the sock is still in the beak after the pick |
| Carrying | +0.5 · 0.04 s · min(distance carried, 0.3 m) per decision |
| Kicked | +10 if the ball lies 0.25 m from its start after a kick |
| Put down | +10 if the sock leaves the beak at least 0.25 m from where it was picked up |
| Fall | −5 and end (gravity z above −0.3) |

Kicked or put down ends the episode as a timeout (`timeout`), not as `terminal`. A pick counts as an
attempt when it starts, and as a held grip if the sock is still in the beak at the end.

### The demonstration (`PlayEnv::teacher`)

The demonstration sees **the same as the network** – the 23 observations, no true positions – and
has no hidden memory except the spot where the left foot stood at the start (in addition it knows
the ball radius and the measured bite position). This lets the network learn it exactly: every
answer follows from what it sees now and how long it has been standing. It decides like this:

- **Kick running:** do nothing.
- **Pick running:** beak open from phase 0.04 to 0.30 (`kPlayBeakOpen`, `kPlayBeakClose`), closed
  otherwise.
- **Holding something:** set off (a fixed first step at full power, then half), head slightly up;
  after carrying 0.3 m, open the beak (spit it out).
- **Nothing known:** turn on the spot (0.8), head in the default pose (tilted or swung out, it tips
  over while turning).
- **Target:** the nearer of ball and sock (a ball in the way is kicked away first).
- **Farther than 0.25 m:** look at it (the head pitches more strongly the closer it is, 0.1–0.5 rad,
  and turns at most 0.3 rad towards the target – gently, because bowed far while walking fast and
  turning, the duck tips over), turn at up to 0.6 rad/s on the spot or 0.4 rad/s while walking;
  walk at 0.15–0.3 m/s as long as the target is less than 0.6 rad off. When it turns on the spot,
  the head is in the default pose.
- **Close** (the same for ball and sock): Pollen's gait can only be stopped, not placed in small
  steps, and a swinging foot pushes a sock away. So it walks slowly and steadily towards the target
  along a line (0.12–0.15 m/s; from standstill a fixed nudge at 0.3 m/s, because the gait otherwise
  does not start), steers instead of side-stepping, says "stop" – for the sock by the coasting
  distance of 0.7 cm before the spot, for the ball only 0.5 cm past it – stands, and starts the
  motion once it has stood long enough. **Standing** means here: both soles on the floor for longer
  than 0.25 s (longer than a step's double support lasts; observation `still_s`). Standing at the
  right spot, it goes on standing and starts the motion, whatever its walking command did a moment
  before: a twitch backwards is no reason to back off. If the target is not in the right place (sock: ±8 mm ahead,
  ±12 mm sideways; ball: ±2 cm), it backs off at full speed and approaches again.
- **The target for the sock** is the measured bite position (`bite_spot`, below); for the ball,
  9 cm in front of and 4.2 cm beside the left foot (`microduck_ball_kick_env_cfg.py`), corrected by
  the ball radius and measured from the foot, because the kick is trained from the default pose.
- **Motion request as a ramp:** the request to kick or pick rises from −1 to +1 over the last half
  second of standing still (it is full at 1.5 s of standing still). A ramp instead of a single
  pulse: this way the network can learn it from the standing time.

### What the Lehrer measures itself

Whatever the demonstration does not know by hand, the Lehrer measures in the physics as it is now:

- **Where the beak grips** (`measure_bite`, `kern_lehrer --biss-fenster`, "bite window"): a duck
  walks a short way and stops (the posture after walking, not the one when it appears). Then the sock
  is placed as a heap at one point of a grid in front of it – 11 points sideways (−2 … +3 cm),
  9 ahead (4.5 … 7.7 cm) –, the pick runs with the beak open in the window [0.04, 0.30), and it is
  checked whether the sock came up and whether it is still in the beak after being carried for 3 s.
  The best point is a held one with the most held points in its neighbourhood (±2 sideways, ±1
  ahead), because the approach is never exact.
- **`bite_spot()`** measures this once per process, before the episodes need it, with the beak
  window the network is shown. The grid points run in parallel on the training's threads; natively
  `std::call_once` ensures the single measurement, while WebAssembly runs on one thread and needs no
  mutex for it. `--spielen` prints the result before the first round ("the bite, measured in this
  physics: …"). If nothing holds anywhere, it takes 6.5 cm in front of the trunk,
  centred. As a result **no bite position is hard-coded as a number**: if the physics changes
  (friction, sock model, beak), the Lehrer measures again.
- **How far the duck keeps walking after "stop"** (`measure_stop`, `--halt-messen`, "measure
  stopping"): four runs each from 0.10, 0.12, 0.15 and 0.20 m/s, measured ahead and sideways.

### DAgger for the play layer

`imitate_play(learner, rounds, episodes, seed, parallel)` works as for the jump: in round 0 the
demonstration drives, afterwards the network (plus noise 0.05); each observation carries the
demonstration's answer, and after each round the network learns from everything (30 epochs). Half
the episodes of a round are on the clean robot, half with full variation.

### Practice with PPO (`--spielen-ueben`)

The imitated network then practises on its own (`--spielen-ueben`, "practise playing"):
`make_play_learner`, warm start from the file (weights and frozen normaliser, no new inputs), action
noise 0.1 (`--uebungs-std`, "practice std"), and for the first 20 iterations only the critic
learns. Only what works is rewarded (table above): where the bite really holds and how it really has
to stand, it finds out for itself. Half the environments are clean, half are varied.

### Play-layer results

> Results will be updated after the current training run.

The last published measurement (before the current rework, `docs/lehrer.md`, section 8) was: ball
learned on the clean robot (13 of 14 kicked away), sock learned but with a low success rate (3 of 18
picked up, carried off and spat out); the bottleneck was the grip itself. With full variation,
Pollen's gait more often failed to start on small commands; that was the reason for the practice
phase with PPO.

## 14.9 Jump results

`policies/jump.onnx` was created with recipe `recipe_for("jump")`, 32 ducks on 2 cores, seed 1,
3000 iterations of practice (from `docs/lehrer.md`):

| | clean robot (40 episodes) | full variation (40 episodes) |
|---|---|---|
| real jumps | 40 of 40 | 36 of 40 |
| rise in the air | 11.6 mm | 8.2 mm |
| flight time | 120 ms | 109 ms |
| tilt (largest, mean) | 8° | 9° |
| falls | 0 of 40 | 0 of 40 |

"Full variation" is Pollen's kick recipe at the end of its curricula. The duck does not fall there;
if a jump does not count as real, it is too flat (less than 5 mm of rise).

The way there:

| Step | Duck steps | Result |
|---|---|---|
| Find a demonstration (once, built in) | about 2 million (18 min on 2 cores) | 16 of 20 clean, 0 falls |
| Imitation, rounds 1–7 | 43,355 | the network's first real jump (5 of 5) |
| Imitation, round 8 | 49,936 | 5 of 5 |
| Practice, 3000 iterations | 2.3 million | robust (table above) |

Because the run is identical bit for bit everywhere, the number of duck steps holds for every
device; only the time depends on the device (measured on the same x86 machine with 2 cores):

| Platform | first real jump | imitation done | practice (duck steps/s) |
|---|---|---|---|
| native, 2 threads | 106 s | 133 s | 1431 |
| native, 1 thread | – | – | 763 |
| browser (Chromium, 2 collector workers + learner) | 515 s | 661 s | 391 |
| browser, everything on 1 core | 539 s | 689 s | 336 |

The browser learns the same network (the same 49,936 examples, the same imitation error 0.0108). It
is about 5 times slower during imitation and just under 4 times slower during practice: WebAssembly
without SIMD and without threads inside a worker.

The physics manages about 1300 duck steps/s per core natively (collection only); one step is 4 world
steps with 16 Box3D sub-steps each. The bottleneck is Box3D's joint solver. 8 sub-steps would be 1.6
times faster but measurably change gait and motions, and are therefore not used.

In the Studio: 3 of 3 real jumps, rise 9.0 mm, flight 120 ms, tilt 8°, after which the duck stands
and walks again (test `micro_duck_jumps_for_real_in_the_studio`).

## 14.10 ONNX export and metadata

`export_onnx(actor, normalizer, metadata)` writes the actor in the form of Pollen's exported networks
(IR version 8, opset 17, producer "kern"):

```
obs [1, n] ─ Sub(obs_mean) ─ Div(obs_div) ─ Gemm(w0, b0, transB) ─ Elu(α=1) ─ … ─ Gemm(wL, bL) ─ actions [1, m]
```

`obs_div` is √var + 0.01. The weights are stored as [out][in] for `transB`. Kern reads the file with
`parse_onnx_policy` (test `lehrer_exports_onnx_like_pollens_networks`: to within 10⁻⁴ of the actor
in the Lehrer), and ONNX Runtime, the robot's runtime, loads it as well.

**Metadata** for motion networks (`micro_duck_metadata(task)`), the same keys as Pollen's:

| Key | Value |
|---|---|
| `action_scale` | `1.0` |
| `command_names` | `twist` |
| `observation_names` | `base_ang_vel,projected_gravity,joint_pos,joint_vel,actions,command,head_command,body_command` |
| `joint_names` | the 14 joints in policy order |
| `default_joint_pos` | the home pose |
| `joint_stiffness`, `joint_damping` | 1.0 and 0.0 per joint, respectively |
| `task` | e.g. `jump` |
| `lehrer` | `Kern (C++ PPO on Box3D)` |

The play layer has its own metadata (`play_metadata()`): `task` = `play`, `lehrer` = `Kern (C++
DAgger + PPO on Box3D)`, `rate_hz` = `25` and the names of the 23 observations and 9 actions.

## 14.11 The Lehrer in the browser

![The Lehrer page in the browser](../buch/bilder/lehrer.png)

`kern/build.sh --lehrer` builds `kern_lehrer.wasm`: engine, Box3D, game runtime, `src/lehrer` and
the C interface (`lehrer_api.cpp`). The page `/lehrer/` (`web/lehrer/`) loads it once and gives the
compiled module to every worker.

**Roles:**

- **Collector:** each worker is a module instance with ducks `i · envs … (i + 1) · envs − 1` of a run
  (`kern_lehrer_create(task, envs, seed, first)`). Per iteration it receives the policy
  (`kern_lehrer_set_policy`) and the curricula (`kern_lehrer_progress`) and returns a rollout
  (`kern_lehrer_collect`). During imitation each computes its share of every round
  (`kern_lehrer_dagger`).
- **Learner:** a separate worker (`kern_lehrer_learner`) for imitation (`kern_lehrer_imitate`), PPO
  update (`kern_lehrer_learn`), jump evaluation every 25 iterations (`kern_lehrer_evaluate`, 5
  episodes) and export (`kern_lehrer_export_onnx`).

The buffers are the same as natively (`Policy::serialize`, `Rollout::serialize`); results come back
as pointers with `kern_lehrer_result_length()` bytes. The learner accepts JSON settings that override
the recipe: `learningRate`, `initStd`, `entropyCoef`, `desiredKl`, `steps`, `epochs`, `minibatches`,
`warmStart` (`velstand` | `none`), `imitate`.

**The page** offers motion (jump, or stand as a quick test), workers (default: cores − 1, at most 8),
ducks per worker (default 8), iterations (default 1500), seed, initial action noise and learning
rate (empty = recipe). It shows iteration, duck steps, steps/s, mean episode return and length,
action noise, real jumps, jump height and flight time, and plots return and length over the
iterations. It keeps the screen awake (Wake Lock).

Every 25 iterations and at the end it stores the network for Kern Studio on the device (IndexedDB
"kern-lehrer", store "policies", key `<task>.onnx`, `jump.onnx` for the jump) and announces it via
the BroadcastChannel "kern-lehrer"; open Studio tabs pick it up immediately. ".onnx herunterladen"
("download .onnx") saves the file.

The same seed and the same number of ducks produce the same network as natively. The play layer
cannot be trained in the browser; the page only knows the tasks `jump` and `stand`.

On the phone: Chrome's CPU throttling only slows the main thread, not the workers, and is therefore
no good as a phone profile; the single-core case was measured instead (table in 14.9). On the device
itself, the page shows the duck steps/s.

## 14.12 Tests

| Test | checks |
|---|---|
| `lehrer_mlp_gradient_matches_finite_differences` | backward pass of the MLP |
| `lehrer_adam_fits_a_curve_and_the_normaliser_tracks` | Adam and normaliser |
| `lehrer_ppo_learns_to_reach` | PPO learns a simple task |
| `lehrer_ppo_learns_from_collectors_elsewhere` | collecting and learning separated |
| `lehrer_duck_env_runs_episodes_headless` | velstand holds the duck via the Lehrer path; the same episode bit for bit |
| `micro_duck_observation_between_ticks_is_what_its_controller_sees` | observation between ticks |
| `determinism_is_bit_identical_across_builds_and_platforms` | fingerprint |
| `lehrer_runs_the_same_on_any_number_of_workers` | 1, 2, 4 workers → the same network |
| `lehrer_exports_onnx_like_pollens_networks` | export |
| `micro_duck_jumps_with_a_network_trained_in_kern`, `micro_duck_jumps_for_real_in_the_studio` | jump |
| `lehrer_calibrates_a_servo_from_bench_logs`, `lehrer_replays_robot_logs_against_the_simulation` | calibration ([Chapter 16](16-calibration.md)) |

---

© 2026 Christian Heinrich Hohlfeld · All rights reserved · [Imprint and rights](00-imprint.md)
