// Kern engine unit tests (host build, see kern/tests/run.sh).
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "gl_stub.h"
#include "kern/kern.h"
#include "kern/physics/physics.h"
#include "kern/core/json.h"
#include "kern/world/runtime.h"
#include "kern/world/scene_meshes.h"
#include "kern/editor/editor.h"

using namespace kern;

namespace {

int g_failures = 0, g_checks = 0;

#define CHECK(cond)                                                                 \
  do {                                                                              \
    ++g_checks;                                                                     \
    if (!(cond)) {                                                                  \
      ++g_failures;                                                                 \
      std::printf("  FAIL %s:%d: %s  [%s]\n", __FILE__, __LINE__, #cond, last_error()); \
    }                                                                               \
  } while (0)
#define CHECK_NEAR(a, b, eps) CHECK(std::fabs((a) - (b)) <= (eps))

struct Test {
  const char* name;
  void (*fn)();
};
std::vector<Test>& registry() {
  static std::vector<Test> tests;
  return tests;
}
struct Register {
  Register(const char* name, void (*fn)()) { registry().push_back({name, fn}); }
};
#define TEST(name)                         \
  void test_##name();                      \
  Register register_##name(#name, test_##name); \
  void test_##name()

}  // namespace
namespace kern::world {
inline f64 rad2deg_test(const DQ& q) { return euler_xyz(q)[2] * 180 / kPiD; }
}  // namespace kern::world
namespace {

bool mat_near(const Mat4& a, const Mat4& b, f32 eps = 1e-5f) {
  for (int i = 0; i < 16; ++i)
    if (std::fabs(a.m[i] - b.m[i]) > eps) return false;
  return true;
}

// ---------------------------------------------------------------------------
// math

TEST(math_matrices) {
  const Mat4 t = Mat4::translate({1, 2, 3}), s = Mat4::scale({2, 3, 4});
  const Quat q = Quat::axis_angle({0, 0, 1}, kPi / 2);
  const Mat4 trs = Mat4::trs({1, 2, 3}, q, {2, 3, 4});
  // trs == T * R * S
  Mat4 r;
  const Vec3 x = q.rotate({1, 0, 0}), y = q.rotate({0, 1, 0}), z = q.rotate({0, 0, 1});
  r.m[0] = x.x; r.m[1] = x.y; r.m[2] = x.z;
  r.m[4] = y.x; r.m[5] = y.y; r.m[6] = y.z;
  r.m[8] = z.x; r.m[9] = z.y; r.m[10] = z.z;
  CHECK(mat_near(trs, t * r * s));
  const Vec3 p = trs.transform_point({1, 0, 0});
  CHECK_NEAR(p.x, 1.0f, 1e-5f);
  CHECK_NEAR(p.y, 4.0f, 1e-5f);
  CHECK_NEAR(p.z, 3.0f, 1e-5f);
  f64 inv[16];
  CHECK(invert(trs, inv));
  Mat4 invf;
  for (int i = 0; i < 16; ++i) invf.m[i] = static_cast<f32>(inv[i]);
  CHECK(mat_near(trs * invf, Mat4::identity(), 1e-5f));
  Mat4 singular = Mat4::scale({1, 0, 1});
  CHECK(!invert(singular, inv));
  // Normal matrix keeps normals perpendicular under non-uniform scale.
  f32 nm[9];
  CHECK(normal_matrix3(s.m, nm));
  const Vec3 tangent = s.transform_vector({1, -1, 0});
  const Vec3 normal{nm[0] * 1 + nm[3] * 1, nm[1] * 1 + nm[4] * 1, nm[2] * 1 + nm[5] * 1};
  CHECK_NEAR(dot(tangent, normal), 0.0f, 1e-5f);
  CHECK(!normal_matrix3(singular.m, nm));
}

TEST(math_camera) {
  const Vec3 eye{10, -5, 4}, target{0, 3, 1};
  bool ok = false;
  const Mat4 view = Mat4::look_at(eye, target, {0, 0, 1}, &ok);
  CHECK(ok);
  const Mat4 vp = Mat4::perspective(1.0f, 1.6f, 0.1f, 500.0f) * view;
  const Vec4 c = vp.transform({target, 1});
  CHECK_NEAR(c.x / c.w, 0.0f, 1e-5f);
  CHECK_NEAR(c.y / c.w, 0.0f, 1e-5f);
  Mat4::look_at(eye, eye, {0, 0, 1}, &ok);
  CHECK(!ok);
  const Frustum f = Frustum::from_matrix(vp);
  CHECK(f.sphere_visible({target, 1}));
  CHECK(!f.sphere_visible({eye + (eye - target) * 3, 1}));
  CHECK(f.sphere_visible({eye + (eye - target) * 3, 1e6f}));
  CHECK(f.sphere_visible(Sphere{}));  // invalid spheres are never culled
}

// ---------------------------------------------------------------------------
// picking & BVH

MeshData grid_mesh(u32 n, f32 size) {
  MeshData m;
  for (u32 j = 0; j < n; ++j)
    for (u32 i = 0; i < n; ++i) {
      const f32 x0 = i * size, y0 = j * size, h = std::sin(i * 0.3f) + std::cos(j * 0.2f);
      const Vec3 a{x0, y0, h}, b{x0 + size, y0, h}, c{x0 + size, y0 + size, h}, d{x0, y0 + size, h};
      for (Vec3 p : {a, b, c, a, c, d}) m.push(p, {0, 0, 1}, {1, 1, 1}, {0, 0});
    }
  return m;
}

TEST(bvh_matches_brute_force) {
  const MeshData g = grid_mesh(60, 2);
  Bvh bvh;
  bvh.build(g.positions.data(), g.vertex_count(), nullptr, 0);
  CHECK(!bvh.empty());
  CHECK(bvh.triangle_count() == g.triangle_count());
  u32 seed = 12345;
  auto rnd = [&] { seed = seed * 1664525u + 1013904223u; return (seed >> 8) * (1.0f / 16777216.0f); };
  int agree = 0;
  for (int k = 0; k < 400; ++k) {
    Ray ray;
    ray.origin = {rnd() * 120, rnd() * 120, 30};
    ray.direction = normalize({rnd() - 0.5f, rnd() - 0.5f, -1});
    f32 brute = 1e30f;
    for (u32 t = 0; t < g.triangle_count(); ++t) {
      TriangleHit h;
      if (intersect_triangle(ray, Vec3::load(&g.positions[t * 9]), Vec3::load(&g.positions[t * 9 + 3]), Vec3::load(&g.positions[t * 9 + 6]), brute,
                             true, h))
        brute = h.distance;
    }
    f32 best = 1e30f;
    bvh.traverse(ray, best, [&](u32 t, f32) {
      TriangleHit h;
      if (intersect_triangle(ray, Vec3::load(&g.positions[t * 9]), Vec3::load(&g.positions[t * 9 + 3]), Vec3::load(&g.positions[t * 9 + 6]), best,
                             true, h))
        best = h.distance;
      return best;
    });
    agree += (best == brute);
  }
  CHECK(agree == 400);
}

// ---------------------------------------------------------------------------
// geometry

TEST(earcut_shapes) {
  Earcut ec;
  std::vector<u32> tris;
  const f64 square[] = {0, 0, 10, 0, 10, 10, 0, 10};
  ec.triangulate(square, 4, nullptr, 0, tris);
  CHECK(tris.size() == 6);
  CHECK_NEAR(Earcut::deviation(square, 4, nullptr, 0, tris.data(), static_cast<u32>(tris.size())), 0.0, 1e-12);
  const f64 holed[] = {0, 0, 10, 0, 10, 10, 0, 10, 3, 3, 3, 7, 7, 7, 7, 3};
  const u32 hole[] = {4};
  tris.clear();
  ec.triangulate(holed, 8, hole, 1, tris);
  CHECK(tris.size() == 24);
  CHECK_NEAR(Earcut::deviation(holed, 8, hole, 1, tris.data(), static_cast<u32>(tris.size())), 0.0, 1e-12);
  const f64 line[] = {0, 0, 5, 0, 10, 0};
  tris.clear();
  ec.triangulate(line, 3, nullptr, 0, tris);
  CHECK(tris.empty());
  // A 200-point star exercises the z-order hashed path.
  std::vector<f64> star;
  for (int i = 0; i < 200; ++i) {
    const f64 a = i * 2 * 3.14159265358979 / 200, r = (i % 2) ? 40 : 100;
    star.push_back(std::cos(a) * r);
    star.push_back(std::sin(a) * r);
  }
  tris.clear();
  ec.triangulate(star.data(), 200, nullptr, 0, tris);
  CHECK(tris.size() == 198 * 3);
  CHECK(Earcut::deviation(star.data(), 200, nullptr, 0, tris.data(), static_cast<u32>(tris.size())) < 1e-9);
}

TEST(weld_and_hash) {
  MeshData cube = primitives::box({1, 2, 3});
  // Expand to a soup, then weld back.
  MeshData soup;
  for (u32 i : cube.indices) soup.push(Vec3::load(&cube.positions[i * 3]), Vec3::load(&cube.normals[i * 3]), {1, 1, 1}, {0, 0});
  CHECK(soup.vertex_count() == 36);
  const u32 hash = content_hash(soup);
  const WeldStats s = weld(soup);
  CHECK(s.indexed);
  CHECK(s.unique_vertices == 24);  // hard edges: 4 per face
  CHECK(soup.indices.size() == 36);
  CHECK(s.bytes_after < s.bytes_before);
  CHECK(content_hash(soup) != hash);
  MeshData a = primitives::box({1, 1, 1}), b = primitives::box({1, 1, 1});
  CHECK(content_hash(a) == content_hash(b));
  b.positions[0] += 1e-3f;
  CHECK(content_hash(a) != content_hash(b));
}

TEST(building_builder) {
  FootprintStream fs;
  const Vec2 rect[4] = {{0, 0}, {10, 0}, {10, 20}, {0, 20}};
  fs.begin(7, 0, 12);
  fs.ring(rect, 4);
  const Vec2 outer[4] = {{400, 0}, {440, 0}, {440, 40}, {400, 40}}, court[4] = {{410, 10}, {410, 30}, {430, 30}, {430, 10}};
  fs.begin(9, 0, 3);  // too low for a cornice
  fs.ring(outer, 4);
  fs.ring(court, 4);
  const Vec2 tiny[3] = {{0, 0}, {0.1f, 0}, {0, 0.1f}};
  fs.begin(11, 0, 10);
  fs.ring(tiny, 3);
  BuildingBuilder b;
  BuildingOptions o;
  o.chunk_size = 320;
  CHECK(b.begin(fs.data(), fs.size(), o));
  while (!b.step(1e9)) {}
  CHECK(b.processed() == 3);
  CHECK(b.built() == 2);
  CHECK(b.skipped() == 1);
  CHECK(b.chunks().size() == 2);
  const GeoChunk& first = b.chunks()[0];
  CHECK(first.triangles == 2 + 8 + 24);   // roof, walls, cornice
  CHECK(first.mesh.vertex_count() == first.triangles * 3);
  CHECK(first.mesh.extra.size() == first.mesh.vertex_count() * 4);
  const GeoChunk& second = b.chunks()[1];
  CHECK(second.triangles == 8 + 16);      // courtyard roof ring + 8 walls, no cornice
  // Every roof normal points up.
  for (u32 v = 0; v < first.mesh.vertex_count(); ++v)
    if (first.mesh.extra[v * 4] == -1) CHECK(first.mesh.normals[v * 3 + 2] == 1);
}

TEST(road_and_terrain_builders) {
  RoadStream rs;
  const Vec2 line[2] = {{0, 0}, {100, 0}};
  rs.road(line, 2, 10, true, 0, 1);
  RoadOptions o;
  o.radius = 1000;
  o.palette = {{0.2, 0.2, 0.2}, {1, 1, 1}};
  RoadBuilder r;
  HeightSampler five;
  five.fn = [](void*, f64, f64) { return 5.0; };
  CHECK(r.begin(rs.data(), rs.size(), o, five));
  while (!r.step(1e9)) {}
  CHECK(!r.failed());
  CHECK(r.triangles() == 34 + 34 + 28);
  for (const GeoChunk& c : r.chunks())
    for (u32 v = 0; v < c.mesh.vertex_count(); ++v) CHECK(c.mesh.positions[v * 3 + 2] > 5.0f && c.mesh.positions[v * 3 + 2] < 5.2f);
  HeightSampler broken;
  broken.fn = [](void*, f64, f64) -> f64 { return NAN; };
  RoadBuilder bad;
  CHECK(bad.begin(rs.data(), rs.size(), o, broken));
  while (!bad.step(1e9)) {}
  CHECK(bad.failed());

  MeshData tile;
  f64 lo = 0, hi = 0;
  CHECK(build_terrain_tile(0, 0, 5, 4, Rgb{0.3, 0.5, 0.2}, five, tile, &lo, &hi));
  CHECK(tile.vertex_count() == 25);
  CHECK(tile.indices.size() == 4 * 4 * 6);
  CHECK(lo == 5 && hi == 5);
  for (u32 v = 0; v < 25; ++v) CHECK(tile.normals[v * 3 + 2] == 1);
  CHECK(!build_terrain_tile(0, 0, 5, 0, Rgb{}, five, tile));
}

TEST(primitives_closed) {
  for (const MeshData& m : {primitives::box({1, 1, 1}), primitives::sphere(1, 12, 8), primitives::cylinder(1, 2, 10), primitives::cone(1, 2, 10)}) {
    CHECK(!m.indices.empty());
    CHECK(m.indices.size() % 3 == 0);
    for (u32 i : m.indices) CHECK(i < m.vertex_count());
    for (u32 v = 0; v < m.vertex_count(); ++v) CHECK_NEAR(length(Vec3::load(&m.normals[v * 3])), 1.0f, 1e-4f);
  }
}

// ---------------------------------------------------------------------------
// renderer (against the GL stand-in)

MeshInput input_of(const MeshData& m) { return m.as_input(); }

TEST(renderer_meshes) {
  Renderer r;
  RendererOptions o;
  o.shadows = true;
  CHECK(r.init(o));
  const MeshData box = primitives::box({1, 1, 1});
  const u32 a = r.allocate_id(), b = r.allocate_id();
  CHECK(a != b && a && b);
  CHECK(r.upsert(a, input_of(box)));
  CHECK(r.upsert(b, input_of(box)));
  CHECK(r.mesh_count() == 2);
  // Invalid replacement leaves the previous mesh in place.
  MeshInput broken = input_of(box);
  broken.positions.size -= 1;
  CHECK(!r.upsert(a, broken));
  CHECK(r.mesh(a) && r.mesh(a)->vertex_count() == 24);
  MeshInput bad_index = input_of(box);
  std::vector<u32> idx(box.indices);
  idx[3] = 999;
  bad_index.indices = Span<const u32>(idx.data(), idx.size());
  CHECK(!r.upsert(a, bad_index));
  // Identical instances do not touch the GPU.
  const Mat4 m = Mat4::translate({2, 0, 0});
  bool changed = false;
  CHECK(r.update_instances(a, Span<const f32>(m.m, 16), {}, &changed) && changed);
  const u64 uploads = g_gl.buffer_sub_data_calls;
  CHECK(r.update_instances(a, Span<const f32>(m.m, 16), {}, &changed) && !changed);
  CHECK(g_gl.buffer_sub_data_calls == uploads);
  // Instance growth reallocates geometrically.
  std::vector<f32> many;
  for (int i = 0; i < 64; ++i) { const Mat4 t = Mat4::translate({static_cast<f32>(i), 0, 0}); many.insert(many.end(), t.m, t.m + 16); }
  const u64 allocations = g_gl.buffer_data_calls;
  CHECK(r.update_instances(a, Span<const f32>(many.data(), many.size()), {}));
  CHECK(g_gl.buffer_data_calls == allocations + 3);
  CHECK(r.update_instances(a, Span<const f32>(many.data(), 32 * 16), {}));
  CHECK(g_gl.buffer_data_calls == allocations + 3);
  CHECK(r.mesh(a)->instance_count == 32);
  // Singular instances are rejected.
  const Mat4 flat = Mat4::scale({1, 1, 0});
  CHECK(!r.update_instances(a, Span<const f32>(flat.m, 16), {}));
  // Ids are recycled.
  CHECK(r.remove(b));
  CHECK(!r.remove(b));
  CHECK(r.allocate_id() == b);
  r.shutdown();
}

TEST(renderer_frame) {
  Renderer r;
  RendererOptions o;
  o.shadows = true;
  o.width = 640;
  o.height = 360;
  CHECK(r.init(o));
  const MeshData box = primitives::box({1, 1, 1});
  const u32 near_id = r.allocate_id(), far_id = r.allocate_id(), glass = r.allocate_id(), gun = r.allocate_id();
  MeshInput in = input_of(box);
  CHECK(r.upsert(near_id, in));
  const Mat4 behind = Mat4::translate({0, 500, 0});  // camera looks toward -y
  in.transforms = Span<const f32>(behind.m, 16);
  CHECK(r.upsert(far_id, in));
  in.transforms = {};
  in.material.alpha = AlphaMode::blend;
  CHECK(r.upsert(glass, in));
  in.material = Material{};
  in.material.viewmodel = true;
  CHECK(r.upsert(gun, in));
  r.camera.eye = {0, 10, 5};
  r.camera.target = {0, 0, 0};
  RenderStats s;
  const u64 uniforms_before = g_gl.uniform_calls;
  CHECK(r.render({}, s));
  const u64 first_frame_uniforms = g_gl.uniform_calls - uniforms_before;
  CHECK(s.draw_calls == 3);
  CHECK(s.culled_meshes == 1);
  CHECK(!s.shadow_reused);
  CHECK(s.shadow_draw_calls >= 1);
  const u64 before_second = g_gl.uniform_calls;
  CHECK(r.render({}, s));
  CHECK(s.shadow_reused);
  // The uniform cache sends only what changed (bones/arrays aside, nothing here).
  CHECK(g_gl.uniform_calls - before_second < first_frame_uniforms / 2);
  // Moving a caster redraws the shadow map.
  const Mat4 moved = Mat4::translate({1, 0, 0});
  CHECK(r.update_instances(near_id, Span<const f32>(moved.m, 16), {}));
  CHECK(r.render({}, s));
  CHECK(!s.shadow_reused);
  // Viewport must fit.
  RenderParams p;
  p.has_viewport = true;
  p.viewport = {0, 0, 700, 100};
  CHECK(!r.render(p, s));
  p.viewport = {0, 0, 320, 360};
  CHECK(r.render(p, s));
  // Picking through the renderer.
  PickHit hit;
  CHECK(r.pick(320, 180, {}, hit));
  CHECK(hit.mesh == near_id || hit.mesh == glass || hit.mesh == gun);
  r.shutdown();
}

TEST(renderer_lines_and_layers) {
  Renderer r;
  RendererOptions o;
  o.shadows = true;
  o.width = 640;
  o.height = 360;
  CHECK(r.init(o));
  const MeshData box = primitives::box({2, 2, 2});
  const u32 a = r.allocate_id(), b = r.allocate_id(), lines = r.allocate_id();
  MeshInput in = input_of(box);
  in.layers = 1;
  CHECK(r.upsert(a, in));
  in.layers = 2;
  const Mat4 right = Mat4::translate({4, 0, 0});
  in.transforms = Span<const f32>(right.m, 16);
  CHECK(r.upsert(b, in));
  // A line mesh: the box edges as index pairs, in layer 1.
  MeshData edges = box;
  edges.indices.clear();
  for (std::size_t t = 0; t + 2 < box.indices.size(); t += 3)
    for (int e = 0; e < 3; ++e) { edges.indices.push_back(box.indices[t + e]); edges.indices.push_back(box.indices[t + (e + 1) % 3]); }
  MeshInput li = input_of(edges);
  li.material.lines = true;
  li.layers = 1;
  CHECK(r.upsert(lines, li));
  r.camera.eye = {0, 12, 4};
  r.camera.target = {2, 0, 0};
  RenderStats s;
  RenderParams p;
  p.layers = 1;
  const u64 line_draws = g_gl.line_draws;
  CHECK(r.render(p, s));
  CHECK(s.draw_calls == 2);                       // box a + its lines; b is in layer 2
  CHECK(g_gl.line_draws - line_draws == 1);       // drawn as GL_LINES
  CHECK(s.shadow_draw_calls == 1);                // lines cast no shadow
  p.layers = 2;
  CHECK(r.render(p, s));
  CHECK(s.draw_calls == 1);
  CHECK(!s.shadow_reused);                        // another layer mask, another shadow map
  p.layers = 3;
  CHECK(r.render(p, s));
  CHECK(s.draw_calls == 3);
  // Picking honours the layer mask and never returns line meshes.
  PickHit hit;
  PickQuery q;
  q.layers = 2;
  const Ray to_a{r.camera.eye, normalize(Vec3{0, 0, 0} - r.camera.eye)};
  CHECK(!r.pick_ray(to_a, q, hit) || hit.mesh == b);
  q.layers = 1;
  CHECK(r.pick_ray(to_a, q, hit));
  CHECK(hit.mesh == a);
  r.shutdown();
}

TEST(renderer_context_loss) {
  Renderer r;
  CHECK(r.init({}));
  const MeshData box = primitives::box({1, 1, 1});
  for (int i = 0; i < 5; ++i) CHECK(r.upsert(r.allocate_id(), input_of(box)));
  std::vector<u8> pixels(4 * 4 * 4, 255);
  const u32 tex = r.create_texture({pixels.data(), 4, 4, true, false});
  CHECK(tex);
  r.context_lost();
  CHECK(r.lost());
  RenderStats s;
  CHECK(!r.render({}, s));
  CHECK(r.upsert(r.allocate_id(), input_of(box)));  // edits keep working while lost
  const u64 vaos = g_gl.vaos_created, textures = g_gl.textures_created;
  CHECK(r.context_restored());
  CHECK(g_gl.vaos_created == vaos + 6 + 1);  // six meshes + the sky pass
  CHECK(g_gl.textures_created == textures + 1);
  CHECK(r.render({}, s));
  CHECK(s.draw_calls == 6);
  r.shutdown();
}

TEST(particles_and_lights) {
  Renderer r;
  CHECK(r.init({}));
  Particles fx;
  CHECK(fx.init(r, 3));
  ParticleSpec s;
  s.position = {0, 0, 2};
  s.start_ms = 1000;
  s.life_ms = 200;
  CHECK(fx.emit(s) == 1);
  s.life_ms = 1000;
  s.grow = 1;
  CHECK(fx.emit(s) == 2);
  s.size = -1;
  CHECK(fx.emit(s) == 0);  // invalid
  s.size = 1;
  CHECK(fx.emit(s) == 3);
  CHECK(fx.emit(s) == 0);  // budget
  CHECK(fx.stats().dropped == 1);
  ParticleStats st = fx.tick(1100, {0, -10, 2});
  CHECK(st.active == 3);
  CHECK(r.mesh(fx.mesh_id()) && r.mesh(fx.mesh_id())->instance_count == 3);
  st = fx.tick(1300, {0, -10, 2});
  CHECK(st.active == 2 && st.expired == 1);
  CHECK(fx.tick(900, {0, -10, 2}).active == 2);  // time is monotonic
  st = fx.tick(5000, {0, -10, 2});
  CHECK(st.active == 0);
  CHECK(!r.mesh(fx.mesh_id())->visible);

  LightPool pool(2);
  CHECK(pool.request({1, 0, 0}, {1, 0, 0}, 10, 20));
  CHECK(pool.request({200, 0, 0}, {0, 1, 0}, 12, 20));
  CHECK(pool.request({2, 0, 0}, {0, 0, 1}, 4, 20));
  CHECK(!pool.request({NAN, 0, 0}, {1, 1, 1}, 1, 1));
  CHECK(!pool.request({0, 0, 0}, {1, 1, 1}, 0, 1));
  const PointLights& l = pool.resolve({0, 0, 0});
  CHECK(l.count == 2);
  CHECK(l.pos_range[0] == 1 && l.pos_range[4] == 2);  // near lights beat the distant bright one
  CHECK(pool.resolve({0, 0, 0}).count == 0);           // each frame owns only fresh requests
  CHECK(r.set_point_lights(l));
  r.shutdown();
}

// ---------------------------------------------------------------------------
// app runtime

// ---------------------------------------------------------------------------
// hardening (malformed streams, id ownership, instance updates, GPU state)

TEST(stream_guards) {
  u32 n = 7;
  CHECK(stream_count(0, 10, n) && n == 0);
  CHECK(stream_count(10, 10, n) && n == 10);
  CHECK(!stream_count(11, 10, n));
  CHECK(!stream_count(-1, 10, n));
  CHECK(!stream_count(2.5, 10, n));
  CHECK(!stream_count(NAN, 10, n));
  CHECK(!stream_count(INFINITY, 1e300, n));
  CHECK(stream_u32(-5) == 0 && stream_u32(1e12) == 0 && stream_u32(4294967295.0) == 4294967295u);

  // A ring count that would wrap a 32-bit `n * 2` must not read past the stream.
  BuildingOptions bo;
  bo.chunk_size = 320;
  const f64 huge_ring[] = {1, 7, 0, 10, 0, 0, 0, 1, 2147483648.0, 0, 0, 10, 0, 10, 10};
  BuildingBuilder b;
  CHECK(b.begin(huge_ring, sizeof huge_ring / sizeof *huge_ring, bo));
  while (!b.step(1e9)) {}
  CHECK(b.built() == 0);
  const f64 bad_count[] = {-3};
  CHECK(!b.begin(bad_count, 1, bo));
  const f64 nan_count[] = {NAN};
  CHECK(!b.begin(nan_count, 1, bo));
  const f64 fractional_rings[] = {1, 7, 0, 10, 0, 0, 0, 1.5, 3, 0, 0, 10, 0, 10, 10};
  CHECK(b.begin(fractional_rings, sizeof fractional_rings / sizeof *fractional_rings, bo));
  while (!b.step(1e9)) {}
  CHECK(b.built() == 0);

  RoadOptions ro;
  ro.radius = 1000;
  ro.palette = {{0.2, 0.2, 0.2}, {1, 1, 1}};
  HeightSampler flat;
  const f64 huge_road[] = {1, 10, 3, 0, 1, 2147483649.0, 0, 0, 100, 0};
  RoadBuilder r;
  CHECK(r.begin(huge_road, sizeof huge_road / sizeof *huge_road, ro, flat));
  while (!r.step(1e9)) {}
  CHECK(r.triangles() == 0);
  // Options that would make the dash loop or the segment split run away.
  RoadOptions no_period = ro;
  no_period.dash_period = 0;
  CHECK(!r.begin(huge_road, 10, no_period, flat));
  RoadOptions no_dash = ro;
  no_dash.dash_length = 0;
  CHECK(!r.begin(huge_road, 10, no_dash, flat));
  RoadOptions tiny_segments = ro;
  tiny_segments.segment_length = 1e-6;
  CHECK(!r.begin(huge_road, 10, tiny_segments, flat));
  RoadOptions nan_radius = ro;
  nan_radius.radius = NAN;
  CHECK(!r.begin(huge_road, 10, nan_radius, flat));
}

TEST(renderer_id_ownership) {
  Renderer r;
  CHECK(r.init({}));
  const MeshData boxd = primitives::box({1, 1, 1});
  const MeshInput box = input_of(boxd);
  CHECK(!r.upsert(1, box));  // never allocated
  const u32 a = r.allocate_id();
  CHECK(r.upsert(a, box));
  CHECK(r.remove(a));
  CHECK(!r.upsert(a, box));  // released by remove(): the old owner cannot resurrect it
  const u32 b = r.allocate_id();
  CHECK(b == a);
  r.release_id(b);
  r.release_id(b);  // double release must not queue the id twice
  const u32 c = r.allocate_id(), d = r.allocate_id();
  CHECK(c == b && d != c);
  r.release_id(d);
  CHECK(!r.id_allocated(d));
  CHECK(r.upsert(c, box));
  r.release_id(c);  // has a mesh: ignored
  CHECK(r.id_allocated(c) && r.mesh(c));
  r.shutdown();
}

TEST(renderer_instance_updates) {
  Renderer r;
  RendererOptions o;
  o.shadows = true;
  o.width = 320;
  o.height = 200;
  CHECK(r.init(o));
  const MeshData boxd = primitives::box({1, 1, 1});
  const u32 a = r.allocate_id();
  std::vector<f32> t;
  for (int i = 0; i < 3; ++i) { const Mat4 m = Mat4::translate({static_cast<f32>(i * 2), 0, 0}); t.insert(t.end(), m.m, m.m + 16); }
  MeshInput in = input_of(boxd);
  in.transforms = Span<const f32>(t.data(), t.size());
  CHECK(r.upsert(a, in));
  r.camera.eye = {0, 10, 5};
  r.camera.target = {0, 0, 0};
  RenderStats s;
  CHECK(r.render({}, s));
  CHECK(r.render({}, s) && s.shadow_reused);
  // Tint-only update: empty transforms keep the instances, upload one stream, keep the shadow map.
  const std::vector<f32> red = {1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1};
  const u64 uploads = g_gl.buffer_sub_data_calls;
  bool changed = false;
  CHECK(r.update_instances(a, {}, Span<const f32>(red.data(), red.size()), &changed) && changed);
  CHECK(r.mesh(a)->instance_count == 3);
  CHECK(r.mesh(a)->transforms.size() == 48 && r.mesh(a)->transforms[12] == 0 && r.mesh(a)->transforms[16 + 12] == 2);
  CHECK(r.mesh(a)->tints[4] == 1 && r.mesh(a)->tints[5] == 0);
  CHECK(g_gl.buffer_sub_data_calls == uploads + 1);
  CHECK(r.render({}, s) && s.shadow_reused);
  CHECK(r.update_instances(a, {}, Span<const f32>(red.data(), red.size()), &changed) && !changed);
  const std::vector<f32> bad_tint = {NAN, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1};
  CHECK(!r.update_instances(a, {}, Span<const f32>(bad_tint.data(), bad_tint.size())));
  CHECK(r.mesh(a)->tints[0] == 1);
  // Transform updates swap with scratch storage and rebuild bounds.
  t[12] = 40;
  CHECK(r.update_instances(a, Span<const f32>(t.data(), t.size()), Span<const f32>(red.data(), red.size()), &changed) && changed);
  CHECK(r.mesh(a)->world_bounds.center.x > 2);
  CHECK(r.render({}, s) && !s.shadow_reused);
  CHECK(!r.update_instances(a, Span<const f32>(t.data(), 15), {}));

  // A static mesh keeps (or regains) its picking BVH while its single instance is the identity.
  const MeshData sphere = primitives::sphere(1, 16, 12);
  const u32 st = r.allocate_id();
  CHECK(r.upsert(st, input_of(sphere)));
  CHECK(r.mesh(st)->bvh != nullptr);
  const Mat4 moved = Mat4::translate({0, 0, 3});
  CHECK(r.update_instances(st, Span<const f32>(moved.m, 16), {}));
  CHECK(r.mesh(st)->bvh == nullptr);
  const Mat4 identity = Mat4::identity();
  CHECK(r.update_instances(st, Span<const f32>(identity.m, 16), {}));
  CHECK(r.mesh(st)->bvh != nullptr);
  r.shutdown();
}

TEST(renderer_mirrored_instances) {
  Renderer r;
  RendererOptions o;
  o.shadows = true;
  o.width = 320;
  o.height = 200;
  CHECK(r.init(o));
  const MeshData boxd = primitives::box({1, 1, 1});
  const u32 a = r.allocate_id();
  const Mat4 mirror = Mat4::scale({-1, 1, 1});
  MeshInput in = input_of(boxd);
  in.transforms = Span<const f32>(mirror.m, 16);
  CHECK(r.upsert(a, in));
  CHECK(r.mesh(a)->mirror == 1 && r.mesh(a)->cull_mode() == 2);
  r.camera.eye = {0, 10, 5};
  r.camera.target = {0, 0, 0};
  RenderStats s;
  const u64 front = g_gl.cull_front;
  CHECK(r.render({}, s));
  CHECK(g_gl.cull_front >= front + 2);  // shadow pass and main pass
  std::vector<f32> mixed(mirror.m, mirror.m + 16);
  const Mat4 plain = Mat4::translate({3, 0, 0});
  mixed.insert(mixed.end(), plain.m, plain.m + 16);
  CHECK(r.update_instances(a, Span<const f32>(mixed.data(), mixed.size()), {}));
  CHECK(r.mesh(a)->mirror == 2 && r.mesh(a)->cull_mode() == 0);
  CHECK(r.update_instances(a, Span<const f32>(plain.m, 16), {}));
  CHECK(r.mesh(a)->mirror == 0 && r.mesh(a)->cull_mode() == 1);
  r.shutdown();
}

TEST(renderer_restore_failure_and_queries) {
  Renderer r;
  RendererOptions o;
  o.width = 64;
  o.height = 64;
  CHECK(r.init(o));
  const u32 a = r.allocate_id();
  const MeshData boxd = primitives::box({1, 1, 1});
  CHECK(r.upsert(a, input_of(boxd)));
  RenderStats s;
  const u64 queries = g_gl.queries_created;
  for (int i = 0; i < 20; ++i) CHECK(r.render({}, s));
  CHECK(g_gl.queries_created - queries <= 2);  // timer queries are recycled, not created per frame
  r.context_lost();
  g_gl.fail_link = true;
  CHECK(!r.context_restored());
  CHECK(r.lost());
  CHECK(!r.render({}, s));
  g_gl.fail_link = false;
  CHECK(r.context_restored());
  CHECK(!r.lost());
  CHECK(r.render({}, s) && s.draw_calls == 1);
  r.shutdown();
}

// ---------------------------------------------------------------------------
// primitives (capsule, torus) and physics (Box3D)

bool outward_winding(const MeshData& m, Vec3 centre_axis_scale) {
  // Every triangle's face normal must agree with its vertex normals.
  for (std::size_t t = 0; t + 2 < m.indices.size(); t += 3) {
    const Vec3 a = Vec3::load(&m.positions[m.indices[t] * 3]), b = Vec3::load(&m.positions[m.indices[t + 1] * 3]),
               c = Vec3::load(&m.positions[m.indices[t + 2] * 3]);
    const Vec3 fn = cross(b - a, c - a);
    if (length(fn) < 1e-9f) continue;
    const Vec3 vn = Vec3::load(&m.normals[m.indices[t] * 3]);
    if (dot(fn, vn) <= 0) return false;
  }
  (void)centre_axis_scale;
  return true;
}

TEST(primitives_capsule_torus) {
  const MeshData cap = primitives::capsule(0.5f, 2, 16, 8);
  CHECK(!cap.indices.empty() && cap.indices.size() % 3 == 0);
  for (u32 i : cap.indices) CHECK(i < cap.vertex_count());
  f32 zmin = 1e9f, zmax = -1e9f;
  for (u32 v = 0; v < cap.vertex_count(); ++v) { zmin = min_of(zmin, cap.positions[v * 3 + 2]); zmax = max_of(zmax, cap.positions[v * 3 + 2]); }
  CHECK_NEAR(zmin, -1.5f, 1e-5f);
  CHECK_NEAR(zmax, 1.5f, 1e-5f);
  CHECK(outward_winding(cap, {}));
  const MeshData tor = primitives::torus(1, 0.1f, 24, 8);
  CHECK(tor.indices.size() == 24 * 8 * 6);
  CHECK(outward_winding(tor, {}));
}

TEST(physics_world_bodies_and_events) {
  using namespace physics;
  World w;
  CHECK(w.create({}));
  BodySettings ground;
  ground.type = BodyType::fixed;
  ground.position = {0, 0, -0.5f};
  const Body g = w.create_body(ground);
  ShapeSettings box;
  box.size = {20, 20, 1};
  CHECK(w.create_shape(g, box).valid());
  BodySettings ball;
  ball.position = {0, 0, 3};
  ball.user = 42;
  const Body b = w.create_body(ball);
  ShapeSettings sphere;
  sphere.kind = ShapeKind::sphere;
  sphere.radius = 0.5f;
  sphere.material.user = 7;
  const Shape bs = w.create_shape(b, sphere);
  CHECK(bs.valid());
  CHECK(w.body_user(b) == 42 && w.shape_user(bs) == 7 && w.shape_body(bs) == b);
  // A sensor slab the ball falls through on its way down.
  BodySettings trigger;
  trigger.type = BodyType::fixed;
  trigger.position = {0, 0, 1.5f};
  const Body t = w.create_body(trigger);
  ShapeSettings slab;
  slab.size = {4, 4, 0.2f};
  slab.material.sensor = true;
  CHECK(w.create_shape(t, slab).valid());
  bool sensed = false, touched = false;
  for (int i = 0; i < 180; ++i) {
    w.step(1.0f / 60, 4);
    for (const TouchEvent& e : w.sensor_begin()) sensed |= e.body_b == b && e.body_a == t;
    for (const TouchEvent& e : w.contact_begin()) touched |= (e.body_a == b || e.body_b == b);
  }
  CHECK(sensed);
  CHECK(touched);
  CHECK_NEAR(w.pose(b).position.z, 0.5f, 0.05f);
  CHECK(w.step_count() == 180);
  CHECK(w.mass(b) > 0);
  // Ray straight down hits the resting ball first, sensors are ignored.
  const RayHit hit = w.cast_ray({0, 0, 10}, {0, 0, -20});
  CHECK(hit.hit && hit.body == b);
  CHECK_NEAR(hit.point.z, 1.0f, 0.06f);
  // Invalid input is rejected without touching the world.
  ShapeSettings bad;
  bad.kind = ShapeKind::sphere;
  bad.radius = -1;
  CHECK(!w.create_shape(b, bad).valid());
  ShapeSettings flat;
  flat.kind = ShapeKind::hull;
  const Vec3 coplanar[4] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 1, 0}};
  flat.points = Span<const Vec3>(coplanar, 4);
  CHECK(!w.create_shape(b, flat).valid());
  w.destroy_body(b);
  CHECK(!w.body_valid(b));
  CHECK(w.body_count() == 2);
  w.destroy();
  CHECK(!w.valid());
}

TEST(physics_joints_and_grab) {
  using namespace physics;
  World w;
  WorldSettings ws;
  ws.gravity = {0, 0, -10};
  CHECK(w.create(ws));
  const JointType types[] = {JointType::revolute, JointType::distance, JointType::prismatic, JointType::wheel, JointType::weld,
                             JointType::motor, JointType::spherical, JointType::parallel, JointType::filter};
  int made = 0;
  for (int k = 0; k < 9; ++k) {
    BodySettings a;
    a.type = BodyType::fixed;
    a.position = {static_cast<f32>(k) * 3, 0, 4};
    BodySettings bb;
    bb.position = {static_cast<f32>(k) * 3, 0, 2};
    const Body A = w.create_body(a), B = w.create_body(bb);
    ShapeSettings box;
    box.size = {0.5f, 0.5f, 0.5f};
    CHECK(w.create_shape(A, box).valid());
    CHECK(w.create_shape(B, box).valid());
    JointSettings j;
    j.type = types[k];
    j.body_a = A;
    j.body_b = B;
    j.frame_a = frame_from_axis({0, 0, -1}, {1, 0, 0}, types[k] == JointType::prismatic || types[k] == JointType::wheel);
    j.frame_b = frame_from_axis({0, 0, 1}, {1, 0, 0}, types[k] == JointType::prismatic || types[k] == JointType::wheel);
    j.length = 2;
    j.upper = 3;
    j.max_spring_force = 100;
    j.linear_hertz = 5;
    made += w.create_joint(j).valid();
  }
  CHECK(made == 9);
  JointSettings self;
  self.body_a = self.body_b = w.create_body({});
  CHECK(!w.create_joint(self).valid());
  // Distance joint keeps the hanging body 2 m below its anchor.
  for (int i = 0; i < 120; ++i) w.step(1.0f / 60, 4);
  // Grab a free body and lift it.
  BodySettings free_body;
  free_body.position = {0, 10, 0.5f};
  const Body fb = w.create_body(free_body);
  ShapeSettings sphere;
  sphere.kind = ShapeKind::sphere;
  sphere.radius = 0.5f;
  CHECK(w.create_shape(fb, sphere).valid());
  MouseGrab grab;
  CHECK(grab.begin(w, fb, {0, 10, 0.5f}));
  grab.set_target({0, 10, 3});
  for (int i = 0; i < 120; ++i) { grab.update(1.0f / 60); w.step(1.0f / 60, 4); }
  CHECK(w.pose(fb).position.z > 2.0f);
  grab.end();
  CHECK(!grab.active());
  // Static bodies cannot be grabbed.
  BodySettings fixed_body;
  fixed_body.type = BodyType::fixed;
  CHECK(!grab.begin(w, w.create_body(fixed_body), {}));
  // frame_from_axis maps local X / Z onto the axis.
  const Pose f = frame_from_axis({1, 2, 3}, {0, 1, 0}, true);
  const Vec3 x = f.rotation.rotate({1, 0, 0});
  CHECK_NEAR(x.y, 1.0f, 1e-5f);
  const Pose fz = frame_from_axis({}, {0, 0, -1}, false);
  CHECK_NEAR(fz.rotation.rotate({0, 0, 1}).z, -1.0f, 1e-5f);
  CHECK(physics::version().major >= 0);
}

TEST(physics_hull_mesh) {
  const Vec3 cube[8] = {{-1, -1, -1}, {1, -1, -1}, {1, 1, -1}, {-1, 1, -1}, {-1, -1, 1}, {1, -1, 1}, {1, 1, 1}, {-1, 1, 1}};
  MeshData m;
  CHECK(physics::hull_mesh(Span<const Vec3>(cube, 8), m));
  CHECK(m.vertex_count() == 36);  // 6 quads -> 12 triangles
  for (u32 t = 0; t < m.vertex_count(); t += 3) {
    const Vec3 a = Vec3::load(&m.positions[t * 3]), b = Vec3::load(&m.positions[t * 3 + 3]), c = Vec3::load(&m.positions[t * 3 + 6]);
    CHECK(dot(cross(b - a, c - a), Vec3::load(&m.normals[t * 3])) > 0);
    CHECK(dot((a + b + c) / 3.0f, Vec3::load(&m.normals[t * 3])) > 0);  // faces point away from the centre
  }
  const Vec3 pyramid[5] = {{-1, -1, -1}, {1, -1, -1}, {1, 1, -1}, {-1, 1, -1}, {0, 0, 1.6f}};
  CHECK(physics::hull_mesh(Span<const Vec3>(pyramid, 5), m));
  CHECK(m.vertex_count() == 18);  // base quad + 4 sides
  const Vec3 flat[4] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 1, 0}};
  CHECK(!physics::hull_mesh(Span<const Vec3>(flat, 4), m));
}

// ---------------------------------------------------------------------------
// JSON

TEST(json_numbers_match_javascript) {
  auto num = [](f64 v) { std::string s; json::append_number(s, v); return s; };
  CHECK(num(0.1) == "0.1");
  CHECK(num(-0.0) == "0");
  CHECK(num(1e21) == "1e+21");
  CHECK(num(1e20) == "100000000000000000000");
  CHECK(num(123456789012345680000.0) == "123456789012345680000");
  CHECK(num(1e-7) == "1e-7");
  CHECK(num(0.000001) == "0.000001");
  CHECK(num(1.5e-7) == "1.5e-7");
  CHECK(num(5e-324) == "5e-324");
  CHECK(num(9.80665) == "9.80665");
  CHECK(num(-9.80665) == "-9.80665");
  CHECK(num(100) == "100");
  CHECK(num(0.30000000000000004) == "0.30000000000000004");
  CHECK(num(4294967295.0) == "4294967295");
  CHECK(num(1.7976931348623157e308) == "1.7976931348623157e+308");
  CHECK(num(NAN) == "null");
}

TEST(json_parse_and_stringify) {
  json::Value v;
  std::string err;
  CHECK(json::parse(R"({"a":[1,2,{"b":null}],"s":"é😀\n","t":true,"n":-1.25e2})", v, &err));
  CHECK(v["a"].size() == 3 && v["a"][2]["b"].is_null());
  CHECK(v["s"].as_string() == "\xc3\xa9\xf0\x9f\x98\x80\n");
  CHECK(v["n"].as_number() == -125);
  CHECK(json::stringify(v) == R"({"a":[1,2,{"b":null}],"s":"é😀\n","t":true,"n":-125})");
  CHECK(json::stringify(v, 2) == "{\n  \"a\": [\n    1,\n    2,\n    {\n      \"b\": null\n    }\n  ],\n  \"s\": \"é😀\\n\",\n  \"t\": true,\n  \"n\": -125\n}");
  CHECK(json::stringify(json::Value::object(), 2) == "{}");
  CHECK(json::stringify(json::Value::array(), 2) == "[]");
  CHECK(!json::parse(R"({"__proto__":1})", v, &err));
  CHECK(err.find("unsafe") != std::string::npos);
  CHECK(!json::parse("[1,2]x", v, &err));
  CHECK(!json::parse("{\"a\":1e999}", v, &err));
  CHECK(!json::parse("[\"\x01\"]", v, &err));
  std::string deep(100, '[');
  deep += std::string(100, ']');
  CHECK(!json::parse(deep, v, &err));
  CHECK(json::parse("{\"k\":1,\"k\":2}", v) && v["k"].as_number() == 2 && v.size() == 1);
  json::Value o = json::Value::object();
  o.set("x", 1);
  o.set("y", 2);
  o.set("x", 3);  // replaced in place, order kept
  CHECK(json::stringify(o) == R"({"x":3,"y":2})");
  CHECK(json::Value(0.0).truthy() == false && json::Value("").truthy() == false && json::Value("a").truthy());
}

// ---------------------------------------------------------------------------
// scenes (.e3d) and the scene runtime

world::Body& add_test_body(world::Scene& s, world::BodyKind kind, world::D3 p, world::FixtureShape shape, world::D3 size) {
  world::Body b = world::new_body(kind, p, "body" + std::to_string(s.bodies.size()));
  world::Fixture f = world::new_fixture(shape, "fixture");
  f.size = size;
  f.radius = size[0];
  if (shape == world::FixtureShape::capsule) f.length = size[1];
  b.fixtures.push_back(f);
  s.bodies.push_back(b);
  return s.bodies.back();
}

TEST(scene_round_trip_and_validation) {
  world::seed_uids(1234);
  world::Scene s = world::new_scene("Test scene");
  add_test_body(s, world::BodyKind::fixed, {0, 0, -0.5}, world::FixtureShape::box, {10, 10, 1});
  world::Body& ball = add_test_body(s, world::BodyKind::dynamic, {0, 0, 2}, world::FixtureShape::sphere, {0.5, 0, 0});
  ball.game.role = world::GameRole::player;
  ball.game.control = world::GameControl::roll;
  ball.unknown.set("futureKey", "kept");
  world::Joint j;
  j.id = world::make_uid();
  j.name = "j";
  j.type = world::JointKind::distance;
  j.body_a = s.bodies[0].id;
  j.body_b = ball.id;
  j.damping_ratio = 0.25;
  s.joints.push_back(j);
  s.game.goal = world::GameGoal::collect_all;
  const std::string text = world::to_text(s);
  world::Scene back;
  std::string err;
  CHECK(world::from_text(text, back, &err, "copy.e3d"));
  CHECK(back.name == "copy");
  CHECK(back.id != s.id);  // a fresh document id, as the editor does
  back.id = s.id;
  back.name = s.name;
  CHECK(world::to_text(back) == text);
  CHECK(back.bodies[1].game.control == world::GameControl::roll);
  CHECK(back.bodies[1].unknown["futureKey"].as_string() == "kept");
  CHECK(back.joints[0].damping_ratio && *back.joints[0].damping_ratio == 0.25);
  CHECK(!back.joints[0].spring_damping_ratio);
  CHECK(world::validate(back).empty());
  // Validation messages of the previous editor.
  world::Scene bad = back;
  bad.bodies[1].fixtures[0].radius = 0;
  bad.joints[0].body_b = bad.joints[0].body_a;
  bad.world.sub_step_count = 0;
  bad.bodies.push_back(bad.bodies[0]);
  const std::vector<std::string> errors = world::validate(bad);
  auto has = [&](const char* text) { for (const std::string& e : errors) if (e.find(text) != std::string::npos) return true; return false; };
  CHECK(has("Missing / duplicate ID"));
  CHECK(has("radius must be positive"));
  CHECK(has("invalid body connection"));
  CHECK(has("Substeps must be 1"));
  // Not a scene.
  CHECK(!world::from_text(R"({"format":"other"})", back, &err));
  CHECK(err == "Expected Editor3D .e3d / exported JSON");
  // Unsupported joint types open but cannot run.
  CHECK(world::from_text(R"({"format":"editor3d","version":2,"name":"x","bodies":[],"joints":[{"id":"q","name":"odd","type":"gear"}]})", back, &err));
  CHECK(!world::validate(back).empty());
  CHECK(world::to_text(back).find("__unsupported") == std::string::npos);
}

TEST(scene_legacy_migration) {
  world::Scene s;
  std::string err;
  bool migrated = false;
  CHECK(world::from_text(R"({"format":"kurzlernen-editor3d","objects":[{"type":"static","shape":"box","size":[4,4,1],"position":[0,0,-0.5]},{"name":"Ball","shape":"sphere","radius":0.3,"position":[0,0,2],"restitution":2}]})",
                         s, &err, "old.json", &migrated));
  CHECK(migrated);
  CHECK(s.name == "old");
  CHECK(s.bodies.size() == 2);
  CHECK(s.bodies[0].type == world::BodyKind::fixed && s.bodies[0].name == "Body 1");
  CHECK(s.bodies[1].fixtures[0].shape == world::FixtureShape::sphere && s.bodies[1].fixtures[0].radius == 0.3);
  CHECK(s.bodies[1].fixtures[0].restitution == 1);
  CHECK(world::validate(s).empty());
}

TEST(scene_runtime_physics_and_joints) {
  world::seed_uids(99);
  world::Scene s = world::new_scene("Joints");
  add_test_body(s, world::BodyKind::fixed, {0, 0, -0.5}, world::FixtureShape::box, {40, 40, 1});
  for (u32 k = 0; k < world::kJointKindCount; ++k) {
    world::Body& a = add_test_body(s, world::BodyKind::fixed, {k * 3.0, 0, 4}, world::FixtureShape::box, {0.4, 0.4, 0.4});
    const std::string aid = a.id;
    world::Body& b = add_test_body(s, world::BodyKind::dynamic, {k * 3.0, 0, 2}, world::FixtureShape::box, {0.7, 0.7, 1.5});
    world::Joint j;
    j.id = world::make_uid();
    j.name = std::string(world::joint_kind_name(static_cast<world::JointKind>(k)));
    j.type = static_cast<world::JointKind>(k);
    j.body_a = aid;
    j.body_b = b.id;
    j.anchor_a = {0, 0, -0.2};
    j.anchor_b = {0, 0, 1.8};
    j.length = 2;
    j.lower_limit = -0.7;
    j.upper_limit = 0.7;
    s.joints.push_back(j);
  }
  world::Body& hull = add_test_body(s, world::BodyKind::dynamic, {-4, 0, 3}, world::FixtureShape::hull, {1, 1, 1});
  hull.fixtures[0].vertices = {{-1, -1, -1}, {1, -1, -1}, {1, 1, -1}, {-1, 1, -1}, {0, 0, 1.6}};
  add_test_body(s, world::BodyKind::dynamic, {-8, 0, 3}, world::FixtureShape::capsule, {0.3, 1, 0});
  world::SceneRuntime rt;
  CHECK(rt.build(s));
  CHECK(rt.world().body_count() == s.bodies.size());
  for (int i = 0; i < 240; ++i) rt.step_once();
  world::DPose p;
  CHECK(rt.body_pose(hull.id, p));
  CHECK(p.position[2] > 0 && p.position[2] < 1.7);  // the hull rests on the ground
  // advance() takes fixed steps from real time.
  const u64 before = rt.steps();
  CHECK(rt.advance(0.05) == 3);
  CHECK(rt.steps() == before + 3);
  // A flat hull makes the build fail with the previous editor's message.
  world::Scene flat = s;
  flat.bodies.back().fixtures[0].shape = world::FixtureShape::hull;
  flat.bodies.back().fixtures[0].vertices = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 1, 0}};
  world::SceneRuntime bad;
  CHECK(!bad.build(flat));
  CHECK(bad.error().find("Box3D rejected hull") != std::string::npos);
}

TEST(scene_runtime_game_components) {
  world::seed_uids(7);
  world::Scene s = world::new_scene("Game");
  add_test_body(s, world::BodyKind::fixed, {0, 0, -0.5}, world::FixtureShape::box, {60, 8, 1});
  world::Body& player = add_test_body(s, world::BodyKind::dynamic, {0, 0, 0.5}, world::FixtureShape::sphere, {0.5, 0, 0});
  player.game.role = world::GameRole::player;
  player.game.control = world::GameControl::roll;
  player.game.force = 15;
  player.game.jump = 5;
  const std::string pid = player.id;
  world::Body& coin = add_test_body(s, world::BodyKind::fixed, {4, 0, 0.5}, world::FixtureShape::box, {0.6, 6, 1});
  coin.game.role = world::GameRole::pickup;
  coin.game.points = 5;
  coin.fixtures[0].sensor = true;
  const std::string cid = coin.id;
  world::Body& goal = add_test_body(s, world::BodyKind::fixed, {9, 0, 0.5}, world::FixtureShape::box, {0.6, 6, 1});
  goal.game.role = world::GameRole::goal;
  goal.fixtures[0].sensor = true;
  s.game.goal = world::GameGoal::collect_all;
  world::SceneRuntime rt;
  CHECK(rt.build(s));
  CHECK(rt.game().status == world::GameStatus::playing);
  CHECK(rt.game().player == pid && rt.game().camera_body == pid);
  CHECK(rt.game().pickups_total == 1);
  world::GameInput in;
  in.camera_forward = {1, 0, 0};  // forward = +X
  in.move_y = 1;
  rt.set_input(in);
  for (int i = 0; i < 600 && rt.game().status == world::GameStatus::playing; ++i) rt.step_once();
  CHECK(rt.game().pickups_collected == 1);
  CHECK(rt.game().score == 5);
  CHECK(!rt.body_alive(cid));
  CHECK(rt.game().status == world::GameStatus::won);
  CHECK(rt.game().message == "Goal reached!");  // collect all, then the goal
  {
    // Without a goal body, collecting everything wins at once.
    world::Scene nogoal = s;
    nogoal.bodies.pop_back();
    world::SceneRuntime r3;
    CHECK(r3.build(nogoal));
    r3.set_input(in);
    for (int i = 0; i < 600 && r3.game().status == world::GameStatus::playing; ++i) r3.step_once();
    CHECK(r3.game().status == world::GameStatus::won);
    CHECK(r3.game().message == "All collected!");
  }
  // Jumping lifts a grounded player once per key press.
  CHECK(rt.restart());
  for (int i = 0; i < 30; ++i) rt.step_once();
  world::GameInput jump;
  jump.jump = true;
  rt.set_input(jump);
  rt.step_once();
  CHECK(rt.world().linear_velocity(rt.physics_body(pid)).z > 3);
  // Falling below the limit loses.
  world::Scene fall = s;
  fall.bodies[0].position = {0, 0, -100};
  fall.game.fall_limit = -5;
  world::SceneRuntime rt2;
  CHECK(rt2.build(fall));
  for (int i = 0; i < 400 && rt2.game().status == world::GameStatus::playing; ++i) rt2.step_once();
  CHECK(rt2.game().status == world::GameStatus::lost);
  CHECK(rt2.game().message == "Fell off the world");
  // Grabbing works on dynamic bodies only.
  CHECK(rt.begin_grab(pid, world::to_f(world::D3{0, 0, 0.5})));
  rt.end_grab();
  CHECK(!rt.begin_grab(s.bodies[0].id, {}));
}

// ---------------------------------------------------------------------------
// editor (C++ core against the GL stand-in)

struct EditorFixture {
  Renderer renderer;
  editor::Editor ed;
  EditorFixture() {
    world::seed_uids(4242);
    RendererOptions o;
    o.width = 800;
    o.height = 600;
    o.shadows = true;
    renderer.init(o);
    ed.init(&renderer);
  }
  ~EditorFixture() { ed.shutdown(); renderer.shutdown(); }
  editor::View& place(editor::View& v) {
    ed.set_view_rect(v.id, 0, 0, 800, 600, 800, 600, 1, true);
    return v;
  }
  json::Value run(const char* text) {
    json::Value c;
    json::parse(text, c);
    return ed.command(c);
  }
};

TEST(editor_documents_history_and_operations) {
  EditorFixture f;
  editor::Editor& ed = f.ed;
  ed.load_demo("mechanism");
  CHECK(ed.documents().size() == 1 && ed.views().size() == 1);
  world::Scene& s = *ed.scene();
  CHECK(s.name == "Suspension test");
  CHECK(s.bodies.size() == 1 + 1 + 1 + 4 + 2 + 6);
  CHECK(s.joints.size() == 5);
  CHECK(world::validate(s).empty());
  f.place(*ed.active_view());
  ed.frame(0);
  ed.frame(16);
  // Select the chassis and move it 2 m along X by typing.
  const std::string chassis = s.bodies[2].id;
  ed.set_mode(editor::Mode::body);
  ed.set_selection({chassis});
  const world::D3 before = s.body(chassis)->position;
  ed.begin_operation(editor::OperationKind::translate);
  CHECK(ed.operation() != nullptr);
  ed.set_operation_axis('x');
  ed.type_operation("2");
  CHECK_NEAR(ed.scene()->body(chassis)->position[0], before[0] + 2, 1e-9);
  // Wheel joint anchors on the chassis move with it (they are body-local).
  ed.finish_operation(true);
  CHECK(ed.doc()->history.back().label == "Translate body");
  ed.undo();
  CHECK_NEAR(ed.scene()->body(chassis)->position[0], before[0], 1e-9);
  ed.redo();
  CHECK_NEAR(ed.scene()->body(chassis)->position[0], before[0] + 2, 1e-9);
  // Rotate 90 degrees about Z around the selection centre.
  ed.begin_operation(editor::OperationKind::rotate);
  ed.type_operation("90");
  ed.finish_operation(true);
  const world::DQ q = ed.scene()->body(chassis)->rotation;
  CHECK_NEAR(std::fabs(q[2]), std::sqrt(0.5), 1e-9);
  // Scale a box fixture by 2 (fixture mode).
  ed.set_mode(editor::Mode::fixture);
  const std::string fid = ed.scene()->body(chassis)->fixtures[0].id;
  ed.set_selection({fid});
  ed.begin_operation(editor::OperationKind::scale);
  ed.type_operation("2");
  ed.finish_operation(true);
  CHECK_NEAR(ed.scene()->fixture(fid)->size[0], 6.4, 1e-9);
  // Esc semantics: cancelling restores.
  ed.begin_operation(editor::OperationKind::scale);
  ed.type_operation("3");
  ed.finish_operation(false);
  CHECK_NEAR(ed.scene()->fixture(fid)->size[0], 6.4, 1e-9);
  // Cursor operation and "cursor to selection".
  ed.cursor_to_selection();
  CHECK(world::length(world::sub(ed.scene()->cursor, world::local_to_world(ed.scene()->body(chassis)->pose(), ed.scene()->fixture(fid)->center))) < 1e-9);
  // Edits that would break the scene are rolled back.
  const std::size_t entries = ed.doc()->history.size();
  CHECK(!ed.set_property("radius", json::Value(-1.0)) || ed.scene()->fixture(fid)->shape == world::FixtureShape::box);
  ed.set_mode(editor::Mode::body);
  ed.set_selection({chassis});
  CHECK(ed.set_property("position.2", json::Value(5.0)));
  CHECK(ed.scene()->body(chassis)->position[2] == 5);
  CHECK(ed.set_property("euler.2", json::Value(45.0)));
  CHECK_NEAR(world::rad2deg_test(ed.scene()->body(chassis)->rotation), 45, 1e-6);
  CHECK(!ed.set_property("position.1", json::Value("abc")));
  CHECK(ed.doc()->history.size() >= entries + 2);
  // History jump to the initial state.
  ed.jump_history(-1);
  CHECK(ed.doc()->index == 0);
  CHECK_NEAR(ed.scene()->body(chassis)->position[0], before[0], 1e-9);
}

TEST(editor_commands_dialogs_and_player) {
  EditorFixture f;
  editor::Editor& ed = f.ed;
  ed.add_document(world::new_scene("Blank"));
  f.place(*ed.active_view());
  // Add two bodies with fixtures, join them, duplicate, copy/paste, delete.
  ed.add_body(world::BodyKind::fixed, "box");
  ed.set_cursor({0, 0, 3});
  ed.add_body(world::BodyKind::dynamic, "sphere");
  world::Scene& s = *ed.scene();
  CHECK(s.bodies.size() == 2 && s.bodies[1].fixtures[0].shape == world::FixtureShape::sphere);
  CHECK(s.bodies[1].position[2] == 3);
  ed.set_selection({s.bodies[0].id, s.bodies[1].id});
  ed.add_joint(world::JointKind::revolute);
  CHECK(ed.scene()->joints.size() == 1);
  CHECK(ed.mode() == editor::Mode::joint);
  ed.set_mode(editor::Mode::body);
  ed.select_all();
  ed.duplicate();
  ed.finish_operation(true);
  CHECK(ed.scene()->bodies.size() == 4 && ed.scene()->joints.size() == 2);
  ed.copy_selection();
  ed.paste();
  ed.finish_operation(false);
  CHECK(ed.scene()->bodies.size() == 6);
  ed.delete_selected();
  CHECK(ed.scene()->bodies.size() == 4 && ed.scene()->joints.size() == 2);
  // Make chain through the dialog protocol.
  ed.set_selection({ed.scene()->bodies[1].id});
  json::Value r = f.run(R"({"cmd":"dialogSubmit","name":"chain","values":{"count":"4","x":"1","y":"0","z":"0","joint":"distance"}})");
  CHECK(r["ok"].truthy());
  CHECK(ed.scene()->bodies.size() == 7 && ed.scene()->joints.size() == 5);
  r = f.run(R"({"cmd":"dialogSubmit","name":"chain","values":{"count":"1","x":"1","y":"0","z":"0","joint":"distance"}})");
  CHECK(!r["ok"].truthy() && r["error"].as_string() == "Invalid chain settings");
  // Custom properties with typed validation.
  r = f.run(R"({"cmd":"dialogSubmit","name":"custom","values":{"props":[{"name":"hp","type":"int","value":"3"},{"name":"tint","type":"color","value":"[255,0,0,255]"}]}})");
  CHECK(r["ok"].truthy());
  CHECK(ed.scene()->body(ed.doc()->selection[editor::Mode::body][0])->custom.size() == 2);
  r = f.run(R"({"cmd":"dialogSubmit","name":"custom","values":{"props":[{"name":"bad","type":"int","value":"1.5"}]}})");
  CHECK(!r["ok"].truthy());
  // Menus and dialogs are JSON.
  r = f.run(R"({"cmd":"menu","name":"File"})");
  CHECK(r["entries"].size() > 8);
  r = f.run(R"({"cmd":"dialog","name":"keyboard"})");
  CHECK(r["ui"][0]["dialog"]["title"].as_string() == "Keyboard reference");
  // Save produces a download with a parseable scene.
  r = f.run(R"({"cmd":"saveScene"})");
  world::Scene back;
  CHECK(world::from_text(r["ui"][0]["text"].as_string(), back, nullptr, "x"));
  CHECK(back.bodies.size() == 7);
  // State and status describe the panels.
  json::Value st = ed.state();
  CHECK(st["tree"]["rows"].size() >= 7);
  CHECK(st["properties"]["fields"].size() > 10);
  CHECK(st["history"]["entries"].size() > 5);
  CHECK(ed.status()["views"].size() == 1);
  // The player runs the scene with Box3D and draws it.
  CHECK(ed.open_player());
  editor::View& pv = f.place(*ed.active_view());
  CHECK(pv.type == editor::ViewType::player && pv.runtime && pv.runtime->valid());
  for (int i = 0; i < 30; ++i) ed.frame(i * 16.0);
  CHECK(pv.runtime->steps() > 20);
  CHECK(g_gl.draws > 0);
  // Keyboard: Space pauses / single-steps the player.
  ed.key(" ", "Space", true, false, false, false, false);
  CHECK(!pv.playing);
  const u64 steps = pv.runtime->steps();
  ed.key(" ", "Space", true, false, false, false, false);
  CHECK(pv.runtime->steps() == steps + 1);
  ed.close_view(&pv);
  CHECK(ed.views().size() == 1);
  // Validation failure keeps the player closed and shows the problems.
  ed.scene()->world.sub_step_count = 0;
  CHECK(!ed.open_player());
  ed.scene()->world.sub_step_count = 4;
  // Autosave + restore.
  ed.frame(10000);
  const std::string saved = ed.take_autosave(20000);
  CHECK(!saved.empty());
  EditorFixture g;
  json::Value restore = json::Value::object();
  restore.set("cmd", "restoreWorkspace");
  restore.set("text", saved);
  CHECK(g.ed.command(restore)["ok"].truthy());
  CHECK(g.ed.documents().size() == 1 && g.ed.scene()->bodies.size() == 7);
}

TEST(editor_pointer_keys_and_picking) {
  EditorFixture f;
  editor::Editor& ed = f.ed;
  ed.load_demo("hulls");
  editor::View& v = f.place(*ed.active_view());
  ed.frame(0);
  // Look straight down at the crate stack and click its centre: a crate gets selected.
  v.camera.target = {0, 0, 0};
  v.camera.set_eye({0.05f, -0.05f, 30});
  ed.set_mode(editor::Mode::body);
  ed.set_selection({});
  ed.frame(16);
  f32 sx = 0, sy = 0;
  // Project the second crate's position.
  const world::Body& crate = ed.scene()->bodies[3];
  {
    const Camera c = v.camera.camera();
    bool ok = false;
    const Mat4 vm = Mat4::look_at(c.eye, c.target, c.up, &ok);
    const Mat4 vp = Mat4::perspective(c.fov, 800.0f / 600.0f, c.near, c.far) * vm;
    const Vec3 p = world::to_f(crate.position);
    const f32 cx = vp.m[0] * p.x + vp.m[4] * p.y + vp.m[8] * p.z + vp.m[12], cy = vp.m[1] * p.x + vp.m[5] * p.y + vp.m[9] * p.z + vp.m[13];
    const f32 cw = vp.m[3] * p.x + vp.m[7] * p.y + vp.m[11] * p.z + vp.m[15];
    sx = (cx / cw + 1) * 400;
    sy = (1 - cy / cw) * 300;
  }
  ed.pointer(v.id, "down", sx, sy, 0, 1, 1, "mouse", false, false, false, 0);
  ed.pointer(v.id, "up", sx, sy, 0, 0, 1, "mouse", false, false, false, 0);
  CHECK(ed.doc()->selection[editor::Mode::body].size() == 1);
  // Box selection over the whole view selects every body.
  ed.pointer(v.id, "down", 2, 2, 0, 1, 2, "mouse", false, false, false, 0);
  // (the click may land on geometry; start the box on empty space)
  ed.set_selection({});
  ed.pointer(v.id, "up", 2, 2, 0, 0, 2, "mouse", false, false, false, 0);
  editor::SelectBox box{true, 0, 0, 800, 600, 3, false, false};
  v.box = box;
  ed.pointer(v.id, "up", 800, 600, 0, 0, 3, "mouse", false, false, false, 0);
  CHECK(ed.doc()->selection[editor::Mode::body].size() == ed.scene()->bodies.size());
  // Mouse-driven translate: first move sets the start, the next moves apply, a click confirms.
  ed.set_selection({crate.id});
  const world::D3 p0 = ed.scene()->body(crate.id)->position;
  ed.key("t", "KeyT", true, false, false, false, false);
  CHECK(ed.operation() && ed.operation()->kind == editor::OperationKind::translate);
  ed.pointer(v.id, "move", 400, 300, -1, 0, 1, "mouse", false, false, false, 0);
  ed.pointer(v.id, "move", 460, 300, -1, 0, 1, "mouse", false, false, false, 0);
  ed.pointer(v.id, "down", 460, 300, 0, 1, 1, "mouse", false, false, false, 0);
  CHECK(!ed.operation());
  CHECK(ed.scene()->body(crate.id)->position[0] > p0[0] + 0.5);
  // Keys: modes, undo, action popup, F1.
  ed.key("f", "KeyF", true, false, false, false, false);
  CHECK(ed.mode() == editor::Mode::fixture);
  ed.key("z", "KeyZ", true, false, true, false, false);
  CHECK_NEAR(ed.scene()->body(crate.id)->position[0], p0[0], 1e-9);
  json::Value k = ed.key(" ", "Space", true, false, false, false, false);
  CHECK(k["ui"][0]["action"].as_string() == "popup");
  k = ed.key("F1", "F1", true, false, false, false, false);
  CHECK(k["ui"][0]["dialog"]["name"].as_string() == "keyboard");
  // Orbit with the right mouse button changes the camera.
  const f32 theta = v.camera.theta;
  ed.pointer(v.id, "down", 100, 100, 2, 2, 9, "mouse", false, false, false, 0);
  ed.pointer(v.id, "move", 160, 100, -1, 2, 9, "mouse", false, false, false, 0);
  for (int i = 0; i < 60; ++i) ed.frame(100 + i * 16.0);
  CHECK(v.camera.theta != theta);
  ed.pointer(v.id, "wheel", 0, 0, 0, 0, 9, "mouse", false, false, false, 120);
  // Grab in a player view lifts a dynamic body.
  CHECK(ed.open_player());
  editor::View& pv = f.place(*ed.active_view());
  pv.camera = v.camera;
  for (int i = 0; i < 5; ++i) ed.frame(2000 + i * 16.0);
  ed.pointer(pv.id, "down", sx, sy, 0, 1, 5, "mouse", false, false, false, 0);
  CHECK(pv.runtime->grabbing());
  ed.pointer(pv.id, "move", sx, sy - 80, -1, 1, 5, "mouse", false, false, false, 0);
  for (int i = 0; i < 30; ++i) ed.frame(2100 + i * 16.0);
  ed.pointer(pv.id, "up", sx, sy - 80, 0, 0, 5, "mouse", false, false, false, 0);
  CHECK(!pv.runtime->grabbing());
}

TEST(editor_game_demo_and_images) {
  EditorFixture f;
  editor::Editor& ed = f.ed;
  // All demos validate and run.
  for (const char* demo : {"mechanism", "cradle", "hulls", "joints", "game"}) {
    ed.load_demo(demo);
    CHECK(world::validate(*ed.scene()).empty());
    CHECK(ed.open_player());
    editor::View& pv = f.place(*ed.active_view());
    for (int i = 0; i < 10; ++i) ed.frame(i * 16.0);
    CHECK(pv.runtime->steps() > 0);
    if (std::string(demo) == "game") {
      CHECK(pv.runtime->game().status == world::GameStatus::playing);
      CHECK(pv.runtime->game().pickups_total == 6);
      // Game keys go to the runtime.
      ed.key("w", "KeyW", true, false, false, false, false);
      CHECK(pv.game_input.move_y == 1);
      ed.key("w", "KeyW", false, false, false, false, false);
      CHECK(pv.game_input.move_y == 0);
      json::Value hud = ed.status()["views"][ed.views().size() - 1]["hud"];
      CHECK(!hud.is_null());
      CHECK(hud["player"].size() == 3);
      // Touch input through the command API: clamped, partial updates.
      json::Value gi = json::Value::object();
      gi.set("cmd", "gameInput");
      gi.set("view", pv.id);
      gi.set("moveX", 3.0);
      gi.set("moveY", -0.5);
      CHECK(ed.command(gi)["ok"].truthy());
      CHECK(pv.game_input.move_x == 1.0f && pv.game_input.move_y == -0.5f);
      json::Value gj = json::Value::object();
      gj.set("cmd", "gameInput");
      gj.set("view", pv.id);
      gj.set("jump", true);
      CHECK(ed.command(gj)["ok"].truthy());
      CHECK(pv.game_input.jump && pv.game_input.move_x == 1.0f);
      for (int i = 10; i < 40; ++i) ed.frame(i * 16.0);
      CHECK(pv.runtime->game().elapsed > 0.2);
      json::Value rs = json::Value::object();
      rs.set("cmd", "restartGame");
      rs.set("view", pv.id);
      CHECK(ed.command(rs)["ok"].truthy());
      CHECK(pv.runtime->game().elapsed == 0);
      // Neither command applies to editor views.
      json::Value bad = json::Value::object();
      bad.set("cmd", "gameInput");
      bad.set("view", ed.views().front()->id);
      CHECK(!ed.command(bad)["ok"].truthy());
    }
    ed.close_view(&pv);
  }
  // View limit: layers stay unique, nothing modified is closed.
  {
    ed.add_document(world::new_scene("Keep"));
    ed.add_body(world::BodyKind::dynamic, "box");  // dirty document
    const u32 keep = ed.active_view()->doc;
    for (int i = 0; i < 40; ++i) ed.add_document(world::new_scene("Many " + std::to_string(i)));
    CHECK(ed.views().size() <= editor::Editor::kMaxViews);
    u32 used = 0;
    bool unique = true, kept = false;
    for (const auto& v : ed.views()) {
      unique = unique && !(used & (1u << v->layer));
      used |= 1u << v->layer;
      kept = kept || v->doc == keep;
    }
    CHECK(unique);
    CHECK(kept);
  }
  // loadDocument reports parse failures as errors.
  {
    json::Value ld = json::Value::object();
    ld.set("cmd", "loadDocument");
    ld.set("text", "{not json");
    ld.set("name", "broken.e3d");
    CHECK(!ed.command(ld)["ok"].truthy());
  }
  // Images: data URL in, pixels from the page, alpha tracing into fixtures.
  ed.add_document(world::new_scene("Images"));
  CHECK(ed.add_image("dot.png", "data:image/png;base64,AAAA", 8, 8));
  json::Value pending = ed.pending_images();
  CHECK(pending.size() == 1);
  std::vector<u8> px(8 * 8 * 4, 0);
  for (int y = 2; y < 6; ++y)
    for (int x = 1; x < 7; ++x) px[(y * 8 + x) * 4 + 3] = 255;
  CHECK(ed.set_image_pixels(pending[0].as_string(), 8, 8, px.data()));
  CHECK(*ed.image_source(pending[0].as_string()) == "data:image/png;base64,AAAA");
  CHECK(ed.trace_image_to_boxes());
  world::Scene& s = *ed.scene();
  CHECK(s.bodies.size() == 1);
  CHECK(s.bodies[0].fixtures.size() == 1);  // one merged rectangle
  CHECK_NEAR(s.bodies[0].fixtures[0].size[0], 1.5, 1e-9);  // 6 of 8 px across a 2 m plane
  CHECK(!ed.add_image("x.txt", "data:text/plain,hi", 1, 1));
  // A sampler covering the image composes and traces it too.
  ed.add_sampler();
  CHECK(ed.sampler_action("trace"));
  std::vector<u8> out;
  i32 w = 0, h = 0;
  CHECK(ed.sampler_action("sobel", &out, &w, &h));
  CHECK(w == 200 && h == 200 && out.size() == static_cast<std::size_t>(w * h * 4));
  // Export game action carries the scene.
  ed.load_demo("game");
  json::Value r = f.run(R"({"cmd":"exportGame"})");
  CHECK(r["ui"][0]["action"].as_string() == "exportGame");
  CHECK(r["ui"][0]["scene"].as_string().find("\"role\":\"player\"") != std::string::npos);
}

TEST(app_input_and_args) {
  Input in;
  in.key(Key::w, true);
  CHECK(in.down(Key::w) && in.pressed(Key::w));
  in.end_frame();
  CHECK(in.down(Key::w) && !in.pressed(Key::w));
  in.key(Key::w, false);
  CHECK(!in.down(Key::w) && in.released(Key::w));
  in.key(Key::a, true);
  in.set_focus(false);
  CHECK(!in.down(Key::a));
  in.touch(7, 0, {10, 10});
  in.touch(8, 0, {20, 20});
  CHECK(in.touch_count() == 2);
  in.touch(7, 1, {15, 12});
  CHECK(in.touches()[0].position.x == 15 && in.touches()[0].start.x == 10);
  in.touch(7, 2, {15, 12});
  CHECK(in.touch_count() == 1);

  Renderer r;
  Engine e(r);
  const char* q = "?time=0.6&name=Orb%20Run&flag&x=+1";
  e.set_args(q, static_cast<u32>(std::strlen(q)));
  CHECK(e.arg_number("time", 0) == 0.6);
  CHECK(std::strcmp(e.arg("name"), "Orb Run") == 0);
  CHECK(e.arg("flag") && !*e.arg("flag"));
  CHECK(e.arg_number("missing", 3) == 3);
  CHECK(e.arg_number("name", 4) == 4);
}

}  // namespace

// The app runtime is linked into the test binary; it needs an app factory.
App* kern_create_app() { return nullptr; }

int main() {
  for (const Test& t : registry()) {
    const int before = g_failures;
    t.fn();
    std::printf("%s %s\n", g_failures == before ? "ok  " : "FAIL", t.name);
  }
  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
