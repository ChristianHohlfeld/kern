// Scene documents: the Editor3D ".e3d" format (version 2), extended with
// optional game components. A scene describes Box3D bodies with their
// fixtures (shapes), joints, image planes and sampler volumes.
//
// Reading accepts version 1 and 2 documents and the legacy
// "kurzlernen-editor3d" object list; writing always produces version 2.
// Keys this model does not know are kept per item and written back, so
// documents from newer tools survive a load/save cycle.
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "kern/core/json.h"
#include "kern/world/dmath.h"

namespace kern::world {

enum class BodyKind : i32 { fixed = 0, kinematic = 1, dynamic = 2 };

/// Editor3D shapes: box (full size), sphere, capsule (radius + length along Z), convex hull.
enum class FixtureShape : u8 { box, sphere, capsule, hull };
const char* shape_name(FixtureShape s);
bool parse_shape(std::string_view s, FixtureShape& out);

/// The joint types of Editor3D, in menu order.
enum class JointKind : u8 { revolute, distance, prismatic, wheel, rope, weld, friction, motor, spherical, parallel, filter };
constexpr u32 kJointKindCount = 11;
const char* joint_kind_name(JointKind k);
bool parse_joint_kind(std::string_view s, JointKind& out);

// ---- game components (optional; absent = plain physics scene) ----

enum class GameRole : u8 { none, player, goal, pickup, hazard };
enum class GameControl : u8 { none, roll, drive, fly, walk };
const char* game_role_name(GameRole r);
const char* game_control_name(GameControl c);
bool parse_game_role(std::string_view s, GameRole& out);
bool parse_game_control(std::string_view s, GameControl& out);

struct BodyGame {
  GameRole role = GameRole::none;
  GameControl control = GameControl::none;
  f64 force = 12;        // control strength (m/s^2 of acceleration per unit input)
  f64 jump = 0;          // jump speed in m/s (0 = cannot jump)
  bool camera_follow = false;
  f64 points = 1;        // pickups: score value
  bool is_default() const;
};

enum class GameGoal : u8 { none, reach_goal, collect_all, survive };
const char* game_goal_name(GameGoal g);
bool parse_game_goal(std::string_view s, GameGoal& out);

struct SceneGame {
  std::string title;
  GameGoal goal = GameGoal::none;
  f64 time_limit = 0;        // seconds, 0 = none (survive: time to survive)
  f64 camera_distance = 8;
  f64 camera_height = 4;
  f64 fall_limit = -20;      // a player below this height is lost (respawn / fail)
  bool is_default() const;
};

// ---- document items ----

struct Fixture {
  std::string id, name;
  FixtureShape shape = FixtureShape::box;
  D3 center{};
  DQ rotation = kIdentityQ;
  D3 size{1, 1, 1};
  f64 radius = 0.5, length = 1;
  std::vector<D3> vertices;  // hull points (fixture space)
  f64 density = 1, friction = 0.4, restitution = 0.1, rolling_resistance = 0;
  bool sensor = false;
  f64 category_bits = 1, mask_bits = 4294967295.0, group_index = 0;
  std::string color = "#8ea9c1";
  json::Value custom = json::Value::array();
  json::Value extra = json::Value::object();
  json::Value unknown = json::Value::object();
};

struct Body {
  std::string id, name;
  BodyKind type = BodyKind::dynamic;
  D3 position{};
  DQ rotation = kIdentityQ;
  D3 linear_velocity{}, angular_velocity{};
  f64 linear_damping = 0, angular_damping = 0, gravity_scale = 1;
  bool allow_sleep = true, awake = true, active = true, bullet = false;
  std::vector<Fixture> fixtures;
  BodyGame game;
  json::Value custom = json::Value::array();
  json::Value extra = json::Value::object();
  json::Value unknown = json::Value::object();
  DPose pose() const { return {position, rotation}; }
};

struct Joint {
  std::string id, name;
  JointKind type = JointKind::revolute;
  std::string body_a, body_b;
  D3 anchor_a{}, anchor_b{};
  D3 axis_a{1, 0, 0};
  std::optional<D3> axis_b;
  bool enable_limit = false, enable_motor = false, enable_spring = false, collide_connected = false;
  f64 lower_limit = -1, upper_limit = 1;
  f64 motor_speed = 0, max_motor_torque = 0, max_motor_force = 0, max_force = 0, max_torque = 0;
  f64 max_length = 0, length = 1;
  f64 spring_frequency = 0, frequency = 0;
  std::optional<f64> spring_damping_ratio, damping_ratio;
  json::Value custom = json::Value::array();
  json::Value extra = json::Value::object();
  json::Value unknown = json::Value::object();
  D3 axis_b_or_a() const { return axis_b ? *axis_b : axis_a; }
};

/// Shared, immutable image payload (data URLs can be megabytes; undo snapshots share them).
using SharedText = std::shared_ptr<const std::string>;

struct ImagePlane {
  std::string id, name, file;
  SharedText src;            // data URL, may be empty
  std::string body;          // owning body id, "" = world
  D3 center{};
  DQ rotation = kIdentityQ;
  f64 size[2] = {2, 2};
  std::optional<f64> opacity;
  std::string tint = "#ffffff";
  bool flip = false;
  i32 filter = 0;            // 0 linear, 1 nearest
  json::Value custom = json::Value::array();
  json::Value extra = json::Value::object();
  json::Value unknown = json::Value::object();
};

struct Sampler {
  std::string id, name, file = "sample.png";
  std::string body;
  D3 center{};
  DQ rotation = kIdentityQ;
  D3 size{2, 2, 0.08};
  f64 resolution = 100, threshold = 32, tolerance = 0.025;
  std::optional<f64> opacity;
  std::string tint = "#ffffff";
  bool flip = false;
  json::Value custom = json::Value::array();
  json::Value extra = json::Value::object();
  json::Value unknown = json::Value::object();
};

struct WorldSettings {
  D3 gravity{0, 0, -9.80665};
  f64 steps_per_second = 60, sub_step_count = 4;
  bool allow_sleep = true, continuous_physics = true;
  f64 restitution_threshold = 1, restitution_iterations = 2;
  bool restitution_propagation = false;
  std::optional<json::Value> custom;  // custom properties of the world, when set
  json::Value unknown = json::Value::object();
};

struct Scene {
  std::string id, name = "Untitled";
  WorldSettings world;
  std::vector<Body> bodies;
  std::vector<Joint> joints;
  std::vector<ImagePlane> images;
  std::vector<Sampler> samplers;
  D3 cursor{};
  SceneGame game;
  json::Value extra = json::Value::object();
  json::Value unknown = json::Value::object();

  Body* body(std::string_view id);
  const Body* body(std::string_view id) const;
  /// Body owning the fixture `fixture_id`.
  Body* body_of_fixture(std::string_view fixture_id);
  const Body* body_of_fixture(std::string_view fixture_id) const;
  Fixture* fixture(std::string_view id);
  Joint* joint(std::string_view id);
  ImagePlane* image(std::string_view id);
  Sampler* sampler(std::string_view id);
  u32 fixture_count() const;
};

// ---- identifiers ----

/// Random RFC 4122 version 4 identifier (seeded once from the host clock).
std::string make_uid();
/// Makes make_uid() deterministic (tests).
void seed_uids(u64 seed);

// ---- construction helpers (the defaults of the editor's "Add" commands) ----

Scene new_scene(std::string name = "Untitled");
Body new_body(BodyKind type, const D3& position, std::string name);
Fixture new_fixture(FixtureShape shape, std::string name);

// ---- reading and writing ----

/// `image_data` false writes image sources as a short placeholder (cheap comparisons, logs).
json::Value to_json(const Scene& scene, bool image_data = true);
std::string to_text(const Scene& scene, i32 indent = 2);
/// Builds a scene from a parsed document (Editor3D v1/v2 or legacy object list).
/// `name` overrides the document name (file name without .e3d/.json); a fresh id is assigned.
/// Returns false with a message in `error` when the document is not a scene.
bool from_json(const json::Value& doc, Scene& out, std::string* error, std::string_view name = {}, bool* migrated = nullptr);
bool from_text(std::string_view text, Scene& out, std::string* error, std::string_view name = {}, bool* migrated = nullptr);
/// Repairs missing or malformed fields in place (the editor's normalizeScene).
void normalize(Scene& scene);

/// Problems that prevent running the scene. Each entry starts with "FATAL: ".
std::vector<std::string> validate(const Scene& scene);

// ---- geometry helpers ----

/// Approximate bounding radius of a fixture around its centre.
f64 fixture_radius(const Fixture& f);
/// Largest extent of all fixtures (the scale of the scene), at least 0.001.
f64 physical_span(const Scene& scene);
/// World placement of an image/sampler (relative to its body, if any).
DPose item_world_pose(const Scene& scene, const std::string& body, const D3& center, const DQ& rotation);

}  // namespace kern::world
