// Proximal Policy Optimization in C++, following rsl_rl's PPO (the recipe Pollen trains the Micro
// Duck's networks with): Gaussian actor with a learnable std, asymmetric critic, running observation
// normalisers, GAE, clipped surrogate and value losses, entropy bonus, adaptive learning rate from
// the KL divergence, gradient clipping per network, Adam.
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
  u64 seed = 1;
};

/// Runs fn(0..n-1); a parallel runner spreads the calls over threads (each index on one thread).
using ParallelFor = std::function<void(i32 n, const std::function<void(i32)>& fn)>;
/// One worker per hardware thread (std::thread); sequential where there are no threads.
ParallelFor thread_pool(i32 threads = 0);

struct IterationStats {
  i32 iteration = 0;
  f32 mean_reward = 0;          // per step, over the rollout
  f32 mean_return = 0;          // of the episodes that ended in this rollout (0 when none did)
  f32 mean_length = 0;          // their length in steps
  i32 episodes = 0;
  f32 value_loss = 0, surrogate_loss = 0, entropy = 0, learning_rate = 0, mean_std = 0;
  f64 collect_seconds = 0, learn_seconds = 0;
  i64 steps = 0;                // total environment steps so far
};

class Ppo {
 public:
  Ppo(const PpoConfig& config, i32 obs_size, i32 critic_size, i32 action_size);
  /// One iteration: steps_per_env steps of every environment (resetting finished episodes), then
  /// the update. The environments must have been reset once before the first call.
  IterationStats iterate(std::vector<Env*>& envs, const ParallelFor& parallel = {});
  /// The deterministic action (the mean) for one observation, as the exported network computes it.
  void act(const f32* observation, f32* action) const;

  const Mlp& actor() const { return actor_; }
  const Mlp& critic() const { return critic_; }
  const Normalizer& actor_normalizer() const { return actor_norm_; }
  const std::vector<f32>& stddev() const { return std_; }
  const PpoConfig& config() const { return cfg_; }
  i64 total_steps() const { return total_steps_; }
  f32 learning_rate() const { return lr_; }

 private:
  void update(IterationStats& stats);
  PpoConfig cfg_;
  i32 obs_ = 0, crit_ = 0, act_ = 0;
  Mlp actor_, critic_;
  Normalizer actor_norm_, critic_norm_;
  std::vector<f32> std_;
  Adam actor_adam_, critic_adam_;
  f32 lr_ = 1e-3f;
  Rng rng_;
  std::vector<Rng> env_rng_;
  i32 iteration_ = 0;
  i64 total_steps_ = 0;
  std::vector<f32> episode_return_;
  std::vector<i32> episode_length_;
  // The rollout, [step][env] rows.
  std::vector<f32> obs_buf_, crit_buf_, act_buf_, mu_buf_, logp_buf_, value_buf_, reward_buf_, done_buf_, return_buf_, adv_buf_;
};

}  // namespace kern::train
