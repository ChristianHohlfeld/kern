#include "kern/physics/physics.h"

#include <algorithm>
#include <cfloat>

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
  if (s.constraint_hertz > 0) base.constraintHertz = s.constraint_hertz;
  if (s.constraint_damping_ratio > 0) base.constraintDampingRatio = s.constraint_damping_ratio;
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
  if (s.contact_hertz > 0) def.contactHertz = s.contact_hertz;
  if (s.contact_damping_ratio > 0) def.contactDampingRatio = s.contact_damping_ratio;
  if (s.contact_speed > 0) def.contactSpeed = s.contact_speed;
  if (s.maximum_linear_speed > 0) def.maximumLinearSpeed = s.maximum_linear_speed;
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
  for (void* m : meshes_) b3DestroyMesh(static_cast<b3MeshData*>(m));
  meshes_.clear();
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
    case ShapeKind::mesh: {
      // Triangle meshes collide only on static bodies (Box3D "Triangle Meshes").
      if (b3Body_GetType(bid) != b3_staticBody) { fail("triangle meshes need a static body"); return {}; }
      if (s.points.size < 3 || s.indices.size < 3 || s.indices.size % 3) { fail("a mesh needs vertices and whole triangles"); return {}; }
      std::vector<b3Vec3> verts;
      verts.reserve(s.points.size);
      for (const Vec3& p : s.points) {
        if (!finite_vec(p)) { fail("non-finite mesh vertex"); return {}; }
        verts.push_back(bv(s.center + s.rotation.rotate(p)));
      }
      std::vector<int32_t> idx;
      idx.reserve(s.indices.size);
      for (u32 i : s.indices) {
        if (i >= s.points.size) { fail("mesh index out of range"); return {}; }
        idx.push_back(static_cast<int32_t>(i));
      }
      b3MeshDef md{};
      md.vertices = verts.data();
      md.vertexCount = static_cast<int>(verts.size());
      md.indices = idx.data();
      md.triangleCount = static_cast<int>(idx.size() / 3);
      md.weldVertices = true;
      md.weldTolerance = 0.0005f;
      md.identifyEdges = true;  // no ghost collisions on internal edges
      b3MeshData* mesh = b3CreateMesh(&md, nullptr, 0);
      if (!mesh) { fail("Box3D rejected the mesh (degenerate triangles?)"); return {}; }
      id = b3CreateMeshShape(bid, &def, mesh, b3Vec3{1, 1, 1});
      if (B3_IS_NULL(id)) { b3DestroyMesh(mesh); break; }
      meshes_.push_back(mesh);
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

void World::set_joint_motor(Joint joint, bool enable, f32 speed, f32 max_force) {
  if (!joint_valid(joint)) return;
  const b3JointId id = joint_id(joint);
  switch (b3Joint_GetType(id)) {
    case b3_revoluteJoint:
      b3RevoluteJoint_EnableMotor(id, enable);
      b3RevoluteJoint_SetMotorSpeed(id, speed);
      b3RevoluteJoint_SetMaxMotorTorque(id, max_force);
      break;
    case b3_prismaticJoint:
      b3PrismaticJoint_EnableMotor(id, enable);
      b3PrismaticJoint_SetMotorSpeed(id, speed);
      b3PrismaticJoint_SetMaxMotorForce(id, max_force);
      break;
    case b3_distanceJoint:
      b3DistanceJoint_EnableMotor(id, enable);
      b3DistanceJoint_SetMotorSpeed(id, speed);
      b3DistanceJoint_SetMaxMotorForce(id, max_force);
      break;
    case b3_sphericalJoint:
      b3SphericalJoint_EnableMotor(id, enable);
      b3SphericalJoint_SetMaxMotorTorque(id, max_force);
      break;
    default: break;
  }
  b3Joint_WakeBodies(id);
}

void World::step(f32 dt, i32 substeps) {
  contact_begin_.clear();
  contact_end_.clear();
  sensor_begin_.clear();
  sensor_end_.clear();
  hits_.clear();
  joint_events_.clear();
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
  const b3JointEvents je = b3World_GetJointEvents(world_id(world_));
  for (int i = 0; i < je.count; ++i) joint_events_.push_back(joint_of(je.jointEvents[i].jointId));
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

void World::set_body_type(Body body, BodyType type) {
  if (!body_valid(body)) return;
  b3Body_SetType(body_id(body), type == BodyType::dynamic ? b3_dynamicBody : type == BodyType::kinematic ? b3_kinematicBody : b3_staticBody);
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

void World::set_motion_locks(Body body, const MotionLocks& l) {
  if (!body_valid(body)) return;
  b3MotionLocks m{};
  m.linearX = l.linear[0];
  m.linearY = l.linear[1];
  m.linearZ = l.linear[2];
  m.angularX = l.angular[0];
  m.angularY = l.angular[1];
  m.angularZ = l.angular[2];
  b3Body_SetMotionLocks(body_id(body), m);
}

void World::set_mass(Body body, f32 mass) {
  if (!body_valid(body) || !(mass > 0) || !std::isfinite(mass)) return;
  b3MassData d = b3Body_GetMassData(body_id(body));
  if (!(d.mass > 0)) return;
  const f32 k = mass / d.mass;
  d.mass = mass;
  d.inertia.cx = b3MulSV(k, d.inertia.cx);
  d.inertia.cy = b3MulSV(k, d.inertia.cy);
  d.inertia.cz = b3MulSV(k, d.inertia.cz);
  b3Body_SetMassData(body_id(body), d);
}

void World::set_joint_thresholds(Joint joint, f32 force, f32 torque) {
  if (!joint_valid(joint)) return;
  // Box3D reports a joint when it exceeds the threshold; FLT_MAX disables it.
  b3Joint_SetForceThreshold(joint_id(joint), force > 0 && std::isfinite(force) ? force : FLT_MAX);
  b3Joint_SetTorqueThreshold(joint_id(joint), torque > 0 && std::isfinite(torque) ? torque : FLT_MAX);
}

Vec3 World::joint_force(Joint joint) const { return joint_valid(joint) ? kv(b3Joint_GetConstraintForce(joint_id(joint))) : Vec3{}; }
Vec3 World::joint_torque(Joint joint) const { return joint_valid(joint) ? kv(b3Joint_GetConstraintTorque(joint_id(joint))) : Vec3{}; }

void World::contact_points(Body body, std::vector<ContactPoint>& out) const {
  if (!body_valid(body)) return;
  const b3BodyId id = body_id(body);
  const int capacity = b3Body_GetContactCapacity(id);
  if (capacity <= 0) return;
  std::vector<b3ContactData> data(static_cast<std::size_t>(capacity));
  const int n = b3Body_GetContactData(id, data.data(), capacity);
  for (int i = 0; i < n; ++i) {
    const b3ContactData& c = data[static_cast<std::size_t>(i)];
    if (!b3Shape_IsValid(c.shapeIdA)) continue;
    const b3Pos center = b3Body_GetWorldCenter(b3Shape_GetBody(c.shapeIdA));
    for (int m = 0; m < c.manifoldCount; ++m) {
      const b3Manifold& man = c.manifolds[m];
      for (int p = 0; p < man.pointCount; ++p) {
        const b3ManifoldPoint& mp = man.points[p];
        out.push_back({{static_cast<f32>(center.x) + mp.anchorA.x, static_cast<f32>(center.y) + mp.anchorA.y, static_cast<f32>(center.z) + mp.anchorA.z},
                       kv(man.normal), mp.separation, mp.normalImpulse});
      }
    }
  }
}

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

RayHit World::cast_sphere(Vec3 origin, f32 radius, Vec3 translation, u64 mask_bits, Body ignore) const {
  RayHit hit;
  if (!valid() || !finite_vec(origin) || !finite_vec(translation) || !(radius > 0)) return hit;
  b3QueryFilter filter = b3DefaultQueryFilter();
  filter.maskBits = mask_bits;
  const b3Vec3 point{0, 0, 0};
  b3ShapeProxy proxy{&point, 1, radius};
  struct Closest {
    u64 ignore = 0;
    b3ShapeId shape = b3_nullShapeId;
    b3Pos point{};
    b3Vec3 normal{};
    f32 fraction = 1;
  } closest;
  closest.ignore = ignore.id;
  auto callback = [](b3ShapeId shape, b3Pos p, b3Vec3 n, float fraction, uint64_t, int, int, void* context) -> float {
    Closest* c = static_cast<Closest*>(context);
    if (b3Shape_IsSensor(shape) || (c->ignore && b3StoreBodyId(b3Shape_GetBody(shape)) == c->ignore)) return -1.0f;
    c->shape = shape;
    c->point = p;
    c->normal = n;
    c->fraction = fraction;
    return fraction;
  };
  b3World_CastShape(world_id(world_), bv(origin), &proxy, bv(translation), filter, callback, &closest);
  if (B3_IS_NULL(closest.shape)) return hit;
  hit.hit = true;
  hit.shape = shape_of(closest.shape);
  hit.body = shape_body_checked(closest.shape);
  hit.point = kv(closest.point);
  hit.normal = kv(closest.normal);
  hit.fraction = closest.fraction;
  return hit;
}

namespace {
struct MoverFilter {
  u64 ignore = 0;
  std::vector<MoverPlane>* out = nullptr;
  Vec3 origin;
};
bool mover_accepts(b3ShapeId shape, void* context) {
  const MoverFilter* f = static_cast<const MoverFilter*>(context);
  if (b3Shape_IsSensor(shape)) return false;
  return !(f->ignore && b3StoreBodyId(b3Shape_GetBody(shape)) == f->ignore);
}
b3Capsule capsule_of(const Capsule& c) { return {bv(c.center1), bv(c.center2), c.radius}; }
}  // namespace

f32 World::cast_mover(Vec3 origin, const Capsule& capsule, Vec3 translation, u64 mask_bits, Body ignore) const {
  if (!valid() || !finite_vec(origin) || !finite_vec(translation)) return 0;
  b3QueryFilter filter = b3DefaultQueryFilter();
  filter.maskBits = mask_bits;
  MoverFilter f;
  f.ignore = ignore.id;
  const b3Capsule c = capsule_of(capsule);
  return b3World_CastMover(world_id(world_), bv(origin), &c, bv(translation), filter, &mover_accepts, &f);
}

void World::collide_mover(Vec3 origin, const Capsule& capsule, u64 mask_bits, Body ignore, std::vector<MoverPlane>& out) const {
  out.clear();
  if (!valid() || !finite_vec(origin)) return;
  b3QueryFilter filter = b3DefaultQueryFilter();
  filter.maskBits = mask_bits;
  MoverFilter f;
  f.ignore = ignore.id;
  f.out = &out;
  f.origin = origin;
  const b3Capsule c = capsule_of(capsule);
  auto callback = [](b3ShapeId shape, const b3PlaneResult* planes, int count, void* context) -> bool {
    MoverFilter* f = static_cast<MoverFilter*>(context);
    if (!mover_accepts(shape, context)) return true;
    for (int i = 0; i < count && f->out->size() < 32; ++i) {
      MoverPlane p;
      p.normal = kv(planes[i].plane.normal);
      p.offset = planes[i].plane.offset;
      p.point = f->origin + kv(planes[i].point);
      p.shape = shape_of(shape);
      p.body = body_of(b3Shape_GetBody(shape));
      f->out->push_back(p);
    }
    return true;
  };
  b3World_CollideMover(world_id(world_), bv(origin), &c, filter, callback, &f);
}

void World::explode(Vec3 center, f32 radius, f32 falloff, f32 impulse_per_area, u64 mask_bits) {
  if (!valid() || !finite_vec(center) || !(radius > 0)) return;
  b3ExplosionDef def = b3DefaultExplosionDef();
  def.maskBits = mask_bits;
  def.position = bv(center);
  def.radius = radius;
  def.falloff = max_of(0.0f, falloff);
  def.impulsePerArea = impulse_per_area;
  b3World_Explode(world_id(world_), &def);
}

void World::push_body(Body body, Vec3 point, Vec3 normal, Vec3 velocity) {
  if (!body_valid(body) || body_type(body) != BodyType::dynamic) return;
  const b3BodyId id = body_id(body);
  const f32 inv_mass = b3Body_GetInverseMass(id);
  const b3Matrix3 inv_i = b3Body_GetWorldInverseRotationalInertia(id);
  const Vec3 r = point - kv(b3Body_GetWorldCenter(id));
  const Vec3 rn = cross(r, normal);
  const b3Vec3 irn = b3MulMV(inv_i, bv(rn));
  const f32 k = inv_mass + dot(rn, kv(irn));
  if (!(k > 0)) return;
  const Vec3 vb = kv(b3Body_GetLinearVelocity(id)) + cross(kv(b3Body_GetAngularVelocity(id)), r);
  const f32 vn = dot(vb - velocity, normal);
  const f32 impulse = max_of(-vn / k, 0.0f);
  if (impulse > 0) b3Body_ApplyLinearImpulse(id, bv(normal * impulse), bv(point), true);
}

// ---------------------------------------------------------------------------
// character mover

Capsule CharacterMover::capsule() const {
  Capsule c;
  c.radius = settings.radius;
  c.center1 = {0, 0, settings.step_height + settings.radius};
  c.center2 = {0, 0, max_of(settings.height - settings.radius, c.center1.z + 0.01f)};
  return c;
}

bool CharacterMover::step(World& world, f32 dt, Vec3 wish, bool jump, bool sprint, Body self) {
  if (!(dt > 0) || !world.valid()) return false;
  const Settings& s = settings;
  bool jumped = false;
  // Friction on the ground (linear above stop_speed, constant below).
  Vec3 horizontal{velocity_.x, velocity_.y, 0};
  const f32 speed = length(horizontal);
  if (on_ground_) {
    if (speed < 0.05f) { velocity_.x = velocity_.y = 0; }
    else {
      const f32 control = speed < s.stop_speed ? s.stop_speed : speed;
      const f32 ratio = max_of(0.0f, speed - control * s.friction * dt) / speed;
      velocity_.x *= ratio;
      velocity_.y *= ratio;
    }
  }
  const f32 max_speed = sprint ? s.max_speed * max_of(1.0f, s.sprint) : s.max_speed;
  wish.z = 0;
  f32 wish_len = length(wish);
  if (wish_len > 1) { wish = wish / wish_len; wish_len = 1; }
  const Vec3 dir = wish_len > 1e-4f ? wish / wish_len : Vec3{};
  const f32 desired = max_speed * wish_len;
  if (on_ground_) velocity_.z = 0;
  // Accelerate along the wish direction up to the desired speed.
  const f32 current = dot(velocity_, dir);
  const f32 add = desired - current;
  if (add > 0 && wish_len > 0) {
    const f32 accel = min_of(add, s.accelerate * max_speed * dt * (on_ground_ ? 1.0f : s.air_control));
    velocity_ += dir * accel;
  }
  if (jump && on_ground_) {
    velocity_.z = s.jump_speed;
    on_ground_ = false;
    jumped = true;
  }
  velocity_.z -= s.gravity * dt;

  // Pogo: a damped spring between the lower sphere and the ground carries the capsule over steps.
  const Capsule cap = capsule();
  const f32 rest = s.step_height + s.radius;
  const f32 ray = rest + s.radius;
  const Vec3 ray_origin = position_ + cap.center1;
  const RayHit ground = world.cast_sphere(ray_origin, s.radius * 0.6f, Vec3{0, 0, -ray}, s.mask_bits, self);
  const f32 min_normal_z = std::cos(clamp(s.max_slope_deg, 0.0f, 89.0f) * 0.0174532925f);
  if (!ground.hit || velocity_.z > 0 || ground.normal.z < min_normal_z) {
    on_ground_ = false;
    pogo_ = 0;
  } else {
    on_ground_ = true;
    const f32 current_length = ground.fraction * ray + s.radius * 0.6f;
    const f32 zeta = 0.7f, omega = 2 * 3.14159265f * 4.0f, oh = omega * dt;
    pogo_ = (pogo_ - omega * oh * (current_length - rest)) / (1 + 2 * zeta * oh + oh * oh);
  }

  const Vec3 start = position_;
  const Vec3 target = position_ + velocity_ * dt + Vec3{0, 0, pogo_ * dt};
  for (int iteration = 0; iteration < 5; ++iteration) {
    world.collide_mover(position_, cap, s.mask_bits, self, planes_);
    std::vector<b3CollisionPlane> cp(planes_.size());
    for (std::size_t i = 0; i < planes_.size(); ++i) cp[i] = {{bv(planes_[i].normal), planes_[i].offset}, FLT_MAX, 0, true};
    const b3PlaneSolverResult r = b3SolvePlanes(bv(target - position_), cp.data(), static_cast<int>(cp.size()));
    Vec3 delta = kv(r.delta);
    const f32 fraction = world.cast_mover(position_, cap, delta, s.mask_bits, self);
    delta = delta * fraction;
    position_ += delta;
    if (dot(delta, delta) < 1e-4f) {
      // Clip the velocity against the final planes so pressing into a wall builds no speed.
      velocity_ = kv(b3ClipVector(bv(velocity_), cp.data(), static_cast<int>(cp.size())));
      break;
    }
    if (iteration == 4) velocity_ = kv(b3ClipVector(bv(velocity_), cp.data(), static_cast<int>(cp.size())));
  }
  (void)start;
  // Push dynamic bodies the mover touches.
  if (s.push_strength > 0)
    for (const MoverPlane& p : planes_)
      if (world.body_valid(p.body) && world.body_type(p.body) == BodyType::dynamic)
        world.push_body(p.body, p.point, p.normal * -1.0f, velocity_ * s.push_strength);
  return jumped;
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
