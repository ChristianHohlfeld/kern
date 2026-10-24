// Kern's play layer for the Micro Duck (docs/lehrer.md, "Spielen"): ball and sock, learned.
//
// Pollen's networks are the duck's moves (walk, ground pick, kicks ...); none of them sees. What to
// do with a ball or a sock in front of the camera - look at it, walk up to it, kick it, pick it up,
// carry it off and spit it out - is a network of its own, play.onnx, trained by the Lehrer. It sits
// where a gamepad or Logic sits: 25 times a second it reads what the camera's detector reports and
// what the duck feels, and answers with the walking command, the head, the beak and which move to
// start. No hand-written behaviour decides anything here.
//
// Around the network there is only sensing:
// * the camera's detector (a device model like the servo's): the nearest ball and sock in the head
//   camera's picture with nothing in front, their position estimated on the floor (real metres,
//   the trunk's heading frame), with the noise, drop-outs the Lehrer varies;
// * a tracker that keeps the last estimate while the object is out of view (odometry), with its
//   age; objects the duck just kicked or put down are not reported for a while (habituation);
// * proprioception: left foot, beak (encoder: holding?), the move running, how far it carried;
// * its own recent past: how long it has stood (both soles down), how long it has walked forward
//   or back.
// The network is engaged while it knows of an object, holds something or runs a move it started;
// otherwise Pollen's brain (walker_brain.cpp) roams as before.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>

#include "kern/core/dmath.h"
#include "kern/game/runtime.h"
#include "kern/game/walker.h"
#include "kern/game/walker_policy.h"
#include "runtime_internal.h"

namespace kern::game {

namespace {

constexpr f32 kMouthClosedRad = -5 * rt::kDegree, kMouthOpenRad = 30 * rt::kDegree;  // walker.cpp
constexpr f32 kForgetS = 3;          // an object not seen for this long is no longer reported
constexpr f32 kNearForgetS = 30;     // ... unless it was close (below the camera: it does not move by itself)
constexpr f32 kBallRestS = 15, kSockRestS = 40;  // habituation after a kick / after putting a sock down
constexpr f32 kPutDownM = 0.25f;                // carried at least this far, letting go is putting it down
constexpr f32 kStillCapS = 4;                   // standing still / walking: reported up to this long
constexpr f32 kStartKickS = 0.6f, kMinWalkVx = 0.15f;  // autod's (walker_brain.cpp kStartKickS, kMinWalkVx)
constexpr f32 kDeadBand = 0.2f;  // of the walking command's range (0.06 m/s, 0.04 m/s, 0.2 rad/s), like a gamepad stick's
// The action's ranges (the pad's: robotd_params::drive; the head's training ranges).
constexpr f32 kVx = 0.3f, kVy = 0.2f, kWz = 1.0f, kYaw = 1.1f, kPitch = 1.1f, kNeck = 1.1f;

}  // namespace


namespace {

bool is_sock(const Actor& a) { return std::find(a.tags.begin(), a.tags.end(), "sock") != a.tags.end(); }
bool is_ball(const Actor& a) {
  return !is_sock(a) && a.body && a.body->motion == Motion::dynamic && a.mesh && a.mesh->shape == Shape::sphere &&
         (std::find(a.tags.begin(), a.tags.end(), "ball") != a.tags.end() || a.name == "Ball");
}

}  // namespace

void GameWorld::ensure_play(WalkerBrain& w) const {
  if (w.play) return;
  w.play = std::make_shared<WalkerBrain::Play>();
  w.play->rng = (0x2545F4914F6CDD1Dull ^ (static_cast<u64>(w.actor) * 0x9E3779B97F4A7C15ull)) | 1;
}

bool GameWorld::walker_play_available(const WalkerBrain& w) const {
  return w.kern_extras && !w.rollers && w.mouth && (w.play && w.play->external ? true : walker_policy("play.onnx") != nullptr);
}

void GameWorld::sense_play(WalkerBrain& w, f32 dt) {
  ensure_play(w);
  WalkerBrain::Play& p = *w.play;
  const f32 S = kWalkerScale;
  for (auto it = p.rest.begin(); it != p.rest.end();) it = (it->second -= dt) <= 0 ? p.rest.erase(it) : std::next(it);
  // The camera's detector: per class the nearest object it sees (centre of its parts, on the floor).
  const physics::Pose tp = physics_.pose(w.trunk);
  f32 best[2] = {1e30f, 1e30f};
  ActorId found[2] = {0, 0};
  f32 at[2][2] = {};
  for (const Actor& a : level_.actors) {
    const i32 c = is_ball(a) ? 0 : is_sock(a) ? 1 : -1;
    if (c < 0 || a.parent != 0 || p.rest.count(a.id) || a.id == p.carried) continue;
    Vec3 sum{};
    int n = 0;
    bool seen = false;
    for (const Actor& part : level_.actors) {
      if (part.id != a.id && part.parent != a.id) continue;
      Mat4 m;
      if (!actor_matrix(part.id, m)) continue;
      sum = sum + m.translation();
      ++n;
      bool visible = false;
      f32 bearing = 0, distance = 0;
      if (!seen && walker_sees(w.actor, part.id, visible, bearing, distance)) seen = visible;
    }
    if (!n || !seen) continue;
    const Vec3 centre = sum * (1.0f / static_cast<f32>(n));
    if (centre.z > (c == 0 ? 0.08f : 0.03f) * S) continue;  // not on the floor (in a beak, say)
    const f32 d = kern::dm::hypot(centre.x - tp.position.x, centre.y - tp.position.y) / S;
    if (d < best[c]) { best[c] = d; found[c] = a.id; at[c][0] = centre.x / S; at[c][1] = centre.y / S; }
  }
  for (i32 c = 0; c < 2; ++c) {
    WalkerBrain::Play::Track& t = p.track[c];
    t.age += dt;
    if (found[c]) {
      // Monocular estimate on the floor: its error grows with the distance.
      f32 x = at[c][0], y = at[c][1];
      if (p.detector.sigma_rel > 0 || p.detector.sigma_abs > 0) {
        const f32 sd = p.detector.sigma_abs + p.detector.sigma_rel * best[c];
        x += sd * p.normal();
        y += sd * p.normal();
      }
      const bool drop = p.detector.dropout > 0 && p.uniform() < p.detector.dropout;
      if (!drop) { t.known = true; t.actor = found[c]; t.x = x; t.y = y; t.age = 0; }
    }
    // Close up an object is below the camera: the tracker keeps where it saw it longer (it does
    // not move by itself until the duck touches it).
    const f32 near = kern::dm::hypot(t.x - tp.position.x / S, t.y - tp.position.y / S);
    if (t.age > (near < 0.35f ? kNearForgetS : kForgetS) || p.rest.count(t.actor)) t.known = false;
  }
  // Carrying: from where it took hold of something, how far and how long.
  const bool holding = walker_holding(w.actor);
  if (holding && !p.was_holding) {
    p.carry_from[0] = tp.position.x / S;
    p.carry_from[1] = tp.position.y / S;
    p.carry_time = 0;
    p.carried = p.track[1].actor;  // what it went for
  }
  if (holding) p.carry_time += dt;
  if (!holding && p.was_holding) {
    // Carried off and put down: leave that sock alone for a while. Lost on the way up or slipped
    // out right away: it is still there to be picked up.
    const f32 carried_m = kern::dm::hypot(tp.position.x / S - p.carry_from[0], tp.position.y / S - p.carry_from[1]);
    if (p.carried && carried_m >= kPutDownM) {
      p.rest[p.carried] = kSockRestS;
      p.track[1].known = false;
    }
    p.carried = 0;
  }
  p.was_holding = holding;
}

bool GameWorld::walker_play_observation(ActorId id, f32 o[kPlayObs]) const {
  WalkerBrain* wp = walker_brain(id);
  if (!wp) return false;
  WalkerBrain& w = *wp;
  ensure_play(w);
  const WalkerBrain::Play& p = *w.play;
  const f32 S = kWalkerScale;
  const physics::Pose tp = physics_.pose(w.trunk);
  const Vec3 fwd = tp.rotation.rotate({1, 0, 0});
  const f32 heading = kern::dm::atan2(fwd.y, fwd.x), c = kern::dm::cos(heading), s = kern::dm::sin(heading);
  const f32 px = tp.position.x / S, py = tp.position.y / S;
  auto local = [&](f32 x, f32 y, f32& ax, f32& ay) {  // world (real) -> the heading frame: ahead, left
    const f32 dx = x - px, dy = y - py;
    ax = c * dx + s * dy;
    ay = -s * dx + c * dy;
  };
  std::fill(o, o + kPlayObs, 0.0f);
  for (i32 k = 0; k < 2; ++k) {
    const WalkerBrain::Play::Track& t = p.track[k];
    if (!t.known) continue;
    o[4 * k] = 1;
    local(t.x, t.y, o[4 * k + 1], o[4 * k + 2]);
    o[4 * k + 3] = min_of(t.age, kForgetS);
  }
  if (w.bodies[5].valid()) {  // the left ankle
    const physics::Pose ap = physics_.pose(w.bodies[5]);
    local(ap.position.x / S, ap.position.y / S, o[8], o[9]);
  }
  o[10] = walker_holding(w.actor) ? 1.0f : 0.0f;
  if (w.mouth) o[11] = clamp((physics_.joint_angle(w.mouth->joint) - kMouthClosedRad) / (kMouthOpenRad - kMouthClosedRad), -0.5f, 1.5f);
  const std::string driving = walker_driving(w.actor);
  o[12] = driving == "walk" || driving == "stand" ? 1.0f : 0.0f;
  o[13] = w.pick_phase >= 0 ? 1.0f : 0.0f;
  o[14] = driving == "kick_left" ? 1.0f : 0.0f;
  o[15] = w.pick_phase >= 0 ? w.pick_phase : 0.0f;
  const Quat qi{-tp.rotation.x, -tp.rotation.y, -tp.rotation.z, tp.rotation.w};
  const Vec3 g = qi.rotate({0, 0, -1});
  o[16] = g.x;
  o[17] = g.y;
  o[18] = g.z;
  if (o[10] > 0) {
    o[19] = min_of(kern::dm::hypot(px - p.carry_from[0], py - p.carry_from[1]), 2.0f);
    o[20] = min_of(p.carry_time, 10.0f);
  }
  o[21] = min_of(p.still, kStillCapS);  // how long it has stood still (both soles down, no move)
  o[22] = clamp(p.walking, -kStillCapS, kStillCapS);  // how long it has been walking forward (+) / back (-)
  return true;
}

void GameWorld::play_walker(WalkerBrain& w, f32 dt) {
  if (!walker_play_available(w)) { if (w.play) w.play->engaged = false; return; }
  ensure_play(w);
  WalkerBrain::Play& p = *w.play;
  // Decide at the start of every kPlayDecisionTicks-th control tick, from what the duck knew before it (the Lehrer
  // reads the same observation between ticks, walker_play_observation), then sense this tick.
  const bool decide = p.tick % kPlayDecisionTicks == 0;
  ++p.tick;
  const bool holding = walker_holding(w.actor);
  const bool move_running = w.skill >= 0 || w.pick_phase >= 0;
  if (!move_running) p.started_move = false;
  // Steered by hand (the pad, the phone's stick, a robot key) or by a Logic command: the network waits.
  const bool driven = w.manual_left > 0 || w.drive_left != 0;
  p.engaged = p.external || (!driven && (p.track[0].known || p.track[1].known || holding || p.started_move));
  if (decide && p.engaged) {
    f32 a[kPlayActions];
    if (p.external) {
      std::memcpy(a, p.external_action, sizeof a);
    } else {
      f32 o[kPlayObs];
      walker_play_observation(w.actor, o);
      const auto net = walker_policy("play.onnx");
      if (net && static_cast<i32>(net->dims.front()) == kPlayObs && static_cast<i32>(net->dims.back()) == kPlayActions) net->run(o, a);
      else std::fill(a, a + kPlayActions, 0.0f);
    }
    for (i32 i = 0; i < kPlayActions; ++i) p.action[i] = std::isfinite(a[i]) ? clamp(a[i], -1.5f, 1.5f) : 0.0f;
    // The moves it asks for (the robot's daemon refuses one while another runs).
    if (!move_running && !holding && p.action[6] > 0.5f && walker_skill(w.actor, "kick_left").empty()) {
      p.started_move = true;
      if (p.track[0].actor) p.rest[p.track[0].actor] = kBallRestS;  // kicked: let it roll
      p.track[0].known = false;
    }
    if (!move_running && !holding && p.action[7] > 0.5f && walker_skill(w.actor, "pick").empty()) p.started_move = true;
  }
  sense_play(w, dt);
  // The walking command has a dead band like a gamepad's stick: "stand still" stays standing still
  // when a network answers 0.1 instead of 0.
  auto band = [](f32 v, f32 b) { v = clamp(v, -1.0f, 1.0f); return std::fabs(v) < b ? 0.0f : v; };
  auto walk = [&](f32 v) { return band(v, kDeadBand); };
  auto forward = walk;
  // Standing still is what the legs do, not what it was told: both soles on the floor. (Told "a little
  // forward", Pollen's gait may not step at all - that is standing, wherever the threshold lies in
  // this physics or on the real duck.)
  bool feet_down = true;
  std::vector<std::pair<physics::Body, f32>> touch;
  for (i32 foot : {5, 14}) {  // the ankle links carry the soles
    bool down = false;
    touch.clear();
    if (w.bodies[foot].valid()) physics_.touching(w.bodies[foot], touch);
    for (const auto& [other, impulse] : touch) {
      bool own = false;
      for (const physics::Body& b : w.bodies) own = own || (b.valid() && b == other);
      down = down || !own;
    }
    feet_down = feet_down && down;
  }
  p.still = p.engaged && feet_down && !move_running ? p.still + dt : 0.0f;
  const f32 dir = forward(p.action[0]) > 0 ? 1.0f : forward(p.action[0]) < 0 ? -1.0f : 0.0f;  // forward, back, neither
  p.walking = p.engaged && !move_running && dir != 0 && (p.walking == 0 || (p.walking > 0) == (dir > 0)) ? p.walking + dir * dt : 0.0f;
  if (!p.engaged) return;
  // Engaged: the network's walking command, head and beak replace the roaming brain's.
  const f32* a = p.action;
  // Backwards as Pollen's autod starts the gait (walker_brain.cpp, walkable): 0.6 s at full speed,
  // then never slower than 0.15 m/s - from a stand a slower step back does not step at all. Forwards
  // the network sets the pace itself (it learnt to give the gait a firm push from a stand).
  {
    const f32 v = forward(a[0]) * kVx;
    w.auto_cmd[0] = v >= 0 ? v : -(-p.walking < kStartKickS ? kVx : max_of(-v, kMinWalkVx));
  }
  w.auto_cmd[1] = walk(a[1]) * kVy;
  w.auto_cmd[2] = walk(a[2]) * kWz;
  w.auto_head[0] = -max_of(0.0f, clamp(a[8], -1.0f, 1.0f)) * kNeck;  // bows the neck (0 .. -1.1)
  w.auto_head[1] = clamp(a[4], -0.5f, 1.0f) * kPitch;                // + looks down
  w.auto_head[2] = clamp(a[3], -1.0f, 1.0f) * kYaw;
  w.auto_head[3] = 0;
  w.auto_head_alpha = 0.3f;
  w.auto_body_active = false;
  w.auto_wheee = false;
  w.auto_mouth = a[5] > 0 ? 1.0f : 0.0f;
  w.state = "Play";
}

bool GameWorld::set_walker_play_external(ActorId id, bool on) {
  WalkerBrain* w = walker_brain(id);
  if (!w) return false;
  ensure_play(*w);
  w->play->external = on;
  return true;
}

bool GameWorld::set_walker_play_action(ActorId id, const f32 action[kPlayActions]) {
  WalkerBrain* w = walker_brain(id);
  if (!w) return false;
  ensure_play(*w);
  std::memcpy(w->play->external_action, action, sizeof(f32) * kPlayActions);
  return true;
}

bool GameWorld::set_walker_detector(ActorId id, const DetectorModel& m) {
  WalkerBrain* w = walker_brain(id);
  if (!w) return false;
  ensure_play(*w);
  WalkerBrain::Play& p = *w->play;
  p.detector = m;
  p.rng = m.seed | 1;
  return true;
}

bool GameWorld::walker_play_due(ActorId id) const {
  const WalkerBrain* w = walker_brain(id);
  return w && walker_tick_due(id) && (!w->play || w->play->tick % kPlayDecisionTicks == 0);
}

bool GameWorld::walker_playing(ActorId id) const {
  const WalkerBrain* w = walker_brain(id);
  return w && w->play && w->play->engaged;
}

}  // namespace kern::game
