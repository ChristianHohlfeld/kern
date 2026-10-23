// kern_train: trains a Micro Duck policy headless on all cores (docs/training.md).
//
//   kern/tools/train.sh                      builds build/kern_train (native, optimised)
//   kern_train --task jump --envs 64 --iterations 2000 --out jump.onnx
//   kern_train --eval jump.onnx --task jump  evaluates an exported network on the clean robot
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "kern/game/walker_policy.h"
#include "kern/train/env.h"
#include "kern/train/onnx_export.h"
#include "kern/train/ppo.h"

using namespace kern;
using namespace kern::train;

namespace {

struct Options {
  std::string task = "jump", out, eval, log;
  i32 envs = 64, threads = 0, iterations = 1500, steps = 24, save_every = 50, episodes = 20;
  u64 seed = 1;
};

std::unique_ptr<Task> make_task(const std::string& name) {
  if (name == "jump") return make_jump_task();
  if (name == "stand") return make_stand_task();
  return nullptr;
}

bool write_file(const std::string& path, const std::vector<u8>& bytes) {
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return false;
  const bool ok = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
  return std::fclose(f) == 0 && ok;
}

std::vector<u8> read_file(const std::string& path) {
  std::vector<u8> out;
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return out;
  u8 buf[65536];
  std::size_t n;
  while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.insert(out.end(), buf, buf + n);
  std::fclose(f);
  return out;
}

// What a jump looks like: per episode the highest trunk above standing, the time with both soles in
// the air, whether it fell, and the return.
int evaluate(const Options& o) {
  game::WalkerPolicy p;
  std::string err;
  const std::vector<u8> bytes = read_file(o.eval);
  if (bytes.empty() || !game::parse_onnx_policy(Span<const u8>(bytes.data(), bytes.size()), p, &err)) {
    std::fprintf(stderr, "cannot read %s: %s\n", o.eval.c_str(), err.c_str());
    return 1;
  }
  DuckEnv env(make_task(o.task), o.seed);
  env.task().randomize = false;
  f64 ret = 0, height = 0, air = 0;
  i32 falls = 0;
  for (i32 e = 0; e < o.episodes; ++e) {
    env.reset();
    f32 best = 0, airborne = 0;
    for (;;) {
      f32 a[kActions];
      p.run(env.observation(), a);
      const Env::Step s = env.step(a);
      const auto& pr = env.probe();
      best = std::max(best, pr.position.z - 0.115f);
      if (!pr.foot_contact[0] && !pr.foot_contact[1]) airborne += kControlDt;
      if (s.terminal) ++falls;
      if (s.terminal || s.timeout) break;
    }
    ret += env.episode_return();
    height += best;
    air += airborne;
  }
  const f64 n = o.episodes;
  std::printf("%s on %s: return %.2f, highest trunk +%.1f mm, both feet in the air %.2f s, fell %d of %d\n",
              o.eval.c_str(), o.task.c_str(), ret / n, height / n * 1000, air / n, falls, o.episodes);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  Options o;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : ""; };
    if (a == "--task") o.task = next();
    else if (a == "--envs") o.envs = std::atoi(next());
    else if (a == "--threads") o.threads = std::atoi(next());
    else if (a == "--iterations") o.iterations = std::atoi(next());
    else if (a == "--steps") o.steps = std::atoi(next());
    else if (a == "--seed") o.seed = static_cast<u64>(std::atoll(next()));
    else if (a == "--out") o.out = next();
    else if (a == "--eval") o.eval = next();
    else if (a == "--episodes") o.episodes = std::atoi(next());
    else if (a == "--log") o.log = next();
    else if (a == "--save-every") o.save_every = std::atoi(next());
    else {
      std::fprintf(stderr, "usage: kern_train [--task jump|stand] [--envs 64] [--threads N] [--iterations 1500] [--steps 24] [--seed 1] [--out task.onnx] [--log log.csv]\n"
                           "       kern_train --eval policy.onnx [--task jump] [--episodes 20]\n");
      return 2;
    }
  }
  if (!make_task(o.task)) { std::fprintf(stderr, "unknown task %s\n", o.task.c_str()); return 2; }
  if (!o.eval.empty()) return evaluate(o);
  if (o.out.empty()) o.out = o.task + ".onnx";

  std::vector<std::unique_ptr<DuckEnv>> owned;
  std::vector<Env*> envs;
  for (i32 i = 0; i < o.envs; ++i) {
    owned.push_back(std::make_unique<DuckEnv>(make_task(o.task), o.seed * 1000003ull + static_cast<u64>(i)));
    owned.back()->reset();
    envs.push_back(owned.back().get());
  }
  PpoConfig cfg;
  cfg.steps_per_env = o.steps;
  cfg.seed = o.seed;
  Ppo ppo(cfg, envs[0]->obs_size(), envs[0]->critic_size(), envs[0]->action_size());
  const ParallelFor pool = thread_pool(o.threads);
  FILE* log = o.log.empty() ? nullptr : std::fopen(o.log.c_str(), "w");
  if (log) std::fprintf(log, "iteration,steps,mean_reward,mean_return,mean_length,episodes,value_loss,surrogate_loss,entropy,mean_std,learning_rate,steps_per_second\n");
  std::printf("training %s: %d envs, %d steps each per iteration, %d iterations\n", o.task.c_str(), o.envs, o.steps, o.iterations);
  for (i32 it = 1; it <= o.iterations; ++it) {
    // Curricula (Pollen's kick: pushes ramp in after the skill forms).
    const f32 push = it < 500 ? 0.0f : it < 1000 ? 0.27f : 1.0f;
    for (auto& e : owned) { e->task().progress(it); e->push_scale = push; }
    const IterationStats s = ppo.iterate(envs, pool);
    const f64 sps = static_cast<f64>(o.envs) * o.steps / (s.collect_seconds + s.learn_seconds);
    std::printf("it %5d  return %8.3f  len %6.1f  reward/step %7.4f  value %8.4f  std %.3f  lr %.1e  %6.0f steps/s (collect %.2fs, learn %.2fs)\n",
                s.iteration, s.mean_return, s.mean_length, s.mean_reward, s.value_loss, s.mean_std, s.learning_rate, sps, s.collect_seconds, s.learn_seconds);
    std::fflush(stdout);
    if (log) {
      std::fprintf(log, "%d,%lld,%g,%g,%g,%d,%g,%g,%g,%g,%g,%g\n", s.iteration, static_cast<long long>(s.steps), s.mean_reward, s.mean_return, s.mean_length, s.episodes,
                   s.value_loss, s.surrogate_loss, s.entropy, s.mean_std, s.learning_rate, sps);
      std::fflush(log);
    }
    if (it % o.save_every == 0 || it == o.iterations) {
      if (!write_file(o.out, export_onnx(ppo.actor(), ppo.actor_normalizer(), micro_duck_metadata(o.task))))
        std::fprintf(stderr, "cannot write %s\n", o.out.c_str());
    }
  }
  if (log) std::fclose(log);
  std::printf("wrote %s\n", o.out.c_str());
  return 0;
}
