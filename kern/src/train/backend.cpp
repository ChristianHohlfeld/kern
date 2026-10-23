// Backends (backend.h): where rollouts are collected and the update's parallel work runs.
#include "kern/train/backend.h"

namespace kern::train {

ThreadBackend::ThreadBackend(i32 workers, i32 envs_per_worker, u64 seed, const EnvFactory& make, EnvPrepare prepare)
    : prepare_(std::move(prepare)), pool_(thread_pool(workers)) {
  groups_.resize(static_cast<std::size_t>(max_of(1, workers)));
  for (std::size_t w = 0; w < groups_.size(); ++w) {
    Group& g = groups_[w];
    const i32 first = static_cast<i32>(w) * envs_per_worker;
    g.envs = make(first, envs_per_worker, seed);
    for (auto& e : g.envs) {
      e->reset();
      g.ptrs.push_back(e.get());
    }
    g.collector = Collector(seed, first);
  }
}

i32 ThreadBackend::environments() const {
  i32 n = 0;
  for (const Group& g : groups_) n += static_cast<i32>(g.ptrs.size());
  return n;
}

void ThreadBackend::collect(const Policy& policy, i32 steps, i32 iteration, std::vector<Rollout>& out) {
  out.resize(groups_.size());
  pool_(static_cast<i32>(groups_.size()), [&](i32 w) {
    Group& g = groups_[static_cast<std::size_t>(w)];
    if (prepare_)
      for (Env* e : g.ptrs) prepare_(*e, iteration);
    g.collector.collect(g.ptrs, steps, policy, out[static_cast<std::size_t>(w)]);  // its group, on this thread
  });
}

IterationStats train_iteration(Ppo& ppo, Backend& backend) {
  std::vector<Rollout> rollouts;
  backend.collect(ppo.policy(), ppo.config().steps_per_env, ppo.iteration() + 1, rollouts);
  std::vector<const Rollout*> ptrs;
  for (const Rollout& r : rollouts) ptrs.push_back(&r);
  ppo.set_parallel(backend.update_parallel());
  return ppo.learn(ptrs);
}

}  // namespace kern::train
