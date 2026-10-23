// Vorbild: a demonstration the network learns from (docs/lehrer.md, the jump).
//
// Reinforcement learning alone finds a jump slowly: the push-off needs every leg servo at its limit
// at the same moment, and random exploration around a standing policy almost never does that. So
// the Lehrer teaches in three steps:
//   1. Vorbild suchen: CMA-ES searches, in the simulation, a jump the duck can do - the leg servos'
//      targets at five moments of the move (crouch, push, tuck, land, absorb; left and right
//      mirrored), with Pollen's velstand network keeping the balance before the push and after the
//      landing. Nothing is hand-set: the physics decides what works.
//   2. Nachahmen: the network learns to act like the demonstration (DAgger: it drives itself, the
//      demonstration says what it should have done, it learns from all of that).
//   3. Üben: PPO with the jump's reward makes it the network's own - robust to everything the
//      randomisation varies.
// At run time there is only the network: the demonstration exists only while it learns.
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "kern/game/walker_policy.h"
#include "kern/lehrer/env.h"
#include "kern/lehrer/ppo.h"

namespace kern::lehrer {

struct Recipe;

/// The jump's demonstration: targets of the left leg's hip pitch, knee and ankle (the right one
/// mirrored) at five moments of the jump's phase, as offsets from home through amplitude x tanh.
struct JumpDemo {
  static constexpr i32 kKeys = 5, kJoints = 3;
  std::vector<f32> keys = std::vector<f32>(kKeys * kJoints, 0.0f);
  f32 amplitude = 2.5f;  // radians: the targets' range (beyond the joint limits: the servo pushes at its torque)
};
std::string to_json(const JumpDemo& d);
bool jump_demo_from_json(std::string_view text, JumpDemo& out, std::string* error = nullptr);
/// The demonstration found for the Micro Duck with the XL330 profile (built in).
JumpDemo builtin_jump_demo();

/// Acts as the demonstration: velstand before the push and after the landing, the demonstration's
/// targets in between. It follows the episode through the probes (has it flown, landed).
class JumpTeacher {
 public:
  explicit JumpTeacher(const JumpDemo& demo);
  void reset();
  /// The action for control tick `step` with `observation` (61); `probe` is the state after the last tick.
  void act(const f32* observation, const game::GameWorld::WalkerProbe& probe, i32 step, f32* action);
  bool ok() const { return velstand_ != nullptr; }

 private:
  JumpDemo demo_;
  std::shared_ptr<const game::WalkerPolicy> velstand_;
  f32 z0_ = 0;
  bool have_z0_ = false, flown_ = false, landed_ = false;
};

/// How well a demonstration jumps (higher is better): height and flight time (capped: a safe jump
/// is the goal), upright at touchdown, no fall, standing at the end; over `episodes` (the first on
/// the clean robot, the others with the training's full variation).
f32 score_jump_demo(const JumpDemo& demo, i32 episodes, u64 seed);

/// The demonstration itself on the clean robot (as evaluate_jump measures a network).
JumpReport evaluate_jump_demo(const JumpDemo& demo, i32 episodes, u64 seed, bool randomized = false);

/// Step 1: CMA-ES over the demonstration's keys. `threads` episodes at once.
JumpDemo search_jump_demo(i32 generations, u64 seed, i32 threads,
                          const std::function<void(i32 generation, f32 best_score, const JumpReport& clean)>& progress = {});

/// Step 2: DAgger. Round 0 the teacher drives; later rounds the learner's own actor (with a little
/// noise) drives and the teacher labels every observation. After each round the actor is fitted to
/// everything labelled so far. Episodes come from the recipe's environments (fully randomised).
struct ImitationReport {
  i32 rounds = 0, samples = 0;
  f32 mse = 0;             // of the last fit
  JumpReport student;      // the actor afterwards, on the clean robot (when evaluated)
};
/// The episodes first .. first + count - 1 of DAgger round `round` (of a run seeded with `seed`):
/// observations (61 per tick) and the teacher's actions (14 per tick), episode after episode. Round
/// 0 the teacher drives, later rounds `student`. Anyone can run any share of a round (a thread, a
/// Web Worker): joined in episode order, the shares give exactly the whole round.
void dagger_episodes(const Recipe& recipe, u64 seed, i32 round, i32 first, i32 count, const Policy* student, std::vector<f32>& observations,
                     std::vector<f32>& actions);

class JumpImitation {
 public:
  JumpImitation(const Recipe& recipe, u64 seed);
  ~JumpImitation();
  /// One DAgger round on `learner` (its episodes spread over `parallel`); `evaluate` also measures
  /// the student.
  ImitationReport round(Ppo& learner, const ParallelFor& parallel, bool evaluate);
  /// The same with the round's episodes collected elsewhere (dagger_episodes, joined in order).
  ImitationReport round_from(Ppo& learner, const std::vector<f32>& observations, const std::vector<f32>& actions, const ParallelFor& parallel, bool evaluate);
  i32 rounds_done() const { return round_; }

 private:
  struct Data;
  std::unique_ptr<Data> d_;
  u64 seed_;
  i32 round_ = 0;
};
/// All of the recipe's rounds.
ImitationReport imitate_jump(Ppo& learner, const Recipe& recipe, u64 seed, const ParallelFor& parallel = {},
                             const std::function<void(i32 round, const ImitationReport&)>& progress = {});

}  // namespace kern::lehrer
