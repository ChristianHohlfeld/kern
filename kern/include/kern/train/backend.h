// Where the trainer's computing happens (docs/training.md). The learner (Ppo) only ever sees a
// Backend: it hands it the current Policy and gets rollouts back, and it asks it for the parallel
// runner its update uses. Behind it the compute is interchangeable:
//   - ThreadBackend: environments on this machine's threads (native),
//   - the browser: Web Workers, each a Collector with its own environments, driven by the page
//     through the C ABI (src/train/train_api.cpp),
//   - later remote machines.
// All of them exchange the same flat buffers: Policy::serialize / Rollout::serialize.
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "kern/train/env.h"
#include "kern/train/ppo.h"

namespace kern::train {

class Backend {
 public:
  virtual ~Backend() = default;
  virtual std::string name() const = 0;
  /// Environments it steps per iteration (all workers together).
  virtual i32 environments() const = 0;
  /// One rollout per worker: `steps` steps of each of its environments with `policy`; `iteration`
  /// lets the environments' curricula follow the training.
  virtual void collect(const Policy& policy, i32 steps, i32 iteration, std::vector<Rollout>& out) = 0;
  /// The runner for the update's parallel work (sequential where there are no threads).
  virtual ParallelFor update_parallel() = 0;
};

/// Makes the environments of one worker (worker index, environments, seed).
using EnvFactory = std::function<std::vector<std::unique_ptr<Env>>(i32 worker, i32 count, u64 seed)>;
/// Before each iteration, per environment (the curricula: task progress, shove strength).
using EnvPrepare = std::function<void(Env& env, i32 iteration)>;

/// Native: `workers` groups of environments, each stepped on its own thread.
class ThreadBackend : public Backend {
 public:
  ThreadBackend(i32 workers, i32 envs_per_worker, u64 seed, const EnvFactory& make, EnvPrepare prepare = {});
  std::string name() const override { return "threads"; }
  i32 environments() const override;
  void collect(const Policy& policy, i32 steps, i32 iteration, std::vector<Rollout>& out) override;
  ParallelFor update_parallel() override { return pool_; }
  i32 workers() const { return static_cast<i32>(groups_.size()); }

 private:
  struct Group {
    std::vector<std::unique_ptr<Env>> envs;
    std::vector<Env*> ptrs;
    Collector collector;
  };
  std::vector<Group> groups_;
  EnvPrepare prepare_;
  ParallelFor pool_;
};

/// The training loop over any backend: hand out the policy, collect, learn.
IterationStats train_iteration(Ppo& ppo, Backend& backend);

}  // namespace kern::train
