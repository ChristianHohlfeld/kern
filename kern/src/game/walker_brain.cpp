// The Micro Duck's autonomous behaviour: what it does when nobody drives it.
//
// A port of Pollen Robotics' autonomous brain, autod (github.com/pollen-robotics/microduck, branch
// autonomous-mode, commit df9a750 of 2026-10-01, Apache-2.0: "the runtime's autonomous.rs, ported
// as a daemon of its own"; autod/src/brain.rs and obstacle.rs, docs/design/autonomous.md). Its
// twelve states, durations, transition weights, energy model, novelty grid, wander goals, quack
// bursts, head bob, voice replies and petting flavours are the original's, with its numbers; its
// ToF obstacle gate (beak-relative corridor, confirmed hits remembered in the odometry frame,
// freshness) is obstacle.rs on Kern's ToF frame (sense_walker). Odometry is the trunk's pose, which
// autod also believes perfect. Its head pitch is "positive up", flipped on the way out as autod's
// main.rs does.
//
// Kern additions, not in Pollen's brain (it has no ball or sock detector): BallPlay (walk up until
// the ball lies where ball_kick_left's training puts it, kick) and Fetch / Carry (walk up to a sock,
// ground pick, carry it off, put it down). They use the head camera's picture (walker_sees) as the
// detector. Kern has no microphone or touch sensor: hearing and petting come from walker_hear /
// walker_pet.
//
// Everything runs in the policy's control tick (50 Hz real); the random choices come from a
// generator seeded with the actor id, so a run is deterministic.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "kern/game/runtime.h"
#include "kern/game/walker.h"
#include "runtime_internal.h"

namespace kern::game {

namespace {

// ---- autod/src/obstacle.rs ----------------------------------------------------------------------
constexpr f64 kStopM = 0.15;        // STOP_M: from the beak, stop and turn away
constexpr f64 kReactM = 0.45;       // REACT_M: slow down and steer away
constexpr f64 kFreshS = 0.9;        // FRESH_S
constexpr f64 kStaleS = 1.5;        // STALE_S: also how long a confirmed hit is remembered
constexpr f64 kBeakM = 0.18;        // BEAK_M: trunk origin to the beak tip
constexpr f64 kHalfWidthM = 0.12;   // HALF_WIDTH_M
constexpr f64 kConfirmM = 0.08;     // CONFIRM_M
constexpr f64 kTurnedRad = 0.45, kMovedM = 0.5;  // TURNED_RAD, MOVED_M

// ---- autod/src/brain.rs -------------------------------------------------------------------------
constexpr f64 kCalmHeadAlpha = 0.15;
constexpr f64 kQuackAnimS = 0.45;
constexpr f64 kGoalDistances[3] = {1.0, 2.0, 3.0};
constexpr int kGoalDirections = 16;
constexpr f64 kGoalReachedM = 0.25, kGoalPatienceS = 8.0, kGoalProgressM = 0.1, kBoutMaxS = 90.0, kGoalSlowM = 0.4;
constexpr f64 kBlockedS = 300.0, kBlockedM = 0.4;
constexpr f64 kMinWalkVx = 0.15, kStartKickS = 0.6;
constexpr f64 kCellM = 0.5;
constexpr f64 kPickAckS = 1.5;
constexpr f64 kMaxLinear = 0.3, kMaxAngular = 1.5;  // robotd_params::drive (the pad's speeds)
constexpr f64 kSelfAudioS = 1.5;                    // main.rs SELF_AUDIO

// ---- Kern: ball play and sock fetching ------------------------------------------------------------
constexpr f64 kBiteAhead = 0.0588, kBiteLeft = 0.0075, kBiteTolerance = 0.004;  // where the beak takes a sock (test micro_duck_picks_up_a_sock)
constexpr f64 kKickAhead = 0.09, kKickLeft = 0.042, kKickTolerance = 0.015;     // microduck_ball_kick_env_cfg.py (left foot, BALL_POS_NOISE_XY +-1.5 cm)
constexpr f64 kTargetForget = 6.0;   // s: a target not seen this long, while far, is given up
constexpr f64 kSockRest = 40.0;      // s: a sock put down or missed four times is left alone

constexpr f64 kTau = 6.283185307179586, kPi = 3.141592653589793;
f64 wrap_angle(f64 a) {
  a = std::fmod(a + kPi, kTau);
  if (a < 0) a += kTau;
  return a - kPi;
}
f64 sgn(f64 v) { return v < 0 ? -1.0 : 1.0; }

enum class S : u8 { chill, look_around, wander, turn, zoomies, stretch, ruffle, dance, sneeze, ground_pick, nap, petted,
                    fetch, act, carry };  // the last three: Kern
const char* state_name(S s) {
  switch (s) {
    case S::chill: return "Chill";
    case S::look_around: return "LookAround";
    case S::wander: return "Wander";
    case S::turn: return "TurnInPlace";
    case S::zoomies: return "Zoomies";
    case S::stretch: return "Stretch";
    case S::ruffle: return "Ruffle";
    case S::dance: return "Dance";
    case S::sneeze: return "Sneeze";
    case S::ground_pick: return "GroundPick";
    case S::nap: return "Nap";
    case S::petted: return "Petted";
    case S::fetch: return "Fetch";
    case S::act: return "Kick";
    case S::carry: return "Carry";
  }
  return "";
}

struct Obstacle {
  f64 ahead_m = INFINITY, away = 1, age_s = INFINITY;
};

}  // namespace

struct GameWorld::WalkerBrain::Brain {
  // autod Rng: xorshift64
  u64 rng = 1;
  u64 next_u64() { u64 x = rng; x ^= x << 13; x ^= x >> 7; x ^= x << 17; rng = x; return x; }
  f64 unit() { return static_cast<f64>(next_u64() >> 11) / static_cast<f64>(1ull << 53); }
  f64 range(f64 lo, f64 hi) { return lo + (hi - lo) * unit(); }
  bool chance(f64 p) { return unit() < p; }
  f64 sym(f64 a) { return range(-a, a); }

  S state = S::chill;
  f64 t_state = 0, state_dur = 3, retarget_in = 0;
  f64 cmd[3] = {0, 0, 0}, head[4] = {0, 0, 0, 0};
  f64 turn_rate = 0, next_quack_in = 8, quack_t = kQuackAnimS + 1;
  u32 quacks_left = 0;
  f64 burst_gap_in = 0;
  bool pick_pending = false, pick_seen_busy = false;
  f64 wander_vx = 0, walking_s = 0, wander_wz = 0;
  bool has_goal = false;
  f64 goal[2] = {0, 0};
  std::vector<std::array<f64, 3>> blocked;  // x, y, clock
  f64 bout_best_left = 0, bout_progress_at = 0;
  bool glancing = false;
  f64 glance_in = 0;
  bool nap_doze = false, nap_sit_sent = false, nap_stand_sent = false;
  f64 droop = 0, twitch_in = 8, twitch_t = 1;
  bool sneeze_fired = false;
  f64 energy = 0.8, stop_heat = 0;
  f64 reply_in = -1, reply_cool = 0, perk_until = -1;
  u8 pet_flavor = 0;
  f64 pet_last_felt = -100, pet_coo_in = 0;
  f64 clock = 0;
  std::unordered_map<i64, f32> visits;
  f64 last_pose[3] = {0, 0, 0};
  u64 decay_tick = 0;
  // obstacle.rs Model
  struct Hazard { f64 x, y, t; };
  std::vector<Hazard> hazards;
  std::vector<std::array<f64, 3>> previous;  // last frame's hits: x, y, height
  bool seen = false;
  f64 seen_t = 0, seen_pose[3] = {0, 0, 0};
  f64 last_frame = -1;
  // what main.rs keeps
  f64 quiet_until = -1;
  bool posing = false;
  // Kern: the target it plays with (a ball) or fetches (a sock)
  ActorId target = 0;
  bool ball = false, acted = false;
  f64 target_at[2] = {0, 0}, target_seen = 99, settle = 0;
  i32 tries = 0;
  std::unordered_map<ActorId, f64> rest;  // targets left alone for a while (s)
  f64 foot_home[2] = {0, 0};
  bool has_foot = false;

  static i64 cell_key(f64 x, f64 y) {
    const i64 cx = static_cast<i64>(std::floor(x / kCellM)), cy = static_cast<i64>(std::floor(y / kCellM));
    return (cx << 32) ^ (cy & 0xffffffff);
  }
  f32 visit_at(f64 x, f64 y) const {
    auto it = visits.find(cell_key(x, y));
    return it == visits.end() ? 0.0f : it->second;
  }
  f64 pick_heading(f64 spread) {
    const f64 x = last_pose[0], y = last_pose[1], yaw = last_pose[2];
    f64 best_score = 1e300, best = yaw;
    for (int k = 0; k < 9; ++k) {
      const f64 h = yaw + spread * (k - 4.0) / 4.0;
      f64 score = 0;
      for (f64 d : {0.7, 1.5, 2.5}) score += visit_at(x + d * std::cos(h), y + d * std::sin(h)) * (1.7 - 0.4 * d);
      score += range(0, 0.5);
      if (score < best_score) { best_score = score; best = h; }
    }
    return best;
  }
  void pick_goal() {
    const f64 x = last_pose[0], y = last_pose[1], yaw = last_pose[2];
    blocked.erase(std::remove_if(blocked.begin(), blocked.end(), [&](const std::array<f64, 3>& b) { return clock - b[2] > kBlockedS; }), blocked.end());
    const f64 offset = range(0, kTau);
    f64 best_score = 1e300;
    goal[0] = x + std::cos(yaw);
    goal[1] = y + std::sin(yaw);
    for (int k = 0; k < kGoalDirections; ++k) {
      const f64 heading = offset + kTau * k / kGoalDirections, c = std::cos(heading), s = std::sin(heading);
      for (f64 d : kGoalDistances) {
        const int steps = static_cast<int>(d / 0.25);
        f64 dwell = 0;
        bool shut = false;
        for (int i = 1; i <= steps; ++i) {
          const f64 r = d * i / steps, px = x + r * c, py = y + r * s;
          dwell += visit_at(px, py);
          for (const auto& b : blocked) shut = shut || std::hypot(b[0] - px, b[1] - py) < kBlockedM;
        }
        const f64 turn = std::fabs(wrap_angle(heading - yaw)) / kPi;
        const f64 score = dwell / steps + (shut ? 100.0 : 0.0) - 0.5 * d + 0.6 * turn + range(0, 0.5);
        if (score < best_score) { best_score = score; goal[0] = x + d * c; goal[1] = y + d * s; }
      }
    }
    has_goal = true;
  }
  f64 walkable(f64 vx, f64 dt) {
    if (vx <= 0.01 || cmd[0] <= 0.01) walking_s = 0;
    if (vx <= 0.01) return 0;
    walking_s += dt;
    return walking_s < kStartKickS ? kMaxLinear : std::max(vx, std::min(kMinWalkVx, kMaxLinear));
  }
  void start_quack_burst() {
    const f64 r = unit();
    const u32 count = r < 0.45 ? 1 : r < 0.75 ? 2 : r < 0.93 ? 3 : 4;
    quack_t = 0;
    quacks_left = count - 1;
    burst_gap_in = range(0.30, 0.45);
  }
  f64 mouth_now() const { return quack_t < kQuackAnimS ? std::sin(kPi * quack_t / kQuackAnimS) : 0.0; }
  void enter(S next) {
    state = next;
    t_state = 0;
    retarget_in = 0;
    if (next != S::ground_pick) pick_pending = false;
    switch (next) {
      case S::chill: state_dur = range(3.0, 7.0); break;
      case S::look_around: state_dur = range(3.5, 7.0); break;
      case S::wander:
        glancing = true;
        glance_in = 0;
        pick_goal();
        bout_best_left = std::hypot(goal[0] - last_pose[0], goal[1] - last_pose[1]);
        bout_progress_at = 0;
        state_dur = kBoutMaxS;
        break;
      case S::turn: {
        const f64 rate = range(0.8, 1.0) * kMaxAngular;
        turn_rate = rate * (chance(0.5) ? 1.0 : -1.0);
        f64 dur = range(0.8, 2.0);
        if (chance(0.7)) {
          const f64 delta = wrap_angle(pick_heading(kPi) - last_pose[2]);
          if (std::fabs(delta) > 0.3) {
            turn_rate = rate * sgn(delta);
            dur = std::clamp(std::fabs(delta) / std::max(rate, 1e-3), 0.5, 2.2);
          }
        }
        state_dur = dur;
        break;
      }
      case S::zoomies: {
        energy = std::max(energy - 0.08, 0.05);
        const f64 dur = range(2.5, 5.0);
        next_quack_in = std::max(next_quack_in, dur + 1.0);
        state_dur = dur;
        break;
      }
      case S::stretch: state_dur = range(5.5, 6.5); break;
      case S::ruffle: state_dur = range(1.4, 2.0); break;
      case S::dance: next_quack_in = std::min(next_quack_in, 0.5); state_dur = range(5.0, 8.0); break;
      case S::sneeze: sneeze_fired = false; state_dur = range(2.6, 3.2); break;
      case S::petted: state_dur = 90; break;
      case S::nap: {
        nap_doze = chance(0.55);
        nap_sit_sent = nap_stand_sent = false;
        droop = -0.3;
        const f64 dur = nap_doze ? range(20.0, 40.0) : range(12.0, 25.0);
        if (nap_doze) next_quack_in = std::max(next_quack_in, dur + 2.0);
        state_dur = dur;
        break;
      }
      case S::ground_pick: state_dur = 60; break;
      case S::fetch: settle = 0; state_dur = 60; break;  // Kern
      case S::act: acted = false; state_dur = 12; break;  // Kern
      case S::carry: state_dur = range(10.0, 20.0); break;  // Kern
    }
  }
  S pick_next_state(bool can_ground_pick, bool can_zoom, bool can_nap) {
    const f64 r = unit();
    switch (state) {
      case S::wander:
        if (r < 0.20) return S::turn;
        if (r < 0.30 && can_zoom) return S::zoomies;
        if (r < 0.50) return S::wander;
        if (r < 0.58) return S::ruffle;
        if (r < 0.80) return S::look_around;
        return S::chill;
      case S::turn: return r < 0.55 ? S::wander : r < 0.80 ? S::look_around : S::chill;
      case S::zoomies: return r < 0.5 ? S::chill : S::look_around;
      case S::ruffle: return r < 0.40 ? S::look_around : r < 0.70 ? S::chill : S::wander;
      case S::sneeze: return r < 0.35 ? S::ruffle : r < 0.70 ? S::look_around : S::chill;
      default: {
        const f64 e = energy;
        const std::pair<S, f64> weights[10] = {
            {S::wander, 0.16 + 0.28 * e},
            {S::turn, 0.06},
            {S::zoomies, can_zoom && e > 0.5 ? 0.07 : 0.0},
            {S::nap, can_nap ? 0.03 + 0.11 * (1.0 - e) : 0.0},
            {S::stretch, 0.05},
            {S::ground_pick, can_ground_pick ? 0.05 : 0.0},
            {S::ruffle, 0.06},
            {S::dance, e > 0.4 ? 0.05 : 0.0},
            {S::sneeze, 0.03},
            {S::look_around, 0.12 + 0.06 * (1.0 - e)}};
        f64 total = 0.10;
        for (const auto& w : weights) total += w.second;
        f64 x = r * total;
        for (const auto& w : weights) {
          if (x < w.second) return w.first;
          x -= w.second;
        }
        return S::chill;
      }
    }
  }
  // The obstacle gate (brain.rs apply_obstacle_gate). Returns vx, wz; the gate's verdict is not kept.
  void gate(f64& vx, f64& wz, const Obstacle* o) {
    if (!o || o->age_s > kStaleS) { vx = 0; return; }  // unseen: turn, do not advance
    const bool aging = o->age_s > kFreshS;
    if (aging) vx *= 0.5;
    if (o->ahead_m < kStopM) {
      if (state == S::wander) {
        const f64 reach = kBeakM + o->ahead_m + 0.1;
        blocked.push_back({last_pose[0] + reach * std::cos(last_pose[2]), last_pose[1] + reach * std::sin(last_pose[2]), clock});
      }
      enter(S::turn);
      turn_rate = std::fabs(turn_rate) * o->away;
      stop_heat += 1;
      if (stop_heat >= 3) {  // boxed in: an annoyed burst and a decisive spin-out
        stop_heat = 0;
        start_quack_burst();
        burst_gap_in = 0;
        quacks_left = std::max(quacks_left, 2u);
        turn_rate = kMaxAngular * o->away;
        state_dur = std::max(state_dur, 1.8);
      }
      vx = 0;
      wz = turn_rate;
    } else if (o->ahead_m < kReactM) {
      const f64 closeness = std::clamp((kReactM - o->ahead_m) / (kReactM - kStopM), 0.0, 1.0);
      vx *= 1 - 0.8 * closeness;
      wz += o->away * 0.6 * closeness;
    }
  }
};

void GameWorld::think_walker(WalkerBrain& w, f32 dt_f) {
  if (!w.brain) {
    w.brain = std::make_shared<WalkerBrain::Brain>();
    w.brain->rng = (0x9e3779b97f4a7c15ull ^ (static_cast<u64>(w.actor) * 2654435761ull)) | 1;
    w.brain->next_quack_in = w.brain->range(4.0, 12.0);
  }
  WalkerBrain::Brain& b = *w.brain;
  const f64 dt = dt_f, S_ = kWalkerScale;
  const WalkerSensors& sn = w.sensors;
  const physics::Pose tp = physics_.pose(w.trunk);
  const Vec3 fwd3 = tp.rotation.rotate({1, 0, 0});
  const f64 pose[3] = {tp.position.x / S_, tp.position.y / S_, std::atan2(fwd3.y, fwd3.x)};  // odometry, real metres
  const f64 now = sim_time_ / std::sqrt(S_);  // real seconds

  // ---- obstacle.rs: fold in a new ToF frame, then summarise the path ahead ------------------------
  if (sn.tof_time >= 0 && sn.tof_time != b.last_frame) {
    b.last_frame = sn.tof_time;
    std::vector<std::array<f64, 3>> hits;
    for (int i = 0; i < sn.hits; ++i) hits.push_back({sn.hit[i].x / S_, sn.hit[i].y / S_, sn.hit[i].z / S_});
    if (sn.forward_observed >= 2) { b.seen = true; b.seen_t = now; std::memcpy(b.seen_pose, pose, sizeof pose); }
    auto close = [](const std::array<f64, 3>& a, const std::array<f64, 3>& c) { return std::hypot(std::hypot(a[0] - c[0], a[1] - c[1]), a[2] - c[2]) <= kConfirmM; };
    for (std::size_t i = 0; i < hits.size(); ++i) {
      bool confirmed = false;
      for (std::size_t j = 0; j < hits.size() && !confirmed; ++j) confirmed = j != i && close(hits[i], hits[j]);
      for (const auto& p : b.previous) confirmed = confirmed || close(hits[i], p);
      if (confirmed) b.hazards.push_back({hits[i][0], hits[i][1], now});
    }
    b.previous = std::move(hits);
  }
  b.hazards.erase(std::remove_if(b.hazards.begin(), b.hazards.end(), [&](const WalkerBrain::Brain::Hazard& h) { return now - h.t > kStaleS; }), b.hazards.end());
  Obstacle ob;
  const bool have_obstacle = b.seen;
  if (b.seen) {
    const bool turned = std::fabs(wrap_angle(pose[2] - b.seen_pose[2])) > kTurnedRad;
    const bool moved = std::hypot(pose[0] - b.seen_pose[0], pose[1] - b.seen_pose[1]) > kMovedM;
    ob.age_s = turned || moved ? INFINITY : std::max(0.0, now - b.seen_t);
    f64 left = INFINITY, right = INFINITY;
    const f64 c = std::cos(pose[2]), s = std::sin(pose[2]);
    for (const auto& h : b.hazards) {
      const f64 dx = h.x - pose[0], dy = h.y - pose[1], x = c * dx + s * dy, y = -s * dx + c * dy;
      if (x <= 0) continue;
      if (std::fabs(y) < kHalfWidthM) ob.ahead_m = std::min(ob.ahead_m, std::max(x - kBeakM, 0.0));
      (y >= 0 ? left : right) = std::min(y >= 0 ? left : right, std::hypot(x, y));
    }
    ob.away = left >= right ? 1.0 : -1.0;
  }
  const Obstacle* obstacle = have_obstacle ? &ob : nullptr;

  // ---- the world as autod sees it (main.rs) ------------------------------------------------------
  using Sit = WalkerBrain::Sit;
  const bool sitting = w.sit == Sit::sitting && w.sit_settle <= 0;
  const bool busy = sn.fallen || w.skill >= 0 || w.pick_phase >= 0 || w.sit == Sit::rising || (w.sit == Sit::sitting && w.sit_settle > 0);
  const PolicyStatus abilities = walker_abilities(w);
  const bool petting = w.petted_until > sim_time_;
  int heard = 0;  // 1 noise, 2 voice (dropped while its own voice could be what it hears)
  if (w.heard && now >= b.quiet_until) heard = w.heard;
  w.heard = 0;

  f64 head_alpha = kCalmHeadAlpha, mouth_extra = 0, body[3] = {0, 0, 0};
  bool body_active = false, wheee = false;
  const char* sound = nullptr;
  bool ground_pick = false, sit_toggle = false;
  b.quack_t += dt;

  if (busy) {
    if (b.pick_pending) b.pick_seen_busy = true;
    if (b.state == S::act) b.acted = true;  // Kern: its kick / pick is running
    w.auto_cmd[0] = w.auto_cmd[1] = w.auto_cmd[2] = 0;
    std::fill(std::begin(w.auto_head), std::end(w.auto_head), 0.0f);
    w.auto_body_active = false;
    w.auto_mouth = static_cast<f32>(b.mouth_now());
    w.auto_wheee = false;
    w.state = sn.fallen ? "GetUp" : b.state == S::act ? (b.ball ? "Kick" : "Pick") : state_name(b.state);
    return;
  }
  if (sitting && b.state != S::nap) { b.enter(S::nap); b.nap_sit_sent = true; }

  b.t_state += dt;
  b.retarget_in -= dt;
  b.clock += dt;
  b.stop_heat = std::max(b.stop_heat - dt / 12.0, 0.0);
  f64 de = dt / 240.0;
  switch (b.state) {
    case S::zoomies: de = -dt / 25.0; break;
    case S::wander: case S::fetch: case S::carry: de = -dt / 110.0; break;
    case S::turn: de = -dt / 70.0; break;
    case S::nap: de = dt / 40.0; break;
    case S::petted: de = dt / 60.0; break;
    case S::dance: de = -dt / 45.0; break;
    case S::chill: de = dt / 120.0; break;
    default: break;
  }
  b.energy = std::clamp(b.energy + de, 0.05, 1.0);
  std::memcpy(b.last_pose, pose, sizeof pose);
  {
    f32& v = b.visits[WalkerBrain::Brain::cell_key(pose[0], pose[1])];
    v = std::min(v + static_cast<f32>(dt), 60.0f);
    if (++b.decay_tick % 1500 == 0)
      for (auto it = b.visits.begin(); it != b.visits.end();) {
        it->second *= 0.97f;
        it = it->second > 0.05f ? std::next(it) : b.visits.erase(it);
      }
  }

  // Spontaneous quacks, in bursts.
  b.next_quack_in -= dt;
  if (b.next_quack_in <= 0) {
    b.next_quack_in = b.range(6.0, 20.0) * (1.8 - b.energy);
    b.start_quack_burst();
    sound = "chirp";
  } else if (b.quacks_left > 0) {
    b.burst_gap_in -= dt;
    if (b.burst_gap_in <= 0) {
      b.quacks_left -= 1;
      b.quack_t = 0;
      b.burst_gap_in = b.range(0.30, 0.45);
      sound = "chirp";
    }
  }
  // Sounds: a voice gets a perk and a quacked answer after a beat; a noise stirs a dozing duck.
  b.reply_cool -= dt;
  const bool dozing = b.state == S::nap && b.nap_doze;
  if (heard == 1 && dozing) { b.droop = -0.6; b.twitch_t = 0; }
  else if (heard == 2 && b.reply_cool <= 0 && b.state != S::petted && !dozing) {
    b.reply_in = b.range(0.35, 0.8);
    b.reply_cool = 4.0;
    b.perk_until = b.clock + 1.4;
  }
  if (b.reply_in > 0) {
    b.reply_in -= dt;
    if (b.reply_in <= 0) { b.reply_in = -1; b.start_quack_burst(); sound = "chirp"; }
  }
  // Petting, the best interrupt there is.
  if (petting && b.state != S::petted && b.state != S::nap && b.state != S::act && b.state != S::carry) {
    b.pet_flavor = b.energy < 0.4 ? 2 : b.energy > 0.65 && b.chance(0.5) ? 1 : 0;
    b.pet_coo_in = 3.0;
    b.enter(S::petted);
  }

  // ---- Kern: a ball or a sock in the head camera's picture ---------------------------------------
  for (auto it = b.rest.begin(); it != b.rest.end();) it = (it->second -= dt) <= 0 ? b.rest.erase(it) : std::next(it);
  b.target_seen += dt;
  {
    const physics::Pose ap = physics_.pose(w.bodies[5]);  // the left ankle (links: trunk, left leg 1..5)
    const f64 fx = (ap.position.x - tp.position.x) / S_, fy = (ap.position.y - tp.position.y) / S_;
    const f64 a = fx * std::cos(pose[2]) + fy * std::sin(pose[2]), l = -fx * std::sin(pose[2]) + fy * std::cos(pose[2]);
    if (!b.has_foot && b.clock > 1.0) { b.foot_home[0] = a; b.foot_home[1] = l; b.has_foot = true; }
  }
  const bool engaged = b.state == S::act || b.state == S::carry || b.state == S::nap || b.state == S::petted || b.state == S::ground_pick;
  if (!w.rollers && !engaged) {
    f64 best = 1e300;
    for (const Actor& a : level_.actors) {
      const bool sock = std::find(a.tags.begin(), a.tags.end(), "sock") != a.tags.end();
      const bool ball = !sock && a.body && a.body->motion == Motion::dynamic && a.mesh && a.mesh->shape == Shape::sphere &&
                        (std::find(a.tags.begin(), a.tags.end(), "ball") != a.tags.end() || a.name == "Ball");
      if (a.parent != 0 || b.rest.count(a.id) || !((ball && abilities.kick_left) || (sock && w.mouth && abilities.ground_pick))) continue;
      if (b.state == S::fetch && b.target && a.id != b.target) continue;  // it keeps to what it went for
      Vec3 sum{};
      int n = 0;
      bool seen = false;
      for (const Actor& c : level_.actors) {
        if (c.id != a.id && c.parent != a.id) continue;
        Mat4 m;
        if (!actor_matrix(c.id, m)) continue;
        sum = sum + m.translation();
        ++n;
        bool visible = false;
        f32 bearing = 0, distance = 0;
        if (!seen && walker_sees(w.actor, c.id, visible, bearing, distance)) seen = visible;
      }
      if (!n) continue;
      const Vec3 at = sum * (1.0f / static_cast<f32>(n));
      if (at.z > (ball ? 0.08 : 0.03) * S_) continue;  // not on the floor
      if (!seen) continue;
      const f64 d = std::hypot(at.x / S_ - pose[0], at.y / S_ - pose[1]) * (sock ? 0.7 : 1.0);  // socks first
      if (d < best) {
        best = d;
        if (b.target != a.id) b.tries = 0;
        b.target = a.id;
        b.ball = ball;
        b.target_at[0] = at.x / S_;
        b.target_at[1] = at.y / S_;
        b.target_seen = 0;
      }
    }
    // Close up the target is under the camera: it goes by where it saw it (it does not move by
    // itself), and forgets it only when it was far.
    const bool near = b.target && std::hypot(b.target_at[0] - pose[0], b.target_at[1] - pose[1]) < 0.3;
    if (near && b.state == S::fetch) b.target_seen = std::min(b.target_seen, kTargetForget * 0.5);
    if (b.target && b.target_seen >= kTargetForget) { b.target = 0; if (b.state == S::fetch) b.enter(S::look_around); }
    if (b.target && b.target_seen < kTargetForget && b.state != S::fetch && !sitting) b.enter(S::fetch);
  }

  const f64 tw = kTau;
  switch (b.state) {
    case S::chill:
      std::fill(std::begin(b.cmd), std::end(b.cmd), 0.0);
      mouth_extra = std::max(0.03 + 0.03 * std::sin(tw * 0.45 * b.t_state), 0.0);
      if (b.retarget_in <= 0) {
        b.retarget_in = b.range(1.2, 3.0);
        b.head[0] = b.sym(0.10); b.head[1] = b.sym(0.20); b.head[2] = b.sym(0.40);
        b.head[3] = b.chance(0.30) ? b.sym(0.25) : 0.0;
      }
      break;
    case S::look_around:
      std::fill(std::begin(b.cmd), std::end(b.cmd), 0.0);
      if (b.retarget_in <= 0) {
        b.retarget_in = b.range(0.7, 1.8);
        b.head[0] = b.sym(0.15); b.head[1] = b.range(-0.55, 0.30); b.head[2] = b.sym(1.1);
        b.head[3] = b.chance(0.60) ? b.sym(0.50) : 0.0;
      }
      break;
    case S::wander: {
      if (b.retarget_in <= 0) {
        b.retarget_in = b.range(1.2, 3.0);
        const f64 pep = 0.65 + 0.35 * b.energy;
        b.wander_vx = b.range(0.6 * kMaxLinear, kMaxLinear) * pep;
      }
      if (!b.has_goal) b.pick_goal();
      const f64 dx = b.goal[0] - pose[0], dy = b.goal[1] - pose[1], distance = std::hypot(dx, dy);
      if (distance < b.bout_best_left - kGoalProgressM) { b.bout_best_left = distance; b.bout_progress_at = b.t_state; }
      if (distance < kGoalReachedM || b.t_state - b.bout_progress_at > kGoalPatienceS) b.t_state = b.state_dur;
      const f64 heading_delta = wrap_angle(std::atan2(dy, dx) - pose[2]);
      const bool pivoting = std::fabs(heading_delta) > 0.7;
      b.wander_wz = pivoting ? 0.9 * kMaxAngular * sgn(heading_delta) : std::clamp(1.2 * heading_delta, -0.6, 0.6);
      b.glance_in -= dt;
      if (b.glance_in <= 0) {
        b.glancing = !b.glancing;
        if (b.glancing) {
          b.glance_in = b.range(0.4, 0.8);
          b.head[0] = 0; b.head[1] = b.range(-0.25, 0.10); b.head[2] = b.sym(0.9); b.head[3] = b.chance(0.3) ? b.sym(0.30) : 0.0;
        } else {
          b.glance_in = b.range(1.5, 3.5);
          b.head[0] = 0; b.head[1] = b.range(-0.15, 0.05); b.head[2] = std::clamp(0.9 * b.wander_wz, -0.5, 0.5); b.head[3] = 0;
        }
      }
      const f64 approach = std::clamp(distance / kGoalSlowM, 0.3, 1.0);
      f64 vx = pivoting ? 0.0 : b.wander_vx * approach, wz = b.wander_wz;
      b.gate(vx, wz, obstacle);
      if (b.state == S::wander) {
        vx = b.walkable(vx, dt);
        b.cmd[0] = vx; b.cmd[1] = 0; b.cmd[2] = wz;
      } else {
        b.cmd[0] = 0; b.cmd[1] = 0; b.cmd[2] = wz;  // the gate turned it away
      }
      break;
    }
    case S::turn: {
      b.cmd[0] = 0; b.cmd[1] = 0; b.cmd[2] = b.turn_rate;
      const f64 side = sgn(b.turn_rate);
      b.head[0] = 0; b.head[1] = 0.10; b.head[2] = 0.7 * side; b.head[3] = 0.15 * side;
      break;
    }
    case S::zoomies: {
      if (b.retarget_in <= 0) {
        b.retarget_in = b.range(0.5, 1.1);
        const f64 delta = wrap_angle(b.pick_heading(1.0) - pose[2]);
        b.wander_wz = std::clamp(delta + b.sym(0.3), -0.7, 0.7);
        b.head[0] = 0; b.head[1] = b.range(-0.10, 0.10); b.head[2] = std::clamp(0.5 * b.wander_wz, -0.35, 0.35); b.head[3] = b.sym(0.15);
      }
      f64 vx = kMaxLinear, wz = b.wander_wz;
      b.gate(vx, wz, obstacle);
      if (b.state == S::zoomies) { b.cmd[0] = b.walkable(vx, dt); b.cmd[1] = 0; b.cmd[2] = wz; }
      else { b.cmd[0] = 0; b.cmd[1] = 0; b.cmd[2] = wz; }
      break;
    }
    case S::stretch: {
      std::fill(std::begin(b.cmd), std::end(b.cmd), 0.0);
      const f64 t = b.t_state, rise_end = 1.4, release_start = b.state_dur - 2.2, shake_start = b.state_dur - 1.2;
      const f64 env = t < release_start ? std::min(t / rise_end, 1.0) : std::clamp(1.0 - (t - release_start), 0.0, 1.0);
      const f64 climax = rise_end + 0.2;
      if (t - dt < climax && t >= climax) sound = "coo";
      body[0] = env * 0.010; body[1] = env * -0.20; body[2] = env * 0.10 * std::sin(tw * 0.35 * t);
      body_active = t < b.state_dur;
      if (t < shake_start) {
        b.head[0] = env * 0.30; b.head[1] = env * 0.70; b.head[2] = env * 0.12 * std::sin(tw * 0.3 * t); b.head[3] = env * 0.15 * std::sin(tw * 0.25 * t + 1.0);
      } else {
        head_alpha = 0.8;
        const f64 ts = t - shake_start, decay = std::clamp(1.0 - ts / 1.2, 0.0, 1.0);
        b.head[0] = -0.10 * decay; b.head[1] = -0.30 * decay; b.head[2] = 0.55 * decay * std::sin(tw * 3.5 * ts); b.head[3] = 0.20 * decay * std::sin(tw * 3.5 * ts + 1.2);
      }
      mouth_extra = std::clamp((t - 0.8) / 0.6, 0.0, 1.0) * env * 0.5;
      break;
    }
    case S::ruffle: {
      std::fill(std::begin(b.cmd), std::end(b.cmd), 0.0);
      head_alpha = 0.85;
      const f64 t = b.t_state, env = std::min(t / 0.15, 1.0) * std::clamp((b.state_dur - t) / 0.4, 0.0, 1.0), wv = tw * 4.5 * t;
      body[0] = -0.005 * env; body[1] = 0; body[2] = 0.13 * env * std::sin(wv);
      body_active = t < b.state_dur;
      b.head[0] = -0.08 * env; b.head[1] = -0.15 * env; b.head[2] = 0.45 * env * std::sin(wv + 0.9); b.head[3] = 0.25 * env * std::sin(wv + 2.2);
      break;
    }
    case S::dance: {
      std::fill(std::begin(b.cmd), std::end(b.cmd), 0.0);
      const f64 t = b.t_state, env = std::min(t / 0.5, 1.0) * std::clamp((b.state_dur - t) / 1.0, 0.0, 1.0);
      body[0] = env * (-0.006 + 0.010 * std::sin(tw * 1.1 * t)); body[1] = env * 0.07 * std::sin(tw * 0.55 * t); body[2] = env * 0.12 * std::sin(tw * 1.1 * t + 1.0);
      body_active = t < b.state_dur;
      head_alpha = 0.30;
      b.head[0] = 0; b.head[1] = env * 0.18 * std::sin(tw * 0.55 * t); b.head[2] = env * 0.35 * std::sin(tw * 0.55 * t + 0.7); b.head[3] = env * 0.45 * std::sin(tw * 1.1 * t + 1.0);
      break;
    }
    case S::sneeze: {
      std::fill(std::begin(b.cmd), std::end(b.cmd), 0.0);
      const f64 t = b.t_state;
      if (t < 0.75) {
        const f64 r = std::pow(t / 0.75, 1.4);
        b.head[0] = 0.15 * r; b.head[1] = 0.55 * r; b.head[2] = 0; b.head[3] = 0;
        mouth_extra = 0.4 * r;
      } else if (t < 0.85) {
        head_alpha = 0.1;
        mouth_extra = 0.45;
      } else {
        if (!b.sneeze_fired) { b.sneeze_fired = true; b.quack_t = 0; sound = "chirp"; }
        const f64 ts = t - 0.85;
        if (ts < 0.5) {
          head_alpha = 0.9;
          const f64 ring = std::sin(tw * 5.0 * ts) * (1.0 - ts / 0.5);
          b.head[0] = -0.10; b.head[1] = -0.65 * (1.0 - ts / 0.5); b.head[2] = 0.25 * ring; b.head[3] = 0.20 * ring;
          mouth_extra = std::max(0.5 - ts, 0.0);
        } else {
          head_alpha = 0.12;
          b.head[0] = 0; b.head[1] = 0.05; b.head[2] = 0.5 * std::sin(tw * 0.35 * (ts - 0.5)); b.head[3] = 0;
        }
      }
      break;
    }
    case S::nap: {
      std::fill(std::begin(b.cmd), std::end(b.cmd), 0.0);
      const f64 t = b.t_state;
      if (!b.nap_sit_sent) { b.nap_sit_sent = true; sit_toggle = true; }
      else if (t > 1.5 && !sitting && !b.nap_stand_sent) b.enter(S::chill);
      else if (t < b.state_dur) {
        if (t < 2.0) std::fill(std::begin(b.head), std::end(b.head), 0.0);
        else if (petting) {
          head_alpha = 0.2;
          b.head[0] = 0.05; b.head[1] = 0.35 + 0.06 * std::sin(tw * 0.5 * t); b.head[2] = 0; b.head[3] = 0.15 * std::sin(tw * 0.3 * t);
          mouth_extra = std::max(0.08 + 0.05 * std::sin(tw * 0.8 * t), 0.0);
          b.pet_coo_in -= dt;
          if (b.pet_coo_in <= 0) { b.pet_coo_in = b.range(4.0, 7.0); sound = "coo"; }
        } else if (b.nap_doze) {
          b.droop += dt / 5.0;
          if (b.droop >= 1.0) b.droop = b.range(-0.5, -0.1);
          const f64 sink = std::pow(std::max(b.droop, 0.0), 1.6);
          b.head[0] = -0.10 * sink; b.head[1] = -0.55 * sink; b.head[2] = 0.06 * std::sin(tw * 0.11 * t); b.head[3] = 0.20 * sink;
          b.twitch_in -= dt;
          if (b.twitch_in <= 0) {
            b.twitch_in = b.range(6.0, 14.0);
            b.twitch_t = 0;
            if (b.chance(0.3)) { b.quack_t = 0; sound = "chirp"; }
          }
          b.twitch_t += dt;
          if (b.twitch_t < 0.35) {
            const f64 tq = std::sin(tw * 9.0 * b.twitch_t) * (1.0 - b.twitch_t / 0.35);
            b.head[2] += 0.12 * tq;
            b.head[3] += 0.10 * tq;
            head_alpha = 0.5;
          }
        } else if (b.retarget_in <= 0) {
          b.retarget_in = b.range(1.6, 3.2);
          b.head[0] = b.sym(0.10); b.head[1] = b.range(-0.15, 0.35); b.head[2] = b.sym(0.8); b.head[3] = b.chance(0.4) ? b.sym(0.35) : 0.0;
        }
      } else if (!b.nap_stand_sent) {
        b.nap_stand_sent = true;
        std::fill(std::begin(b.head), std::end(b.head), 0.0);
        sit_toggle = true;
      } else if (!sitting) {
        b.enter(b.chance(0.65) ? S::stretch : S::look_around);
      } else if (t > b.state_dur + 5.0) {
        b.nap_stand_sent = false;
        b.t_state = b.state_dur;
      }
      break;
    }
    case S::petted: {
      std::fill(std::begin(b.cmd), std::end(b.cmd), 0.0);
      const f64 t = b.t_state;
      if (petting) b.pet_last_felt = b.clock;
      const f64 linger = b.clock - b.pet_last_felt;
      if (linger > 1.2) {
        const f64 r = b.unit();
        if (b.pet_flavor == 2 && abilities.sit_stand && !sitting && r < 0.6) { b.enter(S::nap); b.nap_doze = true; }
        else b.enter(r < 0.40 ? S::ruffle : r < 0.55 ? S::stretch : r < 0.85 ? S::look_around : S::chill);
      } else if (!petting) {
        head_alpha = 0.3;
        b.head[0] = 0.10; b.head[1] = 0.55; b.head[2] = 0.25 * std::sin(tw * 0.8 * t); b.head[3] = 0;
        body_active = true;
      } else {
        body_active = true;
        const f64 env = std::min(t / 0.6, 1.0);
        b.pet_coo_in -= dt;
        if (b.pet_flavor == 1) {
          head_alpha = 0.35;
          const f64 wv = std::sin(tw * 1.6 * t);
          b.head[0] = 0.05; b.head[1] = env * (0.35 + 0.06 * wv); b.head[2] = env * 0.20 * std::sin(tw * 0.8 * t); b.head[3] = env * 0.35 * wv;
          body[0] = -0.004 * env; body[1] = 0; body[2] = 0.12 * env * wv;
          mouth_extra = std::max(0.18 + 0.08 * std::sin(tw * 1.2 * t), 0.0);
          if (b.pet_coo_in <= 0) { b.pet_coo_in = b.range(3.0, 5.0); b.start_quack_burst(); sound = "chirp"; }
        } else if (b.pet_flavor == 2) {
          head_alpha = 0.12;
          const f64 sink = std::min(t / 25.0, 1.0);
          b.head[0] = 0.08 * env; b.head[1] = env * (0.45 * (1.0 - sink) - 0.35 * sink) + 0.05 * std::sin(tw * 0.35 * t);
          b.head[2] = 0.10 * std::sin(tw * 0.20 * t); b.head[3] = env * (0.15 + 0.20 * sink);
          body[0] = -0.010 * env; body[1] = 0; body[2] = 0.06 * env * std::sin(tw * 0.30 * t);
          mouth_extra = std::max(0.08 + 0.05 * std::sin(tw * 0.8 * t), 0.0);
          if (b.pet_coo_in <= 0) { b.pet_coo_in = b.range(6.0, 9.0); sound = "coo"; }
        } else {
          head_alpha = 0.2;
          const f64 lean = 0.45 + 0.08 * std::sin(tw * 0.5 * t);
          b.head[0] = env * 0.12 * std::sin(tw * 0.4 * t); b.head[1] = env * lean; b.head[2] = env * 0.15 * std::sin(tw * 0.22 * t); b.head[3] = env * 0.30 * std::sin(tw * 0.30 * t);
          body[0] = env * (-0.006 + 0.005 * std::sin(tw * 0.35 * t)); body[1] = 0; body[2] = env * 0.10 * std::sin(tw * 0.40 * t);
          mouth_extra = std::max(0.12 + 0.07 * std::sin(tw * 1.0 * t), 0.0);
          if (b.pet_coo_in <= 0) { b.pet_coo_in = b.range(3.5, 6.0); sound = "coo"; }
        }
      }
      break;
    }
    case S::ground_pick:
      std::fill(std::begin(b.cmd), std::end(b.cmd), 0.0);
      std::fill(std::begin(b.head), std::end(b.head), 0.0);
      if (!b.pick_pending) {
        if (abilities.ground_pick) { b.pick_pending = true; b.pick_seen_busy = false; ground_pick = true; }
        else b.enter(S::chill);
      } else if (b.pick_seen_busy) {
        b.pick_pending = false;
        b.start_quack_burst();
        sound = "chirp";
        b.enter(S::chill);
      } else if (b.t_state > kPickAckS) {
        b.pick_pending = false;
        b.enter(S::chill);
      }
      break;
    case S::fetch: {
      // Kern: walk up to the target until it lies where the beak bites (a sock) or in front of the
      // left foot (a ball), head up, then pick / kick.
      const f64 c = std::cos(pose[2]), s = std::sin(pose[2]);
      f64 want_ahead = b.ball ? kKickAhead : kBiteAhead, want_left = b.ball ? kKickLeft : kBiteLeft;
      if (b.ball && b.has_foot) {  // the kick is trained from the home stance: measure from the foot
        const physics::Pose ap = physics_.pose(w.bodies[5]);
        const f64 fx = (ap.position.x - tp.position.x) / S_, fy = (ap.position.y - tp.position.y) / S_;
        want_ahead += fx * c + fy * s - b.foot_home[0];
        want_left += -fx * s + fy * c - b.foot_home[1];
      }
      const f64 tol = b.ball ? kKickTolerance : kBiteTolerance, side_tol = b.ball ? tol : tol * 1.5;
      const f64 dx = b.target_at[0] - pose[0], dy = b.target_at[1] - pose[1];
      const f64 ahead = dx * c + dy * s - want_ahead, side = -dx * s + dy * c - want_left;
      const f64 dist = std::hypot(dx, dy), bearing = wrap_angle(std::atan2(dy, dx) - pose[2]);
      b.cmd[0] = b.cmd[1] = b.cmd[2] = 0;
      std::fill(std::begin(b.head), std::end(b.head), 0.0);
      // Keep it in the picture: the head pitches down (to the training's 1.1 rad), and close by the
      // neck bows too - neck -1.1 with head 1.1 points the camera 87 deg down and sees 6-15 cm in
      // front of the trunk (test micro_duck_camera_sees_the_floor_in_front_when_looking_down).
      // While it walks the head looks down only so far: bowing the neck shifts its weight forward
      // and the gait suffers.
      b.head[1] = -std::clamp(std::atan2(0.12, std::max(dist, 0.05)) - 0.3, 0.0, 0.8);  // positive up
      if (dist > 0.25) {
        b.cmd[0] = std::fabs(bearing) > 0.6 ? 0.0 : std::clamp(dist * 1.2, kMinWalkVx, kMaxLinear);
        b.cmd[2] = std::clamp(bearing * 1.5, -1.0, 1.0);
        // No obstacle gate here: what the ToF sees ahead is the target itself.
        b.cmd[0] = b.walkable(b.cmd[0], dt);
        b.settle = 0;
      } else if (std::fabs(ahead) > tol || std::fabs(side) > side_tol) {
        // Small steps; never less than the gait starts walking at from a stand (autonomous.md: 0.25).
        auto step = [](f64 e, f64 lo, f64 hi) { const f64 v = std::clamp(e * 2.0, -hi, hi); return std::fabs(v) < lo ? (v < 0 ? -lo : lo) : v; };
        const f64 lo = b.ball ? 0.2 : 0.1, hi = b.ball ? 0.25 : 0.15;  // a sock needs finer steps
        if (std::fabs(ahead) > tol) b.cmd[0] = step(ahead, lo, hi);
        if (std::fabs(side) > side_tol) b.cmd[1] = step(side, lo, hi);
        b.cmd[2] = std::clamp(wrap_angle(std::atan2(side + want_left, ahead + want_ahead) - std::atan2(want_left, want_ahead)) * 0.8, -0.3, 0.3);
        if (std::fabs(b.cmd[2]) < 0.05) b.cmd[2] = 0;
        b.settle = 0;
      } else {
        // There: head up (the kick and the pick were trained with the head at home), let the steps
        // die out, then go.
        // (No look down here: bowing neck and head - which would show the target at 6-15 cm - shifts
        // the weight forward, the feet step, and the spot is lost. It goes by where it saw it.)
        b.head[1] = 0;
        b.settle += dt;
        if (b.settle > (b.ball ? 3.0 : 1.2) && sn.speed / S_ < 0.01) {
          if (walker_skill(w.actor, b.ball ? "kick_left" : "pick").empty()) { b.enter(S::act); b.tries++; }
          else b.settle = 0;
        }
      }
      if (b.t_state > 45) { b.rest[b.target] = kSockRest; b.target = 0; b.enter(S::look_around); }
      break;
    }
    case S::act:
      // Kern: the kick / pick runs on its own network (busy above); after it, the mouth encoder.
      b.cmd[0] = b.cmd[1] = b.cmd[2] = 0;
      std::fill(std::begin(b.head), std::end(b.head), 0.0);
      if (!b.acted && b.t_state < 1.0) break;
      if (b.ball) {
        b.rest[b.target] = b.range(2.0, 5.0);
        b.target = 0;
        b.start_quack_burst();
        sound = "chirp";
        b.enter(S::look_around);
      } else if (walker_holding(w.actor)) {
        b.goal[0] = pose[0] + 1.5 * std::cos(b.pick_heading(kPi));
        b.goal[1] = pose[1] + 1.5 * std::sin(b.pick_heading(kPi));
        b.has_goal = true;
        b.enter(S::carry);
      } else {
        if (b.tries >= 4) { b.rest[b.target] = kSockRest; b.target = 0; }
        b.enter(S::fetch);  // a step back happens by itself: it walks up again
        if (!b.target) b.enter(S::look_around);
      }
      break;
    case S::carry: {
      // Kern: off with the sock toward somewhere new, then put it down.
      const f64 dx = b.goal[0] - pose[0], dy = b.goal[1] - pose[1];
      const f64 delta = wrap_angle(std::atan2(dy, dx) - pose[2]);
      f64 vx = std::fabs(delta) > 0.7 ? 0.0 : 0.2, wz = std::fabs(delta) > 0.7 ? 0.9 * kMaxAngular * sgn(delta) : std::clamp(1.2 * delta, -0.6, 0.6);
      b.gate(vx, wz, obstacle);
      if (b.state != S::carry) { b.enter(S::carry); b.t_state = 0; }
      b.cmd[0] = b.walkable(vx, dt); b.cmd[1] = 0; b.cmd[2] = wz;
      b.head[0] = 0; b.head[1] = 0.1; b.head[2] = 0; b.head[3] = 0;  // proud, beak up
      if (!walker_holding(w.actor)) { b.rest[b.target] = kSockRest * 0.5; b.target = 0; b.enter(S::chill); break; }
      if (b.t_state > b.state_dur || std::hypot(dx, dy) < kGoalReachedM) {
        w.beak_open = 0.6f;  // put it down: the beak opens, the sock drops
        b.rest[b.target] = kSockRest;
        b.target = 0;
        b.enter(S::chill);
      }
      break;
    }
  }

  // Listening: a perked head while a reply brews.
  if ((b.state == S::chill || b.state == S::look_around) && b.clock < b.perk_until) {
    b.head[0] = 0.05; b.head[1] = 0.35; b.head[2] = 0; b.head[3] = 0;
    head_alpha = 0.3;
  }
  // The state's time is up (GroundPick, Nap and Kern's own manage their exits).
  if (b.t_state >= b.state_dur && b.state != S::ground_pick && b.state != S::nap && b.state != S::fetch && b.state != S::act && b.state != S::carry) {
    const bool can_zoom = obstacle && ob.age_s <= kFreshS && ob.ahead_m > 1.2;
    const bool can_nap = abilities.sit_stand && !sitting;
    b.has_goal = false;
    b.enter(b.pick_next_state(abilities.ground_pick, can_zoom, can_nap));
  }
  wheee = b.state == S::zoomies;

  // ---- out, as autod's main.rs sends it ------------------------------------------------------------
  if (ground_pick) walker_skill(w.actor, "pick");
  if (sit_toggle) walker_skill(w.actor, "sit");
  if (sound) { walker_say(w, sound); b.quiet_until = now + kSelfAudioS; }
  if (wheee) b.quiet_until = now + kSelfAudioS;
  f64 h[4] = {b.head[0], b.head[1], b.head[2], b.head[3]};
  h[1] += 0.35 * b.mouth_now();  // each quack throws the head up with the open mouth
  if ((b.state == S::wander || b.state == S::zoomies) && b.cmd[0] > 0.02) h[1] += 0.12 * std::sin(kTau * 2.2 * b.t_state);  // the bob
  for (int i = 0; i < 3; ++i) w.auto_cmd[i] = static_cast<f32>(b.cmd[i]);
  w.auto_head[0] = static_cast<f32>(h[0]);
  w.auto_head[1] = static_cast<f32>(-h[1]);  // "positive = up" -> the alpha's +head_pitch looks down
  w.auto_head[2] = static_cast<f32>(h[2]);
  w.auto_head[3] = static_cast<f32>(h[3]);
  w.auto_head_alpha = static_cast<f32>(head_alpha);
  w.auto_body[0] = static_cast<f32>(body[0]);
  w.auto_body[1] = static_cast<f32>(body[2]);  // the observation's order: z, roll, pitch
  w.auto_body[2] = static_cast<f32>(body[1]);
  w.auto_body_active = body_active;
  w.auto_mouth = static_cast<f32>(std::clamp(std::max(mouth_extra, b.mouth_now()), 0.0, 1.0));
  if (b.state == S::carry || b.state == S::act) w.auto_mouth = 0;  // Kern: the beak holds what it carries
  w.auto_wheee = wheee;
  w.state = b.state == S::fetch && b.ball ? "BallPlay" : b.state == S::act ? (b.ball ? "Kick" : "Pick") : state_name(b.state);
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
// kTau: above

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
