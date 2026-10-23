// The training recipe of each task, shared by every place that trains (kern_lehrer natively, the
// browser's workers through lehrer_api.cpp): the same tasks, environments, seeds, curricula, PPO
// settings and warm start everywhere, so the same run gives the same network everywhere
// (docs/lehrer.md, determinism).
#pragma once

#include <memory>
#include <string>

#include "kern/lehrer/env.h"
#include "kern/lehrer/ppo.h"
#include "kern/lehrer/vorbild.h"

namespace kern::lehrer {

struct Recipe {
  std::string task;
  PpoConfig ppo;
  std::string warm_start = "none";  // "velstand": the actor starts as Pollen's velstand network
  i32 substeps = 0;                 // Box3D sub-steps (0: the Studio's, game::kWalkerSubsteps)
  /// The servos the ducks are built with (a calibrated profile makes the training's robot the real one).
  game::ServoProfile servo = game::xl330_profile();
  /// Before the practice (PPO): imitation of a demonstration (vorbild.h), `imitation_rounds` DAgger
  /// rounds of `imitation_episodes` episodes; then the action noise is `practice_std`.
  bool imitate = false;
  JumpDemo demo;
  i32 imitation_rounds = 5, imitation_episodes = 64;
  f32 practice_std = 0.2f;
  /// While practising, the weight of the reward for acting as the demonstration would (0: none).
  f32 imitate_reward = 0;
  /// After imitation, iterations in which only the critic learns (the actor waits for it).
  i32 critic_warmup = 30;
  /// The shoves' strength over the training (Pollen's kick: in once the move has formed).
  f32 push_scale(i32 iteration) const;
};

/// "jump" or "stand"; nullptr for anything else.
std::unique_ptr<Task> make_task(const std::string& name);
/// The recipe's task (with its demonstration's imitation reward when it has one).
std::unique_ptr<Task> make_task(const Recipe& recipe);
/// The recipe for `task` (its defaults; callers may change them).
Recipe recipe_for(const std::string& task);
/// Environment number `index` of a run seeded with `seed`: its seed depends on (seed, index) only.
std::unique_ptr<DuckEnv> make_env(const Recipe& recipe, u64 seed, i32 index);
/// Before iteration `iteration`: the task's curricula and the shoves.
void prepare_env(DuckEnv& env, const Recipe& recipe, i32 iteration);
/// A learner for `recipe`, warm-started as it says. nullptr when the warm start is not possible.
std::unique_ptr<Ppo> make_learner(const Recipe& recipe, u64 seed);
/// After imitation: the practice's action noise, the actor waits for the critic (critic_warmup).
void begin_practice(Ppo& learner, const Recipe& recipe);
/// Before each PPO iteration: the actor joins in once the critic has warmed up.
void before_learning(Ppo& learner, const Recipe& recipe);

}  // namespace kern::lehrer
