// The game runtime: presents a Level with the Kern renderer and plays it on
// Box3D. Kern Studio uses it for its viewports (editing, Simulate, Play) and
// exported games use it on its own.
//
//   GameWorld world(renderer, &night_windows);
//   world.sync(level);                       // meshes for the editor view
//   world.start(level, SimMode::play);       // Box3D world, controllers, rules
//   each frame: world.step(dt, input); world.update_camera(cam, dt, input); world.present(now_ms, eye);
//   world.stop();                            // back to the edited level
#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "kern/game/geometry.h"
#include "kern/game/level.h"
#include "kern/gfx/particles.h"
#include "kern/gfx/renderer.h"
#include "kern/physics/physics.h"

namespace kern::game {

/// Lit night windows on city buildings, driven by the buildings' wall records.
/// Register it with Renderer::init; GameWorld sets its night factor.
class NightWindows final : public ShaderExtension {
 public:
  f32 night = 0;
  const char* name() const override { return "kern-night-windows"; }
  Span<const ExtensionAttribute> attributes() const override;
  const char* vertex_declarations() const override;
  const char* vertex() const override;
  const char* fragment_declarations() const override;
  const char* fragment_surface() const override;
  void bind(const Program& p) override;
  void frame_uniforms(UniformCache& cache, const Renderer&) override;

 private:
  u32 u_night_ = 0;
};

// ---- body planning (shared by the simulation and the editor's collision view) ----

struct ShapePlan {
  ActorId actor = 0;              // actor that contributed the collider
  physics::ShapeKind kind = physics::ShapeKind::box;
  Vec3 center;                    // in the body frame
  Quat rotation;
  Vec3 size{1, 1, 1};
  f32 radius = 0.5f, length = 1;
  std::vector<Vec3> points;       // hull, body frame
  ColliderComponent source;       // material, filter, sensor
  bool automatic = false;         // fitted to a mesh
};

struct BodyPlan {
  ActorId owner = 0;              // actor that owns the body (0 = the city / ground)
  Motion motion = Motion::fixed;
  bool implicit = false;          // created for a mesh, pickup or trigger without a body component
  Vec3 position;                  // body frame in world space (no scale)
  Quat rotation;
  std::vector<ShapePlan> shapes;
};

/// The bodies and compound shapes a level simulates with: actors with a body
/// component own one, children without a body are welded into the nearest
/// ancestor's body, and mesh/pickup/trigger actors outside any body get an
/// implicit one (static, or kinematic when they are animated).
std::vector<BodyPlan> plan_bodies(const Level& level);

// ---- play state ----

struct PlayInput {
  f32 move_x = 0, move_y = 0, up = 0;  // -1..1 (right, forward, up)
  bool jump = false, boost = false;
  f32 look_x = 0, look_y = 0;          // camera orbit in degrees since the last frame
  f32 zoom = 0;                        // camera distance change (wheel)
};

enum class SimMode : u8 { none, simulate, play };
enum class GameStatus : u8 { inactive, playing, won, lost };
const char* game_status_name(GameStatus s);

struct Hud {
  GameStatus status = GameStatus::inactive;
  std::string title, message;
  f32 message_age = 0;            // seconds since the message appeared
  f32 score = 0;
  u32 pickups = 0, pickups_total = 0;
  f32 elapsed = 0, remaining = -1; // remaining < 0: no time limit
  f32 speed = 0;                  // m/s of the player
  f32 boost = -1;                 // 0..1, < 0: the controller has no boost
  f32 best = 0;                   // best finishing time of this session (0 = none)
};

struct SimStats {
  u32 bodies = 0, awake = 0, shapes = 0, joints = 0, broken_joints = 0, contacts = 0;
  u64 steps = 0;
};

class GameWorld {
 public:
  explicit GameWorld(Renderer& renderer, NightWindows* windows = nullptr);
  ~GameWorld();
  GameWorld(const GameWorld&) = delete;
  GameWorld& operator=(const GameWorld&) = delete;

  /// Renderer layer bits for the scene meshes (default 1).
  void set_layers(u32 layers);

  // ---- presentation ----
  /// Brings the renderer's meshes in line with `level` (only what changed is
  /// re-uploaded). Ignored while a simulation runs.
  void sync(const Level& level);
  void clear();
  /// The actor a renderer mesh belongs to (0 = none, e.g. the ground).
  ActorId actor_for_mesh(u32 mesh) const;
  /// Current world matrix of an actor (simulated pose while running).
  bool actor_matrix(ActorId id, Mat4& out) const;
  /// World bounds of an actor's meshes at the current pose.
  bool actor_bounds(ActorId id, Aabb& out) const;
  /// Renderer meshes of an actor (selection outline).
  void actor_meshes(ActorId id, std::vector<u32>& out) const;
  /// Texture assets that need pixels from the host (decoded images).
  std::vector<std::string> pending_textures() const;
  bool set_texture_pixels(const std::string& id, i32 width, i32 height, const u8* rgba);
  /// Lighting, sky, fog and point lights for the frame; returns the night factor.
  f32 apply_lighting(Vec3 eye);
  /// Time of day used for lighting (advances while simulating).
  f32 time_of_day() const { return time_of_day_; }
  /// Uploads the frame's poses, particles and city traffic.
  void present(f64 now_ms, Vec3 eye);

  // ---- simulation ----
  /// Builds the Box3D world from `level` and starts Simulate or Play.
  bool start(const Level& level, SimMode mode);
  void stop();
  bool running() const { return mode_ != SimMode::none; }
  SimMode mode() const { return mode_; }
  void set_paused(bool paused) { paused_ = paused; }
  bool paused() const { return paused_; }
  /// Switches between Simulate and Play while running (Play needs a player).
  bool set_mode(SimMode mode);
  /// Advances real time `dt` (fixed steps at the level's rate, at most 8 per call).
  void step(f32 dt, const PlayInput& input);
  /// Exactly one fixed step (also while paused).
  void step_once(const PlayInput& input = {});
  bool restart();
  /// Moves `camera` like the player's camera; false without a possessed player.
  bool update_camera(Camera& camera, f32 dt, const PlayInput& input);
  ActorId player() const { return player_; }
  const Hud& hud() const { return hud_; }
  SimStats stats() const;
  const Level& level() const { return level_; }  // the level being presented or simulated
  physics::World& physics() { return physics_; }
  /// The Box3D body that moves an actor (its own, an ancestor's or its implicit one).
  physics::Body actor_body(ActorId actor) const;

  /// Drags a dynamic body (Simulate): true when the actor under `point` can be grabbed.
  bool begin_grab(ActorId actor, Vec3 point);
  void move_grab(Vec3 target);
  void end_grab();
  bool grabbing() const { return grab_.active(); }

  /// Physics debug geometry as a line list (positions xyz, colours rgb per vertex).
  void debug_lines(bool colliders, bool contacts, bool joints, std::vector<f32>& positions, std::vector<f32>& colors) const;
  /// Messages of the last steps (pickups, joints breaking, ...); cleared by the caller.
  std::vector<std::string>& events() { return events_; }

 private:
  struct Visual;
  struct CityVisual;
  struct SimBody;
  struct SimJoint;
  struct Traffic;

  void sync_actor_mesh(const Actor& a, const Mat4& world);
  void sync_city(const Actor& a, const Mat4& world);
  void remove_visual(ActorId id);
  void sync_ground(const WorldSettings& w);
  Material material_for(const MaterialDesc& m, u32* texture) const;
  void compute_world_matrices(f32 time);
  Mat4 animated_local(const Actor& a, f32 time) const;
  void fixed_step(const PlayInput& input, f32 dt);
  void drive_player(const PlayInput& input, f32 dt);
  void drive_traffic(f32 dt);
  void handle_events();
  void collect(ActorId pickup);
  void trigger(ActorId actor);
  void finish(GameStatus status, std::string message);
  void say(std::string message);
  void respawn_player();
  bool grounded(physics::Body body, f32 reach) const;
  ActorId actor_for_body(physics::Body body) const;
  void set_actor_alive(ActorId id, bool alive);
  void burst(Vec3 at, Vec3 color);

  Renderer& r_;
  NightWindows* windows_ = nullptr;
  u32 layers_ = 1;

  // presentation
  Level level_;                  // edited level (sync) or the running copy (start)
  Level edit_level_;             // what sync() last saw (restored by stop())
  std::unordered_map<ActorId, std::unique_ptr<Visual>> visuals_;
  std::unordered_map<ActorId, std::unique_ptr<CityVisual>> cities_;
  std::unordered_map<u32, ActorId> mesh_owner_;
  std::unordered_map<ActorId, Mat4> world_;     // current world matrices
  struct TextureSlot { u32 handle = 0; bool requested = false, failed = false; };
  mutable std::unordered_map<std::string, TextureSlot> textures_;
  u32 ground_mesh_ = 0;
  u64 ground_key_ = 0;
  u32 traffic_mesh_ = 0;
  Particles particles_;
  bool particles_ready_ = false;
  LightPool lights_;
  f64 now_ms_ = 0;
  f32 time_of_day_ = 0.42f;

  // simulation
  SimMode mode_ = SimMode::none;
  bool paused_ = false;
  physics::World physics_;
  physics::MouseGrab grab_;
  std::vector<std::unique_ptr<SimBody>> bodies_;
  std::unordered_map<ActorId, SimBody*> body_of_actor_;   // owner -> body
  std::unordered_map<u64, ActorId> actor_of_body_;
  std::vector<std::unique_ptr<SimJoint>> joints_;
  std::vector<std::unique_ptr<Traffic>> traffic_;
  std::unordered_map<ActorId, bool> alive_;
  struct Respawn { ActorId id; f32 at; };
  std::vector<Respawn> respawns_;
  std::vector<ActorId> order_;   // actors parents-first
  f64 accumulator_ = 0;
  f32 sim_time_ = 0;
  ActorId player_ = 0;
  Vec3 spawn_position_;
  Quat spawn_rotation_;
  bool jump_latch_ = false;
  f32 boost_ = 1;
  f32 shake_ = 0;
  f32 heading_vis_ = 0, steer_vis_ = 0, pitch_vis_ = 0, last_forward_ = 0;
  Vec3 camera_eye_;
  f32 camera_yaw_ = 0, camera_pitch_ = 18, camera_zoom_ = 1;
  bool camera_reset_ = true;
  Hud hud_;
  u32 broken_joints_ = 0;
  std::vector<std::string> events_;
  PlayInput last_input_;
};

}  // namespace kern::game
