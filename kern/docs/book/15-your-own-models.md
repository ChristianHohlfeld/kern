# 15 Training your own models

This chapter is the how-to: train a network with the Lehrer (Kern's trainer), read its progress,
pick the best one, use it in the Studio and add a new task. How the Lehrer computes internally (MLP,
PPO, DAgger, CMA-ES, determinism) is covered in [Chapter 14](14-lehrer.md); the general foundations
of neural networks and reinforcement learning in [Appendix A](A-neural-networks.md). Throughout, the
sections here point to the place in the code and in Chapter 14 that explains what a switch does.

## 15.1 Building the tool

```
kern/tools/lehrer.sh              # builds kern/build/kern_lehrer
kern/tools/lehrer.sh /path/out    # builds /path/out/kern_lehrer
```

The script runs on Linux and macOS, on x86 and ARM. It compiles Box3D, the deterministic libm
(`third_party/musl_math`) and all sources under `src/` except `studio/` and `app/` with
`-O3 -DNDEBUG -ffp-contract=off`, Box3D additionally with `-DBOX3D_DISABLE_SIMD`, and links them with
`tools/kern_lehrer.cpp`. It only rebuilds what is older than its source or the newest header.

| Environment variable | Effect |
|---|---|
| `CC`, `CXX` | compilers (default `cc`, `c++`) |
| `KERN_JOBS` | parallel compilers (default: number of cores) |
| `KERN_NATIVE` | 1 (default): `-march=native` or `-mcpu=native` if the compiler supports it; 0: without |

The CPU switches only select instructions; they do not change any result: the network stays the
same bit for bit as in the browser and in the Studio ([Chapter 14](14-lehrer.md#147-determinism)).
How Kern itself is built is described in [Chapter 2](02-building-and-testing.md).

On startup, `kern_lehrer` loads Pollen's networks from `third_party/microduck/` (or
`--policies DIR`); they are needed for the warm start (velstand), the demonstration and the play
layer (kick, pick).

## 15.2 All commands

| Command | Purpose |
|---|---|
| `--task jump\|stand [options]` | PPO training of a motion (with recipe, Section 15.3) |
| `--eval net.onnx [--episodes 10]` | measure the jump: clean and with full variation |
| `--trace net.onnx\|builtin\|demo.json [--task jump] [--randomized]` | one episode tick by tick: height, contacts, reward terms |
| `--vorbild-suchen demo.json [--iterations 120] [--vorbild start.json]` | search for a jump demonstration with CMA-ES ("search Vorbild") |
| `--vorbild-pruefen builtin\|demo.json [--episodes 10]` | how a demonstration jumps ("check Vorbild") |
| `--spielen [--nachahmen 8] [--episoden 64] [--out play.onnx]` | play layer through imitation (DAgger) ("play", "imitate", "episodes") |
| `--spielen-pruefen vorbild\|play.onnx [--episodes 10]` | how the demonstration or the network plays ("check play") |
| `--spielen-ueben play.onnx [--iterations 300] [--envs N] [--uebungs-std 0.1] [--out play_geuebt.onnx]` | practise the play layer with PPO ("practise play", "practice std") |
| `--spielen-spur [--seed N] [--randomized] [--netz play.onnx]` | one play episode, decision by decision ("play trace", "network") |
| `--biss-fenster` | where the beak grips a sock (grid) ("bite window") |
| `--halt-messen` | how far the duck keeps walking after "stop" ("measure stopping") |
| `--demo-spur [--netz play.onnx] [--seed SECONDS]` | the example level as in the Studio, one line per second ("demo trace") |
| `--fingerprint 400` | determinism fingerprint |
| `--kalibrieren`, `--abgleich`, `--beispiele`, `--profil` | calibration ("calibrate", "match", "examples", "profile"; [Chapter 16](16-calibration.md)) |

General options: `--threads N` (default: all cores), `--seed N` (default 1).

## 15.3 Training the jump

### The standard run

```
kern/build/kern_lehrer --task jump --iterations 3000 \
    --out jump.onnx --log jump.csv --checkpoint run.ckpt --keep-every 250
```

This is how `policies/jump.onnx` was created (32 ducks on 2 cores, seed 1, 3000 iterations;
`docs/lehrer.md`). What happens:

1. **Load the recipe** (`recipe_for("jump")`): warm start from velstand, imitation of the built-in
   demonstration (8 rounds of 96 episodes each), then practice with action noise 0.1, learning rate
   10⁻⁴, imitation reward 10. Command-line options override individual values.
2. **Create the ducks:** `--envs` (default 16 per thread) ducks in `--threads` groups
   (`ThreadBackend`). The number is rounded down to a multiple of the threads
   (`envs = (envs / threads) · threads`).
3. **Imitate** (DAgger, [Section 14.6](14-lehrer.md#146-imitation-demonstrations-and-cma-es)).
   Afterwards the tool writes `jump_nachgeahmt.onnx` ("imitated" – what imitation alone produces)
   and, with `--checkpoint`, the learner to `run.ckpt.nachgeahmt`.
4. **Practise** (PPO, [Section 14.5](14-lehrer.md#145-ppo-step-by-step-ppocpp)): for the first
   30 iterations only the critic learns, then both. Every `--eval-every` (10) iterations the network
   jumps 5 episodes on the clean robot.
5. **Save:** every `--save-every` (25) iterations and at the end, the ONNX to `--out` and the learner
   to `--checkpoint`; every `--keep-every` iterations additionally a snapshot `jump_it00250.onnx`
   and so on.

At the first evaluation with at least 4 of 5 real jumps, `FIRST REAL JUMP at iteration …` appears
and the network is saved immediately. With `--stop-after-jump` the run ends there.

### Resuming

```
kern/build/kern_lehrer --task jump --iterations 6000 --resume run.ckpt --checkpoint run.ckpt --out jump.onnx
```

`--resume` loads the whole learner (networks, normalisers, σ, Adam states, learning rate, iteration)
and skips imitation. Iterations keep counting; `--iterations` is the target, not the number of
additional iterations. The log file is then appended to instead of being created afresh. With
`run.ckpt.nachgeahmt` as `--resume`, practice starts over from the imitation result.

### Without a demonstration

```
kern/build/kern_lehrer --task jump --vorbild none --iterations 3000
```

Pure PPO; the reward then gets the term `track_height` (a reference height over the phase).
`docs/lehrer.md` documents that this route did not find the take-off
([Section 14.6](14-lehrer.md#146-imitation-demonstrations-and-cma-es)). For experiments:
`--vorbild demo.json` uses a demonstration of your own, `--nachahmen 5 --episoden 64` changes the
DAgger rounds, `--uebungs-std 0.2` the noise during practice, and `--nachahmungs-lohn 0` ("imitation
reward") switches the imitation reward off.

### Searching for your own demonstration

```
kern/build/kern_lehrer --vorbild-suchen demo.json --iterations 120
kern/build/kern_lehrer --vorbild-suchen demo2.json --vorbild demo.json --iterations 120   # continue from demo.json
kern/build/kern_lehrer --vorbild-pruefen demo2.json --episodes 20
kern/build/kern_lehrer --task jump --vorbild demo2.json --out jump2.onnx
```

For each generation a line appears with the score and the measurement of the search centre on the
clean robot (height, rise, flight time, tilt, "a real jump" / "no real jump" / "fell"). The built-in
demonstration was created in three stages with a total of 370 generations; the first stage took
about 18 minutes on 2 cores for about 2 million duck steps (`docs/lehrer.md`).

### In the browser

The page `/lehrer/` trains the same recipe with web workers
([Section 14.11](14-lehrer.md#1411-the-lehrer-in-the-browser)). The same seed and the same total
number of ducks (workers × ducks per worker) produce the same network as natively.

## 15.4 Reading progress

Each practice iteration writes one line in this format (numbers shown here as placeholders):

```
it <N>  <seconds> s  return <R>  len <L>  value <V>  std <σ>  lr <lr>  <S> steps/s (collect <c>s, learn <l>s)  | jumps <j>/5 up <h> mm rise <r> mm flight <f> s tilt <t> falls <k>
```

The part from `|` onwards only appears in iterations with an evaluation.

| Field | Meaning | What to watch for |
|---|---|---|
| `return` | mean return of the episodes that ended in this iteration | should rise over hundreds of iterations; fluctuates from iteration to iteration |
| `len` | their mean length in control ticks | jump: 100 = 2 s without a fall; shorter means episodes end with a fall or ground contact |
| `value` | the critic's value loss | high after imitation (the critic does not know the returns yet), should then fall |
| `std` | mean action noise σ | drops as the policy becomes more certain; 0.001 is the lower bound |
| `lr` | learning rate after the KL adjustment | if it falls permanently to 1e-5, the policy changes too much per step |
| `steps/s` | duck steps per second (collecting + learning) | device speed |
| `jumps n/5 …` | evaluation on the clean robot | the actual goal |

Imitation writes per round:

```
  round <r> (<seconds> s): <n> samples, error <mse> | the network: <j> of 5 episodes a real jump, ...
```

`samples` are all ticks labelled so far, `error` the mean squared error per action of the last
epoch.

With `--log jump.csv`, each iteration is written as a CSV row with the columns

```
iteration,steps,seconds,mean_reward,mean_return,mean_length,episodes,value_loss,entropy,mean_std,learning_rate,steps_per_second,jumps,height_mm,flight_s,falls
```

`jumps` is −1 in iterations without an evaluation. The file can be loaded directly into a
spreadsheet or a plotting program.

**When something goes wrong,** `--trace` shows an episode tick by tick:

```
kern/build/kern_lehrer --trace jump_it01000.onnx
kern/build/kern_lehrer --trace builtin                 # the demonstration itself
kern/build/kern_lehrer --trace jump.onnx --randomized  # with full variation
```

Each line: tick, trunk height above the start (mm), which sole has contact, whether another part
touches the ground, upright value, tilt x/y and all non-zero reward terms. This shows which term
carries the return – and whether the policy is exploiting the reward instead of jumping.

## 15.5 Picking the best network

The last file saved is not always the best one. PPO can get worse again after a good state; the
evaluation during the run has only 5 clean episodes.

1. Keep snapshots with `--keep-every N`.
2. Measure each candidate with more episodes, clean **and** varied:

   ```
   for f in jump_it*.onnx; do kern/build/kern_lehrer --eval "$f" --episodes 40; done
   ```

   `--eval` prints two lines: "clean robot" (seed 1000) and "randomised: build, servos, sensors,
   start" (seed 5000, full variation).
3. Choose by: real jumps with variation, then falls (0), then rise, flight time and tilt.
4. Check the candidate in the Studio (Section 15.8).

For `policies/jump.onnx` this measurement gave 40 of 40 clean and 36 of 40 with full variation,
without a fall ([Section 14.9](14-lehrer.md#149-jump-results)).

## 15.6 Choosing hyperparameters

### One iteration in numbers

To see what a switch does, it helps to work through one iteration of the jump recipe with 32 ducks
(all steps in [Section 14.5](14-lehrer.md#145-ppo-step-by-step-ppocpp)):

| Step | Computation |
|---|---|
| Collecting | 32 ducks × 48 ticks = 1536 rows; per row a = μ + σ·ε with σ = 0.1 after imitation |
| Normaliser | the critic normaliser takes in the 1536 rows; the actor normaliser is frozen (warm start) |
| GAE | per duck backwards over 48 ticks with γ = 0.99, λ = 0.95; then advantages normalised to mean 0, std 1 |
| Minibatches | 1536 / 4 = 384 rows per minibatch, each in 8 fixed chunks of 48 rows |
| Updates | 5 epochs × 4 minibatches = 20 Adam steps for actor (with σ) and critic |
| Learning rate | before each of the 20 steps: measure KL; if KL > 0.02 divide by 1.5, if KL < 0.005 multiply by 1.5 |

`--steps` and `--envs` change the number of rows and hence the minibatch size; `--epochs` and
`--minibatches` the number of steps per iteration; `--kl` the size each step should reach at most.
Imitation, by contrast, is supervised: for the jump, 8 rounds of 96 episodes of at most 100 ticks
give at most 76,800 labelled examples; 49,936 were measured, because an episode ends early as soon
as the duck falls or a part other than the soles touches the ground. After each round the actor is
fitted to all examples so far in 10 epochs with minibatches of 256.

### The switches

All PPO values are in the recipe; the command line overrides them individually.

| Option | Field | Jump recipe | Effect (Section 14.5) |
|---|---|---|---|
| `--envs N` | ducks in total | 16 per thread | more data per iteration, less noise in the gradient; **determines the result** |
| `--threads N` | threads | all cores | speed only; the result does not depend on it |
| `--steps N` | `steps_per_env` | 48 | ticks per duck and iteration; rows per iteration = ducks × steps |
| `--epochs N`, `--minibatches N` | | 5, 4 | passes over the data, split into minibatches |
| `--lr X` | `learning_rate` | 10⁻⁴ | initial learning rate; the adaptive rule adjusts it afterwards |
| `--adaptive-lr 0\|1`, `--kl X` | | on, 0.01 | target KL per minibatch |
| `--init-std X` | `init_std` | 0.3 | action noise at the start (before imitation) |
| `--uebungs-std X` | `practice_std` | 0.1 | action noise during practice after imitation |
| `--entropy X` | `entropy_coef` | 0.005 | keeps the noise from dropping too fast |
| `--actor 512,256,128` | `actor_hidden` | 512, 256, 128 | actor network size; with a warm start it must match velstand |
| `--critic 256,128` | `critic_hidden` | 256, 128 | critic network size |
| `--warm-start velstand\|none` | `warm_start` | `velstand` | start from the velstand network |
| `--substeps N` | Box3D sub-steps | 16 | do not change (see below) |
| `--profil servo.json` | servo profile | XL330 | training with calibrated servos ([Chapter 16](16-calibration.md)) |

Notes from the runs in `docs/lehrer.md`:

- **Be careful after imitation:** small learning rate (10⁻⁴) and small noise (0.1). An actor that
  already acts well is easily destroyed by large steps and large noise; hence also "critic first".
- **Larger noise does not help with discovery.** With large noise and no warm start, the duck
  learned to stand, not to jump.
- **Few ducks slow things down.** With only a few dozen ducks the minibatches are small, the
  gradient is noisy, and the KL limit keeps the learning rate down.
- **Do not lower the sub-steps.** 8 instead of 16 sub-steps would be 1.6 times faster but measurably
  change gait and motions. A network that learns with different sub-steps learns a different physics
  from the Studio's.

About `--warm-start`: `make_learner` only evaluates the value `velstand`; any other value starts
with randomly initialised weights. A warm start from a file exists only for the play layer
(`--spielen-ueben`).

## 15.7 Training the play layer

### 1. Measure what the demonstration needs

```
kern/build/kern_lehrer --biss-fenster
kern/build/kern_lehrer --halt-messen
```

`--biss-fenster` places a sock on a grid of 11 × 9 points in front of a standing duck, runs Pollen's
pick with the beak open in the phase window [0.04, 0.30), and prints for each point whether the sock
came up and whether it was still in the beak after being carried for 3 s, plus the best point:

```
sock centre (ahead, left, real m): held at the end of the pick / still held after carrying 3 s
+0.045 -0.020  0 0
...
best: +0.0xx ahead, +0.0xx left (n kept around it)
```

Training itself performs the same measurement (`bite_spot`), once per process before the first
episodes; `--spielen` prints the result. The command is there for checking, for example after
friction, the sock model or the beak has changed.

`--halt-messen` makes the duck stop four times each from 0.10, 0.12, 0.15 and 0.20 m/s and prints
how far it then kept going ahead and sideways.

### 2. Imitate

```
kern/build/kern_lehrer --spielen --nachahmen 12 --episoden 32 --out play.onnx --checkpoint play.ckpt
```

The default is 8 rounds of 64 episodes; `policies/play.onnx` before the rework was created with
12 rounds of 32 episodes of 60 s each (`docs/lehrer.md`). For each round the tool measures the
network in 16 episodes on the clean robot:

```
  round <r> (<seconds> s): <n> samples, error <mse> | the network (clean robot): ball kicked away <k> of <b> (<t> s), sock picked up <p> / carried off and spat out <d> of <s> (<t> s; <h> of <a> picks held), fell <f> of 16
```

**Caution:** `--out` is overwritten after **every** round. If you want to keep an earlier round,
copy the file during the run (for `play.onnx` before the rework, round 10 of 12 was the best).
`--checkpoint` saves the learner after each round.

### 3. Check

```
kern/build/kern_lehrer --spielen-pruefen vorbild --episodes 48
kern/build/kern_lehrer --spielen-pruefen play.onnx --episodes 32
```

Two lines, clean and varied: balls kicked away (and mean time), socks picked up, socks carried off
and spat out (time), how many grips held, falls. Measuring the demonstration shows the most the
network can achieve if it only imitates.

### 4. Watch an episode

```
kern/build/kern_lehrer --spielen-spur --seed 3
kern/build/kern_lehrer --spielen-spur --seed 3 --randomized --netz play.onnx
```

One line per decision (25 Hz): ball and sock as the tracker knows them, and where they really are;
left foot, holding, pick and phase, kick, distance carried; the demonstration's nine actions and,
with `--netz`, the network's next to them (the network then drives); standing still and walking
time; how far the sock was moved, ground contact, which network is driving, state, gravity. At the
end, the result of the episode. Where demonstration and network diverge, you can see it here.

### 5. Practise

```
kern/build/kern_lehrer --spielen-ueben play.onnx --iterations 300 --envs 32 --out play_geuebt.onnx
```

Warm start from `play.onnx`, action noise 0.1 (`--uebungs-std`), 20 iterations of critic only, then
PPO with the play layer's reward ([Section 14.8](14-lehrer.md#148-learning-the-play-layer)). Every
20 iterations it measures the network in 16 clean episodes and writes `play_geuebt_it0020.onnx` and
so on, as well as `--out`.

### 6. Check in the example level

```
kern/build/kern_lehrer --demo-spur --netz play_geuebt.onnx --seed 120
```

`--demo-spur` plays the Studio's Micro Duck level with 60 Hz frames, the way the Studio plays it
(120 s here; for this command `--seed` is the duration in seconds), with `kern/policies/jump.onnx`
and the given play network (otherwise `kern/policies/play.onnx`). Once per second: position, state,
driving network, what the tracker knows about ball and sock, holding, standing still, walking time
and the play network's answer to this observation (`net fwd`, `turn`, `kick`, `pick`). With the environment variable `KERN_JUMP_AT=SECOND` it presses key 7 (jump) in that second.

## 15.8 Using it in the Studio

Kern's own networks have fixed file names: `jump.onnx` and `play.onnx` (`KERN_POLICIES` in
`web/studio/game.mjs`). This is how your own network gets into the Studio:

1. **Into the repository:** copy the file to `kern/policies/jump.onnx` or `kern/policies/play.onnx`.
   The developer page loads Kern's networks from `kern/policies/`, a deployed page from `policies/`
   next to the page. The engine requests missing networks as soon as a duck needs them.
2. **From the browser Lehrer:** the Lehrer page stores its network on the same device (IndexedDB
   "kern-lehrer"). The Studio loads it on startup and picks up every new one immediately, including
   in open tabs. This applies to the jump (the page does not train a play layer).
3. **Check:** open the Micro Duck level, Play, key **7** (jump), or leave the duck alone (play
   layer: Autonomous and Kern extras on, ball or sock in view). The browser console reports
   "Kern: trained network jump.onnx loaded" for a network from the Lehrer, and a warning if a file
   fails to load.

The network must have the shape of an rsl_rl actor (Sub, Div, Gemm/Elu …, Gemm); the Lehrer exports
exactly that. The play layer additionally checks the sizes: a network that does not have 23 inputs
and 9 outputs is not evaluated (the action stays 0).

## 15.9 Adding a new task

A new motion with the layout of Pollen's networks (61 observations → 14 actions) needs four things:
a `Task`, an entry in the recipes, a measurement and the wiring in the Studio. A demonstration is
optional, but often necessary for difficult motions.

### 1. Write the `Task`

In `src/lehrer/tasks.cpp` (or a file of its own under `src/lehrer/`, which `lehrer.sh` compiles
automatically), derive a class from `Task`. A skeleton following the pattern of `JumpTask`:

```cpp
class BowTask : public Task {
 public:
  std::string name() const override { return "bow"; }
  f32 episode_seconds() const override { return 2.0f; }
  i32 privileged() const override { return 4; }      // only the critic sees these

  Start reset(Rng& rng) override {
    Start s;
    s.yaw = rng.uniform(-180, 180);
    s.roll = rng.uniform(-3, 3);
    s.pitch = rng.uniform(-3, 3);
    s.foot_friction = rng.uniform(0.7f, 1.3f);
    return s;
  }
  void command(f32 o[kObs], i32 step) override {      // command slots 48..60
    for (i32 i = 48; i < kObs; ++i) o[i] = 0;
    const f32 a = 6.28318531f * step * kControlDt / episode_seconds();
    o[55] = kern::dm::cos(a);                           // phase in the free slots 55/56
    o[56] = kern::dm::sin(a);
  }
  void privileged(const game::GameWorld::WalkerProbe& p, f32* out) override {
    out[0] = p.position.z; out[1] = p.velocity.z;
    out[2] = p.foot_contact[0] ? 1.0f : 0.0f; out[3] = p.foot_contact[1] ? 1.0f : 0.0f;
  }
  void progress(i32 iteration) override {             // curricula
    fall_ = -2.0f - 18.0f * clamp(iteration / 300.0f, 0.0f, 1.0f);
  }
  f32 reward(const Tick& t, bool& terminal) override {
    Terms r(*this);                                     // weight x term x dt, with names for --trace
    const auto& p = t.probe;
    r.add("upright", 2, upright(p));
    r.add("action_rate", -0.1f, action_rate(t));
    // ... the actual task ...
    terminal = fell(p) || p.body_contact;
    if (terminal) r.add("fall", fall_, 1, false);       // one-off, not x dt
    return r.sum();
  }
 private:
  f32 fall_ = -2;
};
```

`Terms`, `upright`, `pose_match`, `action_rate` and `fell` are helper functions in `tasks.cpp`. A
factory function `make_bow_task()` and its declaration in `env.h` make the class usable from outside.

### 2. Recipe and name

In `src/lehrer/recipe.cpp`:

```cpp
std::unique_ptr<Task> make_task(const std::string& name) {
  if (name == "jump") return make_jump_task();
  if (name == "stand") return make_stand_task();
  if (name == "bow") return make_bow_task();          // new
  return nullptr;
}

Recipe recipe_for(const std::string& task) {
  ...
  if (task == "bow") {
    r.warm_start = "velstand";
    r.ppo.init_std = 0.3f;
    r.ppo.learning_rate = 1e-4f;
  }
  return r;
}
```

With this, `kern_lehrer --task bow` and the browser's C interface know the task; on the Lehrer page
only an `<option>` in `web/lehrer/index.html` is missing. If the task uses observation slots that
velstand has never seen, they must be declared as new inputs for the warm start: in `make_learner`,
Kern currently passes the phase slots 55 and 56 for this (`game::kJumpPhaseSlot` and the next one).

### 3. Measure

Without a hard measurement, no network can be selected. `JumpMeter` and `evaluate_jump` show the
pattern: observe an episode tick by tick (`tick(probe)`), and decide at the end whether it succeeded
(`finish`). The measurement should check what the motion **is**, not what the reward counts (see
pitfalls). `--eval` and the evaluation during the run currently call `evaluate_jump`; a new task
needs an evaluation of its own.

### 4. A demonstration (optional)

If PPO does not find the motion, a demonstration helps, as with the jump:

- a small **parameterisation** (for the jump, 15 numbers: three joints at five points in time, the
  other leg mirrored, targets via `amplitude · tanh`),
- a **score** that CMA-ES can minimise (`score_jump_demo` as the pattern: rewards the motion,
  penalises tilt and falls, on clean and varied robots),
- a **teacher object** with `act(observation, probe, step, action)` (`JumpTeacher`) that has an
  answer for every state – a prerequisite for DAgger.

`search_jump_demo`, `dagger_episodes`, `JumpImitation` and `Recipe::demo` are currently tailored to
`JumpDemo` and `JumpTeacher`. For a new task with a demonstration, you write the corresponding
functions following this pattern; `cmaes_minimise` and `Ppo::imitate` are general.

### 5. Wire it up in the Studio

A new motion runs in the Studio like Pollen's kicks and the jump: a hard switch to its network, and
back after its duration. What is needed:

| Where | What |
|---|---|
| `policy_set()` in `src/game/walker.cpp` | a `SkillDef` like `jump`: `name`, `file`, `duration`, `action_scale`, plus `phase_slot` and `period` if the network sees a phase |
| `PlayInput::robot` and `drive_walkers` | a free bit and `walker_skill(w.actor, "bow")` on press |
| `src/studio/viewport.cpp` | a key (like `Digit7` → 256) |
| `src/game/touch.cpp` | a touch button (like "HOP") |
| `src/game/logic_nodes.cpp` | add the name to the description of **Robot Move**; the node calls `walker_skill` with the name |
| `web/studio/game.mjs` | the file name in `KERN_POLICIES`, so that the Studio loads it from `policies/` |
| `kern/policies/` | the file itself, plus a line in `policies/README.md` |

The observation the Studio gives the network is built by `run_controller` exactly as in training:
head and body slots 0, the phase cos/sin in `phase_slot` and the next slot.

### A task with a different layout

The play layer shows the way for a network that is **not** 61 → 14: a layer of its own in the
runtime (`walker_play.cpp`: observation, execution, external access for the Lehrer), an `Env` class
of its own (`PlayEnv`), its own learner (`make_play_learner`), its own metadata (`play_metadata`)
and its own evaluation (`evaluate_play`). The PPO and imitation machinery (`Ppo`, `Collector`,
`ThreadBackend`, `Ppo::imitate`) is the same; it only knows `Env`.

## 15.10 Designing observation, action and reward

What Kern's networks demonstrate:

- **Only what the robot really has.** The actor sees sensor values (IMU, joints, camera detector,
  mouth encoder), never true positions. The play layer knows ball and sock only from the detector,
  with noise and dropouts. True quantities belong in the critic's privileged observation.
- **Make time visible.** A motion with a sequence gets a phase signal (cos, sin), not a counter. A
  network without memory needs time quantities in its observation: the play layer sees how long it
  has been standing (`still_s`), walking (`walking_s`) and carrying (`carried_s`).
- **Measure instead of command.** In the play layer, "standing still" means "both soles on the
  ground, no motion running" – what the legs are doing, not what was commanded. The gait sometimes
  does not start on small commands; that is standing.
- **Actions like controls.** Walking commands with a dead zone like a stick (below 20 % the duck
  stands), motions as a threshold (> 0.5 starts), limits for head and neck.
- **Reward from named terms.** Each term visible individually (`--trace`), with weight × term × dt.
  Regularisers as in Pollen's: action change, angular rate, self-contact. Curricula via
  `progress(iteration)`: penalties and variation ranges grow as the motion takes shape.
- **Hard episode ends.** A fall and ground contact of other parts end the episode with a penalty;
  otherwise leaning on something becomes the safe bet.
- **The demonstration without hidden knowledge.** If the network is to learn a demonstration
  exactly, the demonstration may only see what the network sees and must not have any memory that
  does not follow from the observation. A request that follows from a duration rises as a ramp
  rather than as a single pulse.

## 15.11 Pitfalls from the real runs

From `docs/lehrer.md`:

- **The reward gets exploited.** In the first PPO runs the duck sat on its tail, did a dive or
  propped itself up. Each time, only a stricter rule helped: ground contact of another part ends the
  episode, tilt costs, the reward only counts when upright.
- **The measurement was too lax.** The first jump measurement (trunk at some point 15 mm above
  standing height, 60 ms without ground contact) let through a variant in which the duck straightened
  up and tucked its feet, tipping by 45°. Since then only a real jump counts: rising in the air above
  the lift-off height, flight, at most 30° tilt, landing and standing.
- **PPO alone does not find rare motions.** Something that needs all servos at their limit at the
  same time practically never emerges from random exploration. A demonstration from CMA-ES plus
  imitation solved it.
- **A good actor and a clueless critic.** Without "critic first", the advantages of a critic that
  does not yet know the returns destroy an imitated actor.
- **Normalisers drift.** With a few dozen environments the normalisers change noticeably between two
  iterations; the old policy is therefore re-evaluated with the current normalisers before the
  update.
- **Bugs in the world, not in the network.** Training the play layer brought three bugs in the world
  to light: the sock crept across the floor by itself (round cloth lumps rolled; now rolling
  resistance), the ball rolled forever (now rolling resistance like rubber on wood), and the idle
  yawn opened the beak during the pick (a running motion no longer counts as standing still). A
  network that fails to learn at one point may be pointing at such a bug; `--spielen-spur` and
  `--trace` reveal it.
- **Hand-entered numbers go stale.** The bite position used to be in the documentation as a measured
  number; now the Lehrer measures it on every run in the current physics (`bite_spot`).
- **Breaking determinism.** One `std::sin` instead of `kern::dm::sin`, a sum in thread order, or
  `-ffast-math` – and the network is a different one on another device
  ([Section 14.7](14-lehrer.md#147-determinism)). `kern_lehrer --fingerprint 400` against
  `tests/determinism_fingerprint.txt` shows it.
- **Different physics from the Studio.** Fewer sub-steps, a different floor, a different servo
  profile: the network then learns a different world. The Lehrer therefore uses the same `GameWorld`
  and the same settings as the Studio.

---

© 2026 Christian Heinrich Hohlfeld · All rights reserved · [Imprint and rights](00-imprint.md)
