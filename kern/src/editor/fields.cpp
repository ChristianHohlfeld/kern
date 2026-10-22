// The properties panel: field descriptions per mode, reading values from the
// selected items and writing edits back (one undoable step per change).
#include "internal.h"

namespace kern::editor {

namespace {

struct FieldDef {
  std::string group;  // non-empty: a group header row
  std::string label, path, type = "number";
  json::Value choices;  // [[value, label], ...]
  std::optional<f64> min, max, step;
};

FieldDef group(std::string g) { FieldDef f; f.group = std::move(g); return f; }
FieldDef field(std::string label, std::string path, std::string type = "number") {
  FieldDef f;
  f.label = std::move(label);
  f.path = std::move(path);
  f.type = std::move(type);
  return f;
}
FieldDef num(std::string label, std::string path, std::optional<f64> mn = {}, std::optional<f64> mx = {}, std::optional<f64> step = {}) {
  FieldDef f = field(std::move(label), std::move(path));
  f.min = mn;
  f.max = mx;
  f.step = step;
  return f;
}
FieldDef select(std::string label, std::string path, json::Value choices) {
  FieldDef f = field(std::move(label), std::move(path), "select");
  f.choices = std::move(choices);
  return f;
}
json::Value choice(json::Value v, std::string label) {
  json::Value a = json::Value::array();
  a.push(std::move(v));
  a.push(std::move(label));
  return a;
}
void vector3(std::vector<FieldDef>& out, const std::string& label, const std::string& path) {
  out.push_back(field(label + " X", path + ".0"));
  out.push_back(field(label + " Y", path + ".1"));
  out.push_back(field(label + " Z", path + ".2"));
}
void rotation3(std::vector<FieldDef>& out) {
  out.push_back(num("Rotation X (°)", "euler.0", {}, {}, 1));
  out.push_back(num("Rotation Y (°)", "euler.1", {}, {}, 1));
  out.push_back(num("Rotation Z (°)", "euler.2", {}, {}, 1));
}

json::Value vec_index(const D3& v, std::string_view path, std::string_view key) {
  if (path.size() == key.size() + 2 && path.substr(0, key.size()) == key && path[key.size()] == '.') {
    const int i = path.back() - '0';
    if (i >= 0 && i < 3) return v[static_cast<std::size_t>(i)];
  }
  return {};
}
bool set_vec_index(D3& v, std::string_view path, std::string_view key, f64 value) {
  if (path.size() == key.size() + 2 && path.substr(0, key.size()) == key && path[key.size()] == '.') {
    const int i = path.back() - '0';
    if (i >= 0 && i < 3) { v[static_cast<std::size_t>(i)] = value; return true; }
  }
  return false;
}
D3 euler_degrees(const DQ& q) {
  const D3 e = euler_xyz(qnormalize(q));
  return {rad2deg(e[0]), rad2deg(e[1]), rad2deg(e[2])};
}
bool set_euler(DQ& q, std::string_view path, f64 degrees) {
  D3 e = euler_degrees(q);
  if (!set_vec_index(e, path, "euler", degrees)) return false;
  q = quat_from_euler_xyz({deg2rad(e[0]), deg2rad(e[1]), deg2rad(e[2])});
  return true;
}

json::Value body_choices(const world::Scene& s, bool none) {
  json::Value c = json::Value::array();
  if (none) c.push(choice("", "(none)"));
  for (const Body& b : s.bodies) c.push(choice(b.id, b.name));
  return c;
}

}  // namespace

// ---------------------------------------------------------------------------
// field definitions

namespace {

std::vector<FieldDef> fields_for(Mode m, const world::Scene& s, const std::string& first_shape, JointKind first_joint, bool have_joint) {
  std::vector<FieldDef> f;
  if (m != Mode::world && m != Mode::vertex) f.push_back(field("Name", "name", "text"));
  if (m == Mode::world) {
    f.push_back(group("World"));
    vector3(f, "Gravity", "gravity");
    f.push_back(num("Steps per second", "stepsPerSecond", 1, 1000, 1));
    f.push_back(num("Sub steps", "subStepCount", 1, 32, 1));
    f.push_back(field("Allow sleep", "allowSleep", "checkbox"));
    f.push_back(field("Continuous physics", "continuousPhysics", "checkbox"));
    f.push_back(num("Restitution threshold (m/s)", "restitutionThreshold", 0, {}, 0.01));
    f.push_back(num("Restitution iterations", "restitutionIterations", 0, 63, 1));
    f.push_back(field("Restitution propagation", "restitutionPropagation", "checkbox"));
    f.push_back(group("Game"));
    f.push_back(field("Title", "game.title", "text"));
    json::Value goals = json::Value::array();
    goals.push(choice("none", "None (sandbox)"));
    goals.push(choice("reachGoal", "Reach a goal"));
    goals.push(choice("collectAll", "Collect all, then a goal"));
    goals.push(choice("survive", "Survive the time limit"));
    f.push_back(select("Win condition", "game.goal", std::move(goals)));
    f.push_back(num("Time limit (s, 0 = none)", "game.timeLimit", 0, {}, 1));
    f.push_back(num("Camera distance", "game.cameraDistance", 0.5, {}, 0.5));
    f.push_back(num("Camera height", "game.cameraHeight", 0, {}, 0.5));
    f.push_back(num("Fall limit (z)", "game.fallLimit", {}, {}, 1));
  }
  if (m == Mode::body) {
    json::Value types = json::Value::array();
    types.push(choice(0, "Static"));
    types.push(choice(1, "Kinematic"));
    types.push(choice(2, "Dynamic"));
    f.push_back(select("Type", "type", std::move(types)));
    f.push_back(field("Bullet", "bullet", "checkbox"));
    f.push_back(field("Sleeping allowed", "allowSleep", "checkbox"));
    f.push_back(field("Awake", "awake", "checkbox"));
    f.push_back(field("Active", "active", "checkbox"));
    f.push_back(num("Linear damping", "linearDamping", 0));
    f.push_back(num("Angular damping", "angularDamping", 0));
    f.push_back(field("Gravity scale", "gravityScale"));
    f.push_back(group("Initial state"));
    vector3(f, "Position", "position");
    rotation3(f);
    vector3(f, "Linear velocity", "linearVelocity");
    vector3(f, "Angular velocity", "angularVelocity");
    f.push_back(group("Game"));
    json::Value roles = json::Value::array();
    roles.push(choice("none", "None"));
    roles.push(choice("player", "Player"));
    roles.push(choice("goal", "Goal"));
    roles.push(choice("pickup", "Pickup"));
    roles.push(choice("hazard", "Hazard"));
    f.push_back(select("Role", "game.role", std::move(roles)));
    json::Value controls = json::Value::array();
    controls.push(choice("none", "None"));
    controls.push(choice("roll", "Roll (ball)"));
    controls.push(choice("drive", "Drive (car)"));
    controls.push(choice("fly", "Fly (hover)"));
    controls.push(choice("walk", "Walk (upright)"));
    f.push_back(select("Control", "game.control", std::move(controls)));
    f.push_back(num("Control force", "game.force", 0, {}, 0.5));
    f.push_back(num("Jump speed (m/s)", "game.jump", 0, {}, 0.5));
    f.push_back(field("Camera follows", "game.cameraFollow", "checkbox"));
    f.push_back(num("Pickup points", "game.points", {}, {}, 1));
  }
  if (m == Mode::fixture) {
    json::Value shapes = json::Value::array();
    shapes.push(choice("box", "Box"));
    shapes.push(choice("sphere", "Sphere"));
    shapes.push(choice("capsule", "Capsule"));
    shapes.push(choice("hull", "Hull"));
    f.push_back(select("Shape", "shape", std::move(shapes)));
    vector3(f, "Center", "center");
    rotation3(f);
    f.push_back(num("Density", "density", 0));
    f.push_back(num("Friction", "friction", 0));
    f.push_back(num("Restitution", "restitution", 0, 1));
    f.push_back(num("Rolling resistance", "rollingResistance", 0, 1, 0.001));
    f.push_back(field("Sensor", "sensor", "checkbox"));
    f.push_back(num("Category bits", "categoryBits", 0, {}, 1));
    f.push_back(num("Mask bits", "maskBits", 0, {}, 1));
    f.push_back(num("Group index", "groupIndex", {}, {}, 1));
    f.push_back(group("Shape"));
    if (first_shape == "box") vector3(f, "Size", "size");
    if (first_shape == "sphere" || first_shape == "capsule") f.push_back(num("Radius", "radius", 0.001));
    if (first_shape == "capsule") f.push_back(num("Length", "length", 0.001));
    f.push_back(field("Color", "color", "color"));
  }
  if (m == Mode::vertex) vector3(f, "Local position", "position");
  if (m == Mode::joint) {
    json::Value types = json::Value::array();
    for (u32 k = 0; k < kJointKindCount; ++k) types.push(choice(joint_kind_name(static_cast<JointKind>(k)), capitalize(joint_kind_name(static_cast<JointKind>(k)))));
    f.push_back(select("Type", "type", std::move(types)));
    f.push_back(select("Body A", "bodyA", body_choices(s, false)));
    f.push_back(select("Body B", "bodyB", body_choices(s, false)));
    f.push_back(field("Collide connected", "collideConnected", "checkbox"));
    f.push_back(group("Anchors"));
    vector3(f, "Anchor A", "anchorA");
    vector3(f, "Anchor B", "anchorB");
    const JointKind t = first_joint;
    auto any_of = [&](std::initializer_list<JointKind> ks) {
      if (!have_joint) return false;
      for (JointKind k : ks) if (k == t) return true;
      return false;
    };
    if (any_of({JointKind::revolute, JointKind::prismatic, JointKind::wheel, JointKind::spherical, JointKind::parallel})) {
      f.push_back(group("Axis"));
      vector3(f, "Axis A", "axisA");
    }
    if (any_of({JointKind::revolute, JointKind::prismatic, JointKind::wheel, JointKind::spherical, JointKind::distance, JointKind::rope})) {
      f.push_back(field("Enable limit", "enableLimit", "checkbox"));
      f.push_back(field("Lower limit", "lowerLimit"));
      f.push_back(field("Upper limit", "upperLimit"));
    }
    if (any_of({JointKind::revolute, JointKind::prismatic, JointKind::wheel, JointKind::spherical, JointKind::distance})) {
      f.push_back(field("Enable motor", "enableMotor", "checkbox"));
      f.push_back(field("Motor speed", "motorSpeed"));
      f.push_back(num("Max motor torque", "maxMotorTorque", 0));
      f.push_back(num("Max motor force", "maxMotorForce", 0));
    }
    if (any_of({JointKind::distance, JointKind::rope})) {
      f.push_back(num("Length", "length", 0.001));
      if (t == JointKind::rope) f.push_back(num("Max length", "maxLength", 0.001));
    }
    if (any_of({JointKind::distance, JointKind::spherical})) f.push_back(field("Enable spring", "enableSpring", "checkbox"));
    if (any_of({JointKind::wheel})) {
      f.push_back(num("Spring frequency", "springFrequency", 0));
      f.push_back(num("Spring damping", "springDampingRatio", 0));
    }
    if (any_of({JointKind::motor, JointKind::friction, JointKind::parallel, JointKind::weld, JointKind::spherical, JointKind::distance})) {
      f.push_back(num("Frequency", "frequency", 0));
      f.push_back(num("Damping", "dampingRatio", 0));
    }
    if (any_of({JointKind::friction})) {
      f.push_back(num("Max force", "maxForce", 0));
      f.push_back(num("Max torque", "maxTorque", 0));
    }
  }
  if (m == Mode::image) {
    f.push_back(select("Body", "body", body_choices(s, true)));
    vector3(f, "Center", "center");
    rotation3(f);
    f.push_back(num("Width", "size.0", 0.001));
    f.push_back(num("Height", "size.1", 0.001));
    f.push_back(num("Opacity", "opacity", 0, 1, 0.05));
    f.push_back(field("Flip", "flip", "checkbox"));
    json::Value filters = json::Value::array();
    filters.push(choice(0, "Linear"));
    filters.push(choice(1, "Nearest"));
    f.push_back(select("Filter", "filter", std::move(filters)));
    f.push_back(field("Tint", "tint", "color"));
    f.push_back(field("File", "file", "text"));
  }
  if (m == Mode::sampler) {
    f.push_back(select("Body", "body", body_choices(s, true)));
    vector3(f, "Center", "center");
    rotation3(f);
    vector3(f, "Size", "size");
    f.push_back(num("Resolution", "resolution", 4, 500, 1));
    f.push_back(num("Threshold", "threshold", 0, 255, 1));
    f.push_back(num("Tolerance", "tolerance", 0.001, 1));
    f.push_back(num("Opacity", "opacity", 0, 1, 0.05));
    f.push_back(field("Flip", "flip", "checkbox"));
    f.push_back(field("Tint", "tint", "color"));
    f.push_back(field("File", "file", "text"));
  }
  return f;
}

// ---- reading ----

json::Value get_world(const world::Scene& s, const std::string& p) {
  const WorldSettings& w = s.world;
  if (auto v = vec_index(w.gravity, p, "gravity"); !v.is_null()) return v;
  if (p == "stepsPerSecond") return w.steps_per_second;
  if (p == "subStepCount") return w.sub_step_count;
  if (p == "allowSleep") return w.allow_sleep;
  if (p == "continuousPhysics") return w.continuous_physics;
  if (p == "restitutionThreshold") return w.restitution_threshold;
  if (p == "restitutionIterations") return w.restitution_iterations;
  if (p == "restitutionPropagation") return w.restitution_propagation;
  const SceneGame& g = s.game;
  if (p == "game.title") return g.title;
  if (p == "game.goal") return game_goal_name(g.goal);
  if (p == "game.timeLimit") return g.time_limit;
  if (p == "game.cameraDistance") return g.camera_distance;
  if (p == "game.cameraHeight") return g.camera_height;
  if (p == "game.fallLimit") return g.fall_limit;
  return {};
}

json::Value get_body(const Body& b, const std::string& p) {
  if (p == "name") return b.name;
  if (p == "type") return static_cast<i32>(b.type);
  if (p == "bullet") return b.bullet;
  if (p == "allowSleep") return b.allow_sleep;
  if (p == "awake") return b.awake;
  if (p == "active") return b.active;
  if (p == "linearDamping") return b.linear_damping;
  if (p == "angularDamping") return b.angular_damping;
  if (p == "gravityScale") return b.gravity_scale;
  if (auto v = vec_index(b.position, p, "position"); !v.is_null()) return v;
  if (auto v = vec_index(euler_degrees(b.rotation), p, "euler"); !v.is_null()) return std::round(v.as_number() * 1e4) / 1e4;
  if (auto v = vec_index(b.linear_velocity, p, "linearVelocity"); !v.is_null()) return v;
  if (auto v = vec_index(b.angular_velocity, p, "angularVelocity"); !v.is_null()) return v;
  if (p == "game.role") return game_role_name(b.game.role);
  if (p == "game.control") return game_control_name(b.game.control);
  if (p == "game.force") return b.game.force;
  if (p == "game.jump") return b.game.jump;
  if (p == "game.cameraFollow") return b.game.camera_follow;
  if (p == "game.points") return b.game.points;
  return {};
}

json::Value get_fixture(const Fixture& f, const std::string& p) {
  if (p == "name") return f.name;
  if (p == "shape") return shape_name(f.shape);
  if (auto v = vec_index(f.center, p, "center"); !v.is_null()) return v;
  if (auto v = vec_index(euler_degrees(f.rotation), p, "euler"); !v.is_null()) return std::round(v.as_number() * 1e4) / 1e4;
  if (p == "density") return f.density;
  if (p == "friction") return f.friction;
  if (p == "restitution") return f.restitution;
  if (p == "rollingResistance") return f.rolling_resistance;
  if (p == "sensor") return f.sensor;
  if (p == "categoryBits") return f.category_bits;
  if (p == "maskBits") return f.mask_bits;
  if (p == "groupIndex") return f.group_index;
  if (auto v = vec_index(f.size, p, "size"); !v.is_null()) return v;
  if (p == "radius") return f.radius;
  if (p == "length") return f.length;
  if (p == "color") return f.color;
  return {};
}

json::Value get_joint(const Joint& j, const std::string& p) {
  if (p == "name") return j.name;
  if (p == "type") return joint_kind_name(j.type);
  if (p == "bodyA") return j.body_a;
  if (p == "bodyB") return j.body_b;
  if (p == "collideConnected") return j.collide_connected;
  if (auto v = vec_index(j.anchor_a, p, "anchorA"); !v.is_null()) return v;
  if (auto v = vec_index(j.anchor_b, p, "anchorB"); !v.is_null()) return v;
  if (auto v = vec_index(j.axis_a, p, "axisA"); !v.is_null()) return v;
  if (p == "enableLimit") return j.enable_limit;
  if (p == "lowerLimit") return j.lower_limit;
  if (p == "upperLimit") return j.upper_limit;
  if (p == "enableMotor") return j.enable_motor;
  if (p == "motorSpeed") return j.motor_speed;
  if (p == "maxMotorTorque") return j.max_motor_torque;
  if (p == "maxMotorForce") return j.max_motor_force;
  if (p == "length") return j.length;
  if (p == "maxLength") return j.max_length;
  if (p == "enableSpring") return j.enable_spring;
  if (p == "springFrequency") return j.spring_frequency;
  if (p == "springDampingRatio") return j.spring_damping_ratio ? json::Value(*j.spring_damping_ratio) : json::Value();
  if (p == "frequency") return j.frequency;
  if (p == "dampingRatio") return j.damping_ratio ? json::Value(*j.damping_ratio) : json::Value();
  if (p == "maxForce") return j.max_force;
  if (p == "maxTorque") return j.max_torque;
  return {};
}

template <typename Item>
json::Value get_plane_common(const Item& i, const std::string& p) {
  if (p == "name") return i.name;
  if (p == "body") return i.body;
  if (auto v = vec_index(i.center, p, "center"); !v.is_null()) return v;
  if (auto v = vec_index(euler_degrees(i.rotation), p, "euler"); !v.is_null()) return std::round(v.as_number() * 1e4) / 1e4;
  if (p == "opacity") return i.opacity ? json::Value(*i.opacity) : json::Value();
  if (p == "flip") return i.flip;
  if (p == "tint") return i.tint;
  if (p == "file") return i.file;
  return {};
}

}  // namespace

json::Value Editor::fields_json() {
  json::Value out = json::Value::object();
  Document* d = doc();
  if (!d) return out;
  const world::Scene& s = d->scene;
  std::vector<ItemRef> items = mode_ == Mode::world ? std::vector<ItemRef>{} : chosen(mode_);
  if (mode_ != Mode::world && items.empty()) {
    out.set("empty", std::string("No ") + mode_name(mode_) + " selected.<br>Select an item in the view or Items panel.");
    return out;
  }
  std::string first_shape;
  JointKind first_joint = JointKind::revolute;
  bool have_joint = false;
  if (mode_ == Mode::fixture)
    if (const Fixture* f = d->scene.fixture(items[0].id)) first_shape = shape_name(f->shape);
  if (mode_ == Mode::joint)
    for (const Joint& j : s.joints)
      if (j.id == items[0].id) { first_joint = j.type; have_joint = true; }
  const std::vector<FieldDef> defs = fields_for(mode_, s, first_shape, first_joint, have_joint);
  auto value_of = [&](const ItemRef& item, const std::string& path) -> json::Value {
    switch (mode_) {
      case Mode::body: if (const Body* b = s.body(item.id)) return get_body(*b, path); break;
      case Mode::fixture:
        for (const Body& b : s.bodies) for (const Fixture& f : b.fixtures) if (f.id == item.id) return get_fixture(f, path);
        break;
      case Mode::vertex: if (auto v = vec_index(item.position, path, "position"); !v.is_null()) return v; break;
      case Mode::joint: for (const Joint& j : s.joints) if (j.id == item.id) return get_joint(j, path); break;
      case Mode::image:
        for (const ImagePlane& i : s.images)
          if (i.id == item.id) {
            if (path == "size.0") return i.size[0];
            if (path == "size.1") return i.size[1];
            if (path == "filter") return i.filter;
            return get_plane_common(i, path);
          }
        break;
      case Mode::sampler:
        for (const Sampler& i : s.samplers)
          if (i.id == item.id) {
            if (auto v = vec_index(i.size, path, "size"); !v.is_null()) return v;
            if (path == "resolution") return i.resolution;
            if (path == "threshold") return i.threshold;
            if (path == "tolerance") return i.tolerance;
            return get_plane_common(i, path);
          }
        break;
      case Mode::world: break;
    }
    return {};
  };
  const std::size_t count = mode_ == Mode::world ? 1 : items.size();
  out.set("title", mode_ == Mode::world ? std::string("World parameters")
                                        : std::to_string(count) + " " + mode_name(mode_) + (count == 1 ? "" : "s") + " selected");
  json::Value fields = json::Value::array();
  for (const FieldDef& fd : defs) {
    json::Value o = json::Value::object();
    if (!fd.group.empty()) {
      o.set("group", fd.group);
      fields.push(std::move(o));
      continue;
    }
    o.set("label", fd.label);
    o.set("path", fd.path);
    o.set("type", fd.type);
    json::Value first;
    bool mixed = false;
    if (mode_ == Mode::world) {
      first = get_world(s, fd.path);
    } else {
      first = value_of(items[0], fd.path);
      for (std::size_t k = 1; k < items.size() && !mixed; ++k) mixed = value_of(items[k], fd.path) != first;
    }
    o.set("value", first);
    o.set("mixed", mixed);
    if (fd.min) o.set("min", *fd.min);
    if (fd.max) o.set("max", *fd.max);
    if (fd.step) o.set("step", *fd.step);
    if (!fd.choices.is_null()) o.set("choices", fd.choices);
    fields.push(std::move(o));
  }
  out.set("fields", std::move(fields));
  json::Value actions = json::Value::array();
  auto action = [&](const char* label, json::Value cmd) {
    json::Value a = json::Value::object();
    a.set("label", label);
    a.set("cmd", std::move(cmd));
    actions.push(std::move(a));
  };
  auto cmd = [](const char* name, const char* key = nullptr, const char* value = nullptr) {
    json::Value c = json::Value::object();
    c.set("cmd", name);
    if (key) c.set(key, value);
    return c;
  };
  if (mode_ != Mode::vertex) action("Custom properties…", cmd("dialog", "name", "custom"));
  if (mode_ == Mode::fixture) action("Collision settings…", cmd("dialog", "name", "collision"));
  if (mode_ == Mode::image) action("Create fixtures from alpha…", cmd("traceImage"));
  if (mode_ == Mode::sampler) {
    action("Save sampled PNG", cmd("samplerAction", "action", "png"));
    action("Sobel edges", cmd("samplerAction", "action", "sobel"));
    action("Local maxima", cmd("samplerAction", "action", "maxima"));
    action("Create fixtures", cmd("samplerAction", "action", "trace"));
  }
  out.set("actions", std::move(actions));
  return out;
}

// ---------------------------------------------------------------------------
// writing

bool Editor::set_property(const std::string& path, const json::Value& value_in) {
  Document* d = doc();
  if (!d) return false;
  json::Value value = value_in;
  // Numbers arrive as numbers; strings from number inputs are converted like Number().
  auto number = [&](f64& out) {
    if (value.is_number()) { out = value.as_number(); return std::isfinite(out); }
    if (value.is_string()) { out = js_number(value.as_string()); return std::isfinite(out); }
    if (value.is_bool()) { out = value.as_bool() ? 1 : 0; return true; }
    return false;
  };
  auto flag = [&]() { return value.truthy(); };
  auto text = [&]() { return value.is_string() ? value.as_string() : json::stringify(value); };
  std::string label = "Set " + path;
  for (const FieldDef& fd : fields_for(mode_, d->scene, "box", JointKind::revolute, true))
    if (fd.path == path) label = "Set " + fd.label;
  bool bad_number = false;
  const bool ok = edit(label, [&]() -> bool {
    world::Scene& s = d->scene;
    f64 n = 0;
    const bool is_num = number(n);
    auto need_num = [&]() { if (!is_num) bad_number = true; return is_num; };
    if (mode_ == Mode::world) {
      WorldSettings& w = s.world;
      SceneGame& g = s.game;
      if (path.rfind("gravity.", 0) == 0) return need_num() && set_vec_index(w.gravity, path, "gravity", n);
      if (path == "stepsPerSecond") return need_num() && (w.steps_per_second = n, true);
      if (path == "subStepCount") return need_num() && (w.sub_step_count = n, true);
      if (path == "allowSleep") return w.allow_sleep = flag(), true;
      if (path == "continuousPhysics") return w.continuous_physics = flag(), true;
      if (path == "restitutionThreshold") return need_num() && (w.restitution_threshold = n, true);
      if (path == "restitutionIterations") return need_num() && (w.restitution_iterations = n, true);
      if (path == "restitutionPropagation") return w.restitution_propagation = flag(), true;
      if (path == "game.title") return g.title = text(), true;
      if (path == "game.goal") return parse_game_goal(text(), g.goal);
      if (path == "game.timeLimit") return need_num() && (g.time_limit = std::max(0.0, n), true);
      if (path == "game.cameraDistance") return need_num() && (g.camera_distance = std::max(0.5, n), true);
      if (path == "game.cameraHeight") return need_num() && (g.camera_height = n, true);
      if (path == "game.fallLimit") return need_num() && (g.fall_limit = n, true);
      return false;
    }
    for (const ItemRef& item : chosen(mode_)) {
      switch (mode_) {
        case Mode::body: {
          Body* b = s.body(item.id);
          if (!b) continue;
          if (path == "name") b->name = text();
          else if (path == "type") { if (!need_num()) return false; b->type = static_cast<BodyKind>(static_cast<i32>(n)); }
          else if (path == "bullet") b->bullet = flag();
          else if (path == "allowSleep") b->allow_sleep = flag();
          else if (path == "awake") b->awake = flag();
          else if (path == "active") b->active = flag();
          else if (path == "linearDamping") { if (!need_num()) return false; b->linear_damping = n; }
          else if (path == "angularDamping") { if (!need_num()) return false; b->angular_damping = n; }
          else if (path == "gravityScale") { if (!need_num()) return false; b->gravity_scale = n; }
          else if (path.rfind("position.", 0) == 0) { if (!need_num() || !set_vec_index(b->position, path, "position", n)) return false; }
          else if (path.rfind("euler.", 0) == 0) { if (!need_num() || !set_euler(b->rotation, path, n)) return false; }
          else if (path.rfind("linearVelocity.", 0) == 0) { if (!need_num() || !set_vec_index(b->linear_velocity, path, "linearVelocity", n)) return false; }
          else if (path.rfind("angularVelocity.", 0) == 0) { if (!need_num() || !set_vec_index(b->angular_velocity, path, "angularVelocity", n)) return false; }
          else if (path == "game.role") { if (!parse_game_role(text(), b->game.role)) return false; }
          else if (path == "game.control") { if (!parse_game_control(text(), b->game.control)) return false; }
          else if (path == "game.force") { if (!need_num()) return false; b->game.force = std::max(0.0, n); }
          else if (path == "game.jump") { if (!need_num()) return false; b->game.jump = std::max(0.0, n); }
          else if (path == "game.cameraFollow") b->game.camera_follow = flag();
          else if (path == "game.points") { if (!need_num()) return false; b->game.points = n; }
          else return false;
          break;
        }
        case Mode::fixture: {
          Fixture* f = s.fixture(item.id);
          if (!f) continue;
          if (path == "name") f->name = text();
          else if (path == "shape") {
            FixtureShape sh;
            if (!parse_shape(text(), sh)) return false;
            if (sh == FixtureShape::hull && f->vertices.size() < 4) {
              const D3 h = mul(f->size, 0.5);
              f->vertices.clear();
              for (int i = 0; i < 8; ++i) f->vertices.push_back({i & 1 ? h[0] : -h[0], i & 2 ? h[1] : -h[1], i & 4 ? h[2] : -h[2]});
            }
            f->shape = sh;
          }
          else if (path.rfind("center.", 0) == 0) { if (!need_num() || !set_vec_index(f->center, path, "center", n)) return false; }
          else if (path.rfind("euler.", 0) == 0) { if (!need_num() || !set_euler(f->rotation, path, n)) return false; }
          else if (path == "density") { if (!need_num()) return false; f->density = n; }
          else if (path == "friction") { if (!need_num()) return false; f->friction = n; }
          else if (path == "restitution") { if (!need_num()) return false; f->restitution = n; }
          else if (path == "rollingResistance") { if (!need_num()) return false; f->rolling_resistance = n; }
          else if (path == "sensor") f->sensor = flag();
          else if (path == "categoryBits") { if (!need_num()) return false; f->category_bits = n; }
          else if (path == "maskBits") { if (!need_num()) return false; f->mask_bits = n; }
          else if (path == "groupIndex") { if (!need_num()) return false; f->group_index = n; }
          else if (path.rfind("size.", 0) == 0) { if (!need_num() || !set_vec_index(f->size, path, "size", n)) return false; }
          else if (path == "radius") { if (!need_num()) return false; f->radius = n; }
          else if (path == "length") { if (!need_num()) return false; f->length = n; }
          else if (path == "color") f->color = text();
          else return false;
          break;
        }
        case Mode::vertex: {
          Fixture* f = s.fixture(item.fixture);
          if (!f || item.index < 0 || static_cast<std::size_t>(item.index) >= f->vertices.size()) continue;
          if (!need_num() || !set_vec_index(f->vertices[static_cast<std::size_t>(item.index)], path, "position", n)) return false;
          break;
        }
        case Mode::joint: {
          Joint* j = s.joint(item.id);
          if (!j) continue;
          if (path == "name") j->name = text();
          else if (path == "type") { if (!parse_joint_kind(text(), j->type)) return false; }
          else if (path == "bodyA") j->body_a = text();
          else if (path == "bodyB") j->body_b = text();
          else if (path == "collideConnected") j->collide_connected = flag();
          else if (path.rfind("anchorA.", 0) == 0) { if (!need_num() || !set_vec_index(j->anchor_a, path, "anchorA", n)) return false; }
          else if (path.rfind("anchorB.", 0) == 0) { if (!need_num() || !set_vec_index(j->anchor_b, path, "anchorB", n)) return false; }
          else if (path.rfind("axisA.", 0) == 0) { if (!need_num() || !set_vec_index(j->axis_a, path, "axisA", n)) return false; }
          else if (path == "enableLimit") j->enable_limit = flag();
          else if (path == "lowerLimit") { if (!need_num()) return false; j->lower_limit = n; }
          else if (path == "upperLimit") { if (!need_num()) return false; j->upper_limit = n; }
          else if (path == "enableMotor") j->enable_motor = flag();
          else if (path == "motorSpeed") { if (!need_num()) return false; j->motor_speed = n; }
          else if (path == "maxMotorTorque") { if (!need_num()) return false; j->max_motor_torque = n; }
          else if (path == "maxMotorForce") { if (!need_num()) return false; j->max_motor_force = n; }
          else if (path == "length") { if (!need_num()) return false; j->length = n; }
          else if (path == "maxLength") { if (!need_num()) return false; j->max_length = n; }
          else if (path == "enableSpring") j->enable_spring = flag();
          else if (path == "springFrequency") { if (!need_num()) return false; j->spring_frequency = n; }
          else if (path == "springDampingRatio") { if (!need_num()) return false; j->spring_damping_ratio = n; }
          else if (path == "frequency") { if (!need_num()) return false; j->frequency = n; }
          else if (path == "dampingRatio") { if (!need_num()) return false; j->damping_ratio = n; }
          else if (path == "maxForce") { if (!need_num()) return false; j->max_force = n; }
          else if (path == "maxTorque") { if (!need_num()) return false; j->max_torque = n; }
          else return false;
          break;
        }
        case Mode::image:
        case Mode::sampler: {
          auto common = [&](auto& i) -> int {
            if (path == "name") { i.name = text(); return 1; }
            if (path == "body") { i.body = text(); return 1; }
            if (path.rfind("center.", 0) == 0) return need_num() && set_vec_index(i.center, path, "center", n) ? 1 : 0;
            if (path.rfind("euler.", 0) == 0) return need_num() && set_euler(i.rotation, path, n) ? 1 : 0;
            if (path == "opacity") { if (!need_num()) return 0; i.opacity = n; return 1; }
            if (path == "flip") { i.flip = flag(); return 1; }
            if (path == "tint") { i.tint = text(); return 1; }
            if (path == "file") { i.file = text(); return 1; }
            return -1;
          };
          if (mode_ == Mode::image) {
            ImagePlane* i = s.image(item.id);
            if (!i) continue;
            const int r = common(*i);
            if (r == 0) return false;
            if (r < 0) {
              if (path == "size.0") { if (!need_num()) return false; i->size[0] = n; }
              else if (path == "size.1") { if (!need_num()) return false; i->size[1] = n; }
              else if (path == "filter") { if (!need_num()) return false; i->filter = static_cast<i32>(n); }
              else return false;
            }
          } else {
            Sampler* i = s.sampler(item.id);
            if (!i) continue;
            const int r = common(*i);
            if (r == 0) return false;
            if (r < 0) {
              if (path.rfind("size.", 0) == 0) { if (!need_num() || !set_vec_index(i->size, path, "size", n)) return false; }
              else if (path == "resolution") { if (!need_num()) return false; i->resolution = n; }
              else if (path == "threshold") { if (!need_num()) return false; i->threshold = n; }
              else if (path == "tolerance") { if (!need_num()) return false; i->tolerance = n; }
              else return false;
            }
          }
          break;
        }
        case Mode::world: break;
      }
    }
    return true;
  });
  if (!ok && bad_number) report("Enter a finite number.", "warn");
  touch();  // the panel re-reads the (possibly unchanged) values
  return ok;
}

}  // namespace kern::editor
