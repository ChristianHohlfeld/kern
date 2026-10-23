// Device models (servo.h): profiles as JSON and the position servo's law.
#include "kern/game/servo.h"

#include <cmath>

#include "kern/core/json.h"

namespace kern::game {

namespace {

struct Field { const char* key; f32 ServoProfile::*member; f32 lo, hi; };
constexpr Field kFields[] = {
    {"stiffness", &ServoProfile::stiffness, 0, 1e6f},          {"damping", &ServoProfile::damping, 0, 1e6f},
    {"friction", &ServoProfile::friction, 0, 1e6f},            {"torque", &ServoProfile::torque, 0, 1e6f},
    {"armature", &ServoProfile::armature, 0, 1e3f},            {"delay", &ServoProfile::delay, 0, 1},
    {"spreadStiffness", &ServoProfile::spread_stiffness, 0, 1}, {"spreadDamping", &ServoProfile::spread_damping, 0, 1},
    {"spreadFriction", &ServoProfile::spread_friction, 0, 1},   {"spreadTorque", &ServoProfile::spread_torque, 0, 1},
    {"spreadArmature", &ServoProfile::spread_armature, 0, 1},   {"delayMin", &ServoProfile::delay_min, 0, 1},
    {"delayMax", &ServoProfile::delay_max, 0, 1},
};

}  // namespace

std::string to_json(const ServoProfile& p) {
  json::Value o = json::Value::object();
  o.set("kind", std::string("servo"));
  o.set("name", p.name);
  if (!p.source.empty()) o.set("source", p.source);
  for (const Field& f : kFields) o.set(f.key, static_cast<f64>(p.*(f.member)));
  return json::stringify(o, 2) + "\n";
}

bool servo_profile_from_json(std::string_view text, ServoProfile& out, std::string* error) {
  json::Value v;
  if (!json::parse(text, v, error)) return false;
  if (!v.is_object() || (v.has("kind") && v["kind"].as_string() != "servo")) {
    if (error) *error = "not a servo profile";
    return false;
  }
  ServoProfile p;
  if (const json::Value* n = v.find("name"); n && n->is_string()) p.name = n->as_string();
  if (const json::Value* s = v.find("source"); s && s->is_string()) p.source = s->as_string();
  for (const Field& f : kFields) {
    const json::Value* x = v.find(f.key);
    if (!x) continue;
    const f64 d = x->as_number(-1);
    if (!x->is_number() || !(d >= f.lo && d <= f.hi)) {
      if (error) *error = std::string("bad value for ") + f.key;
      return false;
    }
    p.*(f.member) = static_cast<f32>(d);
  }
  if (p.stiffness <= 0 || p.torque <= 0) {
    if (error) *error = "stiffness and torque must be positive";
    return false;
  }
  if (p.delay_max < p.delay_min) p.delay_max = p.delay_min;
  out = std::move(p);
  return true;
}

void servo_step(physics::World& world, const physics::Joint& joint, f32 target, f32 stiffness, f32 damping, f32 friction, f32 torque) {
  // A position servo from Box3D's own joint parts (docs/physics.md section 5):
  // * torque K x (target - angle) - D x speed: the revolute joint's spring, solved implicitly by
  //   Box3D's soft step, so it is stable at any stiffness. Box3D sets springs in Hertz relative to
  //   the joint's effective mass; set_joint_spring converts the servo's N*m/rad once, at the boundary.
  // * the stall torque: the error is limited so that K x error never exceeds it (a Dynamixel's
  //   duty cycle saturates the same way).
  // * friction: a motor at speed 0 whose maximum torque is the friction (Erin's joint friction).
  const f32 q = world.joint_angle(joint);
  const f32 k = max_of(1e-6f, stiffness);
  const f32 reach = torque > 0 ? torque / k : 1e9f;
  world.set_joint_spring(joint, q + clamp(target - q, -reach, reach), k, max_of(0.0f, damping));
  world.set_joint_motor(joint, friction > 0, 0, max_of(0.0f, friction));
}

}  // namespace kern::game
