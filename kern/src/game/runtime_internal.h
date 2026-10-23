// Internal types of the game runtime shared by runtime.cpp and soldier.cpp.
#pragma once

#include <cmath>
#include <memory>

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
  // Hinge servo (JointComponent::servo, see GameWorld::drive_servos): target in radians,
  // stiffness N*m/rad, damping N*m*s/rad, friction and torque limit N*m.
  bool servo = false;
  f32 servo_target = 0, servo_stiffness = 0, servo_damping = 0, servo_friction = 0, servo_torque = 1;
};

/// A walker's controller state (walker.cpp): the Micro Duck's velstand policy and its servo bus.
struct GameWorld::WalkerBrain {
  ActorId actor = 0;
  physics::Body trunk;
  SimJoint* joint[14] = {};        // in the policy's joint order
  i32 decimation = 4, delay = 4;   // physics steps per control tick; command latency in steps
  u64 step = 0;                    // physics steps since start (the control clock)
  f32 action[14] = {};             // the last action (raw network output)
  f32 target[14] = {};             // joint targets of the last control tick (radians)
  f32 joint_speed[14] = {};        // joint speeds of the last control tick (real rad/s)
  f32 command[3] = {}, head[4] = {};  // smoothed twist (vx, vy, wz) and head command
  f32 quack = 0;                   // real seconds left of the quack nod
  std::vector<std::vector<f32>> pending;  // targets on their way to the servos, oldest first
  PlayInput input;
  bool has_input = false;
  static constexpr i32 kBodies = 16;
  physics::Body bodies[kBodies];   // trunk, the 14 links, the mouth: what its own sensors do not see
  SimJoint* mouth = nullptr;       // the mouth servo (robotd: not part of any policy)
  f32 mouth_open = 0;              // 0 closed (pressed shut) .. 1 open
  f32 idle_mouth = 0;              // the idle look-around's yawn
  WalkerSensors sensors;
  f64 next_tof = 0;                // simulation time of the next ToF frame
  f32 tilted_for = 0;              // real seconds the trunk has been tipped past the fall limit
  // Who drives (each control tick, in this order): the player's stick, then a Logic command, then
  // the autonomous brain; times are real seconds.
  bool autonomous = true;
  f32 manual_left = 0;             // the player touched the controls this recently
  f32 drive_left = 0, look_left = 0;  // Logic commands (< 0 = until the next one)
  f32 drive_cmd[3] = {}, look_cmd[2] = {};
  // The autonomous brain (walker_brain.cpp).
  struct Brain;
  std::shared_ptr<Brain> brain;
  // The idle look-around (robotd's idle_head.rs, walker_brain.cpp), on top of the head command.
  struct Idle;
  std::shared_ptr<Idle> idle;
  f32 head_age = 0;                // real seconds since something last steered the head
  f32 auto_cmd[3] = {}, auto_head[4] = {};
  std::string state = "";
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
