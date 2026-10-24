// Device models: what sits between a command and Box3D (docs/kalibrierung.md).
//
// Box3D stays as it is; what a real device does on top of the rigid-body physics is a small model
// with parameters in real units, kept in a profile. One servo type = one profile; another servo,
// motor or actuator = another profile (and, if its behaviour differs in kind, another model).
// Profiles are calibrated against measurements of the real device (bench logs, kern_lehrer
// --kalibrieren) and checked against held-out logs and whole-robot logs (--abgleich).
//
// The position servo (a Dynamixel XL330 and its kind) as Kern simulates it:
//   torque = clamp(K x (target - angle), -T, T) - D x speed, Coulomb friction F at the joint,
//   the rotor's inertia through the gearbox (armature A) on the joint axis, the command arriving
//   `delay` seconds after it was sent.
// All in real units; the walker scale (game::kWalkerScale) is applied at the Box3D boundary.
#pragma once

#include <string>
#include <string_view>

#include "kern/core/base.h"
#include "kern/game/level.h"
#include "kern/physics/physics.h"

namespace kern::game {

struct ServoProfile {
  std::string name = "xl330";
  std::string source;            // where the values come from (a model, a calibration, a log)
  f32 stiffness = 0;             // K, N*m/rad
  f32 damping = 0;               // D, N*m*s/rad (back-EMF + viscous)
  f32 friction = 0;              // F, N*m (Coulomb, at the joint)
  f32 torque = 0;                // T, N*m: the most the servo pushes (stall torque at its voltage)
  f32 armature = 0;              // A, kg*m^2: rotor inertia through the gearbox, on the joint axis
  f32 delay = 0;                 // s: command -> servo
  // How sure the values are: training draws each servo's values within these (relative +-, and a
  // delay range), so that the policy does not depend on what the measurement could not pin down.
  f32 spread_stiffness = 0, spread_damping = 0, spread_friction = 0, spread_torque = 0, spread_armature = 0;
  f32 delay_min = 0, delay_max = 0;
};

/// Plumo's XL330-M288 as Pollen trains it (BAM "m6" model at 7.4 V, docs/walker.md
/// section 4; delay and spreads as in microduck_rl's randomisation).
ServoProfile xl330_profile();

std::string to_json(const ServoProfile& p);
bool servo_profile_from_json(std::string_view text, ServoProfile& out, std::string* error = nullptr);

/// The servo law for one step (GameWorld::drive_servos and the calibration rig use this one
/// function): Box3D's joint spring toward the target, the error limited so that K x error never
/// exceeds T, and the friction as a motor at speed 0 with F as its maximum torque. Values in the
/// world's units (scaled).
void servo_step(physics::World& world, const physics::Joint& joint, f32 target, f32 stiffness, f32 damping, f32 friction, f32 torque);

/// Gives Plumo `trunk` in `level` the servos of `profile` (all its joints, the rotor
/// inertia on each link, the delay). Its servos are built from xl330_profile() by add_plumo.
void set_plumo_servos(Level& level, ActorId trunk, const ServoProfile& profile);
/// One servo of it: `link` 1..14 the policy's joints in their order, 15 the mouth (no delay: the
/// delay belongs to the bus and is set by set_plumo_servos).
void set_plumo_servo(Level& level, ActorId trunk, i32 link, const ServoProfile& profile);

}  // namespace kern::game
