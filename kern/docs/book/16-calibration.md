# 16 Calibration: from simulation to the real robot

A network that learns in the Lehrer (Kern's trainer) is meant to run on the real Micro Duck. For
that, the simulation has to match the real robot, and the network must not rely on anything the
simulation does not know precisely. Kern pursues three routes to this:

1. **Device models with profiles:** whatever a real device does beyond rigid-body physics (a servo
   with a controller, friction, latency) is a small model with parameters in real units. Profiles are
   fitted to measurements (`--kalibrieren`, "calibrate") and checked against other measurements.
2. **Matching the whole robot** (`--abgleich`, "matching"): a log from the real robot, replayed in
   the simulation; the difference is the remaining gap to the real world.
3. **Domain randomisation:** whatever the measurement cannot pin down, the training varies.

Box3D stays untouched throughout; it computes rigid bodies, joints and contacts
([Chapter 6](06-physics.md)). A fourth principle is added to these: **measure in the physics rather
than hard-code** – quantities that follow from the model and the physics are measured by the Lehrer
itself rather than written into the code as numbers.

> **Safety.** Servos under load can swing fast and hard: clamp the test bench firmly, secure the arm
> and weight, keep hands and cables out of the swing, limit current and keep an emergency stop at
> hand. Never exceed the servo manufacturer's specified voltage. Charge lithium batteries only with a
> suitable charger and under supervision. Test new networks with the robot held or suspended first.
> Networks trained in Kern have not been tested on real hardware. All at your own risk (see the
> [imprint](00-imprint.md)).

| Part | Where | What |
|---|---|---|
| Servo device model | `include/kern/game/servo.h`, `src/game/servo.cpp` | law of the position servo, profile as JSON |
| XL330 profile | `game::xl330_profile()` | Pollen's values (BAM model "m6" at 7.4 V) |
| Test bench | `src/lehrer/kalibrierung.cpp` | a servo with a known load in Box3D, fitting with CMA-ES |
| Matching | `compare_robot_log` | a log of the whole robot against the simulation |
| Tool | `kern_lehrer --kalibrieren / --pruefen / --abgleich / --profil / --beispiele` | |

## 16.1 The servo model

A position servo such as the Dynamixel XL330 is simulated as follows, with the same function
`game::servo_step` in the Studio, in the Lehrer and on the test bench
([Section 13.4](13-micro-duck.md#134-the-servo-model)):

```
torque = clamp(K · (target − angle), −T, T) − D · velocity
       + Coulomb friction F in the joint
       + rotor inertia through the gearbox (armature A) on the joint axis
the command arrives delay seconds after it was sent
```

| Parameter | XL330 (Pollen) | JSON key | Unit |
|---|---|---|---|
| K stiffness | 0.554 | `stiffness` | N·m/rad |
| D damping | 0.053 | `damping` | N·m·s/rad |
| F friction | 0.0048 | `friction` | N·m |
| T maximum torque | 0.963 | `torque` | N·m |
| A armature | 0.0018 | `armature` | kg·m² |
| Delay | 0.02 (training: 0.015–0.030) | `delay` | s |
| Spreads (relative ±) | armature 0.1, friction 0.1, otherwise 0 | `spreadStiffness`, `spreadDamping`, `spreadFriction`, `spreadTorque`, `spreadArmature` | – |
| Delay range | 0.015 … 0.030 | `delayMin`, `delayMax` | s |

The profile as a file (`kern_lehrer --beispiele DIR` writes `xl330.json`; the values are stored
there as float32 in double precision, rounded here):

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

`servo_profile_from_json` checks: `kind` (if present) must be `servo`, every value a number within
its range (gains 0 … 10⁶, armature up to 10³, delay and spreads 0 … 1), stiffness and torque
positive; if `delayMax` is smaller than `delayMin`, it is adjusted. Missing keys stay 0.

A different servo is a different profile. A device that behaves fundamentally differently (a motor
with torque control, a linear actuator) gets a model of its own alongside `servo_step`.

The **spreads** say how certain the values are. From them, the training draws separate values for
each servo and a delay for the bus in each episode (`DuckEnv::reset`):

```cpp
auto spread = [&](f32 value, f32 rel) { return rel > 0 ? value * rng_.uniform(1 - rel, 1 + rel) : value; };
for (i32 link = 1; link <= kActions + 1; ++link) {        // 14 policy servos and the mouth
  game::ServoProfile p = servo;
  p.stiffness = spread(servo.stiffness, servo.spread_stiffness);
  ...
  game::set_micro_duck_servo(l, duck_, link, p);
}
controller->servo_delay = servo.delay_max > servo.delay_min ? rng_.uniform(servo.delay_min, servo.delay_max) : servo.delay;
```

This way the network does not depend on what the measurement could not pin down. A well-measured
servo gets small spreads, a poorly known one large spreads.

## 16.2 Test bench: one servo, measured and fitted

### Measurement protocol

From `docs/kalibrierung.md`:

1. **Setup:** servo firmly clamped, axis horizontal, an arm with a weight attached. Weigh the mass of
   arm and weight (to 0.1 g), measure the distance of the centre of mass from the axis (balance the
   arm on an edge), compute the inertia about its own centre of mass from the shape (small compared
   with m·r² for small weights). Angle 0 = arm hanging straight down.
2. **Operate the servo as in the robot:** same firmware settings (position mode, P/I/D gains,
   profile), same voltage (7.4 V, if the manufacturer allows this voltage for the servo), Return Delay Time 0, same bus rate.
3. **Signals** for each load, with position polling at at least 100 Hz: steps between several angles
   (small and large, e.g. ±0.2 and ±0.9 rad), a chirp from 0.5 → 4 Hz with 0.5 rad amplitude, a
   square wave of ±0.3 rad.
4. **Two or three loads** (e.g. 50 g at 5 cm, 120 g at 6 cm). At least one log per servo is not used
   for fitting (hold-out log).
5. **Measure several servos.** The spread between the individual units becomes the `spread` in the
   profile.

### Log format

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

- Comment lines `# key value` describe the load (`ServoLoad`): mass in kg, distance of the centre of
  mass from the axis in m, inertia about its own centre of mass in kg·m², `horizontal 1` for a
  horizontal axis (gravity loads the arm like a pendulum), otherwise inertia only.
- `time` in seconds (non-decreasing), `target` the target angle sent at the time of sending, `angle`
  the measured angle, both in radians. Lines that do not begin with a number (the header) are
  skipped; separators are comma, space, tab or semicolon.
- At least two rows, positive mass.

### The test bench in Box3D

`simulate_servo(profile, log)` builds the servo with its load in Box3D, in the same environment as
the duck:

- **the same scaling** (scale 4, Froude factors as in [Section 13.3](13-micro-duck.md#133-the-model)),
- **the same step rate** (100 Hz world, 16 sub-steps; one step = 5 ms real),
- **the same joint softness** as the duck's servo joints (constraint 240 Hz, damping ratio 2),
- a fixed base, a link with the mass and centre of mass of the load, inertia of the load plus
  armature as on the duck's links (A about the axis, A/2 across),
- gravity only with a horizontal axis.

It then replays the sent targets the way `GameWorld` feeds them in: the target arriving at time n
was sent `delay` steps earlier and applies until the next sample. The result is the simulated angle
at each point in time of the log; `servo_error` is the root mean square error against the measured
angles.

### Fitting and checking

```
kern_lehrer --kalibrieren steps.csv,chirp.csv --pruefen square.csv --out xl330_unit3.json
kern_lehrer --kalibrieren steps.csv,chirp.csv --profil start.json --iterations 120 --threads 4
```

(`--pruefen` means "check"; `square.csv` is a square-wave log.)

`calibrate_servo(start, fit, holdout, generations, seed, threads)`:

- **Parameters:** six numbers, x = 0 at the start profile (`--profil`, otherwise XL330). The five
  gains K, D, F, T, A as the **logarithm of their ratio** to the start (one unit = a factor of e,
  limited to ±4), the delay in units of 10 ms (0 … 0.2 s). In log space all six coordinates are
  scaled alike, as CMA-ES requires.
- **Search:** CMA-ES ([Section 14.6](14-lehrer.md#146-imitation-demonstrations-and-cma-es)) with
  σ = 0.5, population 12, 80 generations by default (`--iterations` changes this). Cost: RMS error
  over all fitting logs together (each log weighted by its length). The candidates of a generation
  run on `--threads` threads.
- **Result:** the best evaluated point. The profile's `source` says "calibrated on n bench log(s)
  from …".

Output:

```
calibrating xl330 on 2 bench log(s), 1 held out, 4 threads
fit: <before> -> <after> mrad rms;  held out: <before> -> <after> mrad rms (<n> simulations)
  K … N*m/rad  D … N*m*s/rad  F … N*m  T … N*m  armature … kg*m^2  delay … ms
wrote xl330_unit3.json
```

What matters is the **hold-out error** ("held out"). Only it tells you whether the model explains the
servo or has merely memorised the curves. That is why at least one log with a different signal and a
different load belongs in `--pruefen`.

Test `lehrer_calibrates_a_servo_from_bench_logs`: a servo deviates from the nominal profile (K −20 %,
D +30 %, F +50 %, T −10 %, A +20 %, 30 ms delay). Two logs are fitted, a third with a different
signal and a different load is checked. The hold-out error drops to below a fifth and below 3 mrad.

### Example logs

```
kern_lehrer --beispiele beispiele/
```

writes logs in the correct format, generated in the simulation with the XL330 profile:

| File | Contents |
|---|---|
| `servo_steps.csv` | 50 g at 5 cm, horizontal; square wave ±0.5 rad, 0.4 s per level, 2.4 s at 200 Hz |
| `servo_chirp.csv` | 120 g at 6 cm, horizontal; chirp with 0.5 rad amplitude, 2.4 s |
| `robot.csv` | 250 control ticks of the whole robot (Section 16.3) |
| `xl330.json` | the profile |

They show the format and are suitable for running through the tool chain once.

## 16.3 Matching: the whole robot

This assumes a log with what the network saw (61 values) and what it sent (14 actions) on every
control tick (50 Hz), starting still in the default pose. Whether and how the robot's software writes
such a log is not covered in this book.

**Format** (`parse_robot_log`): a header line, then 75 numbers per tick – 61 observations, 14
actions. `robot_log_csv` writes the header `obs0,…,obs60,act0,…,act13`.

```
kern_lehrer --abgleich robot.csv --profil xl330_unit3.json
```

`compare_robot_log(log, servo)`:

- The simulation starts standing on the clean robot (no variation) with the profile's servos and
  receives the actions from the log tick by tick, via the same path as the training (`DuckEnv`,
  external action, scale 1, no low-pass).
- Before each tick it compares what its sensors report with what the log says: gyro (0–2), gravity
  vector (3–5), joint angles (6–19) and joint velocities (20–33).
- Output: RMS per group and the RMS angle error for each of the 14 joints individually:

```
robot.csv against the simulation (xl330), <n> ticks (<s> s):
  gyro … rad/s  gravity …  joints … rad  joint speeds … rad/s (rms)
    left_hip_yaw     … rad
    ...
    right_ankle      … rad
```

That is the remaining gap to the real world. It shrinks with every better profile. If one joint
remains conspicuous, something is missing from the model there, such as gear backlash.

Test `lehrer_replays_robot_logs_against_the_simulation`: a log from the simulation
(`simulated_robot_log`, 120 ticks in the test: velstand stands for 50 ticks and then walks forward at
0.15 m/s; from tick 150 it additionally turns at 0.6 rad/s), replayed with the same servos, yields a
gap of **0** – bit for bit, because the simulation is deterministic
([Section 14.7](14-lehrer.md#147-determinism)). With different servos it yields a measurable gap.

Because the simulation is deterministic on all platforms checked in CI, a difference from a real log is a real
model error and not simulation noise.

## 16.4 Training with the calibrated profile

```
kern_lehrer --task jump --profil xl330_unit3.json --out jump.onnx
```

The ducks in training get the profile's servos with its spreads (`Recipe::servo`,
`DuckEnv::servo`). On the clean robot (evaluation, `randomize = false`) the profile applies exactly
to all servos.

Note: the play layer (`PlayEnv`) currently always builds its servos from `xl330_profile()` and varies
friction and armature within its spreads; `--profil` affects the PPO training of the motions
(`--task`).

## 16.5 Domain randomisation

Whatever is not measured or varies, the training varies. The complete ranges are listed in
[Section 14.3](14-lehrer.md#143-the-ducks-environment); here they are ordered by their role for
sim-to-real:

| Uncertainty | How the training covers it |
|---|---|
| Individual servo units | separate values per servo within the profile spread (XL330: friction, armature ±10 %) |
| Bus latency | 15–30 ms per episode |
| Build (battery, cables, print) | trunk mass and inertia ±5 %, centre of mass of trunk and head (jump: curriculum up to ±15 / ±10 mm) |
| Floor | sole friction × 0.7–1.3 |
| IMU | noise (gyro ±0.03, gravity ±0.01), 0–1 tick delay |
| Encoders | noise ±0.001 rad, offset ±0.015 rad per episode and joint, velocity ±0.25 |
| Disturbances | pushes every 3–6 s up to ±0.3 m/s (from iteration 300) |
| Camera detector (play layer) | position noise 0–4 mm + 0–3 % of the distance, 0–20 % dropouts |
| Toys (play layer) | ball radius 3.5–6 cm, mass 15–50 g, rolling resistance 0.01–0.05 |

The critic always sees the clean state; only the actor, which goes onto the robot, sees the noisy
sensors (asymmetric critic, [Section 14.3](14-lehrer.md#143-the-ducks-environment)).

Gear backlash is not in the model. Pollen's training randomises it (the MJCF variants
`robot_*_backlash.xml`), and velstand is robust to it; Kern's own networks learn without it.

## 16.6 Measuring in the physics instead of hard-coding

Some quantities follow from the model and the physics. If you wrote them into the code as numbers,
they would no longer be correct after the next change to friction, model or servo. Kern therefore
measures them in the simulation, just as you would measure them on the real robot:

- **The bite position** (`bite_spot`, `measure_bite`): where must a sock lie in front of the duck
  for Pollen's pick to grab it and keep hold while carrying it? The Lehrer places the sock on a grid
  of 99 points in front of a duck that has just walked and is standing, runs the pick with the beak
  window the network is shown, and takes the point that, together with its neighbours, holds most
  reliably. This happens once per process before the first training; no bite position is in the code
  as a number ([Section 14.8](14-lehrer.md#148-learning-the-play-layer)).
- **The coasting after "stop"** (`--halt-messen`): how far the duck keeps walking from a slow gait.
  Here the rule is not yet fully implemented: the play layer's demonstration enters the value
  measured with `--halt-messen` as a constant (`kCoastM`, 0.7 cm from 0.12 m/s); after a change in
  the physics, you measure again.
- **The mouth encoder** as a grip sensor: "holding something" is not hard-coded but is whatever the
  servo reports – set to "closed" and still more than 2° open.
- **Standing still** in the play layer: both soles on the ground, measured via the contacts, instead
  of "no walking command".

The **pair friction** of the surfaces (TPU beak pad on cotton 0.8, cotton on cotton 0.5, cotton on
wooden floor 0.4), on the other hand, is hard-coded: static friction values of dry material pairs,
justified in the code with their typical ranges ([Section 13.3](13-micro-duck.md#133-the-model)).
They are candidates for a measurement on the real material.

## 16.7 Limits and next steps

- **No real measurement data yet.** Everything above has been checked with logs from the simulation.
  As soon as a test bench measures, the same tool applies to real logs.
- **Gear backlash** is missing from the servo model. Matching shows whether it is missing; then
  `servo_step` gets a backlash range.
- **Armature** is a disc that turns with the link, not a geared rotor; if the parent link rotates
  about the same axis, the inertia is slightly too large
  ([Section 13.3](13-micro-duck.md#133-the-model)).
- **Known gap:** in Kern, `kick_right` sometimes tips over after the kick. Candidates: backlash,
  contact softness, foot friction – a case for matching against a robot log.
- **IMU:** noise and latency are part of the training variation (`Task::Noise`). A dedicated IMU
  profile will follow if a log shows that it is needed.
- **Friction mixing:** Box3D mixes surfaces without a dedicated pair as the geometric mean, MuJoCo
  takes the sole's. In the example level floor and sole are both 1.0, so the mix is the same.
- **Camera detector:** the model (noise, dropouts) is a device model without measurements on the
  real detector; its ranges are assumptions of the training.
- **Scaling:** the four-times-larger duck is the real robot in different units; all profiles and logs
  are in real units, and the conversion only happens at the Box3D boundary.

---

© 2026 Christian Heinrich Hohlfeld · All rights reserved · [Imprint and rights](00-imprint.md)
