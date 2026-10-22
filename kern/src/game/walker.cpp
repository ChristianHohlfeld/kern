// Walkers: articulated robots that move on their own hinge servos, and the
// Micro Duck (Pollen Robotics' 25 cm, 15-servo biped; kinematics, joint axes,
// ranges and masses from its MJCF, kinematics/assets/alpha/robot_walk.xml).
//
// The real duck walks with a reinforcement-learning policy; Kern drives it with
// a procedural gait instead: a crouched stance, sideways weight shift on the hip
// rolls, swing-foot lift and stride from two-link leg IK that keeps the feet flat,
// hip yaw for turning, ankle and hip corrections from the trunk's tilt, and an
// optional gyro ("balance") that helps the trunk stay upright.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "kern/game/runtime.h"
#include "kern/game/walker.h"
#include "runtime_internal.h"

namespace kern::game {

namespace {

constexpr f32 kPi = 3.14159265f;
using rt::kDegree;

struct DuckLink {
  const char* name;
  i32 parent;           // index into the list (-1: the trunk itself)
  Vec3 pos;             // joint position (world, standing rest pose)
  Vec3 axis;            // joint axis (world)
  f32 lower, upper;     // radians
  f32 mass;             // kg
};

// MJCF bodies in the order of the chain (index 0 = trunk_base, pos 0 0 .12).
const DuckLink kLinks[] = {
    {"trunk_base", -1, {0, 0, .12f}, {0, 0, 0}, 0, 0, .264385f},
    {"left_hip_yaw", 0, {.006f, .0175f, .115f}, {0, 0, -1}, -.4363f, .5236f, .0235131f},
    {"left_hip_roll", 1, {.0225f, .0175f, .1025f}, {1, 0, 0}, -.38397f, .38397f, .00521f},
    {"left_hip_pitch", 2, {.004f, .0425f, .1025f}, {0, 1, 0}, -1.5708f, 1.5708f, .04315f},
    {"left_knee", 3, {-.0318f, .0385f, .0805f}, {0, -1, 0}, -1.5708f, 1.5708f, .0215844f},
    {"left_ankle", 4, {-.0318f, .0645f, .0385f}, {0, 1, 0}, -1.5708f, 1.5708f, .0264653f},
    {"right_hip_yaw", 0, {.006f, -.0175f, .115f}, {0, 0, -1}, -.5236f, .4363f, .0235131f},
    {"right_hip_roll", 6, {.0225f, -.0175f, .1025f}, {1, 0, 0}, -.38397f, .38397f, .00521f},
    {"right_hip_pitch", 7, {.004f, -.0425f, .1025f}, {0, -1, 0}, -1.5708f, 1.5708f, .04315f},
    {"right_knee", 8, {-.0318f, -.0385f, .0805f}, {0, 1, 0}, -1.5708f, 1.5708f, .0215844f},
    {"right_ankle", 9, {-.0318f, -.0645f, .0385f}, {0, -1, 0}, -1.5708f, 1.5708f, .0264654f},
    {"neck_pitch", 0, {.026f, .0145f, .1524f}, {0, -1, 0}, -1.5708f, 1.0472f, .0368414f},
    {"head_pitch", 11, {.026f, .0145f, .2024f}, {0, 1, 0}, -1.5708f, 1.5708f, .00572f},
    {"head_yaw", 12, {.026f, 0, .2211f}, {0, 0, 1}, -2.967f, 2.967f, .02492f},
    {"head_roll", 13, {.0081f, 0, .2356f}, {-1, 0, 0}, -.4363f, .4363f, .172649f},
};
constexpr i32 kLinkCount = static_cast<i32>(sizeof kLinks / sizeof kLinks[0]);
// Box3D is tuned for objects of 0.1 .. 10 m (docs/loose_ends.md "Coordinate Systems"): the real
// duck's 2-4 cm links are below that, so Kern builds it at 4x with one conversion factor at the
// boundary - lengths x4, masses x64, servo torques x256 (dynamically similar: the same poses,
// joint angles and gait, time running 2x slower).
constexpr f32 kDuckScale = 4;
constexpr f32 kSoleZ = .0015f;  // the feet stand on the ground in the rest pose

const Vec3 kShell{.93f, .94f, .95f}, kDark{.16f, .17f, .19f}, kOrange{1.0f, .52f, .1f}, kEye{.03f, .03f, .035f};

Actor part(const char* name, Shape shape, Vec3 color) {
  Actor a;
  a.name = name;
  a.mesh = MeshComponent{};
  a.mesh->shape = shape;
  a.mesh->material.color = color;
  a.mesh->collision = false;
  a.mesh->material.roughness = .45f;
  return a;
}

/// One conversion factor at the Box3D boundary (see kDuckScale).
std::vector<Actor> scaled(std::vector<Actor> v) {
  const f32 s = kDuckScale, s3 = s * s * s, s4 = s3 * s;
  for (Actor& a : v) {
    a.transform.position = a.transform.position * s;
    if (a.mesh) { a.mesh->size = a.mesh->size * s; a.mesh->radius *= s; a.mesh->height *= s; }
    for (ColliderComponent& c : a.colliders) { c.center = c.center * s; c.size = c.size * s; c.radius *= s; c.length *= s; }
    if (a.body) a.body->mass *= s3;
    for (JointComponent& j : a.joints) j.max_motor_force *= s4;
    if (a.controller) {
      a.controller->max_speed *= std::sqrt(s);  // Froude-similar walking speed
      a.controller->camera_distance *= s;
      a.controller->camera_height *= s;
    }
  }
  return v;
}

}  // namespace

std::vector<Actor> micro_duck_actors() {
  std::vector<Actor> out;  // parent / joint target = index + 1 (the catalog's convention)
  const Vec3 root = kLinks[0].pos;
  auto add = [&](Actor a, Vec3 world, i32 parent) {
    a.transform.position = parent >= 0 ? world - root : world;
    a.parent = parent >= 0 ? static_cast<ActorId>(parent + 1) : 0;
    out.push_back(std::move(a));
    return static_cast<i32>(out.size() - 1);
  };
  // Links are children of the trunk in the outliner (their world positions minus the trunk's);
  // the visual shells are children of their links.
  std::vector<i32> link_index(kLinkCount, -1);
  auto collider = [](Actor& a, Vec3 center, Vec3 size, f32 friction = .8f) {
    ColliderComponent c;
    c.shape = ColliderShape::box;
    c.center = center;
    c.size = size;
    c.friction = friction;
    c.group = -31;  // the duck's parts never collide with each other
    a.colliders.push_back(c);
  };
  for (i32 i = 0; i < kLinkCount; ++i) {
    const DuckLink& l = kLinks[i];
    Actor a;
    a.name = i == 0 ? "Micro Duck" : l.name;
    a.body = BodyComponent{};
    // Box3D's solver wants neighbouring masses within ~10x: the 5 g hip-roll bracket would let
    // its joints stretch under servo torque, so the small links weigh at least 30 g.
    a.body->mass = max_of(l.mass, .02f);
    a.body->allow_sleep = false;
    a.body->angular_damping = .2f;
    a.body->impact_sound = "";
    if (i == 0) {
      ControllerComponent c;
      c.mode = ControlMode::walker;
      c.max_speed = .18f;
      c.turn_rate = 70;
      c.boost = false;
      c.jump_speed = 0;
      c.camera_distance = .85f;
      c.camera_height = .45f;
      c.camera_fov = 55;
      c.engine_sound = "builtin:servo";
      c.jump_sound = "builtin:quack";
      a.controller = c;
      collider(a, {-.012f, 0, .006f}, {.105f, .08f, .07f});
    } else {
      JointComponent j;
      j.kind = JointKind::hinge;
      j.target = static_cast<ActorId>(link_index[static_cast<std::size_t>(l.parent)] + 1);
      j.axis = l.axis;
      j.enable_limit = true;
      j.lower = l.lower / kPi * 180;
      j.upper = l.upper / kPi * 180;
      j.enable_motor = true;
      j.max_motor_force = .5f;  // N*m at real size, a Dynamixel XL330 class servo
      j.servo = true;
      j.servo_gain = 22;
      a.joints.push_back(j);
    }
    // Collision boxes (actor-local, the actor sits at its joint).
    const std::string n = l.name;
    const f32 side = n.rfind("left", 0) == 0 ? 1.0f : -1.0f;
    if (n.find("hip_yaw") != std::string::npos || n.find("hip_roll") != std::string::npos) collider(a, {.006f, 0, -.004f}, {.022f, .018f, .016f});
    else if (n.find("hip_pitch") != std::string::npos) collider(a, {-.018f, -.002f * side, -.011f}, {.03f, .022f, .03f});
    else if (n.find("knee") != std::string::npos) collider(a, {0, .013f * side, -.021f}, {.022f, .024f, .042f});
    else if (n.find("ankle") != std::string::npos) collider(a, {.012f, -.014f * side, -.03f}, {.092f, .046f, .016f}, 1.0f);
    else if (n == "neck_pitch") collider(a, {0, -.0145f, .025f}, {.028f, .03f, .05f});
    else if (n == "head_pitch") collider(a, {0, -.0145f, .009f}, {.02f, .02f, .018f});
    else if (n == "head_yaw") collider(a, {0, 0, .007f}, {.02f, .02f, .014f});
    else if (n == "head_roll") collider(a, {.033f, .0015f, .004f}, {.1f, .085f, .07f});
    const i32 at = i == 0 ? add(a, l.pos, -1) : add(a, l.pos, 0);
    link_index[static_cast<std::size_t>(i)] = at;
  }
  // Visual shells (welded to their links).
  auto shell = [&](i32 link, const char* name, Shape shape, Vec3 color, Vec3 world_center, Vec3 size, f32 radius = 0, f32 height = 0) {
    Actor v = part(name, shape, color);
    v.mesh->size = size;
    if (radius > 0) v.mesh->radius = radius;
    if (height > 0) v.mesh->height = height;
    const i32 li = link_index[static_cast<std::size_t>(link)];
    v.transform.position = world_center - kLinks[link].pos;
    v.parent = static_cast<ActorId>(li + 1);
    out.push_back(std::move(v));
  };
  // Trunk: a rounded white body with a dark servo bay.
  shell(0, "Body Shell", Shape::sphere, kShell, {-.014f, 0, .125f}, {}, .056f);
  out.back().transform.scale = {1.05f, .82f, .7f};
  shell(0, "Servo Bay", Shape::box, kDark, {.004f, 0, .1f}, {.05f, .07f, .03f});
  shell(0, "Tail", Shape::cone, kShell, {-.07f, 0, .14f}, {}, .016f, .035f);
  out.back().transform.rotation = {0, -70, 0};
  for (f32 s : {1.0f, -1.0f}) {
    const i32 o = s > 0 ? 0 : 5;
    shell(1 + o, s > 0 ? "Hip Yaw Servo L" : "Hip Yaw Servo R", Shape::box, kDark, {.008f, .0175f * s, .11f}, {.022f, .02f, .018f});
    shell(2 + o, s > 0 ? "Hip Roll Servo L" : "Hip Roll Servo R", Shape::box, kDark, {.016f, .03f * s, .1025f}, {.024f, .016f, .02f});
    shell(3 + o, s > 0 ? "Thigh L" : "Thigh R", Shape::box, kShell, {-.014f, .041f * s, .091f}, {.034f, .024f, .034f});
    shell(4 + o, s > 0 ? "Shin L" : "Shin R", Shape::box, kDark, {-.0318f, .051f * s, .059f}, {.02f, .026f, .044f});
    shell(5 + o, s > 0 ? "Foot L" : "Foot R", Shape::box, kOrange, {-.02f, .0504f * s, .0085f}, {.092f, .046f, .014f});
  }
  shell(11, "Neck", Shape::cylinder, kShell, {.026f, .0145f, .177f}, {}, .013f, .05f);
  shell(14, "Head", Shape::sphere, kShell, {.04f, .0015f, .241f}, {}, .046f);
  out.back().transform.scale = {1.05f, .95f, .85f};
  shell(14, "Beak", Shape::box, kOrange, {.087f, 0, .228f}, {.038f, .034f, .011f});
  shell(14, "Eye L", Shape::sphere, kEye, {.078f, .022f, .25f}, {}, .0065f);
  shell(14, "Eye R", Shape::sphere, kEye, {.078f, -.022f, .25f}, {}, .0065f);
  return scaled(std::move(out));
}

// ---------------------------------------------------------------------------
// gait

namespace {

/// Sagittal plane (x forward, z up): rotation about +Y by `b`.
Vec2 rot_y(Vec2 v, f32 b) { return {v.x * std::cos(b) + v.y * std::sin(b), -v.x * std::sin(b) + v.y * std::cos(b)}; }

/// Two-link IK in the sagittal plane: thigh `d1`, shin `d2` (rest vectors), rotations b1 (thigh)
/// and b1 + b2 (shin) about +Y so that d1' + d2' reaches `p`. Newton from the last answer.
void leg_ik(Vec2 d1, Vec2 d2, Vec2 p, f32& b1, f32& b2) {
  for (int it = 0; it < 12; ++it) {
    const Vec2 a = rot_y(d1, b1), c = rot_y(d2, b1 + b2);
    const Vec2 e{p.x - (a.x + c.x), p.y - (a.y + c.y)};
    if (e.x * e.x + e.y * e.y < 1e-12f) break;
    // d/db of rot_y(v, b) = (-v.x sin + v.y cos, -v.x cos - v.y sin) = (z', -x') of the rotated vector
    const Vec2 da{a.y, -a.x}, dc{c.y, -c.x};
    const f32 j11 = da.x + dc.x, j12 = dc.x, j21 = da.y + dc.y, j22 = dc.y;
    const f32 det = j11 * j22 - j12 * j21;
    if (std::fabs(det) < 1e-9f) break;
    b1 += clamp((e.x * j22 - j12 * e.y) / det, -.3f, .3f);
    b2 += clamp((j11 * e.y - e.x * j21) / det, -.3f, .3f);
  }
}

}  // namespace

void GameWorld::drive_walker(const PlayInput& in, f32 dt) {
  const Actor* a = level_.find(pw_->actor);
  auto it = body_of_actor_.find(pw_->actor);
  if (!a || !a->controller || it == body_of_actor_.end()) return;
  const ControllerComponent& c = *a->controller;
  const physics::Body trunk = it->second->body;
  const f32 S = kDuckScale;
  // The servos of this walker by joint name.
  SimJoint* j[kLinkCount] = {};
  for (auto& sj : joints_) {
    if (sj->broken || !sj->servo) continue;
    const Actor* ja = level_.find(sj->actor);
    if (!ja) continue;
    bool mine = false;  // a link of this walker: the walker is one of its ancestors
    for (const Actor* p = ja; p && !mine; p = level_.find(p->parent)) mine = p->id == pw_->actor;
    if (!mine) continue;
    for (i32 i = 1; i < kLinkCount; ++i)
      if (ja->name == kLinks[i].name) j[i] = sj.get();
  }
  if (!j[5] || !j[10]) return;  // no feet: nothing to walk with
  auto servo = [&](i32 idx, f32 radians) {
    if (SimJoint* sj = j[idx]) sj->servo_target = clamp(radians, kLinks[idx].lower, kLinks[idx].upper);
  };

  // ---- state: trunk frame, centre of mass, capture point --------------------------------------
  const physics::Pose tp = physics_.pose(trunk);
  const Quat q = tp.rotation, qi{-q.x, -q.y, -q.z, q.w};
  const Vec3 up = q.rotate({0, 0, 1}), fwd = q.rotate({1, 0, 0});
  const f32 heading = std::atan2(fwd.y, fwd.x);
  const Vec3 hx{std::cos(heading), std::sin(heading), 0}, hy{-hx.y, hx.x, 0};  // heading frame
  const f32 pitch = std::asin(clamp(dot(up, hx), -1.0f, 1.0f));               // > 0: leaning forward
  const f32 roll = std::asin(clamp(dot(up, hy), -1.0f, 1.0f));                // > 0: leaning left
  const Vec3 w = physics_.angular_velocity(trunk);
  const f32 pitch_rate = dot(w, hy), roll_rate = -dot(w, hx);
  f32 mass = physics_.mass(trunk);
  Vec3 com = tp.position * mass, vel = physics_.linear_velocity(trunk) * mass;
  for (i32 i = 1; i < kLinkCount; ++i)
    if (j[i]) {
      const f32 m = physics_.mass(j[i]->b);
      com += physics_.pose(j[i]->b).position * m;
      vel += physics_.linear_velocity(j[i]->b) * m;
      mass += m;
    }
  com = com / mass;
  vel = vel / mass;
  // Feet: the soles under the ankle links.
  Vec3 sole[2];
  for (int side = 0; side < 2; ++side) {
    const physics::Pose fp = physics_.pose(j[side ? 10 : 5]->b);
    const f32 sg = side ? -1.0f : 1.0f;
    sole[side] = fp.position + fp.rotation.rotate(Vec3{.012f, -.014f * sg, -.038f} * S);
  }
  const f32 ground = min_of(sole[0].z, sole[1].z);
  const f32 omega = std::sqrt(9.81f / max_of(.05f, com.z - ground));  // linear inverted pendulum
  const Vec3 capture = com + Vec3{vel.x, vel.y, 0} / omega;

  // ---- commands --------------------------------------------------------------------------------
  const f32 fwd_cmd = clamp(in.move_y, -1.0f, 1.0f) * c.max_speed;
  const f32 turn_cmd = -clamp(in.move_x, -1.0f, 1.0f) * c.turn_rate * kDegree;
  pw_->walk_speed += (fwd_cmd - pw_->walk_speed) * min_of(1.0f, dt * 2);
  pw_->walk_turn += (turn_cmd - pw_->walk_turn) * min_of(1.0f, dt * 3);
  // Step when asked to move or when the capture point leaves the support (a push, a slope).
  const Vec3 mid = (sole[0] + sole[1]) * .5f;
  const f32 off = std::hypot(capture.x - mid.x, capture.y - mid.y);
  const bool want = std::fabs(pw_->walk_speed) > .01f || std::fabs(pw_->walk_turn) > .05f || off > .03f * S;
  pw_->gait_amp = clamp(pw_->gait_amp + (want ? dt * 4 : -dt * 2), 0.0f, 1.0f);
  const f32 step_time = .32f * std::sqrt(S);  // one step (half a cycle); Froude-scaled
  const f32 prev_phase = pw_->gait_phase;
  if (pw_->gait_amp > 0 || std::fmod(pw_->gait_phase, .5f) > 1e-4f) pw_->gait_phase += dt / (2 * step_time);
  pw_->gait_phase -= std::floor(pw_->gait_phase);
  const bool new_step = static_cast<i32>(prev_phase * 2) != static_cast<i32>(pw_->gait_phase * 2) || !pw_->feet_valid;
  if (new_step) {
    // Touchdown: the foot that swung is planted where it is; the other one lifts off now.
    for (int side = 0; side < 2; ++side) { pw_->foot_plant[side] = sole[side]; pw_->foot_lift[side] = sole[side]; }
    pw_->feet_valid = true;
    // Where the swing foot lands (Raibert / capture point): under the capture point, ahead by half
    // a stride for the commanded speed, out to the side by the stance width.
    const int sw = static_cast<i32>(pw_->gait_phase * 2) == 0 ? 0 : 1;  // left swings in the first half
    const f32 sg = sw ? -1.0f : 1.0f;
    const Vec3 v_des = hx * pw_->walk_speed;
    Vec3 land = capture + v_des * (step_time * .5f) - v_des / omega + hy * (sg * .05f * S);
    // Keep the step inside what the leg can reach.
    Vec3 d = land - pw_->foot_plant[1 - sw];
    d.z = 0;
    const f32 reach = .06f * S, dl = length(d);
    if (dl > reach) land = pw_->foot_plant[1 - sw] + d * (reach / dl);
    land.z = ground;
    pw_->foot_land = land;
    pw_->swing_side = sw;
  }
  const f32 amp = pw_->gait_amp;
  const f32 s = std::fmod(pw_->gait_phase * 2, 1.0f);  // 0..1 within the step
  const int sw = pw_->swing_side;

  // ---- legs -------------------------------------------------------------------------------------
  const f32 hip_height = (kLinks[3].pos.z - .009f) * S;  // crouched: the hip pitch joints this high
  for (int side = 0; side < 2; ++side) {
    const i32 o = side ? 5 : 0;
    const DuckLink &hr = kLinks[2 + o], &hp = kLinks[3 + o], &kn = kLinks[4 + o], &an = kLinks[5 + o];
    const Vec3 roll_joint = tp.position + q.rotate((hr.pos - kLinks[0].pos) * S);
    // World target of this sole.
    Vec3 target;
    const bool swinging = amp > 0 && side == sw;
    if (swinging) {
      const f32 e = (1 - std::cos(kPi * s)) * .5f;
      target = pw_->foot_lift[side] + (pw_->foot_land - pw_->foot_lift[side]) * e;
      target.z = ground + .014f * S * amp * std::sin(kPi * s);
    } else {
      target = pw_->foot_plant[side];
      target.z = ground;
    }
    // The trunk rides at hip_height over the ground: shift the target so the hip ends up there.
    Vec3 rel = target - roll_joint;
    rel.z = -(hip_height - (target.z - ground)) - (hr.pos.z - hp.pos.z) * S;
    // Trunk frame; hip roll swings the leg sideways about the roll joint (axis +X).
    const Vec3 t = qi.rotate(rel);
    const Vec3 rest = (Vec3{-.0318f + .012f, an.pos.y - .014f * (an.pos.y > 0 ? 1.0f : -1.0f), kSoleZ} - hr.pos) * S;
    const f32 hip_roll = std::atan2(t.y, -t.z) - std::atan2(rest.y, -rest.z);
    // In the leg's plane (after undoing the roll), relative to the hip pitch joint.
    const f32 cr = std::cos(-hip_roll), sr = std::sin(-hip_roll);
    const Vec3 tr{t.x, t.y * cr - t.z * sr, t.y * sr + t.z * cr};
    const Vec3 pr = tr - (hp.pos - hr.pos) * S;
    const Vec2 d1{(kn.pos.x - hp.pos.x) * S, (kn.pos.z - hp.pos.z) * S}, d2{(an.pos.x - kn.pos.x) * S, (an.pos.z - kn.pos.z) * S};
    const Vec2 d3{(-.0318f + .012f - an.pos.x) * S, (kSoleZ - an.pos.z) * S};
    f32& b1 = pw_->leg_ik[side * 2];
    f32& b2 = pw_->leg_ik[side * 2 + 1];
    leg_ik(d1, d2, {pr.x - d3.x, pr.z - d3.y}, b1, b2);
    // Posture: the stance hip turns the trunk upright (hip strategy), the stance ankle keeps the
    // sole flat on the ground and pushes the centre of pressure toward the capture point.
    f32 hip_pitch = b1, hip_r = hip_roll, ankle = -(b1 + b2) - pitch;
    if (!swinging) {
      hip_pitch += -(.8f * pitch + .08f * pitch_rate);
      hip_r += .8f * roll + .08f * roll_rate;
      ankle += clamp(dot(capture - target, hx) / (.04f * S), -1.0f, 1.0f) * .15f;
    }
    servo(3 + o, hip_pitch / hp.axis.y);
    servo(4 + o, b2 / kn.axis.y);
    servo(5 + o, ankle / an.axis.y);
    servo(2 + o, hip_r);
    // Turning: the swing leg's hip yaw points it into the turn, the stance leg's turns back.
    const f32 yaw = clamp(pw_->walk_turn * step_time, -.35f, .35f) * amp * (swinging ? s : 1 - s);
    servo(1 + o, -yaw);
  }
  // Head: looks ahead, into the turn; a quack nods it.
  const bool quack = in.jump && !pw_->jump_latch;
  pw_->jump_latch = in.jump;
  if (quack) { cue(c.jump_sound, tp.position + Vec3{0, 0, .1f * S}, .9f, 1, true, audio::Bus::sfx, pw_->actor); pw_->quack = .35f * std::sqrt(S); }
  pw_->quack = max_of(0.0f, pw_->quack - dt);
  const f32 nod = pw_->quack > 0 ? std::sin(pw_->quack / (.35f * std::sqrt(S)) * kPi * 2) * .35f : 0;
  servo(11, 0);
  servo(12, nod + pitch * .4f);
  servo(13, clamp(pw_->walk_turn * .3f, -.6f, .6f));
  servo(14, -roll * .4f);
  // Balance assist: a reaction-wheel style torque on the trunk (bounded, damped), off at 0.
  if (c.balance > 0) {
    const f32 inertia = mass * (.06f * S) * (.06f * S);  // whole duck about its feet, roughly
    const f32 wn = 2 * kPi * 1.2f;                         // 1.2 Hz correction
    Vec3 tq = cross(up, {0, 0, 1}) * (inertia * wn * wn) - Vec3{w.x, w.y, 0} * (inertia * 2 * wn);
    const f32 cap = c.balance * mass * 9.81f * .02f * S;   // like moving 2 cm of support
    if (length(tq) > cap) tq = tq * (cap / length(tq));
    physics_.apply_torque(trunk, tq * c.balance);
  }
  if (new_step && amp > .3f) cue("builtin:step", sole[1 - sw], .12f, 2.2f, true, audio::Bus::sfx, pw_->actor);
  hud_.speed = std::hypot(vel.x, vel.y);
}

void GameWorld::drive_servos(f32 dt) {
  (void)dt;
  for (auto& sj : joints_) {
    if (!sj->servo || sj->broken || !sj->joint.valid()) continue;
    // Position servo (Box3D's documented recipe): motor speed proportional to the error, limited
    // speed and stall torque, like a Dynamixel in position mode.
    const f32 err = sj->servo_target - physics_.joint_angle(sj->joint);
    physics_.set_joint_motor(sj->joint, true, clamp(err * sj->servo_gain, -8.0f, 8.0f), sj->servo_torque);
  }
}

}  // namespace kern::game
