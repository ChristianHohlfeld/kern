// Vorbild (vorbild.h): the jump's demonstration, its search, and imitation by DAgger.
#include "kern/lehrer/vorbild.h"

#include <cmath>
#include <cstring>

#include "kern/core/json.h"
#include "kern/game/walker.h"
#include "kern/lehrer/cmaes.h"
#include "kern/lehrer/recipe.h"

namespace kern::lehrer {

namespace {

// The demonstration's moments in the jump's phase: before the first and after the last the targets
// are home (0); in between they move linearly from key to key.
constexpr f32 kTimes[JumpDemo::kKeys + 2] = {0.15f, 0.25f, 0.29f, 0.34f, 0.40f, 0.47f, 0.60f};
constexpr i32 kLeft[JumpDemo::kJoints] = {2, 3, 4}, kRight[JumpDemo::kJoints] = {11, 12, 13};  // hip pitch, knee, ankle
constexpr f32 kStartVelstand = 0.15f, kEndDemo = 0.5f;  // velstand before the push and from here on

/// tanh from musl's expm1 (deterministic everywhere, docs/lehrer.md).
f32 dtanh(f32 x) {
  if (x > 10) return 1;
  if (x < -10) return -1;
  const f32 e = kern::dm::expm1(2 * x);
  return e / (e + 2);
}

void demo_targets(const JumpDemo& d, f32 phase, f32* action) {
  std::fill(action, action + kActions, 0.0f);
  auto key = [&](i32 k, i32 j) { return (k == 0 || k == JumpDemo::kKeys + 1) ? 0.0f : d.keys[static_cast<std::size_t>((k - 1) * JumpDemo::kJoints + j)]; };
  for (i32 k = 0; k + 1 < JumpDemo::kKeys + 2; ++k)
    if (phase >= kTimes[k] && phase < kTimes[k + 1]) {
      const f32 u = (phase - kTimes[k]) / (kTimes[k + 1] - kTimes[k]);
      for (i32 j = 0; j < JumpDemo::kJoints; ++j) {
        const f32 v = d.amplitude * dtanh(key(k, j) * (1 - u) + key(k + 1, j) * u);
        action[kLeft[j]] = v;
        action[kRight[j]] = -v;  // the right leg mirrors the left (its joint axes are mirrored)
      }
    }
}

}  // namespace

std::string to_json(const JumpDemo& d) {
  json::Value o = json::Value::object();
  o.set("kind", std::string("jump-demo"));
  o.set("amplitude", static_cast<f64>(d.amplitude));
  json::Value k = json::Value::array();
  for (f32 v : d.keys) k.push(static_cast<f64>(v));
  o.set("keys", std::move(k));
  return json::stringify(o, 2) + "\n";
}

bool jump_demo_from_json(std::string_view text, JumpDemo& out, std::string* error) {
  json::Value v;
  if (!json::parse(text, v, error)) return false;
  const json::Value* k = v.find("keys");
  if (!v.is_object() || !k || !k->is_array() || k->size() != static_cast<std::size_t>(JumpDemo::kKeys * JumpDemo::kJoints)) {
    if (error) *error = "not a jump demonstration (15 keys)";
    return false;
  }
  JumpDemo d;
  d.amplitude = static_cast<f32>(v.find("amplitude") ? v["amplitude"].as_number(2.5) : 2.5);
  for (std::size_t i = 0; i < k->size(); ++i) {
    const f64 x = (*k)[i].as_number(1e9);
    if (!(std::fabs(x) < 100)) {
      if (error) *error = "bad key";
      return false;
    }
    d.keys[i] = static_cast<f32>(x);
  }
  out = d;
  return true;
}

JumpDemo builtin_jump_demo() {
  // Found by search_jump_demo (CMA-ES, 120 generations) for the XL330 profile: trunk 25 mm up,
  // 80-100 ms in the air, lands and stands (docs/lehrer.md).
  JumpDemo d;
  d.keys = {-3.220f, -0.587f, -0.123f, 3.912f, -7.415f, 5.358f, -2.770f, 1.375f, -0.182f, 3.402f, 2.321f, -2.513f, 0.557f, -3.713f, -1.859f};
  d.amplitude = 2.5f;
  return d;
}

JumpTeacher::JumpTeacher(const JumpDemo& demo) : demo_(demo), velstand_(game::walker_policy("velstand.onnx")) {}

void JumpTeacher::reset() {
  have_z0_ = flown_ = landed_ = false;
  z0_ = 0;
}

void JumpTeacher::act(const f32* observation, const game::GameWorld::WalkerProbe& p, i32 step, f32* action) {
  if (!have_z0_) { z0_ = p.position.z; have_z0_ = true; }
  const bool air = !p.foot_contact[0] && !p.foot_contact[1] && !p.body_contact;
  if (air && p.position.z - z0_ > 0.003f) flown_ = true;
  if (flown_ && !air) landed_ = true;
  const f32 phase = static_cast<f32>(step) * kControlDt / game::kJumpSeconds;
  if (phase < kStartVelstand || landed_ || phase >= kEndDemo) {
    f32 o[kObs];
    std::memcpy(o, observation, sizeof o);
    o[game::kJumpPhaseSlot] = o[game::kJumpPhaseSlot + 1] = 0;  // velstand never saw the jump's phase
    if (velstand_) velstand_->run(o, action);
    else std::fill(action, action + kActions, 0.0f);
    return;
  }
  demo_targets(demo_, phase, action);
}

f32 score_jump_demo(const JumpDemo& demo, i32 episodes, u64 seed) {
  f64 total = 0;
  JumpTeacher teacher(demo);
  for (i32 e = 0; e < episodes; ++e) {
    DuckEnv env(make_jump_task(), seed + static_cast<u64>(e), game::kWalkerSubsteps);
    env.task().randomize = e > 0;
    if (e > 0) env.task().progress(100000);  // the training's full variation (its curricula's end)
    env.reset();
    teacher.reset();
    const f32 z0 = env.probe().position.z;
    f32 air_apex = 0, flight = 0, longest = 0, touch_up = 0;
    bool fell = false, flown = false, touched = false;
    i32 t = 0;
    for (;; ++t) {
      f32 a[kActions];
      teacher.act(env.observation(), env.probe(), env.steps(), a);
      const Env::Step s = env.step(a);
      const auto& p = env.probe();
      const bool air = !p.foot_contact[0] && !p.foot_contact[1] && !p.body_contact;
      if (air) air_apex = max_of(air_apex, p.position.z - z0);
      if (air && p.position.z - z0 > 0.003f) flown = true;
      if (flown && !air && !touched) { touched = true; touch_up = -p.gravity.z; }
      flight = air ? flight + kControlDt : 0.0f;
      longest = max_of(longest, flight);
      if (s.terminal) { fell = true; break; }
      if (s.timeout) break;
    }
    const auto& p = env.probe();
    const bool standing = !fell && p.foot_contact[0] && p.foot_contact[1] && !p.body_contact && p.gravity.z < -0.9f;
    total += min_of(air_apex, 0.03f) * 1000 + min_of(longest, 0.14f) * 250 + (touched ? 25 * max_of(0.0f, touch_up - 0.5f) / 0.5f : 0.0f) +
             (fell ? -40.0f + 40.0f * static_cast<f32>(t) / 100.0f : 0.0f) + (standing ? 30.0f : 0.0f);
  }
  return static_cast<f32>(total / max_of(1, episodes));
}

JumpReport evaluate_jump_demo(const JumpDemo& demo, i32 episodes, u64 seed, bool randomized) {
  JumpReport rep;
  JumpTeacher teacher(demo);
  for (i32 e = 0; e < episodes; ++e) {
    DuckEnv env(make_jump_task(), seed + static_cast<u64>(e) * 7919u, game::kWalkerSubsteps);
    env.task().randomize = randomized;
    if (randomized) env.task().progress(100000);
    env.reset();
    teacher.reset();
    const f32 z0 = env.probe().position.z;
    f32 apex = 0, flight = 0, longest = 0;
    bool fell = false;
    for (;;) {
      f32 a[kActions];
      teacher.act(env.observation(), env.probe(), env.steps(), a);
      const Env::Step s = env.step(a);
      const auto& p = env.probe();
      apex = max_of(apex, p.position.z - z0);
      const bool air = !p.foot_contact[0] && !p.foot_contact[1] && !p.body_contact;
      flight = air ? flight + kControlDt : 0.0f;
      longest = max_of(longest, flight);
      if (s.terminal) { fell = true; break; }
      if (s.timeout) break;
    }
    const auto& p = env.probe();
    const bool standing = !fell && p.foot_contact[0] && p.foot_contact[1] && !p.body_contact && p.gravity.z < -0.9f;
    ++rep.episodes;
    rep.falls += fell ? 1 : 0;
    rep.height_mm += apex * 1000;
    rep.flight_s += longest;
    rep.best_height_mm = max_of(rep.best_height_mm, apex * 1000);
    rep.best_flight_s = max_of(rep.best_flight_s, longest);
    if (standing && longest >= 0.06f - 1e-4f && apex >= 0.015f) ++rep.jumps;
  }
  if (rep.episodes) { rep.height_mm /= static_cast<f32>(rep.episodes); rep.flight_s /= static_cast<f32>(rep.episodes); }
  return rep;
}

JumpDemo search_jump_demo(i32 generations, u64 seed, i32 threads, const std::function<void(i32, f32, const JumpReport&)>& progress) {
  const ParallelFor run = thread_pool(max_of(1, threads));
  JumpDemo base;
  auto demo_of = [&](const std::vector<f64>& x) {
    JumpDemo d = base;
    for (std::size_t i = 0; i < d.keys.size(); ++i) d.keys[i] = static_cast<f32>(x[i]);
    return d;
  };
  // Every generation meets other robots (start, build, servos, sensors): a demonstration that only
  // works for a few would not teach a robust jump. All candidates of a generation meet the same ones.
  u64 batch = 0;
  const CmaesResult r = cmaes_minimise(
      std::vector<f64>(base.keys.size(), 0.0), 0.8, generations, seed,
      [&](const std::vector<std::vector<f64>>& xs, std::vector<f64>& costs) {
        const u64 episodes_seed = seed * 7777 + 100 * (batch++);
        run(static_cast<i32>(xs.size()), [&](i32 i) { costs[static_cast<std::size_t>(i)] = -score_jump_demo(demo_of(xs[static_cast<std::size_t>(i)]), 6, episodes_seed); });
      },
      16, 1e-8, [&](i32 gen, const CmaesResult& so_far, f64) {
        if (!progress) return;
        progress(gen, static_cast<f32>(-so_far.best_cost), evaluate_jump_demo(demo_of(so_far.best), 1, 100));
      });
  return demo_of(r.best);
}

struct JumpImitation::Data {
  Recipe recipe;
  std::vector<f32> obs, act;
};

JumpImitation::JumpImitation(const Recipe& recipe, u64 seed) : d_(std::make_unique<Data>()), seed_(seed) { d_->recipe = recipe; }
JumpImitation::~JumpImitation() = default;

void dagger_episodes(const Recipe& recipe, u64 seed, i32 round, i32 first, i32 count, const Policy* student, std::vector<f32>& obs_out,
                     std::vector<f32>& act_out) {
  for (i32 e = first; e < first + count; ++e) {
    auto env = make_env(recipe, seed + 7919u * static_cast<u64>(round + 1), e);
    prepare_env(*env, recipe, 1000);  // the full randomisation the practice will see
    env->reset();
    JumpTeacher teacher(recipe.demo);
    teacher.reset();
    Rng noise(seed * 31 + static_cast<u64>(round) * 1000003u + static_cast<u64>(e));
    for (;;) {
      f32 label[kActions], a[kActions];
      teacher.act(env->observation(), env->probe(), env->steps(), label);
      obs_out.insert(obs_out.end(), env->observation(), env->observation() + kObs);
      act_out.insert(act_out.end(), label, label + kActions);
      if (round == 0 || !student) {
        std::memcpy(a, label, sizeof a);  // round 0: the teacher drives
      } else {
        student->act(env->observation(), a);  // later: the learner drives, a little noise for coverage
        for (f32& x : a) x += 0.05f * noise.normal();
      }
      const Env::Step s = env->step(a);
      if (s.terminal || s.timeout) break;
    }
  }
}

ImitationReport JumpImitation::round(Ppo& learner, const ParallelFor& parallel, bool evaluate) {
  const ParallelFor run = parallel ? parallel : ParallelFor([](i32 n, const std::function<void(i32)>& fn) { for (i32 i = 0; i < n; ++i) fn(i); });
  const i32 episodes = d_->recipe.imitation_episodes;
  std::vector<std::vector<f32>> obs(static_cast<std::size_t>(episodes)), act(static_cast<std::size_t>(episodes));
  const Policy student = learner.policy();
  run(episodes, [&](i32 e) {
    dagger_episodes(d_->recipe, seed_, round_, e, 1, &student, obs[static_cast<std::size_t>(e)], act[static_cast<std::size_t>(e)]);
  });
  std::vector<f32> all_obs, all_act;
  for (i32 e = 0; e < episodes; ++e) {
    all_obs.insert(all_obs.end(), obs[static_cast<std::size_t>(e)].begin(), obs[static_cast<std::size_t>(e)].end());
    all_act.insert(all_act.end(), act[static_cast<std::size_t>(e)].begin(), act[static_cast<std::size_t>(e)].end());
  }
  return round_from(learner, all_obs, all_act, parallel, evaluate);
}

ImitationReport JumpImitation::round_from(Ppo& learner, const std::vector<f32>& obs, const std::vector<f32>& act, const ParallelFor& parallel, bool evaluate) {
  d_->obs.insert(d_->obs.end(), obs.begin(), obs.end());
  d_->act.insert(d_->act.end(), act.begin(), act.end());
  const i32 r = round_++;
  ImitationReport rep;
  rep.rounds = round_;
  rep.samples = static_cast<i32>(d_->act.size() / kActions);
  learner.set_parallel(parallel);
  rep.mse = learner.imitate(d_->obs, d_->act, 10, 256, 1e-3f, seed_ + static_cast<u64>(r));
  if (evaluate)
    rep.student = evaluate_jump([](void* u, const f32* o, f32* a) { static_cast<Ppo*>(u)->act(o, a); }, &learner, 5, 1000, d_->recipe.substeps);
  return rep;
}

ImitationReport imitate_jump(Ppo& learner, const Recipe& recipe, u64 seed, const ParallelFor& parallel,
                             const std::function<void(i32, const ImitationReport&)>& progress) {
  JumpImitation im(recipe, seed);
  ImitationReport rep;
  for (i32 r = 0; r < recipe.imitation_rounds; ++r) {
    rep = im.round(learner, parallel, progress != nullptr || r + 1 == recipe.imitation_rounds);
    if (progress) progress(r, rep);
  }
  return rep;
}

}  // namespace kern::lehrer
