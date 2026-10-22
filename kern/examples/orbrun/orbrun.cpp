// ORB RUN - a complete small game written only against the Kern engine API.
//
// Race a hover car through a procedurally generated city and collect every
// energy orb as fast as possible while the day turns into night. Shows the
// engine's building/road builders (with courtyard holes), instancing,
// shadows, sky, bloom, point lights, picking-free arcade collision, a custom
// shader extension (lit night windows from the builder's wall records),
// input (keyboard + touch) and the status channel.
//
// Controls: W/S or arrows drive, A/D steer, Space boost, N fast time,
//           Enter new city. Touch: left half steers/accelerates, right half boosts.
#include <algorithm>
#include <cstdio>
#include <vector>

#include "kern/kern.h"

using namespace kern;

namespace {

// ---------------------------------------------------------------------------
// utilities

struct Rng {
  u32 s;
  explicit Rng(u32 seed) : s(seed ? seed : 0x9e3779b9u) {}
  u32 next() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
  f32 unit() { return static_cast<f32>(next() >> 8) * (1.0f / 16777216.0f); }
  f32 range(f32 a, f32 b) { return a + (b - a) * unit(); }
  u32 below(u32 n) { return next() % n; }
};

f32 smoothstep(f32 a, f32 b, f32 x) {
  const f32 t = clamp((x - a) / (b - a), 0.0f, 1.0f);
  return t * t * (3 - 2 * t);
}
Vec3 mix3(Vec3 a, Vec3 b, f32 t) { return a + (b - a) * t; }
f32 damp(f32 current, f32 target, f32 rate, f32 dt) { return target + (current - target) * std::exp(-rate * dt); }
Vec3 damp3(Vec3 current, Vec3 target, f32 rate, f32 dt) { return target + (current - target) * std::exp(-rate * dt); }

void put_matrix(std::vector<f32>& out, const Mat4& m) { out.insert(out.end(), m.m, m.m + 16); }

// ---------------------------------------------------------------------------
// Lit night windows: a shader extension driven by the building builder's
// per-vertex wall records (wall length, height, seed, storey height).

class NightWindows final : public ShaderExtension {
 public:
  f32 night = 0;
  const char* name() const override { return "orbrun-night-windows"; }
  Span<const ExtensionAttribute> attributes() const override { return {kAttributes, 1}; }
  const char* vertex_declarations() const override { return "flat out vec4 vWall;\n"; }
  const char* vertex() const override { return "  vWall = aWall;\n"; }
  const char* fragment_declarations() const override {
    return R"GLSL(
// flat: the record is constant per wall; interpolation noise would scramble the window hash.
flat in vec4 vWall;
uniform float uNight;
float windowHash(float n) { return fract(sin(n * 91.7) * 43758.5453); }
)GLSL";
  }
  const char* fragment_surface() const override {
    return R"GLSL(
  if (vWall.x > 0.0 && abs(n.z) < 0.5) {
    float u = vUV.x, z = vUV.y, storey = vWall.w;
    float column = floor(u / 3.0), floorIndex = floor(z / storey);
    vec2 cell = vec2(fract(u / 3.0), fract(z / storey));
    // Fade the grid out where it would alias; distant facades keep an average glow instead.
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
  void bind(const Program& p) override { u_night_ = p.location("uNight"); }
  void frame_uniforms(UniformCache& cache, const Renderer&) override { cache.set_float(u_night_, night); }

 private:
  static constexpr ExtensionAttribute kAttributes[] = {{"wall", 4, "aWall"}};
  u32 u_night_ = 0;
};

// ---------------------------------------------------------------------------
// City layout: a grid of blocks with varied footprints, streets between them.

struct Footprint {
  std::vector<Vec2> ring;
  Vec2 lo, hi;
};

class City {
 public:
  static constexpr int kBlocks = 7;
  static constexpr f32 kPitch = 80, kStreet = 16;
  static constexpr f32 kHalf = kBlocks * kPitch * 0.5f;
  static constexpr f32 kCell = 40;

  std::vector<Footprint> buildings;

  void generate(u32 seed, FootprintStream& footprints, RoadStream& roads, MeshData& plates) {
    Rng rng(seed);
    buildings.clear();
    for (int bx = 0; bx < kBlocks; ++bx)
      for (int by = 0; by < kBlocks; ++by) {
        const f32 x0 = -kHalf + bx * kPitch + kStreet * 0.5f, y0 = -kHalf + by * kPitch + kStreet * 0.5f;
        const f32 size = kPitch - kStreet;
        primitives::append(plates, primitives::box_on_ground({size, size, 0.18f}, {0.56f, 0.56f, 0.54f}),
                           Mat4::translate({x0 + size * 0.5f, y0 + size * 0.5f, 0}));
        const Vec2 center{x0 + size * 0.5f, y0 + size * 0.5f};
        const f32 centrality = 1 - clamp(length(center) / (kHalf * 1.2f), 0.0f, 1.0f);
        const f32 tall = 14 + centrality * centrality * 70;
        const f32 inset = 3;
        const f32 a0 = x0 + inset, b0 = y0 + inset, a1 = x0 + size - inset, b1 = y0 + size - inset;
        switch (rng.below(4)) {
          case 0: {  // one block-sized building, often with a courtyard
            const Vec2 outer[4] = {{a0, b0}, {a1, b0}, {a1, b1}, {a0, b1}};
            footprints.begin(rng.next(), 0, rng.range(10, tall));
            footprints.ring(outer, 4);
            if (rng.unit() < 0.6f) {
              const f32 c = 14;
              const Vec2 court[4] = {{a0 + c, b0 + c}, {a0 + c, b1 - c}, {a1 - c, b1 - c}, {a1 - c, b0 + c}};
              footprints.ring(court, 4);
            }
            add_collider(outer, 4);
            break;
          }
          case 1: {  // four lots
            const f32 mx = (a0 + a1) * 0.5f, my = (b0 + b1) * 0.5f, g = 2.5f;
            const f32 lots[4][4] = {{a0, b0, mx - g, my - g}, {mx + g, b0, a1, my - g}, {a0, my + g, mx - g, b1}, {mx + g, my + g, a1, b1}};
            for (const auto& l : lots) add_box(rng, footprints, l[0], l[1], l[2], l[3], rng.range(8, tall));
            break;
          }
          case 2: {  // L-shaped block
            const f32 w = rng.range(16, 24);
            const Vec2 outer[6] = {{a0, b0}, {a1, b0}, {a1, b0 + w}, {a0 + w, b0 + w}, {a0 + w, b1}, {a0, b1}};
            footprints.begin(rng.next(), 0, rng.range(12, tall));
            footprints.ring(outer, 6);
            add_collider(outer, 6);
            add_box(rng, footprints, a0 + w + 6, b0 + w + 6, a1, b1, rng.range(8, tall * 0.7f));
            break;
          }
          default: {  // slabs
            const f32 step = (a1 - a0) / 3;
            for (int k = 0; k < 3; ++k) add_box(rng, footprints, a0 + k * step + 1.5f, b0, a0 + (k + 1) * step - 1.5f, b1, rng.range(8, tall));
            break;
          }
        }
      }
    // Street grid: every block edge is a street centre line.
    for (int k = 0; k <= kBlocks; ++k) {
      const f32 c = -kHalf + k * kPitch;
      const Vec2 h[2] = {{-kHalf - 60, c}, {kHalf + 60, c}}, v[2] = {{c, -kHalf - 60}, {c, kHalf + 60}};
      roads.road(h, 2, 12, true, 0, 1);
      roads.road(v, 2, 12, true, 0, 1);
    }
    index_colliders();
  }

  /// A random point on a street centre line inside the city.
  Vec2 street_point(Rng& rng) const {
    const f32 c = -kHalf + static_cast<f32>(rng.below(kBlocks + 1)) * kPitch;
    const f32 along = rng.range(-kHalf + 10, kHalf - 10);
    return rng.unit() < 0.5f ? Vec2{c, along} : Vec2{along, c};
  }

  /// Pushes a circle out of buildings; reflects the velocity. Returns true on contact.
  bool collide(Vec2& p, Vec2& v, f32 radius) const {
    bool hit = false;
    const i32 cx = cell_of(p.x), cy = cell_of(p.y);
    for (i32 dy = -1; dy <= 1; ++dy)
      for (i32 dx = -1; dx <= 1; ++dx) {
        const i32 gx = cx + dx, gy = cy + dy;
        if (gx < 0 || gy < 0 || gx >= grid_size_ || gy >= grid_size_) continue;
        for (u32 index : grid_[static_cast<std::size_t>(gy) * grid_size_ + gx]) {
          const Footprint& f = buildings[index];
          if (p.x < f.lo.x - radius || p.x > f.hi.x + radius || p.y < f.lo.y - radius || p.y > f.hi.y + radius) continue;
          hit |= push_out(f, p, v, radius);
        }
      }
    const f32 limit = kHalf + 55;
    if (std::fabs(p.x) > limit) { p.x = std::copysign(limit, p.x); v.x *= -0.4f; hit = true; }
    if (std::fabs(p.y) > limit) { p.y = std::copysign(limit, p.y); v.y *= -0.4f; hit = true; }
    return hit;
  }

 private:
  void add_box(Rng& rng, FootprintStream& footprints, f32 a0, f32 b0, f32 a1, f32 b1, f32 height) {
    const Vec2 ring[4] = {{a0, b0}, {a1, b0}, {a1, b1}, {a0, b1}};
    footprints.begin(rng.next(), 0, height);
    footprints.ring(ring, 4);
    add_collider(ring, 4);
  }
  void add_collider(const Vec2* ring, u32 n) {
    Footprint f;
    f.ring.assign(ring, ring + n);
    f.lo = f.hi = ring[0];
    for (u32 i = 1; i < n; ++i) {
      f.lo = {min_of(f.lo.x, ring[i].x), min_of(f.lo.y, ring[i].y)};
      f.hi = {max_of(f.hi.x, ring[i].x), max_of(f.hi.y, ring[i].y)};
    }
    buildings.push_back(std::move(f));
  }
  i32 cell_of(f32 v) const { return static_cast<i32>(std::floor((v + kHalf + 80) / kCell)); }
  void index_colliders() {
    grid_size_ = static_cast<i32>(std::ceil((2 * kHalf + 160) / kCell));
    grid_.assign(static_cast<std::size_t>(grid_size_) * grid_size_, {});
    for (u32 i = 0; i < buildings.size(); ++i) {
      const Footprint& f = buildings[i];
      for (i32 gy = max_of(0, cell_of(f.lo.y)); gy <= min_of(grid_size_ - 1, cell_of(f.hi.y)); ++gy)
        for (i32 gx = max_of(0, cell_of(f.lo.x)); gx <= min_of(grid_size_ - 1, cell_of(f.hi.x)); ++gx)
          grid_[static_cast<std::size_t>(gy) * grid_size_ + gx].push_back(i);
    }
  }
  static bool inside(const Footprint& f, Vec2 p) {
    bool in = false;
    const std::size_t n = f.ring.size();
    for (std::size_t i = 0, j = n - 1; i < n; j = i++) {
      const Vec2 a = f.ring[i], b = f.ring[j];
      if (((a.y > p.y) != (b.y > p.y)) && p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x) in = !in;
    }
    return in;
  }
  static bool push_out(const Footprint& f, Vec2& p, Vec2& v, f32 radius) {
    f32 best = 1e30f;
    Vec2 closest;
    const std::size_t n = f.ring.size();
    for (std::size_t i = 0, j = n - 1; i < n; j = i++) {
      const Vec2 a = f.ring[j], b = f.ring[i], ab = b - a;
      const f32 t = clamp(dot(p - a, ab) / max_of(dot(ab, ab), 1e-6f), 0.0f, 1.0f);
      const Vec2 q = a + ab * t;
      const f32 d = length(p - q);
      if (d < best) { best = d; closest = q; }
    }
    const bool in = inside(f, p);
    if (!in && best >= radius) return false;
    Vec2 normal = p - closest;
    const f32 len = length(normal);
    normal = len > 1e-5f ? normal * (1.0f / len) : Vec2{1, 0};
    if (in) normal = normal * -1.0f;
    p = closest + normal * (radius + 0.01f);
    const f32 vn = dot(v, normal);
    if (vn < 0) v = v - normal * (vn * 1.35f);
    return true;
  }

  std::vector<std::vector<u32>> grid_;
  i32 grid_size_ = 0;
};

// ---------------------------------------------------------------------------
// the game

struct Orb { Vec2 p; f32 phase; bool alive; };
struct TrafficCar { bool horizontal; f32 line, lane, along, speed, dir; Vec4 tint; };

class OrbRun final : public App {
 public:
  RendererOptions configure() override {
    RendererOptions o;
    o.shadows = true;
    o.shadow.resolution = 2048;
    o.shadow.extent = 110;
    o.shadow.bias = 0.0014f;
    return o;
  }
  Span<ShaderExtension* const> extensions() override { return {extensions_, 1}; }

  bool start(Engine& engine) override {
    Renderer& r = engine.renderer;
    screen_width_ = engine.width();
    launch_time_ = engine.arg_number("time", 0.455);  // ?time=0.6 starts at night
    r.camera.fov = 65 * kPi / 180;
    r.camera.far = 2500;
    // Static meshes that never change between runs.
    ground_ = r.allocate_id();
    MeshData ground = primitives::plane(3000, 3000, {0.19f, 0.27f, 0.15f});
    MeshInput gi = ground.as_input();
    gi.material.cast_shadow = false;
    if (!r.upsert(ground_, gi)) return false;
    if (!build_car_meshes(r)) return false;
    if (!particles_.init(r, 512)) return false;
    return new_run(engine, 20261022u);
  }

  void update(Engine& engine, const Frame& frame) override {
    Renderer& r = engine.renderer;
    Input& in = engine.input;
    const f32 dt = frame.dt;
    if (in.pressed(Key::enter)) new_run(engine, rng_.next());
    if (in.pressed(Key::n)) fast_time_ = !fast_time_;
    elapsed_ += dt;
    now_ms_ = frame.time * 1000.0;
    if (!finished_) run_time_ += dt;

    drive(in, dt);
    update_traffic(dt);
    collect(engine);
    update_daylight(r, dt);
    update_camera(r, in, dt);
    upload_dynamic(r);
    update_status(engine);
  }

 private:
  // ---- setup ----
  bool build_car_meshes(Renderer& r) {
    MeshData body;
    primitives::append(body, primitives::box({4.2f, 2.0f, 0.55f}, {0.9f, 0.9f, 0.92f}), Mat4::translate({0, 0, 0.35f}));
    primitives::append(body, primitives::box({2.0f, 1.7f, 0.55f}, {0.12f, 0.16f, 0.22f}), Mat4::translate({-0.3f, 0, 0.85f}));
    primitives::append(body, primitives::box({0.5f, 2.3f, 0.12f}, {0.95f, 0.35f, 0.15f}), Mat4::translate({-2.0f, 0, 0.75f}));
    car_ = r.allocate_id();
    MeshInput bi = body.as_input();
    bi.material.roughness = 0.35f;
    bi.material.metalness = 0.3f;
    if (!r.upsert(car_, bi)) return false;
    thrusters_ = r.allocate_id();
    MeshData pod = primitives::sphere(0.32f, 12, 8, {0.3f, 0.9f, 1.0f});
    MeshInput ti = pod.as_input();
    ti.material.emissive = {0.6f, 2.4f, 3.0f};
    ti.material.cast_shadow = false;
    if (!r.upsert(thrusters_, ti)) return false;
    // Traffic: one instanced mesh for every car.
    MeshData traffic;
    primitives::append(traffic, primitives::box({4.0f, 1.9f, 0.9f}, {1, 1, 1}), Mat4::translate({0, 0, 0.75f}));
    primitives::append(traffic, primitives::box({2.0f, 1.7f, 0.6f}, {0.1f, 0.12f, 0.16f}), Mat4::translate({-0.2f, 0, 1.5f}));
    traffic_mesh_ = std::move(traffic);
    return true;
  }

  bool new_run(Engine& engine, u32 seed) {
    Renderer& r = engine.renderer;
    rng_ = Rng(seed);
    for (u32 id : run_meshes_) r.remove(id);
    run_meshes_.clear();

    FootprintStream footprints;
    RoadStream roads;
    MeshData plates;
    city_.generate(seed, footprints, roads, plates);

    // Buildings: extruded by the engine, wall records feed the window shader.
    BuildingOptions bo;
    bo.chunk_size = 200;
    bo.style.walls = {{0.78, 0.74, 0.68}, {0.62, 0.60, 0.58}, {0.70, 0.52, 0.42}, {0.48, 0.52, 0.57}, {0.84, 0.82, 0.77}, {0.55, 0.40, 0.33}};
    bo.style.roofs = {{0.27, 0.28, 0.30}, {0.22, 0.23, 0.25}, {0.36, 0.30, 0.27}};
    BuildingBuilder buildings;
    if (!buildings.begin(footprints.data(), footprints.size(), bo)) return false;
    while (!buildings.step(1e9)) {}
    for (GeoChunk& chunk : buildings.chunks()) {
      weld(chunk.mesh);
      if (!upload_static(r, chunk.mesh, Material{}, 0)) return false;
    }
    // Streets on flat ground.
    RoadOptions ro;
    ro.radius = 2000;
    ro.chunk_size = 200;
    ro.palette = {{0.20, 0.21, 0.23}, {0.90, 0.89, 0.86}};
    RoadBuilder road_builder;
    if (!road_builder.begin(roads.data(), roads.size(), ro, HeightSampler{})) return false;
    while (!road_builder.step(1e9)) {}
    Material road_material;
    road_material.cast_shadow = false;
    road_material.double_sided = true;
    road_material.roughness = 1.0f;
    for (GeoChunk& chunk : road_builder.chunks()) {
      weld(chunk.mesh);
      if (!upload_static(r, chunk.mesh, road_material, kMaxExtensionAttributes)) return false;
    }
    Material plate_material;
    plate_material.roughness = 0.95f;
    if (!upload_static(r, plates, plate_material, kMaxExtensionAttributes)) return false;
    if (!plant_trees(r)) return false;

    // Orbs on the streets, traffic on the lanes.
    orbs_.clear();
    for (u32 i = 0; i < kOrbs; ++i) orbs_.push_back({city_.street_point(rng_), rng_.range(0, 6.28f), true});
    traffic_.clear();
    for (u32 i = 0; i < kTraffic; ++i) {
      TrafficCar c;
      c.horizontal = rng_.unit() < 0.5f;
      c.line = -City::kHalf + static_cast<f32>(rng_.below(City::kBlocks + 1)) * City::kPitch;
      c.dir = rng_.unit() < 0.5f ? 1.0f : -1.0f;
      c.lane = c.dir * 2.8f;
      c.along = rng_.range(-City::kHalf, City::kHalf);
      c.speed = rng_.range(9, 15);
      const Vec3 colors[5] = {{0.85f, 0.12f, 0.1f}, {0.1f, 0.3f, 0.8f}, {0.95f, 0.8f, 0.2f}, {0.9f, 0.9f, 0.9f}, {0.15f, 0.6f, 0.3f}};
      const Vec3 c3 = colors[rng_.below(5)];
      c.tint = {c3.x, c3.y, c3.z, 1};
      traffic_.push_back(c);
    }
    particles_.clear();
    car_pos_ = {-City::kHalf + City::kPitch * 3, -City::kHalf + City::kPitch * 3 + 20};
    car_vel_ = {};
    heading_ = kPi * 0.5f;
    boost_ = 1;
    collected_ = 0;
    run_time_ = 0;
    finished_ = false;
    time_of_day_ = 0.455f;  // late afternoon: the run ends in the night
    time_of_day_ = static_cast<f32>(clamp(launch_time_, 0.0, 0.999));
    camera_eye_ = {car_pos_.x, car_pos_.y - 14, 7};
    upload_dynamic(r, true);
    return true;
  }

  bool upload_static(Renderer& r, const MeshData& mesh, const Material& material, u32 extra_slot) {
    const u32 id = r.allocate_id();
    MeshInput in = mesh.as_input(extra_slot);
    in.material = material;
    if (!r.upsert(id, in)) return false;
    run_meshes_.push_back(id);
    return true;
  }

  bool plant_trees(Renderer& r) {
    std::vector<f32> trunks, crowns, crown_tints;
    for (int bx = 0; bx < City::kBlocks; ++bx)
      for (int by = 0; by < City::kBlocks; ++by) {
        const f32 x0 = -City::kHalf + bx * City::kPitch + City::kStreet * 0.5f + 1.2f, y0 = -City::kHalf + by * City::kPitch + City::kStreet * 0.5f + 1.2f;
        const f32 x1 = x0 + City::kPitch - City::kStreet - 2.4f, y1 = y0 + City::kPitch - City::kStreet - 2.4f;
        for (f32 t = 4; t < x1 - x0 - 2; t += 11) {
          const Vec2 spots[4] = {{x0 + t, y0}, {x0 + t, y1}, {x0, y0 + t}, {x1, y0 + t}};
          for (Vec2 s : spots) {
            if (rng_.unit() < 0.35f) continue;
            const f32 scale = rng_.range(0.8f, 1.3f);
            put_matrix(trunks, Mat4::trs({s.x, s.y, 0.18f}, Quat{}, {scale, scale, scale}));
            put_matrix(crowns, Mat4::trs({s.x, s.y, 0.18f + 2.0f * scale}, Quat::axis_angle({0, 0, 1}, rng_.range(0, 6)), {scale, scale, scale * rng_.range(0.9f, 1.4f)}));
            const f32 g = rng_.range(0.75f, 1.1f);
            crown_tints.insert(crown_tints.end(), {0.55f * g, 0.85f * g, 0.45f * g, 1});
          }
        }
      }
    MeshData trunk = primitives::cylinder(0.16f, 2.2f, 8, {0.36f, 0.25f, 0.16f});
    MeshData crown = primitives::cone(1.5f, 4.2f, 10, {0.22f, 0.42f, 0.18f});
    for (int k = 0; k < 2; ++k) {
      const u32 id = r.allocate_id();
      MeshInput in = (k ? crown : trunk).as_input();
      const std::vector<f32>& t = k ? crowns : trunks;
      in.transforms = Span<const f32>(t.data(), t.size());
      if (k) in.tints = Span<const f32>(crown_tints.data(), crown_tints.size());
      if (!r.upsert(id, in)) return false;
      run_meshes_.push_back(id);
    }
    return true;
  }

  // ---- simulation ----
  void drive(Input& in, f32 dt) {
    f32 throttle = 0, steer = 0;
    bool boosting = false;
    if (in.down(Key::w) || in.down(Key::up)) throttle += 1;
    if (in.down(Key::s) || in.down(Key::down)) throttle -= 0.7f;
    if (in.down(Key::a) || in.down(Key::left)) steer += 1;
    if (in.down(Key::d) || in.down(Key::right)) steer -= 1;
    if (in.down(Key::space)) boosting = true;
    // Touch: left half = analogue stick from the touch start, right half = boost.
    const f32 half_width = 0.5f * static_cast<f32>(screen_width_);
    for (u32 i = 0; i < kMaxTouches; ++i) {
      const Touch& t = in.touches()[i];
      if (t.id < 0) continue;
      if (t.start.x < half_width) {
        const Vec2 d = t.position - t.start;
        steer = clamp(-d.x / 90.0f, -1.0f, 1.0f);
        throttle = clamp(-d.y / 90.0f, -0.7f, 1.0f);
        if (length(d) < 12) throttle = 0.6f;
      } else {
        boosting = true;
      }
    }
    boosting = boosting && boost_ > 0.02f;
    boost_ = clamp(boost_ + (boosting ? -0.45f : 0.12f) * dt, 0.0f, 1.0f);

    const Vec2 forward{std::cos(heading_), std::sin(heading_)}, side{-forward.y, forward.x};
    f32 fwd = dot(car_vel_, forward), lat = dot(car_vel_, side);
    fwd += throttle * (throttle > 0 ? 24.0f : 30.0f) * dt;
    if (boosting) fwd += 42 * dt;
    const f32 cap = boosting ? 60.0f : 36.0f;
    fwd = clamp(fwd, -12.0f, cap);
    if (throttle == 0 && !boosting) fwd *= std::exp(-0.6f * dt);
    lat *= std::exp(-7.5f * dt);
    const f32 grip = clamp(std::fabs(fwd) / 9.0f, 0.0f, 1.0f) * (fwd >= 0 ? 1.0f : -1.0f);
    heading_ += steer * 2.3f * grip * dt;
    steer_visual_ = damp(steer_visual_, steer * grip, 6, dt);
    car_vel_ = forward * fwd + side * lat;
    // Sub-stepped movement keeps fast boosts from tunnelling through walls.
    const u32 steps = 1 + static_cast<u32>(length(car_vel_) * dt / 0.6f);
    bool bumped = false;
    for (u32 s = 0; s < steps; ++s) {
      car_pos_ += car_vel_ * (dt / steps);
      bumped |= city_.collide(car_pos_, car_vel_, 1.7f);
      for (const TrafficCar& t : traffic_) {
        const Vec2 tp = traffic_position(t), d = car_pos_ - tp;
        const f32 dist = length(d);
        if (dist < 3.2f && dist > 1e-4f) {
          const Vec2 n = d * (1.0f / dist);
          car_pos_ = tp + n * 3.2f;
          const f32 vn = dot(car_vel_, n);
          if (vn < 0) car_vel_ = car_vel_ - n * (vn * 1.5f);
          bumped = true;
        }
      }
    }
    shake_ = max_of(shake_ * std::exp(-5 * dt), bumped ? clamp(length(car_vel_) / 30.0f, 0.15f, 0.8f) : 0.0f);
  }

  Vec2 traffic_position(const TrafficCar& c) const { return c.horizontal ? Vec2{c.along, c.line + c.lane} : Vec2{c.line + c.lane, c.along}; }

  void update_traffic(f32 dt) {
    const f32 span = City::kHalf + 60;
    for (TrafficCar& c : traffic_) {
      // Brake behind the player.
      const Vec2 p = traffic_position(c), ahead = c.horizontal ? Vec2{c.dir, 0} : Vec2{0, c.dir};
      const Vec2 rel = car_pos_ - p;
      const bool blocked = dot(rel, ahead) > 0 && dot(rel, ahead) < 10 && std::fabs(cross(ahead, rel)) < 2.5f;
      c.along += c.dir * (blocked ? 0.0f : c.speed) * dt;
      if (c.along > span) c.along = -span;
      if (c.along < -span) c.along = span;
    }
  }

  void collect(Engine& engine) {
    for (Orb& o : orbs_) {
      if (!o.alive || length(o.p - car_pos_) > 3.2f) continue;
      o.alive = false;
      collected_++;
      for (u32 k = 0; k < 28; ++k) {
        const f32 a = rng_.range(0, 2 * kPi), up = rng_.range(4, 11), out = rng_.range(3, 9);
        ParticleSpec spark;
        spark.position = {o.p.x, o.p.y, 1.6f};
        spark.velocity = {std::cos(a) * out, std::sin(a) * out, up};
        spark.color = {0.55f, 1.0f, 0.92f, 1};
        spark.start_ms = now_ms_;
        spark.life_ms = rng_.range(600, 1100);
        spark.size = rng_.range(0.25f, 0.5f);
        spark.gravity = 14;
        spark.fade_extra = 0.3f;
        particles_.emit(spark);
      }
      if (collected_ == kOrbs) {
        finished_ = true;
        if (best_time_ <= 0 || run_time_ < best_time_) best_time_ = run_time_;
      }
    }
    (void)engine;
  }

  void update_daylight(Renderer& r, f32 dt) {
    time_of_day_ += dt * (fast_time_ ? 1.0f / 40 : 1.0f / 480);
    if (time_of_day_ >= 1) time_of_day_ -= 1;
    const f32 phase = time_of_day_ * 2 * kPi;
    const Vec3 sun = normalize({std::cos(phase) * 0.62f, -0.38f, std::sin(phase)});
    const Vec3 moon = normalize({-sun.x, 0.3f, max_of(0.35f, -sun.z)});
    const f32 night = smoothstep(0.06f, -0.16f, sun.z);
    const f32 dusk = clamp(1 - std::fabs(sun.z) * 4, 0.0f, 1.0f) * (1 - night * 0.6f);
    Lighting& l = r.lighting;
    const Vec3 day_sun{2.8f, 2.47f, 2.11f}, dusk_sun{2.3f, 1.25f, 0.62f}, moon_light{0.16f, 0.2f, 0.32f};
    l.sun_direction = sun.z > -0.04f ? sun : moon;
    l.sun_color = mix3(mix3(day_sun, dusk_sun, dusk), moon_light, night);
    l.sky_color = mix3({0.469f, 0.609f, 0.85f}, {0.035f, 0.05f, 0.1f}, night);
    l.ground_color = mix3({0.125f, 0.094f, 0.054f}, {0.012f, 0.012f, 0.02f}, night);
    l.fog_color = mix3(mix3({0.474f, 0.584f, 0.716f}, {0.62f, 0.45f, 0.36f}, dusk * 0.6f), {0.03f, 0.04f, 0.07f}, night);
    l.fog_density = 0.0011f;
    l.env_intensity = 0.9f - 0.78f * night;
    l.env_zenith = mix3({0.05f, 0.2f, 0.62f}, {0.012f, 0.022f, 0.06f}, night);
    l.env_horizon = mix3({0.52f, 0.66f, 0.86f}, {0.05f, 0.075f, 0.13f}, night);
    l.exposure = 1.0f + 0.35f * night;
    r.sky.sun = sun;
    r.sky.moon = moon;
    r.sky.night = night;
    windows_.night = night;
    night_ = night;
  }

  void update_camera(Renderer& r, Input& in, f32 dt) {
    camera_distance_ = clamp(camera_distance_ * (1 + in.wheel() * 0.0012f), 7.0f, 40.0f);
    const Vec3 forward{std::cos(heading_), std::sin(heading_), 0};
    const f32 speed = length(car_vel_);
    const f32 dist = camera_distance_ + speed * 0.12f;
    const Vec3 car{car_pos_.x, car_pos_.y, 0.6f};
    const Vec3 desired = car - forward * dist + Vec3{0, 0, 3.2f + dist * 0.28f};
    camera_eye_ = damp3(camera_eye_, desired, 4.5f, dt);
    Vec3 eye = camera_eye_;
    if (shake_ > 0.01f) eye += Vec3{rng_.range(-1, 1), rng_.range(-1, 1), rng_.range(-1, 1)} * shake_ * 0.35f;
    r.camera.eye = eye;
    r.camera.target = car + forward * 5 + Vec3{0, 0, 1.2f};
    r.camera.fov = (62 + clamp(speed - 30, 0.0f, 30.0f) * 0.35f) * kPi / 180;
  }

  // ---- per-frame GPU data ----
  void upload_dynamic(Renderer& r, bool force = false) {
    (void)force;
    const f32 t = static_cast<f32>(elapsed_);
    // Car with hover bob and lean.
    const f32 bob = 0.55f + 0.07f * std::sin(t * 3.1f);
    const Vec2 fwd2{std::cos(heading_), std::sin(heading_)};
    const f32 accel_pitch = clamp((dot(car_vel_, fwd2) - last_forward_speed_) * 0.02f, -0.12f, 0.12f);
    last_forward_speed_ = dot(car_vel_, fwd2);
    pitch_ = damp(pitch_, accel_pitch, 5, 1.0f / 60);
    const Quat q = Quat::axis_angle({0, 0, 1}, heading_) * Quat::axis_angle({1, 0, 0}, -steer_visual_ * 0.18f) * Quat::axis_angle({0, 1, 0}, -pitch_);
    const Mat4 car_matrix = Mat4::trs({car_pos_.x, car_pos_.y, bob}, q, {1, 1, 1});
    r.update_instances(car_, Span<const f32>(car_matrix.m, 16), {});
    scratch_.clear();
    const Vec3 pods[4] = {{1.5f, 1.05f, 0.25f}, {1.5f, -1.05f, 0.25f}, {-1.6f, 1.05f, 0.25f}, {-1.6f, -1.05f, 0.25f}};
    for (const Vec3& pod : pods) put_matrix(scratch_, car_matrix * Mat4::translate(pod));
    r.update_instances(thrusters_, Span<const f32>(scratch_.data(), scratch_.size()), {});

    // Orbs (bobbing, spinning); collected ones disappear.
    scratch_.clear();
    for (const Orb& o : orbs_)
      if (o.alive)
        put_matrix(scratch_, Mat4::trs({o.p.x, o.p.y, 1.6f + 0.35f * std::sin(t * 2.2f + o.phase)}, Quat::axis_angle({0, 0, 1}, t + o.phase),
                                       Vec3{1, 1, 1} * (1 + 0.08f * std::sin(t * 6 + o.phase))));
    sync_instanced(r, orbs_id_, orb_mesh_, orb_material(), scratch_, {});

    // Sparks: the engine's particle system (sorted, camera-facing, one draw).
    particles_.tick(now_ms_, r.camera.eye);

    // Traffic.
    scratch_.clear();
    tints_.clear();
    for (const TrafficCar& c : traffic_) {
      const Vec2 p = traffic_position(c);
      const f32 yaw = c.horizontal ? (c.dir > 0 ? 0.0f : kPi) : (c.dir > 0 ? kPi * 0.5f : -kPi * 0.5f);
      put_matrix(scratch_, Mat4::trs({p.x, p.y, 0}, Quat::axis_angle({0, 0, 1}, yaw), {1, 1, 1}));
      tints_.insert(tints_.end(), {c.tint.x, c.tint.y, c.tint.z, c.tint.w});
    }
    Material traffic_material;
    traffic_material.roughness = 0.4f;
    traffic_material.metalness = 0.2f;
    sync_instanced(r, traffic_id_, traffic_mesh_, traffic_material, scratch_, tints_);

    // Lights: the car's headlight glow plus the orbs; the pool keeps the most significant six.
    lights_.request({car_pos_.x + fwd2.x * 6, car_pos_.y + fwd2.y * 6, 2.2f}, {1.0f, 0.9f, 0.75f}, 10 + 40 * night_, 18);
    for (const Orb& o : orbs_)
      if (o.alive) lights_.request({o.p.x, o.p.y, 2.2f}, {0.3f, 1.0f, 0.9f}, 12 + 36 * night_, 16);
    r.set_point_lights(lights_.resolve({car_pos_.x, car_pos_.y, 2.0f}));
  }

  Material orb_material() const {
    Material m;
    m.color = {0.3f, 1.0f, 0.9f, 1};
    m.emissive = {0.5f, 2.6f, 2.2f};
    m.cast_shadow = false;
    return m;
  }
  /// Keeps an instanced mesh in sync with a (possibly empty) instance list.
  void sync_instanced(Renderer& r, u32& id, const MeshData& mesh, const Material& material, const std::vector<f32>& transforms,
                      const std::vector<f32>& tints) {
    const Span<const f32> t(transforms.data(), transforms.size()), c(tints.data(), tints.size());
    if (transforms.empty()) {
      if (id) r.set_visible(id, false);
      return;
    }
    if (!id || !r.mesh(id)) {
      if (!id) id = r.allocate_id();
      MeshInput in = mesh.as_input();
      in.material = material;
      in.transforms = t;
      in.tints = c;
      r.upsert(id, in);
      return;
    }
    r.set_visible(id, true);
    r.update_instances(id, t, c);
  }

  void update_status(Engine& engine) {
    char boost[11] = {};
    for (int i = 0; i < 10; ++i) boost[i] = i < static_cast<int>(boost_ * 10 + 0.5f) ? '#' : '-';
    const int m = static_cast<int>(run_time_) / 60;
    const f32 s = run_time_ - m * 60;
    char best[32] = "--:--.-";
    if (best_time_ > 0) std::snprintf(best, sizeof best, "%02d:%04.1f", static_cast<int>(best_time_) / 60, best_time_ - static_cast<int>(best_time_) / 60 * 60);
    char text[256];
    if (finished_)
      std::snprintf(text, sizeof text, "RUN COMPLETE  %02d:%04.1f  |  best %s  |  Enter: new city", m, s, best);
    else
      std::snprintf(text, sizeof text, "ORBS %u/%u  |  %02d:%04.1f  |  BOOST [%s]  |  %.0f km/h  |  best %s", collected_, kOrbs, m, s, boost,
                    length(car_vel_) * 3.6f, best);
    engine.set_status(text);
  }

  static constexpr u32 kOrbs = 24, kTraffic = 22;

  NightWindows windows_;
  ShaderExtension* extensions_[1] = {&windows_};
  City city_;
  Rng rng_{1};
  std::vector<u32> run_meshes_;
  u32 ground_ = 0, car_ = 0, thrusters_ = 0, orbs_id_ = 0, traffic_id_ = 0;
  Particles particles_;
  LightPool lights_;
  f64 now_ms_ = 0;
  MeshData orb_mesh_ = primitives::sphere(0.9f, 18, 12, {0.3f, 1.0f, 0.9f});
  MeshData traffic_mesh_;
  std::vector<Orb> orbs_;
  std::vector<TrafficCar> traffic_;
  std::vector<f32> scratch_, tints_;
  Vec2 car_pos_, car_vel_;
  f32 heading_ = 0, steer_visual_ = 0, pitch_ = 0, last_forward_speed_ = 0, boost_ = 1, shake_ = 0;
  Vec3 camera_eye_;
  f32 camera_distance_ = 12;
  f64 elapsed_ = 0;
  f32 run_time_ = 0, best_time_ = 0, time_of_day_ = 0.455f, night_ = 0;
  f64 launch_time_ = 0.455;
  u32 collected_ = 0;
  bool finished_ = false, fast_time_ = false;
  i32 screen_width_ = 1;

 public:
  void resized(Engine& engine) override { screen_width_ = engine.width(); }
};

}  // namespace

App* kern_create_app() {
  static OrbRun game;
  return &game;
}
