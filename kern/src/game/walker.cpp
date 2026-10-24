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
#include "kern/core/dmath.h"
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "kern/game/runtime.h"
#include "kern/game/servo.h"
#include "kern/game/walker.h"
#include "kern/game/walker_policy.h"
#include "kern/core/json.h"
#include "kern/audio/duck_voice.h"
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
struct DuckHullDef {
  i32 link;
  f32 friction;          // soles 1, housings 0 (training), the beak's pads 1
  bool sole;
  i32 first, count;      // points in kDuckHullPoints (x, y, z), relative to the link's pos (world axes)
};

struct DuckRollerDef {
  const char* body;      // MJCF body (the ankle with its blade) or the wheel's joint
  const char* joint;     // "" for an ankle replacement, else the free wheel's joint
  i32 ankle;             // the ankle link (kDuckLinks index) it replaces / hangs off
  i32 kind;              // -1 ankle replacement, 0 wheel
  Vec3 pos;              // wheel: its joint relative to the ankle's pos (world axes, home pose)
  Vec3 axis;             // wheel: its axle
  f32 mass;
  Vec3 com;              // relative to its pos (world axes)
  f32 inertia[6];
  i32 first, count;      // wheel: its tyre's hull in kDuckRollerHullPoints
};

struct DuckSiteDef {
  const char* name;
  i32 link;
  Vec3 pos;              // relative to the link's pos (world axes, home pose)
  Vec3 x, y, z;          // the site frame's axes (x = where the camera / ToF looks, z = up)
};

#include "micro_duck_model.inc"
#include "micro_duck_velstand.inc"

constexpr i32 kLinkCount = static_cast<i32>(sizeof kDuckLinks / sizeof kDuckLinks[0]);
constexpr i32 kJoints = 14;
constexpr i32 kMouthLink = kJoints + 1;  // the beak's lower half on the mouth servo (not in any policy)
static_assert(kLinkCount == kJoints + 2, "trunk, the policy's 14 joints, the mouth");

// Box3D is tuned for objects of 0.1 .. 10 m (docs/physics.md section 1): the duck's 2-4 cm links
// are below that, so Kern builds it at 4x with one conversion factor at the boundary. Froude
// similarity keeps the motion the same: lengths x S, masses x S^3, inertias x S^5, torques and
// servo stiffness x S^4, damping x S^4.5, time x sqrt(S) - and the policy sees real units.
constexpr f32 kDuckScale = kWalkerScale;

// The XL330 servo as trained (BAM "m6" model at the nominal 7.4 V; docs/walker.md section 4):
// stiffness K = V * kt/R * 0.5755 /rad, damping = back-EMF kt^2/R + viscous friction, Coulomb
// friction, and the torque at full duty V * kt/R (training runs without the firmware's current
// limit - microduck_rl scripts/infer_policy.py; with the 1.75 A limit it cannot get up from its back).
constexpr f32 kServoStiffness = 0.554f;   // N*m/rad
constexpr f32 kServoDamping = 0.053f;     // N*m*s/rad
constexpr f32 kServoFriction = 0.0048f;   // N*m
constexpr f32 kServoTorque = 0.963f;     // N*m
constexpr f32 kArmature = 0.0018f;       // kg*m^2: rotor inertia through the gearbox (MJCF chosen_actuator)
constexpr f32 kDuckImpactThreshold = 0.12f;  // m/s: from here a touch sounds (plastic tap, footstep)
constexpr f32 kWheelArmature = 0.0001f;  // kg*m^2: a roller wheel's bearing (MJCF passive_joint)

// Timing in real (unscaled) seconds; Kern's simulation runs sqrt(S) slower.
constexpr f32 kControlPeriod = 0.02f;     // the policy runs at 50 Hz
constexpr f32 kActuatorDelay = 0.02f;     // command -> servo latency (training: 15-30 ms)
// Commands (training ranges) and their smoothing on the robot: EMA 0.2 per control tick.
constexpr f32 kMaxForward = 0.4f, kMaxStrafe = 0.3f, kMaxTurn = 1.0f, kCommandAlpha = 0.2f;
constexpr f32 kHeadRange[4] = {1.10f, 1.10f, 1.40f, 0.31f};  // head command limits (training)

// Head sensors (microduck: kinematics/src/tof.rs, tof/, mediad/src/camera.rs).
constexpr int kTofSide = 8;                 // 8 x 8 zones
constexpr f32 kTofFov = 45 * rt::kDegree;   // per axis
constexpr f32 kTofMaxRange = 4.0f;          // VL53L8CX, metres (real)
constexpr f32 kTofRate = 15;                // frames per second (real)
constexpr f32 kTofFloorSafety = 0.85f;      // Reprojector::FLOOR_SAFETY
constexpr f32 kTofMinRange = 0.10f;         // Reprojector::MIN_RANGE_M (horizontal)
// IMX219 behind its M12 lens: 62 degrees across the 16:9 frame's long axis. The camera is mounted a
// quarter turn off (mediad rotate = 90), so the long axis is vertical in the world: upright, the
// picture is 9:16, 62 degrees high and 2 atan(tan 31 x 9/16) = 37.4 degrees wide (the calibrated
// fy gives the same 37.4).
constexpr f32 kCameraLong = 62 * rt::kDegree;
constexpr f32 kCameraAspect = 9.0f / 16.0f;  // width / height, upright
constexpr f32 kFallGravityZ = -0.5f, kFallDebounce = 0.2f;  // robotd's fall verdict (real seconds)
// The mouth (duck-control model.rs): closed -5 deg (held shut against the pads), open 30 deg.
constexpr f32 kMouthClosed = -5 * rt::kDegree, kMouthOpen = 30 * rt::kDegree;
constexpr f32 kHoldingAngle = 2 * rt::kDegree;  // Kern: shut on nothing the beak rests at its pads (0.0 deg); a sock holds it 3.6-7.8
f32 mouth_target(f32 open) { return kMouthClosed + clamp(std::isfinite(open) ? open : 0.0f, 0.0f, 1.0f) * (kMouthOpen - kMouthClosed); }

const DuckSiteDef* site(const char* name) {
  for (const DuckSiteDef& s : kDuckSites)
    if (std::strcmp(s.name, name) == 0) return &s;
  return nullptr;
}


i32 link_of_joint(const std::string& name) {
  for (i32 i = 1; i < kLinkCount; ++i)
    if (name == kDuckLinks[i].joint) return i;
  return -1;
}

/// One conversion factor at the Box3D boundary (see kDuckScale).
std::vector<Actor> scaled(std::vector<Actor> v) {
  const f32 s = kDuckScale, s3 = s * s * s, s4 = s3 * s, s5 = s4 * s, rs = std::sqrt(s);
  for (Actor& a : v) {
    a.transform.position = a.transform.position * s;
    if (a.mesh) { a.mesh->size = a.mesh->size * s; a.mesh->radius *= s; a.mesh->height *= s; }
    for (ColliderComponent& c : a.colliders) {
      c.center = c.center * s; c.size = c.size * s; c.radius *= s; c.length *= s;
      for (Vec3& p : c.points) p = p * s;
    }
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

/// The servo's rotor (armature): MuJoCo adds it to the joint's diagonal; here it is a thin disc on
/// the joint axis that the link carries: `armature` about the axis, half of it across (a real rigid
/// body: I = a/2 (E + axis axis^T); an axial-only tensor is not one, and Box3D gains energy with it).
void add_armature(f32 inertia[6], Vec3 ax, f32 armature) {
  if (armature <= 0) return;
  const f32 add[6] = {1 + ax.x * ax.x, 1 + ax.y * ax.y, 1 + ax.z * ax.z, ax.x * ax.y, ax.x * ax.z, ax.y * ax.z};
  for (int k = 0; k < 6; ++k) inertia[k] += 0.5f * armature * add[k];
}

}  // namespace

ActorId add_micro_duck(Level& l, Vec3 at, bool rollers) {
  std::vector<Actor> parts = micro_duck_actors(rollers);
  const ActorId base = l.next_id;
  for (std::size_t i = 0; i < parts.size(); ++i) {
    Actor a = parts[i];
    a.id = base + static_cast<ActorId>(i);
    if (i == 0) a.transform.position = a.transform.position + at;
    if (a.parent) a.parent = base + a.parent - 1;
    for (JointComponent& j : a.joints)
      if (j.target) j.target = base + j.target - 1;
    l.add(std::move(a));
  }
  l.world.step_hz = kWalkerStepHz;
  l.world.substeps = kWalkerSubsteps;
  return base;
}

void vary_micro_duck(Level& level, ActorId trunk, const DuckVariation& v) {
  const f32 S = kDuckScale;
  if (Actor* a = level.find(trunk); a && a->body) {
    a->body->mass *= v.trunk_mass;
    for (f32& k : a->body->inertia) k *= v.trunk_mass;
    a->body->center_of_mass = a->body->center_of_mass + v.trunk_com * S;
  }
  if (Actor* a = level.find(trunk + 9); a && a->body)  // the head (head_roll)
    a->body->center_of_mass = a->body->center_of_mass + v.head_com * S;
}

ServoProfile xl330_profile() {
  ServoProfile p;
  p.name = "xl330";
  p.source = "Pollen Robotics microduck_rl: BAM m6 model of the XL330-M288 at 7.4 V, MJCF armature";
  p.stiffness = kServoStiffness;
  p.damping = kServoDamping;
  p.friction = kServoFriction;
  p.torque = kServoTorque;
  p.armature = kArmature;
  p.delay = kActuatorDelay;
  // What training draws around it (microduck_rl's randomisation: rotor inertia and joint friction
  // +-10 %, the command's latency 15-30 ms).
  p.spread_armature = 0.1f;
  p.spread_friction = 0.1f;
  p.delay_min = 0.015f;
  p.delay_max = 0.030f;
  return p;
}

void set_micro_duck_servo(Level& level, ActorId trunk, i32 link, const ServoProfile& p) {
  const Actor* t = level.find(trunk);
  Actor* a = level.find(trunk + static_cast<ActorId>(link));
  if (!t || !t->controller || !a || !a->body || link < 1 || link >= kLinkCount) return;
  // Exactly as micro_duck_actors and scaled() build it (the nominal profile gives the same bits).
  const f32 s = kDuckScale, s3 = s * s * s, s4 = s3 * s, s5 = s4 * s, rs = std::sqrt(s);
  (void)s3;
  const f32* base = kDuckLinks[link].inertia;
  if (t->controller->rollers)
    for (const DuckRollerDef& r : kDuckRollers)
      if (r.kind < 0 && r.ankle == link) base = r.inertia;
  f32 inertia[6];
  std::memcpy(inertia, base, sizeof inertia);
  add_armature(inertia, kDuckLinks[link].axis, p.armature);
  for (int k = 0; k < 6; ++k) a->body->inertia[k] = inertia[k] * s5;
  for (JointComponent& j : a->joints) {
    if (!j.servo) continue;
    j.servo_stiffness = p.stiffness * s4;
    j.servo_damping = p.damping * (s4 * rs);
    j.servo_friction = p.friction * s4;
    j.max_motor_force = p.torque * s4;
  }
}

void set_micro_duck_servos(Level& level, ActorId trunk, const ServoProfile& p) {
  for (i32 link = 1; link < kLinkCount; ++link) set_micro_duck_servo(level, trunk, link, p);
  if (Actor* t = level.find(trunk); t && t->controller) t->controller->servo_delay = p.delay;
}

std::vector<Actor> micro_duck_sock_actors() {
  // A crumpled sock, real size x kWalkerScale: seven 12.5 mm lumps of fabric in a loose heap 3.4 cm
  // high (four on the floor, two on them, one on top), each lump tied to its neighbours by a soft
  // spring (Box3D distance joints): it gives under the beak, and what the beak pinches pulls the rest
  // along. 25 g in all (x S^3 in the world).
  const f32 S = kWalkerScale, r = 0.00625f * S, d = 0.0065f * S, h = 0.0107f * S;
  const Vec3 lumps[] = {{-d, -d, r}, {d, -d, r}, {-d, d, r}, {d, d, r}, {0, -d, r + h}, {0, d, r + h}, {0, 0, r + 2 * h}};
  std::vector<Actor> out;
  for (int k = 0; k < 7; ++k) {
    Actor a;
    a.name = k == 0 ? "Sock" : "Sock Lump";
    a.tags.push_back("sock");
    a.parent = k == 0 ? 0 : 1;
    a.transform.position = k == 0 ? lumps[0] : lumps[k] - lumps[0];
    a.mesh = MeshComponent{};
    a.mesh->shape = Shape::sphere;
    a.mesh->radius = r;
    a.mesh->collision = false;
    a.mesh->material.color = {0.93f, 0.36f, 0.45f};
    a.mesh->material.roughness = 0.95f;
    a.body = BodyComponent{};
    a.body->mass = 0.025f * S * S * S / 7;
    ColliderComponent c;
    c.shape = ColliderShape::sphere;
    c.radius = r;
    c.friction = 1;
    c.surface = "cloth";  // cotton knit: on the beak's TPU, on itself and on the floor its own measured friction (physics::surface_friction)
    c.rolling_resistance = 0.5f;  // a lump of cloth does not roll (round lumps would, and the heap crawls)
    a.colliders.push_back(c);
    for (int m = 0; m < k; ++m) {
      if (length(lumps[k] - lumps[m]) > 0.025f * S) continue;
      JointComponent j;
      j.kind = JointKind::distance;
      j.target = static_cast<ActorId>(m + 1);
      j.collide_connected = true;  // the lumps touch, they do not pass through each other
      j.enable_spring = true;
      j.spring_hertz = 15;
      j.spring_damping = 0.7f;
      a.joints.push_back(j);
    }
    out.push_back(std::move(a));
  }
  return out;
}

std::vector<Actor> micro_duck_actors(bool rollers) {
  std::vector<Actor> out;  // parent / joint target = index + 1 (the catalog's convention)
  const Vec3 root = kDuckLinks[0].pos;
  auto roller_ankle = [&](i32 link) -> const DuckRollerDef* {
    if (!rollers) return nullptr;
    for (const DuckRollerDef& r : kDuckRollers)
      if (r.kind < 0 && r.ankle == link) return &r;
    return nullptr;
  };
  for (i32 i = 0; i < kLinkCount; ++i) {
    const DuckLinkDef& l = kDuckLinks[i];
    const DuckRollerDef* blade = roller_ankle(i);  // on rollers the ankle carries the blade
    Actor a;
    a.name = i == 0 ? "Micro Duck" : l.joint;  // links are named after their servo joint
    a.transform.position = i == 0 ? l.pos + Vec3{0, 0, rollers ? kDuckRollerRaise : 0.0f} : l.pos - root;
    a.parent = i == 0 ? 0 : 1;  // the links are children of the trunk in the outliner
    a.body = BodyComponent{};
    a.body->mass = blade ? blade->mass : l.mass;
    a.body->center_of_mass = blade ? blade->com : l.com;
    std::memcpy(a.body->inertia, blade ? blade->inertia : l.inertia, sizeof l.inertia);
    if (i > 0) add_armature(a.body->inertia, l.axis, kArmature);
    a.body->allow_sleep = false;
    a.body->angular_damping = 0;  // MuJoCo bodies have none; the servos damp
    // Its plastic taps when shells and housings touch the floor or each other; the soles step
    // (their colliders below). Soft touches count: the robot is small and light.
    a.body->impact_sound = "builtin:plastic";
    a.body->impact_threshold = kDuckImpactThreshold;
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
      c.jump_sound = "";  // its voice is the duck's own synth (robotd sound.rs), not a sample
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
      j.servo_target = i == kMouthLink ? kMouthClosed / rt::kDegree : j.reference;
      j.servo_stiffness = kServoStiffness;
      j.servo_damping = kServoDamping;
      j.servo_friction = kServoFriction;
      j.max_motor_force = kServoTorque;
      a.joints.push_back(j);
    }
    // The link as it looks: Pollen's meshes of its parts, merged, in the link's frame (builtin
    // mesh "duck/<MJCF body>", vertex colours from the MJCF materials).
    a.mesh = MeshComponent{};
    a.mesh->shape = Shape::mesh;
    a.mesh->asset = blade ? std::string("duck/roller/") + blade->body : std::string("duck/") + l.body;
    a.mesh->size = {1, 1, 1};
    a.mesh->collision = false;
    a.mesh->material.color = {1, 1, 1};
    a.mesh->material.roughness = .5f;
    // What collides: the soles (friction 1) and the XL330 housings (frictionless) as in training,
    // the beak's soft pads (what grips), and the trunk's and head's shells (they enclose the
    // servos: what a fallen duck lies and slides on).
    for (const DuckHullDef& h : kDuckHulls) {
      if (h.link != i || (blade && h.sole)) continue;  // the blade has no sole: its wheels touch
      ColliderComponent c;
      c.shape = ColliderShape::hull;  // MuJoCo collides a mesh geom as its convex hull
      for (i32 k = 0; k < h.count; ++k) {
        const f32* p = kDuckHullPoints + 3 * (h.first + k);
        c.points.push_back({p[0], p[1], p[2]});
      }
      c.friction = h.friction;
      if (h.sole) c.impact_sound = "builtin:plastic_step";
      else if (h.friction > 0) c.surface = "tpu";  // the beak's printed pads: on the sock, TPU on cotton
      a.colliders.push_back(c);
    }
    for (const char* shell : kDuckShellBodies)
      if (std::strcmp(shell, l.body) == 0) {
        ColliderComponent c;
        c.shape = ColliderShape::hull;  // the convex hull of the link's mesh
        c.friction = kDuckShellFriction;
        a.colliders.push_back(c);
        // The shell encloses its neighbours' servos and brackets: it never touches its own robot.
        for (i32 k = 0; k < kLinkCount; ++k) {
          if (k == i || kDuckLinks[k].parent == i || kDuckLinks[i].parent == k) continue;
          JointComponent nc;
          nc.kind = JointKind::no_collision;
          nc.target = static_cast<ActorId>(k + 1);
          a.joints.push_back(nc);
        }
      }
    out.push_back(std::move(a));
  }
  if (rollers) {
    // The four free wheels (rollers model): a hinge with nothing driving it (bearing armature
    // 0.0001 kg m^2), the tyre's hull touching the floor (MuJoCo default friction 1).
    for (const DuckRollerDef& r : kDuckRollers) {
      if (r.kind != 0) continue;
      Actor wa;
      wa.name = r.joint;
      wa.transform.position = kDuckLinks[r.ankle].pos + r.pos - root;
      wa.parent = 1;
      wa.body = BodyComponent{};
      wa.body->mass = r.mass;
      wa.body->center_of_mass = r.com;
      std::memcpy(wa.body->inertia, r.inertia, sizeof r.inertia);
      const Vec3 ax = r.axis;
      const f32 add[6] = {1 + ax.x * ax.x, 1 + ax.y * ax.y, 1 + ax.z * ax.z, ax.x * ax.y, ax.x * ax.z, ax.y * ax.z};
      for (int k = 0; k < 6; ++k) wa.body->inertia[k] += 0.5f * kWheelArmature * add[k];
      wa.body->allow_sleep = false;
      wa.body->angular_damping = 0;
      wa.body->impact_sound = "";
      wa.mesh = MeshComponent{};
      wa.mesh->shape = Shape::mesh;
      wa.mesh->asset = std::string("duck/roller/") + r.joint;
      wa.mesh->collision = false;
      wa.mesh->material.roughness = .6f;
      JointComponent j;
      j.kind = JointKind::hinge;
      j.target = static_cast<ActorId>(r.ankle + 1);
      j.axis = r.axis;
      wa.joints.push_back(j);
      ColliderComponent c;
      c.shape = ColliderShape::hull;
      for (i32 k = 0; k < r.count; ++k) {
        const f32* p = kDuckRollerHullPoints + 3 * (r.first + k);
        c.points.push_back({p[0], p[1], p[2]});
      }
      c.friction = 1.0f;
      wa.colliders.push_back(c);
      out.push_back(std::move(wa));
    }
    out[0].controller->rollers = true;
    out[0].name = "Micro Duck (Rollers)";
  }
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
      out[o] = layer < 3 ? (sum > 0 ? sum : kern::dm::expm1(sum)) : sum;  // ELU (alpha 1)
    }
    std::swap(in, out);
  }
  for (int i = 0; i < 14; ++i) action[i] = in[i];
}

WalkerPolicy builtin_velstand_policy() {
  WalkerPolicy p;
  p.file = "velstand.onnx";
  p.dims.assign(std::begin(kVelstandDims), std::end(kVelstandDims));
  p.mean.assign(std::begin(kVelstandMean), std::end(kVelstandMean));
  p.div.assign(std::begin(kVelstandDiv), std::end(kVelstandDiv));
  p.weights = {{std::begin(kVelstandW0), std::end(kVelstandW0)}, {std::begin(kVelstandW1), std::end(kVelstandW1)},
               {std::begin(kVelstandW2), std::end(kVelstandW2)}, {std::begin(kVelstandW3), std::end(kVelstandW3)}};
  p.biases = {{std::begin(kVelstandB0), std::end(kVelstandB0)}, {std::begin(kVelstandB1), std::end(kVelstandB1)},
              {std::begin(kVelstandB2), std::end(kVelstandB2)}, {std::begin(kVelstandB3), std::end(kVelstandB3)}};
  return p;
}

namespace {

// ---------------------------------------------------------------------------
// the robot's policy set and its daemon's controller (pollen-robotics/microduck robotd/src/control.rs,
// robotd-params): which network drives, with which command, at which action scale

// Action scale and target filter. robotd runs the real robot at 0.9 (0.8 on rollers) with a first-
// order low-pass on the targets (head 0.5, legs 0.7): sim-to-real tuning for the hardware. Kern
// simulates the training physics, so it runs each network as it was trained (mjlab
// JointPositionAction scale 1, no filter) and keeps the set's own action_scale (roller, crouch:
// 0.8). Measured: the kicks fall over at 0.9 + low-pass and stand at 1.0 without it.
constexpr f32 kActionScaleWalk = 1.0f, kActionScaleRoller = 0.8f;
constexpr f32 kStandingActionScale = 1.0f;
constexpr f32 kHeadLowpass = 1.0f, kLegsLowpass = 1.0f;            // 1 = pass-through
// The robot's daemon (robotd-params: action_scale 0.9 walking, head_lowpass 0.5, legs_lowpass 0.7),
// selectable per walker (ControllerComponent::actions = "robot").
constexpr f32 kRobotActionScaleWalk = 0.9f, kRobotHeadLowpass = 0.5f, kRobotLegsLowpass = 0.7f;
constexpr f32 kStandingThreshold = 0.05f;                          // DEFAULT_STANDING_THRESHOLD (twist)
constexpr f32 kChainWindow = 0.15f;                                // CHAIN_WINDOW
constexpr f32 kActuatorTravel = 3.14159265f;                       // safety.rs ACTUATOR_MIN / MAX (rad)
// Kern: robotd leaves the beak to the operator's trigger during a ground pick. The controller opens
// it near the bottom (opened on the way down, the jaw lands on what it should take) and bites while
// the beak is lowest (docs/walker.md section 12).
constexpr f32 kPickOpen = 0.16f;
constexpr f32 kPickClose = 0.24f;

struct SkillDef {
  std::string name, file;
  f32 duration = 0;
  bool chain = false;
  f32 command[3] = {0, 0, 0}, unwind[3] = {0, 0, 0};
  f32 unwind_s = 0;
  f32 action_scale = -1;  // < 0: the gait's
  i32 phase_slot = -1;    // >= 0: the network sees the move's phase as cos / sin in this slot and the next
  f32 period = 0;         // seconds per phase cycle
};
struct PhaseDef {
  std::string file;
  f32 period = 4, end_phase = 0.7f, action_scale = 1;
};
struct PolicySet {
  std::vector<SkillDef> skills;   // episodic, constant command, in the manifest's order (priority)
  PhaseDef pick, crouch;          // phase-encoded: the ground pick, and the crouch on rollers
  std::string sitstand = "alpha_sitstand.onnx";
  f32 ramp_s = 2, rise_s = 1;     // DEFAULT_SITSTAND_RAMP_S / RISE_S, or the set's
};

const PolicySet& policy_set() {
  static const PolicySet set = [] {
    PolicySet out;
    out.pick = {"alpha_ground_pick.onnx", 4, 0.7f, 1.0f};
    out.crouch = {"roller_crouch.onnx", 3, 0.7f, 0.8f};
    json::Value m;
    if (!json::parse(micro_duck_manifest(), m)) return out;
    for (const json::Value& p : m["policies"].items()) {
      const std::string file = p["file"].as_string(), kind = p["kind"].as_string(), mode = p["mode"].as_string();
      const json::Value& cmd = p["command"];
      std::string name = p["name"].is_string() ? p["name"].as_string() : file.substr(0, file.rfind('.'));
      if (kind == "scripted" && cmd["encoding"].as_string() == "posture_flag") {
        out.sitstand = file;
        if (p["ramp_s"].is_number()) out.ramp_s = static_cast<f32>(p["ramp_s"].as_number());
        if (p["unwind_s"].is_number()) out.rise_s = static_cast<f32>(p["unwind_s"].as_number());
      } else if (kind == "episodic" && cmd["encoding"].as_string() == "phase") {
        PhaseDef d{file, static_cast<f32>(cmd["period_s"].as_number(4)), static_cast<f32>(cmd["end_phase"].as_number(0.7)),
                   static_cast<f32>(p["action_scale"].as_number(mode == "roller" ? 0.8 : 1.0))};
        (mode == "roller" ? out.crouch : out.pick) = d;
      } else if (kind == "episodic" && p["duration_s"].is_number()) {
        SkillDef d;
        d.name = name;
        d.file = file;
        d.duration = static_cast<f32>(p["duration_s"].as_number());
        d.chain = p["chain"].as_bool();
        if (p["action_scale"].is_number()) d.action_scale = static_cast<f32>(p["action_scale"].as_number());
        // A constant command (command.value) and the unwind after it (unwind_s with command.idle).
        const json::Value& value = cmd["value"], &idle = cmd["idle"];
        for (int i = 0; i < 3; ++i) {
          if (value.is_array() && value.items().size() > static_cast<std::size_t>(i)) d.command[i] = static_cast<f32>(value.items()[i].as_number());
          if (idle.is_array() && idle.items().size() > static_cast<std::size_t>(i)) d.unwind[i] = static_cast<f32>(idle.items()[i].as_number());
        }
        if (p["unwind_s"].is_number()) d.unwind_s = static_cast<f32>(p["unwind_s"].as_number());
        out.skills.push_back(std::move(d));
      }
    }
    // Kern's own moves, trained in Kern (docs/lehrer.md; Kern Lehrer saves them for the Studio):
    // the jump runs like Pollen's kicks - a hard swap to its network, then back after the episode.
    SkillDef jump;
    jump.name = "jump";
    jump.file = "jump.onnx";
    jump.duration = kJumpSeconds;
    jump.action_scale = 1.0f;
    jump.phase_slot = kJumpPhaseSlot;
    jump.period = kJumpSeconds;
    out.skills.push_back(std::move(jump));
    return out;
  }();
  return set;
}

i32 skill_index(std::string_view name) {
  const PolicySet& set = policy_set();
  for (std::size_t i = 0; i < set.skills.size(); ++i)
    if (set.skills[i].name == name) return static_cast<i32>(i);
  return -1;
}

template <class W>
const char* move_blocked(const W& w) {
  using Sit = decltype(w.sit);
  if (w.pick_phase >= 0) return "a ground pick is running";
  if (w.skill >= 0) return "a scripted move is already running";
  if (w.sit == Sit::sitting) return "the robot is sitting - stand it up first";
  if (w.sit == Sit::rising) return "the robot is standing up";
  return nullptr;
}

}  // namespace

std::string GameWorld::walker_skill(ActorId id, std::string_view name) {
  WalkerBrain* w = walker_brain(id);
  if (!w) return "no such robot";
  using Sit = WalkerBrain::Sit;
  const PolicySet& set = policy_set();
  if (name == "sit") {  // Controller::sit_toggle
    if (w->pick_phase >= 0) return "a ground pick is running";
    if (w->skill >= 0) return "a scripted move is already running";
    if (!walker_policy(set.sitstand)) return set.sitstand + " is not loaded";
    if (w->sit == Sit::up) { w->sit = Sit::sitting; w->sit_settle = set.ramp_s; return ""; }
    if (w->sit == Sit::sitting && w->sit_settle > 0) return "still sitting down";
    if (w->sit == Sit::sitting) { w->sit = Sit::rising; w->sit_remaining = set.rise_s; return ""; }
    return "already standing up";
  }
  if (name == "pick" || name == "ground_pick" || name == "crouch") {  // Controller::start_ground_pick
    const PhaseDef& d = w->rollers ? set.crouch : set.pick;
    if (!walker_policy(d.file)) return d.file + " is not loaded";
    if (const char* why = move_blocked(*w)) return why;
    w->pick_phase = 0;
    if (!w->rollers && w->kern_extras) w->mouth_open = 0;  // Kern: shut on the way down; opens at the bottom, then bites
    return "";
  }
  const i32 index = skill_index(name);  // Controller::start_skill
  if (index < 0) return "no such move";
  const SkillDef& d = set.skills[static_cast<std::size_t>(index)];
  if (!walker_policy(d.file)) return d.file + " is not loaded";
  if (w->skill == index && d.chain && !w->skill_unwinding) { w->skill_chain = kChainWindow; return ""; }
  if (const char* why = move_blocked(*w)) return why;
  w->skill = index;
  w->skill_unwinding = false;
  w->skill_remaining = d.duration;
  w->skill_chain = 0;
  return "";
}

bool GameWorld::walker_mouth(ActorId id, f32 open) {
  bool any = false;
  for (auto& w : walkers_)
    if (!id || w->actor == id) { w->mouth_open = clamp(open, 0.0f, 1.0f); any = true; }
  return any;
}

std::vector<GameWorld::WalkerJoint> GameWorld::walker_joints(ActorId id) const {
  std::vector<WalkerJoint> out;
  const WalkerBrain* w = walker_brain(id);
  if (!w) return out;
  for (i32 j = 0; j <= kJoints; ++j) {
    const SimJoint* sj = j < kJoints ? w->joint[j] : w->mouth;
    if (!sj) continue;
    const DuckLinkDef& l = kDuckLinks[j + 1];
    out.push_back({l.joint, physics_.joint_angle(sj->joint), l.lower, l.upper});
  }
  return out;
}

std::string GameWorld::walker_said(ActorId id) const {
  const WalkerBrain* w = walker_brain(id);
  return w ? w->said : std::string();
}

std::string GameWorld::walker_driving(ActorId id) const {
  const WalkerBrain* w = walker_brain(id);
  return w ? w->driving : std::string();
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
      if (link == kMouthLink) w->mouth = sj.get();
      else if (link > 0 && !w->joint[link - 1]) { w->joint[link - 1] = sj.get(); ++found; }
    }
    w->bodies[0] = w->trunk;
    for (i32 j = 0; j < kJoints; ++j) if (w->joint[j]) w->bodies[j + 1] = w->joint[j]->b;
    if (w->mouth) w->bodies[kMouthLink] = w->mouth->b;
    if (found != kJoints) {
      events_.push_back("Walker \"" + a.name + "\" needs the Micro Duck's 14 servo joints - it stands still");
      continue;
    }
    const f32 hz = clamp(level_.world.step_hz, 15.0f, 480.0f), slow = std::sqrt(kDuckScale);
    w->autonomous = a.controller->autonomous;
    // Its own voice: the robot seeds it with sha256 of its serial; a Kern duck with its name and id.
    w->voice_seed = audio::duck_seed_from_id(a.name + "#" + std::to_string(a.id));
    w->voice_rng = w->voice_seed | 1u;
    w->rollers = a.controller->rollers;
    w->alpha_gait = a.controller->gait == "alpha";
    w->robot_actions = a.controller->actions == "robot";
    w->kern_extras = a.controller->kern_extras;
    w->decimation = max_of(1, static_cast<i32>(std::lround(kControlPeriod * slow * hz)));
    w->delay = max_of(0, static_cast<i32>(std::lround(a.controller->servo_delay * slow * hz)));
    if (std::fabs(static_cast<f32>(w->decimation) / hz - kControlPeriod * slow) > 1e-4f)
      events_.push_back("Walker \"" + a.name + "\": its policy runs at 50 Hz on a " + std::to_string(static_cast<int>(kWalkerStepHz)) +
                        " Hz physics step - set World Settings > Step Rate to " + std::to_string(static_cast<int>(kWalkerStepHz)));
    for (i32 j = 0; j < kJoints; ++j) w->target[j] = kDuckLinks[j + 1].home;
    w->pending.assign(static_cast<std::size_t>(w->delay + 1), std::vector<f32>(w->target, w->target + kJoints));
    walkers_.push_back(std::move(w));
  }
}

bool GameWorld::set_walker_external(ActorId id, bool on) {
  WalkerBrain* w = walker_brain(id);
  if (!w) return false;
  w->external = on;
  return true;
}

bool GameWorld::set_walker_action(ActorId id, const f32 action[14]) {
  WalkerBrain* w = walker_brain(id);
  if (!w) return false;
  for (i32 j = 0; j < kJoints; ++j) w->external_action[j] = std::isfinite(action[j]) ? action[j] : 0.0f;
  return true;
}

bool GameWorld::walker_tick_due(ActorId id) const {
  const WalkerBrain* w = walker_brain(id);
  return w && w->step % static_cast<u64>(w->decimation) == 0;
}

bool GameWorld::walker_last_observation(ActorId id, f32 observation[61]) const {
  const WalkerBrain* w = walker_brain(id);
  if (!w) return false;
  std::memcpy(observation, w->last_obs, sizeof w->last_obs);
  return true;
}

bool GameWorld::walker_observation(ActorId id, f32 obs[61]) const {
  // The same as drive_walkers builds it (keep the two in step): IMU, joints, the joint speeds of the
  // last tick, the last action, then the command slots.
  const WalkerBrain* w = walker_brain(id);
  if (!w) return false;
  const f32 slow = std::sqrt(kDuckScale);
  const physics::Pose tp = physics_.pose(w->trunk);
  const Quat qi{-tp.rotation.x, -tp.rotation.y, -tp.rotation.z, tp.rotation.w};
  const Vec3 gyro = qi.rotate(physics_.angular_velocity(w->trunk)) * slow;
  const Vec3 gravity = qi.rotate({0, 0, -1});
  for (i32 i = 0; i < 61; ++i) obs[i] = 0;
  obs[0] = gyro.x; obs[1] = gyro.y; obs[2] = gyro.z;
  obs[3] = gravity.x; obs[4] = gravity.y; obs[5] = gravity.z;
  for (i32 j = 0; j < kJoints; ++j) {
    obs[6 + j] = physics_.joint_angle(w->joint[j]->joint) - kDuckLinks[j + 1].home;
    obs[20 + j] = w->joint_speed[j];
    obs[34 + j] = w->action[j];
  }
  for (int i = 0; i < 3; ++i) obs[48 + i] = w->command[i];
  for (int i = 0; i < 4; ++i) obs[51 + i] = w->head[i];
  obs[57] = w->body[0];
  obs[58] = w->body[1];
  obs[59] = w->body[2];
  return true;
}

bool GameWorld::walker_probe(ActorId id, WalkerProbe& out) const {
  const WalkerBrain* w = walker_brain(id);
  if (!w) return false;
  const f32 S = kDuckScale, slow = std::sqrt(S);
  const physics::Pose tp = physics_.pose(w->trunk);
  out.position = tp.position * (1 / S);
  out.rotation = tp.rotation;
  out.velocity = physics_.linear_velocity(w->trunk) * (slow / S);
  out.spin = physics_.angular_velocity(w->trunk) * slow;
  const Quat qi{-tp.rotation.x, -tp.rotation.y, -tp.rotation.z, tp.rotation.w};
  out.gravity = qi.rotate({0, 0, -1});
  auto own = [&](physics::Body b) {
    for (const physics::Body& o : w->bodies) if (o.valid() && o == b) return true;
    return false;
  };
  std::vector<std::pair<physics::Body, f32>> touch;
  const i32 feet[2] = {5, 14};  // the ankle links carry the soles (left, right)
  out.self_contact = out.body_contact = false;
  for (i32 i = 0; i < WalkerBrain::kBodies; ++i) {
    if (!w->bodies[i].valid()) continue;
    touch.clear();
    physics_.touching(w->bodies[i], touch);
    for (const auto& [other, impulse] : touch) {
      if (own(other)) out.self_contact = true;
      else if (i != feet[0] && i != feet[1]) out.body_contact = true;
    }
  }
  for (int f = 0; f < 2; ++f) {
    out.foot_contact[f] = false;
    const physics::Body b = w->bodies[feet[f]];
    if (!b.valid()) continue;
    touch.clear();
    physics_.touching(b, touch);
    for (const auto& [other, impulse] : touch) if (!own(other)) out.foot_contact[f] = true;
    out.foot_height[f] = physics_.pose(b).position.z / S;
  }
  for (i32 j = 0; j < kJoints; ++j) {
    out.joint[j] = physics_.joint_angle(w->joint[j]->joint) - kDuckLinks[j + 1].home;
    out.joint_speed[j] = physics_.joint_speed(w->joint[j]->joint) * slow;
    out.action[j] = w->action[j];
  }
  return true;
}

bool GameWorld::walker_push(ActorId id, Vec3 velocity) {
  WalkerBrain* w = walker_brain(id);
  if (!w) return false;
  const Vec3 dv = velocity * (kDuckScale / std::sqrt(kDuckScale));  // real m/s -> world m/s
  for (const physics::Body& b : w->bodies)
    if (b.valid()) physics_.set_linear_velocity(b, physics_.linear_velocity(b) + dv);
  return true;
}

GameWorld::WalkerBrain* GameWorld::walker_brain(ActorId id) const {
  for (const auto& w : walkers_)
    if (!id || w->actor == id) return w.get();
  return nullptr;
}

std::vector<ActorId> GameWorld::walkers() const {
  std::vector<ActorId> out;
  for (const auto& w : walkers_) out.push_back(w->actor);
  return out;
}

bool GameWorld::walker_sensors(ActorId id, WalkerSensors& out) const {
  const WalkerBrain* w = walker_brain(id);
  if (!w) return false;
  out = w->sensors;
  return true;
}

bool GameWorld::walker_camera(ActorId id, Camera& out) const {
  const WalkerBrain* w = walker_brain(id);
  const DuckSiteDef* cam = site("head_camera");
  if (!w || !cam || !w->joint[8]) return false;
  const physics::Pose head = physics_.pose(w->joint[8]->b);
  out = Camera{};
  out.eye = head.position + head.rotation.rotate(cam->pos * kDuckScale);
  out.target = out.eye + head.rotation.rotate(cam->x);
  out.up = head.rotation.rotate(cam->z);
  out.fov = kCameraLong;  // vertical: the frame's long axis stands upright (see kCameraAspect)
  out.near = 0.01f * kDuckScale;
  out.far = 400;
  return true;
}

bool GameWorld::walker_command(ActorId id, f32 forward, f32 strafe, f32 turn, f32 hold) {
  bool any = false;
  for (auto& w : walkers_)
    if (!id || w->actor == id) {
      w->drive_cmd[0] = forward;
      w->drive_cmd[1] = strafe;
      w->drive_cmd[2] = turn;
      w->drive_left = hold > 0 ? hold : -1;
      any = true;
    }
  return any;
}

bool GameWorld::walker_look(ActorId id, f32 yaw, f32 pitch, f32 hold, f32 neck) {
  bool any = false;
  for (auto& w : walkers_)
    if (!id || w->actor == id) {
      w->look_cmd[0] = yaw;
      w->look_cmd[1] = pitch;
      w->look_neck = neck;
      w->look_left = hold > 0 ? hold : -1;
      any = true;
    }
  return any;
}

bool GameWorld::walker_set_autonomous(ActorId id, bool on) {
  bool any = false;
  for (auto& w : walkers_)
    if (!id || w->actor == id) {
      w->autonomous = on;
      if (on) w->drive_left = w->look_left = 0;  // hand back to the brain
      any = true;
    }
  return any;
}

bool GameWorld::walker_quack(ActorId id) {
  bool any = false;
  for (auto& w : walkers_)
    if (!id || w->actor == id) {
      walker_say(*w, "chirp");
      w->beak_open = .25f;  // the beak opens with the call (robotd: the chirp trigger opens the mouth)
      any = true;
    }
  return any;
}

/// One call of the duck's voice (robotd/src/sound.rs): a random variant of the tag in this duck's
/// own voice; a new call cuts the one it was making; while the wheee ride has the speaker, one-shots
/// are dropped.
void GameWorld::walker_say(WalkerBrain& w, const char* tag) {
  if (w.wheee != WalkerBrain::Wheee::off) return;
  const u32 n = audio::duck_variant_count(tag);
  w.voice_rng ^= w.voice_rng << 13;
  w.voice_rng ^= w.voice_rng >> 17;
  w.voice_rng ^= w.voice_rng << 5;
  const u32 variant = n ? w.voice_rng % n : 0;
  voice_cue(w, std::string("builtin:duck_") + tag + "_" + std::to_string(w.voice_seed) + "_" + std::to_string(variant));
  w.said = tag;
}

void GameWorld::warm_walker_voices() {
  // The duck's calls are synthesised on first use otherwise - tens of milliseconds each on a phone,
  // long enough to starve the audio output (a click) and stall the frame. Most used first.
  static constexpr const char* kTags[] = {"chirp", "greet", "inquire", "peck", "coo", "alarm", "wheee"};
  for (auto& w : walkers_) {
    for (;;) {
      u32 index = w->warm_index, tag = 0;
      while (tag < std::size(kTags) && index >= audio::duck_variant_count(kTags[tag])) index -= audio::duck_variant_count(kTags[tag++]);
      if (tag >= std::size(kTags)) break;  // this duck's voice is complete
      ++w->warm_index;
      if (audio::duck_voice_warm(kTags[tag], w->voice_seed, index)) return;  // one synthesis per frame
    }
  }
}

void GameWorld::voice_cue(WalkerBrain& w, const std::string& sound, bool silence) {
  SoundCue c;
  c.sound = sound;
  c.position = physics_.pose(w.joint[8] ? w.joint[8]->b : w.trunk).position;  // the speaker is in the head
  c.volume = 1;
  c.spatial = true;
  c.bus = audio::Bus::voice;
  c.actor = w.actor;
  c.voice = !silence;
  c.silence = silence;
  if (cues_.size() >= 64) cues_.erase(cues_.begin());
  cues_.push_back(std::move(c));
}

/// The wheee ride (sound.rs Ride): start, the loop for as long as it is held, then the end.
void GameWorld::walker_wheee(WalkerBrain& w, bool held) {
  using Wheee = WalkerBrain::Wheee;
  const std::string base = "_" + std::to_string(w.voice_seed) + "_" + std::to_string(w.wheee_variant);
  if (held && w.wheee == Wheee::off) {
    w.voice_rng ^= w.voice_rng << 13;
    w.voice_rng ^= w.voice_rng >> 17;
    w.voice_rng ^= w.voice_rng << 5;
    w.wheee_variant = w.voice_rng % max_of(1u, audio::duck_variant_count("wheee"));
    std::vector<f32> start, loop, end;
    audio::render_duck_wheee(w.voice_seed, w.wheee_variant, 8000, start, loop, end);  // only its length
    w.wheee_left = static_cast<f32>(start.size()) / 8000.0f;
    w.wheee = Wheee::start;
    voice_cue(w, "builtin:duck_wheee-start_" + std::to_string(w.voice_seed) + "_" + std::to_string(w.wheee_variant));
    w.said = "wheee";
  } else if (w.wheee == Wheee::start) {
    w.wheee_left -= kControlPeriod;
    if (w.wheee_left <= 0) {
      if (held) { w.wheee = Wheee::loop; voice_cue(w, "builtin:duck_wheee-loop" + base); }
      else { w.wheee = Wheee::off; voice_cue(w, "builtin:duck_wheee-end" + base); }
    }
  } else if (w.wheee == Wheee::loop && !held) {
    w.wheee = Wheee::off;
    voice_cue(w, "builtin:duck_wheee-end" + base);
  }
}

bool GameWorld::walker_sees(ActorId id, ActorId target, bool& visible, f32& bearing, f32& distance) const {
  visible = false;
  bearing = 0;
  distance = -1;
  const WalkerBrain* w = walker_brain(id);
  Camera cam;
  Mat4 m;
  if (!w || !target || !walker_camera(w->actor, cam) || !actor_matrix(target, m)) return false;
  // The target's centre in the camera's frame: inside the picture (37.4 wide x 62 high) and not hidden.
  const Vec3 to = m.translation() - cam.eye, f = normalize(cam.target - cam.eye), u = normalize(cam.up), r = cross(f, u);
  const f32 x = dot(to, f), y = dot(to, r), z = dot(to, u);
  distance = length(to);
  bearing = kern::dm::atan2(-y, x);
  if (x <= 0) return true;
  const f32 tan_v = kern::dm::tan(cam.fov / 2), tan_h = tan_v * kCameraAspect;
  if (std::fabs(y) > x * tan_h || std::fabs(z) > x * tan_v) return true;
  const physics::RayHit hit = physics_.cast_ray_ignoring(cam.eye, to, Span<const physics::Body>(w->bodies, WalkerBrain::kBodies));
  auto it = actor_of_body_.find(hit.body.id);
  // Seen when the first thing on the line is the target itself (or one of its welded parts).
  visible = !hit.hit || (it != actor_of_body_.end() && (it->second == target || level_.body_owner(target) == it->second));
  return true;
}

bool GameWorld::walker_servo_load(ActorId id, f32& strain, f32& drive, f32& speed) const {
  const WalkerBrain* w = walker_brain(id);
  strain = drive = speed = 0;
  if (!w) return false;
  strain = w->servo_strain;
  drive = w->servo_drive;
  speed = w->servo_speed;
  return true;
}

GameWorld::PolicyStatus GameWorld::walker_abilities(const WalkerBrain& w) const {
  const PolicySet& set = policy_set();
  PolicyStatus out;
  out.ground_pick = !w.rollers && walker_policy(set.pick.file) != nullptr;
  out.sit_stand = !w.rollers && walker_policy(set.sitstand) != nullptr;
  const i32 kick = skill_index("kick_left");
  out.kick_left = !w.rollers && kick >= 0 && walker_policy(set.skills[static_cast<std::size_t>(kick)].file) != nullptr;
  return out;
}

bool GameWorld::walker_pet(ActorId id, f32 seconds) {
  WalkerBrain* w = walker_brain(id);
  if (!w) return false;
  w->petted_until = seconds > 0 ? sim_time_ + seconds * std::sqrt(kDuckScale) : -1;
  return true;
}

bool GameWorld::walker_hear(ActorId id, bool voice) {
  WalkerBrain* w = walker_brain(id);
  if (!w) return false;
  w->heard = voice ? 2 : 1;
  return true;
}

bool GameWorld::walker_holding(ActorId id) const {
  // The mouth servo's own encoder: told to shut, it stops short of its pads - something is in the beak.
  const WalkerBrain* w = walker_brain(id);
  if (!w || !w->mouth || w->rollers) return false;
  const bool shutting = w->mouth->servo_target <= mouth_target(0) + 1e-4f;
  return shutting && physics_.joint_angle(w->mouth->joint) > kHoldingAngle;
}

std::string GameWorld::walker_state(ActorId id) const {
  const WalkerBrain* w = walker_brain(id);
  if (!w) return {};
  if (w->manual_left > 0) return "Driven";
  if (w->drive_left != 0) return "Logic";
  return w->autonomous ? w->state : "Idle";
}

/// One frame of the head ToF: 64 beams from the sensor (Box3D ray casts that skip the robot's own
/// links), classified like the robot does it (kinematics/src/tof.rs): floor, too close, obstacle.
void GameWorld::sense_walker(WalkerBrain& w) {
  const f32 S = kDuckScale;
  WalkerSensors& out = w.sensors;
  const physics::Pose tp = physics_.pose(w.trunk);
  const Quat qi{-tp.rotation.x, -tp.rotation.y, -tp.rotation.z, tp.rotation.w};
  out.gravity = qi.rotate({0, 0, -1});
  // The daemon's fall verdict (robotd-params fall_gravity_z, fall_debounce_ms): projected gravity
  // z above -0.5 for 200 ms. sense_walker runs once per control tick.
  w.tilted_for = out.gravity.z > kFallGravityZ ? w.tilted_for + kControlPeriod : 0.0f;
  out.fallen = w.tilted_for >= kFallDebounce;
  const Vec3 v = physics_.linear_velocity(w.trunk);
  out.speed = kern::dm::hypot(v.x, v.y);
  if (sim_time_ + 1e-6 < w.next_tof) return;
  w.next_tof = sim_time_ + 1.0 / (kTofRate / std::sqrt(S));  // 15 Hz real
  out.tof_time = sim_time_;
  const DuckSiteDef* tof = site("tof");
  if (!tof || !w.joint[8]) return;
  const physics::Pose head = physics_.pose(w.joint[8]->b);
  const Vec3 origin = head.position + head.rotation.rotate(tof->pos * S);
  const Vec3 ax = head.rotation.rotate(tof->x), ay = head.rotation.rotate(tof->y), az = head.rotation.rotate(tof->z);
  // The floor under the robot: the lower of its two soles.
  f32 ground = 1e9f;
  for (const DuckHullDef& h : kDuckHulls) {
    if (!h.sole || !w.joint[h.link - 1]) continue;
    const physics::Pose fp = physics_.pose(w.joint[h.link - 1]->b);
    for (i32 k = 0; k < h.count; ++k) {
      const f32* p = kDuckHullPoints + 3 * (h.first + k);
      ground = min_of(ground, (fp.position + fp.rotation.rotate(Vec3{p[0], p[1], p[2]} * S)).z);
    }
  }
  const f32 above_floor = origin.z - ground;
  const f32 half = kTofFov / 2 - kTofFov / kTofSide / 2, step = 2 * half / (kTofSide - 1);
  const Vec3 fwd = tp.rotation.rotate({1, 0, 0});
  const f32 heading = kern::dm::atan2(fwd.y, fwd.x);
  out.nearest = -1;
  out.hits = 0;
  out.forward_observed = 0;
  int blocked_left = 0, blocked_right = 0;
  for (int i = 0; i < WalkerSensors::kZones; ++i) {
    const f32 elevation = half - static_cast<f32>(i / kTofSide) * step, azimuth = half - static_cast<f32>(i % kTofSide) * step;
    const Vec3 beam{kern::dm::cos(elevation) * kern::dm::cos(azimuth), kern::dm::cos(elevation) * kern::dm::sin(azimuth), kern::dm::sin(elevation)};
    const Vec3 dir = ax * beam.x + ay * beam.y + az * beam.z;
    const physics::RayHit hit = physics_.cast_ray_ignoring(origin, dir * (kTofMaxRange * S), Span<const physics::Body>(w.bodies, WalkerBrain::kBodies));
    // A beam within 0.3 rad of the trunk's heading observes the walking path (obstacle.rs: a range,
    // the floor, or "nothing there" all say something).
    if (std::fabs(rt::wrap180((kern::dm::atan2(dir.y, dir.x) - heading) / rt::kDegree) * rt::kDegree) < 0.3f && dir.x * fwd.x + dir.y * fwd.y > 0) ++out.forward_observed;
    if (!hit.hit) { out.range[i] = -1; out.zone[i] = 0; continue; }
    const f32 r = hit.fraction * kTofMaxRange * S;
    out.range[i] = r;
    {  // obstacle.rs: returns above the floor, at most 0.35 m high and 1.5 m away (real)
      const Vec3 p = origin + dir * r;
      const f32 height = p.z - ground, flat = r * kern::dm::hypot(dir.x, dir.y);
      const bool floor = above_floor > 0 && -dir.z > 0 && r * -dir.z >= above_floor * kTofFloorSafety;
      if (!floor && height <= 0.35f * S && flat <= 1.5f * S && out.hits < WalkerSensors::kZones) out.hit[out.hits++] = {p.x, p.y, height};
    }
    const f32 downward = -dir.z;
    if (above_floor > 0 && downward > 0 && r * downward >= above_floor * kTofFloorSafety) { out.zone[i] = 2; continue; }
    const f32 horizontal = r * kern::dm::hypot(dir.x, dir.y);
    if (horizontal < kTofMinRange * S) { out.zone[i] = 1; continue; }
    out.zone[i] = 3;
    if (out.nearest < 0 || horizontal < out.nearest) {
      out.nearest = horizontal;
      out.nearest_bearing = rt::wrap180((kern::dm::atan2(dir.y, dir.x) - heading) / rt::kDegree) * rt::kDegree;
    }
    if (horizontal < 1.0f * S) ((i % kTofSide) < kTofSide / 2 ? blocked_left : blocked_right)++;
  }
  out.free_left = 1 - static_cast<f32>(blocked_left) / (WalkerSensors::kZones / 2);
  out.free_right = 1 - static_cast<f32>(blocked_right) / (WalkerSensors::kZones / 2);
}

void GameWorld::drive_walker(const PlayInput& in, f32 dt) {
  (void)dt;
  // The player's input reaches the walker's controller (drive_walkers runs every walker).
  for (auto& w : walkers_)
    if (w->actor == pw_->actor) { w->input = in; w->has_input = true; }
  pw_->jump_latch = in.jump;  // the duck's voice is in drive_walkers (Space / QUACK: the chirp)
  if (auto it = body_of_actor_.find(pw_->actor); it != body_of_actor_.end()) {
    const Vec3 v = physics_.linear_velocity(it->second->body);
    hud_.speed = kern::dm::hypot(v.x, v.y);
  }
}

/// robotd's Controller::step after the observation is built: expire the windows, pick the network
/// and re-encode the command for it, infer, scale, low-pass (control.rs, in that order).
void GameWorld::run_controller(WalkerBrain& w, f32 obs[61]) {
  using Sit = WalkerBrain::Sit;
  const PolicySet& set = policy_set();
  // Windows first: a tick after a deadline runs the next thing.
  if (w.skill >= 0 && w.skill_remaining <= 0) {
    const SkillDef& d = set.skills[static_cast<std::size_t>(w.skill)];
    if (!w.skill_unwinding && d.unwind_s > 0) {
      w.skill_unwinding = true;
      w.skill_remaining = d.unwind_s;
    } else if (!w.skill_unwinding && d.chain && w.skill_chain > 0) {
      w.skill_remaining = d.duration;  // the button still held: another one
      w.skill_chain = 0;
    } else {
      w.skill = -1;
    }
  }
  if (w.sit == Sit::rising && w.sit_remaining <= 0) w.sit = Sit::up;
  // The command each network is trained on, and which network drives.
  std::shared_ptr<const WalkerPolicy> net;
  f32 scale = w.rollers ? kActionScaleRoller : w.robot_actions ? kRobotActionScaleWalk : kActionScaleWalk;
  const f32 twist = std::sqrt(obs[48] * obs[48] + obs[49] * obs[49] + obs[50] * obs[50]);
  if (w.skill >= 0) {
    const SkillDef& d = set.skills[static_cast<std::size_t>(w.skill)];
    const f32* c = w.skill_unwinding ? d.unwind : d.command;
    for (int i = 0; i < 3; ++i) obs[48 + i] = c[i];
    for (int i = 51; i < 61; ++i) obs[i] = 0;  // head and body zero-padded, as trained
    if (d.phase_slot >= 0 && d.period > 0) {  // a phase-encoded move (Kern's jump): its clock, as trained
      const f32 a = 2 * 3.14159265f * (d.duration - w.skill_remaining) / d.period;
      obs[d.phase_slot] = kern::dm::cos(a);
      obs[d.phase_slot + 1] = kern::dm::sin(a);
    }
    net = walker_policy(d.file);
    if (d.action_scale >= 0) scale = d.action_scale;
    w.driving = w.skill_unwinding ? d.name + ":unwind" : d.name;
  } else if (w.pick_phase >= 0) {
    const PhaseDef& d = w.rollers ? set.crouch : set.pick;
    const f32 a = 2 * 3.14159265f * w.pick_phase;
    obs[48] = kern::dm::cos(a);
    obs[49] = kern::dm::sin(a);
    obs[50] = 0;
    for (int i = 51; i < 61; ++i) obs[i] = 0;
    net = walker_policy(d.file);
    scale = d.action_scale;
    w.driving = w.rollers ? "crouch" : "ground_pick";
  } else if (w.sit != Sit::up) {
    // The posture flag rides the twist vx slot: 1 sit, 0 stand; head and body stay live.
    obs[48] = w.sit == Sit::sitting ? 1.0f : 0.0f;
    obs[49] = obs[50] = 0;
    net = walker_policy(set.sitstand);
    scale = 1.0f;  // the sit/rise cycle runs at scale 1
    w.driving = w.sit == Sit::sitting ? "sit" : "rise";
  } else if (w.rollers) {
    net = walker_policy("roller.onnx");
    for (int i = 51; i < 61; ++i) obs[i] = 0;  // trained without head or body commands
    w.driving = "walk";
  } else if (w.alpha_gait && twist <= kStandingThreshold && walker_policy("alpha_stand.onnx")) {
    net = walker_policy("alpha_stand.onnx");
    scale = kStandingActionScale;
    w.driving = "stand";
  } else {
    net = walker_policy(w.alpha_gait ? "alpha_walking.onnx" : "velstand.onnx");
    w.driving = "walk";
  }
  if (w.external && w.skill < 0 && w.pick_phase < 0 && w.sit == Sit::up) {
    std::memcpy(w.action, w.external_action, sizeof w.action);  // training: the learner drives, as trained
    scale = kActionScaleWalk;
    w.driving = "learner";
  } else if (!net) {
    // Its network is not loaded (the page fetches the set, and fetches again after a failure): no
    // other network stands in for it - the servos hold the home pose, and the duck says why once.
    const std::string file = w.rollers ? "roller.onnx" : "alpha_walking.onnx";
    if (w.driving != "waiting") say("Micro Duck: " + file + " is not loaded yet, it stands until it is");
    std::fill(std::begin(w.action), std::end(w.action), 0.0f);
    w.driving = "waiting";
  } else {
    net->run(obs, w.action);
  }
  for (i32 j = 0; j < kJoints; ++j) {
    const f32 a = std::isfinite(w.action[j]) ? w.action[j] : 0.0f;
    w.action[j] = a;  // raw: the observation feeds the policy its own output back
    // The servo's goal stays inside the XL330's travel (robotd safety.rs: one turn, centred), not the
    // joint's range: training leaves the target free (MJCF ctrlrange +-10), a goal past the range
    // presses the joint into its stop - Box3D's hard limit, the MJCF range, as in MuJoCo.
    f32 t = clamp(kDuckLinks[j + 1].home + scale * a, -kActuatorTravel, kActuatorTravel);
    if (w.has_previous) {
      const f32 alpha = j >= 5 && j < 9 ? (w.robot_actions ? kRobotHeadLowpass : kHeadLowpass) : (w.robot_actions ? kRobotLegsLowpass : kLegsLowpass);
      t = alpha * t + (1 - alpha) * w.previous_target[j];
    }
    w.previous_target[j] = w.target[j] = t;
  }
  w.has_previous = true;
  // Advance the windows after the tick that used them.
  if (w.pick_phase >= 0) {
    const PhaseDef& d = w.rollers ? set.crouch : set.pick;
    const f32 before = w.pick_phase;
    w.pick_phase += kControlPeriod / d.period;
    // (With the play network the beak is the network's: it learned when to open and bite.)
    const bool beak = w.kern_extras && !w.rollers && !walker_play_available(w);
    if (beak && before < kPickOpen && w.pick_phase >= kPickOpen) w.mouth_open = 1;  // open at the bottom
    if (beak && before < kPickClose && w.pick_phase >= kPickClose) w.mouth_open = 0;  // bite
    if (w.pick_phase >= d.end_phase) w.pick_phase = -1;
  }
  if (w.skill >= 0) {
    w.skill_remaining -= kControlPeriod;
    w.skill_chain = max_of(0.0f, w.skill_chain - kControlPeriod);
  }
  if (w.sit == Sit::rising) w.sit_remaining -= kControlPeriod;
  w.sit_settle = max_of(0.0f, w.sit_settle - kControlPeriod);
}

void GameWorld::drive_walkers(f32 h) {
  (void)h;
  const f32 slow = std::sqrt(kDuckScale);  // simulated time runs this much slower than real time
  for (auto& wp : walkers_) {
    WalkerBrain& w = *wp;
    if (w.step % w.decimation == 0) {
      // ---- one control tick (50 Hz real): observation -> policy -> joint targets ----------------
      // Who drives: the player's stick, then a Logic command, then the autonomous brain.
      const PlayInput in = w.has_input ? w.input : PlayInput{};
      const bool touched = std::fabs(in.move_x) > 0.05f || std::fabs(in.move_y) > 0.05f;
      if (touched) w.manual_left = 2.0f;  // like the real duck's pad: autonomy pauses while you drive
      // The voice, as robotd plays it: greet when it comes alive and when the pad comes back,
      // inquire when the pad goes, the chirp on a fresh press (the beak open while it is held),
      // the wheee ride while held, a peck as it sits down. Nothing while it idles, walks or looks.
      if (!w.greeted) { w.greeted = true; walker_say(w, "greet"); }
      if (w.has_input && in.gamepad != w.pad_was) {
        walker_say(w, in.gamepad ? "greet" : "inquire");
        w.pad_was = in.gamepad;
      }
      {
        const bool chirp = (in.robot & 64) || in.jump;
        if (chirp && !w.chirp_held) walker_say(w, "chirp");
        w.chirp_held = chirp;
        walker_wheee(w, (in.robot & 128) != 0 || (w.autonomous && w.manual_left <= 0 && w.drive_left == 0 && w.auto_wheee));
        w.beak_trigger = chirp || (in.robot & 128) ? 1.0f : 0.0f;
      }
      if (w.has_input || in.robot) {
        const u32 pressed = in.robot & ~w.robot_held;
        if ((pressed & 1) && walker_skill(w.actor, "sit").empty() && w.sit == WalkerBrain::Sit::sitting) walker_say(w, "peck");
        if (pressed & 2) walker_skill(w.actor, "pick");
        if (pressed & 4) walker_skill(w.actor, "kick_left");
        if (pressed & 8) walker_skill(w.actor, "kick_right");
        if (in.robot & 16) walker_skill(w.actor, "roulade");  // held: rolls on (CHAIN_WINDOW)
        if (pressed & 32) w.mouth_open = w.mouth_open > 0.5f ? 0.0f : 1.0f;
        if (pressed & 256) walker_skill(w.actor, "jump");  // Kern's own move (kern/policies/jump.onnx)
        if (in.robot) w.manual_left = 2.0f;
        w.robot_held = in.robot;
      }
      sense_walker(w);
      think_walker(w, kControlPeriod);
      play_walker(w, kControlPeriod);  // Kern's learned play layer (ball, sock), when engaged
      f32 cmd[3] = {0, 0, 0}, head[4] = {0, 0, 0, 0};
      if (w.manual_left > 0) {
        cmd[0] = clamp(in.move_y, -1.0f, 1.0f) * kMaxForward;
        cmd[2] = -clamp(in.move_x, -1.0f, 1.0f) * kMaxTurn;
      } else if (w.drive_left != 0) {
        std::memcpy(cmd, w.drive_cmd, sizeof cmd);
      } else if (w.autonomous) {
        std::memcpy(cmd, w.auto_cmd, sizeof cmd);
      }
      bool steered = false;  // something asks the head for a pose this tick
      f32 head_alpha = kCommandAlpha, body_target[3] = {0, 0, 0};
      bool body_active = false;
      const bool brain = w.autonomous && w.manual_left <= 0 && w.drive_left == 0;
      if (w.look_left != 0) { head[2] = w.look_cmd[0]; head[1] = w.look_cmd[1]; head[0] = w.look_neck; steered = true; }
      else if (w.autonomous && w.manual_left <= 0) {
        std::memcpy(head, w.auto_head, sizeof head);
        head_alpha = w.auto_head_alpha;  // autod sends its own rate with robot.head
        for (f32 v : head) steered = steered || v != 0;
      }
      if (brain && w.auto_body_active) {  // robot.pose: the twist is zeroed while the body poses
        body_active = true;
        std::memcpy(body_target, w.auto_body, sizeof body_target);
        cmd[0] = cmd[1] = cmd[2] = 0;
      }
      cmd[0] = clamp(cmd[0], -kMaxForward, kMaxForward);
      cmd[1] = clamp(cmd[1], -kMaxStrafe, kMaxStrafe);
      cmd[2] = clamp(cmd[2], -kMaxTurn, kMaxTurn);
      for (int i = 0; i < 4; ++i) head[i] = clamp(head[i], -kHeadRange[i], kHeadRange[i]);
      for (int i = 0; i < 3; ++i) w.command[i] += kCommandAlpha * (cmd[i] - w.command[i]);
      for (int i = 0; i < 4; ++i) w.head[i] += head_alpha * (head[i] - w.head[i]);
      // Body pose (robotd: glides at the command rate while held, snaps back when let go).
      for (int i = 0; i < 3; ++i) w.body[i] = body_active ? w.body[i] + kCommandAlpha * (body_target[i] - w.body[i]) : 0.0f;
      // Still, as robotd decides it: nothing asks it to move or is moving it, nobody has steered the
      // head lately, it is upright. Then the idle look-around rides on the smoothed head command.
      if (w.play && w.play->engaged) steered = true;  // the play layer steers the head even when it holds it straight
      w.head_age = steered ? 0.0f : w.head_age + kControlPeriod;
      // A move running (a pick, a kick, sitting) is not standing still.
      bool still = !w.sensors.fallen && cmd[0] == 0 && cmd[1] == 0 && cmd[2] == 0 && w.pick_phase < 0 && w.skill < 0 && w.sit == WalkerBrain::Sit::up;
      for (f32 v : w.command) still = still && std::fabs(v) < 1e-3f;
      f32 idle[4];
      idle_walker(w, still, kControlPeriod, idle);
      f32 head_cmd[4];
      for (int i = 0; i < 4; ++i) head_cmd[i] = clamp(w.head[i] + idle[i], -kHeadRange[i], kHeadRange[i]);
      // The mouth (robotd: not part of any policy): what was asked, or the idle yawn.
      // -5..30 deg (pressed shut at the limit): asked for, the call, the trigger held, the yawn.
      const f32 auto_mouth = w.autonomous && w.manual_left <= 0 ? w.auto_mouth : 0.0f;
      if (w.mouth) w.mouth->servo_target = mouth_target(max_of(max_of(max_of(w.mouth_open, w.idle_mouth), auto_mouth), max_of(w.beak_trigger, w.beak_open > 0 ? 1.0f : 0.0f)));
      w.beak_open = max_of(0.0f, w.beak_open - kControlPeriod);
      w.manual_left = max_of(0.0f, w.manual_left - kControlPeriod);
      if (w.drive_left > 0) w.drive_left = max_of(0.0f, w.drive_left - kControlPeriod);  // < 0 holds until the next command
      if (w.look_left > 0) w.look_left = max_of(0.0f, w.look_left - kControlPeriod);

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
      // What the servos work against, for their sound: each one's torque (the servo law Box3D
      // solves: stiffness x error up to the stall torque, minus damping x speed) as a share of its
      // stall torque, and that times its speed - the mechanical power it puts out.
      w.servo_strain = w.servo_drive = w.servo_speed = 0;
      for (i32 j = 0; j < kJoints; ++j) {
        const SimJoint& sj = *w.joint[j];
        const f32 tmax = max_of(1e-6f, sj.servo_torque);
        const f32 err = sj.servo_target - physics_.joint_angle(sj.joint);
        const f32 tau = clamp(sj.servo_stiffness * err, -tmax, tmax) - sj.servo_damping * physics_.joint_speed(sj.joint);
        const f32 share = min_of(1.5f, std::fabs(tau) / tmax);
        w.servo_strain += share / kJoints;
        w.servo_drive += share * std::fabs(speed[j]);
        w.servo_speed += std::fabs(speed[j]);
      }
      for (int i = 0; i < 3; ++i) obs[48 + i] = w.command[i];
      for (int i = 0; i < 4; ++i) obs[51 + i] = head_cmd[i];
      // body command: x, y, yaw stay 0 (unbound in training); z, roll, pitch carry the pose
      obs[57] = w.body[0];
      obs[58] = w.body[1];
      obs[59] = w.body[2];
      run_controller(w, obs);
      std::memcpy(w.last_obs, obs, sizeof w.last_obs);
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
  // The servos' device model (servo.h): one law for the Studio, the Lehrer and the calibration rig.
  for (auto& sj : joints_) {
    if (!sj->servo || sj->broken || !sj->joint.valid()) continue;
    servo_step(physics_, sj->joint, sj->servo_target, sj->servo_stiffness, sj->servo_damping, sj->servo_friction, sj->servo_torque);
  }
}

}  // namespace kern::game
