// Internal types of the game runtime shared by runtime.cpp and soldier.cpp.
#pragma once

#include <unordered_map>
#include <cmath>
#include "kern/core/dmath.h"
#include <memory>

#include "kern/game/runtime.h"
#include "kern/game/walker_policy.h"

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
inline f32 damp(f32 current, f32 target, f32 rate, f32 dt) { return target + (current - target) * kern::dm::exp(-rate * dt); }
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
  bool external = false;           // set_walker_external: the Lehrer's action drives
  f32 external_action[14] = {};
  f32 last_obs[61] = {};           // what the controller saw at the last tick
  f32 target[14] = {};             // joint targets of the last control tick (radians)
  f32 joint_speed[14] = {};        // joint speeds of the last control tick (real rad/s)
  f32 command[3] = {}, head[4] = {};  // smoothed twist (vx, vy, wz) and head command
  f32 beak_open = 0;               // real seconds the beak stays open for a call
  std::vector<std::vector<f32>> pending;  // targets on their way to the servos, oldest first
  PlayInput input;
  bool has_input = false;
  static constexpr i32 kBodies = 16;
  physics::Body bodies[kBodies];   // trunk, the 14 links, the mouth: what its own sensors do not see
  SimJoint* mouth = nullptr;       // the mouth servo (robotd: not part of any policy)
  f32 mouth_open = 0;              // 0 closed (pressed shut) .. 1 open
  f32 servo_strain = 0, servo_drive = 0, servo_speed = 0;  // walker_servo_load, each control tick
  f32 idle_mouth = 0;              // the idle look-around's yawn
  // robotd's controller (walker.cpp): which network drives and its windows; real seconds.
  bool rollers = false;            // roller skates: roller.onnx walks, the crouch is the pick
  bool alpha_gait = false;
  bool robot_actions = false;
  bool kern_extras = false;        // ControllerComponent::kern_extras      // the daemon's action scale and low-pass (ControllerComponent::actions)         // alpha_walking + alpha_stand instead of velstand
  enum class Sit : u8 { up, sitting, rising } sit = Sit::up;
  f32 sit_remaining = 0, sit_settle = 0;
  f32 pick_phase = -1;             // ground pick / crouch phase, < 0 when none runs
  i32 skill = -1;                  // the one-shot skill running (index into the set), -1 none
  bool skill_unwinding = false;
  f32 skill_remaining = 0, skill_chain = 0;
  f32 previous_target[14] = {};    // the low-pass anchor
  bool has_previous = false;
  u32 robot_held = 0;              // robot buttons held last tick (edges start moves)
  std::string driving = "walk";
  // The voice (robotd sound.rs): this duck's own seed, the wheee ride, what it said last.
  u32 voice_seed = 0, voice_rng = 1, wheee_variant = 0;
  enum class Wheee : u8 { off, start, loop } wheee = Wheee::off;
  f32 wheee_left = 0, beak_trigger = 0;
  bool greeted = false, pad_was = false, chirp_held = false;
  std::string said;
  u32 warm_index = 0;              // warm_walker_voices: the next call to synthesise ahead
  WalkerSensors sensors;
  f64 next_tof = 0;                // simulation time of the next ToF frame
  f32 tilted_for = 0;              // real seconds the trunk has been tipped past the fall limit
  // Who drives (each control tick, in this order): the player's stick, then a Logic command, then
  // the autonomous brain; times are real seconds.
  bool autonomous = true;
  f32 manual_left = 0;             // the player touched the controls this recently
  f32 drive_left = 0, look_left = 0;  // Logic commands (< 0 = until the next one)
  f32 drive_cmd[3] = {}, look_cmd[2] = {}, look_neck = 0;
  // The autonomous brain (walker_brain.cpp).
  struct Brain;
  std::shared_ptr<Brain> brain;
  // Kern's play layer: the learned network for ball and sock (walker_play.cpp).
  struct Play;
  std::shared_ptr<Play> play;
  // The idle look-around (robotd's idle_head.rs, walker_brain.cpp), on top of the head command.
  struct Idle;
  std::shared_ptr<Idle> idle;
  f32 head_age = 0;                // real seconds since something last steered the head
  f32 auto_cmd[3] = {}, auto_head[4] = {};
  f32 auto_head_alpha = 0.2f;            // the head's slew rate the brain asks for (autod: robot.head alpha)
  f32 auto_body[3] = {};                 // body pose z, roll, pitch (real m, rad), while auto_body_active
  bool auto_body_active = false, auto_wheee = false;
  f32 auto_mouth = 0;
  f32 body[3] = {};                      // the smoothed body pose command (observation 57..59)
  f64 petted_until = -1;                 // walker_pet: a hand on it until this simulation time
  i32 heard = 0;                         // walker_hear: 1 a noise, 2 a voice, this tick
  std::string state = "";
  // robotd's pick-up watch (robotd pickup.rs, duck-control pickup.rs): the detector's window (the
  // last second of feature rows, a ring), its latch and the pause ramp; real seconds.
  struct Pickup {
    f32 rows[kPickupWindow * kPickupFeatures] = {};
    i32 next = 0, filled = 0, run = 0;
    bool has_previous = false, previous_paused = false, paused = false, was_paused = false;
    f32 previous_target[14] = {}, from[14] = {};
    f32 paused_for = 0, p_held = -1;
  } pickup;
};

/// Kern's play layer's state (walker_play.cpp).
struct GameWorld::WalkerBrain::Play {
  struct Track {
    bool known = false;
    ActorId actor = 0;
    f32 x = 0, y = 0;   // world, real metres
    f32 age = 99;       // real seconds since the detector last reported it
  };
  Track track[2];       // 0 ball, 1 sock
  std::unordered_map<ActorId, f32> rest;  // habituation: seconds left
  bool external = false;
  f32 external_action[GameWorld::kPlayActions] = {};
  f32 action[GameWorld::kPlayActions] = {};  // the last decision
  i64 tick = 0;                              // control ticks since start (a decision every kPlayDecisionTicks)
  bool engaged = false, started_move = false, was_holding = false;
  f32 carry_from[2] = {0, 0}, carry_time = 0;
  f32 still = 0;                             // real seconds its walking command has been zero
  f32 walking = 0;                           // real seconds it has been told forward (+) / back (-)
  ActorId carried = 0;
  DetectorModel detector;
  u64 rng = 1;
  f32 uniform() {  // the detector's noise: deterministic (xorshift64)
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return static_cast<f32>(rng >> 40) * (1.0f / 16777216.0f);
  }
  f32 normal() {
    const f32 a = max_of(1e-7f, uniform()), b = uniform();
    return std::sqrt(-2 * kern::dm::log(a)) * kern::dm::cos(6.2831853f * b);
  }
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
