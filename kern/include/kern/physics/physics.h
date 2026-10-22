// Rigid-body physics: a C++ layer over Box3D (kern/third_party/box3d), compiled
// into the same WebAssembly module as the renderer. Link with
// `kern/build.sh --physics`.
//
// Conventions match the renderer: metres, seconds, kilograms, Z up, quaternions
// (x, y, z, w). Handles are plain 64-bit values (Box3D ids packed), 0 = none.
// A World owns everything created in it; destroying the World frees it all.
#pragma once

#include <unordered_map>
#include <vector>

#include "kern/geo/mesh_data.h"
#include "kern/math/math.h"

namespace kern::physics {

enum class BodyType : u8 { fixed = 0, kinematic = 1, dynamic = 2 };
enum class ShapeKind : u8 { box, sphere, capsule, hull };
/// Box3D's native joints. (The editor's "rope" and "friction" are a limited
/// distance joint and a motor joint without springs.)
enum class JointType : u8 { revolute, distance, prismatic, wheel, weld, motor, spherical, parallel, filter };

struct Body { u64 id = 0; bool valid() const { return id != 0; } bool operator==(const Body& o) const { return id == o.id; } };
struct Shape { u64 id = 0; bool valid() const { return id != 0; } bool operator==(const Shape& o) const { return id == o.id; } };
struct Joint { u64 id = 0; bool valid() const { return id != 0; } bool operator==(const Joint& o) const { return id == o.id; } };

struct Pose {
  Vec3 position;
  Quat rotation;
};

struct WorldSettings {
  Vec3 gravity{0, 0, -9.80665f};
  bool enable_sleep = true;
  bool enable_continuous = true;
  f32 restitution_threshold = 1;  // m/s; slower impacts do not bounce
  i32 restitution_iterations = 2; // 0..63
  bool restitution_propagation = false;
  f32 hit_event_threshold = 1;    // m/s
};

struct BodySettings {
  BodyType type = BodyType::dynamic;
  Vec3 position;
  Quat rotation;
  Vec3 linear_velocity, angular_velocity;
  f32 linear_damping = 0, angular_damping = 0, gravity_scale = 1;
  bool enable_sleep = true, awake = true, enabled = true, bullet = false;
  u64 user = 0;  // returned by World::body_user
};

struct ShapeMaterial {
  f32 density = 1;            // kg/m^3
  f32 friction = 0.6f;
  f32 restitution = 0;        // 0..1
  f32 rolling_resistance = 0; // spheres and capsules
  bool sensor = false;
  u64 category_bits = 1, mask_bits = ~0ull;
  i32 group_index = 0;
  bool contact_events = true, sensor_events = true, hit_events = false;
  u64 user = 0;               // returned by World::shape_user
};

struct ShapeSettings {
  ShapeKind kind = ShapeKind::box;
  Vec3 center;               // body-local placement of the shape
  Quat rotation;
  Vec3 size{1, 1, 1};        // box: full extents
  f32 radius = 0.5f;         // sphere, capsule
  f32 length = 1;            // capsule: distance between the hemisphere centres, along the shape's Z
  Span<const Vec3> points;   // hull: points in shape space (4..128 after hull reduction)
  ShapeMaterial material;
};

/// One definition for all joint types; each type reads the fields it needs.
/// Local frames: revolute, spherical and parallel joints work about the frame's
/// Z axis, prismatic and wheel joints along its X axis (see frame_from_axis).
struct JointSettings {
  JointType type = JointType::revolute;
  Body body_a, body_b;
  Pose frame_a, frame_b;         // in body-local space
  bool collide_connected = false;
  // springs: revolute, prismatic, distance, spherical, wheel (suspension), parallel
  bool enable_spring = false;
  f32 hertz = 0, damping_ratio = 0;
  // limits: revolute (radians), prismatic and wheel (metres), distance (min/max length), spherical (cone angle = upper)
  bool enable_limit = false;
  f32 lower = 0, upper = 0;
  // motors: revolute and wheel use torque, prismatic and distance use force, spherical uses torque + motor_velocity
  bool enable_motor = false;
  f32 motor_speed = 0, max_motor_torque = 0, max_motor_force = 0;
  Vec3 motor_velocity;
  f32 length = 1;                // distance joint rest length
  // weld joint softness (0 hertz = rigid)
  f32 linear_hertz = 0, angular_hertz = 0, linear_damping_ratio = 1, angular_damping_ratio = 1;
  // motor joint: velocity drive and spring towards frame alignment
  Vec3 linear_velocity, angular_velocity;
  f32 max_velocity_force = 0, max_velocity_torque = 0, max_spring_force = 0, max_spring_torque = 0;
  // parallel joint
  f32 max_torque = 0;
  u64 user = 0;
};

/// Local joint frame at `anchor` whose X (x_axis = true) or Z axis points along `axis`.
Pose frame_from_axis(Vec3 anchor, Vec3 axis, bool x_axis);

struct RayHit {
  bool hit = false;
  Shape shape;
  Body body;
  Vec3 point, normal;
  f32 fraction = 0;
};

struct TouchEvent {
  Shape shape_a, shape_b;  // for sensor events: a = sensor, b = visitor
  Body body_a, body_b;
};

struct HitEvent {
  Shape shape_a, shape_b;
  Body body_a, body_b;
  Vec3 point, normal;
  f32 approach_speed = 0;
};

struct ContactPoint {
  Vec3 point, normal;     // world space; normal points from shape A to shape B
  f32 separation = 0;     // negative when penetrating
  f32 impulse = 0;        // normal impulse of the last sub-step
};

struct MotionLocks {
  bool linear[3] = {false, false, false};
  bool angular[3] = {false, false, false};
};

struct Version { i32 major = 0, minor = 0, revision = 0; };
Version version();

class World {
 public:
  World() = default;
  ~World() { destroy(); }
  World(const World&) = delete;
  World& operator=(const World&) = delete;

  bool create(const WorldSettings& settings);
  void destroy();
  bool valid() const;

  Body create_body(const BodySettings& settings);
  void destroy_body(Body body);
  bool body_valid(Body body) const;
  /// Adds a shape; on failure returns an invalid handle and sets last_error().
  Shape create_shape(Body body, const ShapeSettings& settings);
  Joint create_joint(const JointSettings& settings);
  void destroy_joint(Joint joint, bool wake_bodies = true);
  bool joint_valid(Joint joint) const;

  /// Advances by `dt` seconds with `substeps` solver sub-steps and collects the events of this step.
  void step(f32 dt, i32 substeps);

  Pose pose(Body body) const;
  void set_pose(Body body, const Pose& pose);
  /// Moves a kinematic body so that it reaches `target` after `dt` (wakes touching bodies).
  void set_target_pose(Body body, const Pose& target, f32 dt);
  Vec3 linear_velocity(Body body) const;
  Vec3 angular_velocity(Body body) const;
  void set_linear_velocity(Body body, Vec3 v);
  void set_angular_velocity(Body body, Vec3 w);
  void apply_force(Body body, Vec3 force, Vec3 point);
  void apply_force_to_center(Body body, Vec3 force);
  void apply_torque(Body body, Vec3 torque);
  void apply_linear_impulse(Body body, Vec3 impulse, Vec3 point);
  void apply_linear_impulse_to_center(Body body, Vec3 impulse);
  void apply_angular_impulse(Body body, Vec3 impulse);
  void set_awake(Body body, bool awake);
  bool awake(Body body) const;
  f32 mass(Body body) const;
  BodyType body_type(Body body) const;
  Vec3 local_point(Body body, Vec3 world_point) const;
  Vec3 world_point(Body body, Vec3 local_point) const;
  Vec3 world_vector(Body body, Vec3 local_vector) const;
  u64 body_user(Body body) const;
  u64 shape_user(Shape shape) const;
  Body shape_body(Shape shape) const;
  /// Restricts translation / rotation per world axis (a body that must stay upright, a 2D plane).
  void set_motion_locks(Body body, const MotionLocks& locks);
  /// Overrides the mass, scaling the inertia of the shapes' mass data to match.
  void set_mass(Body body, f32 mass);
  /// Joints whose constraint force (N) or torque (N*m) exceeds these values
  /// are reported in joint_events() after a step (0 = never).
  void set_joint_thresholds(Joint joint, f32 force, f32 torque);
  Vec3 joint_force(Joint joint) const;
  Vec3 joint_torque(Joint joint) const;
  /// Touching contact points of a body (for debug views).
  void contact_points(Body body, std::vector<ContactPoint>& out) const;

  /// Closest hit along origin + translation (non-sensor shapes whose category matches
  /// mask_bits; shapes of `ignore` are skipped, e.g. the body the ray starts in).
  RayHit cast_ray(Vec3 origin, Vec3 translation, u64 mask_bits = ~0ull, Body ignore = {}) const;

  // Events of the last step (shapes may have been destroyed since; check the bodies).
  const std::vector<TouchEvent>& contact_begin() const { return contact_begin_; }
  const std::vector<TouchEvent>& contact_end() const { return contact_end_; }
  const std::vector<TouchEvent>& sensor_begin() const { return sensor_begin_; }
  const std::vector<TouchEvent>& sensor_end() const { return sensor_end_; }
  const std::vector<HitEvent>& hits() const { return hits_; }
  /// Joints over their force/torque threshold in the last step (see set_joint_thresholds).
  const std::vector<Joint>& joint_events() const { return joint_events_; }

  u32 body_count() const { return static_cast<u32>(body_users_.size()); }
  i32 awake_body_count() const;
  u64 step_count() const { return steps_; }
  Vec3 gravity() const { return settings_.gravity; }

 private:
  u32 world_ = 0;  // packed b3WorldId, 0 = none
  WorldSettings settings_;
  std::unordered_map<u64, u64> body_users_, shape_users_;
  std::vector<TouchEvent> contact_begin_, contact_end_, sensor_begin_, sensor_end_;
  std::vector<HitEvent> hits_;
  std::vector<Joint> joint_events_;
  u64 steps_ = 0;
};

/// Drags a dynamic body with a spring: a kinematic "mouse" body and a motor
/// joint (the same technique as the Box3D samples and the previous Editor3D).
class MouseGrab {
 public:
  bool begin(World& world, Body body, Vec3 point, f32 stiffness_hertz = 7.5f);
  void set_target(Vec3 target) { target_ = target; }
  Vec3 target() const { return target_; }
  void update(f32 dt);  // call before World::step
  void end();
  bool active() const { return world_ != nullptr; }
  Body body() const { return body_; }

 private:
  World* world_ = nullptr;
  Body mouse_, body_;
  Joint joint_;
  Vec3 target_;
};

/// Flat-shaded triangles of the convex hull of `points` (Box3D's hull builder).
/// Returns false when the points do not span a volume or exceed the hull limits.
bool hull_mesh(Span<const Vec3> points, MeshData& out, Vec3 color = {1, 1, 1});

}  // namespace kern::physics
