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

enum class Shape : u8 { box, sphere, cylinder, cone, capsule, plane, torus, ramp };
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
  bool collision = true;        // generates a fitted collider when the actor has no explicit one
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
};

enum class ColliderShape : u8 { automatic, box, sphere, capsule, hull };
const char* collider_shape_name(ColliderShape s);

struct ColliderComponent {
  ColliderShape shape = ColliderShape::automatic;  // automatic: fitted to the mesh
  Vec3 center;                  // actor-local offset
  Vec3 rotation;                // Euler degrees
  Vec3 size{1, 1, 1};           // box
  f32 radius = 0.5f, length = 1;  // sphere, capsule (length between the cap centres, along Z)
  f32 density = 1000, friction = 0.6f, restitution = 0, rolling_resistance = 0;
  bool sensor = false;
  u32 category = 1, mask = 0xffffffffu;  // collision channels
  i32 group = 0;
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
  bool enable_motor = false;
  f32 motor_speed = 0;          // degrees per second (hinge, wheel) or m/s (slider, distance)
  f32 max_motor_force = 100;    // N or N*m
  bool enable_spring = false;
  f32 spring_hertz = 4, spring_damping = 0.5f;
  f32 length = 0;               // distance joint rest length, 0 = the distance at start
  f32 break_force = 0, break_torque = 0;  // N / N*m, 0 = unbreakable
  std::string break_sound = "builtin:break";  // played when the joint breaks ("" = silent)
};

enum class ControlMode : u8 { hovercar, rolling, character, flyer, vehicle };
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
};

struct PickupComponent {
  f32 points = 1;
  Vec3 burst_color{0.55f, 1.0f, 0.92f};
  f32 respawn = 0;              // seconds, 0 = gone for this run
  std::string sound = "builtin:pickup";
};

enum class TriggerAction : u8 { goal, hazard, checkpoint, message };
const char* trigger_action_name(TriggerAction a);

struct TriggerComponent {
  TriggerAction action = TriggerAction::goal;
  std::string message;
  bool once = true;
  std::string sound = "auto";   // "auto" = the action's default sound, "" = silent
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
  bool clouds = true;
  Vec3 ground_color{0.19f, 0.27f, 0.15f};  // the endless ground plane under everything
  bool ground = true;
  f32 kill_z = -50;             // a player below this height respawns / loses
  std::string title;
  GameGoal goal = GameGoal::none;
  f32 time_limit = 0;           // seconds, 0 = none
  f32 score_target = 0;         // GameGoal::score
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

struct Level {
  std::string name = "Untitled";
  WorldSettings world;
  std::vector<Actor> actors;
  std::vector<TextureAsset> textures;
  std::vector<SoundAsset> sounds;
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
