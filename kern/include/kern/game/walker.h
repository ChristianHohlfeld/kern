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

/// Micro Duck (Pollen Robotics' biped, from the MJCF its walking policy was trained on) as actors
/// in the catalog's convention: [0] is the trunk (a walker controller); `parent` and joint targets
/// are list index + 1; positions are relative to the parent.
std::vector<Actor> micro_duck_actors();

/// The velstand policy (Pollen Robotics' trained network): 61 observations -> 14 joint actions.
/// Exposed for tests; the runtime calls it from GameWorld::drive_walkers.
void micro_duck_policy(const f32 observation[61], f32 action[14]);

}  // namespace kern::game
