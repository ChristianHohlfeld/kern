// Kern's headless trainer: the Micro Duck in the engine's own GameWorld (Box3D, the same servos,
// sensors and 50 Hz controller as in Kern Studio), with a learning policy in place of its networks.
// A Task says what is learned (reset, command, reward, end); the DuckEnv runs episodes of it.
// See docs/training.md.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "kern/core/base.h"
#include "kern/game/runtime.h"

namespace kern::train {

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
  f32 normal();  // standard normal (Box-Muller)
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
  virtual Start reset(Rng& rng) = 0;
  /// Writes the command slots 48..60 of the observation (the unified layout: twist, head, body).
  virtual void command(f32 observation[kObs]) { for (i32 i = 48; i < kObs; ++i) observation[i] = 0; }
  virtual void privileged(const game::GameWorld::WalkerProbe& p, f32* out) { (void)p; (void)out; }
  /// The reward for the last tick; terminal when the episode failed (no value after it).
  virtual f32 reward(const Tick& t, bool& terminal) = 0;
  /// The reward terms of the last call, weight x term x dt each (for the report).
  std::vector<std::pair<const char*, f32>> terms;
};

/// The jump (Kern's own skill, docs/training.md): from a stand, leave the ground with both feet as
/// high as it can, land and stand again.
std::unique_ptr<Task> make_jump_task();
/// Stand still at home (the pipeline's smoke test: quick to learn, easy to check).
std::unique_ptr<Task> make_stand_task();

/// An environment the trainer learns in: reset, then observation / step. The actor sees
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
  DuckEnv(std::unique_ptr<Task> task, u64 seed);
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

 private:
  void observe();
  struct Impl;
  std::unique_ptr<Impl> impl_;
  std::unique_ptr<Task> task_;
  Rng rng_;
  game::ActorId duck_ = 0;
  i32 step_ = 0, max_steps_ = 0;
  f32 return_ = 0;
  f32 previous_action_[kActions] = {}, last_action_[kActions] = {};
  std::vector<f32> obs_, critic_;
  game::GameWorld::WalkerProbe probe_;
};

}  // namespace kern::train
