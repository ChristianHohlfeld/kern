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
  // Found by search_jump_demo for the XL330 profile (CMA-ES in three stages, 370 generations; every
  // generation on other randomised robots): from a deep crouch the duck launches, rises 10.6 mm in
  // the air, flies 120 ms, tilts ~20 degrees and lands standing (docs/lehrer.md).
  JumpDemo d;
  d.keys = {0.28130585f, 7.89225912f, 5.23304796f, -0.1508708f, -2.45624352f, -2.70261168f, 0.210983217f, 7.36587429f,
            7.69860315f, -0.106615424f, -1.84639323f, 4.50442314f, 0.412051469f, 0.726674736f, -2.61469078f};
  d.amplitude = 2.5f;
  return d;
}

JumpTeacher::JumpTeacher(const JumpDemo& demo) : demo_(demo), velstand_(game::walker_policy("velstand.onnx")) {}

void JumpTeacher::reset() {}

void JumpTeacher::act(const f32* observation, const game::GameWorld::WalkerProbe& p, i32 step, f32* action) {
  (void)p;
  const f32 phase = static_cast<f32>(step) * kControlDt / game::kJumpSeconds;
  if (phase < kStartVelstand || phase >= kEndDemo) {
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
    JumpMeter m;
    m.tick(env.probe());
    bool fell = false;
    for (;;) {
      f32 a[kActions];
      teacher.act(env.observation(), env.probe(), env.steps(), a);
      const Env::Step s = env.step(a);
      m.tick(env.probe());
      if (s.terminal) { fell = true; break; }
      if (s.timeout) break;
    }
    JumpReport one;
    const bool real = m.finish(fell, env.probe(), one);
    const bool standing = !fell && env.probe().foot_contact[0] && env.probe().foot_contact[1] && !env.probe().body_contact && env.probe().gravity.z < -0.9f;
    // A real jump: the body launched (rise in the air), time in the air, upright all along, landed
    // and standing; a fall costs the more the earlier it comes.
    total += 2000 * min_of(m.rise, 0.02f) + 250 * min_of(m.longest, 0.12f) + 500 * min_of(max_of(m.air_apex, 0.0f), 0.03f) -
             300 * max_of(0.0f, 0.966f - m.min_up) + (fell ? -40.0f + 40.0f * static_cast<f32>(m.ticks) / 100.0f : 0.0f) + (standing ? 30.0f : 0.0f) +
             (real ? 20.0f : 0.0f);  // tilting more than 15 degrees costs, a real jump (JumpMeter) pays
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
    JumpMeter m;
    m.tick(env.probe());
    bool fell = false;
    for (;;) {
      f32 a[kActions];
      teacher.act(env.observation(), env.probe(), env.steps(), a);
      const Env::Step s = env.step(a);
      m.tick(env.probe());
      if (s.terminal) { fell = true; break; }
      if (s.timeout) break;
    }
    m.finish(fell, env.probe(), rep);
  }
  finish_report(rep);
  return rep;
}

JumpDemo search_jump_demo(i32 generations, u64 seed, i32 threads, const JumpDemo* start, const std::function<void(i32, f32, const JumpReport&)>& progress) {
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
  std::vector<f64> x0(base.keys.size(), 0.0);
  if (start) {
    base.amplitude = start->amplitude;
    for (std::size_t i = 0; i < base.keys.size(); ++i) x0[i] = start->keys[i];
  }
  const CmaesResult r = cmaes_minimise(
      x0, start ? 0.3 : 0.8, generations, seed,
      [&](const std::vector<std::vector<f64>>& xs, std::vector<f64>& costs) {
        const u64 episodes_seed = seed * 7777 + 100 * (batch++);
        run(static_cast<i32>(xs.size()), [&](i32 i) { costs[static_cast<std::size_t>(i)] = -score_jump_demo(demo_of(xs[static_cast<std::size_t>(i)]), 6, episodes_seed); });
      },
      16, 1e-8, [&](i32 gen, const CmaesResult& so_far, f64) {
        if (!progress) return;
        // The centre of the search (the cost is noisy: other robots every generation).
        const JumpDemo centre = demo_of(so_far.mean);
        progress(gen, score_jump_demo(centre, 6, 424242), evaluate_jump_demo(centre, 1, 100));
      });
  return demo_of(r.mean);
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
