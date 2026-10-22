// Walkers: articulated robots on hinge servos (see src/game/walker.cpp).
#pragma once

#include <vector>

#include "kern/game/level.h"

namespace kern::game {

/// Micro Duck (Pollen Robotics' biped, from its MJCF) as actors in the catalog's convention:
/// [0] is the trunk (a walker controller); `parent` and joint targets are list index + 1;
/// positions are relative to the parent.
std::vector<Actor> micro_duck_actors();

}  // namespace kern::game
