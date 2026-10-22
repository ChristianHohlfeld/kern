#include "kern/game/runtime.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <map>
#include <unordered_set>

#include "kern/geo/primitives.h"

namespace kern::game {

namespace {

constexpr f32 kDeg = kPi / 180.0f;

f32 smoothstep(f32 a, f32 b, f32 x) {
  const f32 t = clamp((x - a) / (b - a), 0.0f, 1.0f);
  return t * t * (3 - 2 * t);
}
Vec3 mix3(Vec3 a, Vec3 b, f32 t) { return a + (b - a) * t; }
f32 damp(f32 current, f32 target, f32 rate, f32 dt) { return target + (current - target) * std::exp(-rate * dt); }
Vec3 damp3(Vec3 current, Vec3 target, f32 rate, f32 dt) { return target + (current - target) * std::exp(-rate * dt); }

u64 fnv(const std::string& s, u64 h = 1469598103934665603ull) {
  for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
  return h;
}

Mat4 inverse(const Mat4& m) {
  f64 inv[16];
  Mat4 r;
  if (!invert(m, inv)) return r;
  for (int i = 0; i < 16; ++i) r.m[i] = static_cast<f32>(inv[i]);
  return r;
}

/// Rotation of a matrix with scale removed.
Quat matrix_rotation(const Mat4& m) {
  const Transform t = matrix_transform(m);
  return euler_to_quat(t.rotation);
}

Mat4 pose_matrix(Vec3 p, Quat q) { return Mat4::trs(p, q, {1, 1, 1}); }

Quat quat_normalize(Quat q) {
  const f32 n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
  return n > 1e-9f ? Quat{q.x / n, q.y / n, q.z / n, q.w / n} : Quat{};
}

f32 yaw_of(Quat q) {
  const Vec3 f = q.rotate({1, 0, 0});
  return std::atan2(f.y, f.x);
}

void push_line(std::vector<f32>& p, std::vector<f32>& c, Vec3 a, Vec3 b, Vec3 color) {
  p.insert(p.end(), {a.x, a.y, a.z, b.x, b.y, b.z});
  c.insert(c.end(), {color.x, color.y, color.z, color.x, color.y, color.z});
}

/// Local line segments (pairs) outlining a planned shape.
void shape_lines(const ShapePlan& s, std::vector<Vec3>& out) {
  auto circle = [&](Vec3 center, Vec3 u, Vec3 v, f32 r) {
    constexpr int n = 24;
    for (int i = 0; i < n; ++i) {
      const f32 a0 = i * 2 * kPi / n, a1 = (i + 1) * 2 * kPi / n;
      out.push_back(center + (u * std::cos(a0) + v * std::sin(a0)) * r);
      out.push_back(center + (u * std::cos(a1) + v * std::sin(a1)) * r);
    }
  };
  const Vec3 X = s.rotation.rotate({1, 0, 0}), Y = s.rotation.rotate({0, 1, 0}), Z = s.rotation.rotate({0, 0, 1});
  switch (s.kind) {
    case physics::ShapeKind::box: {
      const Vec3 h = s.size * 0.5f;
      Vec3 corners[8];
      for (int i = 0; i < 8; ++i)
        corners[i] = s.center + X * (i & 1 ? h.x : -h.x) + Y * (i & 2 ? h.y : -h.y) + Z * (i & 4 ? h.z : -h.z);
      const int e[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
      for (const auto& k : e) { out.push_back(corners[k[0]]); out.push_back(corners[k[1]]); }
      break;
    }
    case physics::ShapeKind::sphere:
      circle(s.center, X, Y, s.radius);
      circle(s.center, X, Z, s.radius);
      circle(s.center, Y, Z, s.radius);
      break;
    case physics::ShapeKind::capsule: {
      const Vec3 a = s.center - Z * (s.length * 0.5f), b = s.center + Z * (s.length * 0.5f);
      circle(a, X, Y, s.radius);
      circle(b, X, Y, s.radius);
      for (Vec3 d : {X, Y, X * -1.0f, Y * -1.0f}) { out.push_back(a + d * s.radius); out.push_back(b + d * s.radius); }
      // Cap arcs.
      for (Vec3 u : {X, Y}) {
        constexpr int n = 12;
        for (int i = 0; i < n; ++i) {
          const f32 a0 = i * kPi / n, a1 = (i + 1) * kPi / n;
          out.push_back(b + (u * std::cos(a0) + Z * std::sin(a0)) * s.radius);
          out.push_back(b + (u * std::cos(a1) + Z * std::sin(a1)) * s.radius);
          out.push_back(a + (u * std::cos(a0) - Z * std::sin(a0)) * s.radius);
          out.push_back(a + (u * std::cos(a1) - Z * std::sin(a1)) * s.radius);
        }
      }
      break;
    }
    case physics::ShapeKind::hull: {
      MeshData m;
      if (!physics::hull_mesh(Span<const Vec3>(s.points.data(), s.points.size()), m)) break;
      // Feature edges: edges between triangles with different normals.
      std::map<std::pair<std::array<i32, 3>, std::array<i32, 3>>, std::vector<Vec3>> edges;
      auto key = [](Vec3 p) { return std::array<i32, 3>{static_cast<i32>(std::lround(p.x * 1e4f)), static_cast<i32>(std::lround(p.y * 1e4f)), static_cast<i32>(std::lround(p.z * 1e4f))}; };
      for (u32 t = 0; t + 2 < m.vertex_count(); t += 3) {
        const Vec3 n = Vec3::load(&m.normals[t * 3]);
        for (u32 k = 0; k < 3; ++k) {
          Vec3 a = Vec3::load(&m.positions[(t + k) * 3]), b = Vec3::load(&m.positions[(t + (k + 1) % 3) * 3]);
          auto ka = key(a), kb = key(b);
          if (kb < ka) { std::swap(ka, kb); std::swap(a, b); }
          auto& list = edges[{ka, kb}];
          if (list.empty()) { list.push_back(a); list.push_back(b); }
          list.push_back(n);
        }
      }
      for (const auto& [k, v] : edges) {
        bool feature = v.size() < 4;
        for (std::size_t i = 3; i < v.size() && !feature; ++i) feature = dot(v[i], v[2]) < 0.999f;
        if (feature) { out.push_back(v[0]); out.push_back(v[1]); }
      }
      break;
    }
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// night windows

namespace {
constexpr ExtensionAttribute kWallAttribute[] = {{"wall", 4, "aWall"}};
}

Span<const ExtensionAttribute> NightWindows::attributes() const { return {kWallAttribute, 1}; }
const char* NightWindows::vertex_declarations() const { return "flat out vec4 vWall;\n"; }
const char* NightWindows::vertex() const { return "  vWall = aWall;\n"; }
const char* NightWindows::fragment_declarations() const {
  return R"GLSL(
// flat: the record is constant per wall; interpolation noise would scramble the window hash.
flat in vec4 vWall;
uniform float uNight;
float windowHash(float n) { return fract(sin(n * 91.7) * 43758.5453); }
)GLSL";
}
const char* NightWindows::fragment_surface() const {
  return R"GLSL(
  if (vWall.x > 0.0 && abs(n.z) < 0.5) {
    float u = vUV.x, z = vUV.y, storey = vWall.w;
    float column = floor(u / 3.0), floorIndex = floor(z / storey);
    vec2 cell = vec2(fract(u / 3.0), fract(z / storey));
    vec2 fw = fwidth(vec2(u / 3.0, z / storey));
    float farFade = 1.0 - smoothstep(0.08, 0.3, max(fw.x, fw.y));
    float win = step(0.22, cell.x) * step(cell.x, 0.78) * step(0.28, cell.y) * step(cell.y, 0.78)
              * step(storey, z) * step(z, vWall.y - 1.0) * farFade;
    float lit = step(0.42, windowHash(column * 7.31 + floorIndex * 17.0 + floor(vWall.z * 997.0 + 0.5)));
    base = mix(base, vec3(0.05, 0.06, 0.08), win * 0.85);
    surfaceGlass = max(surfaceGlass, win * (1.0 - lit * uNight) * 0.7);
    float tall = step(storey, z) * step(z, vWall.y - 1.0);
    emissive += (win * lit + (1.0 - farFade) * tall * 0.13) * uNight * vec3(1.0, 0.74, 0.42) * 1.6;
  }
)GLSL";
}
void NightWindows::bind(const Program& p) { u_night_ = p.location("uNight"); }
void NightWindows::frame_uniforms(UniformCache& cache, const Renderer&) { cache.set_float(u_night_, night); }

const char* game_status_name(GameStatus s) {
  static const char* names[] = {"inactive", "playing", "won", "lost"};
  return names[static_cast<u32>(s)];
}

// ---------------------------------------------------------------------------
// body planning

namespace {

struct Placement {
  Vec3 position;
  Quat rotation;
  Vec3 scale{1, 1, 1};
};

Placement placement_of(const Mat4& m) {
  const Transform t = matrix_transform(m);
  return {t.position, euler_to_quat(t.rotation), t.scale};
}

f32 max3(Vec3 v) { return max_of(std::fabs(v.x), max_of(std::fabs(v.y), std::fabs(v.z))); }

/// Adds the collider `c` of an actor whose matrix relative to the body frame is `rel`.
void add_shape(BodyPlan& body, ActorId actor, const ColliderComponent& c, const Mat4& rel, const MeshComponent* mesh, bool automatic) {
  ShapePlan s;
  s.actor = actor;
  s.source = c;
  s.automatic = automatic;
  const Mat4 local = rel * Mat4::trs(c.center, euler_to_quat(c.rotation), {1, 1, 1});
  const Placement p = placement_of(rel);
  const Placement lp = placement_of(local);
  s.center = lp.position;
  s.rotation = lp.rotation;
  switch (c.shape) {
    case ColliderShape::box:
      s.kind = physics::ShapeKind::box;
      s.size = {max_of(0.002f, c.size.x * std::fabs(p.scale.x)), max_of(0.002f, c.size.y * std::fabs(p.scale.y)), max_of(0.002f, c.size.z * std::fabs(p.scale.z))};
      break;
    case ColliderShape::sphere:
      s.kind = physics::ShapeKind::sphere;
      s.radius = max_of(0.002f, c.radius * max3(p.scale));
      break;
    case ColliderShape::capsule:
      s.kind = physics::ShapeKind::capsule;
      s.radius = max_of(0.002f, c.radius * max_of(std::fabs(p.scale.x), std::fabs(p.scale.y)));
      s.length = max_of(0.0f, c.length * std::fabs(p.scale.z));
      break;
    case ColliderShape::hull:
    case ColliderShape::automatic: {
      s.kind = physics::ShapeKind::hull;
      std::vector<Vec3> pts;
      if (mesh) pts = shape_hull_points(*mesh);
      else {
        const Vec3 h = c.size * 0.5f;
        for (int i = 0; i < 8; ++i) pts.push_back({i & 1 ? h.x : -h.x, i & 2 ? h.y : -h.y, i & 4 ? h.z : -h.z});
      }
      // Hull points in the body frame (scale exact); the shape frame is identity.
      for (Vec3& q : pts) q = local.transform_point(q);
      s.points = std::move(pts);
      s.center = {};
      s.rotation = {};
      break;
    }
  }
  body.shapes.push_back(std::move(s));
}

}  // namespace

std::vector<BodyPlan> plan_bodies(const Level& level) {
  std::vector<BodyPlan> plans;
  std::unordered_map<ActorId, std::size_t> index;
  std::unordered_map<ActorId, Mat4> world;
  for (const Actor& a : level.actors) world[a.id] = level.world_matrix(a.id);
  auto frame_of = [&](BodyPlan& b, const Mat4& m) {
    const Placement p = placement_of(m);
    b.position = p.position;
    b.rotation = p.rotation;
  };
  // 1. Bodies of actors with a body component.
  for (const Actor& a : level.actors) {
    if (!a.body || a.city) continue;
    BodyPlan b;
    b.owner = a.id;
    b.motion = a.body->motion;
    if (b.motion == Motion::fixed && (a.rotator || a.mover)) b.motion = Motion::kinematic;
    frame_of(b, world[a.id]);
    index[a.id] = plans.size();
    plans.push_back(std::move(b));
  }
  // 2. Implicit bodies: meshes with collision, pickups and triggers outside any body.
  for (const Actor& a : level.actors) {
    if (a.city || level.body_owner(a.id)) continue;
    const bool wants = !a.colliders.empty() || (a.mesh && a.mesh->collision) || a.pickup || a.trigger;
    if (!wants) continue;
    // Animated ancestors make the implicit body kinematic.
    bool animated = false;
    for (const Actor* p = &a; p; p = level.find(p->parent)) animated |= p->rotator.has_value() || p->mover.has_value();
    BodyPlan b;
    b.owner = a.id;
    b.implicit = true;
    b.motion = animated ? Motion::kinematic : Motion::fixed;
    frame_of(b, world[a.id]);
    index[a.id] = plans.size();
    plans.push_back(std::move(b));
  }
  // 3. Shapes: every actor contributes to its body owner (itself, an ancestor or its implicit body).
  std::unordered_map<ActorId, bool> has_explicit;
  for (const Actor& a : level.actors) {
    ActorId owner = level.body_owner(a.id);
    if (!owner && index.count(a.id)) owner = a.id;
    if (!owner || !index.count(owner)) continue;
    // An actor with its own implicit body does not weld into an ancestor's implicit body.
    BodyPlan& b = plans[index[owner]];
    const Mat4 frame = pose_matrix(b.position, b.rotation);
    const Mat4 rel = inverse(frame) * world[a.id];
    const bool trigger_like = a.pickup || a.trigger;
    if (!a.colliders.empty()) {
      for (const ColliderComponent& c : a.colliders) {
        ColliderComponent cc = c;
        if (cc.shape == ColliderShape::automatic && a.mesh) {
          const ColliderComponent fit = fit_collider(*a.mesh);
          cc.shape = fit.shape;
          cc.size = fit.size;
          cc.radius = fit.radius;
          cc.length = fit.length;
          cc.center = cc.center + fit.center;
        }
        add_shape(b, a.id, cc, rel, a.mesh ? &*a.mesh : nullptr, c.shape == ColliderShape::automatic);
      }
    } else if (a.mesh && (a.mesh->collision || trigger_like)) {
      ColliderComponent fit = fit_collider(*a.mesh);
      if (trigger_like) fit.sensor = true;
      add_shape(b, a.id, fit, rel, &*a.mesh, true);
    } else if (trigger_like) {
      ColliderComponent s;
      s.shape = ColliderShape::sphere;
      s.radius = 1;
      s.sensor = true;
      add_shape(b, a.id, s, rel, nullptr, true);
    }
  }
  // Bodies without any shape still simulate (point masses) unless they are implicit.
  plans.erase(std::remove_if(plans.begin(), plans.end(), [](const BodyPlan& b) { return b.implicit && b.shapes.empty(); }), plans.end());
  // 4. Cities: one static body with a box per building and pavement.
  for (const Actor& a : level.actors) {
    if (!a.city) continue;
    CityGeometry geo;
    if (!build_city(*a.city, geo)) continue;
    BodyPlan b;
    b.owner = a.id;
    b.motion = Motion::fixed;
    frame_of(b, world[a.id]);
    for (const CityBox& box : geo.colliders) {
      ShapePlan s;
      s.actor = a.id;
      s.kind = physics::ShapeKind::box;
      s.center = box.center;
      s.size = box.size;
      s.source.friction = 0.5f;
      b.shapes.push_back(s);
    }
    plans.push_back(std::move(b));
  }
  // 5. The endless ground.
  if (level.world.ground) {
    BodyPlan g;
    g.owner = 0;
    g.motion = Motion::fixed;
    ShapePlan s;
    s.kind = physics::ShapeKind::box;
    s.center = {0, 0, -1};
    s.size = {20000, 20000, 2};
    s.source.friction = 0.7f;
    g.shapes.push_back(s);
    plans.push_back(std::move(g));
  }
  return plans;
}

// ---------------------------------------------------------------------------
// world

struct GameWorld::Visual {
  u32 mesh = 0;
  u64 key = 0;
  bool visible = true;
  Mat4 world;
};

struct GameWorld::CityVisual {
  u64 key = 0;
  std::vector<u32> chunk_meshes;  // one instance = the city matrix
  u32 trunk = 0, crown = 0;
  CityGeometry geo;
  Mat4 world;
  bool visible = true;
};

struct GameWorld::SimBody {
  BodyPlan plan;
  physics::Body body;
  Vec3 scale{1, 1, 1};    // owner's world scale (render)
  bool animated = false;  // kinematic driven by rotator/mover
};

struct GameWorld::SimJoint {
  ActorId actor = 0, target = 0;
  physics::Joint joint;
  physics::Body a, b;
  physics::Pose frame_a, frame_b;
  JointKind kind = JointKind::hinge;
  bool broken = false;
};

struct GameWorld::Traffic {
  ActorId city = 0;
  bool horizontal = false;
  f32 line = 0, lane = 0, along = 0, speed = 0, dir = 1, span = 0;
  Vec4 tint{1, 1, 1, 1};
  physics::Body body;
  Mat4 city_matrix;
  Vec2 local() const { return horizontal ? Vec2{along, line + lane} : Vec2{line + lane, along}; }
  f32 yaw() const { return horizontal ? (dir > 0 ? 0.0f : kPi) : (dir > 0 ? kPi * 0.5f : -kPi * 0.5f); }
};

GameWorld::GameWorld(Renderer& renderer, NightWindows* windows) : r_(renderer), windows_(windows) {}

GameWorld::~GameWorld() {
  stop();
  clear();
}

void GameWorld::set_layers(u32 layers) {
  layers_ = layers ? layers : 1;
  for (auto& [id, v] : visuals_) r_.set_layers(v->mesh, layers_);
  for (auto& [id, c] : cities_) {
    for (u32 m : c->chunk_meshes) r_.set_layers(m, layers_);
    if (c->trunk) r_.set_layers(c->trunk, layers_);
    if (c->crown) r_.set_layers(c->crown, layers_);
  }
  if (ground_mesh_) r_.set_layers(ground_mesh_, layers_);
  if (traffic_mesh_) r_.set_layers(traffic_mesh_, layers_);
}

void GameWorld::clear() {
  for (auto& [id, v] : visuals_) if (v->mesh) r_.remove(v->mesh);
  visuals_.clear();
  for (auto& [id, c] : cities_) {
    for (u32 m : c->chunk_meshes) r_.remove(m);
    if (c->trunk) r_.remove(c->trunk);
    if (c->crown) r_.remove(c->crown);
  }
  cities_.clear();
  mesh_owner_.clear();
  if (ground_mesh_) r_.remove(ground_mesh_);
  ground_mesh_ = 0;
  ground_key_ = 0;
  if (traffic_mesh_) r_.remove(traffic_mesh_);
  traffic_mesh_ = 0;
  for (auto& [id, t] : textures_) if (t.handle) r_.delete_texture(t.handle);
  textures_.clear();
  if (particles_ready_) particles_.clear();
  world_.clear();
}

Material GameWorld::material_for(const MaterialDesc& m, u32* texture) const {
  Material out;
  out.color = {m.color.x, m.color.y, m.color.z, clamp(m.opacity, 0.0f, 1.0f)};
  out.alpha = m.opacity < 0.999f ? AlphaMode::blend : AlphaMode::opaque;
  out.roughness = clamp(m.roughness, 0.0f, 1.0f);
  out.metalness = clamp(m.metalness, 0.0f, 1.0f);
  out.emissive = srgb_to_linear(m.emissive) * max_of(0.0f, m.emissive_strength);
  out.unlit = m.unlit;
  out.double_sided = m.double_sided;
  out.cast_shadow = m.cast_shadow && m.opacity >= 0.999f;
  *texture = 0;
  if (!m.texture.empty() && level_.texture(m.texture)) {
    TextureSlot& slot = textures_[m.texture];
    if (slot.handle) *texture = slot.handle;
    else if (!slot.failed) slot.requested = true;
  }
  return out;
}

std::vector<std::string> GameWorld::pending_textures() const {
  std::vector<std::string> out;
  for (auto& [id, t] : textures_)
    if (t.requested && !t.handle && !t.failed) out.push_back(id);
  return out;
}

bool GameWorld::set_texture_pixels(const std::string& id, i32 width, i32 height, const u8* rgba) {
  TextureSlot& slot = textures_[id];
  slot.requested = false;
  if (width <= 0 || height <= 0 || !rgba) { slot.failed = true; return false; }
  TextureDesc d;
  d.rgba = rgba;
  d.width = width;
  d.height = height;
  d.repeat = true;
  d.mipmaps = true;
  if (slot.handle) r_.delete_texture(slot.handle);
  slot.handle = r_.create_texture(d);
  if (!slot.handle) { slot.failed = true; return false; }
  // Re-apply materials that use it.
  for (auto& [aid, v] : visuals_) v->key = 0;
  if (!running()) sync(level_);
  else {
    for (const Actor& a : level_.actors)
      if (a.mesh && a.mesh->material.texture == id) sync_actor_mesh(a, world_[a.id]);
  }
  return true;
}

void GameWorld::sync_ground(const WorldSettings& w) {
  const u64 key = w.ground ? fnv(hex_color(w.ground_color)) : 0;
  if (key == ground_key_ && (ground_mesh_ != 0) == w.ground) return;
  ground_key_ = key;
  if (!w.ground) {
    if (ground_mesh_) r_.remove(ground_mesh_);
    ground_mesh_ = 0;
    return;
  }
  MeshData g = primitives::plane(6000, 6000, w.ground_color);
  MeshInput in = g.as_input();
  in.material.cast_shadow = false;
  in.material.roughness = 1;
  in.layers = layers_;
  if (!ground_mesh_) ground_mesh_ = r_.allocate_id();
  // Slightly below z = 0 so planes and pavements placed at 0 win the depth test.
  const Mat4 t = Mat4::translate({0, 0, -0.01f});
  in.transforms = Span<const f32>(t.m, 16);
  if (!r_.upsert(ground_mesh_, in)) { r_.release_id(ground_mesh_); ground_mesh_ = 0; }
}

void GameWorld::sync_actor_mesh(const Actor& a, const Mat4& world) {
  auto& slot = visuals_[a.id];
  if (!slot) slot = std::make_unique<Visual>();
  Visual& v = *slot;
  json::Value desc = actor_to_json(a)["components"]["mesh"];
  u32 texture = 0;
  const Material mat = material_for(a.mesh->material, &texture);
  const u64 key = fnv(json::stringify(desc)) ^ (static_cast<u64>(texture) * 0x9e3779b97f4a7c15ull);
  bool visible = !a.hidden;
  for (const Actor* p = level_.find(a.parent); p && visible; p = level_.find(p->parent)) visible = !p->hidden;
  if (running()) visible = visible && alive_[a.id];
  if (key != v.key || !v.mesh) {
    const MeshData m = shape_mesh(*a.mesh);
    MeshInput in = m.as_input(kMaxExtensionAttributes);
    in.material = mat;
    in.texture = texture;
    in.layers = layers_;
    in.visible = visible;
    in.transforms = Span<const f32>(world.m, 16);
    if (!v.mesh) v.mesh = r_.allocate_id();
    if (!r_.upsert(v.mesh, in)) {
      if (!r_.mesh(v.mesh)) { r_.release_id(v.mesh); v.mesh = 0; }
      return;
    }
    mesh_owner_[v.mesh] = a.id;
    v.key = key;
    v.world = world;
    v.visible = visible;
    return;
  }
  if (std::memcmp(v.world.m, world.m, sizeof world.m) != 0) {
    r_.update_instances(v.mesh, Span<const f32>(world.m, 16), {});
    v.world = world;
  }
  if (visible != v.visible) {
    r_.set_visible(v.mesh, visible);
    v.visible = visible;
  }
}

void GameWorld::sync_city(const Actor& a, const Mat4& world) {
  auto& slot = cities_[a.id];
  if (!slot) slot = std::make_unique<CityVisual>();
  CityVisual& c = *slot;
  const u64 key = fnv(json::stringify(actor_to_json(a)["components"]["city"]));
  const bool visible = !a.hidden;
  if (key != c.key) {
    for (u32 m : c.chunk_meshes) { mesh_owner_.erase(m); r_.remove(m); }
    c.chunk_meshes.clear();
    if (c.trunk) { mesh_owner_.erase(c.trunk); r_.remove(c.trunk); c.trunk = 0; }
    if (c.crown) { mesh_owner_.erase(c.crown); r_.remove(c.crown); c.crown = 0; }
    c.key = key;
    if (!build_city(*a.city, c.geo)) return;
    auto upload = [&](const MeshData& mesh, const Material& material, u32 extension_slot) {
      const u32 id = r_.allocate_id();
      MeshInput in = mesh.as_input(extension_slot);
      in.material = material;
      in.layers = layers_;
      in.visible = visible;
      in.transforms = Span<const f32>(world.m, 16);
      if (!r_.upsert(id, in)) { r_.release_id(id); return; }
      c.chunk_meshes.push_back(id);
      mesh_owner_[id] = a.id;
    };
    Material building;
    building.roughness = 1;
    for (const MeshData& m : c.geo.buildings) upload(m, building, a.city->night_windows ? 0 : kMaxExtensionAttributes);
    Material road;
    road.cast_shadow = false;
    road.double_sided = true;
    road.roughness = 1;
    for (const MeshData& m : c.geo.roads) upload(m, road, kMaxExtensionAttributes);
    Material plate;
    plate.roughness = 0.95f;
    if (c.geo.plates.vertex_count()) upload(c.geo.plates, plate, kMaxExtensionAttributes);
    c.world = Mat4{};
    c.world.m[0] = 0;  // force the tree upload below
  }
  // Trees: per-instance transforms combine the city matrix.
  if ((std::memcmp(c.world.m, world.m, sizeof world.m) != 0 || !c.trunk) && !c.geo.trunk_transforms.empty()) {
    auto instanced = [&](u32& id, const MeshData& mesh, const std::vector<f32>& local, const std::vector<f32>& tints) {
      std::vector<f32> t(local.size());
      for (std::size_t i = 0; i + 15 < local.size(); i += 16) {
        const Mat4 m = world * Mat4::load(&local[i]);
        std::memcpy(&t[i], m.m, sizeof m.m);
      }
      if (id && r_.mesh(id)) { r_.update_instances(id, Span<const f32>(t.data(), t.size()), Span<const f32>(tints.data(), tints.size())); return; }
      id = r_.allocate_id();
      MeshInput in = mesh.as_input(kMaxExtensionAttributes);
      in.layers = layers_;
      in.visible = visible;
      in.material.roughness = 0.9f;
      in.transforms = Span<const f32>(t.data(), t.size());
      in.tints = Span<const f32>(tints.data(), tints.size());
      if (!r_.upsert(id, in)) { r_.release_id(id); id = 0; return; }
      mesh_owner_[id] = a.id;
    };
    instanced(c.trunk, c.geo.trunk, c.geo.trunk_transforms, {});
    instanced(c.crown, c.geo.crown, c.geo.crown_transforms, c.geo.crown_tints);
  }
  if (std::memcmp(c.world.m, world.m, sizeof world.m) != 0) {
    for (u32 m : c.chunk_meshes) r_.update_instances(m, Span<const f32>(world.m, 16), {});
    c.world = world;
  }
  if (visible != c.visible) {
    for (u32 m : c.chunk_meshes) r_.set_visible(m, visible);
    if (c.trunk) r_.set_visible(c.trunk, visible);
    if (c.crown) r_.set_visible(c.crown, visible);
    c.visible = visible;
  }
}

void GameWorld::remove_visual(ActorId id) {
  if (auto it = visuals_.find(id); it != visuals_.end()) {
    if (it->second->mesh) { mesh_owner_.erase(it->second->mesh); r_.remove(it->second->mesh); }
    visuals_.erase(it);
  }
  if (auto it = cities_.find(id); it != cities_.end()) {
    for (u32 m : it->second->chunk_meshes) { mesh_owner_.erase(m); r_.remove(m); }
    if (it->second->trunk) { mesh_owner_.erase(it->second->trunk); r_.remove(it->second->trunk); }
    if (it->second->crown) { mesh_owner_.erase(it->second->crown); r_.remove(it->second->crown); }
    cities_.erase(it);
  }
}

namespace {

/// Initial traffic of a city (deterministic for the seed).
template <typename T>
void plan_traffic(const CityComponent& city, std::vector<T>& out) {
  Rng rng(city.seed ^ 0x7a3f1c9bu);
  const f32 half = city.blocks * city.pitch * 0.5f;
  for (i32 i = 0; i < city.traffic; ++i) {
    T c;
    c.horizontal = rng.unit() < 0.5f;
    c.line = -half + static_cast<f32>(rng.below(static_cast<u32>(city.blocks) + 1)) * city.pitch;
    c.dir = rng.unit() < 0.5f ? 1.0f : -1.0f;
    c.lane = c.dir * city.street * 0.175f;
    c.along = rng.range(-half, half);
    c.speed = rng.range(9, 15);
    c.span = half + 60;
    const Vec3 colors[5] = {{0.85f, 0.12f, 0.1f}, {0.1f, 0.3f, 0.8f}, {0.95f, 0.8f, 0.2f}, {0.9f, 0.9f, 0.9f}, {0.15f, 0.6f, 0.3f}};
    const Vec3 c3 = colors[rng.below(5)];
    c.tint = {c3.x, c3.y, c3.z, 1};
    out.push_back(c);
  }
}

MeshData traffic_car_mesh() {
  MeshData car;
  primitives::append(car, primitives::box({4.0f, 1.9f, 0.9f}, {1, 1, 1}), Mat4::translate({0, 0, 0.75f}));
  primitives::append(car, primitives::box({2.0f, 1.7f, 0.6f}, {0.1f, 0.12f, 0.16f}), Mat4::translate({-0.2f, 0, 1.5f}));
  return car;
}

}  // namespace

void GameWorld::sync(const Level& level) {
  if (running()) return;
  level_ = level;
  edit_level_ = level;
  time_of_day_ = level.world.time_of_day;
  if (!particles_ready_) particles_ready_ = particles_.init(r_, 512);
  sync_ground(level.world);
  world_.clear();
  for (const Actor& a : level_.actors) world_[a.id] = level_.world_matrix(a.id);
  std::unordered_set<ActorId> keep_mesh, keep_city;
  for (const Actor& a : level_.actors) {
    if (a.mesh) { sync_actor_mesh(a, world_[a.id]); keep_mesh.insert(a.id); }
    if (a.city) { sync_city(a, world_[a.id]); keep_city.insert(a.id); }
  }
  std::vector<ActorId> drop;
  for (auto& [id, v] : visuals_) if (!keep_mesh.count(id)) drop.push_back(id);
  for (ActorId id : drop) {
    if (auto it = visuals_.find(id); it != visuals_.end()) {
      if (it->second->mesh) { mesh_owner_.erase(it->second->mesh); r_.remove(it->second->mesh); }
      visuals_.erase(it);
    }
  }
  drop.clear();
  for (auto& [id, c] : cities_) if (!keep_city.count(id)) drop.push_back(id);
  for (ActorId id : drop) remove_visual(id);
  // Parked traffic for the editor view.
  std::vector<f32> transforms, tints;
  for (const Actor& a : level_.actors) {
    if (!a.city || a.hidden || a.city->traffic <= 0) continue;
    std::vector<Traffic> cars;
    plan_traffic(*a.city, cars);
    for (const Traffic& t : cars) {
      const Vec2 p = t.local();
      const Mat4 m = world_[a.id] * Mat4::trs({p.x, p.y, 0}, Quat::axis_angle({0, 0, 1}, t.yaw()), {1, 1, 1});
      transforms.insert(transforms.end(), m.m, m.m + 16);
      tints.insert(tints.end(), {t.tint.x, t.tint.y, t.tint.z, t.tint.w});
    }
  }
  if (transforms.empty()) {
    if (traffic_mesh_) r_.set_visible(traffic_mesh_, false);
  } else if (traffic_mesh_ && r_.mesh(traffic_mesh_) && r_.mesh(traffic_mesh_)->instance_count == transforms.size() / 16) {
    r_.set_visible(traffic_mesh_, true);
    r_.update_instances(traffic_mesh_, Span<const f32>(transforms.data(), transforms.size()), Span<const f32>(tints.data(), tints.size()));
  } else {
    if (!traffic_mesh_) traffic_mesh_ = r_.allocate_id();
    const MeshData car = traffic_car_mesh();
    MeshInput in = car.as_input(kMaxExtensionAttributes);
    in.material.roughness = 0.4f;
    in.material.metalness = 0.2f;
    in.layers = layers_;
    in.transforms = Span<const f32>(transforms.data(), transforms.size());
    in.tints = Span<const f32>(tints.data(), tints.size());
    if (!r_.upsert(traffic_mesh_, in) && !r_.mesh(traffic_mesh_)) { r_.release_id(traffic_mesh_); traffic_mesh_ = 0; }
  }
}

ActorId GameWorld::actor_for_mesh(u32 mesh) const {
  const auto it = mesh_owner_.find(mesh);
  return it == mesh_owner_.end() ? 0 : it->second;
}

bool GameWorld::actor_matrix(ActorId id, Mat4& out) const {
  const auto it = world_.find(id);
  if (it == world_.end()) return false;
  out = it->second;
  return true;
}

bool GameWorld::actor_bounds(ActorId id, Aabb& out) const {
  out = Aabb{};
  const Actor* a = level_.find(id);
  if (!a) return false;
  const auto w = world_.find(id);
  if (w == world_.end()) return false;
  if (a->mesh) {
    const Aabb b = shape_bounds(*a->mesh);
    for (int i = 0; i < 8; ++i) out.extend(w->second.transform_point({i & 1 ? b.max.x : b.min.x, i & 2 ? b.max.y : b.min.y, i & 4 ? b.max.z : b.min.z}));
  }
  if (a->city) {
    const f32 half = a->city->blocks * a->city->pitch * 0.5f;
    for (int i = 0; i < 8; ++i)
      out.extend(w->second.transform_point({i & 1 ? half : -half, i & 2 ? half : -half, i & 4 ? a->city->max_height : 0.0f}));
  }
  if (!out.valid()) {
    // Actors without geometry (lights, empties): a small box at the origin.
    const Vec3 p = w->second.translation();
    out.extend(p - Vec3{0.25f, 0.25f, 0.25f});
    out.extend(p + Vec3{0.25f, 0.25f, 0.25f});
  }
  return true;
}

void GameWorld::actor_meshes(ActorId id, std::vector<u32>& out) const {
  if (auto it = visuals_.find(id); it != visuals_.end() && it->second->mesh) out.push_back(it->second->mesh);
  if (auto it = cities_.find(id); it != cities_.end()) out.insert(out.end(), it->second->chunk_meshes.begin(), it->second->chunk_meshes.end());
}

// ---------------------------------------------------------------------------
// lighting

f32 GameWorld::apply_lighting(Vec3 eye) {
  const WorldSettings& w = level_.world;
  const f32 angle = (time_of_day_ - 0.25f) * 2 * kPi;  // 0.25 sunrise, 0.5 noon, 0.75 sunset
  const f32 az = w.sun_azimuth * kDeg;
  // Sun path: rises in the east of the azimuth, highest at noon.
  const Vec3 across{std::cos(az + kPi * 0.5f), std::sin(az + kPi * 0.5f), 0};
  const Vec3 toward{std::cos(az), std::sin(az), 0};
  const Vec3 sun = normalize(across * (-std::cos(angle) * 0.62f) + toward * 0.38f + Vec3{0, 0, std::sin(angle)});
  const Vec3 moon = normalize(Vec3{-sun.x, -sun.y, max_of(0.35f, -sun.z)});
  const f32 night = smoothstep(0.06f, -0.16f, sun.z);
  const f32 dusk = clamp(1 - std::fabs(sun.z) * 4, 0.0f, 1.0f) * (1 - night * 0.6f);
  Lighting& l = r_.lighting;
  const Vec3 day_sun{2.8f, 2.47f, 2.11f}, dusk_sun{2.3f, 1.25f, 0.62f}, moon_light{0.16f, 0.2f, 0.32f};
  l.sun_direction = sun.z > -0.04f ? sun : moon;
  l.sun_color = mix3(mix3(day_sun, dusk_sun, dusk), moon_light, night);
  l.sky_color = mix3({0.469f, 0.609f, 0.85f}, {0.035f, 0.05f, 0.1f}, night);
  l.ground_color = mix3({0.125f, 0.094f, 0.054f}, {0.012f, 0.012f, 0.02f}, night);
  l.fog_color = mix3(mix3({0.474f, 0.584f, 0.716f}, {0.62f, 0.45f, 0.36f}, dusk * 0.6f), {0.03f, 0.04f, 0.07f}, night);
  l.fog_density = w.fog_density;
  l.env_intensity = 0.9f - 0.78f * night;
  l.env_zenith = mix3({0.05f, 0.2f, 0.62f}, {0.012f, 0.022f, 0.06f}, night);
  l.env_horizon = mix3({0.52f, 0.66f, 0.86f}, {0.05f, 0.075f, 0.13f}, night);
  l.exposure = w.exposure * (1.0f + 0.35f * night);
  r_.sky.sun = sun;
  r_.sky.moon = moon;
  r_.sky.night = night;
  r_.sky.clouds = w.clouds;
  if (windows_) windows_->night = night;
  // Point lights: every light component of a living, visible actor.
  for (const Actor& a : level_.actors) {
    if (!a.light || a.hidden) continue;
    if (running() && !alive_[a.id]) continue;
    const auto it = world_.find(a.id);
    if (it == world_.end()) continue;
    lights_.request(it->second.translation(), srgb_to_linear(a.light->color), a.light->intensity * (1 + 1.8f * night), a.light->range);
  }
  r_.set_point_lights(lights_.resolve(eye));
  return night;
}

// ---------------------------------------------------------------------------
// presentation of a running level

Mat4 GameWorld::animated_local(const Actor& a, f32 time) const {
  Transform t = a.transform;
  Mat4 m = transform_matrix(t);
  if (!a.rotator && !a.mover) return m;
  Vec3 p = t.position;
  if (a.mover) {
    const MoverComponent& mv = *a.mover;
    const f32 u = time / max_of(0.05f, mv.period) + mv.phase;
    f32 k = 0;
    switch (mv.mode) {
      case MoverMode::sine: k = 0.5f - 0.5f * std::cos(u * 2 * kPi); break;
      case MoverMode::ping_pong: { const f32 f = u - std::floor(u); k = f < 0.5f ? f * 2 : 2 - f * 2; break; }
      case MoverMode::loop: k = u - std::floor(u); break;
    }
    p = p + mv.offset * k;
  }
  Quat q = euler_to_quat(t.rotation);
  if (a.rotator) {
    const Vec3 rate = a.rotator->rate;
    const f32 speed = length(rate);
    if (speed > 1e-6f) q = q * Quat::axis_angle(rate / speed, speed * kDeg * time);
  }
  return Mat4::trs(p, quat_normalize(q), t.scale);
}

void GameWorld::compute_world_matrices(f32 time) {
  for (ActorId id : order_) {
    const Actor* a = level_.find(id);
    if (!a) continue;
    const auto parent = world_.find(a->parent);
    const Mat4 parent_m = a->parent && parent != world_.end() ? parent->second : Mat4{};
    if (auto b = body_of_actor_.find(id); b != body_of_actor_.end() && b->second->body.valid()) {
      const physics::Pose p = physics_.pose(b->second->body);
      Mat4 m = Mat4::trs(p.position, p.rotation, b->second->scale);
      if (id == player_ && a->controller && (a->controller->mode == ControlMode::hovercar)) {
        // Hover bob and lean (visual only).
        const f32 bob = 0.07f * std::sin(sim_time_ * 3.1f);
        const Quat lean = Quat::axis_angle({1, 0, 0}, -steer_vis_ * 0.18f) * Quat::axis_angle({0, 1, 0}, -pitch_vis_);
        m = Mat4::trs(p.position + Vec3{0, 0, bob}, quat_normalize(p.rotation * lean), b->second->scale);
      }
      world_[id] = m;
      continue;
    }
    const ActorId owner = level_.body_owner(id);
    if (owner && owner != id && !a->rotator && !a->mover) {
      // Welded into an ancestor's body: keep the start offset to the body frame.
      auto b = body_of_actor_.find(owner);
      auto ow = world_.find(owner);
      if (b != body_of_actor_.end() && ow != world_.end()) {
        world_[id] = parent_m * transform_matrix(a->transform);
        continue;
      }
    }
    world_[id] = parent_m * animated_local(*a, time);
  }
}

void GameWorld::present(f64 now_ms, Vec3 eye) {
  now_ms_ = now_ms;
  if (running()) {
    // Poses and visibility only (geometry does not change while simulating).
    for (auto& [id, v] : visuals_) {
      if (!v->mesh) continue;
      const Mat4& world = world_[id];
      if (std::memcmp(v->world.m, world.m, sizeof world.m) != 0) {
        r_.update_instances(v->mesh, Span<const f32>(world.m, 16), {});
        v->world = world;
      }
      const Actor* a = level_.find(id);
      bool visible = a && !a->hidden && alive_[id];
      for (const Actor* p = a ? level_.find(a->parent) : nullptr; p && visible; p = level_.find(p->parent)) visible = !p->hidden;
      if (visible != v->visible) { r_.set_visible(v->mesh, visible); v->visible = visible; }
    }
    // Moving traffic.
    if (traffic_mesh_ && !traffic_.empty()) {
      std::vector<f32> transforms;
      transforms.reserve(traffic_.size() * 16);
      for (const auto& t : traffic_) {
        const physics::Pose p = physics_.pose(t->body);
        const Mat4 m = Mat4::trs(p.position - p.rotation.rotate({0, 0, 0.8f}), p.rotation, {1, 1, 1});
        transforms.insert(transforms.end(), m.m, m.m + 16);
      }
      r_.update_instances(traffic_mesh_, Span<const f32>(transforms.data(), transforms.size()), {});
    }
  }
  if (particles_ready_) particles_.tick(now_ms, eye);
}

// ---------------------------------------------------------------------------
// simulation

bool GameWorld::start(const Level& level, SimMode mode) {
  if (mode == SimMode::none) return fail("choose Simulate or Play");
  if (running()) stop();
  for (const Issue& i : validate(level))
    if (i.error) return fail("%s", i.text.c_str());
  sync(level);
  level_ = level;
  edit_level_ = level;
  physics::WorldSettings ws;
  ws.gravity = level.world.gravity;
  ws.hit_event_threshold = 2;
  if (!physics_.create(ws)) return false;
  mode_ = mode;
  paused_ = false;
  accumulator_ = 0;
  sim_time_ = 0;
  time_of_day_ = level.world.time_of_day;
  broken_joints_ = 0;
  events_.clear();
  respawns_.clear();
  alive_.clear();
  for (const Actor& a : level_.actors) alive_[a.id] = true;

  // Hierarchy order: parents before children.
  order_.clear();
  std::unordered_set<ActorId> placed;
  std::vector<ActorId> stack;
  for (ActorId top : level_.children(0)) stack.push_back(top);
  std::reverse(stack.begin(), stack.end());
  while (!stack.empty()) {
    const ActorId id = stack.back();
    stack.pop_back();
    if (placed.count(id)) continue;
    placed.insert(id);
    order_.push_back(id);
    std::vector<ActorId> kids = level_.children(id);
    for (auto it = kids.rbegin(); it != kids.rend(); ++it) stack.push_back(*it);
  }
  for (const Actor& a : level_.actors)
    if (!placed.count(a.id)) order_.push_back(a.id);

  // Bodies.
  for (BodyPlan& plan : plan_bodies(level_)) {
    auto sb = std::make_unique<SimBody>();
    physics::BodySettings bs;
    bs.type = plan.motion == Motion::dynamic ? physics::BodyType::dynamic : plan.motion == Motion::kinematic ? physics::BodyType::kinematic : physics::BodyType::fixed;
    bs.position = plan.position;
    bs.rotation = plan.rotation;
    const Actor* owner = level_.find(plan.owner);
    if (owner && owner->body && !plan.implicit) {
      const BodyComponent& b = *owner->body;
      bs.linear_damping = b.linear_damping;
      bs.angular_damping = b.angular_damping;
      bs.gravity_scale = b.gravity_scale;
      bs.bullet = b.bullet;
      bs.awake = b.start_awake;
      bs.enable_sleep = b.allow_sleep;
      bs.linear_velocity = b.linear_velocity;
      bs.angular_velocity = b.angular_velocity * kDeg;
    }
    bs.user = plan.owner;
    sb->body = physics_.create_body(bs);
    if (!sb->body.valid()) { stop(); return false; }
    for (const ShapePlan& s : plan.shapes) {
      physics::ShapeSettings ss;
      ss.kind = s.kind;
      ss.center = s.center;
      ss.rotation = s.rotation;
      ss.size = s.size;
      ss.radius = s.radius;
      ss.length = s.length;
      ss.points = Span<const Vec3>(s.points.data(), s.points.size());
      ss.material.density = max_of(0.001f, s.source.density);
      ss.material.friction = s.source.friction;
      ss.material.restitution = s.source.restitution;
      ss.material.rolling_resistance = s.source.rolling_resistance;
      ss.material.sensor = s.source.sensor;
      ss.material.category_bits = s.source.category;
      ss.material.mask_bits = s.source.mask;
      ss.material.group_index = s.source.group;
      ss.material.hit_events = plan.motion == Motion::dynamic;
      ss.material.user = s.actor;
      if (!physics_.create_shape(sb->body, ss).valid()) {
        const std::string why = last_error();
        events_.push_back("Collider of \"" + (level_.find(s.actor) ? level_.find(s.actor)->name : std::string("?")) + "\" skipped: " + why);
      }
    }
    if (owner) {
      if (owner->body && owner->body->mass > 0 && plan.motion == Motion::dynamic) physics_.set_mass(sb->body, owner->body->mass);
      physics::MotionLocks locks;
      if (owner->body) for (int i = 0; i < 3; ++i) { locks.linear[i] = owner->body->lock_linear[i]; locks.angular[i] = owner->body->lock_angular[i]; }
      if (owner->controller && owner->controller->mode != ControlMode::rolling && owner->controller->mode != ControlMode::flyer) {
        locks.angular[0] = locks.angular[1] = true;  // vehicles and characters stay upright
      }
      if (owner->controller && owner->controller->mode == ControlMode::flyer) locks.angular[0] = locks.angular[1] = true;
      physics_.set_motion_locks(sb->body, locks);
      sb->scale = placement_of(level_.world_matrix(owner->id)).scale;
      sb->animated = plan.motion == Motion::kinematic && (owner->rotator || owner->mover);
      if (!sb->animated && plan.motion == Motion::kinematic) {
        for (const Actor* p = level_.find(owner->parent); p && !sb->animated; p = level_.find(p->parent)) sb->animated = p->rotator || p->mover;
      }
    }
    actor_of_body_[sb->body.id] = plan.owner;
    sb->plan = std::move(plan);
    if (sb->plan.owner && !level_.find(sb->plan.owner)->city) body_of_actor_[sb->plan.owner] = sb.get();
    bodies_.push_back(std::move(sb));
  }

  // Joints.
  for (const Actor& a : level_.actors) {
    for (const JointComponent& j : a.joints) {
      auto ab = body_of_actor_.find(a.id);
      if (ab == body_of_actor_.end()) { events_.push_back("Joint on \"" + a.name + "\" ignored: the actor has no body"); continue; }
      physics::Body bb;
      const Mat4 aw = level_.world_matrix(a.id);
      const Vec3 anchor = aw.transform_point(j.anchor);
      const Vec3 axis = normalize(matrix_rotation(aw).rotate(j.axis));
      if (j.target) {
        // The target's body: its own, an ancestor's, or its implicit static body.
        ActorId owner = level_.body_owner(j.target);
        if (!owner || !body_of_actor_.count(owner)) owner = j.target;
        auto tb = body_of_actor_.find(owner);
        if (tb == body_of_actor_.end()) { events_.push_back("Joint on \"" + a.name + "\" ignored: the target has no body"); continue; }
        bb = tb->second->body;
      } else {
        // Anchor to the world: a static body at the anchor.
        auto sb = std::make_unique<SimBody>();
        physics::BodySettings bs;
        bs.type = physics::BodyType::fixed;
        bs.position = anchor;
        sb->body = physics_.create_body(bs);
        sb->plan.owner = 0;
        bb = sb->body;
        bodies_.push_back(std::move(sb));
      }
      // Box3D measures B relative to A. The actor moves relative to its target, so
      // the target is body A (positive slider travel / hinge angle / motor speed move
      // the actor); wheel joints keep Box3D's order (A = chassis = the actor, B = wheel).
      physics::Body ba = ab->second->body;
      if (j.kind != JointKind::wheel) std::swap(ba, bb);
      physics::JointSettings js;
      js.body_a = ba;
      js.body_b = bb;
      js.collide_connected = j.collide_connected;
      const bool x_axis = j.kind == JointKind::slider;
      physics::Pose world_frame = physics::frame_from_axis(anchor, axis, x_axis);
      if (j.kind == JointKind::wheel) {
        // Box3D wheel: spins about frame Z (the joint axis), suspension along frame X
        // (the chassis' up direction, made orthogonal to the spin axis).
        Vec3 up = matrix_rotation(aw).rotate({0, 0, 1});
        up = normalize(up - axis * dot(up, axis), {0, 0, 1});
        const Vec3 y = cross(axis, up);
        const Mat4 basis = [&] {
          Mat4 m;
          m.m[0] = up.x; m.m[1] = up.y; m.m[2] = up.z;
          m.m[4] = y.x; m.m[5] = y.y; m.m[6] = y.z;
          m.m[8] = axis.x; m.m[9] = axis.y; m.m[10] = axis.z;
          return m;
        }();
        world_frame.rotation = matrix_rotation(basis);
      }
      auto local_frame = [&](physics::Body b) {
        const physics::Pose p = physics_.pose(b);
        const Quat inv{-p.rotation.x, -p.rotation.y, -p.rotation.z, p.rotation.w};
        return physics::Pose{inv.rotate(world_frame.position - p.position), quat_normalize(inv * world_frame.rotation)};
      };
      js.frame_a = local_frame(ba);
      js.frame_b = local_frame(bb);
      js.enable_spring = j.enable_spring;
      js.hertz = j.spring_hertz;
      js.damping_ratio = j.spring_damping;
      js.enable_limit = j.enable_limit;
      js.enable_motor = j.enable_motor;
      switch (j.kind) {
        case JointKind::hinge:
          js.type = physics::JointType::revolute;
          js.lower = j.lower * kDeg;
          js.upper = j.upper * kDeg;
          js.motor_speed = j.motor_speed * kDeg;
          js.max_motor_torque = j.max_motor_force;
          break;
        case JointKind::slider:
          js.type = physics::JointType::prismatic;
          js.lower = j.lower;
          js.upper = j.upper;
          js.motor_speed = j.motor_speed;
          js.max_motor_force = j.max_motor_force;
          break;
        case JointKind::ball:
          js.type = physics::JointType::spherical;
          js.upper = j.upper * kDeg;
          js.max_motor_torque = j.enable_motor ? j.max_motor_force : 0;
          js.motor_velocity = axis * (j.motor_speed * kDeg);
          break;
        case JointKind::fixed:
          js.type = physics::JointType::weld;
          if (j.enable_spring) { js.linear_hertz = js.angular_hertz = j.spring_hertz; js.linear_damping_ratio = js.angular_damping_ratio = j.spring_damping; }
          break;
        case JointKind::distance: {
          js.type = physics::JointType::distance;
          // Body A (the target) holds the rope at its origin (or at the world anchor),
          // body B (the actor) at the actor's anchor.
          const physics::Pose pa = physics_.pose(ba);
          const Vec3 target_anchor = j.target ? level_.world_matrix(j.target).translation() : anchor;
          const Quat inva{-pa.rotation.x, -pa.rotation.y, -pa.rotation.z, pa.rotation.w};
          js.frame_a = {inva.rotate(target_anchor - pa.position), {}};
          js.frame_b = {js.frame_b.position, {}};
          js.length = j.length > 0 ? j.length : max_of(0.01f, distance(anchor, target_anchor));
          js.lower = j.lower;
          js.upper = j.upper;
          js.motor_speed = j.motor_speed;
          js.max_motor_force = j.max_motor_force;
          break;
        }
        case JointKind::wheel:
          js.type = physics::JointType::wheel;
          js.lower = j.lower;
          js.upper = j.upper;
          js.motor_speed = j.motor_speed * kDeg;
          js.max_motor_torque = j.max_motor_force;
          break;
        case JointKind::motor:
          js.type = physics::JointType::motor;
          js.linear_hertz = j.enable_spring ? j.spring_hertz : 4;
          js.angular_hertz = js.linear_hertz;
          js.linear_damping_ratio = js.angular_damping_ratio = j.enable_spring ? j.spring_damping : 1;
          js.max_spring_force = j.max_motor_force;
          js.max_spring_torque = j.max_motor_force;
          js.angular_velocity = axis * (j.enable_motor ? j.motor_speed * kDeg : 0.0f);
          js.max_velocity_torque = j.enable_motor ? j.max_motor_force : 0;
          break;
        case JointKind::parallel:
          js.type = physics::JointType::parallel;
          js.hertz = j.enable_spring ? j.spring_hertz : 8;
          js.damping_ratio = j.enable_spring ? j.spring_damping : 1;
          js.max_torque = j.max_motor_force;
          break;
        case JointKind::no_collision:
          js.type = physics::JointType::filter;
          break;
      }
      auto sj = std::make_unique<SimJoint>();
      sj->actor = a.id;
      sj->target = j.target;
      sj->kind = j.kind;
      sj->a = ba;
      sj->b = bb;
      sj->frame_a = js.frame_a;
      sj->frame_b = js.frame_b;
      sj->joint = physics_.create_joint(js);
      if (!sj->joint.valid()) { events_.push_back("Joint on \"" + a.name + "\" rejected: " + last_error()); continue; }
      physics_.set_joint_thresholds(sj->joint, j.break_force, j.break_torque);
      joints_.push_back(std::move(sj));
    }
  }

  // City traffic: kinematic cars on the lanes.
  for (const Actor& a : level_.actors) {
    if (!a.city || a.hidden || a.city->traffic <= 0) continue;
    std::vector<Traffic> cars;
    plan_traffic(*a.city, cars);
    const Mat4 cm = level_.world_matrix(a.id);
    const Placement cp = placement_of(cm);
    for (Traffic& t : cars) {
      auto car = std::make_unique<Traffic>(t);
      car->city = a.id;
      car->city_matrix = cm;
      const Vec2 p = t.local();
      physics::BodySettings bs;
      bs.type = physics::BodyType::kinematic;
      bs.position = cm.transform_point({p.x, p.y, 0.8f});
      bs.rotation = quat_normalize(cp.rotation * Quat::axis_angle({0, 0, 1}, t.yaw()));
      bs.enable_sleep = false;
      car->body = physics_.create_body(bs);
      physics::ShapeSettings ss;
      ss.kind = physics::ShapeKind::box;
      ss.size = {4.0f, 1.9f, 1.6f};
      ss.material.friction = 0.4f;
      physics_.create_shape(car->body, ss);
      traffic_.push_back(std::move(car));
    }
  }

  // The possessed player.
  player_ = 0;
  if (mode == SimMode::play)
    for (const Actor& a : level_.actors)
      if (a.controller && a.body && a.body->motion == Motion::dynamic && body_of_actor_.count(a.id)) { player_ = a.id; break; }
  if (player_) {
    const physics::Pose p = physics_.pose(body_of_actor_[player_]->body);
    spawn_position_ = p.position;
    spawn_rotation_ = p.rotation;
    camera_yaw_ = yaw_of(p.rotation) / kDeg;
  }
  camera_reset_ = true;
  camera_pitch_ = 18;
  camera_zoom_ = 1;
  boost_ = 1;
  shake_ = 0;
  steer_vis_ = pitch_vis_ = 0;
  jump_latch_ = false;

  // Rules.
  const f32 best = hud_.best;
  hud_ = Hud{};
  hud_.best = best;
  hud_.title = level_.world.title.empty() ? level_.name : level_.world.title;
  for (const Actor& a : level_.actors) if (a.pickup) ++hud_.pickups_total;
  hud_.status = mode == SimMode::play ? GameStatus::playing : GameStatus::inactive;
  if (level_.world.time_limit > 0) hud_.remaining = level_.world.time_limit;
  if (player_ && level_.find(player_)->controller->boost) hud_.boost = 1;
  if (mode == SimMode::play) say(hud_.title);

  world_.clear();
  for (const Actor& a : level_.actors) world_[a.id] = level_.world_matrix(a.id);
  compute_world_matrices(0);
  return true;
}

void GameWorld::stop() {
  if (!running()) return;
  grab_.end();
  joints_.clear();
  traffic_.clear();
  bodies_.clear();
  body_of_actor_.clear();
  actor_of_body_.clear();
  physics_.destroy();
  mode_ = SimMode::none;
  player_ = 0;
  paused_ = false;
  if (particles_ready_) particles_.clear();
  hud_.status = GameStatus::inactive;
  sync(edit_level_);
}

bool GameWorld::set_mode(SimMode mode) {
  if (!running() || mode == SimMode::none) return false;
  if (mode == mode_) return true;
  if (mode == SimMode::play) {
    for (const Actor& a : level_.actors)
      if (a.controller && body_of_actor_.count(a.id) && physics_.body_type(body_of_actor_[a.id]->body) == physics::BodyType::dynamic) { player_ = a.id; break; }
    if (!player_) return fail("no actor with a player controller to possess");
    hud_.status = GameStatus::playing;
    camera_reset_ = true;
  } else {
    player_ = 0;
  }
  mode_ = mode;
  return true;
}

bool GameWorld::restart() {
  if (!running()) return false;
  const Level level = edit_level_;
  const SimMode mode = mode_;
  stop();
  return start(level, mode);
}

void GameWorld::step(f32 dt, const PlayInput& input) {
  last_input_ = input;
  if (!running() || paused_) return;
  const f64 h = 1.0 / static_cast<f64>(clamp(level_.world.step_hz, 15.0f, 480.0f));
  accumulator_ += clamp(dt, 0.0f, 0.1f);
  u32 steps = 0;
  while (accumulator_ >= h && steps < 8) {
    fixed_step(input, static_cast<f32>(h));
    accumulator_ -= h;
    ++steps;
  }
  if (steps == 8) accumulator_ = 0;
  if (level_.world.day_length > 0) {
    time_of_day_ += dt / level_.world.day_length;
    time_of_day_ -= std::floor(time_of_day_);
  }
  compute_world_matrices(sim_time_);
}

void GameWorld::step_once(const PlayInput& input) {
  if (!running()) return;
  fixed_step(input, 1.0f / clamp(level_.world.step_hz, 15.0f, 480.0f));
  compute_world_matrices(sim_time_);
}

void GameWorld::fixed_step(const PlayInput& input, f32 h) {
  if (player_ && hud_.status == GameStatus::playing) drive_player(input, h);
  // Kinematic animation targets.
  compute_world_matrices(sim_time_ + h);
  for (const auto& b : bodies_) {
    if (!b->animated || !b->body.valid()) continue;
    const Mat4& m = world_[b->plan.owner];
    // world_ for animated bodies came from the body; recompute from the parent chain.
    const Actor* a = level_.find(b->plan.owner);
    if (!a) continue;
    Mat4 parent;
    if (a->parent) parent = world_[a->parent];
    const Placement p = placement_of(parent * animated_local(*a, sim_time_ + h));
    physics_.set_target_pose(b->body, physics::Pose{p.position, p.rotation}, h);
    (void)m;
  }
  drive_traffic(h);
  grab_.update(h);
  physics_.step(h, clamp(level_.world.substeps, 1, 16));
  sim_time_ += h;
  if (hud_.status == GameStatus::playing) {
    hud_.elapsed += h;
    if (level_.world.time_limit > 0) hud_.remaining = max_of(0.0f, level_.world.time_limit - hud_.elapsed);
  }
  handle_events();
  // Respawning pickups.
  for (std::size_t i = 0; i < respawns_.size();) {
    if (sim_time_ >= respawns_[i].at) {
      set_actor_alive(respawns_[i].id, true);
      respawns_.erase(respawns_.begin() + static_cast<std::ptrdiff_t>(i));
    } else ++i;
  }
  if (hud_.status == GameStatus::playing) {
    hud_.message_age += h;
    const WorldSettings& w = level_.world;
    if (w.time_limit > 0) {
      if (hud_.remaining <= 0) {
        if (w.goal == GameGoal::survive) finish(GameStatus::won, "Survived!");
        else finish(GameStatus::lost, "Time is up");
      }
    }
    if (player_) {
      const physics::Body pb = body_of_actor_[player_]->body;
      if (physics_.pose(pb).position.z < w.kill_z) respawn_player();
      hud_.speed = length(physics_.linear_velocity(pb));
    }
    if (w.goal == GameGoal::score && w.score_target > 0 && hud_.score >= w.score_target) finish(GameStatus::won, "Target score reached!");
  } else {
    hud_.message_age += h;
  }
}

bool GameWorld::grounded(physics::Body body, f32 reach) const {
  const physics::Pose p = physics_.pose(body);
  return physics_.cast_ray(p.position, Vec3{0, 0, -reach}, ~0ull, body).hit;
}

void GameWorld::drive_player(const PlayInput& in, f32 dt) {
  const Actor* a = level_.find(player_);
  auto it = body_of_actor_.find(player_);
  if (!a || !a->controller || it == body_of_actor_.end()) return;
  const ControllerComponent& c = *a->controller;
  const physics::Body body = it->second->body;
  const physics::Pose pose = physics_.pose(body);
  const f32 m = max_of(0.001f, physics_.mass(body));
  const Vec3 v = physics_.linear_velocity(body);
  const Vec3 g = physics_.gravity() * (a->body ? a->body->gravity_scale : 1.0f);
  const bool jump_pressed = in.jump && !jump_latch_;
  jump_latch_ = in.jump;
  // Reach of the body below its origin (for ground probes).
  f32 reach = 0.5f;
  for (const ShapePlan& s : it->second->plan.shapes) {
    if (s.source.sensor) continue;
    switch (s.kind) {
      case physics::ShapeKind::box: reach = max_of(reach, -s.center.z + s.size.z * 0.5f); break;
      case physics::ShapeKind::sphere: reach = max_of(reach, -s.center.z + s.radius); break;
      case physics::ShapeKind::capsule: reach = max_of(reach, -s.center.z + s.radius + s.length * 0.5f); break;
      case physics::ShapeKind::hull: for (const Vec3& p : s.points) reach = max_of(reach, -p.z); break;
    }
  }
  const f32 turn = c.turn_rate * kDeg;
  // Camera-relative directions (rolling, character, flyer).
  const Vec3 cam_fwd{std::cos(camera_yaw_ * kDeg), std::sin(camera_yaw_ * kDeg), 0};
  const Vec3 cam_right{cam_fwd.y, -cam_fwd.x, 0};
  Vec3 move = cam_right * clamp(in.move_x, -1.0f, 1.0f) + cam_fwd * clamp(in.move_y, -1.0f, 1.0f);
  if (length(move) > 1) move = normalize(move);
  bool boosting = c.boost && in.boost && boost_ > 0.02f;
  boost_ = c.boost ? clamp(boost_ + (boosting ? -0.45f : 0.12f) * dt, 0.0f, 1.0f) : 0.0f;
  if (c.boost) hud_.boost = boost_;

  switch (c.mode) {
    case ControlMode::hovercar:
    case ControlMode::vehicle: {
      Vec3 forward = pose.rotation.rotate({1, 0, 0});
      forward.z = 0;
      forward = normalize(forward, {1, 0, 0});
      const Vec3 side{-forward.y, forward.x, 0};
      const f32 throttle = clamp(in.move_y, -0.7f, 1.0f), steer = clamp(-in.move_x, -1.0f, 1.0f);
      const f32 fwd = dot(v, forward), lat = dot(v, side);
      const bool hover = c.mode == ControlMode::hovercar;
      const bool on_ground = hover || grounded(body, reach + 0.25f);
      if (on_ground) {
        f32 accel = throttle * (throttle > 0 ? c.acceleration : c.acceleration * 1.25f);
        if (boosting) accel += 42;
        const f32 cap = boosting ? c.boost_speed : c.max_speed;
        if (fwd > cap) accel = min_of(accel, -(fwd - cap) * 4);
        if (fwd < -cap * 0.33f) accel = max_of(accel, (-cap * 0.33f - fwd) * 4);
        if (throttle == 0 && !boosting) accel -= fwd * 0.6f;  // rolling drag
        physics_.apply_force_to_center(body, forward * (accel * m));
        // Lateral grip.
        physics_.apply_linear_impulse_to_center(body, side * (-lat * (1 - std::exp(-7.5f * dt)) * m));
        const f32 grip = clamp(std::fabs(fwd) / 9.0f, 0.0f, 1.0f) * (fwd >= 0 ? 1.0f : -1.0f);
        const Vec3 w = physics_.angular_velocity(body);
        physics_.set_angular_velocity(body, {w.x, w.y, steer * turn * grip});
        steer_vis_ = damp(steer_vis_, steer * grip, 6, dt);
        // Nose dips when braking, lifts when accelerating (per 60 Hz step like Orb Run).
        const f32 accel_pitch = clamp((fwd - last_forward_) * 0.02f * (1.0f / (60.0f * dt)), -0.12f, 0.12f);
        pitch_vis_ = damp(pitch_vis_, accel_pitch, 5, dt);
        last_forward_ = fwd;
      }
      if (hover) {
        // Hover spring over whatever is below (Box3D ray cast).
        const f32 probe = c.hover_height * 4 + reach;
        const physics::RayHit hit = physics_.cast_ray(pose.position, Vec3{0, 0, -probe}, ~0ull, body);
        if (hit.hit) {
          const f32 dist = hit.fraction * probe - reach;
          const f32 accel = (c.hover_height - dist) * 60 - v.z * 9 - g.z;
          physics_.apply_force_to_center(body, Vec3{0, 0, clamp(accel, -40.0f, 80.0f) * m});
        }
      }
      break;
    }
    case ControlMode::rolling: {
      physics_.apply_force_to_center(body, move * (c.acceleration * m));
      if (jump_pressed && c.jump_speed > 0 && grounded(body, reach + 0.15f)) physics_.set_linear_velocity(body, {v.x, v.y, c.jump_speed});
      const f32 hs = std::sqrt(v.x * v.x + v.y * v.y);
      if (hs > c.max_speed) physics_.apply_force_to_center(body, Vec3{v.x, v.y, 0} * (-(hs - c.max_speed) / hs * 4 * m));
      break;
    }
    case ControlMode::character: {
      const f32 speed = boosting ? c.boost_speed : c.max_speed;
      const Vec3 target = move * speed;
      const bool on_ground = grounded(body, reach + 0.12f);
      const f32 control = on_ground ? 10.0f : 2.0f;
      const Vec3 dv{target.x - v.x, target.y - v.y, 0};
      physics_.apply_linear_impulse_to_center(body, dv * (m * min_of(1.0f, control * dt)));
      if (jump_pressed && c.jump_speed > 0 && on_ground) physics_.set_linear_velocity(body, {v.x, v.y, c.jump_speed});
      // Face the walking direction.
      if (length(move) > 0.1f) {
        const f32 yaw = yaw_of(pose.rotation), want = std::atan2(move.y, move.x);
        f32 d = want - yaw;
        while (d > kPi) d -= 2 * kPi;
        while (d < -kPi) d += 2 * kPi;
        physics_.set_angular_velocity(body, {0, 0, clamp(d * 10, -turn, turn)});
      } else {
        physics_.set_angular_velocity(body, {0, 0, 0});
      }
      break;
    }
    case ControlMode::flyer: {
      const f32 speed = boosting ? c.boost_speed : c.max_speed;
      const Vec3 target = move * speed + Vec3{0, 0, clamp(in.up, -1.0f, 1.0f) * speed * 0.6f};
      const Vec3 accel = (target - v) * clamp(c.acceleration / max_of(1.0f, speed), 0.5f, 20.0f) - g;
      physics_.apply_force_to_center(body, accel * m);
      const f32 yaw = yaw_of(pose.rotation);
      f32 d = camera_yaw_ * kDeg - yaw;
      while (d > kPi) d -= 2 * kPi;
      while (d < -kPi) d += 2 * kPi;
      physics_.set_angular_velocity(body, {0, 0, clamp(d * 6, -turn, turn)});
      break;
    }
  }
}

void GameWorld::drive_traffic(f32 dt) {
  if (traffic_.empty()) return;
  Vec3 player_pos{1e9f, 1e9f, 1e9f};
  if (player_) player_pos = physics_.pose(body_of_actor_[player_]->body).position;
  for (auto& t : traffic_) {
    const Vec2 p = t->local();
    const Vec2 ahead = t->horizontal ? Vec2{t->dir, 0} : Vec2{0, t->dir};
    const Mat4 inv = inverse(t->city_matrix);
    const Vec3 lp = inv.transform_point(player_pos);
    const Vec2 rel{lp.x - p.x, lp.y - p.y};
    const bool blocked = dot(rel, ahead) > 0 && dot(rel, ahead) < 10 && std::fabs(cross(ahead, rel)) < 2.5f;
    t->along += t->dir * (blocked ? 0.0f : t->speed) * dt;
    bool wrapped = false;
    if (t->along > t->span) { t->along = -t->span; wrapped = true; }
    if (t->along < -t->span) { t->along = t->span; wrapped = true; }
    const Vec2 q = t->local();
    const Placement cp = placement_of(t->city_matrix);
    const physics::Pose target{t->city_matrix.transform_point({q.x, q.y, 0.8f}), quat_normalize(cp.rotation * Quat::axis_angle({0, 0, 1}, t->yaw()))};
    if (wrapped) physics_.set_pose(t->body, target);
    else physics_.set_target_pose(t->body, target, dt);
  }
}

ActorId GameWorld::actor_for_body(physics::Body body) const {
  const auto it = actor_of_body_.find(body.id);
  return it == actor_of_body_.end() ? 0 : it->second;
}

void GameWorld::handle_events() {
  // The player touched `shape` of `body`: the actor is the shape's contributor
  // (a welded child) or the body's owner.
  auto touched = [&](physics::Shape shape, physics::Body body) {
    ActorId target = static_cast<ActorId>(physics_.shape_user(shape));
    if (!target) target = actor_for_body(body);
    const Actor* a = level_.find(target);
    if (!a || !alive_[target]) return;
    if (a->pickup) collect(target);
    else if (a->trigger) trigger(target);
  };
  if (player_) {
    const physics::Body pb = body_of_actor_[player_]->body;
    for (const physics::TouchEvent& e : physics_.sensor_begin())
      if (e.body_b == pb) touched(e.shape_a, e.body_a);  // a = sensor, b = visitor
    for (const physics::TouchEvent& e : physics_.contact_begin()) {
      if (e.body_a == pb) touched(e.shape_b, e.body_b);
      else if (e.body_b == pb) touched(e.shape_a, e.body_a);
    }
  }
  // Camera shake from hard player hits.
  for (const physics::HitEvent& h : physics_.hits()) {
    if (!player_) break;
    const physics::Body pb = body_of_actor_[player_]->body;
    if (h.body_a == pb || h.body_b == pb) shake_ = max_of(shake_, clamp(h.approach_speed / 30.0f, 0.15f, 0.8f));
  }
  // Breaking joints.
  for (const physics::Joint& j : physics_.joint_events()) {
    for (auto& sj : joints_) {
      if (sj->broken || !(sj->joint == j)) continue;
      physics_.destroy_joint(sj->joint, true);
      sj->broken = true;
      ++broken_joints_;
      const Actor* a = level_.find(sj->actor);
      events_.push_back(std::string("Joint on \"") + (a ? a->name : std::string("?")) + "\" broke");
    }
  }
}

void GameWorld::set_actor_alive(ActorId id, bool alive) {
  // The actor and its subtree.
  std::vector<ActorId> stack{id};
  while (!stack.empty()) {
    const ActorId x = stack.back();
    stack.pop_back();
    alive_[x] = alive;
    for (ActorId k : level_.children(x)) stack.push_back(k);
  }
  auto it = body_of_actor_.find(id);
  if (it != body_of_actor_.end()) {
    SimBody& b = *it->second;
    if (!alive && b.body.valid()) {
      actor_of_body_.erase(b.body.id);
      physics_.destroy_body(b.body);
      b.body = {};
    } else if (alive && !b.body.valid()) {
      physics::BodySettings bs;
      bs.type = b.plan.motion == Motion::dynamic ? physics::BodyType::dynamic : b.plan.motion == Motion::kinematic ? physics::BodyType::kinematic : physics::BodyType::fixed;
      bs.position = b.plan.position;
      bs.rotation = b.plan.rotation;
      bs.user = b.plan.owner;
      b.body = physics_.create_body(bs);
      for (const ShapePlan& s : b.plan.shapes) {
        physics::ShapeSettings ss;
        ss.kind = s.kind;
        ss.center = s.center;
        ss.rotation = s.rotation;
        ss.size = s.size;
        ss.radius = s.radius;
        ss.length = s.length;
        ss.points = Span<const Vec3>(s.points.data(), s.points.size());
        ss.material.sensor = s.source.sensor;
        ss.material.density = max_of(0.001f, s.source.density);
        ss.material.friction = s.source.friction;
        ss.material.user = s.actor;
        physics_.create_shape(b.body, ss);
      }
      actor_of_body_[b.body.id] = b.plan.owner;
    }
  }
}

void GameWorld::burst(Vec3 at, Vec3 color) {
  if (!particles_ready_) return;
  Rng rng(static_cast<u32>(sim_time_ * 1000) + 77);
  for (u32 k = 0; k < 28; ++k) {
    const f32 a = rng.range(0, 2 * kPi), up = rng.range(4, 11), out = rng.range(3, 9);
    ParticleSpec spark;
    spark.position = at;
    spark.velocity = {std::cos(a) * out, std::sin(a) * out, up};
    spark.color = {color.x, color.y, color.z, 1};
    spark.start_ms = now_ms_;
    spark.life_ms = rng.range(600, 1100);
    spark.size = rng.range(0.25f, 0.5f);
    spark.gravity = 14;
    spark.fade_extra = 0.3f;
    particles_.emit(spark);
  }
}

void GameWorld::collect(ActorId id) {
  const Actor* a = level_.find(id);
  if (!a || !a->pickup || !alive_[id] || hud_.status != GameStatus::playing) return;
  const Vec3 at = world_[id].translation();
  set_actor_alive(id, false);
  hud_.score += a->pickup->points;
  ++hud_.pickups;
  burst(at, srgb_to_linear(a->pickup->burst_color) * 1.6f);
  if (a->pickup->respawn > 0) respawns_.push_back({id, sim_time_ + a->pickup->respawn});
  else if (level_.world.goal == GameGoal::collect_all && hud_.pickups >= hud_.pickups_total) {
    bool has_goal = false;
    for (const Actor& x : level_.actors) has_goal |= x.trigger && x.trigger->action == TriggerAction::goal;
    if (has_goal) say("All collected - reach the goal!");
    else finish(GameStatus::won, "All collected!");
  }
}

void GameWorld::trigger(ActorId id) {
  const Actor* a = level_.find(id);
  if (!a || !a->trigger || hud_.status != GameStatus::playing) return;
  const TriggerComponent& t = *a->trigger;
  switch (t.action) {
    case TriggerAction::goal:
      if (level_.world.goal == GameGoal::collect_all && hud_.pickups < hud_.pickups_total) { say("Collect everything first"); return; }
      finish(GameStatus::won, t.message.empty() ? "Goal reached!" : t.message);
      break;
    case TriggerAction::hazard:
      finish(GameStatus::lost, t.message.empty() ? "Hit a hazard" : t.message);
      break;
    case TriggerAction::checkpoint:
      spawn_position_ = world_[id].translation() + Vec3{0, 0, 1};
      say(t.message.empty() ? "Checkpoint" : t.message);
      break;
    case TriggerAction::message:
      say(t.message);
      break;
  }
  if (t.once && t.action != TriggerAction::goal && t.action != TriggerAction::hazard) set_actor_alive(id, false);
}

void GameWorld::finish(GameStatus status, std::string message) {
  if (hud_.status != GameStatus::playing) return;
  hud_.status = status;
  say(std::move(message));
  if (status == GameStatus::won && (level_.world.goal == GameGoal::collect_all || level_.world.goal == GameGoal::reach_goal))
    if (hud_.best <= 0 || hud_.elapsed < hud_.best) hud_.best = hud_.elapsed;
  events_.push_back(status == GameStatus::won ? "Game won: " + hud_.message : "Game lost: " + hud_.message);
}

void GameWorld::say(std::string message) {
  hud_.message = std::move(message);
  hud_.message_age = 0;
}

void GameWorld::respawn_player() {
  auto it = body_of_actor_.find(player_);
  if (it == body_of_actor_.end()) return;
  physics_.set_pose(it->second->body, {spawn_position_, spawn_rotation_});
  physics_.set_linear_velocity(it->second->body, {});
  physics_.set_angular_velocity(it->second->body, {});
  camera_reset_ = true;
  say("Respawned");
}

bool GameWorld::update_camera(Camera& cam, f32 dt, const PlayInput& in) {
  if (!player_) return false;
  const Actor* a = level_.find(player_);
  auto it = body_of_actor_.find(player_);
  if (!a || !a->controller || it == body_of_actor_.end()) return false;
  const ControllerComponent& c = *a->controller;
  const physics::Body body = it->second->body;
  const physics::Pose pose = physics_.pose(body);
  const Vec3 v = physics_.linear_velocity(body);
  const f32 speed = length(v);
  camera_yaw_ -= in.look_x;
  camera_pitch_ = clamp(camera_pitch_ + in.look_y, -80.0f, 85.0f);
  camera_zoom_ = clamp(camera_zoom_ * (1 + in.zoom * 0.0012f), 0.35f, 4.0f);
  const Vec3 center = pose.position;
  cam.up = {0, 0, 1};
  cam.orthographic = false;
  cam.far = 4000;
  Vec3 eye, target;
  switch (c.camera) {
    case CameraMode::first_person: {
      if (c.mode == ControlMode::hovercar || c.mode == ControlMode::vehicle) camera_yaw_ = yaw_of(pose.rotation) / kDeg + 0;
      const Vec3 dir{std::cos(camera_yaw_ * kDeg) * std::cos(-camera_pitch_ * kDeg * 0.5f), std::sin(camera_yaw_ * kDeg) * std::cos(-camera_pitch_ * kDeg * 0.5f),
                     std::sin(-camera_pitch_ * kDeg * 0.5f)};
      eye = center + Vec3{0, 0, c.camera_height * 0.25f};
      target = eye + dir * 10;
      camera_eye_ = eye;
      break;
    }
    case CameraMode::top_down: {
      target = center;
      eye = center + Vec3{0, -c.camera_distance * 0.35f * camera_zoom_, c.camera_distance * 1.6f * camera_zoom_};
      camera_eye_ = camera_reset_ ? eye : damp3(camera_eye_, eye, 6, dt);
      eye = camera_eye_;
      break;
    }
    case CameraMode::chase: {
      if (c.mode == ControlMode::hovercar || c.mode == ControlMode::vehicle) {
        Vec3 forward = pose.rotation.rotate({1, 0, 0});
        forward.z = 0;
        forward = normalize(forward, {1, 0, 0});
        const f32 heading = std::atan2(forward.y, forward.x);
        const f32 dist = c.camera_distance * camera_zoom_ + speed * 0.12f;
        const Vec3 desired = center - forward * dist + Vec3{0, 0, c.camera_height * 0.8f + dist * 0.28f};
        camera_eye_ = camera_reset_ ? desired : damp3(camera_eye_, desired, 4.5f, dt);
        eye = camera_eye_;
        target = center + forward * 5 + Vec3{0, 0, 1.2f};
        camera_yaw_ = heading / kDeg;
      } else {
        const f32 dist = c.camera_distance * camera_zoom_;
        const Vec3 dir{std::cos(camera_yaw_ * kDeg) * std::cos(camera_pitch_ * kDeg), std::sin(camera_yaw_ * kDeg) * std::cos(camera_pitch_ * kDeg),
                       std::sin(camera_pitch_ * kDeg)};
        target = center + Vec3{0, 0, c.camera_height * 0.25f};
        Vec3 desired = target - dir * dist;
        // Keep the camera out of walls (Box3D ray from the target).
        const physics::RayHit hit = physics_.cast_ray(target, desired - target, ~0ull, body);
        if (hit.hit) desired = target + (desired - target) * max_of(0.05f, hit.fraction - 0.04f);
        camera_eye_ = camera_reset_ ? desired : damp3(camera_eye_, desired, 12, dt);
        eye = camera_eye_;
      }
      break;
    }
  }
  camera_reset_ = false;
  shake_ *= std::exp(-5 * dt);
  if (shake_ > 0.01f) {
    Rng rng(static_cast<u32>(sim_time_ * 997.0f) + 13);
    eye += Vec3{rng.range(-1, 1), rng.range(-1, 1), rng.range(-1, 1)} * shake_ * 0.35f;
  }
  cam.eye = eye;
  cam.target = target;
  cam.fov = (c.camera_fov + clamp(speed - 30, 0.0f, 30.0f) * 0.35f) * kDeg;
  return true;
}

SimStats GameWorld::stats() const {
  SimStats s;
  if (!running()) return s;
  s.bodies = physics_.body_count();
  s.awake = static_cast<u32>(physics_.awake_body_count());
  for (const auto& b : bodies_) s.shapes += static_cast<u32>(b->plan.shapes.size());
  for (const auto& j : joints_) s.joints += j->broken ? 0 : 1;
  s.broken_joints = broken_joints_;
  s.steps = physics_.step_count();
  return s;
}

physics::Body GameWorld::actor_body(ActorId actor) const {
  ActorId owner = level_.body_owner(actor);
  if (!owner || !body_of_actor_.count(owner)) owner = actor;
  const auto it = body_of_actor_.find(owner);
  return it == body_of_actor_.end() ? physics::Body{} : it->second->body;
}

bool GameWorld::begin_grab(ActorId actor, Vec3 point) {
  if (!running()) return false;
  const ActorId owner = level_.body_owner(actor) ? level_.body_owner(actor) : actor;
  auto it = body_of_actor_.find(owner);
  if (it == body_of_actor_.end() || !it->second->body.valid()) return false;
  return grab_.begin(physics_, it->second->body, point);
}
void GameWorld::move_grab(Vec3 target) { grab_.set_target(target); }
void GameWorld::end_grab() { grab_.end(); }

void GameWorld::debug_lines(bool colliders, bool contacts, bool joints, std::vector<f32>& p, std::vector<f32>& c) const {
  const Vec3 kStatic{0.45f, 0.85f, 0.45f}, kKinematic{0.45f, 0.65f, 1.0f}, kDynamic{1.0f, 0.62f, 0.25f}, kSensor{0.85f, 0.4f, 1.0f}, kSleep{0.55f, 0.55f, 0.55f};
  std::vector<Vec3> local;
  if (colliders) {
    auto draw_plan = [&](const BodyPlan& plan, Vec3 pos, Quat rot, bool awake) {
      for (const ShapePlan& s : plan.shapes) {
        if (s.actor && level_.find(s.actor) && level_.find(s.actor)->city && !running()) continue;  // thousands of boxes: too noisy while editing
        local.clear();
        shape_lines(s, local);
        const Vec3 color = s.source.sensor ? kSensor : !awake ? kSleep : plan.motion == Motion::dynamic ? kDynamic : plan.motion == Motion::kinematic ? kKinematic : kStatic;
        for (std::size_t i = 0; i + 1 < local.size(); i += 2) push_line(p, c, pos + rot.rotate(local[i]), pos + rot.rotate(local[i + 1]), color);
      }
    };
    if (running()) {
      for (const auto& b : bodies_) {
        if (!b->body.valid() || !b->plan.owner) continue;
        const physics::Pose pose = physics_.pose(b->body);
        draw_plan(b->plan, pose.position, pose.rotation, b->plan.motion != Motion::dynamic || physics_.awake(b->body));
      }
      for (const auto& t : traffic_) {
        const physics::Pose pose = physics_.pose(t->body);
        ShapePlan s;
        s.kind = physics::ShapeKind::box;
        s.size = {4.0f, 1.9f, 1.6f};
        local.clear();
        shape_lines(s, local);
        for (std::size_t i = 0; i + 1 < local.size(); i += 2) push_line(p, c, pose.position + pose.rotation.rotate(local[i]), pose.position + pose.rotation.rotate(local[i + 1]), kKinematic);
      }
    } else {
      for (const BodyPlan& plan : plan_bodies(level_)) {
        if (!plan.owner) continue;
        draw_plan(plan, plan.position, plan.rotation, true);
      }
    }
  }
  if (contacts && running()) {
    std::vector<physics::ContactPoint> points;
    for (const auto& b : bodies_)
      if (b->body.valid() && b->plan.motion == Motion::dynamic) physics_.contact_points(b->body, points);
    for (const physics::ContactPoint& cp : points) {
      push_line(p, c, cp.point, cp.point + cp.normal * 0.4f, {1, 0.2f, 0.2f});
      push_line(p, c, cp.point - Vec3{0.06f, 0, 0}, cp.point + Vec3{0.06f, 0, 0}, {1, 1, 0.2f});
      push_line(p, c, cp.point - Vec3{0, 0.06f, 0}, cp.point + Vec3{0, 0.06f, 0}, {1, 1, 0.2f});
    }
  }
  if (joints) {
    const Vec3 kJoint{0.3f, 0.9f, 1.0f}, kAxis{1.0f, 0.85f, 0.2f};
    if (running()) {
      for (const auto& j : joints_) {
        if (j->broken) continue;
        const physics::Pose pa = physics_.pose(j->a), pb = physics_.pose(j->b);
        const Vec3 wa = pa.position + pa.rotation.rotate(j->frame_a.position), wb = pb.position + pb.rotation.rotate(j->frame_b.position);
        push_line(p, c, pa.position, wa, kJoint);
        push_line(p, c, wa, wb, kJoint);
        push_line(p, c, pb.position, wb, kJoint);
        const bool x_axis = j->kind == JointKind::slider;
        const Vec3 axis = (pa.rotation * j->frame_a.rotation).rotate(x_axis ? Vec3{1, 0, 0} : Vec3{0, 0, 1});
        push_line(p, c, wa - axis * 0.5f, wa + axis * 0.5f, kAxis);
      }
    } else {
      for (const Actor& a : level_.actors)
        for (const JointComponent& j : a.joints) {
          const Mat4 aw = level_.world_matrix(a.id);
          const Vec3 anchor = aw.transform_point(j.anchor);
          const Vec3 axis = normalize(matrix_rotation(aw).rotate(j.axis));
          push_line(p, c, aw.translation(), anchor, kJoint);
          if (j.target && level_.find(j.target)) push_line(p, c, anchor, level_.world_matrix(j.target).translation(), kJoint);
          push_line(p, c, anchor - axis * 0.6f, anchor + axis * 0.6f, kAxis);
        }
    }
  }
}

}  // namespace kern::game
