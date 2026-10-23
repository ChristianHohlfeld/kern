// The Micro Duck's autonomous behaviour: what it does when nobody drives it.
//
// It is a port of the behaviour stack the real robot's runtime runs (docs/walker.md section 8): a
// state machine whose states issue the same commands a pad would (a twist and a head pose for
// the velstand policy), an energy model that decides between resting and roaming, a coarse
// exploration memory (novelty grid) that pulls it toward places it has not been, and obstacle
// avoidance from its head ToF (8 x 8 zones, the robot's own floor and too-close filters). It only
// reads what the robot could sense - its IMU, its ToF, its own odometry - never the level.
//
// Everything runs in the policy's control tick (50 Hz real) and is deterministic: its random
// choices come from a generator seeded with the actor id.
#include <algorithm>
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
constexpr f32 kTofFresh = 0.3f;            // s: older ToF frames are not trusted
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
  f32 head_phase = 0;
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
  b.head_phase += dt;
  switch (b.state) {
    case S::chill:
      head[2] = 0.15f * std::sin(b.head_phase * 0.7f);
      head[1] = 0.05f * std::sin(b.head_phase * 0.45f);
      break;
    case S::look_around:
      head[2] = 1.0f * std::sin(b.head_phase * 0.9f);
      head[1] = -0.15f + 0.2f * std::sin(b.head_phase * 0.55f);
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
      head[2] = clamp(err, -0.8f, 0.8f);  // the head leads
      if (std::fabs(err) < 0.15f) b.enter(S::wander, b.range(5, 12)), b.goal_heading = heading;
      break;
    }
    case S::back_off:
      cmd[0] = -0.15f;
      cmd[2] = b.turn_dir * 0.4f;
      if (b.time > b.length) { b.enter(S::turn, 4); }
      break;
    case S::get_up:
      break;
  }
  std::memcpy(w.auto_cmd, cmd, sizeof cmd);
  std::memcpy(w.auto_head, head, sizeof head);
  w.state = state_name(b.state);
}

}  // namespace kern::game
