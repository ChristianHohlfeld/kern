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

#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
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
  std::vector<Vec3> points;       // hull / mesh vertices, body frame
  std::vector<u32> indices;       // mesh triangles
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
  // Remote players: the direction their camera looks (degrees) for camera-relative movement.
  f32 view_yaw = 0;
  bool has_view_yaw = false;
  f32 view_pitch = 0;                  // degrees, positive looks down (soldier aim)
  // soldier
  bool fire = false, aim = false, reload = false, crouch = false, interact = false;
  f32 look_stick_x = 0, look_stick_y = 0;  // gamepad right stick (-1..1), turned into look by the camera
  bool gamepad = false;                // the input came from a pad (aim assist, response curves)
  f32 rewind = 0;                      // server: seconds the shooter saw the world in the past (lag compensation)
};

/// replica: a network client's copy of a server's game (poses come from snapshots, see net_game.h).
enum class SimMode : u8 { none, simulate, play, replica };
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
  // soldier (first-person shooter) state of the local player
  bool soldier = false;
  f32 health = -1, max_health = 100, armor = 0;  // health < 0: no health
  i32 ammo = -1, magazine = 0, reserve = 0;      // ammo < 0: no weapon
  f32 reload = -1;                // 0..1 while reloading
  f32 spread = 0;                 // current spread in degrees (crosshair size)
  f32 hit_age = 99;               // seconds since our last hit (hit marker)
  bool hit_head = false, hit_kill = false;
  f32 damage_age = 99;            // seconds since we took damage (vignette)
  f32 damage_yaw = 0;             // direction the damage came from, relative to the view (degrees)
  f32 respawn_in = -1;            // seconds until respawn while dead
  bool aiming = false;
  f32 fov = 60;                   // current vertical field of view (degrees)
};

/// A hitscan shot for presentation (tracer, muzzle flash, impact).
struct SoldierArt;  // soldier_art.cpp
struct SoldierArtDelete { void operator()(SoldierArt* a) const; };

struct ShotEvent {
  Vec3 from, to;
  ActorId shooter = 0, hit = 0;
  bool impact = false, head = false;
  Vec3 normal{0, 0, 1};           // surface normal at `to` (impacts)
  u8 surface = 0;                 // 0 nothing, 1 static world, 2 moving body, 3 flesh (no decal)
};

/// A sound the simulation asks for (GameAudio plays it). Cues are produced in
/// fixed steps and so are deterministic; the caller drains them every frame.
struct SoundCue {
  std::string sound;              // "builtin:<name>" or an imported sound id
  Vec3 position;
  f32 volume = 1, pitch = 1;
  bool spatial = true;
  audio::Bus bus = audio::Bus::sfx;
  ActorId actor = 0;
};

/// What a network client needs to show its own soldier's HUD (sent by the server per client).
struct SoldierState {
  f32 health = 100, max_health = 100, armor = 0;
  i32 ammo = 0, magazine = 0, reserve = 0;
  f32 reload = -1, spread = 0, respawn_in = -1;
  u8 hits = 0, damages = 0;        // counters: a change shows the hit marker / damage vignette
  bool hit_head = false, hit_kill = false, dead = false;
  f32 damage_yaw = 0;              // world yaw (degrees) the last damage came from
};

struct SimStats {
  u32 bodies = 0, awake = 0, shapes = 0, joints = 0, broken_joints = 0, contacts = 0;
  u64 steps = 0;
};

/// The gameplay nodes of Kern Logic are registered (always true; links logic_nodes.cpp).
bool logic_nodes_registered();

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
  /// Counts start() calls (restart included): listeners notice a fresh run.
  u32 session() const { return session_; }
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
  f32 camera_yaw() const { return camera_yaw_; }
  f32 camera_pitch() const { return camera_pitch_; }
  f32 sim_time() const { return sim_time_; }

  // ---- players (multiplayer) ----
  /// Adds a player pawn: a copy of `source` (0 = the level's player actor) with its children,
  /// placed next to the spawn. `id` forces the root's id (network clients mirror the server);
  /// children follow with consecutive ids. Returns the pawn's root actor (0 on failure).
  ActorId add_pawn(ActorId source = 0, ActorId id = 0);
  bool remove_pawn(ActorId pawn);
  /// The actor a pawn was copied from (the pawn itself for the level's own player).
  ActorId pawn_source(ActorId pawn) const;
  /// Input of a remote player's pawn (used in the next fixed steps).
  void set_pawn_input(ActorId pawn, const PlayInput& input);
  /// The pawn this machine controls and the camera follows.
  void set_local_pawn(ActorId pawn);
  std::vector<ActorId> pawns() const;
  /// Collected pickups / used triggers since the last call (appended as id, alive).
  void take_alive_changes(std::vector<std::pair<ActorId, bool>>& out);
  std::vector<ActorId> dead_actors() const;
  void set_alive(ActorId id, bool alive);
  /// Actors whose pose changes while running (dynamic and kinematic bodies).
  void networked_actors(std::vector<ActorId>& out) const;
  Hud& hud_mut() { return hud_; }

  // ---- replica (network client) ----
  /// Starts `level` as a replica: bodies only follow the poses set here, rules run on the server.
  bool start_replica(const Level& level);
  void set_replica_pose(ActorId id, Vec3 position, Quat rotation);
  /// Advances the replica's clock (`time` = the server time being shown).
  void step_replica(f32 dt, f32 time, const PlayInput& input);
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
  /// Sounds of the last steps (jumps, pickups, impacts, ...); cleared by the caller.
  std::vector<SoundCue>& sound_cues() { return cues_; }
  /// Shots of the last steps (soldiers); presented and then cleared by present().
  std::vector<ShotEvent>& shots() { return shots_; }
  /// Applies damage (server / standalone): armor first, death, respawn, score for `attacker`.
  void damage(ActorId target, f32 amount, ActorId attacker, bool head, Vec3 from);
  /// Health of an actor (-1 = no health). Network replicas set what the server sends.
  f32 health_of(ActorId id) const;
  void set_health(ActorId id, f32 health, f32 armor, bool dead);
  /// Server: HUD state of a soldier pawn for its client. False if the pawn is no soldier.
  bool soldier_state(ActorId pawn, SoldierState& out) const;
  /// Client: applies the server's state of this machine's soldier to the HUD.
  void apply_soldier_state(const SoldierState& s);
  /// The camera the frame is drawn with (view model of a first-person player).
  void set_view(const Camera& camera) { view_camera_ = camera; view_valid_ = true; }
  /// False for collected pickups and used triggers (and their children).
  bool actor_alive(ActorId id) const;
  const PlayInput& last_input() const { return last_input_; }

  // ---- logic (Kern Logic graphs: the level's and the actors') ----
  /// The running graphs (Play and Simulate on the authority; null otherwise).
  logic::Runtime* logic_runtime() { return logic_.get(); }
  /// A key of this machine's keyboard while playing ("KeyF", "Digit1"...): On Key events.
  void key_event(const std::string& code, bool down);
  /// Replicated changes made by logic (the network server sends them; clients apply them).
  struct PropertyChange { ActorId actor = 0; std::string path; json::Value value; };
  struct SpawnChange { ActorId source = 0, id = 0; Vec3 position, rotation; };
  void take_replicated_changes(std::vector<PropertyChange>& props, std::vector<SpawnChange>& spawns);
  /// Everything changed so far (for players who join later).
  void replicated_state(std::vector<PropertyChange>& props, std::vector<SpawnChange>& spawns) const;
  /// Sets a component property of a running actor by JSON path ("components.light.color").
  bool set_property(ActorId id, const std::string& path, const json::Value& value);
  /// A copy of `source` (with children and logic) at a place; `id` forces the root id (replicas).
  ActorId spawn_copy(ActorId source, Vec3 position, Vec3 rotation_deg, ActorId id = 0);
  bool teleport(ActorId id, Vec3 position, const Vec3* rotation_deg = nullptr);
  Vec3 actor_velocity(ActorId id) const;
  bool set_actor_velocity(ActorId id, Vec3 velocity);
  bool push_actor(ActorId id, Vec3 amount, bool impulse);  // impulse (N*s) or force for one step (N)
  bool set_simulate_physics(ActorId id, bool simulate);
  bool set_joint_motor(ActorId id, i32 joint, bool enable, f32 speed, f32 max_force);
  ActorId find_actor(std::string_view name) const;
  void actors_with_tag(std::string_view tag, std::vector<ActorId>& out) const;
  bool has_tag(ActorId id, std::string_view tag) const;
  /// Closest hit between two points (sensors ignored): actor, point, normal.
  bool line_trace(Vec3 from, Vec3 to, ActorId ignore, ActorId& actor, Vec3& point, Vec3& normal);
  void add_score(f32 points);
  void end_game(bool won, const std::string& message);
  void show_message(const std::string& message) { say(message); }
  void play_sound(const std::string& sound, Vec3 at, f32 volume, f32 pitch, bool spatial, ActorId actor = 0) { cue(sound, at, volume, pitch, spatial, spatial ? audio::Bus::sfx : audio::Bus::ui, actor); }
  void particle_burst(Vec3 at, Vec3 color) { burst(at, color); }
  void shake_camera(f32 amount) { shake_ = max_of(shake_, amount); shake_energy_ = min_of(1.35f, shake_energy_ + amount); }
  void destroy_actor(ActorId id);
  void heal(ActorId id, f32 amount);
  void give_ammo(ActorId pawn, i32 rounds);
  /// Calls a logic Custom Event by name (Studio console, other systems).
  void call_logic_event(const std::string& name, ActorId target = 0);

  // ---- walkers (articulated robots on their policies; src/game/walker.cpp) ----
  /// What a walker's sensors report, like the real Micro Duck's: the head ToF (VL53L8CX, 8 x 8
  /// zones over 45 x 45 degrees) classified as the robot does (kinematics/src/tof.rs), the trunk IMU.
  struct WalkerSensors {
    static constexpr int kZones = 64;
    f32 range[kZones] = {};           // slant range per zone, metres in the robot's own scale; < 0 = no return
    u8 zone[kZones] = {};             // 0 empty, 1 too close, 2 floor, 3 obstacle
    f32 nearest = -1;                 // closest obstacle, horizontal metres (robot scale), < 0 = none
    f32 nearest_bearing = 0;          // its direction relative to the trunk heading, radians (+ = left)
    f32 free_left = 0, free_right = 0;  // how open the left / right halves are (0 blocked .. 1 free)
    Vec3 gravity{0, 0, -1};           // trunk frame
    bool fallen = false;
    f32 speed = 0;                    // horizontal, robot scale m/s
    f64 tof_time = -1;                // simulation time of the last ToF frame
  };
  /// The walker `id` (0 = the first one): its sensors; false when there is no such walker.
  bool walker_sensors(ActorId id, WalkerSensors& out) const;
  /// The head camera of walker `id` (0 = the first): the robot's IMX219, 62 degrees wide, 16:9.
  bool walker_camera(ActorId id, Camera& out) const;
  /// Commands for walker `id` (0 = all): twist in the robot's real units (m/s, rad/s), head deltas
  /// (radians). They override autonomy for `hold` seconds (0 = until the next command).
  bool walker_command(ActorId id, f32 forward, f32 strafe, f32 turn, f32 hold);
  bool walker_look(ActorId id, f32 yaw, f32 pitch, f32 hold);
  /// Autonomy on / off (on: it roams on its own whenever nobody drives it).
  bool walker_set_autonomous(ActorId id, bool on);
  /// The current behaviour of walker `id` ("Wander", "Chill"... "" = none).
  std::string walker_state(ActorId id) const;
  /// Every walker actor of the running world.
  std::vector<ActorId> walkers() const;

 private:
  struct Visual;
  struct CityVisual;
  struct SimBody;
  struct SimJoint;
  struct WalkerBrain;
  struct Traffic;
  class LogicHost;

  physics::Body create_body(const BodyPlan& plan);
  void sync_actor_mesh(const Actor& a, const Mat4& world);
  void sync_city(const Actor& a, const Mat4& world);
  void remove_visual(ActorId id);
  void sync_ground(const WorldSettings& w);
  Material material_for(const MaterialDesc& m, u32* texture) const;
  /// `alpha` blends from the previous fixed step's poses to the current ones (render interpolation).
  void compute_world_matrices(f32 time, f32 alpha = 1);
  Mat4 animated_local(const Actor& a, f32 time) const;
  void fixed_step(const PlayInput& input, f32 dt);
  void drive_player(const PlayInput& input, f32 dt);
  void drive_walker(const PlayInput& input, f32 dt);
  void create_walkers();
  void drive_walkers(f32 h);
  void sense_walker(WalkerBrain& w);
  WalkerBrain* walker_brain(ActorId id) const;
  void drive_servos(f32 dt);
  void drive_traffic(f32 dt);
  void handle_events();
  void collect(ActorId pickup);
  void trigger(ActorId actor);
  void finish(GameStatus status, std::string message, bool jingle = true);
  void cue(const std::string& sound, Vec3 at, f32 volume = 1, f32 pitch = 1, bool spatial = true, audio::Bus bus = audio::Bus::sfx, ActorId actor = 0);
  void say(std::string message);
  void respawn_player();
  bool grounded(physics::Body body, f32 reach) const;
  void drive_soldier(const PlayInput& in, f32 dt);
  void fire_weapon(const PlayInput& in, const WeaponComponent& w);
  void update_health(f32 dt);
  void present_soldier(f64 now_ms);
  /// Soldier art: SpawnCity's articulated soldiers (also while editing), the first-person weapon
  /// and arms, muzzle flashes, tracers, decals, chips and brass (soldier_art.cpp).
  void present_soldier_art(f64 now_ms, f32 dt);
 public:
  struct ArtStats { u32 rigs = 0, decals = 0, tracers = 0, chips = 0; bool view_model = false; };
  /// What the soldier art draws right now (tests, stats).
  ArtStats art_stats() const;
 private:
  void release_soldier_art();
  std::unique_ptr<SoldierArt, SoldierArtDelete> art_;
  std::unordered_set<ActorId> art_hidden_;  // actors drawn by a soldier rig instead of their meshes
  void replica_soldier(const PlayInput& in, f32 dt);
  void soldier_camera(Camera& cam, f32 dt, const PlayInput& in, const ControllerComponent& c);
  ActorId actor_for_body(physics::Body body) const;
  f32 a_eye_height(ActorId actor) const;
  void init_combat(ActorId id);
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
  u32 session_ = 0;
  bool paused_ = false;
  physics::World physics_;
  physics::MouseGrab grab_;
  std::vector<std::unique_ptr<SimBody>> bodies_;
  std::unordered_map<ActorId, SimBody*> body_of_actor_;   // owner -> body
  std::unordered_map<u64, ActorId> actor_of_body_;
  std::vector<std::unique_ptr<SimJoint>> joints_;
  std::vector<std::unique_ptr<WalkerBrain>> walkers_;  // articulated robots and their policies
  std::vector<std::unique_ptr<Traffic>> traffic_;
  std::unordered_map<ActorId, bool> alive_;
  struct Respawn { ActorId id; f32 at; };
  std::vector<Respawn> respawns_;
  std::vector<ActorId> order_;   // actors parents-first
  f64 accumulator_ = 0;
  u64 step_count_ = 0;             // fixed steps since start (sim_time_ = step_count_ / step_hz)
  f32 sim_time_ = 0;
  ActorId player_ = 0;
  void possess_character(bool on);
  void touched(physics::Shape shape, physics::Body body);
  void create_joints(const Actor& a);
  f32 shake_ = 0;
  Vec3 camera_eye_;
  // The player's body pose as drawn this frame (interpolated between physics steps): the
  // camera follows this, not the latest physics state, or fast bodies shake on screen.
  physics::Pose presented_player_;
  ActorId presented_player_id_ = 0;
  f32 camera_yaw_ = 0, camera_pitch_ = 18, camera_zoom_ = 1;
  bool camera_reset_ = true;
  Hud hud_;
  u32 broken_joints_ = 0;
  std::vector<std::string> events_;
  std::vector<SoundCue> cues_;
  PlayInput last_input_;
  // sound state
  std::unordered_map<ActorId, f32> last_impact_;
  /// One player's controller state (the local one and every remote one).
  struct Pawn {
    ActorId actor = 0, source = 0;
    std::vector<ActorId> actors;      // the pawn's actors (root first)
    PlayInput input;                  // remote players: their latest input
    f32 yaw = 0;                      // camera yaw for camera-relative movement (degrees)
    Vec3 spawn_position;
    Quat spawn_rotation;
    bool jump_latch = false, was_boosting = false, airborne = false;
    f32 boost = 1, fall_speed = 0, stride = 0;
    u32 steps = 0;
    f32 steer_vis = 0, pitch_vis = 0, last_forward = 0;
    // Character players walk with Box3D's kinematic mover instead of fighting the solver.
    physics::CharacterMover mover;
    bool use_mover = false;
    f32 mover_lift = 0;               // body origin above the feet
    // soldier
    f32 pitch = 0;                    // aim pitch (degrees, positive looks down)
    f32 crouch = 0;                   // 0 standing .. 1 crouched
    f32 coyote = 0, jump_buffer = 0;  // seconds
    i32 mag = 0, reserve = 0;
    f32 next_fire = 0, reload_until = -1, spread = 0;
    bool fire_latch = false;
    u32 shots = 0;
    f32 respawn_at = -1, protected_until = 0;
    std::vector<std::pair<f32, Vec3>> history;  // (sim time, feet) for lag compensation
    u8 hit_count = 0, damage_count = 0;          // replicated feedback counters
    bool hit_head = false, hit_kill = false;
    f32 damage_from_yaw = 0;                     // world yaw the last damage came from
    Vec3 seen_feet, seen_vel;                    // replicas: presented feet and their velocity
    bool seen_valid = false;
    f32 client_next_fire = 0;                    // replicas: predicted fire timing (camera kick)
    PlayInput prev_input;                        // input actions: edges for logic events
    f32 teleport_until = 0;                      // teleporters: no instant return trip
  };
  struct HealthState {
    f32 hp = 100, armor = 0, last_damage = -1e9f;
    bool dead = false;
    f32 respawn_at = -1;
  };
  std::unordered_map<ActorId, HealthState> health_;
  std::vector<ShotEvent> shots_;
  Camera view_camera_;
  bool view_valid_ = false;
  // camera feel (local player)
  f32 bob_phase_ = 0, bob_weight_ = 0, recoil_pitch_ = 0, recoil_yaw_ = 0, recoil_roll_ = 0, shake_energy_ = 0, land_dip_ = 0;
  f32 fov_now_ = 0, look_rate_x_ = 0, look_rate_y_ = 0, aim_blend_ = 0;
  u32 viewmodel_mesh_ = 0, flash_mesh_ = 0, tracer_mesh_ = 0;
  f64 flash_until_ = 0;
  Vec3 flash_at_;
  struct Tracer { Vec3 from, to; f64 born = 0; };
  std::vector<Tracer> tracers_;
  std::vector<std::unique_ptr<Pawn>> pawns_;
  Pawn idle_pawn_;                    // state target when nobody is possessed
  Pawn* pw_ = &idle_pawn_;            // the pawn being driven
  Pawn* local_ = nullptr;             // the pawn this machine controls
  Pawn* pawn_of(ActorId actor);
  std::vector<std::pair<ActorId, bool>> alive_changes_;
  // logic
  void start_logic();
  void logic_fire(const char* type, ActorId actor, ActorId other, std::vector<std::pair<std::string, logic::Value>> extra = {}, std::string key = {});
  void logic_inputs(Pawn& pawn, const PlayInput& in);
  void interact(Pawn& pawn);
  std::unique_ptr<LogicHost> logic_host_;
  std::unique_ptr<logic::Runtime> logic_;
  u32 logic_seed_ = 0x9e3779b9u;
  bool logic_events_ = false;
  bool starting_replica_ = false;
  std::map<std::pair<ActorId, ActorId>, int> overlaps_;  // (sensor, visitor) -> shapes inside       // start() for a network replica: no logic           // the level has graphs: shapes report sensor / hit events
  std::vector<PropertyChange> prop_changes_, prop_log_;
  std::vector<SpawnChange> spawn_changes_, spawn_log_;
  std::unordered_map<ActorId, std::pair<Vec3, Quat>> replica_poses_;
};

}  // namespace kern::game
