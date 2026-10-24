// Walkers: articulated robots on hinge servos, driven by a control policy (see src/game/walker.cpp
// and docs/walker.md).
#pragma once

#include <vector>

#include "kern/game/level.h"

namespace kern::game {

/// The fixed physics rate a walker's controller is built for: the Micro Duck was trained at 200 Hz
/// physics / 50 Hz control in real size; Kern builds it 4x larger (time runs 2x slower), so 100 Hz
/// and a policy step every 4 physics steps reproduce the training timing exactly.
constexpr f32 kWalkerStepHz = 100;
/// Box3D sub-steps per fixed step for a walker: its servo chains need more than the default to keep
/// the joints from stretching (docs/physics.md section 3, docs/walker.md section 3).
constexpr i32 kWalkerSubsteps = 16;
/// The Micro Duck is built this many times its real size (Box3D's size range, docs/walker.md).
constexpr f32 kWalkerScale = 4;

/// Micro Duck (Pollen Robotics' biped, from the MJCF its walking policy was trained on) as actors
/// in the catalog's convention: [0] is the trunk (a walker controller); `parent` and joint targets
/// are list index + 1; positions are relative to the parent.
/// rollers: wearing the roller skates (rollers model: blades and four free wheels).
std::vector<Actor> micro_duck_actors(bool rollers = false);
/// Adds a Micro Duck to `level` standing at `at` (its trunk on the floor plane at z = 0 when at.z is
/// 0) and sets the level's step rate for it; returns the trunk's id.
ActorId add_micro_duck(Level& level, Vec3 at, bool rollers = false);
/// Kern's jump (trained in Kern, docs/lehrer.md): one jump in kJumpSeconds. Its network sees the
/// move's phase t / kJumpSeconds as cos and sin in observation slots kJumpPhaseSlot and the next (the
/// body command's x and y, which Pollen's networks never use); the rest of the command is 0.
constexpr f32 kJumpSeconds = 2.0f;
constexpr i32 kJumpPhaseSlot = 55;

/// A variation of the Micro Duck's build for training (domain randomisation as in Pollen's recipes):
/// trunk mass and inertia scaled together, the trunk's and the head's centre of mass moved (real
/// metres). The servos vary through their device profile (servo.h: set_micro_duck_servo).
struct DuckVariation {
  f32 trunk_mass = 1;
  Vec3 trunk_com, head_com;
};
/// Applies `v` to the duck whose trunk is `trunk` in `level` (as add_micro_duck laid it out).
void vary_micro_duck(Level& level, ActorId trunk, const DuckVariation& v);
/// A crumpled sock for the duck to pick up (seven lumps of fabric on soft springs, tagged "sock"),
/// in the same convention.
std::vector<Actor> micro_duck_sock_actors();

/// The velstand policy (Pollen Robotics' trained network): 61 observations -> 14 joint actions.
/// Exposed for tests; the runtime calls it from GameWorld::drive_walkers.
void micro_duck_policy(const f32 observation[61], f32 action[14]);

struct WalkerPolicy;
/// velstand.onnx as built into the engine (walker_policy("velstand.onnx") returns it).
WalkerPolicy builtin_velstand_policy();

}  // namespace kern::game
