// kern_lehrer: trains a Micro Duck policy headless on all cores (docs/lehrer.md).
//
//   kern/tools/lehrer.sh                      builds kern/build/kern_lehrer (native, optimised)
//   kern_lehrer --task jump --iterations 600 --out jump.onnx
//   kern_lehrer --eval jump.onnx               the jump on the clean robot in the Studio's physics
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
#include "kern/lehrer/backend.h"
#include "kern/lehrer/env.h"
#include "kern/lehrer/kalibrierung.h"
#include "kern/lehrer/onnx_export.h"
#include "kern/lehrer/ppo.h"
#include "kern/lehrer/recipe.h"

using namespace kern;
using namespace kern::lehrer;

namespace {

struct Options {
  std::string task = "jump", out, eval, log, checkpoint, resume, warm, trace;
  std::string calibrate, holdout, profile, compare, examples;
  i32 envs = 0, threads = 0, iterations = 600, steps = 0, save_every = 25, episodes = 10;
  i32 substeps = 0, eval_every = 10, stop_after_jump = 0, fingerprint = 0;
  f32 init_std = 0, learning_rate = 0, entropy = -1;  // 0 / -1: the recipe's
  std::vector<i32> critic;
  u64 seed = 1;
};

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

/// One clean episode of `policy`, tick by tick: the trunk's height, contacts and every reward term.
int trace(const Options& o) {
  game::WalkerPolicy p;
  std::string err;
  const std::vector<u8> bytes = read_file(o.trace);
  if (bytes.empty() || !game::parse_onnx_policy(Span<const u8>(bytes.data(), bytes.size()), p, &err)) {
    std::fprintf(stderr, "cannot read %s: %s\n", o.trace.c_str(), err.c_str());
    return 1;
  }
  DuckEnv env(make_task(o.task), 1000, game::kWalkerSubsteps);
  env.task().randomize = false;
  env.reset();
  const f32 z0 = env.probe().position.z;
  for (;;) {
    f32 a[kActions];
    p.run(env.observation(), a);
    const Env::Step s = env.step(a);
    const auto& q = env.probe();
    std::printf("%4d  z %+6.1f mm  feet %d%d body %d  up %.2f |", env.steps(), (q.position.z - z0) * 1000, q.foot_contact[0], q.foot_contact[1], q.body_contact,
                -q.gravity.z);
    for (const auto& [name, v] : env.task().terms)
      if (v != 0) std::printf(" %s %.3f", name, v);
    std::printf("\n");
    if (s.terminal || s.timeout) break;
  }
  std::printf("return %.3f\n", env.episode_return());
  return 0;
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

std::vector<std::string> split(const std::string& list) {
  std::vector<std::string> out;
  std::size_t at = 0;
  while (at <= list.size()) {
    const std::size_t end = list.find(',', at);
    const std::string item = list.substr(at, end == std::string::npos ? std::string::npos : end - at);
    if (!item.empty()) out.push_back(item);
    if (end == std::string::npos) break;
    at = end + 1;
  }
  return out;
}

bool load_profile(const Options& o, game::ServoProfile& p) {
  p = game::xl330_profile();
  if (o.profile.empty()) return true;
  const std::vector<u8> bytes = read_file(o.profile);
  std::string err;
  if (bytes.empty() || !game::servo_profile_from_json(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), p, &err)) {
    std::fprintf(stderr, "cannot read the profile %s: %s\n", o.profile.c_str(), err.c_str());
    return false;
  }
  return true;
}

bool load_bench(const std::string& list, std::vector<ServoLog>& out) {
  for (const std::string& f : split(list)) {
    const std::vector<u8> bytes = read_file(f);
    ServoLog l;
    std::string err;
    if (bytes.empty() || !parse_servo_log(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), l, &err)) {
      std::fprintf(stderr, "cannot read the bench log %s: %s\n", f.c_str(), err.c_str());
      return false;
    }
    l.name = f;
    out.push_back(std::move(l));
  }
  return true;
}

void print_profile(const game::ServoProfile& p) {
  std::printf("  K %.4f N*m/rad  D %.5f N*m*s/rad  F %.5f N*m  T %.4f N*m  armature %.6f kg*m^2  delay %.1f ms\n", p.stiffness, p.damping, p.friction,
              p.torque, p.armature, p.delay * 1000);
}

int calibrate(const Options& o) {
  game::ServoProfile start;
  if (!load_profile(o, start)) return 1;
  std::vector<ServoLog> fit, held;
  if (!load_bench(o.calibrate, fit) || !load_bench(o.holdout, held)) return 1;
  const i32 threads = o.threads > 0 ? o.threads : max_of(1, static_cast<i32>(std::thread::hardware_concurrency()));
  std::printf("calibrating %s on %zu bench log(s), %zu held out, %d threads\n", start.name.c_str(), fit.size(), held.size(), threads);
  const CalibrationReport r = calibrate_servo(start, fit, held, o.iterations > 0 && o.iterations != 600 ? o.iterations : 80, o.seed, threads);
  std::printf("fit: %.3f -> %.3f mrad rms;  held out: %.3f -> %.3f mrad rms (%d simulations)\n", r.fit_rms_before * 1000, r.fit_rms * 1000,
              r.holdout_rms_before * 1000, r.holdout_rms * 1000, r.evaluations);
  print_profile(r.profile);
  const std::string out = o.out.empty() ? start.name + "_calibrated.json" : o.out;
  const std::string text = game::to_json(r.profile);
  write_file(out, std::vector<u8>(text.begin(), text.end()));
  std::printf("wrote %s\n", out.c_str());
  return 0;
}

int compare(const Options& o) {
  game::ServoProfile servo;
  if (!load_profile(o, servo)) return 1;
  const std::vector<u8> bytes = read_file(o.compare);
  RobotLog log;
  std::string err;
  if (bytes.empty() || !parse_robot_log(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), log, &err)) {
    std::fprintf(stderr, "cannot read the robot log %s: %s\n", o.compare.c_str(), err.c_str());
    return 1;
  }
  const LogDelta d = compare_robot_log(log, servo);
  std::printf("%s against the simulation (%s), %d ticks (%.2f s):\n", o.compare.c_str(), servo.name.c_str(), d.ticks, d.ticks * kControlDt);
  std::printf("  gyro %.4f rad/s  gravity %.4f  joints %.4f rad  joint speeds %.3f rad/s (rms)\n", d.gyro, d.gravity, d.joint_pos, d.joint_vel);
  static const char* names[kActions] = {"left_hip_yaw", "left_hip_roll", "left_hip_pitch", "left_knee", "left_ankle", "neck_pitch", "head_pitch",
                                        "head_yaw", "head_roll", "right_hip_yaw", "right_hip_roll", "right_hip_pitch", "right_knee", "right_ankle"};
  for (i32 j = 0; j < kActions; ++j) std::printf("    %-16s %.4f rad\n", names[j], d.joint_pos_each[j]);
  return 0;
}

int examples(const Options& o) {
  const game::ServoProfile p = game::xl330_profile();
  auto bench = [&](const char* name, ServoLoad load, int kind) {
    ServoLog l;
    l.load = load;
    for (int i = 0; i < 480; ++i) {
      const f32 t = static_cast<f32>(i) * 0.005f;
      l.time.push_back(t);
      l.target.push_back(kind == 0 ? ((i / 80) % 2 ? 0.5f : -0.5f) : 0.5f * kern::dm::sin(6.2831853f * (0.5f + 0.6f * t) * t));
      l.angle.push_back(0);
    }
    l.angle = simulate_servo(p, l);
    const std::string text = servo_log_csv(l);
    write_file(o.examples + "/" + name, std::vector<u8>(text.begin(), text.end()));
  };
  bench("servo_steps.csv", {0.05f, 0.05f, 1e-5f, true}, 0);
  bench("servo_chirp.csv", {0.12f, 0.06f, 3e-5f, true}, 1);
  const std::string robot = robot_log_csv(simulated_robot_log(p, 250));
  write_file(o.examples + "/robot.csv", std::vector<u8>(robot.begin(), robot.end()));
  const std::string prof = game::to_json(p);
  write_file(o.examples + "/xl330.json", std::vector<u8>(prof.begin(), prof.end()));
  std::printf("wrote servo_steps.csv, servo_chirp.csv, robot.csv and xl330.json to %s\n", o.examples.c_str());
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
    else if (a == "--fingerprint") o.fingerprint = std::atoi(next());
    else if (a == "--trace") o.trace = next();
    else if (a == "--kalibrieren") o.calibrate = next();
    else if (a == "--pruefen") o.holdout = next();
    else if (a == "--profil") o.profile = next();
    else if (a == "--abgleich") o.compare = next();
    else if (a == "--beispiele") o.examples = next();
    else {
      std::fprintf(stderr,
                   "usage: kern_lehrer [--task jump|stand] [--envs N] [--threads N] [--iterations 600] [--steps 24] [--seed 1]\n"
                   "                  [--out task.onnx] [--log log.csv] [--checkpoint run.ckpt] [--resume run.ckpt] [--save-every 25]\n"
                   "                  [--warm-start velstand|none|file.onnx] [--init-std 0.3] [--lr 1e-3] [--entropy 0.005]\n"
                   "                  [--critic 256,128] [--substeps 16] [--eval-every 10] [--stop-after-jump]\n"
                   "       kern_lehrer --fingerprint 400       one state hash per control tick (determinism)\n"
                   "       kern_lehrer --eval policy.onnx [--episodes 10]\n"
                   "       kern_lehrer --trace policy.onnx [--task jump]   one clean episode tick by tick (height, contacts, reward terms)\n"
                   "       kern_lehrer --kalibrieren bench1.csv,bench2.csv [--pruefen held.csv] [--profil start.json] [--out servo.json]\n"
                   "       kern_lehrer --abgleich robot.csv [--profil servo.json]   how far the simulation is from a robot log\n"
                   "       kern_lehrer --beispiele DIR    example bench and robot logs (made in the simulation) in the formats above\n"
                   "       training with --profil servo.json builds the ducks with that servo profile\n");
      return 2;
    }
  }
  if (o.fingerprint > 0) {  // the determinism fingerprint (docs/lehrer.md): one hash per control tick
    const std::vector<u64> h = determinism_fingerprint(o.fingerprint);
    for (std::size_t i = 0; i < h.size(); ++i) std::printf("%zu %016llx\n", i, static_cast<unsigned long long>(h[i]));
    return 0;
  }
  if (!make_task(o.task)) { std::fprintf(stderr, "unknown task %s\n", o.task.c_str()); return 2; }
  if (!o.eval.empty()) return evaluate(o);
  if (!o.trace.empty()) return trace(o);
  if (!o.calibrate.empty()) return calibrate(o);
  if (!o.compare.empty()) return compare(o);
  if (!o.examples.empty()) return examples(o);
  if (o.out.empty()) o.out = o.task + ".onnx";
  const i32 threads = o.threads > 0 ? o.threads : max_of(1, static_cast<i32>(std::thread::hardware_concurrency()));
  if (o.envs <= 0) o.envs = 16 * threads;
  // The task's recipe (recipe.h; the browser trains with the same), then what the command line says.
  Recipe recipe = recipe_for(o.task);
  if (o.steps > 0) recipe.ppo.steps_per_env = o.steps;
  if (o.init_std > 0) recipe.ppo.init_std = o.init_std;
  if (o.learning_rate > 0) recipe.ppo.learning_rate = o.learning_rate;
  if (o.entropy >= 0) recipe.ppo.entropy_coef = o.entropy;
  if (!o.critic.empty()) recipe.ppo.critic_hidden = o.critic;
  if (o.substeps > 0) recipe.substeps = o.substeps;
  if (!o.warm.empty()) recipe.warm_start = o.warm;
  if (!load_profile(o, recipe.servo)) return 1;

  // The compute: this machine's threads, each stepping its own group of ducks (backend.h). The
  // result depends on the number of ducks, not on the number of threads.
  const i32 per_worker = max_of(1, o.envs / threads);
  o.envs = per_worker * threads;
  ThreadBackend backend(
      threads, per_worker, o.seed,
      [&](i32 first, i32 count, u64 seed) {
        std::vector<std::unique_ptr<Env>> v;
        for (i32 i = 0; i < count; ++i) v.push_back(make_env(recipe, seed, first + i));
        return v;
      },
      [&](Env& e, i32 it) { prepare_env(static_cast<DuckEnv&>(e), recipe, it); });
  std::unique_ptr<Ppo> learner = make_learner(recipe, o.seed);
  if (!learner) { std::fprintf(stderr, "cannot warm-start from %s\n", recipe.warm_start.c_str()); return 1; }
  Ppo& ppo = *learner;
  if (!o.resume.empty()) {
    const std::vector<u8> bytes = read_file(o.resume);
    if (bytes.empty() || !ppo.load(bytes.data(), bytes.size())) { std::fprintf(stderr, "cannot resume from %s\n", o.resume.c_str()); return 1; }
    std::printf("resumed at iteration %d (%lld steps)\n", ppo.iteration(), static_cast<long long>(ppo.total_steps()));
  }
  FILE* log = o.log.empty() ? nullptr : std::fopen(o.log.c_str(), o.resume.empty() ? "w" : "a");
  if (log && o.resume.empty())
    std::fprintf(log, "iteration,steps,seconds,mean_reward,mean_return,mean_length,episodes,value_loss,entropy,mean_std,learning_rate,steps_per_second,jumps,height_mm,flight_s,falls\n");
  std::printf("training %s: %d envs on %d threads, %d steps each per iteration, physics %d sub-steps, warm start %s\n", o.task.c_str(), o.envs,
              threads, recipe.ppo.steps_per_env, recipe.substeps, o.resume.empty() ? recipe.warm_start.c_str() : "(resumed)");
  const auto start = std::chrono::steady_clock::now();
  bool jumped = false;
  for (i32 it = ppo.iteration() + 1; it <= o.iterations; ++it) {
    const IterationStats s = train_iteration(ppo, backend);
    const f64 seconds = std::chrono::duration<f64>(std::chrono::steady_clock::now() - start).count();
    const f64 sps = static_cast<f64>(o.envs) * recipe.ppo.steps_per_env / (s.collect_seconds + s.learn_seconds);
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
