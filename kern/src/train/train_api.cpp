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

using namespace kern;
using namespace kern::train;

namespace {

struct TrainHost {
  std::string task;
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

std::unique_ptr<Task> task_named(const std::string& name) {
  if (name == "jump") return make_jump_task();
  if (name == "stand") return make_stand_task();
  return nullptr;
}

}  // namespace

/// Size in bytes of the last result.
KERN_EXPORT(kern_train_result_length) u32 kern_train_result_length() { return g ? g->length : 0; }

/// Sets up `envs` environments of `task` ("jump", "stand") for collecting, seeded with `seed`.
KERN_EXPORT(kern_train_create) i32 kern_train_create(const char* task, u32 task_length, i32 envs, u32 seed) {
  if (!g) g = new TrainHost();
  g->task = std::string(task, task_length);
  if (!task_named(g->task)) return 0;
  g->envs.clear();
  g->env_ptrs.clear();
  g->collector = Collector(static_cast<u64>(seed) * 7919u + 17u);
  for (i32 i = 0; i < envs; ++i) {
    g->envs.push_back(std::make_unique<DuckEnv>(task_named(g->task), static_cast<u64>(seed) * 1000003ull + static_cast<u64>(i)));
    g->envs.back()->reset();
    g->env_ptrs.push_back(g->envs.back().get());
  }
  return 1;
}

/// The training's progress for the tasks' curricula (iteration) and the shoves' strength (0..1).
KERN_EXPORT(kern_train_progress) void kern_train_progress(i32 iteration, f32 push_scale) {
  if (!g) return;
  for (auto& e : g->envs) { e->task().progress(iteration); e->push_scale = push_scale; }
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

/// Creates the learner for `task` with PPO settings as JSON (keys of PpoConfig, optional).
KERN_EXPORT(kern_train_learner) i32 kern_train_learner(const char* task, u32 task_length, const char* config, u32 config_length, u32 seed) {
  if (!g) g = new TrainHost();
  const std::string name(task, task_length);
  auto t = task_named(name);
  if (!t) return 0;
  DuckEnv probe(task_named(name), 1);
  PpoConfig cfg;
  cfg.seed = seed;
  json::Value c;
  if (config && config_length && json::parse(std::string_view(config, config_length), c) && c.is_object()) {
    auto num = [&](const char* k, f32& v) { if (c[k].is_number()) v = static_cast<f32>(c[k].as_number()); };
    auto inum = [&](const char* k, i32& v) { if (c[k].is_number()) v = static_cast<i32>(c[k].as_number()); };
    num("learningRate", cfg.learning_rate);
    num("initStd", cfg.init_std);
    num("entropyCoef", cfg.entropy_coef);
    num("desiredKl", cfg.desired_kl);
    inum("steps", cfg.steps_per_env);
    inum("epochs", cfg.epochs);
    inum("minibatches", cfg.minibatches);
  }
  g->task = name;
  g->learner = std::make_unique<Ppo>(cfg, probe.obs_size(), probe.critic_size(), probe.action_size());
  return 1;
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
