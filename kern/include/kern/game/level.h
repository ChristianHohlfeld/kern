// Levels: actors with components, the document Kern Studio edits and the
// game runtime plays (kern/game/runtime.h). Stored as JSON (".klevel").
//
// An actor has a transform relative to its parent and a fixed set of optional
// components (one mesh, light, body, controller ... and any number of
// colliders and joints). Child actors without a body of their own are welded
// into the nearest ancestor's body as a compound when the level simulates.
//
// Conventions: metres, seconds, kilograms, Z up. Rotations are stored as
// Euler angles in degrees (x roll, y pitch, z yaw; applied yaw * pitch * roll)
// like an editor rotator, colours as sRGB 0..1.
#pragma once

#include "kern/logic/logic.h"
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "kern/audio/audio.h"
#include "kern/core/json.h"
#include "kern/math/math.h"

namespace kern::game {

using ActorId = u32;  // unique within a level, 0 = none

struct Transform {
  Vec3 position;
  Vec3 rotation;        // Euler degrees (x roll, y pitch, z yaw)
  Vec3 scale{1, 1, 1};
};

/// Quaternion of an Euler rotation in degrees (yaw * pitch * roll).
Quat euler_to_quat(Vec3 degrees);
/// Euler degrees of a quaternion (yaw in -180..180, pitch in -90..90).
Vec3 quat_to_euler(Quat q);
/// Matrix of a transform (translation * rotation * scale).
Mat4 transform_matrix(const Transform& t);
/// Decomposes a matrix without shear into a transform (scale signs stay positive).
Transform matrix_transform(const Mat4& m);

// ---- components ----

enum class Shape : u8 { box, sphere, cylinder, cone, capsule, plane, torus, ramp, mesh };
const char* shape_name(Shape s);
bool parse_shape(std::string_view s, Shape& out);

struct MaterialDesc {
  Vec3 color{0.8f, 0.8f, 0.8f};  // sRGB
  f32 roughness = 0.6f, metalness = 0;
  Vec3 emissive{0, 0, 0};       // sRGB colour, multiplied by emissive_strength
  f32 emissive_strength = 0;
  f32 opacity = 1;              // < 1: alpha blended
  bool unlit = false, cast_shadow = true, double_sided = false;
  std::string texture;          // texture asset id ("" = none)
  f32 tiling = 1;               // texture repeats per metre (planes, boxes) or per surface
};

struct MeshComponent {
  Shape shape = Shape::box;
  Vec3 size{1, 1, 1};           // box, plane (x, y), ramp
  f32 radius = 0.5f;            // sphere, cylinder, cone, capsule, torus (major)
  f32 height = 1;               // cylinder, cone, capsule (total), torus (minor radius)
  i32 segments = 24;
  std::string asset;            // mesh: a built-in mesh ("duck/xl330", kern/game/builtin_meshes.h), scaled by size
  bool collision = true;        // generates a fitted collider when the actor has no explicit one
  // The physical material of that fitted collider (Box3D surface material; touching materials
  // combine friction as sqrt(a x b)): beside the render material, as the object's physics.
  f32 friction = 0.6f, restitution = 0, rolling_resistance = 0;
  std::string surface;          // what it is made of, where a pair has its own friction: "", cloth, tpu, floor (physics::Surface)
  MaterialDesc material;
};

struct LightComponent {
  Vec3 color{1, 0.95f, 0.85f};
  f32 intensity = 20;
  f32 range = 12;
};

enum class Motion : u8 { fixed, kinematic, dynamic };
const char* motion_name(Motion m);

struct BodyComponent {
  Motion motion = Motion::dynamic;
  f32 mass = 0;                 // kg; 0 = from the colliders' densities
  f32 linear_damping = 0, angular_damping = 0.05f, gravity_scale = 1;
  bool bullet = false, start_awake = true, allow_sleep = true;
  bool lock_linear[3] = {false, false, false};
  bool lock_angular[3] = {false, false, false};
  Vec3 linear_velocity, angular_velocity;  // initial; angular in degrees per second
  std::string impact_sound = "builtin:impact";  // played on hard hits ("" = silent)
  f32 impact_threshold = 2.5f;  // m/s approach speed for an impact sound
  // Explicit mass properties (imported robots, MJCF / URDF): the centre of mass (actor-local) and
  // the inertia tensor about it in actor axes (kg*m^2: xx yy zz xy xz yz). Used together with
  // `mass` when the diagonal is positive; otherwise the colliders' shapes and densities decide.
  Vec3 center_of_mass;
  f32 inertia[6] = {0, 0, 0, 0, 0, 0};
  bool explicit_inertia() const { return inertia[0] > 0 && inertia[1] > 0 && inertia[2] > 0; }
};

enum class ColliderShape : u8 { automatic, box, sphere, capsule, hull };
const char* collider_shape_name(ColliderShape s);

struct ColliderComponent {
  ColliderShape shape = ColliderShape::automatic;  // automatic: fitted to the mesh
  Vec3 center;                  // actor-local offset
  Vec3 rotation;                // Euler degrees
  Vec3 size{1, 1, 1};           // box
  f32 radius = 0.5f, length = 1;  // sphere, capsule (length between the cap centres, along Z)
  std::vector<Vec3> points;     // hull: its points (actor-local); empty = the hull of the actor's mesh
  f32 density = 1000, friction = 0.6f, restitution = 0, rolling_resistance = 0;
  std::string surface;          // "", cloth, tpu, floor: a pair of these has its own friction (physics::Surface)
  bool sensor = false;
  u32 category = 1, mask = 0xffffffffu;  // collision channels
  i32 group = 0;
  /// Played when this collider hits something: "auto" = its body's impact sound, "" / "none" =
  /// silent. Lets parts of one body sound different (a sole steps, a plastic shell taps).
  std::string impact_sound = "auto";
};

enum class JointKind : u8 { hinge, slider, ball, fixed, distance, wheel, motor, parallel, no_collision };
const char* joint_kind_name(JointKind k);

struct JointComponent {
  JointKind kind = JointKind::hinge;
  ActorId target = 0;           // the other actor (0 = the world)
  Vec3 anchor;                  // actor-local
  Vec3 axis{0, 0, 1};           // actor-local: hinge/ball/parallel axis, slider/wheel direction
  bool collide_connected = false;
  bool enable_limit = false;
  f32 lower = -45, upper = 45;  // hinge, ball cone (upper): degrees; slider, wheel: metres; distance: min/max length
  f32 reference = 0;            // hinge: the angle (degrees) the joint reads in the pose the level builds it in
  bool enable_motor = false;
  f32 motor_speed = 0;          // degrees per second (hinge, wheel) or m/s (slider, distance)
  f32 max_motor_force = 100;    // N or N*m
  bool enable_spring = false;
  f32 spring_hertz = 4, spring_damping = 0.5f;
  f32 length = 0;               // distance joint rest length, 0 = the distance at start
  f32 break_force = 0, break_torque = 0;  // N / N*m, 0 = unbreakable
  // Hinge servo (a position-controlled motor, like a Dynamixel): torque
  //   servo_stiffness x (servo_target - angle) - servo_damping x speed, at most max_motor_force N*m,
  // with servo_friction N*m of joint friction. Box3D's joint spring and motor (GameWorld::drive_servos).
  bool servo = false;
  f32 servo_target = 0;          // degrees
  f32 servo_stiffness = 50;      // N*m/rad
  f32 servo_damping = 2;         // N*m*s/rad
  f32 servo_friction = 0.1f;     // N*m
  std::string break_sound = "builtin:break";  // played when the joint breaks ("" = silent)
};

/// soldier: a first-person shooter player (Box3D character mover, crouch, sprint,
/// aim, a weapon from its WeaponComponent, health from its HealthComponent).
/// walker: an articulated robot that walks on its own servos, driven by its control policy (the
/// Micro Duck: Pollen Robotics' trained neural network, see docs/walker.md).
enum class ControlMode : u8 { hovercar, rolling, character, flyer, vehicle, soldier, walker };
const char* control_mode_name(ControlMode m);
enum class CameraMode : u8 { chase, first_person, top_down };
const char* camera_mode_name(CameraMode m);

struct ControllerComponent {
  ControlMode mode = ControlMode::hovercar;
  f32 max_speed = 36;           // m/s
  f32 acceleration = 24;        // m/s^2
  f32 turn_rate = 130;          // degrees per second
  f32 jump_speed = 6;           // m/s, 0 = no jump
  bool boost = true;
  f32 boost_speed = 60;         // m/s with boost
  f32 hover_height = 0.6f;      // hovercar
  CameraMode camera = CameraMode::chase;
  f32 camera_distance = 12, camera_height = 4, camera_fov = 62;
  // sounds ("auto" = the movement mode's default, "" = silent)
  std::string engine_sound = "auto";  // loop, pitch and volume follow speed and throttle
  std::string jump_sound = "builtin:jump";
  std::string boost_sound = "builtin:boost";
  // soldier
  f32 crouch_speed = 2.6f;      // m/s while crouching
  f32 eye_height = 1.62f;       // m above the feet standing (crouching: 60 %)
  f32 look_sensitivity = 1;     // mouse / touch / gamepad look scale
  bool head_bob = true;
  // walker
  bool autonomous = true;       // roams on its own (camera, ToF) whenever nobody drives it
  std::string gait = "velstand";  // walking network: "velstand" (walks and stands) or "alpha" (alpha_walking + alpha_stand)
  bool rollers = false;         // wears the roller skates (roller.onnx and the crouch drive it)
  // How network outputs become servo goals: "training" (scale 1, no filter, as the networks were
  // trained) or "robot" (the robot's daemon: scale 0.9, low-pass head 0.5 / legs 0.7).
  std::string actions = "training";
  // Kern's own additions on top of Pollen's software, off by default (then only Pollen's runtime
  // and brain run): ball play, sock fetching, the beak opening and biting during a ground pick.
  bool kern_extras = false;
  // Its servos' command latency (real seconds; the device profile, game/servo.h).
  f32 servo_delay = 0.02f;
};

enum class WeaponKind : u8 { rifle, pistol, shotgun, sniper };
const char* weapon_kind_name(WeaponKind k);

/// A hitscan weapon for soldier players (server-authoritative and lag-compensated in multiplayer).
struct WeaponComponent {
  WeaponKind kind = WeaponKind::rifle;
  f32 damage = 24;              // per hit (per pellet for shotguns)
  f32 fire_rate = 600;          // rounds per minute
  bool automatic = true;        // hold to fire
  i32 magazine = 30;
  i32 reserve = 120;            // spare rounds
  f32 reload_time = 2.0f;       // seconds
  f32 spread = 1.2f;            // degrees, hip fire while standing
  f32 aim_spread = 0.15f;       // degrees aiming down sights
  f32 recoil = 0.8f;            // degrees of view kick per shot
  f32 range = 300;              // m
  f32 headshot = 2.5f;          // damage multiplier on head colliders
  f32 impulse = 4;              // N*s on dynamic bodies that are hit
  i32 pellets = 1;              // shotgun: rays per shot
  f32 aim_fov = 42;             // degrees aiming down sights
  std::string fire_sound = "builtin:smg";
  std::string reload_sound = "builtin:reload";
};

/// Hit points: actors with health take damage from weapons (and falls for players).
/// Handling of a weapon type: damage, rate, magazine, spread and recoil.
WeaponComponent weapon_preset(WeaponKind kind);

struct HealthComponent {
  f32 max_health = 100;
  f32 armor = 0;                // absorbs damage first
  f32 regen_delay = 5;          // seconds after the last damage, 0 = no regeneration
  f32 regen_rate = 12;          // hit points per second
  bool respawn = true;          // players respawn at their spawn; others reappear in place
  f32 respawn_delay = 3;        // seconds
  f32 points = 10;              // score for the player who brings it down
  std::string hit_sound = "builtin:hit";
  std::string death_sound = "builtin:down";
  f32 explode_radius = 0;       // > 0: blows up when destroyed (impulse and damage around it)
  f32 explode_damage = 80;      // damage at the centre, falling off to 0 at the radius
};

struct PickupComponent {
  f32 points = 1;
  Vec3 burst_color{0.55f, 1.0f, 0.92f};
  f32 respawn = 0;              // seconds, 0 = gone for this run
  std::string sound = "builtin:pickup";
  f32 heal = 0;                 // health given to the collector
  i32 ammo = 0;                 // rounds added to the collector's reserve
};

enum class TriggerAction : u8 { goal, hazard, checkpoint, message, launch, teleport, damage, event };
const char* trigger_action_name(TriggerAction a);

struct TriggerComponent {
  TriggerAction action = TriggerAction::goal;
  std::string message;
  bool once = true;
  std::string sound = "auto";   // "auto" = the action's default sound, "" = silent
  f32 strength = 12;            // launch: m/s upwards (plus forward along the trigger's X); damage: hit points
  u32 target = 0;               // teleport: the actor to arrive at
};

/// A sound emitter: a looping ambience, a machine hum, a radio, a one-shot.
struct AudioComponent {
  std::string sound = "builtin:hum";  // "builtin:<name>" or an imported sound asset id
  f32 volume = 1;               // linear gain (0..4)
  f32 pitch = 1;                // playback rate
  bool loop = true;
  bool autoplay = true;         // starts with Play / Simulate
  bool spatial = true;          // 3D at the actor (false: plays 2D on its bus)
  f32 min_distance = 2;         // full volume inside (m)
  f32 max_distance = 40;        // silent beyond (m)
  audio::Rolloff rolloff = audio::Rolloff::inverse;
  audio::Bus bus = audio::Bus::sfx;
  f32 doppler = 1;              // 0 = no Doppler shift
  f32 pitch_by_speed = 0;       // extra pitch per m/s of the actor's body
  f32 delay = 0;                // seconds after the start
};

struct RotatorComponent {
  Vec3 rate{0, 0, 60};          // degrees per second about the actor's local axes
};

enum class MoverMode : u8 { sine, ping_pong, loop };
const char* mover_mode_name(MoverMode m);

struct MoverComponent {
  Vec3 offset{0, 0, 1};         // metres, relative to the start position (parent space)
  f32 period = 2;               // seconds per cycle
  f32 phase = 0;                // 0..1
  MoverMode mode = MoverMode::sine;
};

struct CityComponent {
  u32 seed = 20261022;
  i32 blocks = 7;               // blocks per side
  f32 pitch = 80;               // metres from street to street
  f32 street = 16;              // street width
  f32 max_height = 84;          // tallest building in the centre
  bool trees = true;
  i32 traffic = 22;             // cars on the lanes
  bool night_windows = true;    // lit windows at night (shader extension)
};

struct Actor {
  ActorId id = 0;
  std::string name;
  ActorId parent = 0;
  std::string folder;           // outliner folder ("" = top level)
  bool hidden = false;          // hidden in the editor and in game
  bool locked = false;          // not selectable in the viewport
  Transform transform;          // relative to the parent
  std::vector<std::string> tags;
  std::optional<MeshComponent> mesh;
  std::optional<LightComponent> light;
  std::optional<BodyComponent> body;
  std::vector<ColliderComponent> colliders;
  std::vector<JointComponent> joints;
  std::optional<ControllerComponent> controller;
  std::optional<PickupComponent> pickup;
  std::optional<TriggerComponent> trigger;
  std::optional<RotatorComponent> rotator;
  std::optional<MoverComponent> mover;
  std::optional<CityComponent> city;
  std::optional<AudioComponent> audio;
  std::optional<WeaponComponent> weapon;
  std::optional<HealthComponent> health;
  std::optional<logic::Graph> logic;   // the actor's own logic graph (Self = this actor)
};

enum class GameGoal : u8 { none, collect_all, reach_goal, survive, score };
const char* game_goal_name(GameGoal g);

struct WorldSettings {
  Vec3 gravity{0, 0, -9.80665f};
  i32 substeps = 4;
  f32 step_hz = 60;
  f32 time_of_day = 0.42f;      // 0..1 (0.25 sunrise, 0.5 noon, 0.75 sunset)
  f32 day_length = 0;           // seconds per full day while playing, 0 = frozen
  f32 sun_azimuth = 210;        // degrees, where the sun stands at noon
  f32 fog_density = 0.0009f;
  f32 exposure = 1;
  f32 ao_radius = 1.6f;         // ambient occlusion reach in metres (scene scale), 0 = none
  bool clouds = true;
  Vec3 ground_color{0.19f, 0.27f, 0.15f};  // the endless ground plane under everything
  bool ground = true;
  // Contact physics (Box3D): the endless ground's material (two touching materials combine as
  // sqrt(a x b), Box3D's rule) and the contact softness - stiffness (Hz), damping ratio, push-out
  // speed cap (m/s), Box3D's world definition.
  f32 ground_friction = 0.7f, ground_restitution = 0;
  std::string ground_surface;  // "" or floor (physics::Surface)
  f32 contact_hertz = 30, contact_damping = 10, contact_speed = 3;
  f32 kill_z = -50;             // a player below this height respawns / loses
  std::string title;
  GameGoal goal = GameGoal::none;
  f32 time_limit = 0;           // seconds, 0 = none
  f32 score_target = 0;         // GameGoal::score
  u32 player = 0;               // the actor the player controls (0 = the first with a Player Controller)
  bool observer = false;        // Play starts with a free camera: fly and watch, drag bodies (F8 takes over the player)
  // audio
  f32 master_volume = 1;
  std::string music;            // looping 2D track on the music bus ("" = none)
  f32 music_volume = 0.6f;
  std::string ambience;         // looping 2D bed on the ambience bus
  f32 ambience_volume = 0.6f;
  f32 reverb = 0.25f;           // master reverb amount 0..1
  f32 doppler = 1;              // Doppler scale for every 3D sound
};

struct TextureAsset {
  std::string id, name;
  std::shared_ptr<const std::string> src;  // data URL (png/jpeg/webp), shared by undo snapshots
  i32 width = 0, height = 0;
};

/// An imported audio file (wav, mp3, ogg, m4a/aac, flac - whatever the browser decodes).
struct SoundAsset {
  std::string id, name;
  std::shared_ptr<const std::string> src;  // data URL, shared by undo snapshots
  f32 duration = 0;             // seconds (informational)
};

struct EditorCamera {
  Vec3 position{-24, -30, 18};
  f32 yaw = 51, pitch = -24;    // degrees
};

/// The actor Play possesses: world.player when it has a Player Controller, else the first that has one.
struct Level;
u32 default_player(const Level& level);

struct Level {
  std::string name = "Untitled";
  WorldSettings world;
  std::vector<Actor> actors;
  std::vector<TextureAsset> textures;
  std::vector<SoundAsset> sounds;
  logic::Graph logic;            // the level graph (Self = none; sees every actor's events)
  EditorCamera camera;
  ActorId next_id = 1;

  Actor* find(ActorId id);
  const Actor* find(ActorId id) const;
  Actor& add(Actor actor);       // assigns an id when actor.id == 0
  /// Children of `parent` in document order (0 = top level).
  std::vector<ActorId> children(ActorId parent) const;
  /// Matrix from actor space to world space (parents applied).
  Mat4 world_matrix(ActorId id) const;
  /// The nearest ancestor (or the actor itself) with a body, 0 = none.
  ActorId body_owner(ActorId id) const;
  bool is_ancestor(ActorId ancestor, ActorId id) const;
  const TextureAsset* texture(std::string_view id) const;
  const SoundAsset* sound(std::string_view id) const;
};

// ---- serialization ----

json::Value to_json(const Level& level);
std::string to_text(const Level& level, i32 indent = 2);
/// Reads a level; unknown keys are ignored, missing ones take defaults.
bool from_json(const json::Value& doc, Level& out, std::string* error);
bool from_text(std::string_view text, Level& out, std::string* error);
json::Value actor_to_json(const Actor& a);
bool actor_from_json(const json::Value& v, Actor& out, std::string* error);
json::Value world_to_json(const WorldSettings& w);
bool world_from_json(const json::Value& v, WorldSettings& out, std::string* error);

struct Issue {
  bool error = true;  // false: a warning, the level still plays
  ActorId actor = 0;
  std::string text;
};
/// Problems of a level: errors keep it from playing (dangling references, bad
/// values), warnings point at likely mistakes.
/// True for "", "none", "auto", a known "builtin:<name>" or an imported sound id.
bool sound_reference_ok(const Level& level, std::string_view ref);
std::vector<Issue> validate(const Level& level);

/// "#rrggbb" <-> sRGB 0..1.
std::string hex_color(Vec3 c);
bool parse_hex_color(std::string_view s, Vec3& out);
/// sRGB 0..1 -> linear.
Vec3 srgb_to_linear(Vec3 c);

}  // namespace kern::game
