// Internal types of the game runtime shared by runtime.cpp and soldier.cpp.
#pragma once

#include <cmath>

#include "kern/game/runtime.h"

namespace kern::game {

struct GameWorld::Visual {
  u32 mesh = 0;
  u64 key = 0;
  bool visible = true;
  Mat4 world;
};

struct GameWorld::SimBody {
  BodyPlan plan;
  physics::Body body;
  Vec3 scale{1, 1, 1};    // owner's world scale (render)
  bool animated = false;  // kinematic driven by rotator/mover
  physics::Pose prev;     // pose before the last fixed step (render interpolation)
  bool has_prev = false;
};

namespace rt {
constexpr f32 kDegree = 0.017453292519943295f;
inline f32 damp(f32 current, f32 target, f32 rate, f32 dt) { return target + (current - target) * std::exp(-rate * dt); }
inline f32 wrap180(f32 a) { a = std::fmod(a + 180.0f, 360.0f); if (a < 0) a += 360.0f; return a - 180.0f; }
/// Deterministic hash noise in [-1, 1].
inline f32 noise(u32 x) { x = hash_u32(x); return static_cast<f32>(x & 0xFFFFFF) / static_cast<f32>(0x7FFFFF) - 1.0f; }
}  // namespace rt

struct GameWorld::SimJoint {
  ActorId actor = 0, target = 0;
  physics::Joint joint;
  physics::Body a, b;
  physics::Pose frame_a, frame_b;
  JointKind kind = JointKind::hinge;
  std::size_t index = 0;          // into the actor's joints
  bool broken = false;
  // Hinge servo: the motor holds servo_target (radians) at servo_gain x error, servo_torque N*m.
  bool servo = false;
  f32 servo_target = 0, servo_gain = 18, servo_torque = 1;
};

class GameWorld::LogicHost : public logic::Host {
 public:
  explicit LogicHost(GameWorld& w) : w_(w) {}
  f64 time() const override { return w_.sim_time_; }
  void print(const std::string& text, bool screen) override {
    w_.events_.push_back("Logic: " + text);
    if (screen) w_.say(text);
  }
  void error(const std::string& text) override { w_.events_.push_back("Logic error: " + text); }
  u32 random() override {
    u32& s = w_.logic_seed_;
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
  }
  void* game() override { return &w_; }

 private:
  GameWorld& w_;
};

}  // namespace kern::game
