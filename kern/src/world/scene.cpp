#include "kern/world/scene.h"

#include <algorithm>
#include <set>

namespace kern::world {

// ---------------------------------------------------------------------------
// names

namespace {
constexpr const char* kShapeNames[] = {"box", "sphere", "capsule", "hull"};
constexpr const char* kJointNames[] = {"revolute", "distance", "prismatic", "wheel", "rope", "weld",
                                       "friction", "motor",    "spherical", "parallel", "filter"};
constexpr const char* kRoleNames[] = {"none", "player", "goal", "pickup", "hazard"};
constexpr const char* kControlNames[] = {"none", "roll", "drive", "fly", "walk"};
constexpr const char* kGoalNames[] = {"none", "reachGoal", "collectAll", "survive"};

template <typename E, std::size_t N>
bool parse_name(std::string_view s, const char* const (&names)[N], E& out) {
  for (std::size_t i = 0; i < N; ++i)
    if (s == names[i]) { out = static_cast<E>(i); return true; }
  return false;
}
}  // namespace

const char* shape_name(FixtureShape s) { return kShapeNames[static_cast<u32>(s)]; }
bool parse_shape(std::string_view s, FixtureShape& out) { return parse_name(s, kShapeNames, out); }
const char* joint_kind_name(JointKind k) { return kJointNames[static_cast<u32>(k)]; }
bool parse_joint_kind(std::string_view s, JointKind& out) { return parse_name(s, kJointNames, out); }
const char* game_role_name(GameRole r) { return kRoleNames[static_cast<u32>(r)]; }
const char* game_control_name(GameControl c) { return kControlNames[static_cast<u32>(c)]; }
bool parse_game_role(std::string_view s, GameRole& out) { return parse_name(s, kRoleNames, out); }
bool parse_game_control(std::string_view s, GameControl& out) { return parse_name(s, kControlNames, out); }
const char* game_goal_name(GameGoal g) { return kGoalNames[static_cast<u32>(g)]; }
bool parse_game_goal(std::string_view s, GameGoal& out) { return parse_name(s, kGoalNames, out); }

bool BodyGame::is_default() const {
  const BodyGame d;
  return role == d.role && control == d.control && force == d.force && jump == d.jump && camera_follow == d.camera_follow &&
         points == d.points;
}
bool SceneGame::is_default() const {
  const SceneGame d;
  return title == d.title && goal == d.goal && time_limit == d.time_limit && camera_distance == d.camera_distance &&
         camera_height == d.camera_height && fall_limit == d.fall_limit;
}

// ---------------------------------------------------------------------------
// lookups

Body* Scene::body(std::string_view id) {
  for (Body& b : bodies) if (b.id == id) return &b;
  return nullptr;
}
const Body* Scene::body(std::string_view id) const {
  for (const Body& b : bodies) if (b.id == id) return &b;
  return nullptr;
}
Body* Scene::body_of_fixture(std::string_view id) {
  for (Body& b : bodies)
    for (const Fixture& f : b.fixtures) if (f.id == id) return &b;
  return nullptr;
}
const Body* Scene::body_of_fixture(std::string_view id) const {
  for (const Body& b : bodies)
    for (const Fixture& f : b.fixtures) if (f.id == id) return &b;
  return nullptr;
}
Fixture* Scene::fixture(std::string_view id) {
  for (Body& b : bodies)
    for (Fixture& f : b.fixtures) if (f.id == id) return &f;
  return nullptr;
}
Joint* Scene::joint(std::string_view id) {
  for (Joint& j : joints) if (j.id == id) return &j;
  return nullptr;
}
ImagePlane* Scene::image(std::string_view id) {
  for (ImagePlane& i : images) if (i.id == id) return &i;
  return nullptr;
}
Sampler* Scene::sampler(std::string_view id) {
  for (Sampler& s : samplers) if (s.id == id) return &s;
  return nullptr;
}
u32 Scene::fixture_count() const {
  u32 n = 0;
  for (const Body& b : bodies) n += static_cast<u32>(b.fixtures.size());
  return n;
}

// ---------------------------------------------------------------------------
// identifiers

namespace {
u64 g_uid_state = 0;
u64 splitmix(u64& s) {
  u64 z = (s += 0x9e3779b97f4a7c15ull);
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
  return z ^ (z >> 31);
}
}  // namespace

void seed_uids(u64 seed) { g_uid_state = seed ? seed : 1; }

std::string make_uid() {
  if (!g_uid_state) {
    const f64 t = now_ms();
    u64 bits = 0;
    std::memcpy(&bits, &t, sizeof bits);
    g_uid_state = bits ^ reinterpret_cast<std::uintptr_t>(&g_uid_state) ^ 0x2545F4914F6CDD1Dull;
  }
  u8 b[16];
  const u64 a = splitmix(g_uid_state), c = splitmix(g_uid_state);
  std::memcpy(b, &a, 8);
  std::memcpy(b + 8, &c, 8);
  b[6] = static_cast<u8>((b[6] & 0x0f) | 0x40);
  b[8] = static_cast<u8>((b[8] & 0x3f) | 0x80);
  static const char* hex = "0123456789abcdef";
  std::string out;
  out.reserve(36);
  for (int i = 0; i < 16; ++i) {
    if (i == 4 || i == 6 || i == 8 || i == 10) out += '-';
    out += hex[b[i] >> 4];
    out += hex[b[i] & 15];
  }
  return out;
}

// ---------------------------------------------------------------------------
// construction

Scene new_scene(std::string name) {
  Scene s;
  s.id = make_uid();
  s.name = std::move(name);
  return s;
}

Body new_body(BodyKind type, const D3& position, std::string name) {
  Body b;
  b.id = make_uid();
  b.name = std::move(name);
  b.type = type;
  b.position = position;
  return b;
}

Fixture new_fixture(FixtureShape shape, std::string name) {
  Fixture f;
  f.id = make_uid();
  f.name = std::move(name);
  f.shape = shape;
  if (shape == FixtureShape::hull)
    f.vertices = {{-.5, -.5, -.5}, {.5, -.5, -.5}, {.5, .5, -.5}, {-.5, .5, -.5}, {-.5, -.5, .5}, {.5, -.5, .5}, {.5, .5, .5}, {-.5, .5, .5}};
  return f;
}

// ---------------------------------------------------------------------------
// writing

namespace {

json::Value vec(const D3& v) {
  json::Value a = json::Value::array();
  for (f64 x : v) a.push(x);
  return a;
}
json::Value quat(const DQ& q) {
  json::Value a = json::Value::array();
  for (f64 x : q) a.push(x);
  return a;
}
void append_unknown(json::Value& o, const json::Value& unknown) {
  for (const json::Value::Member& m : unknown.members())
    if (m.first != "__unsupported" && !o.has(m.first)) o.set(m.first, m.second);
}

json::Value fixture_json(const Fixture& f) {
  json::Value o = json::Value::object();
  o.set("id", f.id);
  o.set("name", f.name);
  o.set("shape", shape_name(f.shape));
  o.set("center", vec(f.center));
  o.set("rotation", quat(f.rotation));
  o.set("size", vec(f.size));
  o.set("radius", f.radius);
  o.set("length", f.length);
  json::Value verts = json::Value::array();
  for (const D3& v : f.vertices) verts.push(vec(v));
  o.set("vertices", std::move(verts));
  o.set("density", f.density);
  o.set("friction", f.friction);
  o.set("restitution", f.restitution);
  o.set("rollingResistance", f.rolling_resistance);
  o.set("sensor", f.sensor);
  o.set("categoryBits", f.category_bits);
  o.set("maskBits", f.mask_bits);
  o.set("groupIndex", f.group_index);
  o.set("color", f.color);
  o.set("customProperties", f.custom);
  o.set("extra", f.extra);
  append_unknown(o, f.unknown);
  return o;
}

json::Value body_game_json(const BodyGame& g) {
  json::Value o = json::Value::object();
  o.set("role", game_role_name(g.role));
  o.set("control", game_control_name(g.control));
  o.set("force", g.force);
  o.set("jump", g.jump);
  o.set("cameraFollow", g.camera_follow);
  o.set("points", g.points);
  return o;
}

json::Value body_json(const Body& b) {
  json::Value o = json::Value::object();
  o.set("id", b.id);
  o.set("name", b.name);
  o.set("type", static_cast<i32>(b.type));
  o.set("position", vec(b.position));
  o.set("rotation", quat(b.rotation));
  o.set("linearVelocity", vec(b.linear_velocity));
  o.set("angularVelocity", vec(b.angular_velocity));
  o.set("linearDamping", b.linear_damping);
  o.set("angularDamping", b.angular_damping);
  o.set("gravityScale", b.gravity_scale);
  o.set("allowSleep", b.allow_sleep);
  o.set("awake", b.awake);
  o.set("active", b.active);
  o.set("bullet", b.bullet);
  json::Value fixtures = json::Value::array();
  for (const Fixture& f : b.fixtures) fixtures.push(fixture_json(f));
  o.set("fixtures", std::move(fixtures));
  o.set("customProperties", b.custom);
  o.set("extra", b.extra);
  if (!b.game.is_default()) o.set("game", body_game_json(b.game));
  append_unknown(o, b.unknown);
  return o;
}

json::Value joint_json(const Joint& j) {
  json::Value o = json::Value::object();
  o.set("id", j.id);
  o.set("name", j.name);
  o.set("type", joint_kind_name(j.type));
  o.set("bodyA", j.body_a);
  o.set("bodyB", j.body_b);
  o.set("anchorA", vec(j.anchor_a));
  o.set("anchorB", vec(j.anchor_b));
  o.set("axisA", vec(j.axis_a));
  if (j.axis_b) o.set("axisB", vec(*j.axis_b));
  o.set("enableLimit", j.enable_limit);
  o.set("enableMotor", j.enable_motor);
  o.set("lowerLimit", j.lower_limit);
  o.set("upperLimit", j.upper_limit);
  o.set("motorSpeed", j.motor_speed);
  o.set("maxMotorTorque", j.max_motor_torque);
  o.set("maxMotorForce", j.max_motor_force);
  o.set("maxForce", j.max_force);
  o.set("maxTorque", j.max_torque);
  o.set("maxLength", j.max_length);
  o.set("enableSpring", j.enable_spring);
  o.set("length", j.length);
  o.set("springFrequency", j.spring_frequency);
  if (j.spring_damping_ratio) o.set("springDampingRatio", *j.spring_damping_ratio);
  o.set("frequency", j.frequency);
  if (j.damping_ratio) o.set("dampingRatio", *j.damping_ratio);
  if (j.collide_connected) o.set("collideConnected", true);
  o.set("customProperties", j.custom);
  o.set("extra", j.extra);
  append_unknown(o, j.unknown);
  return o;
}

json::Value body_ref(const std::string& id) { return id.empty() ? json::Value() : json::Value(id); }

json::Value image_json(const ImagePlane& i, bool image_data) {
  json::Value o = json::Value::object();
  o.set("id", i.id);
  o.set("name", i.name);
  o.set("file", i.file);
  if (image_data || !i.src) o.set("src", i.src ? *i.src : std::string());
  else o.set("src", "<" + std::to_string(i.src->size()) + " bytes @" + std::to_string(reinterpret_cast<std::uintptr_t>(i.src.get())) + ">");
  o.set("body", body_ref(i.body));
  o.set("center", vec(i.center));
  o.set("rotation", quat(i.rotation));
  json::Value size = json::Value::array();
  size.push(i.size[0]);
  size.push(i.size[1]);
  o.set("size", std::move(size));
  if (i.opacity) o.set("opacity", *i.opacity);
  o.set("tint", i.tint);
  o.set("flip", i.flip);
  o.set("filter", i.filter);
  o.set("customProperties", i.custom);
  o.set("extra", i.extra);
  append_unknown(o, i.unknown);
  return o;
}

json::Value sampler_json(const Sampler& s) {
  json::Value o = json::Value::object();
  o.set("id", s.id);
  o.set("name", s.name);
  o.set("body", body_ref(s.body));
  o.set("center", vec(s.center));
  o.set("rotation", quat(s.rotation));
  o.set("size", vec(s.size));
  o.set("resolution", s.resolution);
  o.set("threshold", s.threshold);
  o.set("tolerance", s.tolerance);
  if (s.opacity) o.set("opacity", *s.opacity);
  o.set("tint", s.tint);
  o.set("flip", s.flip);
  o.set("file", s.file);
  o.set("customProperties", s.custom);
  o.set("extra", s.extra);
  append_unknown(o, s.unknown);
  return o;
}

json::Value world_json(const WorldSettings& w) {
  json::Value o = json::Value::object();
  o.set("gravity", vec(w.gravity));
  o.set("stepsPerSecond", w.steps_per_second);
  o.set("subStepCount", w.sub_step_count);
  o.set("allowSleep", w.allow_sleep);
  o.set("continuousPhysics", w.continuous_physics);
  o.set("restitutionThreshold", w.restitution_threshold);
  o.set("restitutionIterations", w.restitution_iterations);
  o.set("restitutionPropagation", w.restitution_propagation);
  if (w.custom) o.set("customProperties", *w.custom);
  append_unknown(o, w.unknown);
  return o;
}

json::Value scene_game_json(const SceneGame& g) {
  json::Value o = json::Value::object();
  o.set("title", g.title);
  o.set("goal", game_goal_name(g.goal));
  o.set("timeLimit", g.time_limit);
  o.set("cameraDistance", g.camera_distance);
  o.set("cameraHeight", g.camera_height);
  o.set("fallLimit", g.fall_limit);
  return o;
}

}  // namespace

json::Value to_json(const Scene& s, bool image_data) {
  json::Value o = json::Value::object();
  o.set("format", "editor3d");
  o.set("version", 2);
  o.set("id", s.id);
  o.set("name", s.name);
  o.set("world", world_json(s.world));
  json::Value bodies = json::Value::array(), joints = json::Value::array(), images = json::Value::array(),
              samplers = json::Value::array();
  for (const Body& b : s.bodies) bodies.push(body_json(b));
  for (const Joint& j : s.joints) joints.push(joint_json(j));
  for (const ImagePlane& i : s.images) images.push(image_json(i, image_data));
  for (const Sampler& x : s.samplers) samplers.push(sampler_json(x));
  o.set("bodies", std::move(bodies));
  o.set("joints", std::move(joints));
  o.set("images", std::move(images));
  o.set("samplers", std::move(samplers));
  o.set("cursor", vec(s.cursor));
  o.set("extra", s.extra);
  if (!s.game.is_default()) o.set("game", scene_game_json(s.game));
  append_unknown(o, s.unknown);
  return o;
}

std::string to_text(const Scene& s, i32 indent) { return json::stringify(to_json(s), indent); }

// ---------------------------------------------------------------------------
// reading

namespace {

/// Reads members of one JSON object, recording which keys were consumed.
struct Reader {
  const json::Value& o;
  std::set<std::string_view> used;
  explicit Reader(const json::Value& obj) : o(obj) {}
  const json::Value* get(std::string_view key) {
    used.insert(key);
    return o.find(key);
  }
  std::string str(std::string_view key, std::string fallback = {}) {
    const json::Value* v = get(key);
    if (!v) return fallback;
    if (v->is_string()) return v->as_string();
    if (v->is_number()) { std::string s; json::append_number(s, v->as_number()); return s; }
    return fallback;
  }
  f64 num(std::string_view key, f64 fallback) {
    const json::Value* v = get(key);
    return v && v->is_number() ? v->as_number() : fallback;
  }
  std::optional<f64> opt_num(std::string_view key) {
    const json::Value* v = get(key);
    if (v && v->is_number()) return v->as_number();
    return std::nullopt;
  }
  /// JavaScript truthiness of the member (absent = fallback).
  bool flag(std::string_view key, bool fallback) {
    const json::Value* v = get(key);
    return v ? v->truthy() : fallback;
  }
  bool vec3(std::string_view key, D3& out) {
    const json::Value* v = get(key);
    if (!v || !v->is_array() || v->size() != 3) return false;
    for (std::size_t k = 0; k < 3; ++k) {
      if (!(*v)[k].is_number()) return false;
      out[k] = (*v)[k].as_number();
    }
    return true;
  }
  bool quat(std::string_view key, DQ& out) {
    const json::Value* v = get(key);
    if (!v || !v->is_array() || v->size() != 4) return false;
    for (std::size_t k = 0; k < 4; ++k) {
      if (!(*v)[k].is_number()) return false;
      out[k] = (*v)[k].as_number();
    }
    return true;
  }
  json::Value raw(std::string_view key, json::Value fallback) {
    const json::Value* v = get(key);
    return v ? *v : fallback;
  }
  json::Value rest() const {
    json::Value r = json::Value::object();
    for (const json::Value::Member& m : o.members())
      if (!used.count(m.first)) r.set(m.first, m.second);
    return r;
  }
};

std::string body_ref_of(Reader& r, std::string_view key) {
  const json::Value* v = r.get(key);
  return v && v->is_string() ? v->as_string() : std::string();
}

void read_body_game(const json::Value* g, BodyGame& out) {
  if (!g || !g->is_object()) return;
  parse_game_role((*g)["role"].as_string(), out.role);
  parse_game_control((*g)["control"].as_string(), out.control);
  out.force = (*g)["force"].as_number(out.force);
  out.jump = (*g)["jump"].as_number(out.jump);
  out.camera_follow = (*g)["cameraFollow"].truthy();
  out.points = (*g)["points"].as_number(out.points);
}

void read_scene_game(const json::Value* g, SceneGame& out) {
  if (!g || !g->is_object()) return;
  out.title = (*g)["title"].as_string();
  parse_game_goal((*g)["goal"].as_string(), out.goal);
  out.time_limit = (*g)["timeLimit"].as_number(out.time_limit);
  out.camera_distance = (*g)["cameraDistance"].as_number(out.camera_distance);
  out.camera_height = (*g)["cameraHeight"].as_number(out.camera_height);
  out.fall_limit = (*g)["fallLimit"].as_number(out.fall_limit);
}

Fixture read_fixture(const json::Value& o, std::vector<std::string>& unsupported) {
  Fixture f;
  if (!o.is_object()) return f;
  Reader r(o);
  f.id = r.str("id");
  f.name = r.str("name");
  const std::string shape = r.str("shape", "box");
  if (!parse_shape(shape, f.shape)) unsupported.push_back(f.name + ": unsupported shape " + shape);
  r.vec3("center", f.center);
  r.quat("rotation", f.rotation);
  r.vec3("size", f.size);
  f.radius = r.num("radius", f.radius);
  f.length = r.num("length", f.length);
  if (const json::Value* v = r.get("vertices"); v && v->is_array()) {
    for (const json::Value& p : v->items()) {
      D3 d{NAN, NAN, NAN};
      if (p.is_array() && p.size() == 3)
        for (std::size_t k = 0; k < 3; ++k) d[k] = p[k].as_number(NAN);
      f.vertices.push_back(d);
    }
  }
  f.density = r.num("density", f.density);
  f.friction = r.num("friction", f.friction);
  f.restitution = r.num("restitution", f.restitution);
  f.rolling_resistance = r.num("rollingResistance", f.rolling_resistance);
  f.sensor = r.flag("sensor", false);
  f.category_bits = r.num("categoryBits", f.category_bits);
  f.mask_bits = r.num("maskBits", f.mask_bits);
  f.group_index = r.num("groupIndex", f.group_index);
  f.color = r.str("color", f.color);
  f.custom = r.raw("customProperties", json::Value::array());
  f.extra = r.raw("extra", json::Value::object());
  f.unknown = r.rest();
  return f;
}

Body read_body(const json::Value& o, std::vector<std::string>& unsupported) {
  Body b;
  if (!o.is_object()) return b;
  Reader r(o);
  b.id = r.str("id");
  b.name = r.str("name");
  const f64 type = r.num("type", -1);
  b.type = static_cast<BodyKind>(type == 0 || type == 1 || type == 2 ? static_cast<i32>(type) : -1);
  r.vec3("position", b.position);
  r.quat("rotation", b.rotation);
  r.vec3("linearVelocity", b.linear_velocity);
  r.vec3("angularVelocity", b.angular_velocity);
  b.linear_damping = r.num("linearDamping", 0);
  b.angular_damping = r.num("angularDamping", 0);
  b.gravity_scale = r.num("gravityScale", 1);
  b.allow_sleep = r.flag("allowSleep", false);
  b.awake = r.flag("awake", false);
  b.active = r.flag("active", false);
  b.bullet = r.flag("bullet", false);
  if (const json::Value* fs = r.get("fixtures"); fs && fs->is_array())
    for (const json::Value& f : fs->items()) b.fixtures.push_back(read_fixture(f, unsupported));
  read_body_game(r.get("game"), b.game);
  b.custom = r.raw("customProperties", json::Value::array());
  b.extra = r.raw("extra", json::Value::object());
  b.unknown = r.rest();
  return b;
}

Joint read_joint(const json::Value& o, std::vector<std::string>& unsupported) {
  Joint j;
  if (!o.is_object()) return j;
  Reader r(o);
  j.id = r.str("id");
  j.name = r.str("name");
  const std::string type = r.str("type");
  if (!parse_joint_kind(type, j.type)) unsupported.push_back(j.name + ": unsupported joint " + type);
  j.body_a = r.str("bodyA");
  j.body_b = r.str("bodyB");
  r.vec3("anchorA", j.anchor_a);
  r.vec3("anchorB", j.anchor_b);
  r.vec3("axisA", j.axis_a);
  D3 axis_b;
  if (r.vec3("axisB", axis_b)) j.axis_b = axis_b;
  j.enable_limit = r.flag("enableLimit", false);
  j.enable_motor = r.flag("enableMotor", false);
  j.enable_spring = r.flag("enableSpring", false);
  j.collide_connected = r.flag("collideConnected", false);
  j.lower_limit = r.num("lowerLimit", 0);
  j.upper_limit = r.num("upperLimit", 0);
  j.motor_speed = r.num("motorSpeed", 0);
  j.max_motor_torque = r.num("maxMotorTorque", 0);
  j.max_motor_force = r.num("maxMotorForce", 0);
  j.max_force = r.num("maxForce", 0);
  j.max_torque = r.num("maxTorque", 0);
  j.max_length = r.num("maxLength", 0);
  j.length = r.num("length", 1);
  j.spring_frequency = r.num("springFrequency", 0);
  j.spring_damping_ratio = r.opt_num("springDampingRatio");
  j.frequency = r.num("frequency", 0);
  j.damping_ratio = r.opt_num("dampingRatio");
  j.custom = r.raw("customProperties", json::Value::array());
  j.extra = r.raw("extra", json::Value::object());
  j.unknown = r.rest();
  return j;
}

ImagePlane read_image(const json::Value& o) {
  ImagePlane i;
  if (!o.is_object()) return i;
  Reader r(o);
  i.id = r.str("id");
  i.name = r.str("name");
  i.file = r.str("file");
  const std::string src = r.str("src");
  if (!src.empty()) i.src = std::make_shared<const std::string>(src);
  i.body = body_ref_of(r, "body");
  r.vec3("center", i.center);
  r.quat("rotation", i.rotation);
  if (const json::Value* s = r.get("size"); s && s->is_array() && s->size() >= 2) {
    i.size[0] = (*s)[0].as_number(2);
    i.size[1] = (*s)[1].as_number(2);
  }
  i.opacity = r.opt_num("opacity");
  i.tint = r.str("tint", i.tint);
  i.flip = r.flag("flip", false);
  i.filter = static_cast<i32>(r.num("filter", 0));
  i.custom = r.raw("customProperties", json::Value::array());
  i.extra = r.raw("extra", json::Value::object());
  i.unknown = r.rest();
  return i;
}

Sampler read_sampler(const json::Value& o) {
  Sampler s;
  if (!o.is_object()) return s;
  Reader r(o);
  s.id = r.str("id");
  s.name = r.str("name");
  s.body = body_ref_of(r, "body");
  r.vec3("center", s.center);
  r.quat("rotation", s.rotation);
  r.vec3("size", s.size);
  s.resolution = r.num("resolution", s.resolution);
  s.threshold = r.num("threshold", s.threshold);
  s.tolerance = r.num("tolerance", s.tolerance);
  s.opacity = r.opt_num("opacity");
  s.tint = r.str("tint", s.tint);
  s.flip = r.flag("flip", false);
  s.file = r.str("file", s.file);
  s.custom = r.raw("customProperties", json::Value::array());
  s.extra = r.raw("extra", json::Value::object());
  s.unknown = r.rest();
  return s;
}

void read_world(const json::Value* w, WorldSettings& out) {
  if (!w || !w->is_object()) return;
  Reader r(*w);
  D3 g;
  if (r.vec3("gravity", g)) out.gravity = g;
  out.steps_per_second = r.num("stepsPerSecond", out.steps_per_second);
  out.sub_step_count = r.num("subStepCount", out.sub_step_count);
  out.allow_sleep = r.flag("allowSleep", out.allow_sleep);
  out.continuous_physics = r.flag("continuousPhysics", out.continuous_physics);
  out.restitution_threshold = r.num("restitutionThreshold", out.restitution_threshold);
  out.restitution_iterations = r.num("restitutionIterations", out.restitution_iterations);
  out.restitution_propagation = r.flag("restitutionPropagation", out.restitution_propagation);
  if (const json::Value* c = r.get("customProperties")) out.custom = *c;
  out.unknown = r.rest();
}

f64 finite_or(f64 v, f64 fallback) { return std::isfinite(v) ? v : fallback; }

std::string strip_extension(std::string_view name) {
  std::string n(name);
  for (const char* ext : {".e3d", ".json", ".E3D", ".JSON"}) {
    const std::size_t len = std::strlen(ext);
    if (n.size() > len && n.compare(n.size() - len, len, ext) == 0) return n.substr(0, n.size() - len);
  }
  // Case-insensitive fallback.
  std::string lower = n;
  for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  for (const char* ext : {".e3d", ".json"}) {
    const std::size_t len = std::strlen(ext);
    if (lower.size() > len && lower.compare(lower.size() - len, len, ext) == 0) return n.substr(0, n.size() - len);
  }
  return n;
}

}  // namespace

bool from_json(const json::Value& doc, Scene& out, std::string* error, std::string_view name, bool* migrated) {
  if (migrated) *migrated = false;
  std::vector<std::string> unsupported;
  Scene s;
  const std::string& format = doc["format"].as_string();
  if (format == "kurzlernen-editor3d" && doc["objects"].is_array()) {
    s = new_scene(strip_extension(name.empty() ? std::string_view("Imported") : name));
    i32 n = 0;
    for (const json::Value& o : doc["objects"].items()) {
      ++n;
      const std::string& t = o["type"].as_string();
      const BodyKind type = t == "static" ? BodyKind::fixed : t == "kinematic" ? BodyKind::kinematic : BodyKind::dynamic;
      D3 position{};
      if (o["position"].is_array() && o["position"].size() == 3)
        for (std::size_t k = 0; k < 3; ++k) position[k] = o["position"][k].as_number(0);
      Body b = new_body(type, position, o["name"].as_string().empty() ? "Body " + std::to_string(n) : o["name"].as_string());
      if (o["rotation"].is_array() && o["rotation"].size() == 4)
        for (std::size_t k = 0; k < 4; ++k) b.rotation[k] = o["rotation"][k].as_number(0);
      b.linear_damping = finite_or(o["linearDamping"].as_number(0), 0);
      b.angular_damping = finite_or(o["angularDamping"].as_number(0), 0);
      b.gravity_scale = finite_or(o["gravityScale"].as_number(1), 1);
      b.bullet = o["bullet"].truthy();
      FixtureShape shape = FixtureShape::box;
      const std::string& sh = o["shape"].as_string();
      if (sh == "sphere") shape = FixtureShape::sphere;
      else if (sh == "capsule") shape = FixtureShape::capsule;
      Fixture f = new_fixture(shape, "fixture0");
      if (o["size"].is_array() && o["size"].size() == 3)
        for (std::size_t k = 0; k < 3; ++k) f.size[k] = std::max(0.001, finite_or(o["size"][k].as_number(1), 1));
      f.radius = std::max(0.001, finite_or(o["radius"].as_number(f.radius), f.radius));
      f.length = std::max(0.0, finite_or(o["length"].as_number(f.length), f.length));
      f.density = std::max(0.0, finite_or(o["density"].as_number(1), 1));
      f.friction = std::max(0.0, finite_or(o["friction"].as_number(0.5), 0.5));
      f.restitution = std::clamp(finite_or(o["restitution"].as_number(0), 0), 0.0, 1.0);
      if (o["color"].is_string() && !o["color"].as_string().empty()) f.color = o["color"].as_string();
      b.fixtures.push_back(std::move(f));
      s.bodies.push_back(std::move(b));
    }
    if (migrated) *migrated = true;
  } else {
    const f64 version = doc["version"].as_number(0);
    if (format != "editor3d" || (version != 1 && version != 2)) {
      if (error) *error = "Expected Editor3D .e3d / exported JSON";
      return false;
    }
    Reader r(doc);
    r.get("format");
    r.get("version");
    s.id = r.str("id");
    s.name = r.str("name", "Untitled");
    read_world(r.get("world"), s.world);
    if (const json::Value* v = r.get("bodies"); v && v->is_array())
      for (const json::Value& b : v->items()) s.bodies.push_back(read_body(b, unsupported));
    if (const json::Value* v = r.get("joints"); v && v->is_array())
      for (const json::Value& j : v->items()) s.joints.push_back(read_joint(j, unsupported));
    if (const json::Value* v = r.get("images"); v && v->is_array())
      for (const json::Value& i : v->items()) s.images.push_back(read_image(i));
    if (const json::Value* v = r.get("samplers"); v && v->is_array())
      for (const json::Value& i : v->items()) s.samplers.push_back(read_sampler(i));
    r.vec3("cursor", s.cursor);
    s.extra = r.raw("extra", json::Value::object());
    read_scene_game(r.get("game"), s.game);
    s.unknown = r.rest();
  }
  s.id = make_uid();
  if (!name.empty() || s.name.empty()) s.name = strip_extension(name.empty() ? std::string_view("Untitled") : name);
  else s.name = strip_extension(s.name);
  normalize(s);
  s.extra.remove("__unsupported");
  if (!unsupported.empty()) {
    // Kept for validate(): the scene opens, but cannot run until fixed.
    json::Value list = json::Value::array();
    for (const std::string& u : unsupported) list.push(u);
    s.unknown.set("__unsupported", std::move(list));
  }
  out = std::move(s);
  return true;
}

bool from_text(std::string_view text, Scene& out, std::string* error, std::string_view name, bool* migrated) {
  json::Value doc;
  std::string perr;
  if (!json::parse(text, doc, &perr)) {
    if (error) *error = perr;
    return false;
  }
  return from_json(doc, out, error, name, migrated);
}

void normalize(Scene& s) {
  if (!finite(s.world.gravity)) s.world.gravity = {0, 0, -9.80665};
  if (!finite(s.cursor)) s.cursor = {};
  for (Body& b : s.bodies) {
    if (!finite(b.position)) b.position = {};
    b.rotation = finite(b.rotation) ? qnormalize(b.rotation) : kIdentityQ;
    if (!finite(b.linear_velocity)) b.linear_velocity = {};
    if (!finite(b.angular_velocity)) b.angular_velocity = {};
    for (Fixture& f : b.fixtures) {
      if (!finite(f.center)) f.center = {};
      f.rotation = finite(f.rotation) ? qnormalize(f.rotation) : kIdentityQ;
    }
  }
}

std::vector<std::string> validate(const Scene& s) {
  std::vector<std::string> errors;
  auto add = [&](std::string e) {
    if (std::find(errors.begin(), errors.end(), e) == errors.end()) errors.push_back(std::move(e));
  };
  std::set<std::string> ids;
  auto check_id = [&](const std::string& id) {
    if (id.empty() || ids.count(id)) add("FATAL: Missing / duplicate ID");
    ids.insert(id);
  };
  for (const Body& b : s.bodies) check_id(b.id);
  for (const Body& b : s.bodies)
    for (const Fixture& f : b.fixtures) check_id(f.id);
  for (const Joint& j : s.joints) check_id(j.id);
  for (const ImagePlane& i : s.images) check_id(i.id);
  for (const Sampler& i : s.samplers) check_id(i.id);
  if (const json::Value* u = s.unknown.find("__unsupported"))
    for (const json::Value& e : u->items()) add("FATAL: " + e.as_string());
  for (const Body& b : s.bodies) {
    const i32 t = static_cast<i32>(b.type);
    if (t < 0 || t > 2) add("FATAL: " + b.name + ": invalid body type");
    if (!finite(b.position) || !finite(b.rotation)) add("FATAL: " + b.name + ": invalid transform");
    for (const Fixture& f : b.fixtures) {
      if (f.shape == FixtureShape::box && !(f.size[0] > 0 && f.size[1] > 0 && f.size[2] > 0 && finite(f.size)))
        add("FATAL: " + f.name + ": invalid size");
      if ((f.shape == FixtureShape::sphere || f.shape == FixtureShape::capsule) && !(f.radius > 0))
        add("FATAL: " + f.name + ": radius must be positive");
      if (f.shape == FixtureShape::capsule && !(f.length >= 0)) add("FATAL: " + f.name + ": invalid length");
      if (f.shape == FixtureShape::hull) {
        bool ok = f.vertices.size() >= 4;
        for (const D3& p : f.vertices) ok = ok && finite(p);
        if (!ok) add("FATAL: " + f.name + ": hull requires at least four finite points");
      }
    }
  }
  for (const Joint& j : s.joints) {
    if (!s.body(j.body_a) || !s.body(j.body_b) || j.body_a == j.body_b) add("FATAL: " + j.name + ": invalid body connection");
    if (j.enable_limit && j.lower_limit > j.upper_limit) add("FATAL: " + j.name + ": lower limit exceeds upper limit");
  }
  if (!(s.world.steps_per_second > 0 && s.world.steps_per_second <= 1000)) add("FATAL: Steps per second must be 1–1000");
  if (!(s.world.sub_step_count >= 1 && s.world.sub_step_count <= 32)) add("FATAL: Substeps must be 1–32");
  return errors;
}

f64 fixture_radius(const Fixture& f) {
  switch (f.shape) {
    case FixtureShape::box: return 0.5 * std::hypot(f.size[0], f.size[1], f.size[2]);
    case FixtureShape::sphere: return std::max(0.001, f.radius > 0 ? f.radius : 0.5);
    case FixtureShape::capsule: return std::max(0.001, f.radius > 0 ? f.radius : 0.5) + std::max(0.0, f.length) * 0.5;
    case FixtureShape::hull: {
      f64 r = 0.001;
      for (const D3& p : f.vertices) if (finite(p)) r = std::max(r, length(p));
      return r;
    }
  }
  return 0.1;
}

f64 physical_span(const Scene& s) {
  D3 lo{INFINITY, INFINITY, INFINITY}, hi{-INFINITY, -INFINITY, -INFINITY};
  bool any = false;
  for (const Body& b : s.bodies)
    for (const Fixture& f : b.fixtures) {
      const D3 c = local_to_world(b.pose(), f.center);
      const f64 r = fixture_radius(f);
      for (int k = 0; k < 3; ++k) { lo[k] = std::min(lo[k], c[k] - r); hi[k] = std::max(hi[k], c[k] + r); }
      any = true;
    }
  return any ? std::max({0.001, hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]}) : 1.0;
}

DPose item_world_pose(const Scene& s, const std::string& body, const D3& center, const DQ& rotation) {
  if (const Body* b = body.empty() ? nullptr : s.body(body)) return compose(b->pose(), center, rotation);
  return {center, rotation};
}

}  // namespace kern::world
