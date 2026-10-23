// C ABI of the trainer for the browser (kern/web/train/): one module instance per Web Worker. A
// worker collects (its own environments, the policy it was handed) and/or learns (the PPO learner).
//
// Buffers go in through kern_alloc'd memory; results come back as a pointer that stays valid until
// the next call, with kern_train_result_length() bytes.
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "kern/api/c_api.h"
#include "kern/core/json.h"
#include "kern/train/env.h"
#include "kern/train/onnx_export.h"
#include "kern/train/ppo.h"
#include "kern/train/recipe.h"

using namespace kern;
using namespace kern::train;

namespace {

struct TrainHost {
  std::string task;
  Recipe recipe;
  std::vector<std::unique_ptr<DuckEnv>> envs;
  std::vector<Env*> env_ptrs;
  Collector collector;
  Policy policy;
  bool has_policy = false;
  std::unique_ptr<Ppo> learner;
  std::vector<u8> bytes;
  std::vector<f32> floats;
  std::string text;
  u32 length = 0;
};

TrainHost* g = nullptr;

}  // namespace

/// Size in bytes of the last result.
KERN_EXPORT(kern_train_result_length) u32 kern_train_result_length() { return g ? g->length : 0; }

/// Sets up environments first .. first + envs - 1 of a `task` run seeded with `seed` ("jump",
/// "stand"), with the task's recipe (recipe.h): worker w of k gets first = w * envs, and the run is
/// the same as natively with as many environments in all.
KERN_EXPORT(kern_train_create) i32 kern_train_create(const char* task, u32 task_length, i32 envs, u32 seed, i32 first) {
  if (!g) g = new TrainHost();
  g->task = std::string(task, task_length);
  if (!make_task(g->task)) return 0;
  g->recipe = recipe_for(g->task);
  g->envs.clear();
  g->env_ptrs.clear();
  g->collector = Collector(seed, first);
  for (i32 i = 0; i < envs; ++i) {
    g->envs.push_back(make_env(g->recipe, seed, first + i));
    g->envs.back()->reset();
    g->env_ptrs.push_back(g->envs.back().get());
  }
  return 1;
}

/// Before iteration `iteration` (from 1): the task's curricula and the shoves, as the recipe says.
KERN_EXPORT(kern_train_progress) void kern_train_progress(i32 iteration) {
  if (!g) return;
  for (auto& e : g->envs) prepare_env(*e, g->recipe, iteration);
}

/// Hands the collector the policy to act with (a Policy::serialize buffer, `count` floats).
KERN_EXPORT(kern_train_set_policy) i32 kern_train_set_policy(const f32* data, u32 count) {
  if (!g) return 0;
  g->has_policy = g->policy.deserialize(data, count);
  return g->has_policy ? 1 : 0;
}

/// Collects `steps` steps of every environment; returns the Rollout's bytes.
KERN_EXPORT(kern_train_collect) const u8* kern_train_collect(i32 steps) {
  if (!g || !g->has_policy || g->env_ptrs.empty()) return nullptr;
  Rollout r;
  g->collector.collect(g->env_ptrs, steps, g->policy, r);
  g->bytes = r.serialize();
  g->length = static_cast<u32>(g->bytes.size());
  return g->bytes.data();
}

/// Creates the learner for `task` with its recipe; settings as JSON override it (learningRate,
/// initStd, entropyCoef, desiredKl, steps, epochs, minibatches, warmStart "velstand" | "none").
KERN_EXPORT(kern_train_learner) i32 kern_train_learner(const char* task, u32 task_length, const char* config, u32 config_length, u32 seed) {
  if (!g) g = new TrainHost();
  const std::string name(task, task_length);
  if (!make_task(name)) return 0;
  Recipe r = recipe_for(name);
  json::Value c;
  if (config && config_length && json::parse(std::string_view(config, config_length), c) && c.is_object()) {
    auto num = [&](const char* k, f32& v) { if (c[k].is_number()) v = static_cast<f32>(c[k].as_number()); };
    auto inum = [&](const char* k, i32& v) { if (c[k].is_number()) v = static_cast<i32>(c[k].as_number()); };
    num("learningRate", r.ppo.learning_rate);
    num("initStd", r.ppo.init_std);
    num("entropyCoef", r.ppo.entropy_coef);
    num("desiredKl", r.ppo.desired_kl);
    inum("steps", r.ppo.steps_per_env);
    inum("epochs", r.ppo.epochs);
    inum("minibatches", r.ppo.minibatches);
    if (c["warmStart"].is_string()) r.warm_start = c["warmStart"].as_string();
  }
  g->task = name;
  g->recipe = r;
  g->learner = make_learner(r, seed);
  return g->learner ? 1 : 0;
}

/// The learner's current policy (Policy::serialize floats; length in bytes).
KERN_EXPORT(kern_train_policy) const f32* kern_train_policy() {
  if (!g || !g->learner) return nullptr;
  g->floats = g->learner->policy().serialize();
  g->length = static_cast<u32>(g->floats.size() * sizeof(f32));
  return g->floats.data();
}

/// Learns from `count` rollouts laid out one after the other in `data` (each prefixed with its byte
/// size as a u32). Returns the iteration's statistics as JSON.
KERN_EXPORT(kern_train_learn) const char* kern_train_learn(const u8* data, u32 bytes, u32 count) {
  if (!g || !g->learner) return nullptr;
  std::vector<Rollout> rollouts(count);
  std::vector<const Rollout*> ptrs;
  std::size_t at = 0;
  for (u32 k = 0; k < count; ++k) {
    if (at + 4 > bytes) return nullptr;
    u32 n = 0;
    std::memcpy(&n, data + at, 4);
    at += 4;
    if (at + n > bytes || !rollouts[k].deserialize(data + at, n)) return nullptr;
    at += n;
    ptrs.push_back(&rollouts[k]);
  }
  const IterationStats s = g->learner->learn(ptrs);
  json::Value v = json::Value::object();
  v.set("iteration", s.iteration);
  v.set("steps", static_cast<f64>(s.steps));
  v.set("meanReward", s.mean_reward);
  v.set("meanReturn", s.mean_return);
  v.set("meanLength", s.mean_length);
  v.set("episodes", s.episodes);
  v.set("valueLoss", s.value_loss);
  v.set("surrogateLoss", s.surrogate_loss);
  v.set("entropy", s.entropy);
  v.set("learningRate", s.learning_rate);
  v.set("meanStd", s.mean_std);
  v.set("collectSeconds", s.collect_seconds);
  v.set("learnSeconds", s.learn_seconds);
  g->text = json::stringify(v);
  g->length = static_cast<u32>(g->text.size());
  return g->text.c_str();
}

/// The learner's actor as an ONNX file in the shape of Pollen's networks.
KERN_EXPORT(kern_train_export_onnx) const u8* kern_train_export_onnx() {
  if (!g || !g->learner) return nullptr;
  g->bytes = export_onnx(g->learner->actor(), g->learner->actor_normalizer(), micro_duck_metadata(g->task));
  g->length = static_cast<u32>(g->bytes.size());
  return g->bytes.data();
}

/// The determinism fingerprint (env.h): `ticks` hashes, one per control tick (u64 each).
KERN_EXPORT(kern_train_fingerprint) const u64* kern_train_fingerprint(i32 ticks) {
  if (!g) g = new TrainHost();
  static std::vector<u64> hashes;
  hashes = determinism_fingerprint(ticks);
  g->length = static_cast<u32>(hashes.size() * sizeof(u64));
  return hashes.data();
}
