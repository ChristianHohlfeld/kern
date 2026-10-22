#include "kern/physics/physics.h"

#include <algorithm>

#include "box3d/box3d.h"
#include "box3d/collision.h"

namespace kern::physics {

namespace {

b3Vec3 bv(Vec3 v) { return {v.x, v.y, v.z}; }
Vec3 kv(b3Vec3 v) { return {v.x, v.y, v.z}; }
b3Quat bq(Quat q) {
  const f32 n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
  if (!(n > 1e-12f) || !std::isfinite(n)) return {{0, 0, 0}, 1};
  return {{q.x / n, q.y / n, q.z / n}, q.w / n};
}
Quat kq(b3Quat q) { return {q.v.x, q.v.y, q.v.z, q.s}; }
b3Transform bt(const Pose& p) { return {bv(p.position), bq(p.rotation)}; }

b3WorldId world_id(u32 w) { return b3LoadWorldId(w); }
b3BodyId body_id(Body b) { return b3LoadBodyId(b.id); }
b3ShapeId shape_id(Shape s) { return b3LoadShapeId(s.id); }
b3JointId joint_id(Joint j) { return b3LoadJointId(j.id); }
Body body_of(b3BodyId id) { return {B3_IS_NULL(id) ? 0 : b3StoreBodyId(id)}; }
Shape shape_of(b3ShapeId id) { return {B3_IS_NULL(id) ? 0 : b3StoreShapeId(id)}; }
Joint joint_of(b3JointId id) { return {B3_IS_NULL(id) ? 0 : b3StoreJointId(id)}; }

bool finite_vec(Vec3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
bool finite_quat(Quat q) { return std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.z) && std::isfinite(q.w); }

Body shape_body_checked(b3ShapeId s) { return b3Shape_IsValid(s) ? body_of(b3Shape_GetBody(s)) : Body{}; }

void fill_base(b3JointDef& base, const JointSettings& s) {
  base.bodyIdA = body_id(s.body_a);
  base.bodyIdB = body_id(s.body_b);
  base.localFrameA = bt(s.frame_a);
  base.localFrameB = bt(s.frame_b);
  base.collideConnected = s.collide_connected;
}

}  // namespace

Version version() {
  const b3Version v = b3GetVersion();
  return {v.major, v.minor, v.revision};
}

Pose frame_from_axis(Vec3 anchor, Vec3 axis, bool x_axis) {
  Pose p;
  p.position = anchor;
  const f32 len = length(axis);
  if (!(len > 1e-8f) || !finite_vec(axis)) return p;
  const Vec3 from = x_axis ? Vec3{1, 0, 0} : Vec3{0, 0, 1};
  const Vec3 to = axis / len;
  const f32 d = dot(from, to);
  if (d > 1 - 1e-7f) return p;
  if (d < -1 + 1e-7f) {
    // 180 degrees about any axis perpendicular to `from`.
    Vec3 ortho = cross(from, Vec3{1, 0, 0});
    if (length(ortho) < 1e-6f) ortho = cross(from, Vec3{0, 1, 0});
    ortho = normalize(ortho);
    p.rotation = {ortho.x, ortho.y, ortho.z, 0};
    return p;
  }
  const Vec3 c = cross(from, to);
  Quat q{c.x, c.y, c.z, 1 + d};
  const f32 n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
  p.rotation = {q.x / n, q.y / n, q.z / n, q.w / n};
  return p;
}

// ---------------------------------------------------------------------------
// world

bool World::create(const WorldSettings& s) {
  destroy();
  if (!finite_vec(s.gravity)) return fail("non-finite gravity");
  b3WorldDef def = b3DefaultWorldDef();
  def.gravity = bv(s.gravity);
  def.enableSleep = s.enable_sleep;
  def.enableContinuous = s.enable_continuous;
  def.restitutionThreshold = max_of(0.0f, s.restitution_threshold);
  def.restitutionIterations = std::clamp(s.restitution_iterations, 0, 63);
  def.enableRestitutionPropagation = s.restitution_propagation;
  def.hitEventThreshold = max_of(0.0f, s.hit_event_threshold);
  def.workerCount = 1;  // single-threaded WebAssembly: no scheduler threads
  const b3WorldId id = b3CreateWorld(&def);
  if (!b3World_IsValid(id)) return fail("Box3D could not create a world (too many worlds?)");
  world_ = b3StoreWorldId(id);
  settings_ = s;
  steps_ = 0;
  return true;
}

void World::destroy() {
  if (world_) {
    const b3WorldId id = world_id(world_);
    if (b3World_IsValid(id)) b3DestroyWorld(id);
  }
  world_ = 0;
  body_users_.clear();
  shape_users_.clear();
  contact_begin_.clear();
  contact_end_.clear();
  sensor_begin_.clear();
  sensor_end_.clear();
  hits_.clear();
  steps_ = 0;
}

bool World::valid() const { return world_ && b3World_IsValid(world_id(world_)); }

Body World::create_body(const BodySettings& s) {
  if (!valid()) { fail("physics world not created"); return {}; }
  if (!finite_vec(s.position) || !finite_quat(s.rotation) || !finite_vec(s.linear_velocity) || !finite_vec(s.angular_velocity)) {
    fail("non-finite body state");
    return {};
  }
  b3BodyDef def = b3DefaultBodyDef();
  def.type = s.type == BodyType::fixed ? b3_staticBody : s.type == BodyType::kinematic ? b3_kinematicBody : b3_dynamicBody;
  def.position = bv(s.position);
  def.rotation = bq(s.rotation);
  def.linearVelocity = bv(s.linear_velocity);
  def.angularVelocity = bv(s.angular_velocity);
  def.linearDamping = max_of(0.0f, s.linear_damping);
  def.angularDamping = max_of(0.0f, s.angular_damping);
  def.gravityScale = std::isfinite(s.gravity_scale) ? s.gravity_scale : 1.0f;
  def.enableSleep = s.enable_sleep;
  def.isAwake = s.awake;
  def.isEnabled = s.enabled;
  def.isBullet = s.bullet;
  const b3BodyId id = b3CreateBody(world_id(world_), &def);
  const Body body = body_of(id);
  if (!body.valid()) { fail("Box3D rejected the body"); return {}; }
  body_users_[body.id] = s.user;
  return body;
}

void World::destroy_body(Body body) {
  if (!body_valid(body)) return;
  const b3BodyId id = body_id(body);
  // Forget the shapes of this body.
  const int count = b3Body_GetShapeCount(id);
  if (count > 0) {
    std::vector<b3ShapeId> shapes(static_cast<std::size_t>(count));
    b3Body_GetShapes(id, shapes.data(), count);
    for (b3ShapeId s : shapes) shape_users_.erase(b3StoreShapeId(s));
  }
  b3DestroyBody(id);
  body_users_.erase(body.id);
}

bool World::body_valid(Body body) const { return valid() && body.valid() && b3Body_IsValid(body_id(body)); }

Shape World::create_shape(Body body, const ShapeSettings& s) {
  if (!body_valid(body)) { fail("shape needs a valid body"); return {}; }
  if (!finite_vec(s.center) || !finite_quat(s.rotation)) { fail("non-finite shape placement"); return {}; }
  const ShapeMaterial& m = s.material;
  b3ShapeDef def = b3DefaultShapeDef();
  def.density = max_of(0.0001f, std::isfinite(m.density) ? m.density : 1.0f);
  def.baseMaterial.friction = max_of(0.0f, std::isfinite(m.friction) ? m.friction : 0.0f);
  def.baseMaterial.restitution = std::clamp(std::isfinite(m.restitution) ? m.restitution : 0.0f, 0.0f, 1.0f);
  def.baseMaterial.rollingResistance = max_of(0.0f, std::isfinite(m.rolling_resistance) ? m.rolling_resistance : 0.0f);
  def.isSensor = m.sensor;
  def.filter.categoryBits = m.category_bits;
  def.filter.maskBits = m.mask_bits;
  def.filter.groupIndex = m.group_index;
  def.enableSensorEvents = m.sensor_events;
  def.enableContactEvents = m.contact_events;
  def.enableHitEvents = m.hit_events;
  const b3BodyId bid = body_id(body);
  b3ShapeId id = b3_nullShapeId;
  switch (s.kind) {
    case ShapeKind::sphere: {
      if (!(s.radius > 0) || !std::isfinite(s.radius)) { fail("sphere radius must be positive"); return {}; }
      const b3Sphere sphere{bv(s.center), s.radius};
      id = b3CreateSphereShape(bid, &def, &sphere);
      break;
    }
    case ShapeKind::capsule: {
      if (!(s.radius > 0) || !(s.length >= 0) || !std::isfinite(s.radius) || !std::isfinite(s.length)) {
        fail("capsule needs a positive radius and a non-negative length");
        return {};
      }
      const Vec3 axis = s.rotation.rotate(Vec3{0, 0, s.length * 0.5f});
      const b3Capsule capsule{bv(s.center - axis), bv(s.center + axis), s.radius};
      id = b3CreateCapsuleShape(bid, &def, &capsule);
      break;
    }
    case ShapeKind::box: {
      if (!(s.size.x > 0 && s.size.y > 0 && s.size.z > 0) || !finite_vec(s.size)) { fail("box size must be positive"); return {}; }
      const b3BoxHull box = b3MakeTransformedBoxHull(s.size.x * 0.5f, s.size.y * 0.5f, s.size.z * 0.5f, {bv(s.center), bq(s.rotation)});
      id = b3CreateHullShape(bid, &def, &box.base);
      break;
    }
    case ShapeKind::hull: {
      if (s.points.size < 4) { fail("a hull needs at least four points"); return {}; }
      std::vector<b3Vec3> pts;
      pts.reserve(s.points.size);
      for (const Vec3& p : s.points) {
        if (!finite_vec(p)) { fail("non-finite hull point"); return {}; }
        pts.push_back(bv(s.center + s.rotation.rotate(p)));
      }
      b3HullData* hull = b3CreateHull(pts.data(), static_cast<int>(pts.size()), B3_MAX_HULL_VERTICES);
      if (!hull) { fail("Box3D rejected the hull (flat, degenerate or too many features)"); return {}; }
      id = b3CreateHullShape(bid, &def, hull);
      b3DestroyHull(hull);
      break;
    }
  }
  const Shape shape = shape_of(id);
  if (!shape.valid()) { fail("Box3D rejected the shape"); return {}; }
  shape_users_[shape.id] = m.user;
  return shape;
}

Joint World::create_joint(const JointSettings& s) {
  if (!body_valid(s.body_a) || !body_valid(s.body_b)) { fail("joint needs two valid bodies"); return {}; }
  if (s.body_a == s.body_b) { fail("a joint cannot connect a body to itself"); return {}; }
  const b3WorldId w = world_id(world_);
  b3JointId id = b3_nullJointId;
  switch (s.type) {
    case JointType::revolute: {
      b3RevoluteJointDef d = b3DefaultRevoluteJointDef();
      fill_base(d.base, s);
      d.enableSpring = s.enable_spring;
      d.hertz = s.hertz;
      d.dampingRatio = s.damping_ratio;
      d.enableLimit = s.enable_limit;
      d.lowerAngle = min_of(s.lower, s.upper);
      d.upperAngle = max_of(s.lower, s.upper);
      d.enableMotor = s.enable_motor;
      d.motorSpeed = s.motor_speed;
      d.maxMotorTorque = max_of(0.0f, s.max_motor_torque);
      id = b3CreateRevoluteJoint(w, &d);
      break;
    }
    case JointType::distance: {
      b3DistanceJointDef d = b3DefaultDistanceJointDef();
      fill_base(d.base, s);
      d.length = max_of(0.001f, s.length);
      d.enableSpring = s.enable_spring;
      d.hertz = s.hertz;
      d.dampingRatio = s.damping_ratio;
      d.enableLimit = s.enable_limit;
      d.minLength = max_of(0.0f, s.lower);
      d.maxLength = max_of(d.minLength + 0.001f, s.upper);
      d.enableMotor = s.enable_motor;
      d.motorSpeed = s.motor_speed;
      d.maxMotorForce = max_of(0.0f, s.max_motor_force);
      id = b3CreateDistanceJoint(w, &d);
      break;
    }
    case JointType::prismatic: {
      b3PrismaticJointDef d = b3DefaultPrismaticJointDef();
      fill_base(d.base, s);
      d.enableSpring = s.enable_spring;
      d.hertz = s.hertz;
      d.dampingRatio = s.damping_ratio;
      d.enableLimit = s.enable_limit;
      d.lowerTranslation = min_of(s.lower, s.upper);
      d.upperTranslation = max_of(s.lower, s.upper);
      d.enableMotor = s.enable_motor;
      d.motorSpeed = s.motor_speed;
      d.maxMotorForce = max_of(0.0f, s.max_motor_force);
      id = b3CreatePrismaticJoint(w, &d);
      break;
    }
    case JointType::wheel: {
      b3WheelJointDef d = b3DefaultWheelJointDef();
      fill_base(d.base, s);
      d.enableSuspensionSpring = s.enable_spring;
      d.suspensionHertz = s.hertz;
      d.suspensionDampingRatio = s.damping_ratio;
      d.enableSuspensionLimit = s.enable_limit;
      d.lowerSuspensionLimit = min_of(s.lower, s.upper);
      d.upperSuspensionLimit = max_of(s.lower, s.upper);
      d.enableSpinMotor = s.enable_motor;
      d.spinSpeed = s.motor_speed;
      d.maxSpinTorque = max_of(0.0f, s.max_motor_torque);
      id = b3CreateWheelJoint(w, &d);
      break;
    }
    case JointType::weld: {
      b3WeldJointDef d = b3DefaultWeldJointDef();
      fill_base(d.base, s);
      d.linearHertz = max_of(0.0f, s.linear_hertz);
      d.angularHertz = max_of(0.0f, s.angular_hertz);
      d.linearDampingRatio = max_of(0.0f, s.linear_damping_ratio);
      d.angularDampingRatio = max_of(0.0f, s.angular_damping_ratio);
      id = b3CreateWeldJoint(w, &d);
      break;
    }
    case JointType::motor: {
      b3MotorJointDef d = b3DefaultMotorJointDef();
      fill_base(d.base, s);
      d.linearVelocity = bv(s.linear_velocity);
      d.angularVelocity = bv(s.angular_velocity);
      d.maxVelocityForce = max_of(0.0f, s.max_velocity_force);
      d.maxVelocityTorque = max_of(0.0f, s.max_velocity_torque);
      d.linearHertz = max_of(0.0f, s.linear_hertz);
      d.linearDampingRatio = max_of(0.0f, s.linear_damping_ratio);
      d.maxSpringForce = max_of(0.0f, s.max_spring_force);
      d.angularHertz = max_of(0.0f, s.angular_hertz);
      d.angularDampingRatio = max_of(0.0f, s.angular_damping_ratio);
      d.maxSpringTorque = max_of(0.0f, s.max_spring_torque);
      id = b3CreateMotorJoint(w, &d);
      break;
    }
    case JointType::spherical: {
      b3SphericalJointDef d = b3DefaultSphericalJointDef();
      fill_base(d.base, s);
      d.enableSpring = s.enable_spring;
      d.hertz = s.hertz;
      d.dampingRatio = s.damping_ratio;
      d.enableConeLimit = s.enable_limit;
      d.coneAngle = max_of(0.0f, s.upper);
      d.enableMotor = s.enable_motor;
      d.motorVelocity = bv(s.motor_velocity);
      d.maxMotorTorque = max_of(0.0f, s.max_motor_torque);
      id = b3CreateSphericalJoint(w, &d);
      break;
    }
    case JointType::parallel: {
      b3ParallelJointDef d = b3DefaultParallelJointDef();
      fill_base(d.base, s);
      d.hertz = max_of(0.0f, s.hertz);
      d.dampingRatio = max_of(0.0f, s.damping_ratio);
      d.maxTorque = max_of(0.0f, s.max_torque);
      id = b3CreateParallelJoint(w, &d);
      break;
    }
    case JointType::filter: {
      b3FilterJointDef d = b3DefaultFilterJointDef();
      d.base.bodyIdA = body_id(s.body_a);
      d.base.bodyIdB = body_id(s.body_b);
      id = b3CreateFilterJoint(w, &d);
      break;
    }
  }
  const Joint joint = joint_of(id);
  if (!joint.valid()) fail("Box3D rejected the joint");
  return joint;
}

void World::destroy_joint(Joint joint, bool wake_bodies) {
  if (joint_valid(joint)) b3DestroyJoint(joint_id(joint), wake_bodies);
}

bool World::joint_valid(Joint joint) const { return valid() && joint.valid() && b3Joint_IsValid(joint_id(joint)); }

void World::step(f32 dt, i32 substeps) {
  contact_begin_.clear();
  contact_end_.clear();
  sensor_begin_.clear();
  sensor_end_.clear();
  hits_.clear();
  if (!valid() || !(dt > 0) || !std::isfinite(dt)) return;
  b3World_Step(world_id(world_), dt, std::clamp(substeps, 1, 64));
  ++steps_;
  const b3ContactEvents ce = b3World_GetContactEvents(world_id(world_));
  for (int i = 0; i < ce.beginCount; ++i) {
    const b3ContactBeginTouchEvent& e = ce.beginEvents[i];
    contact_begin_.push_back({shape_of(e.shapeIdA), shape_of(e.shapeIdB), shape_body_checked(e.shapeIdA), shape_body_checked(e.shapeIdB)});
  }
  for (int i = 0; i < ce.endCount; ++i) {
    const b3ContactEndTouchEvent& e = ce.endEvents[i];
    contact_end_.push_back({shape_of(e.shapeIdA), shape_of(e.shapeIdB), shape_body_checked(e.shapeIdA), shape_body_checked(e.shapeIdB)});
  }
  for (int i = 0; i < ce.hitCount; ++i) {
    const b3ContactHitEvent& e = ce.hitEvents[i];
    hits_.push_back({shape_of(e.shapeIdA), shape_of(e.shapeIdB), shape_body_checked(e.shapeIdA), shape_body_checked(e.shapeIdB),
                     kv(e.point), kv(e.normal), e.approachSpeed});
  }
  const b3SensorEvents se = b3World_GetSensorEvents(world_id(world_));
  for (int i = 0; i < se.beginCount; ++i) {
    const b3SensorBeginTouchEvent& e = se.beginEvents[i];
    sensor_begin_.push_back({shape_of(e.sensorShapeId), shape_of(e.visitorShapeId), shape_body_checked(e.sensorShapeId),
                             shape_body_checked(e.visitorShapeId)});
  }
  for (int i = 0; i < se.endCount; ++i) {
    const b3SensorEndTouchEvent& e = se.endEvents[i];
    sensor_end_.push_back({shape_of(e.sensorShapeId), shape_of(e.visitorShapeId), shape_body_checked(e.sensorShapeId),
                           shape_body_checked(e.visitorShapeId)});
  }
}

Pose World::pose(Body body) const {
  if (!body_valid(body)) return {};
  const b3WorldTransform t = b3Body_GetTransform(body_id(body));
  return {kv(t.p), kq(t.q)};
}

void World::set_pose(Body body, const Pose& p) {
  if (body_valid(body) && finite_vec(p.position) && finite_quat(p.rotation)) b3Body_SetTransform(body_id(body), bv(p.position), bq(p.rotation));
}

void World::set_target_pose(Body body, const Pose& target, f32 dt) {
  if (body_valid(body) && finite_vec(target.position) && dt > 0) b3Body_SetTargetTransform(body_id(body), bt(target), dt, true);
}

Vec3 World::linear_velocity(Body body) const { return body_valid(body) ? kv(b3Body_GetLinearVelocity(body_id(body))) : Vec3{}; }
Vec3 World::angular_velocity(Body body) const { return body_valid(body) ? kv(b3Body_GetAngularVelocity(body_id(body))) : Vec3{}; }
void World::set_linear_velocity(Body body, Vec3 v) { if (body_valid(body) && finite_vec(v)) b3Body_SetLinearVelocity(body_id(body), bv(v)); }
void World::set_angular_velocity(Body body, Vec3 w) { if (body_valid(body) && finite_vec(w)) b3Body_SetAngularVelocity(body_id(body), bv(w)); }
void World::apply_force(Body body, Vec3 f, Vec3 p) { if (body_valid(body) && finite_vec(f) && finite_vec(p)) b3Body_ApplyForce(body_id(body), bv(f), bv(p), true); }
void World::apply_force_to_center(Body body, Vec3 f) { if (body_valid(body) && finite_vec(f)) b3Body_ApplyForceToCenter(body_id(body), bv(f), true); }
void World::apply_torque(Body body, Vec3 t) { if (body_valid(body) && finite_vec(t)) b3Body_ApplyTorque(body_id(body), bv(t), true); }
void World::apply_linear_impulse(Body body, Vec3 i, Vec3 p) {
  if (body_valid(body) && finite_vec(i) && finite_vec(p)) b3Body_ApplyLinearImpulse(body_id(body), bv(i), bv(p), true);
}
void World::apply_linear_impulse_to_center(Body body, Vec3 i) { if (body_valid(body) && finite_vec(i)) b3Body_ApplyLinearImpulseToCenter(body_id(body), bv(i), true); }
void World::apply_angular_impulse(Body body, Vec3 i) { if (body_valid(body) && finite_vec(i)) b3Body_ApplyAngularImpulse(body_id(body), bv(i), true); }
void World::set_awake(Body body, bool a) { if (body_valid(body)) b3Body_SetAwake(body_id(body), a); }
bool World::awake(Body body) const { return body_valid(body) && b3Body_IsAwake(body_id(body)); }
f32 World::mass(Body body) const { return body_valid(body) ? b3Body_GetMass(body_id(body)) : 0.0f; }
BodyType World::body_type(Body body) const {
  if (!body_valid(body)) return BodyType::fixed;
  const b3BodyType t = b3Body_GetType(body_id(body));
  return t == b3_staticBody ? BodyType::fixed : t == b3_kinematicBody ? BodyType::kinematic : BodyType::dynamic;
}
Vec3 World::local_point(Body body, Vec3 p) const { return body_valid(body) ? kv(b3Body_GetLocalPoint(body_id(body), bv(p))) : p; }
Vec3 World::world_point(Body body, Vec3 p) const { return body_valid(body) ? kv(b3Body_GetWorldPoint(body_id(body), bv(p))) : p; }
Vec3 World::world_vector(Body body, Vec3 v) const { return body_valid(body) ? kv(b3Body_GetWorldVector(body_id(body), bv(v))) : v; }
u64 World::body_user(Body body) const {
  const auto it = body_users_.find(body.id);
  return it == body_users_.end() ? 0 : it->second;
}
u64 World::shape_user(Shape shape) const {
  const auto it = shape_users_.find(shape.id);
  return it == shape_users_.end() ? 0 : it->second;
}
Body World::shape_body(Shape shape) const { return valid() && shape.valid() ? shape_body_checked(shape_id(shape)) : Body{}; }

RayHit World::cast_ray(Vec3 origin, Vec3 translation, u64 mask_bits, Body ignore) const {
  RayHit hit;
  if (!valid() || !finite_vec(origin) || !finite_vec(translation)) return hit;
  b3QueryFilter filter = b3DefaultQueryFilter();
  filter.maskBits = mask_bits;
  // Closest non-sensor hit: returning the fraction clips the ray, -1 skips a shape.
  struct Closest {
    u64 ignore = 0;
    b3ShapeId shape = b3_nullShapeId;
    b3Pos point{};
    b3Vec3 normal{};
    f32 fraction = 1;
  } closest;
  closest.ignore = ignore.id;
  auto callback = [](b3ShapeId shape, b3Pos point, b3Vec3 normal, float fraction, uint64_t, int, int, void* context) -> float {
    Closest* c = static_cast<Closest*>(context);
    if (b3Shape_IsSensor(shape) || (c->ignore && b3StoreBodyId(b3Shape_GetBody(shape)) == c->ignore)) return -1.0f;
    c->shape = shape;
    c->point = point;
    c->normal = normal;
    c->fraction = fraction;
    return fraction;
  };
  b3World_CastRay(world_id(world_), bv(origin), bv(translation), filter, callback, &closest);
  if (B3_IS_NULL(closest.shape)) return hit;
  hit.hit = true;
  hit.shape = shape_of(closest.shape);
  hit.body = shape_body_checked(closest.shape);
  hit.point = kv(closest.point);
  hit.normal = kv(closest.normal);
  hit.fraction = closest.fraction;
  return hit;
}

i32 World::awake_body_count() const { return valid() ? b3World_GetAwakeBodyCount(world_id(world_)) : 0; }

// ---------------------------------------------------------------------------
// mouse grab

bool MouseGrab::begin(World& world, Body body, Vec3 point, f32 stiffness_hertz) {
  end();
  if (!world.body_valid(body) || world.body_type(body) != BodyType::dynamic || !finite_vec(point)) return false;
  BodySettings ms;
  ms.type = BodyType::kinematic;
  ms.position = point;
  ms.enable_sleep = false;
  const Body mouse = world.create_body(ms);
  if (!mouse.valid()) return false;
  JointSettings js;
  js.type = JointType::motor;
  js.body_a = mouse;
  js.body_b = body;
  js.frame_b.position = world.local_point(body, point);
  js.linear_hertz = stiffness_hertz;
  js.linear_damping_ratio = 1;
  const f32 g = length(world.gravity());
  js.max_spring_force = 100 * max_of(0.001f, world.mass(body)) * max_of(g, 1.0f);
  js.angular_hertz = 0;
  js.max_spring_torque = 0;
  const Joint joint = world.create_joint(js);
  if (!joint.valid()) { world.destroy_body(mouse); return false; }
  world_ = &world;
  mouse_ = mouse;
  body_ = body;
  joint_ = joint;
  target_ = point;
  world.set_awake(body, true);
  return true;
}

void MouseGrab::update(f32 dt) {
  if (!world_ || !(dt > 0)) return;
  if (!world_->body_valid(body_)) { end(); return; }
  world_->set_target_pose(mouse_, Pose{target_, Quat{}}, dt);
}

void MouseGrab::end() {
  if (world_) {
    world_->destroy_joint(joint_, true);
    world_->destroy_body(mouse_);
  }
  world_ = nullptr;
  mouse_ = body_ = {};
  joint_ = {};
}

// ---------------------------------------------------------------------------
// hull geometry

bool hull_mesh(Span<const Vec3> points, MeshData& out, Vec3 color) {
  out.clear();
  if (points.size < 4 || points.size > 4096) return false;
  std::vector<b3Vec3> pts;
  pts.reserve(points.size);
  for (const Vec3& p : points) {
    if (!finite_vec(p)) return false;
    pts.push_back(bv(p));
  }
  b3HullData* hull = b3CreateHull(pts.data(), static_cast<int>(pts.size()), B3_MAX_HULL_VERTICES);
  if (!hull) return false;
  const b3Vec3* hp = b3GetHullPoints(hull);
  const b3HullHalfEdge* edges = b3GetHullEdges(hull);
  const b3HullFace* faces = b3GetHullFaces(hull);
  const b3Plane* planes = b3GetHullPlanes(hull);
  for (int f = 0; f < hull->faceCount; ++f) {
    const Vec3 n = kv(planes[f].normal);
    std::vector<Vec3> poly;
    int e = faces[f].edge, guard = 0;
    do {
      poly.push_back(kv(hp[edges[e].origin]));
      e = edges[e].next;
    } while (e != faces[f].edge && ++guard < B3_MAX_HULL_EDGES);
    for (std::size_t k = 1; k + 1 < poly.size(); ++k) {
      Vec3 a = poly[0], b = poly[k], c = poly[k + 1];
      if (dot(cross(b - a, c - a), n) < 0) std::swap(b, c);
      out.push(a, n, color, {0, 0});
      out.push(b, n, color, {0, 0});
      out.push(c, n, color, {0, 0});
    }
  }
  b3DestroyHull(hull);
  return out.vertex_count() >= 12;
}

}  // namespace kern::physics
