#include "kern/game/level.h"

#include "kern/audio/synth.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <unordered_set>

namespace kern::game {

namespace {

constexpr f32 kDeg = kPi / 180.0f;

template <typename E, std::size_t N>
bool parse_enum(std::string_view s, const char* const (&names)[N], E& out) {
  for (std::size_t i = 0; i < N; ++i)
    if (s == names[i]) { out = static_cast<E>(i); return true; }
  return false;
}

constexpr const char* kShapeNames[] = {"box", "sphere", "cylinder", "cone", "capsule", "plane", "torus", "ramp"};
constexpr const char* kMotionNames[] = {"static", "kinematic", "dynamic"};
constexpr const char* kColliderNames[] = {"auto", "box", "sphere", "capsule", "hull"};
constexpr const char* kJointNames[] = {"hinge", "slider", "ball", "fixed", "distance", "wheel", "motor", "parallel", "noCollision"};
constexpr const char* kControlNames[] = {"hovercar", "rolling", "character", "flyer", "vehicle", "soldier", "walker"};
constexpr const char* kWeaponNames[] = {"rifle", "pistol", "shotgun", "sniper"};
constexpr const char* kCameraNames[] = {"chase", "firstPerson", "topDown"};
constexpr const char* kTriggerNames[] = {"goal", "hazard", "checkpoint", "message", "launch", "teleport", "damage", "event"};
constexpr const char* kMoverNames[] = {"sine", "pingPong", "loop"};
constexpr const char* kGoalNames[] = {"none", "collectAll", "reachGoal", "survive", "score"};

// ---- writing ----

json::Value vec(Vec3 v) {
  json::Value a = json::Value::array();
  for (int i = 0; i < 3; ++i) {
    // Short decimal output: 1e-6 precision is plenty for an editor document.
    const f64 x = std::round(static_cast<f64>(v[i]) * 1e6) / 1e6;
    a.push(x == 0 ? 0.0 : x);
  }
  return a;
}
json::Value num(f32 v) {
  const f64 x = std::round(static_cast<f64>(v) * 1e6) / 1e6;
  return x == 0 ? 0.0 : x;
}
/// Seven significant digits (inertia tensors span many orders of magnitude).
json::Value sig(f32 v) {
  char b[32];
  std::snprintf(b, sizeof b, "%.7g", static_cast<f64>(v));
  return std::strtod(b, nullptr);
}

// ---- reading ----

struct Reader {
  const json::Value& v;
  std::string* error;
  bool ok = true;

  bool fail(const std::string& what) {
    if (ok && error) *error = what;
    ok = false;
    return false;
  }
  void number(const char* key, f32& out, f64 lo = -1e9, f64 hi = 1e9) {
    const json::Value* x = v.find(key);
    if (!x) return;
    if (!x->is_number() || !std::isfinite(x->as_number())) { fail(std::string(key) + " must be a number"); return; }
    out = static_cast<f32>(std::clamp(x->as_number(), lo, hi));
  }
  void integer(const char* key, i32& out, i32 lo, i32 hi) {
    f32 f = static_cast<f32>(out);
    number(key, f, lo, hi);
    out = static_cast<i32>(std::lround(f));
  }
  void uinteger(const char* key, u32& out) {
    const json::Value* x = v.find(key);
    if (!x) return;
    if (!x->is_number() || x->as_number() < 0 || x->as_number() > 4294967295.0) { fail(std::string(key) + " must be an unsigned integer"); return; }
    out = static_cast<u32>(x->as_number());
  }
  void boolean(const char* key, bool& out) {
    const json::Value* x = v.find(key);
    if (!x) return;
    if (!x->is_bool()) { fail(std::string(key) + " must be true or false"); return; }
    out = x->as_bool();
  }
  void string(const char* key, std::string& out) {
    const json::Value* x = v.find(key);
    if (!x) return;
    if (!x->is_string()) { fail(std::string(key) + " must be a string"); return; }
    out = x->as_string();
  }
  void vector(const char* key, Vec3& out) {
    const json::Value* x = v.find(key);
    if (!x) return;
    if (!x->is_array() || x->size() != 3) { fail(std::string(key) + " must be [x, y, z]"); return; }
    for (int i = 0; i < 3; ++i) {
      const json::Value& c = (*x)[static_cast<std::size_t>(i)];
      if (!c.is_number() || !std::isfinite(c.as_number()) || std::fabs(c.as_number()) > 1e7) { fail(std::string(key) + " has an invalid component"); return; }
      out[i] = static_cast<f32>(c.as_number());
    }
  }
  template <std::size_t N>
  void numbers(const char* key, f32 (&out)[N], f64 lo, f64 hi) {
    const json::Value* x = v.find(key);
    if (!x) return;
    if (!x->is_array() || x->size() != N) { fail(std::string(key) + " must be an array of " + std::to_string(N) + " numbers"); return; }
    for (std::size_t i = 0; i < N; ++i) {
      const json::Value& c = (*x)[i];
      if (!c.is_number() || !std::isfinite(c.as_number()) || c.as_number() < lo || c.as_number() > hi) { fail(std::string(key) + " has an invalid component"); return; }
      out[i] = static_cast<f32>(c.as_number());
    }
  }
  void color(const char* key, Vec3& out) {
    const json::Value* x = v.find(key);
    if (!x) return;
    if (!x->is_string() || !parse_hex_color(x->as_string(), out)) fail(std::string(key) + " must be a #rrggbb colour");
  }
  template <typename E, std::size_t N>
  void enumeration(const char* key, const char* const (&names)[N], E& out) {
    const json::Value* x = v.find(key);
    if (!x) return;
    if (!x->is_string() || !parse_enum(x->as_string(), names, out)) fail(std::string(key) + ": unknown value");
  }
  void flags(const char* key, bool (&out)[3]) {
    const json::Value* x = v.find(key);
    if (!x) return;
    if (!x->is_array() || x->size() != 3) { fail(std::string(key) + " must be [x, y, z] booleans"); return; }
    for (int i = 0; i < 3; ++i) out[i] = (*x)[static_cast<std::size_t>(i)].truthy();
  }
};

json::Value transform_json(const Transform& t) {
  json::Value o = json::Value::object();
  o.set("position", vec(t.position));
  o.set("rotation", vec(t.rotation));
  o.set("scale", vec(t.scale));
  return o;
}

json::Value material_json(const MaterialDesc& m) {
  json::Value o = json::Value::object();
  o.set("color", hex_color(m.color));
  o.set("roughness", num(m.roughness));
  o.set("metalness", num(m.metalness));
  o.set("emissive", hex_color(m.emissive));
  o.set("emissiveStrength", num(m.emissive_strength));
  o.set("opacity", num(m.opacity));
  o.set("unlit", m.unlit);
  o.set("castShadow", m.cast_shadow);
  o.set("doubleSided", m.double_sided);
  o.set("texture", m.texture);
  o.set("tiling", num(m.tiling));
  return o;
}

bool read_material(const json::Value& v, MaterialDesc& m, std::string* error) {
  if (!v.is_object()) return true;
  Reader r{v, error};
  r.color("color", m.color);
  r.number("roughness", m.roughness, 0, 1);
  r.number("metalness", m.metalness, 0, 1);
  r.color("emissive", m.emissive);
  r.number("emissiveStrength", m.emissive_strength, 0, 1000);
  r.number("opacity", m.opacity, 0, 1);
  r.boolean("unlit", m.unlit);
  r.boolean("castShadow", m.cast_shadow);
  r.boolean("doubleSided", m.double_sided);
  r.string("texture", m.texture);
  r.number("tiling", m.tiling, 0.001, 1000);
  return r.ok;
}

}  // namespace

// ---------------------------------------------------------------------------
// names

const char* shape_name(Shape s) { return kShapeNames[static_cast<u32>(s)]; }
bool parse_shape(std::string_view s, Shape& out) { return parse_enum(s, kShapeNames, out); }
const char* motion_name(Motion m) { return kMotionNames[static_cast<u32>(m)]; }
const char* collider_shape_name(ColliderShape s) { return kColliderNames[static_cast<u32>(s)]; }
const char* joint_kind_name(JointKind k) { return kJointNames[static_cast<u32>(k)]; }
const char* control_mode_name(ControlMode m) { return kControlNames[static_cast<u32>(m)]; }
const char* camera_mode_name(CameraMode m) { return kCameraNames[static_cast<u32>(m)]; }
const char* weapon_kind_name(WeaponKind k) { return kWeaponNames[static_cast<u32>(k)]; }
const char* trigger_action_name(TriggerAction a) { return kTriggerNames[static_cast<u32>(a)]; }
const char* mover_mode_name(MoverMode m) { return kMoverNames[static_cast<u32>(m)]; }
const char* game_goal_name(GameGoal g) { return kGoalNames[static_cast<u32>(g)]; }

// ---------------------------------------------------------------------------
// transforms

Quat euler_to_quat(Vec3 d) {
  const Quat qx = Quat::axis_angle({1, 0, 0}, d.x * kDeg);
  const Quat qy = Quat::axis_angle({0, 1, 0}, d.y * kDeg);
  const Quat qz = Quat::axis_angle({0, 0, 1}, d.z * kDeg);
  return qz * qy * qx;
}

WeaponComponent weapon_preset(WeaponKind kind) {
  WeaponComponent w;
  w.kind = kind;
  switch (kind) {
    case WeaponKind::rifle: break;  // the defaults: 24 dmg, 600 rpm automatic, 30 / 120
    case WeaponKind::pistol:
      w.damage = 30; w.fire_rate = 360; w.automatic = false; w.magazine = 12; w.reserve = 60; w.reload_time = 1.4f;
      w.spread = 1.0f; w.aim_spread = 0.25f; w.recoil = 1.1f; w.range = 120; w.headshot = 2.0f; w.aim_fov = 52;
      w.fire_sound = "builtin:pistol";
      break;
    case WeaponKind::shotgun:
      w.damage = 11; w.pellets = 8; w.fire_rate = 75; w.automatic = false; w.magazine = 6; w.reserve = 36; w.reload_time = 2.6f;
      w.spread = 4.5f; w.aim_spread = 3.2f; w.recoil = 2.4f; w.range = 45; w.headshot = 1.5f; w.impulse = 2; w.aim_fov = 55;
      w.fire_sound = "builtin:launcher";
      break;
    case WeaponKind::sniper:
      w.damage = 90; w.fire_rate = 45; w.automatic = false; w.magazine = 5; w.reserve = 25; w.reload_time = 2.8f;
      w.spread = 3.0f; w.aim_spread = 0.02f; w.recoil = 3.0f; w.range = 1000; w.headshot = 3.0f; w.impulse = 12; w.aim_fov = 16;
      w.fire_sound = "builtin:sniper";
      break;
  }
  return w;
}

Vec3 quat_to_euler(Quat q) {
  const f32 n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
  if (n > 0) { q.x /= n; q.y /= n; q.z /= n; q.w /= n; }
  // ZYX (yaw, pitch, roll) decomposition.
  const f32 sinp = clamp(2 * (q.w * q.y - q.z * q.x), -1.0f, 1.0f);
  const f32 pitch = std::asin(sinp);
  f32 roll, yaw;
  if (std::fabs(sinp) > 0.99999f) {
    // Gimbal lock: put everything into yaw.
    roll = 0;
    yaw = std::atan2(-2 * (q.x * q.y - q.w * q.z), 1 - 2 * (q.x * q.x + q.z * q.z));
  } else {
    roll = std::atan2(2 * (q.w * q.x + q.y * q.z), 1 - 2 * (q.x * q.x + q.y * q.y));
    yaw = std::atan2(2 * (q.w * q.z + q.x * q.y), 1 - 2 * (q.y * q.y + q.z * q.z));
  }
  auto tidy = [](f32 deg) {
    const f32 r = std::round(deg * 1e4f) / 1e4f;
    return r == 0 ? 0.0f : r;  // no negative zero in the details panel
  };
  return {tidy(roll / kDeg), tidy(pitch / kDeg), tidy(yaw / kDeg)};
}

Mat4 transform_matrix(const Transform& t) { return Mat4::trs(t.position, euler_to_quat(t.rotation), t.scale); }

Transform matrix_transform(const Mat4& m) {
  Transform t;
  t.position = m.translation();
  Vec3 c0{m.m[0], m.m[1], m.m[2]}, c1{m.m[4], m.m[5], m.m[6]}, c2{m.m[8], m.m[9], m.m[10]};
  t.scale = {length(c0), length(c1), length(c2)};
  if (t.scale.x > 1e-9f) c0 = c0 / t.scale.x;
  if (t.scale.y > 1e-9f) c1 = c1 / t.scale.y;
  if (t.scale.z > 1e-9f) c2 = c2 / t.scale.z;
  // Rotation matrix -> quaternion.
  const f32 trace = c0.x + c1.y + c2.z;
  Quat q;
  if (trace > 0) {
    const f32 s = std::sqrt(trace + 1) * 2;
    q = {(c1.z - c2.y) / s, (c2.x - c0.z) / s, (c0.y - c1.x) / s, 0.25f * s};
  } else if (c0.x > c1.y && c0.x > c2.z) {
    const f32 s = std::sqrt(1 + c0.x - c1.y - c2.z) * 2;
    q = {0.25f * s, (c1.x + c0.y) / s, (c2.x + c0.z) / s, (c1.z - c2.y) / s};
  } else if (c1.y > c2.z) {
    const f32 s = std::sqrt(1 + c1.y - c0.x - c2.z) * 2;
    q = {(c1.x + c0.y) / s, 0.25f * s, (c2.y + c1.z) / s, (c2.x - c0.z) / s};
  } else {
    const f32 s = std::sqrt(1 + c2.z - c0.x - c1.y) * 2;
    q = {(c2.x + c0.z) / s, (c2.y + c1.z) / s, 0.25f * s, (c0.y - c1.x) / s};
  }
  t.rotation = quat_to_euler(q);
  return t;
}

// ---------------------------------------------------------------------------
// level

Actor* Level::find(ActorId id) {
  if (!id) return nullptr;
  for (Actor& a : actors)
    if (a.id == id) return &a;
  return nullptr;
}
const Actor* Level::find(ActorId id) const { return const_cast<Level*>(this)->find(id); }

Actor& Level::add(Actor actor) {
  if (!actor.id || find(actor.id)) actor.id = next_id;
  next_id = max_of(next_id, actor.id + 1);
  actors.push_back(std::move(actor));
  return actors.back();
}

std::vector<ActorId> Level::children(ActorId parent) const {
  std::vector<ActorId> out;
  for (const Actor& a : actors)
    if (a.parent == parent) out.push_back(a.id);
  return out;
}

Mat4 Level::world_matrix(ActorId id) const {
  Mat4 m;
  u32 guard = 0;
  for (const Actor* a = find(id); a && guard < 256; a = find(a->parent), ++guard) m = transform_matrix(a->transform) * m;
  return m;
}

ActorId Level::body_owner(ActorId id) const {
  u32 guard = 0;
  for (const Actor* a = find(id); a && guard < 256; a = find(a->parent), ++guard)
    if (a->body) return a->id;
  return 0;
}

bool Level::is_ancestor(ActorId ancestor, ActorId id) const {
  u32 guard = 0;
  for (const Actor* a = find(id); a && guard < 256; ++guard) {
    if (a->parent == ancestor && ancestor) return true;
    a = find(a->parent);
  }
  return false;
}

const TextureAsset* Level::texture(std::string_view id) const {
  for (const TextureAsset& t : textures)
    if (t.id == id) return &t;
  return nullptr;
}

const SoundAsset* Level::sound(std::string_view id) const {
  for (const SoundAsset& s : sounds)
    if (s.id == id) return &s;
  return nullptr;
}

// ---------------------------------------------------------------------------
// colours

std::string hex_color(Vec3 c) {
  char buf[8];
  auto b = [](f32 v) { return static_cast<int>(std::lround(clamp(v, 0.0f, 1.0f) * 255)); };
  std::snprintf(buf, sizeof buf, "#%02x%02x%02x", b(c.x), b(c.y), b(c.z));
  return buf;
}

bool parse_hex_color(std::string_view s, Vec3& out) {
  if (s.size() != 7 || s[0] != '#') return false;
  u32 v = 0;
  for (std::size_t i = 1; i < 7; ++i) {
    const char c = s[i];
    const int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
    if (d < 0) return false;
    v = v * 16 + static_cast<u32>(d);
  }
  out = kern::hex_color(v);
  return true;
}

Vec3 srgb_to_linear(Vec3 c) {
  auto f = [](f32 v) { return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f); };
  return {f(c.x), f(c.y), f(c.z)};
}

// ---------------------------------------------------------------------------
// actors

json::Value actor_to_json(const Actor& a) {
  json::Value o = json::Value::object();
  o.set("id", a.id);
  o.set("name", a.name);
  if (a.parent) o.set("parent", a.parent);
  if (!a.folder.empty()) o.set("folder", a.folder);
  if (a.hidden) o.set("hidden", true);
  if (a.locked) o.set("locked", true);
  o.set("transform", transform_json(a.transform));
  if (!a.tags.empty()) {
    json::Value t = json::Value::array();
    for (const std::string& s : a.tags) t.push(s);
    o.set("tags", std::move(t));
  }
  json::Value c = json::Value::object();
  if (a.mesh) {
    const MeshComponent& m = *a.mesh;
    json::Value x = json::Value::object();
    x.set("shape", shape_name(m.shape));
    x.set("size", vec(m.size));
    x.set("radius", num(m.radius));
    x.set("height", num(m.height));
    x.set("segments", m.segments);
    x.set("collision", m.collision);
    x.set("material", material_json(m.material));
    c.set("mesh", std::move(x));
  }
  if (a.light) {
    json::Value x = json::Value::object();
    x.set("color", hex_color(a.light->color));
    x.set("intensity", num(a.light->intensity));
    x.set("range", num(a.light->range));
    c.set("light", std::move(x));
  }
  if (a.body) {
    const BodyComponent& b = *a.body;
    json::Value x = json::Value::object();
    x.set("motion", motion_name(b.motion));
    x.set("mass", num(b.mass));
    x.set("linearDamping", num(b.linear_damping));
    x.set("angularDamping", num(b.angular_damping));
    x.set("gravityScale", num(b.gravity_scale));
    x.set("bullet", b.bullet);
    x.set("startAwake", b.start_awake);
    x.set("allowSleep", b.allow_sleep);
    json::Value ll = json::Value::array(), la = json::Value::array();
    for (int i = 0; i < 3; ++i) { ll.push(b.lock_linear[i]); la.push(b.lock_angular[i]); }
    x.set("lockLinear", std::move(ll));
    x.set("lockAngular", std::move(la));
    x.set("linearVelocity", vec(b.linear_velocity));
    x.set("angularVelocity", vec(b.angular_velocity));
    x.set("impactSound", b.impact_sound);
    x.set("impactThreshold", num(b.impact_threshold));
    if (b.explicit_inertia()) {
      x.set("centerOfMass", vec(b.center_of_mass));
      json::Value in = json::Value::array();
      for (f32 v : b.inertia) in.push(sig(v));
      x.set("inertia", std::move(in));
    }
    c.set("body", std::move(x));
  }
  if (!a.colliders.empty()) {
    json::Value list = json::Value::array();
    for (const ColliderComponent& k : a.colliders) {
      json::Value x = json::Value::object();
      x.set("shape", collider_shape_name(k.shape));
      x.set("center", vec(k.center));
      x.set("rotation", vec(k.rotation));
      x.set("size", vec(k.size));
      x.set("radius", num(k.radius));
      x.set("length", num(k.length));
      x.set("density", num(k.density));
      x.set("friction", num(k.friction));
      x.set("restitution", num(k.restitution));
      x.set("rollingResistance", num(k.rolling_resistance));
      x.set("sensor", k.sensor);
      x.set("category", k.category);
      x.set("mask", k.mask);
      x.set("group", k.group);
      list.push(std::move(x));
    }
    c.set("colliders", std::move(list));
  }
  if (!a.joints.empty()) {
    json::Value list = json::Value::array();
    for (const JointComponent& j : a.joints) {
      json::Value x = json::Value::object();
      x.set("kind", joint_kind_name(j.kind));
      x.set("target", j.target);
      x.set("anchor", vec(j.anchor));
      x.set("axis", vec(j.axis));
      x.set("collideConnected", j.collide_connected);
      x.set("enableLimit", j.enable_limit);
      x.set("lower", num(j.lower));
      x.set("upper", num(j.upper));
      if (j.reference != 0) x.set("reference", num(j.reference));
      x.set("enableMotor", j.enable_motor);
      x.set("motorSpeed", num(j.motor_speed));
      x.set("maxMotorForce", num(j.max_motor_force));
      x.set("enableSpring", j.enable_spring);
      x.set("springHertz", num(j.spring_hertz));
      x.set("springDamping", num(j.spring_damping));
      x.set("length", num(j.length));
      x.set("breakForce", num(j.break_force));
      x.set("breakTorque", num(j.break_torque));
      x.set("breakSound", j.break_sound);
      if (j.servo) {
        x.set("servo", true);
        x.set("servoTarget", num(j.servo_target));
        x.set("servoStiffness", num(j.servo_stiffness));
        x.set("servoDamping", num(j.servo_damping));
        x.set("servoFriction", num(j.servo_friction));
      }
      list.push(std::move(x));
    }
    c.set("joints", std::move(list));
  }
  if (a.controller) {
    const ControllerComponent& k = *a.controller;
    json::Value x = json::Value::object();
    x.set("mode", control_mode_name(k.mode));
    if (k.mode == ControlMode::walker) x.set("autonomous", k.autonomous);
    x.set("maxSpeed", num(k.max_speed));
    x.set("acceleration", num(k.acceleration));
    x.set("turnRate", num(k.turn_rate));
    x.set("jumpSpeed", num(k.jump_speed));
    x.set("boost", k.boost);
    x.set("boostSpeed", num(k.boost_speed));
    x.set("hoverHeight", num(k.hover_height));
    x.set("camera", camera_mode_name(k.camera));
    x.set("cameraDistance", num(k.camera_distance));
    x.set("cameraHeight", num(k.camera_height));
    x.set("cameraFov", num(k.camera_fov));
    x.set("engineSound", k.engine_sound);
    x.set("jumpSound", k.jump_sound);
    x.set("boostSound", k.boost_sound);
    x.set("crouchSpeed", num(k.crouch_speed));
    x.set("eyeHeight", num(k.eye_height));
    x.set("lookSensitivity", num(k.look_sensitivity));
    x.set("headBob", k.head_bob);
    c.set("controller", std::move(x));
  }
  if (a.weapon) {
    const WeaponComponent& k = *a.weapon;
    json::Value x = json::Value::object();
    x.set("kind", weapon_kind_name(k.kind));
    x.set("damage", num(k.damage));
    x.set("fireRate", num(k.fire_rate));
    x.set("automatic", k.automatic);
    x.set("magazine", k.magazine);
    x.set("reserve", k.reserve);
    x.set("reloadTime", num(k.reload_time));
    x.set("spread", num(k.spread));
    x.set("aimSpread", num(k.aim_spread));
    x.set("recoil", num(k.recoil));
    x.set("range", num(k.range));
    x.set("headshot", num(k.headshot));
    x.set("impulse", num(k.impulse));
    x.set("pellets", k.pellets);
    x.set("aimFov", num(k.aim_fov));
    x.set("fireSound", k.fire_sound);
    x.set("reloadSound", k.reload_sound);
    c.set("weapon", std::move(x));
  }
  if (a.health) {
    const HealthComponent& k = *a.health;
    json::Value x = json::Value::object();
    x.set("maxHealth", num(k.max_health));
    x.set("armor", num(k.armor));
    x.set("regenDelay", num(k.regen_delay));
    x.set("regenRate", num(k.regen_rate));
    x.set("respawn", k.respawn);
    x.set("respawnDelay", num(k.respawn_delay));
    x.set("points", num(k.points));
    x.set("hitSound", k.hit_sound);
    x.set("deathSound", k.death_sound);
    x.set("explodeRadius", num(k.explode_radius));
    x.set("explodeDamage", num(k.explode_damage));
    c.set("health", std::move(x));
  }
  if (a.pickup) {
    json::Value x = json::Value::object();
    x.set("points", num(a.pickup->points));
    x.set("burstColor", hex_color(a.pickup->burst_color));
    x.set("respawn", num(a.pickup->respawn));
    x.set("sound", a.pickup->sound);
    x.set("heal", num(a.pickup->heal));
    x.set("ammo", a.pickup->ammo);
    c.set("pickup", std::move(x));
  }
  if (a.trigger) {
    json::Value x = json::Value::object();
    x.set("action", trigger_action_name(a.trigger->action));
    x.set("message", a.trigger->message);
    x.set("once", a.trigger->once);
    x.set("sound", a.trigger->sound);
    x.set("strength", num(a.trigger->strength));
    x.set("target", a.trigger->target);
    c.set("trigger", std::move(x));
  }
  if (a.rotator) {
    json::Value x = json::Value::object();
    x.set("rate", vec(a.rotator->rate));
    c.set("rotator", std::move(x));
  }
  if (a.mover) {
    json::Value x = json::Value::object();
    x.set("offset", vec(a.mover->offset));
    x.set("period", num(a.mover->period));
    x.set("phase", num(a.mover->phase));
    x.set("mode", mover_mode_name(a.mover->mode));
    c.set("mover", std::move(x));
  }
  if (a.city) {
    const CityComponent& k = *a.city;
    json::Value x = json::Value::object();
    x.set("seed", k.seed);
    x.set("blocks", k.blocks);
    x.set("pitch", num(k.pitch));
    x.set("street", num(k.street));
    x.set("maxHeight", num(k.max_height));
    x.set("trees", k.trees);
    x.set("traffic", k.traffic);
    x.set("nightWindows", k.night_windows);
    c.set("city", std::move(x));
  }
  if (a.audio) {
    const AudioComponent& k = *a.audio;
    json::Value x = json::Value::object();
    x.set("sound", k.sound);
    x.set("volume", num(k.volume));
    x.set("pitch", num(k.pitch));
    x.set("loop", k.loop);
    x.set("autoplay", k.autoplay);
    x.set("spatial", k.spatial);
    x.set("minDistance", num(k.min_distance));
    x.set("maxDistance", num(k.max_distance));
    x.set("rolloff", audio::rolloff_name(k.rolloff));
    x.set("bus", audio::bus_name(k.bus));
    x.set("doppler", num(k.doppler));
    x.set("pitchBySpeed", num(k.pitch_by_speed));
    x.set("delay", num(k.delay));
    c.set("audio", std::move(x));
  }
  if (a.logic) c.set("logic", logic::graph_to_json(*a.logic));
  o.set("components", std::move(c));
  return o;
}

bool actor_from_json(const json::Value& v, Actor& a, std::string* error) {
  if (!v.is_object()) { if (error) *error = "actor must be an object"; return false; }
  Reader r{v, error};
  r.uinteger("id", a.id);
  r.string("name", a.name);
  r.uinteger("parent", a.parent);
  r.string("folder", a.folder);
  r.boolean("hidden", a.hidden);
  r.boolean("locked", a.locked);
  if (const json::Value* t = v.find("transform")) {
    Reader tr{*t, error};
    tr.vector("position", a.transform.position);
    tr.vector("rotation", a.transform.rotation);
    tr.vector("scale", a.transform.scale);
    if (!tr.ok) return false;
  }
  if (const json::Value* tags = v.find("tags"); tags && tags->is_array())
    for (const json::Value& t : tags->items())
      if (t.is_string()) a.tags.push_back(t.as_string());
  const json::Value* comps = v.find("components");
  if (comps && comps->is_object()) {
    const json::Value& c = *comps;
    if (const json::Value* x = c.find("mesh")) {
      MeshComponent m;
      Reader mr{*x, error};
      mr.enumeration("shape", kShapeNames, m.shape);
      mr.vector("size", m.size);
      mr.number("radius", m.radius, 0.001, 1e5);
      mr.number("height", m.height, 0.001, 1e5);
      mr.integer("segments", m.segments, 3, 128);
      mr.boolean("collision", m.collision);
      if (!mr.ok || !read_material((*x)["material"], m.material, error)) return false;
      a.mesh = m;
    }
    if (const json::Value* x = c.find("light")) {
      LightComponent l;
      Reader lr{*x, error};
      lr.color("color", l.color);
      lr.number("intensity", l.intensity, 0, 1e5);
      lr.number("range", l.range, 0.1, 1e4);
      if (!lr.ok) return false;
      a.light = l;
    }
    if (const json::Value* x = c.find("body")) {
      BodyComponent b;
      Reader br{*x, error};
      br.enumeration("motion", kMotionNames, b.motion);
      br.number("mass", b.mass, 0, 1e9);
      br.number("linearDamping", b.linear_damping, 0, 1e4);
      br.number("angularDamping", b.angular_damping, 0, 1e4);
      br.number("gravityScale", b.gravity_scale, -100, 100);
      br.boolean("bullet", b.bullet);
      br.boolean("startAwake", b.start_awake);
      br.boolean("allowSleep", b.allow_sleep);
      br.flags("lockLinear", b.lock_linear);
      br.flags("lockAngular", b.lock_angular);
      br.vector("linearVelocity", b.linear_velocity);
      br.vector("angularVelocity", b.angular_velocity);
      br.string("impactSound", b.impact_sound);
      br.number("impactThreshold", b.impact_threshold, 0, 1e4);
      br.vector("centerOfMass", b.center_of_mass);
      br.numbers("inertia", b.inertia, -1e9, 1e9);
      if (!br.ok) return false;
      a.body = b;
    }
    if (const json::Value* list = c.find("colliders"); list && list->is_array())
      for (const json::Value& x : list->items()) {
        ColliderComponent k;
        Reader kr{x, error};
        kr.enumeration("shape", kColliderNames, k.shape);
        kr.vector("center", k.center);
        kr.vector("rotation", k.rotation);
        kr.vector("size", k.size);
        kr.number("radius", k.radius, 0.001, 1e5);
        kr.number("length", k.length, 0, 1e5);
        kr.number("density", k.density, 0, 1e6);
        kr.number("friction", k.friction, 0, 10);
        kr.number("restitution", k.restitution, 0, 1);
        kr.number("rollingResistance", k.rolling_resistance, 0, 10);
        kr.boolean("sensor", k.sensor);
        kr.uinteger("category", k.category);
        kr.uinteger("mask", k.mask);
        kr.integer("group", k.group, -32768, 32767);
        if (!kr.ok) return false;
        a.colliders.push_back(k);
      }
    if (const json::Value* list = c.find("joints"); list && list->is_array())
      for (const json::Value& x : list->items()) {
        JointComponent j;
        Reader jr{x, error};
        jr.enumeration("kind", kJointNames, j.kind);
        jr.uinteger("target", j.target);
        jr.vector("anchor", j.anchor);
        jr.vector("axis", j.axis);
        jr.boolean("collideConnected", j.collide_connected);
        jr.boolean("enableLimit", j.enable_limit);
        jr.number("lower", j.lower);
        jr.number("upper", j.upper);
        jr.boolean("enableMotor", j.enable_motor);
        jr.number("motorSpeed", j.motor_speed);
        jr.number("maxMotorForce", j.max_motor_force, 0, 1e9);
        jr.boolean("enableSpring", j.enable_spring);
        jr.number("springHertz", j.spring_hertz, 0, 1000);
        jr.number("springDamping", j.spring_damping, 0, 100);
        jr.number("length", j.length, 0, 1e5);
        jr.number("breakForce", j.break_force, 0, 1e12);
        jr.number("breakTorque", j.break_torque, 0, 1e12);
        jr.string("breakSound", j.break_sound);
        jr.boolean("servo", j.servo);
        jr.number("servoTarget", j.servo_target, -3600, 3600);
        jr.number("servoStiffness", j.servo_stiffness, 0, 1e9);
        jr.number("servoDamping", j.servo_damping, 0, 1e9);
        jr.number("servoFriction", j.servo_friction, 0, 1e9);
        jr.number("reference", j.reference, -360, 360);
        if (!jr.ok) return false;
        a.joints.push_back(j);
      }
    if (const json::Value* x = c.find("controller")) {
      ControllerComponent k;
      Reader kr{*x, error};
      kr.enumeration("mode", kControlNames, k.mode);
      kr.boolean("autonomous", k.autonomous);
      kr.number("maxSpeed", k.max_speed, 0, 1000);
      kr.number("acceleration", k.acceleration, 0, 1000);
      kr.number("turnRate", k.turn_rate, 0, 3600);
      kr.number("jumpSpeed", k.jump_speed, 0, 200);
      kr.boolean("boost", k.boost);
      kr.number("boostSpeed", k.boost_speed, 0, 1000);
      kr.number("hoverHeight", k.hover_height, 0.05, 50);
      kr.enumeration("camera", kCameraNames, k.camera);
      kr.number("cameraDistance", k.camera_distance, 0, 500);
      kr.number("cameraHeight", k.camera_height, -100, 500);
      kr.number("cameraFov", k.camera_fov, 20, 120);
      kr.string("engineSound", k.engine_sound);
      kr.string("jumpSound", k.jump_sound);
      kr.string("boostSound", k.boost_sound);
      kr.number("crouchSpeed", k.crouch_speed, 0, 100);
      kr.number("eyeHeight", k.eye_height, 0.2, 10);
      kr.number("lookSensitivity", k.look_sensitivity, 0.05, 10);
      kr.boolean("headBob", k.head_bob);
      if (!kr.ok) return false;
      a.controller = k;
    }
    if (const json::Value* x = c.find("weapon")) {
      WeaponComponent k;
      Reader wr{*x, error};
      wr.enumeration("kind", kWeaponNames, k.kind);
      wr.number("damage", k.damage, 0, 1e6);
      wr.number("fireRate", k.fire_rate, 1, 6000);
      wr.boolean("automatic", k.automatic);
      wr.integer("magazine", k.magazine, 1, 10000);
      wr.integer("reserve", k.reserve, 0, 1000000);
      wr.number("reloadTime", k.reload_time, 0, 60);
      wr.number("spread", k.spread, 0, 45);
      wr.number("aimSpread", k.aim_spread, 0, 45);
      wr.number("recoil", k.recoil, 0, 30);
      wr.number("range", k.range, 1, 10000);
      wr.number("headshot", k.headshot, 0, 100);
      wr.number("impulse", k.impulse, 0, 1e6);
      wr.integer("pellets", k.pellets, 1, 64);
      wr.number("aimFov", k.aim_fov, 5, 120);
      wr.string("fireSound", k.fire_sound);
      wr.string("reloadSound", k.reload_sound);
      if (!wr.ok) return false;
      a.weapon = k;
    }
    if (const json::Value* x = c.find("health")) {
      HealthComponent k;
      Reader hr{*x, error};
      hr.number("maxHealth", k.max_health, 1, 1e7);
      hr.number("armor", k.armor, 0, 1e7);
      hr.number("regenDelay", k.regen_delay, 0, 1e5);
      hr.number("regenRate", k.regen_rate, 0, 1e6);
      hr.boolean("respawn", k.respawn);
      hr.number("respawnDelay", k.respawn_delay, 0, 1e5);
      hr.number("points", k.points, -1e6, 1e6);
      hr.string("hitSound", k.hit_sound);
      hr.string("deathSound", k.death_sound);
      hr.number("explodeRadius", k.explode_radius, 0, 1000);
      hr.number("explodeDamage", k.explode_damage, 0, 1e6);
      if (!hr.ok) return false;
      a.health = k;
    }
    if (const json::Value* x = c.find("pickup")) {
      PickupComponent p;
      Reader pr{*x, error};
      pr.number("points", p.points, -1e6, 1e6);
      pr.color("burstColor", p.burst_color);
      pr.number("respawn", p.respawn, 0, 1e5);
      pr.string("sound", p.sound);
      pr.number("heal", p.heal, 0, 1e6);
      pr.integer("ammo", p.ammo, 0, 100000);
      if (!pr.ok) return false;
      a.pickup = p;
    }
    if (const json::Value* x = c.find("trigger")) {
      TriggerComponent t;
      Reader tr{*x, error};
      tr.enumeration("action", kTriggerNames, t.action);
      tr.string("message", t.message);
      tr.boolean("once", t.once);
      tr.string("sound", t.sound);
      tr.number("strength", t.strength, 0, 1e5);
      tr.uinteger("target", t.target);
      if (!tr.ok) return false;
      a.trigger = t;
    }
    if (const json::Value* x = c.find("rotator")) {
      RotatorComponent t;
      Reader tr{*x, error};
      tr.vector("rate", t.rate);
      if (!tr.ok) return false;
      a.rotator = t;
    }
    if (const json::Value* x = c.find("mover")) {
      MoverComponent m;
      Reader mr{*x, error};
      mr.vector("offset", m.offset);
      mr.number("period", m.period, 0.05, 1e5);
      mr.number("phase", m.phase, 0, 1);
      mr.enumeration("mode", kMoverNames, m.mode);
      if (!mr.ok) return false;
      a.mover = m;
    }
    if (const json::Value* x = c.find("city")) {
      CityComponent k;
      Reader kr{*x, error};
      kr.uinteger("seed", k.seed);
      kr.integer("blocks", k.blocks, 1, 16);
      kr.number("pitch", k.pitch, 30, 400);
      kr.number("street", k.street, 6, 60);
      kr.number("maxHeight", k.max_height, 6, 400);
      kr.boolean("trees", k.trees);
      kr.integer("traffic", k.traffic, 0, 200);
      kr.boolean("nightWindows", k.night_windows);
      if (!kr.ok) return false;
      if (k.street > k.pitch * 0.5f) k.street = k.pitch * 0.5f;
      a.city = k;
    }
    if (const json::Value* x = c.find("audio")) {
      AudioComponent k;
      Reader kr{*x, error};
      kr.string("sound", k.sound);
      kr.number("volume", k.volume, 0, 4);
      kr.number("pitch", k.pitch, 0.05, 8);
      kr.boolean("loop", k.loop);
      kr.boolean("autoplay", k.autoplay);
      kr.boolean("spatial", k.spatial);
      kr.number("minDistance", k.min_distance, 0.01, 1e5);
      kr.number("maxDistance", k.max_distance, 0.02, 1e5);
      if (const json::Value* ro = x->find("rolloff"); ro && (!ro->is_string() || !audio::parse_rolloff(ro->as_string(), k.rolloff))) kr.fail("rolloff: unknown value");
      if (const json::Value* bu = x->find("bus"); bu && (!bu->is_string() || !audio::parse_bus(bu->as_string(), k.bus) || k.bus == audio::Bus::master)) kr.fail("bus: unknown value");
      kr.number("doppler", k.doppler, 0, 4);
      kr.number("pitchBySpeed", k.pitch_by_speed, -10, 10);
      kr.number("delay", k.delay, 0, 1e5);
      if (!kr.ok) return false;
      if (k.max_distance <= k.min_distance) k.max_distance = k.min_distance + 0.01f;
      a.audio = k;
    }
    if (const json::Value* x = c.find("logic")) {
      logic::Graph g;
      if (!logic::graph_from_json(*x, g, error)) return false;
      a.logic = std::move(g);
    }
  }
  return r.ok;
}

json::Value world_to_json(const WorldSettings& w) {
  json::Value o = json::Value::object();
  o.set("gravity", vec(w.gravity));
  o.set("substeps", w.substeps);
  o.set("stepHz", num(w.step_hz));
  o.set("timeOfDay", num(w.time_of_day));
  o.set("dayLength", num(w.day_length));
  o.set("sunAzimuth", num(w.sun_azimuth));
  o.set("fogDensity", num(w.fog_density));
  o.set("exposure", num(w.exposure));
  o.set("clouds", w.clouds);
  o.set("ground", w.ground);
  o.set("groundColor", hex_color(w.ground_color));
  o.set("killZ", num(w.kill_z));
  o.set("title", w.title);
  o.set("goal", game_goal_name(w.goal));
  o.set("timeLimit", num(w.time_limit));
  o.set("scoreTarget", num(w.score_target));
  o.set("player", w.player);
  o.set("masterVolume", num(w.master_volume));
  o.set("music", w.music);
  o.set("musicVolume", num(w.music_volume));
  o.set("ambience", w.ambience);
  o.set("ambienceVolume", num(w.ambience_volume));
  o.set("reverb", num(w.reverb));
  o.set("doppler", num(w.doppler));
  return o;
}

bool world_from_json(const json::Value& v, WorldSettings& w, std::string* error) {
  if (!v.is_object()) return true;
  Reader r{v, error};
  r.vector("gravity", w.gravity);
  r.integer("substeps", w.substeps, 1, 16);
  r.number("stepHz", w.step_hz, 15, 480);
  r.number("timeOfDay", w.time_of_day, 0, 1);
  r.number("dayLength", w.day_length, 0, 1e6);
  r.number("sunAzimuth", w.sun_azimuth, -360, 720);
  r.number("fogDensity", w.fog_density, 0, 0.1);
  r.number("exposure", w.exposure, 0.05, 20);
  r.boolean("clouds", w.clouds);
  r.boolean("ground", w.ground);
  r.color("groundColor", w.ground_color);
  r.number("killZ", w.kill_z, -1e5, 1e5);
  r.string("title", w.title);
  r.enumeration("goal", kGoalNames, w.goal);
  r.number("timeLimit", w.time_limit, 0, 1e6);
  r.number("scoreTarget", w.score_target, 0, 1e9);
  r.uinteger("player", w.player);
  r.number("masterVolume", w.master_volume, 0, 4);
  r.string("music", w.music);
  r.number("musicVolume", w.music_volume, 0, 4);
  r.string("ambience", w.ambience);
  r.number("ambienceVolume", w.ambience_volume, 0, 4);
  r.number("reverb", w.reverb, 0, 1);
  r.number("doppler", w.doppler, 0, 4);
  if (w.time_of_day >= 1) w.time_of_day = 0;
  return r.ok;
}

json::Value to_json(const Level& level) {
  json::Value o = json::Value::object();
  o.set("format", "kern-level");
  o.set("version", 1);
  o.set("name", level.name);
  o.set("world", world_to_json(level.world));
  json::Value cam = json::Value::object();
  cam.set("position", vec(level.camera.position));
  cam.set("yaw", num(level.camera.yaw));
  cam.set("pitch", num(level.camera.pitch));
  o.set("camera", std::move(cam));
  o.set("nextId", level.next_id);
  json::Value actors = json::Value::array();
  for (const Actor& a : level.actors) actors.push(actor_to_json(a));
  o.set("actors", std::move(actors));
  json::Value tex = json::Value::array();
  for (const TextureAsset& t : level.textures) {
    json::Value x = json::Value::object();
    x.set("id", t.id);
    x.set("name", t.name);
    x.set("width", t.width);
    x.set("height", t.height);
    x.set("src", t.src ? *t.src : std::string());
    tex.push(std::move(x));
  }
  o.set("textures", std::move(tex));
  json::Value snd = json::Value::array();
  for (const SoundAsset& s : level.sounds) {
    json::Value x = json::Value::object();
    x.set("id", s.id);
    x.set("name", s.name);
    x.set("duration", num(s.duration));
    x.set("src", s.src ? *s.src : std::string());
    snd.push(std::move(x));
  }
  o.set("sounds", std::move(snd));
  if (!level.logic.empty()) o.set("logic", logic::graph_to_json(level.logic));
  return o;
}

u32 default_player(const Level& level) {
  if (const Actor* a = level.find(level.world.player); a && a->controller) return a->id;
  for (const Actor& a : level.actors)
    if (a.controller) return a.id;
  return 0;
}

std::string to_text(const Level& level, i32 indent) { return json::stringify(to_json(level), indent); }

bool from_json(const json::Value& doc, Level& out, std::string* error) {
  if (!doc.is_object()) { if (error) *error = "a level must be a JSON object"; return false; }
  if (doc["format"].as_string() != "kern-level") { if (error) *error = "not a Kern level (format must be \"kern-level\")"; return false; }
  if (doc["version"].as_number(0) > 1) { if (error) *error = "level was written by a newer Kern Studio"; return false; }
  Level l;
  Reader r{doc, error};
  r.string("name", l.name);
  if (!world_from_json(doc["world"], l.world, error)) return false;
  if (const json::Value* c = doc.find("camera")) {
    Reader cr{*c, error};
    cr.vector("position", l.camera.position);
    cr.number("yaw", l.camera.yaw, -1e4, 1e4);
    cr.number("pitch", l.camera.pitch, -90, 90);
    if (!cr.ok) return false;
  }
  r.uinteger("nextId", l.next_id);
  if (!r.ok) return false;
  if (const json::Value* g = doc.find("logic"); g && !g->is_null())
    if (!logic::graph_from_json(*g, l.logic, error)) return false;
  const json::Value& actors = doc["actors"];
  if (!actors.is_null() && !actors.is_array()) { if (error) *error = "actors must be an array"; return false; }
  if (actors.size() > 100000) { if (error) *error = "too many actors"; return false; }
  std::unordered_set<ActorId> ids;
  for (const json::Value& v : actors.items()) {
    Actor a;
    if (!actor_from_json(v, a, error)) {
      if (error) *error = "actor \"" + v["name"].as_string() + "\": " + *error;
      return false;
    }
    if (!a.id || ids.count(a.id)) a.id = 0;  // re-assigned by add()
    if (a.id) ids.insert(a.id);
    l.add(std::move(a));
  }
  for (Actor& a : l.actors)
    if (a.parent && (!l.find(a.parent) || a.parent == a.id)) a.parent = 0;
  // Break parent cycles.
  for (Actor& a : l.actors) {
    std::unordered_set<ActorId> seen{a.id};
    for (Actor* p = l.find(a.parent); p; p = l.find(p->parent)) {
      if (seen.count(p->id)) { a.parent = 0; break; }
      seen.insert(p->id);
    }
  }
  if (const json::Value* tex = doc.find("textures"); tex && tex->is_array())
    for (const json::Value& v : tex->items()) {
      TextureAsset t;
      t.id = v["id"].as_string();
      t.name = v["name"].as_string();
      t.src = std::make_shared<const std::string>(v["src"].as_string());
      t.width = static_cast<i32>(v["width"].as_number(0));
      t.height = static_cast<i32>(v["height"].as_number(0));
      if (t.id.empty() || t.src->rfind("data:image/", 0) != 0) continue;
      l.textures.push_back(std::move(t));
    }
  if (const json::Value* snd = doc.find("sounds"); snd && snd->is_array())
    for (const json::Value& v : snd->items()) {
      SoundAsset s;
      s.id = v["id"].as_string();
      s.name = v["name"].as_string();
      s.duration = static_cast<f32>(std::clamp(v["duration"].as_number(0), 0.0, 1e6));
      s.src = std::make_shared<const std::string>(v["src"].as_string());
      if (s.id.empty() || s.id.rfind("builtin:", 0) == 0 || (s.src->rfind("data:audio/", 0) != 0 && s.src->rfind("data:video/", 0) != 0 && s.src->rfind("data:application/ogg", 0) != 0)) continue;
      l.sounds.push_back(std::move(s));
    }
  out = std::move(l);
  return true;
}

bool from_text(std::string_view text, Level& out, std::string* error) {
  json::Value doc;
  if (!json::parse(text, doc, error)) return false;
  return from_json(doc, out, error);
}

bool sound_reference_ok(const Level& level, std::string_view ref) {
  if (ref.empty() || ref == "none" || ref == "auto") return true;
  if (ref.rfind("builtin:", 0) == 0) return audio::find_builtin(ref.substr(8)) != nullptr;
  return level.sound(ref) != nullptr;
}

std::vector<Issue> validate(const Level& level) {
  std::vector<Issue> out;
  std::unordered_set<ActorId> ids;
  u32 players = 0;
  for (const Actor& a : level.actors) {
    const std::string who = "\"" + (a.name.empty() ? std::string("actor ") + std::to_string(a.id) : a.name) + "\"";
    auto error = [&](const std::string& t) { out.push_back({true, a.id, who + ": " + t}); };
    auto warn = [&](const std::string& t) { out.push_back({false, a.id, who + ": " + t}); };
    if (!a.id || ids.count(a.id)) error("duplicate or missing id");
    ids.insert(a.id);
    if (a.parent && !level.find(a.parent)) error("parent does not exist");
    if (!finite(a.transform.position) || !finite(a.transform.rotation) || !finite(a.transform.scale)) error("transform is not finite");
    for (int i = 0; i < 3; ++i)
      if (std::fabs(a.transform.scale[i]) < 1e-4f) { error("scale must not be zero"); break; }
    for (const JointComponent& j : a.joints) {
      if (j.target == a.id) error("a joint cannot connect an actor to itself");
      else if (j.target && !level.find(j.target)) error("joint target does not exist");
      if (length(j.axis) < 1e-6f) error("joint axis must not be zero");
      if (!a.body) warn("a joint needs a body on its actor (it is ignored)");
      if (const Actor* t = j.target ? level.find(j.target) : nullptr) {
        const bool collides = t->body || level.body_owner(t->id) || !t->colliders.empty() || (t->mesh && t->mesh->collision);
        if (!collides) warn("joint target has neither a body nor collision (the joint is ignored)");
      }
    }
    auto sound = [&](std::string_view ref, const char* what) {
      if (!sound_reference_ok(level, ref)) warn(std::string(what) + " \"" + std::string(ref) + "\" does not exist (it is silent)");
    };
    if (a.body) sound(a.body->impact_sound, "impact sound");
    for (const JointComponent& j : a.joints) sound(j.break_sound, "joint break sound");
    if (a.controller) { sound(a.controller->engine_sound, "engine sound"); sound(a.controller->jump_sound, "jump sound"); sound(a.controller->boost_sound, "boost sound"); }
    if (a.weapon) { sound(a.weapon->fire_sound, "fire sound"); sound(a.weapon->reload_sound, "reload sound"); }
    if (a.health) { sound(a.health->hit_sound, "hit sound"); sound(a.health->death_sound, "down sound"); }
    if (a.weapon && !(a.controller && a.controller->mode == ControlMode::soldier)) warn("a weapon is only used by a Soldier player controller");
    if (a.pickup) sound(a.pickup->sound, "pickup sound");
    if (a.trigger) sound(a.trigger->sound, "trigger sound");
    if (a.audio) {
      sound(a.audio->sound, "audio source sound");
      if (a.audio->sound.empty() || a.audio->sound == "none" || a.audio->sound == "auto") warn("the audio source has no sound");
    }
    if (a.controller) ++players;
    if (a.controller && !a.body) error("a controller needs a body on the same actor");
    if (a.controller && a.body && a.body->motion != Motion::dynamic) warn("a controller only drives dynamic bodies");
    if (a.city && a.parent) error("a city must be a top-level actor");
    if ((a.pickup || a.trigger) && !a.colliders.empty()) {
      bool sensor = false;
      for (const ColliderComponent& c : a.colliders) sensor |= c.sensor;
      if (!sensor) warn("pickups and triggers react to sensor colliders; none is marked as sensor");
    }
  }
  for (const char* ref : {level.world.music.c_str(), level.world.ambience.c_str()})
    if (!sound_reference_ok(level, ref)) out.push_back({false, 0, std::string("world sound \"") + ref + "\" does not exist (it is silent)"});
  if (players > 1) out.push_back({false, 0, "more than one actor has a player controller; the first one is possessed"});
  if (level.world.goal == GameGoal::reach_goal) {
    bool goal = false;
    for (const Actor& a : level.actors) goal |= a.trigger && a.trigger->action == TriggerAction::goal;
    if (!goal) out.push_back({false, 0, "the win condition is \"reach goal\" but no trigger is a goal"});
  }
  return out;
}

}  // namespace kern::game
