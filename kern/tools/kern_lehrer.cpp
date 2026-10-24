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
#include "kern/lehrer/vorbild.h"
#include "kern/lehrer/spielen.h"
#include "kern/core/json.h"
#include "kern/game/demos.h"
#include "kern/gfx/renderer.h"

using namespace kern;
using namespace kern::lehrer;

namespace {

struct Options {
  std::string task = "jump", out, eval, log, checkpoint, resume, warm, trace;
  std::string calibrate, holdout, profile, compare, examples, demo, demo_search, demo_check, play_eval;
  i32 play = 0;
  std::string policies;
  i32 rounds = 0, round_episodes = 0, keep_every = 0, randomized = 0;
  std::string play_net, play_practice;
  f32 practice_std = 0, imitate_reward = -1;
  i32 envs = 0, threads = 0, iterations = 600, steps = 0, save_every = 25, episodes = 10;
  i32 substeps = 0, eval_every = 10, stop_after_jump = 0, fingerprint = 0;
  f32 init_std = 0, learning_rate = 0, entropy = -1;  // 0 / -1: the recipe's
  std::vector<i32> critic, actor;
  i32 epochs = 0, minibatches = 0, adaptive = -1;
  f32 kl = 0;
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
  std::printf("%s: %d of %d episodes a real jump, trunk in the air %.1f mm above standing (best %.1f), rose %.1f mm, flight %.3f s (best %.3f), "
              "tilt %.0f deg, fell %d\n", what, r.jumps, r.episodes, r.height_mm, r.best_height_mm, r.rise_mm, r.flight_s, r.best_flight_s, r.tilt_deg, r.falls);
}

bool load_demo(const std::string& name, JumpDemo& d);

/// One clean episode of `policy`, tick by tick: the trunk's height, contacts and every reward term.
int trace(const Options& o) {
  game::WalkerPolicy p;
  std::string err;
  std::unique_ptr<JumpTeacher> teacher;  // --trace builtin / demo.json: the demonstration itself
  if (o.trace == "builtin" || (o.trace.size() > 5 && o.trace.substr(o.trace.size() - 5) == ".json")) {
    JumpDemo d;
    if (!load_demo(o.trace, d)) return 1;
    teacher = std::make_unique<JumpTeacher>(d);
  } else {
    const std::vector<u8> bytes = read_file(o.trace);
    if (bytes.empty() || !game::parse_onnx_policy(Span<const u8>(bytes.data(), bytes.size()), p, &err)) {
      std::fprintf(stderr, "cannot read %s: %s\n", o.trace.c_str(), err.c_str());
      return 1;
    }
  }
  DuckEnv env(make_task(o.task), 1000 + o.seed, game::kWalkerSubsteps);
  env.task().randomize = o.randomized != 0;
  if (o.randomized) env.task().progress(100000);
  env.reset();
  const f32 z0 = env.probe().position.z;
  for (;;) {
    f32 a[kActions];
    if (teacher) teacher->act(env.observation(), env.probe(), env.steps(), a);
    else p.run(env.observation(), a);
    const Env::Step s = env.step(a);
    const auto& q = env.probe();
    std::printf("%4d  z %+6.1f mm  feet %d%d body %d  up %.2f  tilt x %+.2f y %+.2f |", env.steps(), (q.position.z - z0) * 1000, q.foot_contact[0], q.foot_contact[1],
                q.body_contact, -q.gravity.z, q.gravity.x, q.gravity.y);
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
  print_report((o.eval + " (clean robot)").c_str(), evaluate_jump(act_onnx, &p, o.episodes, 1000, game::kWalkerSubsteps));
  print_report((o.eval + " (randomised: build, servos, sensors, start)").c_str(), evaluate_jump(act_onnx, &p, o.episodes, 5000, game::kWalkerSubsteps, true));
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

bool load_demo(const std::string& name, JumpDemo& d) {
  if (name == "builtin") { d = builtin_jump_demo(); return true; }
  const std::vector<u8> bytes = read_file(name);
  std::string err;
  if (bytes.empty() || !jump_demo_from_json(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), d, &err)) {
    std::fprintf(stderr, "cannot read the demonstration %s: %s\n", name.c_str(), err.c_str());
    return false;
  }
  return true;
}

int demo_search(const Options& o) {
  const i32 threads = o.threads > 0 ? o.threads : max_of(1, static_cast<i32>(std::thread::hardware_concurrency()));
  const i32 gens = o.iterations > 0 && o.iterations != 600 ? o.iterations : 120;
  std::printf("searching the jump's demonstration: CMA-ES, %d generations, %d threads\n", gens, threads);
  JumpDemo start;
  if (!o.demo.empty() && !load_demo(o.demo, start)) return 1;
  const JumpDemo d = search_jump_demo(gens, o.seed, threads, o.demo.empty() ? nullptr : &start, [](i32 g, f32 score, const JumpReport& r) {
    std::printf("gen %4d  score %6.1f | clean: up %.1f mm, rose %.1f mm, flight %.0f ms, tilt %.0f deg, %s\n", g, score, r.height_mm, r.rise_mm, r.flight_s * 1000, r.tilt_deg,
                r.falls ? "fell" : r.jumps ? "a real jump" : "no real jump");
    std::fflush(stdout);
  });
  const std::string text = to_json(d);
  write_file(o.demo_search, std::vector<u8>(text.begin(), text.end()));
  print_report("the demonstration", evaluate_jump_demo(d, 10, 1000));
  std::printf("wrote %s\n", o.demo_search.c_str());
  return 0;
}

int demo_check(const Options& o) {
  JumpDemo d;
  if (!load_demo(o.demo_check, d)) return 1;
  print_report("the demonstration (clean)", evaluate_jump_demo(d, o.episodes, 1000));
  print_report("the demonstration (randomised)", evaluate_jump_demo(d, o.episodes, 5000, true));
  std::printf("score over 10 episodes (1 clean, 9 randomised): %.1f\n", score_jump_demo(d, 10, 100));
  return 0;
}

void print_play(const char* what, const PlayReport& r) {
  std::printf("%s: ball kicked away %d of %d (%.1f s), sock picked up %d / carried off and spat out %d of %d (%.1f s; %d of %d picks held), fell %d of %d\n",
              what, r.kicked, r.ball_episodes, r.ball_seconds, r.picked, r.delivered, r.sock_episodes, r.sock_seconds, r.picks_held, r.pick_attempts,
              r.falls, r.episodes);
}

/// --spielen-pruefen vorbild|play.onnx: how the demonstration or a play network does.
int play_check(const Options& o) {
  const i32 threads = o.threads > 0 ? o.threads : max_of(1, static_cast<i32>(std::thread::hardware_concurrency()));
  const ParallelFor pool = thread_pool(threads);
  if (o.play_eval == "vorbild") {
    print_play("the demonstration (clean)", evaluate_play(nullptr, nullptr, o.episodes, 1000, false, pool));
    print_play("the demonstration (randomised)", evaluate_play(nullptr, nullptr, o.episodes, 5000, true, pool));
    return 0;
  }
  game::WalkerPolicy p;
  std::string err;
  const std::vector<u8> bytes = read_file(o.play_eval);
  if (bytes.empty() || !game::parse_onnx_policy(Span<const u8>(bytes.data(), bytes.size()), p, &err)) {
    std::fprintf(stderr, "cannot read %s: %s\n", o.play_eval.c_str(), err.c_str());
    return 1;
  }
  print_play((o.play_eval + " (clean)").c_str(), evaluate_play(act_onnx, &p, o.episodes, 1000, false, pool));
  print_play((o.play_eval + " (randomised)").c_str(), evaluate_play(act_onnx, &p, o.episodes, 5000, true, pool));
  return 0;
}

/// --spielen-spur [--seed N] [--randomized] [--netz play.onnx]: one episode, decision by decision (the
/// demonstration drives; with --netz the network does, and the line shows both answers).
int play_trace(const Options& o) {
  PlayEnv env(1000 + o.seed, o.randomized != 0);
  env.reset();
  // --netz play.onnx: the network drives; the line shows the demonstration's answer, then the network's.
  game::WalkerPolicy net;
  bool has_net = false;
  if (!o.play_net.empty()) {
    std::string err;
    const std::vector<u8> bytes = read_file(o.play_net);
    if (bytes.empty() || !game::parse_onnx_policy(Span<const u8>(bytes.data(), bytes.size()), net, &err)) {
      std::fprintf(stderr, "cannot read %s: %s\n", o.play_net.c_str(), err.c_str());
      return 1;
    }
    has_net = true;
  }
  for (;;) {
    f32 a[kPlayActions], n[kPlayActions];
    env.teacher(a);
    if (has_net) act_onnx(&net, env.observation(), n);
    const f32* ob = env.observation();
    const f32* q = env.critic() + kPlayObs;
    std::printf("ball %d (%+.2f %+.2f) true (%+.2f %+.2f) | sock %d (%+.2f %+.2f) true (%+.2f %+.2f) | foot %+.3f %+.3f hold %.0f pick %.0f/%.2f kick %.0f carry %.2f | a",
                static_cast<int>(ob[0]), ob[1], ob[2], q[1], q[2], static_cast<int>(ob[4]), ob[5], ob[6], q[4], q[5], ob[8], ob[9], ob[10], ob[13], ob[15], ob[14], ob[19]);
    for (f32 v : a) std::printf(" %+.2f", v);
    if (has_net) {
      std::printf(" | net");
      for (f32 v : n) std::printf(" %+.2f", v);
      std::printf(" still %.2f", env.observation()[21]);
    }
    game::GameWorld::WalkerProbe wp;
    env.world().walker_probe(env.duck(), wp);
    std::printf(" | moved %.3f body %d %s/%s grav %.2f", env.sock_moved(), wp.body_contact ? 1 : 0, env.world().walker_driving(env.duck()).c_str(), env.world().walker_state(env.duck()).c_str(), wp.gravity.z);
    std::printf("\n");
    const Env::Step s = env.step(has_net ? n : a);
    if (s.terminal || s.timeout) break;
  }
  const PlayEnv::Outcome& r = env.outcome();
  std::printf("ball %d kicked %d | sock %d picked %d delivered %d | fell %d after %.1f s\n", r.has_ball, r.kicked, r.has_sock, r.picked, r.delivered, r.fell, r.seconds);
  return 0;
}

/// --spielen [--nachahmen 12] [--episoden 32]: the play network, imitating the demonstration (DAgger).
int play_train(const Options& o) {
  const i32 threads = o.threads > 0 ? o.threads : max_of(1, static_cast<i32>(std::thread::hardware_concurrency()));
  const ParallelFor pool = thread_pool(threads);
  std::unique_ptr<Ppo> learner = make_play_learner(o.seed);
  const i32 rounds = o.rounds > 0 ? o.rounds : 8, episodes = o.round_episodes > 0 ? o.round_episodes : 64;
  const std::string out = o.out.empty() || o.out == "jump.onnx" ? "play.onnx" : o.out;
  std::printf("the play network: imitating the demonstration, %d rounds of %d episodes, %d threads\n", rounds, episodes, threads);
  const auto t0 = std::chrono::steady_clock::now();
  const BiteSpot bite = bite_spot(pool);
  std::printf("  the bite, measured in this physics: the sock %.1f cm ahead, %+.1f cm left (%d places around it held)\n", bite.ahead * 100, bite.left * 100, bite.kept);
  std::fflush(stdout);
  imitate_play(*learner, rounds, episodes, o.seed, pool, [&](i32 round, const PlayImitationReport& r) {
    std::printf("  round %d (%.0f s): %d samples, error %.5f | ", round, std::chrono::duration<f64>(std::chrono::steady_clock::now() - t0).count(), r.samples, r.mse);
    print_play("the network (clean robot)", r.student);
    write_file(out, export_onnx(learner->actor(), learner->actor_normalizer(), play_metadata()));
    if (!o.checkpoint.empty()) write_file(o.checkpoint, learner->save());
    std::fflush(stdout);
  });
  std::printf("wrote %s\n", out.c_str());
  return 0;
}

/// --spielen-ueben play.onnx: the imitated network practises on its own (PPO): rewarded only for what
/// works - toward the target, the sock up and still in the beak, carried off, put down; the ball
/// kicked away; a fall costs. Where it bites, how it stands, it finds where it really works.
int play_practice(const Options& o) {
  const i32 threads = o.threads > 0 ? o.threads : max_of(1, static_cast<i32>(std::thread::hardware_concurrency()));
  const i32 envs = o.envs > 0 ? o.envs : 16 * threads, per_worker = max_of(1, envs / threads);
  const i32 iterations = o.iterations > 0 && o.iterations != 600 ? o.iterations : 300;
  const std::string out = o.out.empty() || o.out == "jump.onnx" ? "play_geuebt.onnx" : o.out;
  const std::string stem = out.size() > 5 && out.substr(out.size() - 5) == ".onnx" ? out.substr(0, out.size() - 5) : out;
  game::WalkerPolicy start;
  std::string err;
  const std::vector<u8> bytes = read_file(o.play_practice);
  if (bytes.empty() || !game::parse_onnx_policy(Span<const u8>(bytes.data(), bytes.size()), start, &err)) {
    std::fprintf(stderr, "cannot read %s: %s\n", o.play_practice.c_str(), err.c_str());
    return 1;
  }
  std::unique_ptr<Ppo> learner = make_play_learner(o.seed);
  if (!learner->warm_start(start.dims, start.weights, start.biases, start.mean, start.div, {})) {
    std::fprintf(stderr, "%s does not fit the play network\n", o.play_practice.c_str());
    return 1;
  }
  Ppo& ppo = *learner;
  ppo.set_stddev(o.practice_std > 0 ? o.practice_std : 0.1f);
  ThreadBackend backend(threads, per_worker, o.seed, [](i32 first, i32 count, u64 seed) {
    std::vector<std::unique_ptr<Env>> v;
    for (i32 i = 0; i < count; ++i) {
      auto e = std::make_unique<PlayEnv>(seed * 1000003ull + static_cast<u64>(first + i), (first + i) % 2 == 1);  // half clean, half varied
      e->reset();
      v.push_back(std::move(e));
    }
    return v;
  });
  const BiteSpot bite = bite_spot(backend.update_parallel());
  (void)bite;
  constexpr i32 kCriticWarmup = 20;  // the critic learns the returns before the actor moves
  std::printf("practising %s: %d envs on %d threads, %d iterations\n", o.play_practice.c_str(), per_worker * threads, threads, iterations);
  const auto t0 = std::chrono::steady_clock::now();
  for (i32 it = 1; it <= iterations; ++it) {
    ppo.set_actor_frozen(it <= kCriticWarmup);
    const IterationStats s = train_iteration(ppo, backend);
    std::printf("it %4d %6.0f s  return %7.3f  len %6.1f  value %7.4f  std %.3f\n", it, std::chrono::duration<f64>(std::chrono::steady_clock::now() - t0).count(),
                s.mean_return, s.mean_length, s.value_loss, s.mean_std);
    if (it % 20 == 0 || it == iterations) {
      game::WalkerPolicy p;
      const std::vector<u8> onnx = export_onnx(ppo.actor(), ppo.actor_normalizer(), play_metadata());
      game::parse_onnx_policy(Span<const u8>(onnx.data(), onnx.size()), p, &err);
      print_play("  the network (clean robot)", evaluate_play(act_onnx, &p, 16, 99991, false, backend.update_parallel()));
      char name[32];
      std::snprintf(name, sizeof name, "_it%04d.onnx", it);
      write_file(stem + name, onnx);
      write_file(out, onnx);
    }
    std::fflush(stdout);
  }
  std::printf("wrote %s\n", out.c_str());
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

/// --demo-spur [--netz play.onnx] [--seed SECONDS]: the Studio's Micro Duck example level, played
/// as the Studio plays it (60 Hz frames); once a second what the duck does.
int demo_trace(const Options& o) {
  // Kern's networks as the Studio loads them: kern/policies/ (or --netz for the play network).
  std::string dir = __FILE__;
  dir = dir.substr(0, dir.find_last_of('/')) + "/../policies/";
  for (const char* file : {"jump.onnx", "play.onnx"}) {
    const std::string path = std::string(file) == "play.onnx" && !o.play_net.empty() ? o.play_net : dir + file;
    auto p = std::make_shared<game::WalkerPolicy>();
    std::string err;
    const std::vector<u8> bytes = read_file(path);
    if (bytes.empty() || !game::parse_onnx_policy(Span<const u8>(bytes.data(), bytes.size()), *p, &err)) { std::fprintf(stderr, "%s: %s\n", path.c_str(), err.c_str()); return 1; }
    p->file = file;
    game::register_walker_policy(p);
  }
  Renderer renderer;
  renderer.set_headless();
  game::GameWorld w(renderer);
  game::Level l = game::micro_duck_level();
  w.sync(l);
  if (!w.start(l, game::SimMode::play)) return 1;
  const game::ActorId duck = l.world.player;
  const i32 secs = o.seed > 0 ? static_cast<i32>(o.seed) : 120;
  for (i32 i = 0; i < 60 * secs; ++i) {
    game::PlayInput in;
    if (std::getenv("KERN_JUMP_AT") && i / 60 == std::atoi(std::getenv("KERN_JUMP_AT")) && i % 60 < 3) in.robot = 256;  // key 7
    w.step(1.0f / 60, in);
    if (i % 60) continue;
    Mat4 m;
    w.actor_matrix(duck, m);
    f32 ob[game::GameWorld::kPlayObs] = {};
    w.walker_play_observation(duck, ob);
    std::printf("t %3d pos %+.2f %+.2f %-10s %-12s ball %d (%+.2f %+.2f) sock %d (%+.2f %+.2f) hold %d still %.1f walk %+.1f\n", i / 60,
                m.translation().x, m.translation().y, w.walker_state(duck).c_str(), w.walker_driving(duck).c_str(), static_cast<int>(ob[0]), ob[1], ob[2],
                static_cast<int>(ob[4]), ob[5], ob[6], static_cast<int>(ob[10]), ob[21], ob[22]);
  }
  return 0;
}

}  // namespace

/// Pollen's moves beyond velstand (pick, kicks, sit ...): the robot's own ONNX files, from the
/// repository (third_party/microduck) or --policies DIR.
void load_pollen_policies(const std::string& dir_in) {
  std::string dir = dir_in;
  if (dir.empty()) {
    dir = __FILE__;
    dir = dir.substr(0, dir.find_last_of('/')) + "/../third_party/microduck/";
  }
  if (!dir.empty() && dir.back() != '/') dir += '/';
  json::Value m;
  if (!json::parse(game::micro_duck_manifest(), m)) return;
  for (const json::Value& p : m["policies"].items()) {
    const std::string file = p["file"].as_string();
    if (game::walker_policy(file)) continue;
    const std::vector<u8> bytes = read_file(dir + file);
    auto pol = std::make_shared<game::WalkerPolicy>();
    if (bytes.empty() || !game::parse_onnx_policy(Span<const u8>(bytes.data(), bytes.size()), *pol)) continue;
    pol->file = file;
    game::register_walker_policy(pol);
  }
}

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
    else if (a == "--actor") o.actor = parse_dims(next());
    else if (a == "--epochs") o.epochs = std::atoi(next());
    else if (a == "--minibatches") o.minibatches = std::atoi(next());
    else if (a == "--adaptive-lr") o.adaptive = std::atoi(next());
    else if (a == "--kl") o.kl = static_cast<f32>(std::atof(next()));
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
    else if (a == "--vorbild") o.demo = next();
    else if (a == "--vorbild-suchen") o.demo_search = next();
    else if (a == "--vorbild-pruefen") o.demo_check = next();
    else if (a == "--nachahmen") o.rounds = std::atoi(next());
    else if (a == "--episoden") o.round_episodes = std::atoi(next());
    else if (a == "--uebungs-std") o.practice_std = static_cast<f32>(std::atof(next()));
    else if (a == "--keep-every") o.keep_every = std::atoi(next());
    else if (a == "--randomized") o.randomized = 1;
    else if (a == "--spielen-pruefen") o.play_eval = next();
    else if (a == "--spielen-ueben") o.play_practice = next();
    else if (a == "--spielen") o.play = 1;
    else if (a == "--policies") o.policies = next();
    else if (a == "--spielen-spur") o.play = 2;
    else if (a == "--netz") o.play_net = next();
    else if (a == "--biss-fenster") o.play = 3;
    else if (a == "--halt-messen") o.play = 4;
    else if (a == "--demo-spur") o.play = 6;
    else if (a == "--nachahmungs-lohn") o.imitate_reward = static_cast<f32>(std::atof(next()));
    else {
      std::fprintf(stderr,
                   "usage: kern_lehrer [--task jump|stand] [--envs N] [--threads N] [--iterations 600] [--steps 24] [--seed 1]\n"
                   "                  [--out task.onnx] [--log log.csv] [--checkpoint run.ckpt] [--resume run.ckpt] [--save-every 25]\n"
                   "                  [--warm-start velstand|none|file.onnx] [--init-std 0.3] [--lr 1e-3] [--entropy 0.005]\n"
                   "                  [--critic 256,128] [--actor 512,256,128] [--epochs 5] [--minibatches 4] [--adaptive-lr 1] [--kl 0.01]\n"
                   "                  [--substeps 16] [--eval-every 10] [--stop-after-jump] [--profil servo.json]\n"
                   "       kern_lehrer --fingerprint 400       one state hash per control tick (determinism)\n"
                   "       kern_lehrer --eval policy.onnx [--episodes 10]\n"
                   "       kern_lehrer --trace policy.onnx [--task jump]   one clean episode tick by tick (height, contacts, reward terms)\n"
                   "       kern_lehrer --kalibrieren bench1.csv,bench2.csv [--pruefen held.csv] [--profil start.json] [--out servo.json]\n"
                   "       kern_lehrer --abgleich robot.csv [--profil servo.json]   how far the simulation is from a robot log\n"
                   "       kern_lehrer --beispiele DIR    example bench and robot logs (made in the simulation) in the formats above\n"
                   "       training with --profil servo.json builds the ducks with that servo profile\n"
                   "       kern_lehrer --vorbild-suchen demo.json [--iterations 120]   search the jump's demonstration (CMA-ES)\n"
                   "       kern_lehrer --vorbild-pruefen builtin|demo.json             how the demonstration jumps\n"
                   "       training with --vorbild builtin|demo.json [--nachahmen 5] [--episoden 64] [--uebungs-std 0.2]: imitate first, then practise\n"
                   "       kern_lehrer --spielen [--nachahmen 12] [--episoden 32] [--out play.onnx]   the play network (ball, sock): imitation\n"
                   "       kern_lehrer --spielen-pruefen vorbild|play.onnx [--episodes 24]   how the demonstration or a network plays\n"
                   "       kern_lehrer --spielen-ueben play.onnx [--iterations 300] [--envs 32] [--out play_geuebt.onnx]   then practise (PPO, the rewards only)\n"
                   "       kern_lehrer --spielen-spur [--seed N] [--randomized] [--netz play.onnx]   one play episode, decision by decision\n"
                   "       kern_lehrer --biss-fenster | --halt-messen   where the beak gets a sock / how far the duck goes after stop\n");
      return 2;
    }
  }
  load_pollen_policies(o.policies);
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
  if (!o.demo_search.empty()) return demo_search(o);
  if (!o.demo_check.empty()) return demo_check(o);
  if (!o.play_eval.empty()) return play_check(o);
  if (!o.play_practice.empty()) return play_practice(o);
  if (o.play == 2) return play_trace(o);
  if (o.play == 3) { measure_bite(kPlayBeakOpen, kPlayBeakClose, true, thread_pool(max_of(1, static_cast<i32>(std::thread::hardware_concurrency())))); return 0; }
  if (o.play == 4) { measure_stop(); return 0; }
  if (o.play == 6) return demo_trace(o);
  if (o.play) return play_train(o);
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
  if (!o.actor.empty()) recipe.ppo.actor_hidden = o.actor;
  if (o.epochs > 0) recipe.ppo.epochs = o.epochs;
  if (o.minibatches > 0) recipe.ppo.minibatches = o.minibatches;
  if (o.adaptive >= 0) recipe.ppo.adaptive_lr = o.adaptive != 0;
  if (o.kl > 0) recipe.ppo.desired_kl = o.kl;
  if (o.substeps > 0) recipe.substeps = o.substeps;
  if (!o.warm.empty()) recipe.warm_start = o.warm;
  if (!load_profile(o, recipe.servo)) return 1;
  if (!o.demo.empty()) {
    if (o.demo == "none") recipe.imitate = false;
    else if (!load_demo(o.demo, recipe.demo)) return 1;
    else recipe.imitate = true;
  }
  if (o.rounds > 0) recipe.imitation_rounds = o.rounds;
  if (o.round_episodes > 0) recipe.imitation_episodes = o.round_episodes;
  if (o.practice_std > 0) recipe.practice_std = o.practice_std;
  if (o.imitate_reward >= 0) recipe.imitate_reward = o.imitate_reward;

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
  if (recipe.imitate && o.resume.empty()) {
    // Imitate the demonstration first (vorbild.h), then practise with PPO.
    std::printf("imitating the demonstration: %d rounds of %d episodes\n", recipe.imitation_rounds, recipe.imitation_episodes);
    const auto t0 = std::chrono::steady_clock::now();
    const ImitationReport r = imitate_jump(ppo, recipe, o.seed, backend.update_parallel(),
                                           [&](i32 round, const ImitationReport& rr) {
                                             std::printf("  round %d (%.0f s): %d samples, error %.5f | ", round,
                                                         std::chrono::duration<f64>(std::chrono::steady_clock::now() - t0).count(), rr.samples, rr.mse);
                                             print_report("the network", rr.student);
                                             std::fflush(stdout);
                                           });
    std::printf("imitation done in %.0f s\n", std::chrono::duration<f64>(std::chrono::steady_clock::now() - t0).count());
    begin_practice(ppo, recipe);
    (void)r;
    // What imitation alone gives (and where practice can start again from: --resume).
    const std::string stem = o.out.size() > 5 && o.out.substr(o.out.size() - 5) == ".onnx" ? o.out.substr(0, o.out.size() - 5) : o.out;
    write_file(stem + "_nachgeahmt.onnx", export_onnx(ppo.actor(), ppo.actor_normalizer(), micro_duck_metadata(o.task)));
    if (!o.checkpoint.empty()) write_file(o.checkpoint + ".nachgeahmt", ppo.save());
  }
  FILE* log = o.log.empty() ? nullptr : std::fopen(o.log.c_str(), o.resume.empty() ? "w" : "a");
  if (log && o.resume.empty())
    std::fprintf(log, "iteration,steps,seconds,mean_reward,mean_return,mean_length,episodes,value_loss,entropy,mean_std,learning_rate,steps_per_second,jumps,height_mm,flight_s,falls\n");
  std::printf("training %s: %d envs on %d threads, %d steps each per iteration, physics %d sub-steps, warm start %s\n", o.task.c_str(), o.envs,
              threads, recipe.ppo.steps_per_env, recipe.substeps, o.resume.empty() ? recipe.warm_start.c_str() : "(resumed)");
  const auto start = std::chrono::steady_clock::now();
  bool jumped = false;
  for (i32 it = ppo.iteration() + 1; it <= o.iterations; ++it) {
    before_learning(ppo, recipe);
    const IterationStats s = train_iteration(ppo, backend);
    const f64 seconds = std::chrono::duration<f64>(std::chrono::steady_clock::now() - start).count();
    const f64 sps = static_cast<f64>(o.envs) * recipe.ppo.steps_per_env / (s.collect_seconds + s.learn_seconds);
    JumpReport rep;
    const bool evaluated = o.task == "jump" && o.eval_every > 0 && it % o.eval_every == 0;
    if (evaluated) rep = evaluate_jump(act_ppo, &ppo, 5, 1000, game::kWalkerSubsteps);
    std::printf("it %5d  %8.0f s  return %7.3f  len %5.1f  value %7.4f  std %.3f  lr %.1e  %5.0f steps/s (collect %.2fs, learn %.2fs)", s.iteration, seconds,
                s.mean_return, s.mean_length, s.value_loss, s.mean_std, s.learning_rate, sps, s.collect_seconds, s.learn_seconds);
    if (evaluated) std::printf("  | jumps %d/5 up %.1f mm rise %.1f mm flight %.2f s tilt %.0f falls %d", rep.jumps, rep.height_mm, rep.rise_mm, rep.flight_s, rep.tilt_deg, rep.falls);
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
    if (o.keep_every > 0 && it % o.keep_every == 0) {  // snapshots to choose from afterwards
      char name[64];
      std::snprintf(name, sizeof name, "_it%05d.onnx", it);
      const std::string stem = o.out.size() > 5 && o.out.substr(o.out.size() - 5) == ".onnx" ? o.out.substr(0, o.out.size() - 5) : o.out;
      write_file(stem + name, export_onnx(ppo.actor(), ppo.actor_normalizer(), micro_duck_metadata(o.task)));
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
