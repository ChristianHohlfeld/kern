// Kern's headless Lehrer: the Micro Duck in the engine's own GameWorld (Box3D, the same servos,
// sensors and 50 Hz controller as in Kern Studio), with a learning policy in place of its networks.
// A Task says what is learned (reset, command, reward, end); the DuckEnv runs episodes of it.
// See docs/lehrer.md.
#pragma once

#include "kern/core/dmath.h"
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "kern/core/base.h"
#include "kern/game/runtime.h"
#include "kern/game/servo.h"
#include "kern/game/walker.h"

namespace kern::lehrer {

/// Small, fast and reproducible (splitmix64): every environment has its own stream.
struct Rng {
  u64 state = 1;
  explicit Rng(u64 seed = 1) : state(seed) {}
  u64 next() {
    u64 z = (state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
  }
  f32 uniform() { return static_cast<f32>(next() >> 40) * (1.0f / 16777216.0f); }  // [0, 1)
  f32 uniform(f32 lo, f32 hi) { return lo + (hi - lo) * uniform(); }
  f32 normal() {  // standard normal (Box-Muller)
    const f32 u1 = uniform(), u2 = uniform();
    return std::sqrt(-2.0f * kern::dm::log(u1 > 1e-7f ? u1 : 1e-7f)) * kern::dm::cos(6.28318531f * u2);
  }
};

constexpr i32 kObs = 61, kActions = 14;
constexpr f32 kControlDt = 0.02f;  // real seconds per control tick (50 Hz)

/// One control tick as the task sees it: the state after the last action, in real units.
struct Tick {
  const game::GameWorld::WalkerProbe& probe;
  const f32* action;           // the action just taken
  const f32* previous_action;  // the one before it (action rate)
  i32 step;                    // control ticks since the episode began (1 after the first action)
};

/// What the robot learns. Rewards follow mjlab's convention: the sum of weight x term x dt.
class Task {
 public:
  virtual ~Task() = default;
  virtual std::string name() const = 0;
  /// Extra observations only the critic sees (asymmetric actor-critic), appended to the 61.
  virtual i32 privileged() const { return 0; }
  virtual f32 episode_seconds() const = 0;
  /// The duck's start and the episode's randomisation.
  struct Start {
    f32 yaw = 0, roll = 0, pitch = 0;  // degrees
    f32 dz = 0;                        // real metres above the standing pose
    f32 foot_friction = 1;             // the soles' friction scale
  };
  /// Randomisation of the episode (matched to Pollen's velocity / kick recipes): noise on what the
  /// actor observes, an encoder bias per joint, shoves at random intervals.
  struct Noise {
    f32 gyro = 0.03f, gravity = 0.01f, joint_pos = 0.001f, joint_vel = 0.25f;  // uniform +-
    f32 encoder_bias = 0.015f;          // per joint and episode, uniform +-
    f32 push_every_min = 3, push_every_max = 6;  // real seconds
    f32 push = 0.3f;                    // real m/s, x and y uniform +-
    // The build, per episode (game::vary_micro_duck): trunk mass and inertia, the trunk's and the
    // head's centre of mass (real metres, uniform +- per axis; the curricula widen them). The
    // servos vary within their device profile's spreads (DuckEnv::servo).
    f32 mass_min = 0.95f, mass_max = 1.05f;
    f32 com = 0.003f, head_com = 0.003f;
    // The IMU (gyro, gravity) arrives 0..imu_lag ticks late, re-drawn every imu_lag_period ticks.
    i32 imu_lag = 1, imu_lag_period = 64;
  };
  Noise noise;
  bool randomize = true;  // off: the clean robot (evaluation)
  virtual Start reset(Rng& rng) = 0;
  /// Writes the command slots 48..60 of the observation (the unified layout: twist, head, body) for
  /// control tick `step` of the episode (0 = its first).
  virtual void command(f32 observation[kObs], i32 step) { (void)step; for (i32 i = 48; i < kObs; ++i) observation[i] = 0; }
  virtual void privileged(const game::GameWorld::WalkerProbe& p, f32* out) { (void)p; (void)out; }
  /// Before each action: the clean observation (61, with this task's commands), the state and the
  /// tick (a task that rewards imitation asks its teacher here).
  virtual void before_action(const f32* observation, const game::GameWorld::WalkerProbe& probe, i32 step) { (void)observation; (void)probe; (void)step; }
  /// The reward for the last tick; terminal when the episode failed (no value after it).
  virtual f32 reward(const Tick& t, bool& terminal) = 0;
  /// The training's progress (PPO iterations so far): curricula ramp weights and ranges with it.
  virtual void progress(i32 iteration) { (void)iteration; }
  /// The reward terms of the last call, weight x term x dt each (for the report).
  std::vector<std::pair<const char*, f32>> terms;
};

/// The jump (Kern's own skill, docs/lehrer.md): from a stand, leave the ground with both feet as
/// high as it can, land and stand again.
struct JumpDemo;
/// With `demo` and `imitate` > 0 the reward also has the term "imitate": imitate x how close the
/// action is to what the demonstration's teacher would do (vorbild.h; while practising).
std::unique_ptr<Task> make_jump_task(const JumpDemo* demo = nullptr, f32 imitate = 0);
/// Stand still at home (the pipeline's smoke test: quick to learn, easy to check).
std::unique_ptr<Task> make_stand_task();

/// An environment the Lehrer learns in: reset, then observation / step. The actor sees
/// observation() (obs_size), the critic critic() (critic_size, the actor's first).
class Env {
 public:
  virtual ~Env() = default;
  virtual i32 obs_size() const = 0;
  virtual i32 critic_size() const = 0;
  virtual i32 action_size() const = 0;
  virtual void reset() = 0;
  virtual const f32* observation() const = 0;
  virtual const f32* critic() const = 0;
  struct Step { f32 reward = 0; bool terminal = false, timeout = false; };
  /// One step with `action`. After a terminal or timeout step the observations show the final
  /// state; the caller resets.
  virtual Step step(const f32* action) = 0;
};

/// One duck on a flat floor in its own GameWorld (no GPU: the renderer stays headless), driven tick
/// by tick from outside: reset, then observation / step.
class DuckEnv : public Env {
 public:
  /// substeps: Box3D sub-steps per fixed step (the Studio runs game::kWalkerSubsteps; a Lehrer
  /// may run fewer for speed, docs/lehrer.md measures what that changes).
  DuckEnv(std::unique_ptr<Task> task, u64 seed, i32 substeps = 0);
  ~DuckEnv();
  DuckEnv(const DuckEnv&) = delete;
  DuckEnv& operator=(const DuckEnv&) = delete;
  Task& task() { return *task_; }
  i32 obs_size() const override { return kObs; }
  i32 critic_size() const override { return kObs + task_->privileged(); }
  i32 action_size() const override { return kActions; }
  void reset() override;
  /// The actor's observation (61) and the critic's (61 + privileged), at the coming tick.
  const f32* observation() const override { return obs_.data(); }
  const f32* critic() const override { return critic_.data(); }
  /// One control tick (50 Hz real) with `action`.
  Step step(const f32* action) override;
  i32 steps() const { return step_; }
  f32 episode_return() const { return return_; }
  game::GameWorld& world();
  game::ActorId duck() const { return duck_; }
  const game::GameWorld::WalkerProbe& probe() const { return probe_; }
  /// The shoves' strength as a share of Task::Noise::push (the curriculum ramps it in).
  f32 push_scale = 1;
  /// The servos' device profile (game/servo.h): the clean robot has exactly these values; with
  /// randomisation every servo draws its own within the profile's spreads, and the bus its delay,
  /// each episode.
  game::ServoProfile servo = game::xl330_profile();

 private:
  void observe();
  i32 push_interval();
  struct Impl;
  std::unique_ptr<Impl> impl_;
  std::unique_ptr<Task> task_;
  Rng rng_;
  game::ActorId duck_ = 0;
  i32 step_ = 0, max_steps_ = 0;
  f32 return_ = 0;
  f32 previous_action_[kActions] = {}, last_action_[kActions] = {};
  f32 bias_[kActions] = {};
  i32 next_push_ = 0;
  f32 imu_[6] = {};            // the IMU part of the last observation (for the delay)
  i32 imu_lag_ = 0, imu_lag_left_ = 0;
  std::vector<f32> obs_, critic_;
  game::GameWorld::WalkerProbe probe_;
};

}  // namespace kern::lehrer

namespace kern::lehrer {

/// How a policy jumps (Kern's jump task), in the Studio's physics. Per episode: the longest flight
/// (nothing of the duck on the floor), how far the trunk rose while in the air above where it was
/// when the feet left (a jump launches the body; lifting the feet is no jump), the trunk's highest
/// point in the air above its standing height, the most it tilted, whether it fell (or touched the
/// floor with anything but its soles) and whether it stands upright on both soles at the end.
/// A jump counts as real when it flew at least 60 ms, rose at least 5 mm in the air, never tilted
/// more than 30 degrees, never fell and stands at the end.
struct JumpReport {
  i32 episodes = 0, jumps = 0, falls = 0;
  f32 height_mm = 0, rise_mm = 0, flight_s = 0, tilt_deg = 0;  // means over the episodes
  f32 best_height_mm = 0, best_flight_s = 0;
  f32 success() const { return episodes ? static_cast<f32>(jumps) / static_cast<f32>(episodes) : 0.0f; }
};
/// Watches one episode tick by tick (JumpReport's measures).
struct JumpMeter {
  f32 z0 = 0, ground_z = 0, liftoff_z = 0, flight = 0, longest = 0, rise = 0, air_apex = -1, min_up = 1;
  bool started = false, in_air = false;
  i32 ticks = 0;
  void tick(const game::GameWorld::WalkerProbe& p);
  /// Adds the episode to `rep`; true when it was a real jump.
  bool finish(bool fell, const game::GameWorld::WalkerProbe& last, JumpReport& rep) const;
};
/// Divides the sums of finish() into means.
void finish_report(JumpReport& rep);
using ActFn = void (*)(void* user, const f32* observation, f32* action);
/// `randomized`: the training's full variation (build, servos, sensors, start) instead of the clean robot.
JumpReport evaluate_jump(ActFn act, void* user, i32 episodes, u64 seed, i32 substeps = 0, bool randomized = false);

}  // namespace kern::lehrer

namespace kern::lehrer {

/// A reproducibility fingerprint: Pollen's velstand network drives the clean duck (no randomisation)
/// for `ticks` control ticks in the Studio's physics, walking forward then turning; out[t] is a
/// hash over the exact bits of the robot's state after tick t (trunk pose and velocity, every joint
/// angle and speed). Two builds or platforms are bit-identical exactly when the lists are equal; the
/// first differing index says when they part (docs/lehrer.md, determinism).
std::vector<u64> determinism_fingerprint(i32 ticks);

}  // namespace kern::lehrer
