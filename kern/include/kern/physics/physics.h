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
enum class ShapeKind : u8 { box, sphere, capsule, hull, mesh };
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
  // Contact softness (Box3D "World Definition"): stiffness, bounciness and the cap on overlap push-out.
  f32 contact_hertz = 30, contact_damping_ratio = 10, contact_speed = 3;  // Hz, -, m/s
  f32 maximum_linear_speed = 400;  // m/s
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
  Span<const Vec3> points;   // hull: points in shape space (4..128 after hull reduction); mesh: vertices
  Span<const u32> indices;   // mesh: 3 per triangle, counter-clockwise (static bodies only)
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
  // Joint softness of the solver (0 = Box3D defaults); keep below half the step rate.
  f32 constraint_hertz = 0, constraint_damping_ratio = 0;
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

/// A contact plane of a character mover (relative to the mover's origin).
struct MoverPlane {
  Vec3 normal;            // outward, away from the shape
  f32 offset = 0;         // separation of a point p = dot(normal, p) - offset
  Vec3 point;             // closest point on the shape (world space)
  Shape shape;
  Body body;
};

/// Capsule along Z between two sphere centres (relative to an origin).
struct Capsule {
  Vec3 center1, center2;
  f32 radius = 0.3f;
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
  void set_body_type(Body body, BodyType type);
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
  /// Overrides all mass properties: mass (kg), centre of mass (body-local) and the inertia tensor
  /// about it in body axes (kg*m^2: xx yy zz xy xz yz), e.g. from a robot description (MJCF, URDF).
  void set_mass_data(Body body, f32 mass, Vec3 center, const f32 inertia[6]);
  /// Joints whose constraint force (N) or torque (N*m) exceeds these values
  /// are reported in joint_events() after a step (0 = never).
  void set_joint_thresholds(Joint joint, f32 force, f32 torque);
  Vec3 joint_force(Joint joint) const;
  /// Motor of a revolute (rad/s, N*m), prismatic (m/s, N), distance (m/s, N) or spherical joint (torque only).
  void set_joint_motor(Joint joint, bool enable, f32 speed, f32 max_force);
  /// Revolute joints: angle of B relative to A (radians, from the reference angle); 0 for others.
  f32 joint_angle(Joint joint) const;
  /// Revolute joints: angular speed of B relative to A about the hinge axis (rad/s); 0 for others.
  f32 joint_speed(Joint joint) const;
  /// Revolute joints: a spring-damper toward `target` (radians) in physical units, stiffness N*m/rad
  /// and damping N*m*s/rad. Box3D's spring is set in Hertz and a damping ratio relative to the
  /// joint's axial effective mass; this converts with the current mass, so call it every step.
  /// The spring is solved implicitly (soft step): stable for any stiffness. stiffness <= 0 disables it.
  void set_joint_spring(Joint joint, f32 target, f32 stiffness, f32 damping);
  Vec3 joint_torque(Joint joint) const;
  /// Touching contact points of a body (for debug views).
  void contact_points(Body body, std::vector<ContactPoint>& out) const;
  /// The bodies `body` touches now (a manifold with points), each with the summed normal impulse of
  /// the last sub-step (N*s): what a contact sensor reports (a sole on the floor, a link on a link).
  void touching(Body body, std::vector<std::pair<Body, f32>>& out) const;

  /// Closest hit along origin + translation (non-sensor shapes whose category matches
  /// mask_bits; shapes of `ignore` are skipped, e.g. the body the ray starts in).
  RayHit cast_ray(Vec3 origin, Vec3 translation, u64 mask_bits = ~0ull, Body ignore = {}) const;
  /// Closest hit that is not on any of `ignore` (a robot's sensor does not see its own links).
  RayHit cast_ray_ignoring(Vec3 origin, Vec3 translation, Span<const Body> ignore, u64 mask_bits = ~0ull) const;
  /// Closest hit of a sphere swept along translation (a fat ray: ground probes that do not miss ledges).
  RayHit cast_sphere(Vec3 origin, f32 radius, Vec3 translation, u64 mask_bits = ~0ull, Body ignore = {}) const;
  /// Character mover queries (Box3D "Character Mover"): how far the capsule can travel (0..1) and
  /// the planes it touches. Sensors and the shapes of `ignore` are skipped.
  f32 cast_mover(Vec3 origin, const Capsule& capsule, Vec3 translation, u64 mask_bits = ~0ull, Body ignore = {}) const;
  void collide_mover(Vec3 origin, const Capsule& capsule, u64 mask_bits, Body ignore, std::vector<MoverPlane>& out) const;
  /// Radial impulse on spheres, capsules and hulls (b3World_Explode); negative impulse implodes.
  void explode(Vec3 center, f32 radius, f32 falloff, f32 impulse_per_area, u64 mask_bits = ~0ull);
  /// Impulse a mover moving with `velocity` applies to a dynamic body at `point` along `normal` (into the body).
  void push_body(Body body, Vec3 point, Vec3 normal, Vec3 velocity);

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
  std::vector<void*> meshes_;  // b3MeshData of mesh shapes (freed after the world)
  std::vector<TouchEvent> contact_begin_, contact_end_, sensor_begin_, sensor_end_;
  std::vector<HitEvent> hits_;
  std::vector<Joint> joint_events_;
  u64 steps_ = 0;
};

/// Drags a dynamic body with a spring: a kinematic "mouse" body and a motor
/// joint (the same technique as the Box3D samples).
class MouseGrab {
 public:
  /// `hold_rotation`: the grab also keeps the body's orientation, as a hand holding it does (a
  /// point grab lets it swing and turn over).
  bool begin(World& world, Body body, Vec3 point, f32 stiffness_hertz = 7.5f, bool hold_rotation = false);
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

/// A kinematic character controller on Box3D's mover API (the Box3D "Mover"
/// sample, Z up): a capsule outside the solver that accelerates with friction,
/// rides a damped "pogo" spring over steps and slopes, slides along the
/// collision planes (b3SolvePlanes), clips its velocity (b3ClipVector) and
/// pushes dynamic bodies it walks into.
class CharacterMover {
 public:
  struct Settings {
    f32 radius = 0.4f;
    f32 height = 1.8f;           // feet to head
    f32 step_height = 0.45f;     // the pogo spring carries the capsule this high over the ground
    f32 max_speed = 6;           // m/s
    f32 accelerate = 10;         // 1/s
    f32 friction = 8;            // 1/s on the ground
    f32 air_control = 0.25f;
    f32 stop_speed = 3;          // m/s
    f32 jump_speed = 6;          // m/s
    f32 sprint = 1.6f;           // speed factor while sprinting (boost)
    f32 gravity = 20;            // m/s^2 (snappier than the world's)
    f32 max_slope_deg = 50;
    f32 push_strength = 1;       // 0 = dynamic bodies are not pushed
    u64 mask_bits = ~0ull;
  };
  Settings settings;
  /// `position` is the feet.
  void reset(Vec3 position) { position_ = position; velocity_ = {}; pogo_ = 0; on_ground_ = false; }
  /// One fixed step: `wish` is the desired horizontal direction * speed fraction (|wish| <= 1).
  /// Returns true when a jump started.
  bool step(World& world, f32 dt, Vec3 wish, bool jump, bool sprint, Body self);
  Vec3 position() const { return position_; }
  Vec3 velocity() const { return velocity_; }
  void set_velocity(Vec3 v) { velocity_ = v; }
  /// Leaves the ground with `speed` m/s upwards (the next step will not snap back down).
  void jump(f32 speed) { velocity_.z = speed; on_ground_ = false; pogo_ = 0; }
  bool on_ground() const { return on_ground_; }
  /// Planes touched in the last step (triggers and pickups the mover walked into).
  const std::vector<MoverPlane>& planes() const { return planes_; }
  Capsule capsule() const;   // relative to position()

 private:
  Vec3 position_, velocity_;
  f32 pogo_ = 0;
  bool on_ground_ = false;
  std::vector<MoverPlane> planes_;
};

/// Flat-shaded triangles of the convex hull of `points` (Box3D's hull builder).
/// Returns false when the points do not span a volume or exceed the hull limits.
bool hull_mesh(Span<const Vec3> points, MeshData& out, Vec3 color = {1, 1, 1});

}  // namespace kern::physics
