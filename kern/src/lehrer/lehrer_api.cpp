// C ABI of the Lehrer for the browser (kern/web/lehrer/): one module instance per Web Worker. A
// worker collects (its own environments, the policy it was handed) and/or learns (the PPO learner).
//
// Buffers go in through kern_alloc'd memory; results come back as a pointer that stays valid until
// the next call, with kern_lehrer_result_length() bytes.
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "kern/api/c_api.h"
#include "kern/core/json.h"
#include "kern/lehrer/env.h"
#include "kern/lehrer/onnx_export.h"
#include "kern/lehrer/ppo.h"
#include "kern/lehrer/recipe.h"
#include "kern/lehrer/vorbild.h"

using namespace kern;
using namespace kern::lehrer;

namespace {

struct TrainHost {
  std::string task;
  Recipe recipe;
  u64 seed = 1;
  std::vector<std::unique_ptr<DuckEnv>> envs;
  std::vector<Env*> env_ptrs;
  Collector collector;
  Policy policy;
  bool has_policy = false;
  std::unique_ptr<Ppo> learner;
  std::unique_ptr<JumpImitation> imitation;
  std::vector<u8> bytes;
  std::vector<f32> floats;
  std::string text;
  u32 length = 0;
};

TrainHost* g = nullptr;

}  // namespace

/// Size in bytes of the last result.
KERN_EXPORT(kern_lehrer_result_length) u32 kern_lehrer_result_length() { return g ? g->length : 0; }

/// Sets up environments first .. first + envs - 1 of a `task` run seeded with `seed` ("jump",
/// "stand"), with the task's recipe (recipe.h): worker w of k gets first = w * envs, and the run is
/// the same as natively with as many environments in all.
KERN_EXPORT(kern_lehrer_create) i32 kern_lehrer_create(const char* task, u32 task_length, i32 envs, u32 seed, i32 first) {
  if (!g) g = new TrainHost();
  g->task = std::string(task, task_length);
  if (!make_task(g->task)) return 0;
  g->recipe = recipe_for(g->task);
  g->seed = seed;
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
KERN_EXPORT(kern_lehrer_progress) void kern_lehrer_progress(i32 iteration) {
  if (!g) return;
  for (auto& e : g->envs) prepare_env(*e, g->recipe, iteration);
}

/// Hands the collector the policy to act with (a Policy::serialize buffer, `count` floats).
KERN_EXPORT(kern_lehrer_set_policy) i32 kern_lehrer_set_policy(const f32* data, u32 count) {
  if (!g) return 0;
  g->has_policy = g->policy.deserialize(data, count);
  return g->has_policy ? 1 : 0;
}

/// Collects `steps` steps of every environment; returns the Rollout's bytes.
KERN_EXPORT(kern_lehrer_collect) const u8* kern_lehrer_collect(i32 steps) {
  if (!g || !g->has_policy || g->env_ptrs.empty()) return nullptr;
  Rollout r;
  g->collector.collect(g->env_ptrs, steps, g->policy, r);
  g->bytes = r.serialize();
  g->length = static_cast<u32>(g->bytes.size());
  return g->bytes.data();
}

/// Creates the learner for `task` with its recipe; settings as JSON override it (learningRate,
/// initStd, entropyCoef, desiredKl, steps, epochs, minibatches, warmStart "velstand" | "none").
KERN_EXPORT(kern_lehrer_learner) i32 kern_lehrer_learner(const char* task, u32 task_length, const char* config, u32 config_length, u32 seed) {
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
    if (c["imitate"].is_bool()) r.imitate = c["imitate"].as_bool();
  }
  g->task = name;
  g->recipe = r;
  g->learner = make_learner(r, seed);
  g->imitation.reset();
  if (g->learner && r.imitate) g->imitation = std::make_unique<JumpImitation>(r, seed);
  return g->learner ? 1 : 0;
}

/// Steps per environment and iteration of the learner's recipe (what the collectors run).
KERN_EXPORT(kern_lehrer_steps) i32 kern_lehrer_steps() { return g && g->learner ? g->recipe.ppo.steps_per_env : 0; }

/// How the learner's network jumps now (clean robot, `episodes` episodes) as JSON: jumps,
/// episodes, heightMm, flightS, falls.
KERN_EXPORT(kern_lehrer_evaluate) const char* kern_lehrer_evaluate(i32 episodes) {
  if (!g || !g->learner || g->task != "jump") return nullptr;
  const JumpReport r = evaluate_jump([](void* u, const f32* o, f32* a) { static_cast<Ppo*>(u)->act(o, a); }, g->learner.get(), max_of(1, episodes), 1000,
                                     g->recipe.substeps);
  json::Value v = json::Value::object();
  v.set("jumps", r.jumps);
  v.set("episodes", r.episodes);
  v.set("heightMm", static_cast<f64>(r.height_mm));
  v.set("flightS", static_cast<f64>(r.flight_s));
  v.set("falls", r.falls);
  g->text = json::stringify(v);
  g->length = static_cast<u32>(g->text.size());
  return g->text.c_str();
}

/// A collector's share of an imitation round (vorbild.h, dagger_episodes): episodes first ..
/// first + count - 1 of round `round`, driven by the policy it was handed. Returns u32 ticks, then
/// ticks x 61 observations and ticks x 14 teacher actions (f32).
KERN_EXPORT(kern_lehrer_dagger) const u8* kern_lehrer_dagger(i32 round, i32 first, i32 count) {
  if (!g || !g->has_policy) return nullptr;
  std::vector<f32> obs, act;
  dagger_episodes(g->recipe, g->seed, round, first, count, &g->policy, obs, act);
  const u32 ticks = static_cast<u32>(act.size() / kActions);
  g->bytes.assign(4 + (obs.size() + act.size()) * sizeof(f32), 0);
  std::memcpy(g->bytes.data(), &ticks, 4);
  std::memcpy(g->bytes.data() + 4, obs.data(), obs.size() * sizeof(f32));
  std::memcpy(g->bytes.data() + 4 + obs.size() * sizeof(f32), act.data(), act.size() * sizeof(f32));
  g->length = static_cast<u32>(g->bytes.size());
  return g->bytes.data();
}

/// Episodes per imitation round (the recipe's).
KERN_EXPORT(kern_lehrer_imitation_episodes) i32 kern_lehrer_imitation_episodes() { return g ? g->recipe.imitation_episodes : 0; }

/// Imitation rounds still to do before the practice (0: none, or done).
KERN_EXPORT(kern_lehrer_imitation_left) i32 kern_lehrer_imitation_left() {
  if (!g || !g->learner || !g->imitation) return 0;
  return g->recipe.imitation_rounds - g->imitation->rounds_done();
}

/// One imitation round (vorbild.h) in the learner from the collectors' shares (`count` buffers of
/// kern_lehrer_dagger laid one after the other, each prefixed with its byte size as a u32, in
/// episode order); with count 0 the learner runs the whole round itself. After the last round the
/// practice begins. Returns JSON: round, rounds, samples, error and how the network jumps now.
KERN_EXPORT(kern_lehrer_imitate) const char* kern_lehrer_imitate(const u8* data, u32 bytes, u32 count) {
  if (!g || !g->learner || !g->imitation || kern_lehrer_imitation_left() <= 0) return nullptr;
  ImitationReport r;
  if (count == 0) {
    r = g->imitation->round(*g->learner, {}, true);
  } else {
    std::vector<f32> obs, act;
    std::size_t at = 0;
    for (u32 k = 0; k < count; ++k) {
      if (at + 8 > bytes) return nullptr;
      u32 n = 0, ticks = 0;
      std::memcpy(&n, data + at, 4);
      std::memcpy(&ticks, data + at + 4, 4);
      const std::size_t need = 4 + static_cast<std::size_t>(ticks) * (kObs + kActions) * sizeof(f32);
      if (n != need || at + 4 + n > bytes) return nullptr;
      const f32* f = reinterpret_cast<const f32*>(data + at + 8);  // kern_alloc'd: 4-byte aligned
      std::vector<f32> part(f, f + static_cast<std::size_t>(ticks) * (kObs + kActions));
      obs.insert(obs.end(), part.begin(), part.begin() + static_cast<std::ptrdiff_t>(ticks) * kObs);
      act.insert(act.end(), part.begin() + static_cast<std::ptrdiff_t>(ticks) * kObs, part.end());
      at += 4 + n;
    }
    r = g->imitation->round_from(*g->learner, obs, act, {}, true);
  }
  if (kern_lehrer_imitation_left() == 0) begin_practice(*g->learner, g->recipe);
  json::Value v = json::Value::object();
  v.set("round", r.rounds);
  v.set("rounds", g->recipe.imitation_rounds);
  v.set("samples", r.samples);
  v.set("error", r.mse);
  v.set("jumps", r.student.jumps);
  v.set("episodes", r.student.episodes);
  v.set("heightMm", r.student.height_mm);
  v.set("flightS", r.student.flight_s);
  v.set("falls", r.student.falls);
  g->text = json::stringify(v);
  g->length = static_cast<u32>(g->text.size());
  return g->text.c_str();
}

/// The learner's current policy (Policy::serialize floats; length in bytes).
KERN_EXPORT(kern_lehrer_policy) const f32* kern_lehrer_policy() {
  if (!g || !g->learner) return nullptr;
  g->floats = g->learner->policy().serialize();
  g->length = static_cast<u32>(g->floats.size() * sizeof(f32));
  return g->floats.data();
}

/// Learns from `count` rollouts laid out one after the other in `data` (each prefixed with its byte
/// size as a u32). Returns the iteration's statistics as JSON.
KERN_EXPORT(kern_lehrer_learn) const char* kern_lehrer_learn(const u8* data, u32 bytes, u32 count) {
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
  before_learning(*g->learner, g->recipe);
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
KERN_EXPORT(kern_lehrer_export_onnx) const u8* kern_lehrer_export_onnx() {
  if (!g || !g->learner) return nullptr;
  g->bytes = export_onnx(g->learner->actor(), g->learner->actor_normalizer(), micro_duck_metadata(g->task));
  g->length = static_cast<u32>(g->bytes.size());
  return g->bytes.data();
}

/// The determinism fingerprint (env.h): `ticks` hashes, one per control tick (u64 each).
KERN_EXPORT(kern_lehrer_fingerprint) const u64* kern_lehrer_fingerprint(i32 ticks) {
  if (!g) g = new TrainHost();
  static std::vector<u64> hashes;
  hashes = determinism_fingerprint(ticks);
  g->length = static_cast<u32>(hashes.size() * sizeof(u64));
  return hashes.data();
}
