// Spielen: Plumo learns to play - see a ball and kick it, see a sock, pick it up, carry it
// off and spit it out (docs/lehrer.md). The network is the play layer's (game/walker_play.cpp,
// play.onnx): 23 observations (the camera's detector, the left foot, the beak, the move running,
// the carry, gravity, how long it has stood still or walked) -> 9 actions (walk, head, beak, kick, pick, neck), 25 Hz,
// on top of Pollen's networks for the moves.
//
// It learns like the jump: a demonstration (a teacher that knows where things really are) shows the
// way, the network imitates it from what its own sensors give (DAgger), then practises. At run time
// there is only the network.
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "kern/game/runtime.h"
#include "kern/lehrer/env.h"
#include "kern/lehrer/ppo.h"

namespace kern::lehrer {

constexpr i32 kPlayObs = game::GameWorld::kPlayObs, kPlayActions = game::GameWorld::kPlayActions;
/// Where in the ground pick's phase the demonstration has the beak open (measured: --biss-fenster).
constexpr f32 kPlayBeakOpen = 0.04f, kPlayBeakClose = 0.30f;

/// One duck on a floor with a ball, a sock or both (random places, sizes, its heading), driven
/// through the play layer. An episode ends when the ball was kicked away or a sock was carried off
/// and put down (success), when the duck falls, or after 60 s.
/// Where the sock should lie for the beak's bite (measure_bite): ahead / left of the duck, real metres;
/// kept: how many measured places around it held the sock.
struct BiteSpot { f32 ahead = 0, left = 0; i32 kept = 0; };

class PlayEnv : public Env {
 public:
  PlayEnv(u64 seed, bool randomize);
  ~PlayEnv();
  PlayEnv(const PlayEnv&) = delete;
  PlayEnv& operator=(const PlayEnv&) = delete;
  i32 obs_size() const override { return kPlayObs; }
  i32 critic_size() const override { return kPlayObs + kPrivileged; }
  i32 action_size() const override { return kPlayActions; }
  void reset() override;
  const f32* observation() const override { return obs_.data(); }
  const f32* critic() const override { return critic_.data(); }
  Step step(const f32* action) override;
  /// What the demonstration does now (it knows where the ball and the sock really are).
  void teacher(f32 action[kPlayActions]);
  struct Outcome {
    bool has_ball = false, has_sock = false;
    bool kicked = false, picked = false, delivered = false, fell = false;
    i32 pick_attempts = 0, picks_held = 0;  // picks started; the sock still in the beak when one ended
    f32 seconds = 0;  // real seconds until the episode ended
  };
  const Outcome& outcome() const { return outcome_; }
  /// How far the sock's centre is from where it lay at the start (real metres; 0 without a sock).
  f32 sock_moved() const;
  /// The world and the duck (for traces).
  const game::GameWorld& world() const;
  game::ActorId duck() const;
  static constexpr i32 kPrivileged = 6;
  /// For measuring where the beak gets a sock: the next reset puts only a sock there (part 0 at
  /// `ahead`, `left` real metres from the trunk, heading 0), no randomisation of the scene.
  void place_sock_next(f32 ahead, f32 left);

 private:
  friend BiteSpot measure_bite(f32, f32, bool, const ParallelFor&);
  friend void measure_stop();
  void observe();
  struct Impl;
  std::unique_ptr<Impl> impl_;
  Rng rng_;
  bool randomize_;
  std::vector<f32> obs_, critic_;
  Outcome outcome_;
  i32 decisions_ = 0;
};

/// The play network's ONNX metadata (what its inputs and outputs are).
std::vector<std::pair<std::string, std::string>> play_metadata();

/// How a play policy (or the demonstration: act == nullptr) does: per kind of episode, how often
/// it succeeded and how long it took.
struct PlayReport {
  i32 episodes = 0, ball_episodes = 0, kicked = 0, sock_episodes = 0, picked = 0, delivered = 0, falls = 0;
  i32 pick_attempts = 0, picks_held = 0;
  f32 ball_seconds = 0, sock_seconds = 0;  // mean time of the successes
};
using PlayActFn = void (*)(void* user, const f32* observation, f32* action);
PlayReport evaluate_play(PlayActFn act, void* user, i32 episodes, u64 seed, bool randomized, const ParallelFor& parallel = {});

/// The play network's learner: actor 23 -> 256 -> 128 -> 9.
std::unique_ptr<Ppo> make_play_learner(u64 seed);

/// DAgger for the play network: round 0 the demonstration drives, later rounds the network itself;
/// every observation is labelled with the demonstration's action; after each round the actor is
/// fitted to all of it. Episodes of a round spread over `parallel`.
struct PlayImitationReport {
  i32 rounds = 0, samples = 0;
  f32 mse = 0;
  PlayReport student;  // the network afterwards (when evaluated)
};
/// Where the beak gets a sock (kern_lehrer --biss-fenster): a grid of sock places in front of a
/// duck that has walked and stands, the pick run with the beak open over [open, close) of its phase;
/// per place whether the sock came up and whether it stayed in the beak while carried off.
/// Prints the grid and the place it finds best; returns it.
BiteSpot measure_bite(f32 open, f32 close, bool print = true, const ParallelFor& parallel = {});
/// The place the demonstration aims the sock at: measured once per process (measure_bite with the
/// beak window the network is shown), in the physics as it is now - no number written down.
BiteSpot bite_spot(const ParallelFor& parallel = {});
/// How far the duck still goes when it is told to stop from a slow walk (kern_lehrer --halt-messen).
void measure_stop();

PlayImitationReport imitate_play(Ppo& learner, i32 rounds, i32 episodes, u64 seed, const ParallelFor& parallel = {},
                                 const std::function<void(i32 round, const PlayImitationReport&)>& progress = {});

}  // namespace kern::lehrer
