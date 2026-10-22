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

}  // namespace kern::game
