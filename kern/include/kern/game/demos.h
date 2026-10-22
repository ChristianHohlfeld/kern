// Built-in levels: templates for File > New and the example scenes.
#pragma once

#include "kern/game/level.h"

namespace kern::game {

/// Orb Run as a level: procedural city with traffic, a Box3D hover car with a
/// chase camera, 24 energy orbs to collect, day turning into night.
Level orb_run_level();
/// Every joint type and body motion in one place: stacks, a pendulum, a
/// hinged door, a breakable rope bridge, a motorised turntable, a slider
/// lift, a self-driving car on wheel joints, and a rolling ball to play with.
Level physics_lab_level();
/// A floor, daylight and nothing else.
Level basic_level();
/// Nothing at all.
Level empty_level();

}  // namespace kern::game
