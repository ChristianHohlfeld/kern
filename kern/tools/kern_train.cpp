// kern_train: trains a Micro Duck policy headless on all cores (docs/training.md).
//
//   kern/tools/train.sh                      builds kern/build/kern_train (native, optimised)
//   kern_train --task jump --iterations 600 --out jump.onnx
//   kern_train --eval jump.onnx               the jump on the clean robot in the Studio's physics
//
// Long runs (on a server, say): --checkpoint run.ckpt saves the whole learner every --save-every
// iterations; --resume run.ckpt carries on from it.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "kern/game/walker.h"
#include "kern/game/walker_policy.h"
#include "kern/train/backend.h"
#include "kern/train/env.h"
#include "kern/train/onnx_export.h"
#include "kern/train/ppo.h"

using namespace kern;
using namespace kern::train;

namespace {

struct Options {
  std::string task = "jump", out, eval, log, checkpoint, resume, warm = "velstand";
  i32 envs = 0, threads = 0, iterations = 600, steps = 24, save_every = 25, episodes = 10;
  i32 substeps = game::kWalkerSubsteps, eval_every = 10, stop_after_jump = 0;
  f32 init_std = 0.3f, learning_rate = 1e-3f, entropy = 0.005f;
  std::vector<i32> critic{256, 128};
  u64 seed = 1;
};

std::unique_ptr<Task> make_task(const std::string& name) {
  if (name == "jump") return make_jump_task();
  if (name == "stand") return make_stand_task();
  return nullptr;
}

bool write_file(const std::string& path, const std::vector<u8>& bytes) {
  const std::string tmp = path + ".tmp";
  FILE* f = std::fopen(tmp.c_str(), "wb");
  if (!f) return false;
  const bool ok = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
  return std::fclose(f) == 0 && ok && std::rename(tmp.c_str(), path.c_str()) == 0;
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

void act_onnx(void* user, const f32* o, f32* a) { static_cast<const game::WalkerPolicy*>(user)->run(o, a); }
void act_ppo(void* user, const f32* o, f32* a) { static_cast<const Ppo*>(user)->act(o, a); }

void print_report(const char* what, const JumpReport& r) {
  std::printf("%s: %d of %d episodes a real jump, trunk up %.1f mm (best %.1f), flight %.3f s (best %.3f), fell %d\n", what, r.jumps,
              r.episodes, r.height_mm, r.best_height_mm, r.flight_s, r.best_flight_s, r.falls);
}

int evaluate(const Options& o) {
  game::WalkerPolicy p;
  std::string err;
  const std::vector<u8> bytes = read_file(o.eval);
  if (bytes.empty() || !game::parse_onnx_policy(Span<const u8>(bytes.data(), bytes.size()), p, &err)) {
    std::fprintf(stderr, "cannot read %s: %s\n", o.eval.c_str(), err.c_str());
    return 1;
  }
  print_report(o.eval.c_str(), evaluate_jump(act_onnx, &p, o.episodes, 1000, game::kWalkerSubsteps));
  return 0;
}

std::vector<i32> parse_dims(const char* s) {
  std::vector<i32> out;
  while (*s) {
    out.push_back(std::atoi(s));
    while (*s && *s != ',') ++s;
    if (*s == ',') ++s;
  }
  return out;
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
    else if (a == "--init-std") o.init_std = static_cast<f32>(std::atof(next()));
    else if (a == "--lr") o.learning_rate = static_cast<f32>(std::atof(next()));
    else if (a == "--entropy") o.entropy = static_cast<f32>(std::atof(next()));
    else if (a == "--substeps") o.substeps = std::atoi(next());
    else if (a == "--critic") o.critic = parse_dims(next());
    else if (a == "--warm-start") o.warm = next();
    else if (a == "--checkpoint") o.checkpoint = next();
    else if (a == "--resume") o.resume = next();
    else if (a == "--eval-every") o.eval_every = std::atoi(next());
    else if (a == "--stop-after-jump") o.stop_after_jump = 1;
    else {
      std::fprintf(stderr,
                   "usage: kern_train [--task jump|stand] [--envs N] [--threads N] [--iterations 600] [--steps 24] [--seed 1]\n"
                   "                  [--out task.onnx] [--log log.csv] [--checkpoint run.ckpt] [--resume run.ckpt] [--save-every 25]\n"
                   "                  [--warm-start velstand|none|file.onnx] [--init-std 0.3] [--lr 1e-3] [--entropy 0.005]\n"
                   "                  [--critic 256,128] [--substeps 16] [--eval-every 10] [--stop-after-jump]\n"
                   "       kern_train --eval policy.onnx [--episodes 10]\n");
      return 2;
    }
  }
  if (!make_task(o.task)) { std::fprintf(stderr, "unknown task %s\n", o.task.c_str()); return 2; }
  if (!o.eval.empty()) return evaluate(o);
  if (o.out.empty()) o.out = o.task + ".onnx";
  const i32 threads = o.threads > 0 ? o.threads : max_of(1, static_cast<i32>(std::thread::hardware_concurrency()));
  if (o.envs <= 0) o.envs = 16 * threads;

  // The compute: this machine's threads, each stepping its own group of ducks (backend.h).
  const i32 per_worker = max_of(1, o.envs / threads);
  o.envs = per_worker * threads;
  ThreadBackend backend(
      threads, per_worker, o.seed,
      [&](i32, i32 count, u64 seed) {
        std::vector<std::unique_ptr<Env>> v;
        for (i32 i = 0; i < count; ++i) v.push_back(std::make_unique<DuckEnv>(make_task(o.task), seed + static_cast<u64>(i), o.substeps));
        return v;
      },
      [](Env& e, i32 it) {
        auto& d = static_cast<DuckEnv&>(e);
        d.task().progress(it);
        d.push_scale = it < 300 ? 0.0f : it < 500 ? 0.27f : 1.0f;  // shoves come in once the move has formed (Pollen's kick)
      });
  DuckEnv probe_env(make_task(o.task), 1);
  PpoConfig cfg;
  cfg.steps_per_env = o.steps;
  cfg.seed = o.seed;
  cfg.init_std = o.init_std;
  cfg.learning_rate = o.learning_rate;
  cfg.entropy_coef = o.entropy;
  cfg.critic_hidden = o.critic;
  cfg.learn_threads = threads;
  Ppo ppo(cfg, probe_env.obs_size(), probe_env.critic_size(), probe_env.action_size());
  // Warm start: the actor begins as Pollen's velstand network (it stands and balances); the jump's
  // phase arrives in slots that network never used.
  if (o.warm != "none" && o.resume.empty()) {
    std::shared_ptr<const game::WalkerPolicy> w;
    if (o.warm == "velstand") w = game::walker_policy("velstand.onnx");
    else {
      auto p = std::make_shared<game::WalkerPolicy>();
      const std::vector<u8> bytes = read_file(o.warm);
      if (!bytes.empty() && game::parse_onnx_policy(Span<const u8>(bytes.data(), bytes.size()), *p)) w = p;
    }
    if (!w || !ppo.warm_start(w->dims, w->weights, w->biases, w->mean, w->div, {game::kJumpPhaseSlot, game::kJumpPhaseSlot + 1})) {
      std::fprintf(stderr, "cannot warm-start from %s\n", o.warm.c_str());
      return 1;
    }
  }
  if (!o.resume.empty()) {
    const std::vector<u8> bytes = read_file(o.resume);
    if (bytes.empty() || !ppo.load(bytes.data(), bytes.size())) { std::fprintf(stderr, "cannot resume from %s\n", o.resume.c_str()); return 1; }
    std::printf("resumed at iteration %d (%lld steps)\n", ppo.iteration(), static_cast<long long>(ppo.total_steps()));
  }
  FILE* log = o.log.empty() ? nullptr : std::fopen(o.log.c_str(), o.resume.empty() ? "w" : "a");
  if (log && o.resume.empty())
    std::fprintf(log, "iteration,steps,seconds,mean_reward,mean_return,mean_length,episodes,value_loss,entropy,mean_std,learning_rate,steps_per_second,jumps,height_mm,flight_s,falls\n");
  std::printf("training %s: %d envs on %d threads, %d steps each per iteration, physics %d sub-steps, warm start %s\n", o.task.c_str(), o.envs,
              threads, o.steps, o.substeps, o.resume.empty() ? o.warm.c_str() : "(resumed)");
  const auto start = std::chrono::steady_clock::now();
  bool jumped = false;
  for (i32 it = ppo.iteration() + 1; it <= o.iterations; ++it) {
    const IterationStats s = train_iteration(ppo, backend);
    const f64 seconds = std::chrono::duration<f64>(std::chrono::steady_clock::now() - start).count();
    const f64 sps = static_cast<f64>(o.envs) * o.steps / (s.collect_seconds + s.learn_seconds);
    JumpReport rep;
    const bool evaluated = o.task == "jump" && o.eval_every > 0 && it % o.eval_every == 0;
    if (evaluated) rep = evaluate_jump(act_ppo, &ppo, 5, 1000, game::kWalkerSubsteps);
    std::printf("it %5d  %8.0f s  return %7.3f  len %5.1f  value %7.4f  std %.3f  lr %.1e  %5.0f steps/s (collect %.2fs, learn %.2fs)", s.iteration, seconds,
                s.mean_return, s.mean_length, s.value_loss, s.mean_std, s.learning_rate, sps, s.collect_seconds, s.learn_seconds);
    if (evaluated) std::printf("  | jumps %d/5 up %.1f mm flight %.2f s falls %d", rep.jumps, rep.height_mm, rep.flight_s, rep.falls);
    std::printf("\n");
    if (evaluated && rep.jumps >= 4 && !jumped) {
      jumped = true;
      std::printf("FIRST REAL JUMP at iteration %d: %lld steps, %.0f s (%d threads)\n", s.iteration, static_cast<long long>(s.steps), seconds, threads);
      write_file(o.out, export_onnx(ppo.actor(), ppo.actor_normalizer(), micro_duck_metadata(o.task)));
    }
    std::fflush(stdout);
    if (log) {
      std::fprintf(log, "%d,%lld,%.1f,%g,%g,%g,%d,%g,%g,%g,%g,%.0f,%d,%.1f,%.3f,%d\n", s.iteration, static_cast<long long>(s.steps), seconds, s.mean_reward,
                   s.mean_return, s.mean_length, s.episodes, s.value_loss, s.entropy, s.mean_std, s.learning_rate, sps, evaluated ? rep.jumps : -1,
                   rep.height_mm, rep.flight_s, rep.falls);
      std::fflush(log);
    }
    if (it % o.save_every == 0 || it == o.iterations) {
      if (!jumped || !o.stop_after_jump) write_file(o.out, export_onnx(ppo.actor(), ppo.actor_normalizer(), micro_duck_metadata(o.task)));
      if (!o.checkpoint.empty()) write_file(o.checkpoint, ppo.save());
    }
    if (jumped && o.stop_after_jump) break;
  }
  if (log) std::fclose(log);
  std::printf("wrote %s\n", o.out.c_str());
  return 0;
}
