// Scene runtime: turns a scene document into a running Box3D world and plays
// its game components. Used by the editor's player views, by exported games
// and by C++ games that load .e3d scenes.
//
//   world::SceneRuntime rt;
//   if (!rt.build(scene)) log("%s", rt.error().c_str());
//   rt.set_input(input);          // optional: game controls
//   rt.advance(frame_dt);         // fixed steps at scene.world.steps_per_second
//   rt.body_pose(body_id, pose);  // draw with the current poses
#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "kern/physics/physics.h"
#include "kern/world/scene.h"

namespace kern::world {

/// Control input for game components (keyboard / touch mapped by the host).
struct GameInput {
  f32 move_x = 0, move_y = 0;  // -1..1: right / forward relative to the camera
  f32 up = 0;                  // -1..1: fly up / down
  bool jump = false;           // edge-triggered by the runtime
  Vec3 camera_forward{0, 1, 0};  // horizontal camera direction (for camera-relative moves)
};

enum class GameStatus : u8 { inactive, playing, won, lost };

struct GameState {
  GameStatus status = GameStatus::inactive;
  f64 elapsed = 0;   // seconds of play
  f64 score = 0;
  u32 pickups_total = 0, pickups_collected = 0;
  std::string message;  // "You win!", "Fell off the world", ...
  std::string player;   // body id of the (first) player, "" without players
  std::string camera_body;  // body the camera follows ("" = free camera)
};

class SceneRuntime {
 public:
  SceneRuntime() = default;
  SceneRuntime(const SceneRuntime&) = delete;
  SceneRuntime& operator=(const SceneRuntime&) = delete;

  /// Validates and instantiates `scene` (copied). On failure error() explains why.
  bool build(const Scene& scene);
  void destroy();
  bool valid() const { return world_.valid(); }
  const std::string& error() const { return error_; }
  const Scene& source() const { return source_; }

  /// Advances real time `dt` (seconds, clamped to 0.1) with fixed steps; returns the steps taken (max 240).
  u32 advance(f64 dt);
  /// Exactly one fixed step.
  void step_once();
  u64 steps() const { return world_.step_count(); }
  f64 step_seconds() const { return 1.0 / source_.world.steps_per_second; }

  /// Current pose of a scene body (false when unknown or destroyed, e.g. a collected pickup).
  bool body_pose(const std::string& body_id, DPose& out) const;
  bool body_alive(const std::string& body_id) const;
  physics::Body physics_body(const std::string& body_id) const;
  /// Scene body id of a physics body ("" when it is not a scene body, e.g. the grab helper).
  const std::string& body_id(physics::Body body) const;
  physics::World& world() { return world_; }
  const physics::World& world() const { return world_; }

  // ---- mouse grab (dynamic bodies) ----
  bool begin_grab(const std::string& body_id, Vec3 point);
  void set_grab_target(Vec3 target) { grab_.set_target(target); }
  void end_grab() { grab_.end(); }
  bool grabbing() const { return grab_.active(); }
  Vec3 grab_target() const { return grab_.target(); }

  // ---- game components ----
  void set_input(const GameInput& input);
  const GameState& game() const { return game_; }
  /// Rebuilds from the source scene (restart after win / loss).
  bool restart();

 private:
  bool create_joint(const Joint& j);
  void game_begin();
  void game_pre_step(f32 dt);
  void game_post_step(f32 dt);
  bool grounded(physics::Body body, f32 probe) const;
  void finish(GameStatus status, std::string message);

  Scene source_;
  physics::World world_;
  physics::MouseGrab grab_;
  std::unordered_map<std::string, physics::Body> bodies_;
  std::unordered_map<u64, std::string> names_;
  std::vector<std::string> scene_order_;
  f64 accumulator_ = 0;
  std::string error_;
  GameState game_;
  GameInput input_;
  bool jump_latch_ = false;
  bool has_goal_ = false;
  std::vector<std::string> pickups_alive_;
};

/// Box3D joint settings for an editor joint (anchors/axes relative to the bodies).
physics::JointSettings joint_settings(const Joint& j, physics::Body a, physics::Body b);
/// Box3D shape settings for a fixture (hull points are written to `points_storage`).
physics::ShapeSettings shape_settings(const Fixture& f, std::vector<Vec3>& points_storage);
/// Box3D body settings for a scene body.
physics::BodySettings body_settings(const Body& b);

}  // namespace kern::world
