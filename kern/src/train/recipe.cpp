// Training recipes (recipe.h).
#include "kern/train/recipe.h"

#include "kern/game/walker.h"
#include "kern/game/walker_policy.h"

namespace kern::train {

f32 Recipe::push_scale(i32 iteration) const { return iteration < 300 ? 0.0f : iteration < 500 ? 0.27f : 1.0f; }

std::unique_ptr<Task> make_task(const std::string& name) {
  if (name == "jump") return make_jump_task();
  if (name == "stand") return make_stand_task();
  return nullptr;
}

Recipe recipe_for(const std::string& task) {
  Recipe r;
  r.task = task;
  r.substeps = game::kWalkerSubsteps;
  r.ppo.critic_hidden = {256, 128};
  if (task == "jump") {
    // From velstand's balance (warm start), a small action noise around it, a light entropy bonus.
    r.warm_start = "velstand";
    r.ppo.init_std = 0.3f;
    r.ppo.entropy_coef = 0.005f;
  }
  return r;
}

std::unique_ptr<DuckEnv> make_env(const Recipe& recipe, u64 seed, i32 index) {
  return std::make_unique<DuckEnv>(make_task(recipe.task), seed * 1000003ull + static_cast<u64>(index), recipe.substeps);
}

void prepare_env(DuckEnv& env, const Recipe& recipe, i32 iteration) {
  env.task().progress(iteration);
  env.push_scale = recipe.push_scale(iteration);
}

std::unique_ptr<Ppo> make_learner(const Recipe& recipe, u64 seed) {
  DuckEnv probe(make_task(recipe.task), 1, recipe.substeps);
  PpoConfig cfg = recipe.ppo;
  cfg.seed = seed;
  auto ppo = std::make_unique<Ppo>(cfg, probe.obs_size(), probe.critic_size(), probe.action_size());
  if (recipe.warm_start == "velstand") {
    auto w = game::walker_policy("velstand.onnx");
    if (!w || !ppo->warm_start(w->dims, w->weights, w->biases, w->mean, w->div, {game::kJumpPhaseSlot, game::kJumpPhaseSlot + 1})) return nullptr;
  }
  return ppo;
}

}  // namespace kern::train
