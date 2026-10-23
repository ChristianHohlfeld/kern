// The Micro Duck's autonomous behaviour: what it does when nobody drives it.
//
// The real robot's behaviour machine lives in its runtime's autonomous.rs, which was not available
// to this port (access to apirrone/microduck_runtime was refused; docs/walker.md section 9). What is
// faithful to the robot: the sensors it reads (head ToF 8 x 8 with the robot's floor and too-close
// filters, the trunk IMU, its own odometry - never the level), the fall verdict (robotd: gravity z
// above -0.5 for 200 ms), the command smoothing and limits, and the idle look-around, a line-by-line
// port of pollen-robotics/microduck robotd/src/idle_head.rs with its [idle] defaults (IdleHead
// below). The state machine on top - its states are the ones the robot's docs name (Chill,
// LookAround, Wander, TurnInPlace, BackOff, Zoomies, GetUp), with an energy model, a novelty grid and
// ToF obstacle avoidance - uses Kern's own numbers until autonomous.rs can be read.
//
// Everything runs in the policy's control tick (50 Hz real) and is deterministic: its random
// choices come from generators seeded with the actor id.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <unordered_map>

#include "kern/game/runtime.h"
#include "kern/game/walker.h"
#include "runtime_internal.h"

namespace kern::game {

namespace {

// Real units (metres, seconds, radians); the robot is built kWalkerScale larger in the world.
constexpr f32 kWanderSpeed = 0.30f;        // m/s forward while roaming (the policy tracks ~0.17 at 0.4)
constexpr f32 kZoomSpeed = 0.40f;          // full command
constexpr f32 kTurnRate = 0.8f;            // rad/s turning in place
constexpr f32 kStopRange = 0.32f;          // an obstacle this close ahead stops the walk
constexpr f32 kSlowRange = 0.70f;          // ... and this close slows it and steers away
constexpr f32 kAheadCone = 0.6f;           // rad: what counts as ahead
constexpr f32 kTofFresh = 0.4f;            // s: older ToF frames are not trusted (robotctl's TOF_STALE)
constexpr f32 kCell = 0.5f;                // m: exploration memory cells
constexpr f32 kForget = 0.02f;             // visits decay per second (old places become new again)

enum class S : u8 { chill, look_around, wander, turn, back_off, zoomies, get_up };
const char* state_name(S s) {
  switch (s) {
    case S::chill: return "Chill";
    case S::look_around: return "LookAround";
    case S::wander: return "Wander";
    case S::turn: return "TurnInPlace";
    case S::back_off: return "BackOff";
    case S::zoomies: return "Zoomies";
    case S::get_up: return "GetUp";
  }
  return "";
}

}  // namespace

struct GameWorld::WalkerBrain::Brain {
  S state = S::chill;
  f32 time = 0, length = 2;     // seconds in the state, planned duration
  f32 energy = 0.8f;            // 0 tired .. 1 rested
  f32 goal_heading = 0;         // wander / turn target (world yaw, radians)
  f32 turn_dir = 1;
  f32 upright_for = 0;
  f32 since_obstacle = 99;
  u32 rng = 0x9e3779b9u;
  // Exploration memory: visits per cell (world cells of kCell real metres) and cells seen blocked.
  std::unordered_map<i64, f32> visits;
  std::unordered_map<i64, f32> blocked;
  f32 rand01() {
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return static_cast<f32>(rng >> 8) / static_cast<f32>(1u << 24);
  }
  f32 range(f32 a, f32 b) { return a + (b - a) * rand01(); }
  static i64 key(f32 x, f32 y) {
    const i64 cx = static_cast<i64>(std::floor(x / kCell)), cy = static_cast<i64>(std::floor(y / kCell));
    return (cx << 32) ^ (cy & 0xffffffff);
  }
  void enter(S s, f32 len) {
    state = s;
    time = 0;
    length = len;
  }
};

void GameWorld::think_walker(WalkerBrain& w, f32 dt) {
  if (!w.brain) {
    w.brain = std::make_shared<WalkerBrain::Brain>();
    w.brain->rng ^= w.actor * 2654435761u;
    w.brain->rand01();
  }
  WalkerBrain::Brain& b = *w.brain;
  const f32 S_ = kWalkerScale;
  const WalkerSensors& s = w.sensors;
  const physics::Pose tp = physics_.pose(w.trunk);
  const Vec3 fwd = tp.rotation.rotate({1, 0, 0});
  const f32 heading = std::atan2(fwd.y, fwd.x);
  const f32 px = tp.position.x / S_, py = tp.position.y / S_;  // real metres (odometry)
  auto wrap = [](f32 a) { while (a > 3.14159265f) a -= 6.2831853f; while (a < -3.14159265f) a += 6.2831853f; return a; };
  f32 cmd[3] = {0, 0, 0}, head[4] = {0, 0, 0, 0};
  b.time += dt;

  // ---- what the sensors say ---------------------------------------------------------------------
  const bool fresh = s.tof_time >= 0 && sim_time_ - s.tof_time < kTofFresh * std::sqrt(S_);
  const f32 nearest = fresh && s.nearest >= 0 ? s.nearest / S_ : -1;
  const bool ahead_blocked = nearest >= 0 && nearest < kStopRange && std::fabs(s.nearest_bearing) < kAheadCone;
  const bool ahead_close = nearest >= 0 && nearest < kSlowRange && std::fabs(s.nearest_bearing) < kAheadCone;
  b.since_obstacle = ahead_close ? 0 : b.since_obstacle + dt;
  // Memory: this cell was visited; obstacle returns mark their cells blocked.
  for (auto& [k, v] : b.visits) v = max_of(0.0f, v - kForget * dt);
  b.visits[WalkerBrain::Brain::key(px, py)] += dt;
  if (fresh && nearest >= 0) {
    const f32 a = heading + s.nearest_bearing;
    b.blocked[WalkerBrain::Brain::key(px + std::cos(a) * nearest, py + std::sin(a) * nearest)] = 1;
  }

  // ---- energy: roaming tires, resting restores -----------------------------------------------
  const bool moving = b.state == S::wander || b.state == S::zoomies;
  b.energy = clamp(b.energy + (moving ? -0.012f : 0.03f) * dt, 0.0f, 1.0f);

  // ---- falls come first: the policy gets up on its own (velstand v8), the brain waits ----------
  if (s.fallen) {
    if (b.state != S::get_up) b.enter(S::get_up, 0);
    b.upright_for = 0;
  } else if (b.state == S::get_up) {
    b.upright_for += dt;
    if (b.upright_for > 1.0f) b.enter(S::chill, b.range(2, 4));
  }

  // ---- choosing the next state ------------------------------------------------------------------
  auto pick_heading = [&]() {
    // The least visited reachable direction (8 candidates, 1 m out), a little randomness.
    f32 best = heading, best_cost = 1e9f;
    for (int k = 0; k < 8; ++k) {
      const f32 a = heading + static_cast<f32>(k) * 0.785398f;
      f32 cost = 0;
      for (f32 d : {0.5f, 1.0f, 1.5f}) {
        const i64 key = WalkerBrain::Brain::key(px + std::cos(a) * d, py + std::sin(a) * d);
        if (auto it = b.visits.find(key); it != b.visits.end()) cost += it->second;
        if (b.blocked.count(key)) cost += 6;
      }
      cost += std::fabs(wrap(a - heading)) * 0.6f + b.rand01() * 1.5f;  // prefer ahead, a bit of chance
      if (cost < best_cost) { best_cost = cost; best = a; }
    }
    return best;
  };
  if (b.state != S::get_up && b.time >= b.length) {
    const f32 r = b.rand01();
    if (b.state == S::wander || b.state == S::zoomies || b.state == S::turn || b.state == S::back_off) {
      if (b.energy < 0.25f || r < 0.3f) b.enter(r < 0.15f ? S::look_around : S::chill, b.range(3, 7));
      else { b.goal_heading = pick_heading(); b.enter(S::wander, b.range(5, 12)); }
    } else {  // rested enough: go
      if (b.energy > 0.5f && r < 0.08f) b.enter(S::zoomies, b.range(2, 4));
      else if (b.energy > 0.35f && r < 0.75f) { b.goal_heading = pick_heading(); b.enter(S::turn, 3); }
      else b.enter(r < 0.5f ? S::look_around : S::chill, b.range(3, 6));
    }
  }

  // ---- obstacles interrupt walking --------------------------------------------------------------
  if ((b.state == S::wander || b.state == S::zoomies) && ahead_blocked) {
    // Turn toward the freer side, far enough to clear it.
    b.turn_dir = s.free_left >= s.free_right ? 1.0f : -1.0f;
    b.goal_heading = heading + b.turn_dir * b.range(1.0f, 2.2f);
    b.enter(nearest < kStopRange * 0.5f ? S::back_off : S::turn, nearest < kStopRange * 0.5f ? 1.2f : 4);
  }

  // ---- what each state does ----------------------------------------------------------------------
  switch (b.state) {
    case S::chill:
    case S::look_around:
      // Standing still with the head left alone: the robot's idle look-around takes over after
      // [idle] after_s (idle_walker), glancing, holding, dozing, yawning.
      break;
    case S::wander:
    case S::zoomies: {
      const f32 err = wrap(b.goal_heading - heading);
      const bool zoom = b.state == S::zoomies;
      f32 speed = zoom ? kZoomSpeed : kWanderSpeed;
      if (ahead_close) speed *= clamp((nearest - kStopRange) / (kSlowRange - kStopRange), 0.3f, 1.0f);
      if (std::fabs(err) > 0.9f) speed *= 0.3f;  // mostly turn first
      cmd[0] = speed;
      cmd[2] = clamp(err * 1.2f, -kTurnRate, kTurnRate);
      // Something close to one side: lean away from it.
      if (ahead_close) cmd[2] += (s.free_left >= s.free_right ? 1.0f : -1.0f) * 0.4f;
      if (zoom && b.rand01() < 0.02f) b.goal_heading += b.range(-1.0f, 1.0f);
      head[2] = clamp(cmd[2] * 0.5f, -0.6f, 0.6f);  // look into the turn
      head[1] = 0.1f;                               // and a little down, where obstacles are
      break;
    }
    case S::turn: {
      const f32 err = wrap(b.goal_heading - heading);
      cmd[2] = clamp(err * 1.5f, -kTurnRate, kTurnRate);
      head[2] = clamp(err * 0.5f, -0.4f, 0.4f);  // the head leads
      if (std::fabs(err) < 0.15f) b.enter(S::wander, b.range(5, 12)), b.goal_heading = heading;
      break;
    }
    case S::back_off:
      cmd[0] = -0.15f;
      cmd[2] = 0;  // straight back: backing and turning at once tips it
      if (b.time > b.length) { b.enter(S::turn, 4); }
      break;
    case S::get_up:
      break;
  }
  std::memcpy(w.auto_cmd, cmd, sizeof cmd);
  std::memcpy(w.auto_head, head, sizeof head);
  w.state = state_name(b.state);
}

// ---- the idle look-around: robotd/src/idle_head.rs (pollen-robotics/microduck), ported ----------
//
// After `after_s` with no motion asked for and nobody steering the head it glances somewhere (mostly
// left or right, a little up or down, sometimes back to the middle, never where it already was) on a
// minimum-jerk curve, holds the look while the gaze shifts a little around it, leans the neck with
// each look, tilts its head now and then, never quite freezes (a slow drift on every axis), calms
// down the longer nothing happens and now and then yawns (the head tips back). It fades in and out
// over `fade_s`. The beak and the breath are computed as on the robot but not applied: Kern's duck
// has no mouth servo, and the breath is for the standing network only (velstand ignores it).
namespace {

struct IdleTuning {  // robotd-params IdleParams::default()
  f64 after_s = 2.0, fade_s = 1.5;
  f64 glance_yaw = 0.6, glance_pitch = 0.2, glance_neck = 0.15, glance_mouth = 0.4;
  f64 tilt_roll = 0.3, tilt_chance = 0.25, centre_chance = 0.25, glance_min = 0.2;
  f64 hold_min_s = 1.0, hold_max_s = 3.0, move_min_s = 0.3, move_s_per_rad = 1.0;
  f64 nudge_yaw = 0.08, nudge_pitch = 0.05, nudge_every_min_s = 0.4, nudge_every_max_s = 1.2;
  f64 drift = 0.04, drift_hz = 0.15, breath_m = 0.001, breath_period_s = 3.6;
  f64 doze_tau_s = 90.0, doze_floor = 0.5;
  f64 yawn_after_s = 45.0, yawn_every_s = 90.0, yawn_s = 3.0, yawn_open = 0.6, yawn_pitch = -0.15;
};
constexpr IdleTuning kIdle{};
constexpr f64 kTau = 6.283185307179586;

/// Minimum-jerk ease from 0 to 1: still at both ends, quickest in the middle.
f64 ease(f64 x) {
  x = std::clamp(x, 0.0, 1.0);
  return x * x * x * (10.0 + x * (-15.0 + 6.0 * x));
}
/// A yawn's beak opening over its length: slow open, a hold at the top, a quicker close.
f64 yawn_shape(f64 x) {
  if (x < 0.4) return ease(x / 0.4);
  if (x < 0.7) return 1.0;
  return 1.0 - ease((x - 0.7) / 0.3);
}

/// robotd's xorshift64 (13, 7, 17), never zero.
struct Rng64 {
  u64 s = 1;
  u64 next() {
    u64 x = s;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    return s = x;
  }
  f64 unit() { return static_cast<f64>(next() >> 11) / static_cast<f64>(u64{1} << 53); }
  f64 between(f64 lo, f64 hi) { return lo + (hi - lo) * unit(); }
  f64 sign() { return unit() < 0.5 ? -1.0 : 1.0; }
};

// Yaw, pitch, roll, neck and the beak's opening.
using Pose5 = std::array<f64, 5>;

}  // namespace

struct GameWorld::WalkerBrain::Idle {
  struct Glance { Pose5 from{}, to{}; f64 start = 0, travel = 1, until = 0; };
  struct Look { Pose5 anchor{}; f64 until = 0; };
  Rng64 rng;
  f64 now = 0;                       // this behaviour's clock, real seconds
  bool still = false;
  f64 still_since = 0;
  f64 gain = 0;
  bool running = false;
  f64 started = 0;
  bool has_look = false, has_glance = false, has_yawn = false;
  Look look;
  Glance glance;
  f64 yawn = 0;
  f64 phases[4] = {};
  f64 mouth = 0, body_z = 0;         // computed like the robot, not applied (see above)

  explicit Idle(u64 seed) {
    rng.s = seed ? seed : 1;
    for (f64& p : phases) p = rng.unit() * kTau;
  }

  Pose5 draw(f64 energy) {
    const IdleTuning& p = kIdle;
    if (rng.unit() < p.centre_chance) return Pose5{};
    const f64 reach = 0.5 + 0.5 * energy;
    const f64 tilt = rng.unit() < p.tilt_chance ? p.tilt_roll * rng.sign() : 0.0;
    Pose5 to{};
    to[0] = p.glance_yaw * reach * rng.between(-1.0, 1.0);
    to[1] = p.glance_pitch * reach * rng.between(-1.0, 1.0);
    to[2] = tilt;
    to[3] = p.glance_neck * reach * rng.between(-1.0, 1.0);
    to[4] = p.glance_mouth * rng.between(0.5, 1.0);
    return to;
  }

  Pose5 somewhere_new(f64 energy) {
    const Pose5 last = has_look ? look.anchor : Pose5{};
    auto away = [&](const Pose5& to) { return std::hypot(to[0] - last[0], to[1] - last[1]); };
    Pose5 best = draw(energy);
    for (int i = 0; i < 8; ++i) {
      if (away(best) >= kIdle.glance_min) break;
      const Pose5 next = draw(energy);
      if (away(next) > away(best)) best = next;
    }
    return best;
  }

  Glance next_glance(const Pose5& from, f64 t, f64 energy) {
    const IdleTuning& p = kIdle;
    const bool holding = has_look && t < look.until;
    Pose5 to;
    if (holding) {
      to = look.anchor;
      to[0] = look.anchor[0] + p.nudge_yaw * energy * rng.between(-1.0, 1.0);
      to[1] = look.anchor[1] + p.nudge_pitch * energy * rng.between(-1.0, 1.0);
    } else {
      to = somewhere_new(energy);
    }
    f64 distance = 0;  // angles only: the beak is an opening, not radians travelled
    for (int i = 0; i < 4; ++i) distance += (to[i] - from[i]) * (to[i] - from[i]);
    distance = std::sqrt(distance);
    const f64 travel = (p.move_min_s + p.move_s_per_rad * distance) / std::sqrt(energy);
    f64 look_until;
    if (holding) {
      look_until = look.until;
    } else {
      const f64 hold = rng.between(p.hold_min_s, p.hold_max_s) / energy;
      look_until = t + travel + hold;
      look = Look{to, look_until};
      has_look = true;
    }
    const f64 gap = rng.between(p.nudge_every_min_s, p.nudge_every_max_s) / energy;
    return Glance{from, to, t, travel, std::max(std::min(t + travel + gap, look_until), t + travel)};
  }

  /// One tick; `steered_age` is how long nobody has steered the head. Returns the head offset in the
  /// head command's order: neck pitch, head pitch, head yaw, head roll.
  std::array<f64, 4> tick(bool still_now, f64 dt) {
    const IdleTuning& p = kIdle;
    now += dt;
    if (still_now) {
      if (!still) still_since = now;
    }
    still = still_now;
    const bool idle = still && now - still_since >= p.after_s - 1e-9;
    const f64 step = std::clamp(dt / p.fade_s, 0.0, 1.0);
    gain = idle ? std::min(gain + step, 1.0) : std::max(gain - step, 0.0);
    if (idle && !running) { running = true; started = now; }
    mouth = body_z = 0;
    if (!running) return {};
    if (gain == 0.0) {  // faded out: the next spell starts afresh, at the middle, wide awake
      running = has_look = has_glance = has_yawn = false;
      return {};
    }
    const f64 t = now - started;
    const f64 energy = p.doze_floor + (1.0 - p.doze_floor) * std::exp(-t / p.doze_tau_s);
    if (has_yawn && t - yawn >= p.yawn_s) has_yawn = false;
    if (idle && !has_yawn && p.yawn_every_s > 0.0 && t >= p.yawn_after_s && rng.unit() < dt / p.yawn_every_s) {
      has_yawn = true;
      yawn = t;
    }
    const f64 yawn_v = has_yawn ? yawn_shape((t - yawn) / p.yawn_s) : 0.0;
    if (idle && !has_yawn && (!has_glance || t >= glance.until)) {
      const Pose5 from = has_glance ? glance.to : Pose5{};
      glance = next_glance(from, t, energy);
      has_glance = true;
    }
    Pose5 g5{};
    if (has_glance) {
      const f64 s = ease((t - glance.start) / glance.travel);
      for (int i = 0; i < 5; ++i) g5[i] = glance.from[i] + (glance.to[i] - glance.from[i]) * s;
    }
    // The drift: two incommensurate slow waves per axis, so it never visibly repeats.
    auto drift = [&](f64 phase, f64 f) {
      return p.drift * 0.5 * (std::sin(kTau * f * t + phase) + std::sin(kTau * f * 1.618 * t + 2.0 * phase));
    };
    const f64 breath = p.breath_m * std::sin(kTau * t / p.breath_period_s + phases[3]);
    body_z = gain * breath;
    mouth = std::min(gain * (g5[4] + p.yawn_open * yawn_v), 1.0);
    return {gain * g5[3],
            gain * (g5[1] + p.yawn_pitch * yawn_v + drift(phases[0], p.drift_hz * 1.2)),
            gain * (g5[0] + drift(phases[1], p.drift_hz * 0.8)),
            gain * (g5[2] + drift(phases[2], p.drift_hz))};
  }
};

void GameWorld::idle_walker(WalkerBrain& w, bool still, f32 dt, f32 head[4]) {
  if (!w.idle) w.idle = std::make_shared<WalkerBrain::Idle>((static_cast<u64>(w.actor) + 1) * 0x9e3779b97f4a7c15ull);
  // robotd: still also needs nobody to have steered the head for `after_s`.
  const std::array<f64, 4> o = w.idle->tick(still && w.head_age >= kIdle.after_s, dt);
  for (int i = 0; i < 4; ++i) head[i] = static_cast<f32>(o[i]);
  w.idle_mouth = static_cast<f32>(w.idle->mouth);
}

}  // namespace kern::game
