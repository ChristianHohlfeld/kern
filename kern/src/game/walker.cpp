// Walkers: articulated robots that move on their own hinge servos, driven by a control policy.
//
// The Micro Duck is Pollen Robotics' 25 cm, 14-servo biped (github.com/pollen-robotics/microduck).
// It walks, turns, stands and gets up with "velstand", a neural network trained in MuJoCo
// (pollen-robotics/microduck_rl, Apache-2.0). Kern runs that network unchanged, on the model it was
// trained on (robot_allcollisions_backlash.xml: bodies, joints, masses, inertia tensors, colliding
// parts), with the observation, action and servo conventions of training. docs/walker.md has the
// whole contract, the measurements and what differs from MuJoCo.
//
// Generated data (tools/micro_duck_import.py): micro_duck_model.inc, micro_duck_velstand.inc.
#include <algorithm>
#include <cmath>
#include <cstring>

#include "kern/game/runtime.h"
#include "kern/game/walker.h"
#include "runtime_internal.h"

namespace kern::game {

namespace {

struct DuckLinkDef {
  const char* body;      // MJCF body
  const char* joint;     // MJCF joint ("" for the trunk)
  i32 parent;            // index into kDuckLinks (-1: the trunk)
  Vec3 pos;              // joint position in the home pose (world, metres, soles on the ground)
  Vec3 axis;             // hinge axis in the home pose (world)
  f32 lower, upper;      // radians (MJCF angles)
  f32 home;              // the home (STAND) angle, radians
  f32 mass;              // kg
  Vec3 com;              // centre of mass relative to pos (world axes)
  f32 inertia[6];        // about the centre of mass, world axes: xx yy zz xy xz yz
};
struct DuckBoxDef {
  i32 link;
  Vec3 center;           // relative to the link's pos (world axes)
  Vec3 rotation;         // Euler degrees
  Vec3 size;
  bool sole;
};

#include "micro_duck_model.inc"
#include "micro_duck_velstand.inc"

constexpr i32 kLinkCount = static_cast<i32>(sizeof kDuckLinks / sizeof kDuckLinks[0]);
constexpr i32 kJoints = 14;
static_assert(kLinkCount == kJoints + 1, "trunk + the policy's 14 joints");

// Box3D is tuned for objects of 0.1 .. 10 m (docs/physics.md section 1): the duck's 2-4 cm links
// are below that, so Kern builds it at 4x with one conversion factor at the boundary. Froude
// similarity keeps the motion the same: lengths x S, masses x S^3, inertias x S^5, torques and
// servo stiffness x S^4, damping x S^4.5, time x sqrt(S) - and the policy sees real units.
constexpr f32 kDuckScale = 4;

// The XL330 servo as trained (BAM "m6" model at the nominal 7.4 V; docs/walker.md section 4):
// stiffness K = V * kt/R * 0.5755 /rad, damping = back-EMF kt^2/R + viscous friction, Coulomb
// friction, and the torque at full duty V * kt/R (training runs without the firmware's current
// limit - microduck_rl scripts/infer_policy.py; with the 1.75 A limit it cannot get up from its back).
constexpr f32 kServoStiffness = 0.554f;   // N*m/rad
constexpr f32 kServoDamping = 0.053f;     // N*m*s/rad
constexpr f32 kServoFriction = 0.0048f;   // N*m
constexpr f32 kServoTorque = 0.963f;     // N*m

// Timing in real (unscaled) seconds; Kern's simulation runs sqrt(S) slower.
constexpr f32 kControlPeriod = 0.02f;     // the policy runs at 50 Hz
constexpr f32 kActuatorDelay = 0.02f;     // command -> servo latency (training: 15-30 ms)
// Commands (training ranges) and their smoothing on the robot: EMA 0.2 per control tick.
constexpr f32 kMaxForward = 0.4f, kMaxTurn = 1.0f, kCommandAlpha = 0.2f;

const Vec3 kShell{.93f, .94f, .95f}, kDark{.16f, .17f, .19f}, kOrange{1.0f, .52f, .1f}, kEye{.03f, .03f, .035f};

i32 link_of_joint(const std::string& name) {
  for (i32 i = 1; i < kLinkCount; ++i)
    if (name == kDuckLinks[i].joint) return i;
  return -1;
}

/// A point / rotation given in the MJCF zero pose (all joints at 0) moved into the home pose:
/// from the root down, every joint of the chain turns its subtree by its home angle about its
/// home axis (a joint's axis only depends on its ancestors, so the home axes are the right ones).
void zero_to_home(i32 link, Vec3& p, Quat& r) {
  i32 chain[kLinkCount];
  i32 n = 0;
  for (i32 i = link; i > 0; i = kDuckLinks[i].parent) chain[n++] = i;
  while (n-- > 0) {
    const DuckLinkDef& l = kDuckLinks[chain[n]];
    const Quat q = Quat::axis_angle(l.axis, l.home);
    p = l.pos + q.rotate(p - l.pos);
    r = q * r;
  }
}

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
  const f32 s = kDuckScale, s3 = s * s * s, s4 = s3 * s, s5 = s4 * s, rs = std::sqrt(s);
  for (Actor& a : v) {
    a.transform.position = a.transform.position * s;
    if (a.mesh) { a.mesh->size = a.mesh->size * s; a.mesh->radius *= s; a.mesh->height *= s; }
    for (ColliderComponent& c : a.colliders) { c.center = c.center * s; c.size = c.size * s; c.radius *= s; c.length *= s; }
    if (a.body) {
      a.body->mass *= s3;
      a.body->center_of_mass = a.body->center_of_mass * s;
      for (f32& i : a.body->inertia) i *= s5;
    }
    for (JointComponent& j : a.joints) {
      j.max_motor_force *= s4;
      j.servo_stiffness *= s4;
      j.servo_damping *= s4 * rs;
      j.servo_friction *= s4;
    }
    if (a.controller) {
      a.controller->max_speed *= rs;
      a.controller->camera_distance *= s;
      a.controller->camera_height *= s;
    }
  }
  return v;
}

}  // namespace

std::vector<Actor> micro_duck_actors() {
  std::vector<Actor> out;  // parent / joint target = index + 1 (the catalog's convention)
  const Vec3 root = kDuckLinks[0].pos;
  for (i32 i = 0; i < kLinkCount; ++i) {
    const DuckLinkDef& l = kDuckLinks[i];
    Actor a;
    a.name = i == 0 ? "Micro Duck" : l.joint;  // links are named after their servo joint
    a.transform.position = i == 0 ? l.pos : l.pos - root;
    a.parent = i == 0 ? 0 : 1;  // the links are children of the trunk in the outliner
    a.body = BodyComponent{};
    a.body->mass = l.mass;
    a.body->center_of_mass = l.com;
    std::memcpy(a.body->inertia, l.inertia, sizeof l.inertia);
    a.body->allow_sleep = false;
    a.body->angular_damping = 0;  // MuJoCo bodies have none; the servos damp
    a.body->impact_sound = "";
    if (i == 0) {
      ControllerComponent c;
      c.mode = ControlMode::walker;
      c.max_speed = kMaxForward;  // the policy's command range (m/s, real size)
      c.turn_rate = kMaxTurn / rt::kDegree;
      c.boost = false;
      c.jump_speed = 0;
      c.camera_distance = .85f;
      c.camera_height = .45f;
      c.camera_fov = 55;
      c.engine_sound = "builtin:servo";
      c.jump_sound = "builtin:quack";
      a.controller = c;
    } else {
      JointComponent j;
      j.kind = JointKind::hinge;
      j.target = static_cast<ActorId>(l.parent + 1);
      j.axis = l.axis;
      j.enable_limit = true;
      j.lower = l.lower / rt::kDegree;
      j.upper = l.upper / rt::kDegree;
      j.reference = l.home / rt::kDegree;  // built in the home pose, reading the MJCF angle
      j.enable_motor = true;
      j.servo = true;
      j.servo_target = j.reference;
      j.servo_stiffness = kServoStiffness;
      j.servo_damping = kServoDamping;
      j.servo_friction = kServoFriction;
      j.max_motor_force = kServoTorque;
      a.joints.push_back(j);
    }
    // What collides in training: the soles (friction 1) and the XL330 housings (frictionless).
    for (const DuckBoxDef& b : kDuckBoxes) {
      if (b.link != i) continue;
      ColliderComponent c;
      c.shape = ColliderShape::box;
      c.center = b.center;
      c.rotation = b.rotation;
      c.size = b.size;
      c.friction = b.sole ? 1.0f : 0.0f;
      a.colliders.push_back(c);
    }
    out.push_back(std::move(a));
  }
  // Visual shells (welded to their links), placed in the MJCF zero pose and moved to the home pose.
  const f32 lift = kDuckLinks[0].pos.z - .12f;  // the zero-pose trunk stands at 0.12 m
  auto shell = [&](i32 link, const char* name, Shape shape, Vec3 color, Vec3 zero_center, Vec3 size, f32 radius = 0, f32 height = 0,
                   Vec3 scale = {1, 1, 1}, Vec3 euler = {}) {
    Actor v = part(name, shape, color);
    v.mesh->size = size;
    if (radius > 0) v.mesh->radius = radius;
    if (height > 0) v.mesh->height = height;
    Vec3 p = zero_center + Vec3{0, 0, lift};
    Quat r = euler_to_quat(euler);
    zero_to_home(link, p, r);
    v.transform.position = p - kDuckLinks[link].pos;
    v.transform.rotation = quat_to_euler(r);
    v.transform.scale = scale;
    v.parent = static_cast<ActorId>(link + 1);
    out.push_back(std::move(v));
  };
  shell(0, "Body Shell", Shape::sphere, kShell, {-.014f, 0, .125f}, {}, .056f, 0, {1.05f, .82f, .7f});
  shell(0, "Servo Bay", Shape::box, kDark, {.004f, 0, .1f}, {.05f, .07f, .03f});
  shell(0, "Tail", Shape::cone, kShell, {-.07f, 0, .14f}, {}, .016f, .035f, {1, 1, 1}, {0, -70, 0});
  for (f32 s : {1.0f, -1.0f}) {
    const i32 o = s > 0 ? 0 : 9;  // left leg: links 1-5, right leg: 10-14
    shell(1 + o, s > 0 ? "Hip Yaw Servo L" : "Hip Yaw Servo R", Shape::box, kDark, {.008f, .0175f * s, .11f}, {.022f, .02f, .018f});
    shell(2 + o, s > 0 ? "Hip Roll Servo L" : "Hip Roll Servo R", Shape::box, kDark, {.016f, .03f * s, .1025f}, {.024f, .016f, .02f});
    shell(3 + o, s > 0 ? "Thigh L" : "Thigh R", Shape::box, kShell, {-.014f, .041f * s, .091f}, {.034f, .024f, .034f});
    shell(4 + o, s > 0 ? "Shin L" : "Shin R", Shape::box, kDark, {-.0318f, .051f * s, .059f}, {.02f, .026f, .044f});
    shell(5 + o, s > 0 ? "Foot L" : "Foot R", Shape::box, kOrange, {-.02f, .0504f * s, .0085f}, {.092f, .046f, .014f});
  }
  shell(6, "Neck", Shape::cylinder, kShell, {.026f, .0145f, .177f}, {}, .013f, .05f);
  shell(9, "Head", Shape::sphere, kShell, {.04f, .0015f, .241f}, {}, .046f, 0, {1.05f, .95f, .85f});
  shell(9, "Beak", Shape::box, kOrange, {.087f, 0, .228f}, {.038f, .034f, .011f});
  shell(9, "Eye L", Shape::sphere, kEye, {.078f, .022f, .25f}, {}, .0065f);
  shell(9, "Eye R", Shape::sphere, kEye, {.078f, -.022f, .25f}, {}, .0065f);
  return scaled(std::move(out));
}

// ---------------------------------------------------------------------------
// the policy

void micro_duck_policy(const f32 observation[61], f32 action[14]) {
  // rsl_rl's exported actor: (obs - mean) / divisor, then 512 / 256 / 128 ELU layers and a linear
  // output (ONNX Gemm with transB: y = W x + b, W stored [out][in]).
  static_assert(kVelstandDims[0] == 61 && kVelstandDims[4] == 14, "velstand: 61 observations, 14 actions");
  f32 a[512], b[512];
  for (int i = 0; i < 61; ++i) a[i] = (observation[i] - kVelstandMean[i]) / kVelstandDiv[i];
  const f32* const weights[] = {kVelstandW0, kVelstandW1, kVelstandW2, kVelstandW3};
  const f32* const biases[] = {kVelstandB0, kVelstandB1, kVelstandB2, kVelstandB3};
  f32* in = a;
  f32* out = b;
  for (int layer = 0; layer < 4; ++layer) {
    const int n_in = kVelstandDims[layer], n_out = kVelstandDims[layer + 1];
    const f32* w = weights[layer];
    for (int o = 0; o < n_out; ++o) {
      f32 sum = biases[layer][o];
      const f32* row = w + static_cast<std::size_t>(o) * static_cast<std::size_t>(n_in);
      for (int i = 0; i < n_in; ++i) sum += row[i] * in[i];
      out[o] = layer < 3 ? (sum > 0 ? sum : std::expm1(sum)) : sum;  // ELU (alpha 1)
    }
    std::swap(in, out);
  }
  for (int i = 0; i < 14; ++i) action[i] = in[i];
}

// ---------------------------------------------------------------------------
// walkers in the game

void GameWorld::create_walkers() {
  walkers_.clear();
  for (const Actor& a : level_.actors) {
    if (!a.controller || a.controller->mode != ControlMode::walker) continue;
    auto body = body_of_actor_.find(a.id);
    if (body == body_of_actor_.end()) continue;
    auto w = std::make_unique<WalkerBrain>();
    w->actor = a.id;
    w->trunk = body->second->body;
    i32 found = 0;
    for (auto& sj : joints_) {
      if (!sj->servo || sj->kind != JointKind::hinge) continue;
      const Actor* ja = level_.find(sj->actor);
      bool mine = false;  // a link of this walker: the walker is one of its ancestors
      for (const Actor* p = ja; p && !mine; p = level_.find(p->parent)) mine = p->id == a.id;
      if (!mine) continue;
      const i32 link = link_of_joint(ja->name);
      if (link > 0 && !w->joint[link - 1]) { w->joint[link - 1] = sj.get(); ++found; }
    }
    if (found != kJoints) {
      events_.push_back("Walker \"" + a.name + "\" needs the Micro Duck's 14 servo joints - it stands still");
      continue;
    }
    const f32 hz = clamp(level_.world.step_hz, 15.0f, 480.0f), slow = std::sqrt(kDuckScale);
    w->decimation = max_of(1, static_cast<i32>(std::lround(kControlPeriod * slow * hz)));
    w->delay = max_of(0, static_cast<i32>(std::lround(kActuatorDelay * slow * hz)));
    if (std::fabs(static_cast<f32>(w->decimation) / hz - kControlPeriod * slow) > 1e-4f)
      events_.push_back("Walker \"" + a.name + "\": its policy runs at 50 Hz on a " + std::to_string(static_cast<int>(kWalkerStepHz)) +
                        " Hz physics step - set World Settings > Step Rate to " + std::to_string(static_cast<int>(kWalkerStepHz)));
    for (i32 j = 0; j < kJoints; ++j) w->target[j] = kDuckLinks[j + 1].home;
    w->pending.assign(static_cast<std::size_t>(w->delay + 1), std::vector<f32>(w->target, w->target + kJoints));
    walkers_.push_back(std::move(w));
  }
}

void GameWorld::drive_walker(const PlayInput& in, f32 dt) {
  (void)dt;
  // The player's input reaches the walker's controller (drive_walkers runs every walker).
  for (auto& w : walkers_)
    if (w->actor == pw_->actor) { w->input = in; w->has_input = true; }
  if (in.jump && !pw_->jump_latch) {
    const Actor* a = level_.find(pw_->actor);
    auto it = body_of_actor_.find(pw_->actor);
    if (a && a->controller && it != body_of_actor_.end()) {
      cue(a->controller->jump_sound, physics_.pose(it->second->body).position + Vec3{0, 0, .1f * kDuckScale}, .9f, 1, true, audio::Bus::sfx, pw_->actor);
      for (auto& w : walkers_)
        if (w->actor == pw_->actor) w->quack = .3f;  // a nod while it quacks (real seconds)
    }
  }
  pw_->jump_latch = in.jump;
  if (auto it = body_of_actor_.find(pw_->actor); it != body_of_actor_.end()) {
    const Vec3 v = physics_.linear_velocity(it->second->body);
    hud_.speed = std::hypot(v.x, v.y);
  }
}

void GameWorld::drive_walkers(f32 h) {
  (void)h;
  const f32 slow = std::sqrt(kDuckScale);  // simulated time runs this much slower than real time
  for (auto& wp : walkers_) {
    WalkerBrain& w = *wp;
    if (w.step % w.decimation == 0) {
      // ---- one control tick (50 Hz real): observation -> policy -> joint targets ----------------
      const PlayInput in = w.has_input ? w.input : PlayInput{};
      const f32 cmd[3] = {clamp(in.move_y, -1.0f, 1.0f) * kMaxForward, 0, -clamp(in.move_x, -1.0f, 1.0f) * kMaxTurn};
      for (int i = 0; i < 3; ++i) w.command[i] += kCommandAlpha * (cmd[i] - w.command[i]);
      const f32 head[4] = {0, w.quack > 0 ? .35f : 0.0f, 0, 0};  // neck_pitch, head_pitch, head_yaw, head_roll
      for (int i = 0; i < 4; ++i) w.head[i] += kCommandAlpha * (head[i] - w.head[i]);
      w.quack = max_of(0.0f, w.quack - kControlPeriod);

      const physics::Pose tp = physics_.pose(w.trunk);
      const Quat qi{-tp.rotation.x, -tp.rotation.y, -tp.rotation.z, tp.rotation.w};
      f32 obs[61] = {};
      const Vec3 gyro = qi.rotate(physics_.angular_velocity(w.trunk)) * slow;  // trunk frame, real rad/s
      const Vec3 gravity = qi.rotate({0, 0, -1});
      obs[0] = gyro.x; obs[1] = gyro.y; obs[2] = gyro.z;
      obs[3] = gravity.x; obs[4] = gravity.y; obs[5] = gravity.z;
      f32 speed[kJoints];
      for (i32 j = 0; j < kJoints; ++j) {
        obs[6 + j] = physics_.joint_angle(w.joint[j]->joint) - kDuckLinks[j + 1].home;
        obs[20 + j] = w.joint_speed[j];  // one control tick old, as in training
        speed[j] = physics_.joint_speed(w.joint[j]->joint) * slow;
        obs[34 + j] = w.action[j];
      }
      std::memcpy(w.joint_speed, speed, sizeof speed);
      for (int i = 0; i < 3; ++i) obs[48 + i] = w.command[i];
      for (int i = 0; i < 4; ++i) obs[51 + i] = w.head[i];
      // body command (x, y, z, roll, pitch, yaw) stays 0: the stand pose
      micro_duck_policy(obs, w.action);
      for (i32 j = 0; j < kJoints; ++j) {
        const f32 a = std::isfinite(w.action[j]) ? w.action[j] : 0.0f;
        w.action[j] = a;
        w.target[j] = kDuckLinks[j + 1].home + a;  // action scale 1, no filter (training)
      }
    }
    // ---- every physics step: the servos get the command sent `delay` steps ago ------------------
    w.pending.erase(w.pending.begin());
    w.pending.emplace_back(w.target, w.target + kJoints);
    for (i32 j = 0; j < kJoints; ++j) w.joint[j]->servo_target = w.pending.front()[static_cast<std::size_t>(j)];
    ++w.step;
  }
}

void GameWorld::drive_servos(f32 h) {
  (void)h;
  // A position servo from Box3D's own joint parts (docs/physics.md section 5):
  // * torque K x (target - angle) - D x speed: the revolute joint's spring, solved implicitly by
  //   Box3D's soft step, so it is stable at any stiffness. Box3D sets springs in Hertz relative to
  //   the joint's effective mass; set_joint_spring converts the servo's N*m/rad once, at the boundary.
  // * the stall torque: the error is limited so that K x error never exceeds it (a Dynamixel's
  //   duty cycle saturates the same way).
  // * friction: a motor at speed 0 whose maximum torque is the friction (Erin's joint friction).
  for (auto& sj : joints_) {
    if (!sj->servo || sj->broken || !sj->joint.valid()) continue;
    const f32 q = physics_.joint_angle(sj->joint);
    const f32 k = max_of(1e-6f, sj->servo_stiffness);
    const f32 reach = sj->servo_torque > 0 ? sj->servo_torque / k : 1e9f;
    physics_.set_joint_spring(sj->joint, q + clamp(sj->servo_target - q, -reach, reach), k, max_of(0.0f, sj->servo_damping));
    physics_.set_joint_motor(sj->joint, sj->servo_friction > 0, 0, max_of(0.0f, sj->servo_friction));
  }
}

}  // namespace kern::game
