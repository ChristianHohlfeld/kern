#include "kern/world/runtime.h"

#include <algorithm>

namespace kern::world {

namespace {

/// JavaScript `a || b` for numbers (0 and NaN fall through).
f64 orv(f64 a, f64 b) { return a != 0 && !std::isnan(a) ? a : b; }
f32 f(f64 v) { return static_cast<f32>(v); }

physics::Pose joint_frame(const D3& anchor, const D3& axis, bool x_axis) {
  physics::Pose p;
  p.position = to_f(anchor);
  if (finite(axis) && length(axis) > 1e-8) {
    const D3 from = x_axis ? D3{1, 0, 0} : D3{0, 0, 1};
    p.rotation = to_f(qfrom_unit_vectors(from, normalized(axis)));
  }
  return p;
}

f64 unknown_number(const Joint& j, std::string_view key) {
  const json::Value* v = j.unknown.find(key);
  return v ? v->as_number(0) : 0;
}

const std::string kNoName;

}  // namespace

physics::BodySettings body_settings(const Body& b) {
  physics::BodySettings s;
  s.type = b.type == BodyKind::fixed ? physics::BodyType::fixed
           : b.type == BodyKind::kinematic ? physics::BodyType::kinematic
                                           : physics::BodyType::dynamic;
  s.position = to_f(b.position);
  s.rotation = to_f(b.rotation);
  s.linear_velocity = to_f(b.linear_velocity);
  s.angular_velocity = to_f(b.angular_velocity);
  s.linear_damping = f(b.linear_damping);
  s.angular_damping = f(b.angular_damping);
  s.gravity_scale = f(b.gravity_scale);
  s.enable_sleep = b.allow_sleep;
  s.awake = b.awake;
  s.enabled = b.active;
  s.bullet = b.bullet;
  return s;
}

physics::ShapeSettings shape_settings(const Fixture& fx, std::vector<Vec3>& points) {
  physics::ShapeSettings s;
  s.center = to_f(fx.center);
  s.rotation = to_f(fx.rotation);
  s.size = to_f(fx.size);
  s.radius = f(fx.radius);
  s.length = f(fx.length);
  switch (fx.shape) {
    case FixtureShape::box: s.kind = physics::ShapeKind::box; break;
    case FixtureShape::sphere: s.kind = physics::ShapeKind::sphere; break;
    case FixtureShape::capsule: s.kind = physics::ShapeKind::capsule; break;
    case FixtureShape::hull:
      s.kind = physics::ShapeKind::hull;
      points.clear();
      for (const D3& p : fx.vertices) points.push_back(to_f(p));
      s.points = Span<const Vec3>(points.data(), points.size());
      break;
  }
  physics::ShapeMaterial& m = s.material;
  m.density = f(std::max(0.0001, orv(fx.density, 0)));
  m.friction = f(std::max(0.0, orv(fx.friction, 0)));
  m.restitution = f(std::clamp(orv(fx.restitution, 0), 0.0, 1.0));
  m.rolling_resistance = f(std::max(0.0, orv(fx.rolling_resistance, 0)));
  m.sensor = fx.sensor;
  // Editor3D stores 32-bit masks (JavaScript >>> 0).
  m.category_bits = static_cast<u32>(static_cast<i64>(fx.category_bits));
  m.mask_bits = static_cast<u32>(static_cast<i64>(fx.mask_bits));
  m.group_index = static_cast<i32>(fx.group_index);
  m.contact_events = true;
  m.sensor_events = true;
  return s;
}

physics::JointSettings joint_settings(const Joint& j, physics::Body a, physics::Body b) {
  using physics::JointType;
  physics::JointSettings s;
  s.body_a = a;
  s.body_b = b;
  const bool x_axis = j.type == JointKind::prismatic || j.type == JointKind::wheel;
  s.frame_a = joint_frame(j.anchor_a, j.axis_a, x_axis);
  s.frame_b = joint_frame(j.anchor_b, j.axis_b_or_a(), x_axis);
  s.collide_connected = j.collide_connected;
  switch (j.type) {
    case JointKind::revolute:
      s.type = JointType::revolute;
      s.enable_limit = j.enable_limit;
      s.lower = f(j.lower_limit);
      s.upper = f(j.upper_limit);
      s.enable_motor = j.enable_motor;
      s.motor_speed = f(j.motor_speed);
      s.max_motor_torque = f(j.max_motor_torque);
      break;
    case JointKind::distance:
    case JointKind::rope:
      s.type = JointType::distance;
      s.length = f(std::max(0.001, j.length));
      s.enable_spring = j.enable_spring;
      s.hertz = f(orv(j.frequency, orv(j.spring_frequency, 0)));
      s.damping_ratio = f(j.damping_ratio ? *j.damping_ratio : j.spring_damping_ratio ? *j.spring_damping_ratio : 0);
      if (j.type == JointKind::rope) {
        s.enable_limit = true;
        s.lower = 0;
        s.upper = f(std::max(0.001, orv(j.max_length, j.length)));
      } else {
        s.enable_limit = j.enable_limit;
        s.lower = f(std::max(0.0, orv(j.lower_limit, 0)));
        s.upper = f(std::max(static_cast<f64>(s.lower) + 0.001, orv(j.upper_limit, j.length)));
      }
      s.enable_motor = j.enable_motor;
      s.motor_speed = f(orv(j.motor_speed, 0));
      s.max_motor_force = f(orv(j.max_motor_force, 0));
      break;
    case JointKind::prismatic:
      s.type = JointType::prismatic;
      s.enable_limit = j.enable_limit;
      s.lower = f(j.lower_limit);
      s.upper = f(j.upper_limit);
      s.enable_motor = j.enable_motor;
      s.motor_speed = f(j.motor_speed);
      s.max_motor_force = f(j.max_motor_force);
      break;
    case JointKind::wheel:
      s.type = JointType::wheel;
      s.enable_spring = true;
      s.hertz = f(orv(j.spring_frequency, 4));
      s.damping_ratio = f(j.spring_damping_ratio ? *j.spring_damping_ratio : 0.7);
      s.enable_limit = j.enable_limit;
      s.lower = f(j.lower_limit);
      s.upper = f(j.upper_limit);
      s.enable_motor = j.enable_motor;
      s.motor_speed = f(orv(j.motor_speed, 0));
      s.max_motor_torque = f(orv(j.max_motor_torque, 0));
      break;
    case JointKind::weld:
      s.type = JointType::weld;
      s.linear_hertz = s.angular_hertz = f(orv(j.frequency, 0));
      s.linear_damping_ratio = s.angular_damping_ratio = f(j.damping_ratio ? *j.damping_ratio : 1);
      break;
    case JointKind::spherical:
      s.type = JointType::spherical;
      s.enable_spring = j.enable_spring;
      s.hertz = f(orv(j.frequency, 0));
      s.damping_ratio = f(j.damping_ratio ? *j.damping_ratio : 0.7);
      s.enable_limit = j.enable_limit;
      s.upper = f(std::max(0.0, orv(j.upper_limit, 0)));
      s.enable_motor = j.enable_motor;
      s.motor_velocity = {0, 0, f(orv(j.motor_speed, 0))};
      s.max_motor_torque = f(orv(j.max_motor_torque, 0));
      break;
    case JointKind::motor:
    case JointKind::friction: {
      const bool friction = j.type == JointKind::friction;
      s.type = JointType::motor;
      s.max_velocity_force = f(friction ? orv(j.max_force, orv(j.max_motor_force, 100))
                                        : orv(unknown_number(j, "maxVelocityForce"), orv(j.max_motor_force, 100)));
      s.max_velocity_torque = f(friction ? orv(j.max_torque, orv(j.max_motor_torque, 100))
                                         : orv(unknown_number(j, "maxVelocityTorque"), orv(j.max_motor_torque, 100)));
      s.linear_hertz = f(friction ? 0 : orv(j.frequency, 5));
      s.linear_damping_ratio = f(j.damping_ratio ? *j.damping_ratio : 1);
      s.max_spring_force = f(friction ? 0 : orv(j.max_motor_force, 100));
      s.angular_hertz = f(friction ? 0 : orv(j.frequency, 5));
      s.angular_damping_ratio = f(j.damping_ratio ? *j.damping_ratio : 1);
      s.max_spring_torque = f(friction ? 0 : orv(j.max_motor_torque, 100));
      break;
    }
    case JointKind::parallel:
      s.type = JointType::parallel;
      s.hertz = f(orv(j.frequency, 5));
      s.damping_ratio = f(j.damping_ratio ? *j.damping_ratio : 1);
      s.max_torque = f(orv(j.max_motor_torque, 100));
      break;
    case JointKind::filter:
      s.type = JointType::filter;
      s.frame_a = s.frame_b = {};
      break;
  }
  return s;
}

// ---------------------------------------------------------------------------

bool SceneRuntime::build(const Scene& scene) {
  destroy();
  error_.clear();
  const std::vector<std::string> errors = validate(scene);
  if (!errors.empty()) {
    for (const std::string& e : errors) error_ += (error_.empty() ? "" : "\n") + e;
    return false;
  }
  source_ = scene;
  physics::WorldSettings ws;
  ws.gravity = to_f(scene.world.gravity);
  ws.enable_sleep = scene.world.allow_sleep;
  ws.enable_continuous = scene.world.continuous_physics;
  ws.restitution_threshold = f(std::max(0.0, scene.world.restitution_threshold));
  ws.restitution_iterations = static_cast<i32>(std::clamp(std::round(scene.world.restitution_iterations), 0.0, 63.0));
  ws.restitution_propagation = scene.world.restitution_propagation;
  if (!world_.create(ws)) { error_ = last_error(); return false; }
  std::vector<Vec3> points;
  for (const Body& b : scene.bodies) {
    const physics::Body pb = world_.create_body(body_settings(b));
    if (!pb.valid()) { error_ = b.name + ": " + last_error(); destroy(); return false; }
    bodies_[b.id] = pb;
    names_[pb.id] = b.id;
    scene_order_.push_back(b.id);
    for (const Fixture& fx : b.fixtures) {
      if (!world_.create_shape(pb, shape_settings(fx, points)).valid()) {
        error_ = fx.shape == FixtureShape::hull ? "Box3D rejected hull: " + fx.name : fx.name + ": " + last_error();
        destroy();
        return false;
      }
    }
  }
  for (const Joint& j : scene.joints)
    if (!create_joint(j)) { destroy(); return false; }
  accumulator_ = 0;
  game_begin();
  return true;
}

bool SceneRuntime::create_joint(const Joint& j) {
  const auto a = bodies_.find(j.body_a), b = bodies_.find(j.body_b);
  if (a == bodies_.end() || b == bodies_.end()) { error_ = j.name + ": missing physics body"; return false; }
  if (!world_.create_joint(joint_settings(j, a->second, b->second)).valid()) { error_ = j.name + ": " + last_error(); return false; }
  return true;
}

void SceneRuntime::destroy() {
  grab_.end();
  world_.destroy();
  bodies_.clear();
  names_.clear();
  scene_order_.clear();
  pickups_alive_.clear();
  accumulator_ = 0;
  game_ = {};
  jump_latch_ = false;
}

bool SceneRuntime::restart() {
  const Scene copy = source_;
  return build(copy);
}

u32 SceneRuntime::advance(f64 dt) {
  if (!valid() || !(dt > 0)) return 0;
  accumulator_ += std::min(dt, 0.1);
  const f64 step = step_seconds();
  u32 count = 0;
  while (accumulator_ >= step && count < 240) {
    step_once();
    accumulator_ -= step;
    ++count;
  }
  return count;
}

void SceneRuntime::step_once() {
  if (!valid()) return;
  const f32 dt = f(step_seconds());
  grab_.update(dt);
  game_pre_step(dt);
  world_.step(dt, static_cast<i32>(source_.world.sub_step_count));
  game_post_step(dt);
}

bool SceneRuntime::body_pose(const std::string& id, DPose& out) const {
  const auto it = bodies_.find(id);
  if (it == bodies_.end() || !world_.body_valid(it->second)) return false;
  const physics::Pose p = world_.pose(it->second);
  out = {to_d(p.position), to_d(p.rotation)};
  return true;
}

bool SceneRuntime::body_alive(const std::string& id) const {
  const auto it = bodies_.find(id);
  return it != bodies_.end() && world_.body_valid(it->second);
}

physics::Body SceneRuntime::physics_body(const std::string& id) const {
  const auto it = bodies_.find(id);
  return it == bodies_.end() ? physics::Body{} : it->second;
}

const std::string& SceneRuntime::body_id(physics::Body body) const {
  const auto it = names_.find(body.id);
  return it == names_.end() ? kNoName : it->second;
}

bool SceneRuntime::begin_grab(const std::string& id, Vec3 point) {
  const Body* src = source_.body(id);
  if (!src || src->type != BodyKind::dynamic) return false;
  return grab_.begin(world_, physics_body(id), point);
}

// ---------------------------------------------------------------------------
// game components

void SceneRuntime::set_input(const GameInput& input) {
  if (input.jump && !input_.jump) jump_latch_ = true;
  input_ = input;
}

void SceneRuntime::game_begin() {
  game_ = {};
  has_goal_ = false;
  for (const Body& b : source_.bodies) {
    if (b.game.role == GameRole::player && game_.player.empty()) game_.player = b.id;
    if (b.game.camera_follow && game_.camera_body.empty()) game_.camera_body = b.id;
    if (b.game.role == GameRole::pickup) { ++game_.pickups_total; pickups_alive_.push_back(b.id); }
    if (b.game.role == GameRole::goal) has_goal_ = true;
  }
  if (game_.camera_body.empty()) game_.camera_body = game_.player;
  if (!game_.player.empty()) {
    game_.status = GameStatus::playing;
    game_.message = source_.game.title.empty() ? "Go!" : source_.game.title;
  }
}

bool SceneRuntime::grounded(physics::Body body, f32 probe) const {
  // A ray down from the body's origin, a little longer than the body's reach; the body itself is ignored.
  const physics::Pose p = world_.pose(body);
  return world_.cast_ray(p.position, Vec3{0, 0, -probe}, ~0ull, body).hit;
}

void SceneRuntime::game_pre_step(f32 dt) {
  if (game_.status != GameStatus::playing) { jump_latch_ = false; return; }
  Vec3 fwd{input_.camera_forward.x, input_.camera_forward.y, 0};
  if (length(fwd) < 1e-6f) fwd = {0, 1, 0};
  fwd = normalize(fwd);
  const Vec3 right{fwd.y, -fwd.x, 0};
  Vec3 move = right * clamp(input_.move_x, -1.0f, 1.0f) + fwd * clamp(input_.move_y, -1.0f, 1.0f);
  if (length(move) > 1) move = normalize(move);
  const bool jump = jump_latch_;
  jump_latch_ = false;
  for (const Body& b : source_.bodies) {
    if (b.game.role != GameRole::player || b.game.control == GameControl::none) continue;
    const physics::Body pb = physics_body(b.id);
    if (!world_.body_valid(pb) || world_.body_type(pb) != physics::BodyType::dynamic) continue;
    const f32 m = max_of(0.001f, world_.mass(pb)), a = f(b.game.force);
    const Vec3 v = world_.linear_velocity(pb);
    switch (b.game.control) {
      case GameControl::roll:
        world_.apply_force_to_center(pb, move * (a * m));
        break;
      case GameControl::drive: {
        Vec3 forward = world_.world_vector(pb, {1, 0, 0});
        forward.z = 0;
        if (length(forward) > 1e-4f) forward = normalize(forward);
        world_.apply_force_to_center(pb, forward * (clamp(input_.move_y, -1.0f, 1.0f) * a * m));
        world_.apply_torque(pb, Vec3{0, 0, -clamp(input_.move_x, -1.0f, 1.0f) * a * m * 0.5f});
        break;
      }
      case GameControl::fly: {
        const Vec3 g = world_.gravity() * f(b.gravity_scale);
        world_.apply_force_to_center(pb, (g * -1.0f + move * a + Vec3{0, 0, clamp(input_.up, -1.0f, 1.0f) * a} - v * 0.8f) * m);
        break;
      }
      case GameControl::walk: {
        const Vec3 target = move * a;
        const Vec3 dv{target.x - v.x, target.y - v.y, 0};
        world_.apply_linear_impulse_to_center(pb, dv * (m * min_of(1.0f, 10 * dt)));
        world_.set_angular_velocity(pb, {0, 0, 0});
        break;
      }
      case GameControl::none: break;
    }
    f64 reach = 0.1;
    for (const Fixture& fx : b.fixtures) reach = std::max(reach, length(fx.center) + fixture_radius(fx));
    if (jump && b.game.jump > 0 && b.game.control != GameControl::fly && grounded(pb, f(reach + 0.15)))
      world_.set_linear_velocity(pb, {v.x, v.y, f(b.game.jump)});
  }
}

void SceneRuntime::finish(GameStatus status, std::string message) {
  if (game_.status != GameStatus::playing) return;
  game_.status = status;
  game_.message = std::move(message);
}

void SceneRuntime::game_post_step(f32 dt) {
  if (game_.status != GameStatus::playing) return;
  game_.elapsed += dt;
  auto role_of = [&](const std::string& id) -> const Body* { return id.empty() ? nullptr : source_.body(id); };
  auto handle = [&](const std::string& ida, const std::string& idb) {
    const Body* a = role_of(ida);
    const Body* b = role_of(idb);
    if (!a || !b) return;
    if (b->game.role == GameRole::player) std::swap(a, b);
    if (a->game.role != GameRole::player || b->game.role == GameRole::player) return;
    switch (b->game.role) {
      case GameRole::pickup: {
        auto it = std::find(pickups_alive_.begin(), pickups_alive_.end(), b->id);
        if (it == pickups_alive_.end()) return;
        pickups_alive_.erase(it);
        world_.destroy_body(physics_body(b->id));
        game_.score += b->game.points;
        ++game_.pickups_collected;
        if (source_.game.goal == GameGoal::collect_all && game_.pickups_collected >= game_.pickups_total) {
          // With a goal body in the scene the level ends there; otherwise now.
          if (has_goal_) game_.message = "All collected - reach the goal!";
          else finish(GameStatus::won, "All collected!");
        }
        break;
      }
      case GameRole::goal:
        if (source_.game.goal == GameGoal::collect_all && game_.pickups_collected < game_.pickups_total) return;
        if (source_.game.goal != GameGoal::survive) finish(GameStatus::won, "Goal reached!");
        break;
      case GameRole::hazard: finish(GameStatus::lost, "Hit a hazard"); break;
      default: break;
    }
  };
  for (const physics::TouchEvent& e : world_.contact_begin()) handle(body_id(e.body_a), body_id(e.body_b));
  for (const physics::TouchEvent& e : world_.sensor_begin()) handle(body_id(e.body_a), body_id(e.body_b));
  if (game_.status != GameStatus::playing) return;
  DPose p;
  for (const Body& b : source_.bodies)
    if (b.game.role == GameRole::player && body_pose(b.id, p) && p.position[2] < source_.game.fall_limit) {
      finish(GameStatus::lost, "Fell off the world");
      return;
    }
  if (source_.game.time_limit > 0 && game_.elapsed >= source_.game.time_limit) {
    if (source_.game.goal == GameGoal::survive) finish(GameStatus::won, "Survived!");
    else finish(GameStatus::lost, "Time is up");
  }
}

}  // namespace kern::world
