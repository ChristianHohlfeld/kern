// Proximal Policy Optimization in C++, following rsl_rl's PPO (the recipe Pollen trains the Micro
// Duck's networks with): Gaussian actor with a learnable std, asymmetric critic, running observation
// normalisers, GAE, clipped surrogate and value losses, entropy bonus, adaptive learning rate from
// the KL divergence, gradient clipping per network, Adam.
//
// Collecting and learning are separate so that they can run in different places: a Collector only
// needs the Policy (actor, std, normaliser) and returns a Rollout; the learner (Ppo) takes the
// rollouts of all collectors (threads here, Web Workers in the browser), updates, and hands out the
// next Policy. Both serialise to flat buffers.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "kern/train/env.h"
#include "kern/train/nn.h"

namespace kern::train {

struct PpoConfig {
  std::vector<i32> actor_hidden{512, 256, 128}, critic_hidden{512, 256, 128};
  f32 init_std = 1.0f;
  i32 steps_per_env = 24;
  i32 epochs = 5, minibatches = 4;
  f32 clip = 0.2f, gamma = 0.99f, lam = 0.95f;
  f32 value_coef = 1.0f, entropy_coef = 0.01f;
  f32 learning_rate = 1e-3f, desired_kl = 0.01f, max_grad_norm = 1.0f;
  bool adaptive_lr = true, clipped_value_loss = true;
  /// A warm-started actor keeps the normaliser its weights were trained with (see warm_start).
  bool freeze_actor_normalizer = false;
  /// Threads the update's minibatches are cut across (the ParallelFor of iterate / set_parallel).
  i32 learn_threads = 1;
  u64 seed = 1;
};

/// Runs fn(0..n-1); a parallel runner spreads the calls over threads (each index on one thread).
using ParallelFor = std::function<void(i32 n, const std::function<void(i32)>& fn)>;
/// One worker per hardware thread (std::thread); sequential where there are no threads.
ParallelFor thread_pool(i32 threads = 0);

/// What acting needs: the actor network, its observation normaliser and the action noise.
struct Policy {
  Mlp actor;
  Normalizer normalizer;
  std::vector<f32> stddev;
  /// The mean action (what the exported network computes).
  void act(const f32* observation, f32* action) const;
  std::vector<f32> serialize() const;
  bool deserialize(const f32* data, std::size_t size);
};

/// steps x envs transitions, [step][env] rows, plus where each environment stands at the end.
struct Rollout {
  i32 envs = 0, steps = 0, obs = 0, critic = 0, actions = 0;
  std::vector<f32> observation, critic_obs, action, reward;
  std::vector<u8> terminal, timeout;
  std::vector<f32> last_critic;
  std::vector<f32> episode_returns;  // episodes that ended during the rollout
  std::vector<i32> episode_lengths;
  f64 seconds = 0;
  std::vector<u8> serialize() const;
  bool deserialize(const u8* data, std::size_t size);
};

/// Steps environments with a Policy's sampled actions.
class Collector {
 public:
  explicit Collector(u64 seed = 1) : rng_(seed) {}
  /// `steps` steps of every environment (finished episodes reset); the environments must have been
  /// reset once before the first call.
  void collect(std::vector<Env*>& envs, i32 steps, const Policy& policy, Rollout& out, const ParallelFor& parallel = {});

 private:
  Rng rng_;
  std::vector<Rng> env_rng_;
  std::vector<f32> episode_return_;
  std::vector<i32> episode_length_;
};

struct IterationStats {
  i32 iteration = 0;
  f32 mean_reward = 0;          // per step, over the rollouts
  f32 mean_return = 0;          // of the episodes that ended in them (0 when none did)
  f32 mean_length = 0;          // their length in steps
  i32 episodes = 0;
  f32 value_loss = 0, surrogate_loss = 0, entropy = 0, learning_rate = 0, mean_std = 0;
  f64 collect_seconds = 0, learn_seconds = 0;
  i64 steps = 0;                // total environment steps so far
};

class Ppo {
 public:
  Ppo(const PpoConfig& config, i32 obs_size, i32 critic_size, i32 action_size);
  /// Collect (steps_per_env steps of every environment here, in parallel) and learn.
  IterationStats iterate(std::vector<Env*>& envs, const ParallelFor& parallel = {});
  /// Learn from rollouts collected elsewhere with the current policy().
  IterationStats learn(const std::vector<const Rollout*>& rollouts);
  void set_parallel(const ParallelFor& parallel) { parallel_ = parallel; }
  /// Starts the actor from a trained network of the same shape (Pollen's velstand, say): its
  /// weights and its observation normaliser, which then stays fixed. Inputs in `new_inputs` were
  /// unused in that network's training (their weights never learned): they start at 0 weight and
  /// a unit normaliser, so the warm-started actor acts exactly as the network did until it learns
  /// to use them. False when the shapes differ.
  bool warm_start(const std::vector<i32>& dims, const std::vector<std::vector<f32>>& weights_out_in,
                  const std::vector<std::vector<f32>>& biases, const std::vector<f32>& mean, const std::vector<f32>& div,
                  const std::vector<i32>& new_inputs);
  /// The whole learner (networks, normalisers, std, Adam, schedule) for checkpoints.
  std::vector<u8> save() const;
  bool load(const u8* data, std::size_t size);
  /// The deterministic action (the mean) for one observation, as the exported network computes it.
  void act(const f32* observation, f32* action) const { policy_.act(observation, action); }

  const Policy& policy() const { return policy_; }
  const Mlp& actor() const { return policy_.actor; }
  const Mlp& critic() const { return critic_; }
  const Normalizer& actor_normalizer() const { return policy_.normalizer; }
  const std::vector<f32>& stddev() const { return policy_.stddev; }
  const PpoConfig& config() const { return cfg_; }
  i64 total_steps() const { return total_steps_; }
  f32 learning_rate() const { return lr_; }
  i32 iteration() const { return iteration_; }

 private:
  void update(IterationStats& stats);
  ParallelFor parallel_;
  PpoConfig cfg_;
  i32 obs_ = 0, crit_ = 0, act_ = 0;
  Policy policy_;
  Mlp critic_;
  Normalizer critic_norm_;
  Adam actor_adam_, critic_adam_;
  f32 lr_ = 1e-3f;
  Rng rng_;
  Collector collector_;
  Rollout local_;
  i32 iteration_ = 0;
  i64 total_steps_ = 0;
  // The learner's batch: every row of every rollout.
  std::vector<f32> obs_buf_, crit_buf_, act_buf_, mu_buf_, logp_buf_, value_buf_, return_buf_, adv_buf_;
};

}  // namespace kern::train
