// Calibration of the digital twin (kalibrierung.h, docs/kalibrierung.md).
#include "kern/lehrer/kalibrierung.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "kern/game/walker.h"
#include "kern/game/walker_policy.h"
#include "kern/lehrer/cmaes.h"
#include "kern/lehrer/ppo.h"
#include "kern/physics/physics.h"

namespace kern::lehrer {

namespace {

/// Splits CSV text into lines of numbers; comment lines ("# key value") go to `meta`. Lines that do
/// not start with a number (a header) are skipped.
bool read_csv(std::string_view text, std::vector<std::vector<f64>>& rows, std::vector<std::pair<std::string, f64>>* meta, std::string* error) {
  std::size_t at = 0;
  i32 line_no = 0;
  while (at < text.size()) {
    std::size_t end = text.find('\n', at);
    if (end == std::string_view::npos) end = text.size();
    std::string line(text.substr(at, end - at));
    at = end + 1;
    ++line_no;
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    if (line.empty()) continue;
    if (line[0] == '#') {
      char key[64] = {};
      double value = 0;
      if (meta && std::sscanf(line.c_str(), "# %63s %lf", key, &value) == 2) meta->push_back({key, value});
      continue;
    }
    const char c0 = line[0];
    if (!(c0 == '-' || c0 == '+' || c0 == '.' || (c0 >= '0' && c0 <= '9'))) continue;  // the header
    std::vector<f64> row;
    const char* p = line.c_str();
    while (*p) {
      char* next = nullptr;
      const double v = std::strtod(p, &next);
      if (next == p || !std::isfinite(v)) {
        if (error) *error = "line " + std::to_string(line_no) + ": not a number";
        return false;
      }
      row.push_back(v);
      p = next;
      while (*p == ',' || *p == ' ' || *p == '\t' || *p == ';') ++p;
    }
    rows.push_back(std::move(row));
  }
  return true;
}

constexpr f32 kS = game::kWalkerScale;

/// Real seconds per physics step of the walker world (5 ms at 100 Hz and scale 4).
f32 step_seconds() { return 1.0f / (game::kWalkerStepHz * std::sqrt(kS)); }

}  // namespace

bool parse_servo_log(std::string_view csv, ServoLog& out, std::string* error) {
  std::vector<std::vector<f64>> rows;
  std::vector<std::pair<std::string, f64>> meta;
  if (!read_csv(csv, rows, &meta, error)) return false;
  ServoLog log;
  for (const auto& [k, v] : meta) {
    if (k == "mass") log.load.mass = static_cast<f32>(v);
    else if (k == "arm") log.load.arm = static_cast<f32>(v);
    else if (k == "inertia") log.load.inertia = static_cast<f32>(v);
    else if (k == "horizontal") log.load.horizontal_axis = v != 0;
  }
  for (const auto& r : rows) {
    if (r.size() < 3) {
      if (error) *error = "each line needs time, target, angle";
      return false;
    }
    if (!log.time.empty() && r[0] < log.time.back()) {
      if (error) *error = "time goes backwards";
      return false;
    }
    log.time.push_back(static_cast<f32>(r[0]));
    log.target.push_back(static_cast<f32>(r[1]));
    log.angle.push_back(static_cast<f32>(r[2]));
  }
  if (log.time.size() < 2 || log.load.mass <= 0 || log.load.inertia < 0) {
    if (error) *error = log.time.size() < 2 ? "too few samples" : "bad load";
    return false;
  }
  out = std::move(log);
  return true;
}

std::string servo_log_csv(const ServoLog& log) {
  std::string s;
  char buf[160];
  std::snprintf(buf, sizeof buf, "# mass %.9g\n# arm %.9g\n# inertia %.9g\n# horizontal %d\ntime,target,angle\n", log.load.mass, log.load.arm,
                log.load.inertia, log.load.horizontal_axis ? 1 : 0);
  s += buf;
  for (std::size_t i = 0; i < log.time.size(); ++i) {
    std::snprintf(buf, sizeof buf, "%.9g,%.9g,%.9g\n", log.time[i], log.target[i], log.angle[i]);
    s += buf;
  }
  return s;
}

std::vector<f32> simulate_servo(const game::ServoProfile& p, const ServoLog& log) {
  // The rig at the walker scale, with the walker's step rate, sub-steps and servo-joint softness
  // (GameWorld builds servo joints so), the servo law of the robot (game::servo_step).
  using namespace physics;
  const f32 s = kS, s3 = s * s * s, s4 = s3 * s, s5 = s4 * s, rs = std::sqrt(s);
  const f32 hz = game::kWalkerStepHz;
  const i32 substeps = game::kWalkerSubsteps;
  World w;
  WorldSettings ws;
  ws.enable_sleep = false;
  ws.gravity = log.load.horizontal_axis ? Vec3{0, 0, -9.80665f} : Vec3{0, 0, 0};
  w.create(ws);
  BodySettings bs;
  bs.type = BodyType::fixed;
  const Body base = w.create_body(bs);
  const Vec3 axis{0, 1, 0};
  const f32 a0 = log.angle.empty() ? 0.0f : log.angle[0];
  BodySettings ls;
  ls.rotation = Quat::axis_angle(axis, a0);
  ls.enable_sleep = false;
  const Body link = w.create_body(ls);
  ShapeSettings sh;  // a token shape (it touches nothing); the mass comes from the load
  sh.kind = ShapeKind::sphere;
  sh.radius = 0.005f * s;
  sh.center = Vec3{0, 0, -log.load.arm} * s;
  sh.material.mask_bits = 0;
  w.create_shape(link, sh);
  // The arm's inertia about its centre of mass plus the rotor (armature) as on the duck's links: a
  // disc on the axis, `armature` about it and half across.
  const f32 i0 = log.load.inertia;
  f32 inertia[6] = {i0 + 0.5f * p.armature, i0 + p.armature, i0 + 0.5f * p.armature, 0, 0, 0};
  for (f32& v : inertia) v *= s5;
  w.set_mass_data(link, log.load.mass * s3, Vec3{0, 0, -log.load.arm} * s, inertia);
  JointSettings j;
  j.body_a = base;
  j.body_b = link;
  j.frame_a = frame_from_axis({}, axis, false);
  j.frame_b = frame_from_axis({}, axis, false);
  j.constraint_hertz = min_of(240.0f, 0.5f * hz * static_cast<f32>(substeps));
  j.constraint_damping_ratio = 2;
  const Joint joint = w.create_joint(j);
  // As GameWorld feeds it: the command `delay` steps late, held between samples.
  const f32 dt = step_seconds();
  const i32 delay = max_of(0, static_cast<i32>(std::lround(p.delay / dt)));
  const f32 k = p.stiffness * s4, d = p.damping * s4 * rs, f = p.friction * s4, t = p.torque * s4;
  std::vector<f32> out(log.time.size(), a0);
  std::size_t k_out = 0, k_cmd = 0;
  while (k_out < log.time.size() && log.time[k_out] <= 0) ++k_out;
  const f32 t_end = log.time.back();
  for (i32 n = 0; static_cast<f32>(n) * dt <= t_end + dt; ++n) {
    const f32 sent = static_cast<f32>(n - delay) * dt;  // the command that arrives now was sent then
    while (k_cmd + 1 < log.time.size() && log.time[k_cmd + 1] <= sent + 1e-6f) ++k_cmd;
    const f32 target = sent < log.time[0] ? log.target[0] : log.target[k_cmd];
    game::servo_step(w, joint, target, k, d, f, t);
    w.step(1.0f / hz, substeps);
    const f32 now = static_cast<f32>(n + 1) * dt;
    const f32 q = w.joint_angle(joint);
    while (k_out < log.time.size() && log.time[k_out] <= now + 1e-6f) out[k_out++] = q;
  }
  return out;
}

f32 servo_error(const game::ServoProfile& p, const ServoLog& log) {
  const std::vector<f32> sim = simulate_servo(p, log);
  f64 e = 0;
  for (std::size_t i = 0; i < sim.size(); ++i) e += static_cast<f64>(sim[i] - log.angle[i]) * (sim[i] - log.angle[i]);
  return sim.empty() ? 0.0f : static_cast<f32>(std::sqrt(e / static_cast<f64>(sim.size())));
}

namespace {

f32 rms_over(const game::ServoProfile& p, const std::vector<ServoLog>& logs) {
  f64 e = 0;
  std::size_t n = 0;
  for (const ServoLog& l : logs) {
    const f32 r = servo_error(p, l);
    e += static_cast<f64>(r) * r * static_cast<f64>(l.time.size());
    n += l.time.size();
  }
  return n ? static_cast<f32>(std::sqrt(e / static_cast<f64>(n))) : 0.0f;
}

/// The fitted parameters, x = 0 at `start`: the five gains as logarithms of their ratio to the start
/// (a factor e per unit), the delay in 10 ms units.
game::ServoProfile from_x(const game::ServoProfile& start, const std::vector<f64>& x) {
  game::ServoProfile p = start;
  auto g = [&](f32 v, f64 xi) { return static_cast<f32>(v * kern::dm::exp(clamp(xi, -4.0, 4.0))); };
  p.stiffness = g(start.stiffness, x[0]);
  p.damping = g(max_of(start.damping, 1e-6f), x[1]);
  p.friction = g(max_of(start.friction, 1e-6f), x[2]);
  p.torque = g(start.torque, x[3]);
  p.armature = g(max_of(start.armature, 1e-7f), x[4]);
  p.delay = static_cast<f32>(clamp(start.delay + 0.01 * x[5], 0.0, 0.2));
  return p;
}

}  // namespace

CalibrationReport calibrate_servo(const game::ServoProfile& start, const std::vector<ServoLog>& fit, const std::vector<ServoLog>& holdout,
                                  i32 generations, u64 seed, i32 threads) {
  CalibrationReport rep;
  rep.fit_rms_before = rms_over(start, fit);
  rep.holdout_rms_before = rms_over(start, holdout);
  const ParallelFor run = thread_pool(max_of(1, threads));
  const CmaesResult r = cmaes_minimise(
      std::vector<f64>(6, 0.0), 0.5, generations, seed,
      [&](const std::vector<std::vector<f64>>& xs, std::vector<f64>& costs) {
        run(static_cast<i32>(xs.size()), [&](i32 i) {
          costs[static_cast<std::size_t>(i)] = rms_over(from_x(start, xs[static_cast<std::size_t>(i)]), fit);
        });
      },
      12);
  rep.profile = from_x(start, r.best);
  rep.profile.source = "calibrated on " + std::to_string(fit.size()) + " bench log(s) from " + start.name;
  rep.fit_rms = rms_over(rep.profile, fit);
  rep.holdout_rms = rms_over(rep.profile, holdout);
  rep.evaluations = r.evaluations;
  return rep;
}

// ---- the whole robot ----------------------------------------------------------------------------

bool parse_robot_log(std::string_view csv, RobotLog& out, std::string* error) {
  std::vector<std::vector<f64>> rows;
  if (!read_csv(csv, rows, nullptr, error)) return false;
  RobotLog log;
  for (const auto& r : rows) {
    if (r.size() != static_cast<std::size_t>(kObs + kActions)) {
      if (error) *error = "each line needs 61 observations and 14 actions";
      return false;
    }
    log.observation.emplace_back(r.begin(), r.begin() + kObs);
    log.action.emplace_back(r.begin() + kObs, r.end());
  }
  if (log.action.empty()) {
    if (error) *error = "empty log";
    return false;
  }
  out = std::move(log);
  return true;
}

std::string robot_log_csv(const RobotLog& log) {
  std::string s;
  for (i32 i = 0; i < kObs; ++i) s += "obs" + std::to_string(i) + ",";
  for (i32 i = 0; i < kActions; ++i) s += "act" + std::to_string(i) + (i + 1 < kActions ? "," : "\n");
  char buf[32];
  for (std::size_t t = 0; t < log.action.size(); ++t) {
    for (f32 v : log.observation[t]) { std::snprintf(buf, sizeof buf, "%.9g,", v); s += buf; }
    for (std::size_t i = 0; i < log.action[t].size(); ++i) {
      std::snprintf(buf, sizeof buf, "%.9g%s", log.action[t][i], i + 1 < log.action[t].size() ? "," : "\n");
      s += buf;
    }
  }
  return s;
}

LogDelta compare_robot_log(const RobotLog& log, const game::ServoProfile& servo) {
  DuckEnv env(make_stand_task(), 1, game::kWalkerSubsteps);
  env.task().randomize = false;
  env.servo = servo;
  env.reset();
  LogDelta d;
  f64 g = 0, gr = 0, q = 0, v = 0, qj[kActions] = {};
  for (std::size_t t = 0; t < log.action.size(); ++t) {
    const f32* o = env.observation();  // what the simulated robot sees before tick t
    const std::vector<f32>& r = log.observation[t];
    for (i32 i = 0; i < 3; ++i) g += static_cast<f64>(o[i] - r[i]) * (o[i] - r[i]);
    for (i32 i = 3; i < 6; ++i) gr += static_cast<f64>(o[i] - r[i]) * (o[i] - r[i]);
    for (i32 j = 0; j < kActions; ++j) {
      const f64 e = o[6 + j] - r[static_cast<std::size_t>(6 + j)];
      q += e * e;
      qj[j] += e * e;
      v += static_cast<f64>(o[20 + j] - r[static_cast<std::size_t>(20 + j)]) * (o[20 + j] - r[static_cast<std::size_t>(20 + j)]);
    }
    env.step(log.action[t].data());
    ++d.ticks;
  }
  const f64 n = max_of(1, d.ticks);
  d.gyro = static_cast<f32>(std::sqrt(g / (3 * n)));
  d.gravity = static_cast<f32>(std::sqrt(gr / (3 * n)));
  d.joint_pos = static_cast<f32>(std::sqrt(q / (kActions * n)));
  d.joint_vel = static_cast<f32>(std::sqrt(v / (kActions * n)));
  for (i32 j = 0; j < kActions; ++j) d.joint_pos_each[j] = static_cast<f32>(std::sqrt(qj[j] / n));
  return d;
}

RobotLog simulated_robot_log(const game::ServoProfile& servo, i32 ticks) {
  auto policy = game::walker_policy("velstand.onnx");
  DuckEnv env(make_stand_task(), 1, game::kWalkerSubsteps);
  env.task().randomize = false;
  env.servo = servo;
  env.reset();
  RobotLog log;
  for (i32 t = 0; t < ticks; ++t) {
    std::vector<f32> o(env.observation(), env.observation() + kObs);
    o[48] = t < 50 ? 0.0f : 0.15f;  // stand, walk forward, turn
    o[50] = t < 150 ? 0.0f : 0.6f;
    std::vector<f32> a(kActions, 0.0f);
    if (policy) policy->run(o.data(), a.data());
    log.observation.push_back(o);
    log.action.push_back(a);
    env.step(a.data());
  }
  return log;
}

}  // namespace kern::lehrer
