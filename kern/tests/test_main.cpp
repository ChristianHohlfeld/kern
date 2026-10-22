// Kern engine unit tests (host build, see kern/tests/run.sh).
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "gl_stub.h"
#include "kern/kern.h"
#include "kern/physics/physics.h"
#include "kern/core/json.h"
#include "kern/game/demos.h"
#include "kern/game/runtime.h"
#include "kern/studio/studio.h"
#include "kern/net/client_server.h"
#include "kern/game/net_game.h"

#include <algorithm>
#include <memory>

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
// renderer: orthographic cameras

TEST(renderer_orthographic_camera) {
  Renderer r;
  RendererOptions o;
  o.width = 400;
  o.height = 200;
  CHECK(r.init(o));
  const MeshData box = primitives::box({2, 2, 2});
  const u32 id = r.allocate_id();
  CHECK(r.upsert(id, input_of(box)));
  r.camera.orthographic = true;
  r.camera.ortho_height = 10;
  r.camera.eye = {0, 0, 50};
  r.camera.target = {0, 0, 0};
  r.camera.up = {0, 1, 0};
  r.camera.near = 0.1f;
  r.camera.far = 200;
  RenderStats s;
  CHECK(r.render({}, s));
  CHECK(s.draw_calls == 1);
  // Parallel rays: every pixel looks straight down; off-centre rays start off-centre.
  Ray a, b;
  CHECK(r.screen_ray(200, 100, a));
  CHECK(r.screen_ray(390, 100, b));
  CHECK_NEAR(a.direction.z, -1.0f, 1e-4f);
  CHECK_NEAR(b.direction.z, -1.0f, 1e-4f);
  CHECK(b.origin.x > a.origin.x + 8);  // 10 m visible height, 2:1 aspect -> 20 m wide
  PickHit hit;
  CHECK(r.pick(200, 100, {}, hit));
  CHECK(hit.mesh == id);
  CHECK(!r.pick(390, 100, {}, hit));
  r.camera.ortho_height = 0;
  CHECK(!r.render({}, s));  // invalid projection is rejected
  r.shutdown();
}

// ---------------------------------------------------------------------------
// game: levels, body planning, the runtime

TEST(game_euler_and_transforms) {
  for (const Vec3 e : {Vec3{0, 0, 0}, Vec3{10, 20, 30}, Vec3{-45, 60, -170}, Vec3{90, 0, 0}, Vec3{0, 0, 179}}) {
    const Vec3 back = game::quat_to_euler(game::euler_to_quat(e));
    const Quat q1 = game::euler_to_quat(e), q2 = game::euler_to_quat(back);
    const f32 d = std::fabs(q1.x * q2.x + q1.y * q2.y + q1.z * q2.z + q1.w * q2.w);
    CHECK_NEAR(d, 1.0f, 1e-4f);  // same rotation
  }
  // Yaw 90 turns +X into +Y.
  const Vec3 x = game::euler_to_quat({0, 0, 90}).rotate({1, 0, 0});
  CHECK_NEAR(x.y, 1.0f, 1e-5f);
  game::Transform t;
  t.position = {1, 2, 3};
  t.rotation = {15, -30, 120};
  t.scale = {2, 3, 0.5f};
  const game::Transform back = game::matrix_transform(game::transform_matrix(t));
  CHECK(mat_near(game::transform_matrix(back), game::transform_matrix(t), 1e-4f));
  Vec3 c;
  CHECK(game::parse_hex_color("#ff8000", c) && c.x == 1 && std::fabs(c.y - 128 / 255.0f) < 1e-6f && c.z == 0);
  CHECK(game::hex_color({1, 0.5f, 0}) == "#ff8000");
  CHECK(!game::parse_hex_color("ff8000", c) && !game::parse_hex_color("#ggg000", c));
}

TEST(game_level_round_trip_and_validation) {
  game::Level l = game::physics_lab_level();
  const std::string text = game::to_text(l);
  game::Level back;
  std::string error;
  CHECK(game::from_text(text, back, &error));
  CHECK(game::to_text(back) == text);  // byte for byte
  CHECK(back.actors.size() == l.actors.size());
  CHECK(game::validate(back).empty() || !game::validate(back)[0].error);
  for (const game::Issue& i : game::validate(back)) CHECK(!i.error);
  // Orb Run round-trips too and has 24 orbs, one player, a city.
  const game::Level orb = game::orb_run_level();
  game::Level orb2;
  CHECK(game::from_text(game::to_text(orb), orb2, &error));
  u32 orbs = 0, players = 0, cities = 0;
  for (const game::Actor& a : orb2.actors) { orbs += a.pickup ? 1 : 0; players += a.controller ? 1 : 0; cities += a.city ? 1 : 0; }
  CHECK(orbs == 24 && players == 1 && cities == 1);
  for (const game::Issue& i : game::validate(orb2)) CHECK(!i.error);
  // Rejections and repairs.
  CHECK(!game::from_text("{\"format\":\"other\"}", back, &error));
  CHECK(!game::from_text("{\"format\":\"kern-level\",\"version\":99}", back, &error));
  CHECK(!game::from_text("{\"format\":\"kern-level\",\"actors\":[{\"components\":{\"mesh\":{\"shape\":\"blob\"}}}]}", back, &error));
  CHECK(error.find("shape") != std::string::npos);
  CHECK(game::from_text("{\"format\":\"kern-level\",\"actors\":[{\"id\":1,\"parent\":2},{\"id\":2,\"parent\":1},{\"id\":2}]}", back, &error));
  CHECK(back.actors.size() == 3);
  u32 roots = 0;
  for (const game::Actor& a : back.actors) roots += a.parent == 0;
  CHECK(roots >= 1);                       // the cycle is broken
  CHECK(back.actors[2].id != 2);           // duplicate id re-assigned
  // Validation finds real problems.
  game::Level bad = game::basic_level();
  bad.actors[1].joints.push_back(game::JointComponent{});
  bad.actors[1].joints.back().target = 999;
  bad.actors[1].transform.scale.z = 0;
  bad.actors[1].controller = game::ControllerComponent{};
  u32 errors = 0;
  for (const game::Issue& i : game::validate(bad)) errors += i.error;
  CHECK(errors >= 3);
}

TEST(game_body_planning) {
  game::Level l;
  game::Actor parent;
  parent.name = "Cart";
  parent.transform.position = {0, 0, 2};
  parent.mesh = game::MeshComponent{};
  parent.body = game::BodyComponent{};
  const game::ActorId pid = l.add(parent).id;
  game::Actor child;
  child.name = "Wheel";
  child.parent = pid;
  child.transform.position = {1, 0, 0};
  child.transform.scale = {2, 2, 2};
  child.mesh = game::MeshComponent{};
  child.mesh->shape = game::Shape::sphere;
  child.mesh->radius = 0.5f;
  l.add(child);
  game::Actor wall;
  wall.name = "Wall";
  wall.transform.position = {5, 0, 1};
  wall.mesh = game::MeshComponent{};
  l.add(wall);
  game::Actor deco;
  deco.name = "Decal";
  deco.mesh = game::MeshComponent{};
  deco.mesh->collision = false;
  l.add(deco);
  game::Actor coin;
  coin.name = "Coin";
  coin.pickup = game::PickupComponent{};
  coin.rotator = game::RotatorComponent{};
  l.add(coin);
  l.world.ground = true;
  const std::vector<game::BodyPlan> plans = game::plan_bodies(l);
  // Cart (compound with the welded wheel), wall (implicit static), coin (implicit kinematic sensor), ground.
  CHECK(plans.size() == 4);
  const game::BodyPlan* cart = nullptr;
  for (const game::BodyPlan& p : plans) if (p.owner == pid) cart = &p;
  CHECK(cart && cart->shapes.size() == 2 && cart->motion == game::Motion::dynamic);
  if (cart && cart->shapes.size() == 2) {
    const game::ShapePlan& wheel = cart->shapes[1];
    CHECK(wheel.kind == physics::ShapeKind::sphere);
    CHECK_NEAR(wheel.radius, 1.0f, 1e-5f);        // scaled by 2
    CHECK_NEAR(wheel.center.x, 1.0f, 1e-5f);      // in the cart's frame
    CHECK_NEAR(cart->position.z, 2.0f, 1e-5f);
  }
  bool coin_ok = false, wall_ok = false;
  for (const game::BodyPlan& p : plans) {
    const game::Actor* a = l.find(p.owner);
    if (a && a->name == "Coin") coin_ok = p.implicit && p.motion == game::Motion::kinematic && p.shapes.size() == 1 && p.shapes[0].source.sensor;
    if (a && a->name == "Wall") wall_ok = p.implicit && p.motion == game::Motion::fixed && p.shapes[0].kind == physics::ShapeKind::box;
  }
  CHECK(coin_ok && wall_ok);
  // Automatic hull colliders for cylinders.
  game::MeshComponent cyl;
  cyl.shape = game::Shape::cylinder;
  CHECK(game::fit_collider(cyl).shape == game::ColliderShape::hull);
  CHECK(game::shape_hull_points(cyl).size() == 32);
  for (const game::Shape s : {game::Shape::box, game::Shape::sphere, game::Shape::cylinder, game::Shape::cone, game::Shape::capsule, game::Shape::plane,
                              game::Shape::torus, game::Shape::ramp}) {
    game::MeshComponent m;
    m.shape = s;
    const MeshData d = game::shape_mesh(m);
    CHECK(d.vertex_count() > 0 && d.indices.size() % 3 == 0);
    const Aabb b = game::shape_bounds(m);
    for (u32 v = 0; v < d.vertex_count(); ++v) {
      const Vec3 p = Vec3::load(&d.positions[v * 3]);
      if (p.x < b.min.x - 1e-4f || p.x > b.max.x + 1e-4f || p.z < b.min.z - 1e-4f || p.z > b.max.z + 1e-4f) { CHECK(false); break; }
    }
  }
}

TEST(game_city_generation) {
  game::CityComponent c;
  game::CityGeometry a, b;
  CHECK(game::build_city(c, a));
  CHECK(game::build_city(c, b));
  CHECK(a.buildings.size() > 3 && a.roads.size() > 3 && a.colliders.size() > 49);
  CHECK(a.colliders.size() == b.colliders.size());
  CHECK(content_hash(a.buildings[0]) == content_hash(b.buildings[0]));  // deterministic
  CHECK(!a.trunk_transforms.empty() && a.crown_tints.size() / 4 == a.crown_transforms.size() / 16);
  CHECK(a.buildings[0].extra_size == 4);  // wall records for the night windows
  c.street = 200;
  CHECK(!game::build_city(c, a));
}

struct GameFixture {
  Renderer renderer;
  game::NightWindows windows;
  std::unique_ptr<game::GameWorld> world;
  GameFixture() {
    ShaderExtension* ext[1] = {&windows};
    RendererOptions o;
    o.shadows = true;
    renderer.init(o, Span<ShaderExtension* const>(ext, 1));
    world = std::make_unique<game::GameWorld>(renderer, &windows);
  }
  ~GameFixture() { world.reset(); renderer.shutdown(); }
  void run(f32 seconds, const game::PlayInput& in = {}) {
    for (f32 t = 0; t < seconds; t += 1.0f / 60) world->step(1.0f / 60, in);
  }
};

TEST(game_world_presentation) {
  GameFixture f;
  game::Level l = game::orb_run_level();
  f.world->sync(l);
  const u32 meshes = f.renderer.mesh_count();
  CHECK(meshes > 30);
  // Picking a mesh tells which actor it is.
  u32 owner_checks = 0;
  for (Mesh* m : f.renderer.meshes()) if (f.world->actor_for_mesh(m->id)) ++owner_checks;
  CHECK(owner_checks > 25);
  // Moving one actor only updates its instance; hiding hides it.
  game::Actor* orb = nullptr;
  for (game::Actor& a : l.actors) if (a.pickup) { orb = &a; break; }
  orb->transform.position.x += 5;
  orb->hidden = true;
  f.world->sync(l);
  CHECK(f.renderer.mesh_count() == meshes);
  std::vector<u32> ids;
  f.world->actor_meshes(orb->id, ids);
  CHECK(ids.size() == 1 && !f.renderer.mesh(ids[0])->visible);
  Mat4 m;
  CHECK(f.world->actor_matrix(orb->id, m) && std::fabs(m.translation().x - (orb->transform.position.x)) < 1e-4f);
  // Removing actors removes their meshes.
  l.actors.erase(std::remove_if(l.actors.begin(), l.actors.end(), [](const game::Actor& a) { return a.pickup.has_value(); }), l.actors.end());
  f.world->sync(l);
  CHECK(f.renderer.mesh_count() == meshes - 24);
  // Lighting follows the time of day.
  l.world.time_of_day = 0.5f;
  f.world->sync(l);
  CHECK(f.world->apply_lighting({0, 0, 10}) < 0.01f);
  CHECK(f.renderer.lighting.sun_direction.z > 0.9f);
  l.world.time_of_day = 0.0f;
  f.world->sync(l);
  CHECK(f.world->apply_lighting({0, 0, 10}) > 0.99f);
  CHECK(f.windows.night > 0.99f);
  RenderStats s;
  CHECK(f.renderer.render({}, s));
}

TEST(game_world_orb_run_play) {
  GameFixture f;
  game::Level l = game::orb_run_level();
  f.world->sync(l);
  CHECK(f.world->start(l, game::SimMode::play));
  CHECK(f.world->running() && f.world->player() != 0);
  CHECK(f.world->hud().status == game::GameStatus::playing && f.world->hud().pickups_total == 24);
  CHECK(f.world->stats().bodies > 24);
  // Hovering: the car settles near its hover height above the street.
  f.run(2);
  Mat4 m;
  CHECK(f.world->actor_matrix(f.world->player(), m));
  const f32 z = m.translation().z;
  CHECK(z > 0.4f && z < 2.0f);
  // Driving forward moves the car along its heading (+Y at yaw 90).
  const Vec3 start = m.translation();
  game::PlayInput go;
  go.move_y = 1;
  f.run(3, go);
  CHECK(f.world->actor_matrix(f.world->player(), m));
  CHECK(m.translation().y - start.y > 20);
  CHECK(std::fabs(m.translation().x - start.x) < 6);
  CHECK(f.world->hud().speed > 10);
  // Boost drains.
  go.boost = true;
  f.run(1, go);
  CHECK(f.world->hud().boost < 0.8f);
  // The chase camera sits behind and above.
  Camera cam;
  CHECK(f.world->update_camera(cam, 1.0f / 60, {}));
  CHECK(cam.eye.z > m.translation().z + 2);
  // Collect an orb: put the car on it.
  game::ActorId orb = 0;
  for (const game::Actor& a : f.world->level().actors) if (a.pickup) { orb = a.id; break; }
  Mat4 om;
  CHECK(f.world->actor_matrix(orb, om));
  physics::World& phys = f.world->physics();
  const physics::Body car_body = f.world->actor_body(f.world->player());
  CHECK(car_body.valid());
  phys.set_pose(car_body, {om.translation(), game::euler_to_quat({0, 0, 90})});
  phys.set_linear_velocity(car_body, {});
  f.run(0.3f);
  CHECK(f.world->hud().pickups == 1 && f.world->hud().score == 1);
  std::vector<u32> ids;
  f.world->present(1000, {0, 0, 10});
  // Falling off the world respawns.
  phys.set_pose(car_body, {{0, 0, -40}, {}});
  f.run(0.2f);
  CHECK(f.world->actor_matrix(f.world->player(), m) && m.translation().z > -5);
  // Stop restores the edited level and its meshes.
  f.world->stop();
  CHECK(!f.world->running());
  game::ActorId car = 0;
  for (const game::Actor& a : l.actors) if (a.controller) car = a.id;
  CHECK(f.world->actor_matrix(car, m) && std::fabs(m.translation().x - l.find(car)->transform.position.x) < 1e-4f);
  RenderStats s;
  CHECK(f.renderer.render({}, s));
}

TEST(game_world_physics_lab) {
  GameFixture f;
  game::Level l = game::physics_lab_level();
  for (const game::Issue& i : game::validate(l)) CHECK(!i.error);
  CHECK(f.world->start(l, game::SimMode::simulate));
  CHECK(f.world->events().empty());  // every collider and joint was accepted
  CHECK(f.world->player() == 0);  // Simulate never possesses
  const game::SimStats st = f.world->stats();
  CHECK(st.joints >= 20);       // pendulum, door, bridge 13, turntable, lift, rover 4
  auto find = [&](const char* name) {
    for (const game::Actor& a : l.actors) if (a.name == name) return a.id;
    return game::ActorId{0};
  };
  Mat4 m0, m1;
  const game::ActorId ball = find("Pendulum Ball"), table = find("Turntable"), lift = find("Lift Platform"), rover = find("Rover Chassis"),
                      plank = find("Plank 06");
  CHECK(f.world->actor_matrix(ball, m0));
  Mat4 r0, t0, p0;
  f.world->actor_matrix(rover, r0);
  f.world->actor_matrix(lift, t0);
  f.world->actor_matrix(plank, p0);
  f.run(2);
  CHECK(f.world->actor_matrix(ball, m1));
  // Pendulum: swings down but stays on its rope (distance to the beam is constant).
  const Vec3 beam{-8, -12, 6};
  CHECK_NEAR(distance(m1.translation(), beam), distance(m0.translation(), beam), 0.15f);
  CHECK(distance(m1.translation(), m0.translation()) > 2);  // it swings
  // Turntable spins (motor), lift rises (slider motor), rover drives (wheel motors), bridge hangs.
  Mat4 tm;
  f.world->actor_matrix(table, tm);
  CHECK(std::fabs(game::matrix_transform(tm).rotation.z) > 30);
  Mat4 t1, r1, p1;
  f.world->actor_matrix(lift, t1);
  CHECK(t1.translation().z > t0.translation().z + 1.5f);
  f.world->actor_matrix(rover, r1);
  CHECK(distance(r1.translation(), r0.translation()) > 2);
  f.world->actor_matrix(plank, p1);
  CHECK(p1.translation().z > 0.5f);  // held by the joints
  // Overloading the bridge breaks its weak joints.
  physics::World& phys = f.world->physics();
  const physics::RayHit hit = phys.cast_ray(p1.translation() + Vec3{0, 0, 3}, Vec3{0, 0, -4});
  CHECK(hit.hit);
  for (int i = 0; i < 30 && f.world->stats().broken_joints == 0; ++i) {
    phys.apply_linear_impulse_to_center(hit.body, {0, 0, -900});
    f.run(1.0f / 30);
  }
  CHECK(f.world->stats().broken_joints >= 1);
  // Debug geometry.
  std::vector<f32> pos, col;
  f.world->debug_lines(true, true, true, pos, col);
  CHECK(pos.size() > 300 && pos.size() == col.size());
  // Switching to Play possesses the rolling ball.
  CHECK(f.world->set_mode(game::SimMode::play));
  CHECK(f.world->player() == find("Player Ball"));
  game::PlayInput push;
  push.move_y = 1;
  Mat4 b0, b1;
  f.world->actor_matrix(f.world->player(), b0);
  Camera cam;
  f.world->update_camera(cam, 1.0f / 60, {});
  f.run(1.5f, push);
  f.world->actor_matrix(f.world->player(), b1);
  CHECK(distance(b1.translation(), b0.translation()) > 2);
  // Grab in simulate.
  CHECK(f.world->set_mode(game::SimMode::simulate));
  Mat4 cm;
  const game::ActorId crate = find("Crate 15");
  f.world->actor_matrix(crate, cm);
  CHECK(f.world->begin_grab(crate, cm.translation()));
  f.world->move_grab(cm.translation() + Vec3{0, 0, 3});
  f.run(1);
  Mat4 cm2;
  f.world->actor_matrix(crate, cm2);
  CHECK(cm2.translation().z > cm.translation().z + 1.5f);
  f.world->end_grab();
  CHECK(f.world->restart());
  CHECK(f.world->stats().broken_joints == 0);
  f.world->stop();
}

TEST(game_world_rules) {
  GameFixture f;
  game::Level l = game::basic_level();
  game::Actor ball;
  ball.name = "Ball";
  ball.transform.position = {0, 0, 1};
  ball.mesh = game::MeshComponent{};
  ball.mesh->shape = game::Shape::sphere;
  ball.body = game::BodyComponent{};
  ball.controller = game::ControllerComponent{};
  ball.controller->mode = game::ControlMode::rolling;
  const game::ActorId ball_id = l.add(ball).id;
  game::Actor goal;
  goal.name = "Goal";
  goal.transform.position = {6, 0, 0.5f};
  goal.trigger = game::TriggerComponent{};
  l.add(goal);
  game::Actor hazard;
  hazard.name = "Lava";
  hazard.transform.position = {-6, 0, 0.5f};
  hazard.trigger = game::TriggerComponent{game::TriggerAction::hazard, "", true};
  l.add(hazard);
  l.world.goal = game::GameGoal::reach_goal;
  l.world.time_limit = 30;
  CHECK(f.world->start(l, game::SimMode::play));
  CHECK(f.world->player() == ball_id);
  Camera cam;
  f.world->update_camera(cam, 1.0f / 60, {});
  physics::World& phys = f.world->physics();
  phys.set_pose(f.world->actor_body(ball_id), {{6, 0, 0.6f}, {}});
  f.run(0.2f);
  CHECK(f.world->hud().status == game::GameStatus::won);
  CHECK(f.world->hud().best > 0);
  CHECK(f.world->restart());
  CHECK(f.world->hud().status == game::GameStatus::playing);
  f.world->physics().set_pose(f.world->actor_body(ball_id), {{-6, 0, 0.6f}, {}});
  f.run(0.2f);
  CHECK(f.world->hud().status == game::GameStatus::lost);
  CHECK(f.world->hud().remaining > 0 && f.world->hud().remaining < 30);
  f.world->stop();
}

TEST(game_character_mover) {
  GameFixture f;
  game::Level l = game::basic_level();
  game::Actor hero;
  hero.name = "Hero";
  hero.transform.position = {-6, 0, 1.0f};
  hero.mesh = game::MeshComponent{};
  hero.mesh->shape = game::Shape::capsule;
  hero.mesh->radius = 0.4f;
  hero.mesh->height = 1.8f;
  hero.body = game::BodyComponent{};
  hero.body->mass = 75;
  hero.controller = game::ControllerComponent{};
  hero.controller->mode = game::ControlMode::character;
  hero.controller->max_speed = 5;
  hero.controller->jump_speed = 6;
  const game::ActorId hero_id = l.add(hero).id;
  auto box = [&](const char* name, Vec3 at, Vec3 size, bool dynamic) {
    game::Actor a;
    a.name = name;
    a.transform.position = at;
    a.mesh = game::MeshComponent{};
    a.mesh->size = size;
    a.mesh->collision = true;
    if (dynamic) { a.body = game::BodyComponent{}; a.body->mass = 20; }
    return l.add(a).id;
  };
  box("Step", {-2, 0, 0.15f}, {2, 4, 0.3f}, false);          // 30 cm step: walked over
  box("Wall", {4, 0, 1.5f}, {0.5f, 6, 3}, false);           // blocks
  const game::ActorId crate = box("Crate", {-4, 3, 0.5f}, {1, 1, 1}, true);
  game::Actor orb;
  orb.name = "Orb";
  orb.transform.position = {1, 0, 0.9f};
  orb.pickup = game::PickupComponent{};
  game::ColliderComponent sensor;
  sensor.shape = game::ColliderShape::sphere;
  sensor.radius = 0.5f;
  sensor.sensor = true;
  orb.colliders.push_back(sensor);
  l.add(orb);
  for (game::Actor& a : l.actors) if (a.name == "Cube") a.transform.position = {20, 20, 0.5f};
  CHECK(f.world->start(l, game::SimMode::play));
  CHECK(f.world->player() == hero_id);
  CHECK(f.world->physics().body_type(f.world->actor_body(hero_id)) == physics::BodyType::kinematic);
  Camera cam;
  game::PlayInput in;
  for (int i = 0; i < 30; ++i) { f.world->step(1.0f / 60, in); f.world->update_camera(cam, 1.0f / 60, in); }
  const f32 rest_z = f.world->physics().pose(f.world->actor_body(hero_id)).position.z;
  CHECK(rest_z > 0.7f && rest_z < 1.3f);  // standing on the floor (feet near 0.0, origin 0.9 above)
  // Walk along +X (the chase camera looks along the hero's facing, +X): over the step, through the orb, into the wall.
  in.move_y = 1;
  f32 max_z = 0;
  for (int i = 0; i < 240; ++i) {
    f.world->step(1.0f / 60, in);
    f.world->update_camera(cam, 1.0f / 60, in);
    max_z = std::max(max_z, f.world->physics().pose(f.world->actor_body(hero_id)).position.z);
  }
  const Vec3 at_wall = f.world->physics().pose(f.world->actor_body(hero_id)).position;
  CHECK(max_z > rest_z + 0.2f);                 // climbed the step
  CHECK(at_wall.x > 2.5f && at_wall.x < 3.8f);  // stopped by the wall (wall face at 3.75, radius 0.4)
  CHECK(f.world->hud().pickups == 1);           // walked through the orb sensor
  // Jump.
  in.move_y = 0;
  in.jump = true;
  f.world->step(1.0f / 60, in);
  in.jump = false;
  f32 peak = 0;
  for (int i = 0; i < 40; ++i) { f.world->step(1.0f / 60, in); peak = std::max(peak, f.world->physics().pose(f.world->actor_body(hero_id)).position.z); }
  CHECK(peak > rest_z + 0.5f);
  for (int i = 0; i < 90; ++i) f.world->step(1.0f / 60, in);
  CHECK(std::fabs(f.world->physics().pose(f.world->actor_body(hero_id)).position.z - rest_z) < 0.15f);
  // Walking into the crate pushes it.
  const Vec3 crate0 = f.world->physics().pose(f.world->actor_body(crate)).position;
  f.world->physics().set_pose(f.world->actor_body(hero_id), {{-4, 0.5f, rest_z}, {}});
  f.world->stop();
  CHECK(f.world->start(l, game::SimMode::play));
  // Start right next to the crate, facing +Y via the camera yaw: drive with move_x / move_y relative to the camera.
  for (int i = 0; i < 20; ++i) f.world->step(1.0f / 60, {});
  physics::World& phys = f.world->physics();
  (void)phys;
  game::PlayInput towards;
  // Camera yaw follows the hero facing +X; +Y is "left" (move_x = -1).
  towards.move_x = -1;
  for (int i = 0; i < 20; ++i) f.world->step(1.0f / 60, towards);
  // Teleport next to the crate and keep walking into it.
  game::Level copy = l;
  for (game::Actor& a : copy.actors) if (a.id == hero_id) a.transform.position = {-4, 1.5f, 1.0f};
  f.world->stop();
  CHECK(f.world->start(copy, game::SimMode::play));
  for (int i = 0; i < 90; ++i) f.world->step(1.0f / 60, towards);
  const Vec3 crate1 = f.world->physics().pose(f.world->actor_body(crate)).position;
  CHECK(crate1.y > crate0.y + 0.3f);
  // Ejecting turns the hero back into a dynamic body; possessing again uses the mover.
  CHECK(f.world->set_mode(game::SimMode::simulate));
  CHECK(f.world->physics().body_type(f.world->actor_body(hero_id)) == physics::BodyType::dynamic);
  CHECK(f.world->set_mode(game::SimMode::play));
  CHECK(f.world->physics().body_type(f.world->actor_body(hero_id)) == physics::BodyType::kinematic);
  f.world->stop();
}

// ---------------------------------------------------------------------------
// Audio

namespace {
struct Rendered {
  std::vector<f32> pcm;
  f32 peak_l = 0, peak_r = 0, rms_l = 0, rms_r = 0;
  bool finite = true;
};
Rendered render_mixer(audio::Mixer& m, u32 frames) {
  Rendered r;
  r.pcm.resize(static_cast<std::size_t>(frames) * 2);
  // Odd block sizes on purpose: the mixer must not depend on the host's buffer size.
  u32 done = 0, block = 97;
  while (done < frames) {
    const u32 n = std::min(block, frames - done);
    m.render(r.pcm.data() + done * 2, n);
    done += n;
    block = block == 97 ? 512 : block == 512 ? 128 : 97;
  }
  f64 sl = 0, sr = 0;
  for (u32 i = 0; i < frames; ++i) {
    const f32 l = r.pcm[i * 2], rr = r.pcm[i * 2 + 1];
    r.finite = r.finite && std::isfinite(l) && std::isfinite(rr);
    r.peak_l = std::max(r.peak_l, std::fabs(l));
    r.peak_r = std::max(r.peak_r, std::fabs(rr));
    sl += l * l;
    sr += rr * rr;
  }
  r.rms_l = static_cast<f32>(std::sqrt(sl / frames));
  r.rms_r = static_cast<f32>(std::sqrt(sr / frames));
  return r;
}
std::vector<f32> sine(u32 rate, f32 seconds, f32 hz, f32 amp = 0.5f) {
  std::vector<f32> v(static_cast<std::size_t>(rate * seconds));
  for (std::size_t i = 0; i < v.size(); ++i) v[i] = amp * std::sin(2 * kPi * hz * static_cast<f32>(i) / static_cast<f32>(rate));
  return v;
}
}  // namespace

TEST(audio_mixer_panning_and_limits) {
  audio::Mixer m;
  CHECK(!m.ready());
  CHECK(m.init(48000, 16));
  const audio::ClipId tone = m.adopt_clip(sine(48000, 1, 440), 1, "tone");
  CHECK(tone != 0);
  CHECK_NEAR(m.clip_info(tone)->duration, 1.0f, 1e-3f);
  // Hard left.
  audio::PlayParams p;
  p.pan = -1;
  audio::VoiceId v = m.play(tone, p);
  CHECK(v != 0 && m.playing(v));
  Rendered r = render_mixer(m, 4800);
  CHECK(r.finite);
  CHECK(r.rms_l > 0.1f);
  CHECK(r.rms_r < r.rms_l * 0.02f);
  m.stop(v);
  render_mixer(m, 2400);
  CHECK(!m.playing(v));
  // Eight loud voices at once: the limiter keeps the output inside [-1, 1].
  for (int i = 0; i < 8; ++i) {
    audio::PlayParams q;
    q.volume = 4;
    q.pitch = 1 + 0.01f * i;
    m.play(tone, q);
  }
  r = render_mixer(m, 9600);
  CHECK(r.finite);
  CHECK(r.peak_l <= 1.0f && r.peak_r <= 1.0f);
  CHECK(r.peak_l > 0.5f);
  CHECK(m.stats().limiter_reduction_db < 0);
  // One-shots end by themselves, loops keep going.
  m.stop_all();
  render_mixer(m, 2400);
  const audio::VoiceId once = m.play(tone, {});
  audio::PlayParams lp;
  lp.loop = true;
  const audio::VoiceId loop = m.play(tone, lp);
  render_mixer(m, 48000 + 4800);
  CHECK(!m.playing(once));
  CHECK(m.playing(loop));
  CHECK(m.position_of(loop) < 1.0f);
  // Pitch 2 plays an octave up: the clip ends after half its length.
  m.stop_all();
  render_mixer(m, 2400);
  audio::PlayParams fast;
  fast.pitch = 2;
  const audio::VoiceId f2 = m.play(tone, fast);
  render_mixer(m, 26000);
  CHECK(!m.playing(f2));
  // Muted buses are silent, and the bus meters see their own signal only.
  m.stop_all();
  render_mixer(m, 2400);
  m.bus(audio::Bus::music).muted = true;
  audio::PlayParams mp;
  mp.bus = audio::Bus::music;
  mp.loop = true;
  m.play(tone, mp);
  r = render_mixer(m, 4800);
  CHECK(m.stats().buses[static_cast<u32>(audio::Bus::music)].peak[0] < 1e-4f);
  m.bus(audio::Bus::music).muted = false;
  r = render_mixer(m, 4800);
  CHECK(r.peak_l > 0.1f);
  CHECK(m.stats().buses[static_cast<u32>(audio::Bus::music)].peak[0] > 0.1f);
  CHECK(m.stats().buses[static_cast<u32>(audio::Bus::sfx)].peak[0] < 1e-4f);
  // Invalid input is rejected.
  CHECK(m.create_clip(nullptr, 10, 1, 48000, "x") == 0);
  const std::vector<f32> bad = {0.1f, NAN, 0.2f};
  CHECK(m.create_clip(bad.data(), 3, 1, 48000, "nan") == 0 || m.clip_info(m.create_clip(bad.data(), 3, 1, 48000, "nan"))->peak <= 1);
  CHECK(m.play(9999, {}) == 0);
}

TEST(audio_mixer_resampling_and_stereo) {
  audio::Mixer m;
  CHECK(m.init(48000));
  // 1 s at 22050 Hz becomes 48000 frames; a 1 kHz tone stays 1 kHz (zero crossings).
  const std::vector<f32> src = sine(22050, 1, 1000);
  const audio::ClipId c = m.create_clip(src.data(), static_cast<u32>(src.size()), 1, 22050, "low");
  CHECK(c != 0);
  CHECK(m.clip_info(c)->frames >= 47990 && m.clip_info(c)->frames <= 48010);
  const std::vector<f32>& pcm = *m.clip_samples(c);
  u32 crossings = 0;
  for (std::size_t i = 1000; i < 47000; ++i) crossings += (pcm[i - 1] < 0) != (pcm[i] < 0);
  CHECK(crossings > 1900 && crossings < 2020);  // 2 per period, 46000 frames ~ 958 periods
  // Stereo clips keep their channels.
  std::vector<f32> st(48000 * 2);
  for (u32 i = 0; i < 48000; ++i) { st[i * 2] = 0.5f * std::sin(2 * kPi * 300 * i / 48000.0f); st[i * 2 + 1] = 0; }
  const audio::ClipId sc = m.create_clip(st.data(), 48000, 2, 48000, "stereo");
  CHECK(sc != 0 && m.clip_info(sc)->channels == 2);
  m.play(sc, {});
  Rendered r = render_mixer(m, 9600);
  CHECK(r.rms_l > 0.1f && r.rms_r < 0.01f);
  // Destroying a clip stops its voices.
  CHECK(m.destroy_clip(sc));
  r = render_mixer(m, 2400);
  CHECK(m.stats().voices == 0);
  CHECK(m.clip_info(sc) == nullptr);
}

TEST(audio_mixer_spatial) {
  audio::Mixer m;
  CHECK(m.init(48000));
  const audio::ClipId tone = m.adopt_clip(sine(48000, 1, 330), 1, "tone");
  audio::Listener l;
  l.position = {0, 0, 0};
  l.forward = {1, 0, 0};
  l.up = {0, 0, 1};
  m.set_listener(l);
  auto level_at = [&](Vec3 at, audio::Rolloff ro = audio::Rolloff::inverse, f32 mn = 1, f32 mx = 50) {
    m.stop_all();
    render_mixer(m, 1200);
    audio::PlayParams p;
    p.spatial = true;
    p.loop = true;
    p.position = at;
    p.rolloff = ro;
    p.min_distance = mn;
    p.max_distance = mx;
    m.play(tone, p);
    render_mixer(m, 2400);  // ramps settle
    return render_mixer(m, 9600);
  };
  // +Y is left when looking along +X with Z up.
  Rendered left = level_at({0, 5, 0});
  CHECK(left.rms_l > left.rms_r * 2);
  Rendered right = level_at({0, -5, 0});
  CHECK(right.rms_r > right.rms_l * 2);
  // Further is quieter; beyond max_distance a linear source is silent.
  Rendered near = level_at({4, 0, 0}), far = level_at({30, 0, 0});
  CHECK(near.rms_l > far.rms_l * 2);
  Rendered gone = level_at({80, 0, 0}, audio::Rolloff::linear, 1, 50);
  CHECK(gone.rms_l < 1e-3f);
  CHECK(m.stats().virtual_voices >= 1);
  // Doppler: a source rushing towards the listener plays higher (more zero crossings).
  auto crossings = [&](Vec3 vel) {
    m.stop_all();
    render_mixer(m, 1200);
    audio::PlayParams p;
    p.spatial = true;
    p.loop = true;
    p.position = {10, 0, 0};
    p.velocity = vel;
    m.play(tone, p);
    render_mixer(m, 4800);
    Rendered r = render_mixer(m, 48000);
    u32 n = 0;
    for (std::size_t i = 2; i < r.pcm.size(); i += 2) n += (r.pcm[i - 2] < 0) != (r.pcm[i] < 0);
    return n;
  };
  const u32 still = crossings({}), towards = crossings({-60, 0, 0});
  CHECK(still > 640 && still < 680);
  CHECK(towards > still + 40);
  // Pausing everything holds the position.
  m.stop_all();
  render_mixer(m, 1200);
  audio::PlayParams p;
  p.loop = true;
  const audio::VoiceId v = m.play(tone, p);
  render_mixer(m, 4800);
  const f32 at = m.position_of(v);
  m.set_all_paused(true);
  Rendered silent = render_mixer(m, 4800);
  CHECK_NEAR(m.position_of(v), at, 1e-3f);
  CHECK(silent.peak_l < 0.05f);
  m.set_all_paused(false);
  render_mixer(m, 4800);
  CHECK(m.position_of(v) > at + 0.05f);
}

TEST(audio_mixer_voice_stealing) {
  audio::Mixer m;
  CHECK(m.init(44100, 4));
  const audio::ClipId tone = m.adopt_clip(sine(44100, 2, 220), 1, "tone");
  std::vector<audio::VoiceId> ids;
  for (int i = 0; i < 4; ++i) ids.push_back(m.play(tone, {}));
  audio::PlayParams important;
  important.priority = 10;
  const audio::VoiceId vip = m.play(tone, important);
  CHECK(vip != 0 && m.playing(vip));
  u32 alive = 0;
  for (audio::VoiceId id : ids) alive += m.playing(id);
  CHECK(alive == 3);
  CHECK(m.stats().stolen >= 1);
  // Low priority cannot steal from higher priorities once all are important.
  for (int i = 0; i < 3; ++i) m.play(tone, important);
  audio::PlayParams low;
  low.priority = -5;
  CHECK(m.play(tone, low) == 0);
  render_mixer(m, 1000);
  CHECK(m.stats().voices <= 4);
}

TEST(audio_builtin_library) {
  const u32 rate = 48000;
  CHECK(audio::builtin_sounds().size() >= 25);
  CHECK(audio::find_builtin("quack") && audio::find_builtin("music_drive") && !audio::find_builtin("nope"));
  for (const audio::BuiltinSound& b : audio::builtin_sounds()) {
    std::vector<f32> pcm;
    u32 ch = 0;
    CHECK(audio::render_builtin(b.name, rate, pcm, ch));
    CHECK(ch == 1 || ch == 2);
    CHECK(!pcm.empty());
    f32 peak = 0;
    bool finite = true;
    for (f32 x : pcm) { finite = finite && std::isfinite(x); peak = std::max(peak, std::fabs(x)); }
    if (!finite || peak > 1.0f || peak < 0.02f) std::printf("    %s: finite %d peak %.3f\n", b.name, finite, peak);
    CHECK(finite);
    CHECK(peak <= 1.0f);
    CHECK(peak > 0.02f);
    if (b.loop) {
      // Seamless: the jump from the last frame to the first is no larger than a typical step.
      const std::size_t n = pcm.size() / ch;
      f32 seam = 0, typical = 0;
      for (u32 c = 0; c < ch; ++c) seam = std::max(seam, std::fabs(pcm[(n - 1) * ch + c] - pcm[c]));
      for (std::size_t i = 1; i < n; ++i) typical += std::fabs(pcm[i * ch] - pcm[(i - 1) * ch]);
      typical /= static_cast<f32>(n);
      if (seam > typical * 12 + 0.02f) std::printf("    %s: seam %.4f typical %.4f\n", b.name, seam, typical);
      CHECK(seam <= typical * 12 + 0.02f);
    }
  }
  // Deterministic: the same sound renders bit-identically, and the sliced job equals the one-shot render.
  std::vector<f32> a, b;
  u32 ca = 0, cb = 0;
  audio::render_builtin("explosion", rate, a, ca);
  audio::render_builtin("explosion", rate, b, cb);
  CHECK(a == b);
  audio::BuiltinJob job;
  CHECK(job.start("music_calm", rate));
  CHECK(!job.done());
  int steps = 0;
  while (!job.step(0.5)) ++steps;
  CHECK(steps > 1);
  u32 cj = 0;
  const std::vector<f32> sliced = job.take(cj);
  std::vector<f32> whole;
  u32 cw = 0;
  audio::render_builtin("music_calm", rate, whole, cw);
  CHECK(cj == cw && sliced == whole);
  CHECK(!job.start("nope", rate));
}

TEST(audio_level_fields_round_trip) {
  game::Level l = game::basic_level();
  game::Actor speaker;
  speaker.name = "Speaker";
  speaker.audio = game::AudioComponent{};
  speaker.audio->sound = "snd1";
  speaker.audio->volume = 0.7f;
  speaker.audio->bus = audio::Bus::ambience;
  speaker.audio->rolloff = audio::Rolloff::linear;
  speaker.audio->min_distance = 3;
  speaker.audio->max_distance = 60;
  speaker.audio->pitch_by_speed = 0.5f;
  speaker.body = game::BodyComponent{};
  speaker.body->impact_sound = "builtin:break";
  speaker.body->impact_threshold = 4;
  l.add(speaker);
  l.world.music = "builtin:music_drive";
  l.world.music_volume = 0.4f;
  l.world.reverb = 0.6f;
  game::SoundAsset snd;
  snd.id = "snd1";
  snd.name = "Engine";
  snd.duration = 2.5f;
  snd.src = std::make_shared<const std::string>("data:audio/wav;base64,UklGRg==");
  l.sounds.push_back(snd);
  const std::string text = game::to_text(l, 2);
  game::Level back;
  std::string err;
  CHECK(game::from_text(text, back, &err));
  const game::Actor* sp = nullptr;
  for (const game::Actor& a : back.actors) if (a.name == "Speaker") sp = &a;
  CHECK(sp && sp->audio);
  CHECK(sp->audio->sound == "snd1");
  CHECK_NEAR(sp->audio->volume, 0.7f, 1e-5f);
  CHECK(sp->audio->bus == audio::Bus::ambience);
  CHECK(sp->audio->rolloff == audio::Rolloff::linear);
  CHECK_NEAR(sp->audio->max_distance, 60.0f, 1e-4f);
  CHECK(sp->body->impact_sound == "builtin:break");
  CHECK(back.world.music == "builtin:music_drive");
  CHECK_NEAR(back.world.reverb, 0.6f, 1e-5f);
  CHECK(back.sounds.size() == 1 && back.sound("snd1") && back.sound("snd1")->name == "Engine");
  CHECK(game::to_text(back, 2) == text);
  // Unknown references are warnings, not errors.
  game::Level bad = back;
  for (game::Actor& a : bad.actors) if (a.audio) a.audio->sound = "builtin:does_not_exist";
  bool warned = false, errors = false;
  for (const game::Issue& i : game::validate(bad)) { warned |= i.text.find("does not exist") != std::string::npos; errors |= i.error; }
  CHECK(warned && !errors);
  // Bad values are rejected with a message.
  game::Level rejected;
  std::string text2 = text;
  const std::size_t at = text2.find("\"bus\": \"ambience\"");
  CHECK(at != std::string::npos);
  text2.replace(at, std::strlen("\"bus\": \"ambience\""), "\"bus\": \"loud\"");
  CHECK(!game::from_text(text2, rejected, &err));
  CHECK(err.find("bus") != std::string::npos);
  // Only audio data URLs are accepted as sound assets.
  std::string text3 = text;
  const std::size_t d = text3.find("data:audio/wav");
  text3.replace(d, std::strlen("data:audio/wav"), "javascript:alert");
  game::Level filtered;
  CHECK(game::from_text(text3, filtered, &err));
  CHECK(filtered.sounds.empty());
}

TEST(audio_game_world_cues) {
  GameFixture f;
  audio::Mixer mixer;
  CHECK(mixer.init(48000));
  game::SoundBank bank(mixer);
  game::GameAudio ga(mixer, bank);
  game::Level l = game::basic_level();
  game::Actor ball;
  ball.name = "Ball";
  ball.transform.position = {0, 0, 1};
  ball.mesh = game::MeshComponent{};
  ball.mesh->shape = game::Shape::sphere;
  ball.body = game::BodyComponent{};
  ball.controller = game::ControllerComponent{};
  ball.controller->mode = game::ControlMode::rolling;
  ball.controller->jump_speed = 6;
  const game::ActorId ball_id = l.add(ball).id;
  game::Actor crate;
  crate.name = "Crate";
  crate.transform.position = {5, 5, 6};
  crate.mesh = game::MeshComponent{};
  crate.body = game::BodyComponent{};
  crate.body->motion = game::Motion::dynamic;
  l.add(crate);
  game::Actor orb;
  orb.name = "Orb";
  orb.transform.position = {-4, 0, 0.6f};
  orb.pickup = game::PickupComponent{};
  l.add(orb);
  game::Actor hum;
  hum.name = "Hum";
  hum.transform.position = {2, 0, 1};
  hum.audio = game::AudioComponent{};
  hum.audio->delay = 0.2f;
  l.add(hum);
  l.world.music = "builtin:music_calm";
  // Everything the level uses is prepared ahead (all built-ins render in time slices).
  bank.request_level(l);
  CHECK(bank.queued() > 5);
  while (!bank.prepare(5)) {}
  CHECK(bank.ready("builtin:music_calm") && bank.ready("builtin:impact") && bank.ready("builtin:jump") && bank.ready("builtin:pickup") && bank.ready("builtin:hum"));
  CHECK(f.world->start(l, game::SimMode::play));
  Camera cam;
  f.world->update_camera(cam, 1.0f / 60, {});
  game::PlayInput in;
  auto frame = [&](const game::PlayInput& input) {
    f.world->step(1.0f / 60, input);
    f.world->update_camera(cam, 1.0f / 60, input);
    ga.update(*f.world, cam, 1.0f / 60);
    render_mixer(mixer, 800);
  };
  for (int i = 0; i < 120; ++i) frame(in);
  CHECK(ga.active());
  // The crate fell and hit the ground: an impact.
  const u32 after_fall = ga.cues_played();
  CHECK(after_fall >= 1);
  // Music, the hum and the impact sounded.
  CHECK(mixer.stats().voices >= 2);
  // Jump.
  in.jump = true;
  frame(in);
  in.jump = false;
  for (int i = 0; i < 5; ++i) frame(in);
  CHECK(ga.cues_played() > after_fall);
  // Pickup.
  const u32 before_pickup = ga.cues_played();
  f.world->physics().set_pose(f.world->actor_body(ball_id), {{-4, 0, 0.6f}, {}});
  for (int i = 0; i < 10; ++i) frame(in);
  CHECK(f.world->hud().pickups == 1);
  CHECK(ga.cues_played() > before_pickup);
  CHECK(ga.cues_dropped() == 0);
  Rendered r = render_mixer(mixer, 4800);
  CHECK(r.finite && r.peak_l > 0.01f && r.peak_l <= 1.0f);
  // Pause silences the world (positions held), stop fades everything out.
  f.world->set_paused(true);
  ga.update(*f.world, cam, 1.0f / 60);
  render_mixer(mixer, 4800);
  r = render_mixer(mixer, 4800);
  CHECK(mixer.stats().buses[static_cast<u32>(audio::Bus::music)].peak[0] < 1e-4f);
  CHECK(mixer.stats().buses[static_cast<u32>(audio::Bus::sfx)].peak[0] < 1e-4f);
  f.world->set_paused(false);
  // A restart begins a fresh session (music again, sources again).
  CHECK(f.world->restart());
  ga.update(*f.world, cam, 1.0f / 60);
  CHECK(ga.active());
  f.world->stop();
  ga.update(*f.world, cam, 1.0f / 60);
  CHECK(!ga.active());
  render_mixer(mixer, 48000 / 2);
  CHECK(mixer.stats().voices == 0);
  // Deterministic cues: two runs of the same level produce the same cue sequence.
  auto trace = [&]() {
    GameFixture g;
    std::string out;
    CHECK(g.world->start(l, game::SimMode::play));
    game::PlayInput jump;
    for (int i = 0; i < 120; ++i) {
      jump.jump = i == 70;
      g.world->step(1.0f / 60, jump);
      for (const game::SoundCue& c : g.world->sound_cues()) {
        char buf[160];
        std::snprintf(buf, sizeof buf, "%d %s %.4f %.4f|", i, c.sound.c_str(), c.volume, c.pitch);
        out += buf;
      }
      g.world->sound_cues().clear();
    }
    return out;
  };
  const std::string t1 = trace(), t2 = trace();
  CHECK(!t1.empty() && t1 == t2);
  CHECK(t1.find("builtin:impact") != std::string::npos && t1.find("builtin:jump") != std::string::npos);
}

TEST(audio_imported_sounds_in_the_bank) {
  audio::Mixer mixer;
  CHECK(mixer.init(44100));
  game::SoundBank bank(mixer);
  game::Level l = game::basic_level();
  game::SoundAsset s;
  s.id = "snd1";
  s.name = "Beep";
  s.src = std::make_shared<const std::string>("data:audio/wav;base64,AAAA");
  l.sounds.push_back(s);
  bank.sync_assets(l);
  CHECK(bank.clip("snd1") == 0);  // requested: the host decodes it
  CHECK(bank.pending_imports() == std::vector<std::string>{"snd1"});
  CHECK(bank.source("snd1") && *bank.source("snd1") == *s.src);
  const std::vector<f32> pcm = sine(22050, 0.5f, 600);
  CHECK(bank.set_pcm("snd1", pcm.data(), static_cast<u32>(pcm.size()), 1, 22050));
  CHECK(bank.ready("snd1"));
  CHECK_NEAR(bank.duration("snd1"), 0.5f, 0.01f);
  CHECK(bank.pending_imports().empty());
  // Replacing the asset (new source) drops the clip; removing it forgets it.
  l.sounds[0].src = std::make_shared<const std::string>("data:audio/wav;base64,BBBB");
  bank.sync_assets(l);
  CHECK(!bank.ready("snd1"));
  CHECK(bank.clip("snd1") == 0);
  CHECK(bank.pending_imports().size() == 1);
  bank.set_failed("snd1", "corrupt");
  CHECK(bank.failed("snd1") && bank.pending_imports().empty());
  l.sounds.clear();
  bank.sync_assets(l);
  CHECK(!bank.failed("snd1"));
  // Unknown built-ins fail at once, silence references are ignored.
  bank.request("builtin:nope");
  CHECK(bank.failed("builtin:nope"));
  CHECK(bank.clip("") == 0 && bank.clip("none") == 0 && bank.queued() == 0);
}

// ---------------------------------------------------------------------------
// Network

namespace {
Quat qnorm(Quat q) {
  const f32 l = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
  return {q.x / l, q.y / l, q.z / l, q.w / l};
}
struct NetSample {
  i32 health = 0;
  bool alive = false;
  f32 speed = 0, x = 0;
  Vec3 pos;
  Quat rot;
  std::string name;
  u16 seq = 0;
  template <class S>
  bool serialize(S& s) {
    return net::serialize_int(s, health, -5, 100) && net::serialize_bool(s, alive) && net::serialize_float(s, speed) &&
           net::serialize_compressed_float(s, x, -512, 512, 0.01f) && net::serialize_compressed_vec3(s, pos, 1000, 0.001f) &&
           net::serialize_quat(s, rot, 12) && net::serialize_string(s, name, 64) && net::serialize_sequence_relative(s, 100, seq);
  }
};
}  // namespace

TEST(net_serialization) {
  NetSample a;
  a.health = 73;
  a.alive = true;
  a.speed = 12.25f;
  a.x = -311.237f;
  a.pos = {12.3456f, -987.6543f, 0.001f};
  a.rot = qnorm(Quat{0.3f, -0.5f, 0.1f, 0.8f});
  a.name = "Hover Car 2";
  a.seq = 117;
  std::vector<u8> bytes;
  CHECK(net::write_message(a, bytes, 256));
  net::Message m;
  m.data = bytes;
  NetSample b;
  CHECK(net::read(m, b));
  CHECK(b.health == 73 && b.alive && b.speed == 12.25f && b.name == "Hover Car 2" && b.seq == 117);
  CHECK_NEAR(b.x, a.x, 0.006f);
  CHECK_NEAR(b.pos.x, a.pos.x, 0.0006f);
  CHECK_NEAR(b.pos.y, a.pos.y, 0.0006f);
  const f32 dq = std::fabs(a.rot.x * b.rot.x + a.rot.y * b.rot.y + a.rot.z * b.rot.z + a.rot.w * b.rot.w);
  CHECK(dq > 0.99999f);
  // The measure stream bounds the written size.
  net::MeasureStream ms;
  CHECK(a.serialize(ms));
  CHECK(ms.bytes_processed() >= static_cast<int>(bytes.size()));
  // Out-of-range values refuse to write; truncated or corrupt data refuses to read.
  NetSample bad = a;
  bad.health = 101;
  CHECK(!net::write_message(bad, bytes, 256));
  for (std::size_t cut = 0; cut + 1 < m.data.size(); ++cut) {
    net::Message t;
    t.data.assign(m.data.begin(), m.data.begin() + static_cast<std::ptrdiff_t>(cut));
    NetSample c;
    CHECK(!net::read(t, c));
  }
  // Random garbage never crashes (and mostly fails).
  u32 seed = 12345;
  int accepted = 0;
  for (int i = 0; i < 2000; ++i) {
    net::Message g;
    g.data.resize(1 + i % 60);
    for (u8& x : g.data) { seed = seed * 1664525u + 1013904223u; x = static_cast<u8>(seed >> 24); }
    NetSample c;
    accepted += net::read(g, c);
  }
  CHECK(accepted < 2000);
  // Bits needed and wrap-around sequences.
  CHECK(net::bits_required(0, 1) == 1 && net::bits_required(0, 255) == 8 && net::bits_required(0, 256) == 9);
  CHECK(net::sequence_greater(1, 65535) && net::sequence_less(65535, 1) && !net::sequence_greater(1, 1));
  CHECK(net::crc32("123456789", 9) == 0xCBF43926u);
  // Quaternion edge cases (negative w, axis-aligned).
  for (Quat q : {Quat{0, 0, 0, 1}, Quat{0, 0, 0, -1}, Quat{1, 0, 0, 0}, qnorm(Quat{-0.7f, 0.7f, 0.1f, -0.1f})}) {
    std::vector<u8> buf(16);
    net::WriteStream w(buf.data(), 16);
    CHECK(net::serialize_quat(w, q, 10));
    w.flush();
    net::ReadStream r(buf.data(), 16);
    Quat out;
    CHECK(net::serialize_quat(r, out, 10));
    CHECK(std::fabs(q.x * out.x + q.y * out.y + q.z * out.z + q.w * out.w) > 0.9995f);
  }
}

namespace {
/// Two connections joined by a simulated wire.
struct NetPair {
  net::Simulator sim{7};
  net::Connection a, b;
  f64 time = 0;
  explicit NetPair(const net::ConnectionConfig& c = {}) : a(c), b(c) {
    a.transmit = [](void* ctx, const u8* d, int n) { static_cast<NetPair*>(ctx)->sim.send({1}, {2}, d, n, static_cast<NetPair*>(ctx)->time); };
    a.transmit_ctx = this;
    b.transmit = [](void* ctx, const u8* d, int n) { static_cast<NetPair*>(ctx)->sim.send({2}, {1}, d, n, static_cast<NetPair*>(ctx)->time); };
    b.transmit_ctx = this;
  }
  void step(f64 dt = 1.0 / 60) {
    time += dt;
    a.advance_time(time);
    b.advance_time(time);
    a.send_packet();
    b.send_packet();
    std::vector<net::Simulator::Packet> due;
    sim.take_due(time, due);
    for (auto& p : due) (p.to.id == 1 ? a : b).receive_packet(p.data.data(), static_cast<int>(p.data.size()));
  }
};
}  // namespace

TEST(net_endpoint_acks_and_fragments) {
  struct Ctx {
    std::vector<std::vector<u8>> wire;
    std::vector<std::vector<u8>> got;
  } ca, cb;
  net::EndpointConfig cfg;
  net::Endpoint a(cfg, [](void* c, u16, const u8* d, int n) { static_cast<Ctx*>(c)->wire.emplace_back(d, d + n); },
                  [](void* c, u16, const u8* d, int n) { static_cast<Ctx*>(c)->got.emplace_back(d, d + n); return true; }, &ca);
  net::Endpoint b(cfg, [](void* c, u16, const u8* d, int n) { static_cast<Ctx*>(c)->wire.emplace_back(d, d + n); },
                  [](void* c, u16, const u8* d, int n) { static_cast<Ctx*>(c)->got.emplace_back(d, d + n); return true; }, &cb);
  // 300 packets from a to b; every third lost; b answers each received one.
  u32 lost = 0;
  for (int i = 0; i < 300; ++i) {
    const u8 payload[3] = {static_cast<u8>(i), static_cast<u8>(i >> 8), 0x5a};
    a.update(i / 60.0);
    b.update(i / 60.0);
    a.send_packet(payload, 3);
    for (auto& pkt : ca.wire) {
      if (i % 3 == 2) { ++lost; continue; }
      b.receive_packet(pkt.data(), static_cast<int>(pkt.size()));
    }
    ca.wire.clear();
    b.send_packet(payload, 1);
    for (auto& pkt : cb.wire) a.receive_packet(pkt.data(), static_cast<int>(pkt.size()));
    cb.wire.clear();
  }
  CHECK(cb.got.size() == 300 - lost);
  CHECK(a.stats().packets_acked >= 300 - lost - 1);
  CHECK(a.stats().packet_loss > 20 && a.stats().packet_loss < 45);
  // A 9 KB packet goes as fragments and arrives whole; duplicates are ignored.
  std::vector<u8> big(9000);
  for (std::size_t i = 0; i < big.size(); ++i) big[i] = static_cast<u8>(i * 7);
  cb.got.clear();
  a.send_packet(big.data(), static_cast<int>(big.size()));
  CHECK(ca.wire.size() == 9);
  for (int k = 8; k >= 0; --k) b.receive_packet(ca.wire[static_cast<std::size_t>(k)].data(), static_cast<int>(ca.wire[static_cast<std::size_t>(k)].size()));
  for (auto& pkt : ca.wire) b.receive_packet(pkt.data(), static_cast<int>(pkt.size()));
  ca.wire.clear();
  CHECK(cb.got.size() == 1 && cb.got[0] == big);
  // Garbage is counted, never crashes.
  u32 seed = 99;
  for (int i = 0; i < 3000; ++i) {
    std::vector<u8> g(1 + i % 200);
    for (u8& x : g) { seed = seed * 1664525u + 1013904223u; x = static_cast<u8>(seed >> 24); }
    b.receive_packet(g.data(), static_cast<int>(g.size()));
  }
  CHECK(b.stats().packets_invalid + b.stats().fragments_invalid + b.stats().packets_stale > 0);
}

TEST(net_reliable_channel_under_bad_network) {
  NetPair p;
  p.sim.settings.latency_ms = 60;
  p.sim.settings.jitter_ms = 30;
  p.sim.settings.loss_percent = 25;
  p.sim.settings.duplicate_percent = 10;
  // 400 small messages, three 6 KB blocks in between, and unreliable chatter.
  std::vector<std::vector<u8>> sent;
  int queued = 0;
  for (int frame = 0; frame < 2400; ++frame) {
    for (int k = 0; k < 2 && queued < 403; ++k) {
      if (!p.a.can_send(0)) break;
      std::vector<u8> data;
      if (queued % 130 == 64) { data.resize(6000); for (std::size_t i = 0; i < data.size(); ++i) data[i] = static_cast<u8>(i ^ queued); }
      else data = {static_cast<u8>(queued), static_cast<u8>(queued >> 8), 1, 2, 3};
      sent.push_back(data);
      CHECK(p.a.send(0, static_cast<u16>(queued % 7), data));
      ++queued;
    }
    p.a.send(1, 3, std::vector<u8>{9, 9});
    p.step();
  }
  std::vector<std::vector<u8>> received;
  while (auto m = p.b.receive(0)) {
    CHECK(m->type == received.size() % 7);
    received.push_back(m->data);
  }
  CHECK(p.a.error() == net::ConnectionError::none && p.b.error() == net::ConnectionError::none);
  CHECK(received.size() == sent.size());
  CHECK(received == sent);
  int unreliable = 0;
  while (p.b.receive(1)) ++unreliable;
  CHECK(unreliable > 1000 && unreliable < 2400);  // some lost, never duplicated beyond the sends
  CHECK(p.a.stats().rtt_ms > 80 && p.a.stats().rtt_ms < 250);
  CHECK(p.a.pending(0) == 0);
  // Typed messages.
  NetSample s;
  s.health = 5;
  s.name = "x";
  CHECK(p.a.send(0, 42, s));
  for (int i = 0; i < 120; ++i) p.step();
  auto m = p.b.receive(0);
  NetSample back;
  CHECK(m && m->type == 42 && net::read(*m, back) && back.health == 5 && back.name == "x");
}

TEST(net_client_server) {
  net::LoopbackNetwork hub(3);
  hub.simulator().settings.latency_ms = 40;
  hub.simulator().settings.jitter_ms = 10;
  hub.simulator().settings.loss_percent = 10;
  auto st = hub.create();
  net::ClientServerConfig cfg;
  net::Server server(*st, cfg, 1);
  server.start(2);
  std::vector<std::unique_ptr<net::Transport>> ts;
  std::vector<std::unique_ptr<net::Client>> clients;
  for (int i = 0; i < 3; ++i) {
    ts.push_back(hub.create());
    clients.push_back(std::make_unique<net::Client>(*ts.back(), cfg, 100 + i));
  }
  f64 t = 0;
  auto tick = [&](int n) {
    for (int k = 0; k < n; ++k) {
      t += 1.0 / 60;
      hub.advance_time(t);
      server.advance_time(t);
      server.receive_packets();
      for (auto& c : clients) { c->advance_time(t); c->receive_packets(); }
      server.send_packets();
      for (auto& c : clients) c->send_packets();
    }
  };
  clients[0]->connect(st->address(), t);
  clients[1]->connect(st->address(), t);
  tick(90);
  CHECK(clients[0]->connected() && clients[1]->connected());
  CHECK(clients[0]->client_index() != clients[1]->client_index());
  CHECK(server.connected_clients() == 2);
  auto events = server.take_events();
  CHECK(events.size() == 2 && events[0].second && events[1].second);
  // The server is full: the third client is denied.
  clients[2]->connect(st->address(), t);
  tick(90);
  CHECK(clients[2]->state() == net::ClientState::denied);
  // Messages both ways.
  CHECK(clients[0]->send(0, 1, std::vector<u8>{1, 2, 3}));
  server.broadcast(0, 2, NetSample{});
  tick(60);
  const int i0 = clients[0]->client_index();
  auto m = server.receive(i0, 0);
  CHECK(m && m->type == 1 && m->data == std::vector<u8>({1, 2, 3}));
  CHECK(clients[0]->receive(0).has_value() && clients[1]->receive(0).has_value());
  // A client that disconnects frees its slot; a silent one times out.
  clients[1]->disconnect();
  tick(30);
  CHECK(server.connected_clients() == 1);
  ts[0].reset();  // client 0 vanishes without a word
  clients[0].reset();
  clients.erase(clients.begin());
  tick(static_cast<int>(cfg.timeout * 60) + 30);
  CHECK(server.connected_clients() == 0);
  // Spoofed and garbage datagrams are ignored.
  auto attacker = hub.create();
  u32 seed = 5;
  for (int i = 0; i < 500; ++i) {
    std::vector<u8> g(20 + i % 1300);
    for (u8& x : g) { seed = seed * 1664525u + 1013904223u; x = static_cast<u8>(seed >> 24); }
    attacker->send(st->address(), g.data(), static_cast<int>(g.size()));
  }
  tick(30);
  CHECK(server.connected_clients() == 0);
  // Reconnect works after all that.
  clients[1]->connect(st->address(), t);
  tick(90);
  CHECK(clients[1]->connected());
  server.stop();
  tick(30);
  CHECK(!clients[1]->connected());
}

TEST(net_game_server_and_clients) {
  Renderer renderer;
  game::NightWindows windows;
  ShaderExtension* ext[1] = {&windows};
  RendererOptions o;
  renderer.init(o, Span<ShaderExtension* const>(ext, 1));
  game::Level l = game::basic_level();
  game::Actor ball;
  ball.name = "Ball";
  ball.transform.position = {0, 0, 0.6f};
  ball.mesh = game::MeshComponent{};
  ball.mesh->shape = game::Shape::sphere;
  ball.body = game::BodyComponent{};
  ball.controller = game::ControllerComponent{};
  ball.controller->mode = game::ControlMode::rolling;
  ball.controller->jump_speed = 5;
  ball.body->linear_damping = 1.5f;
  const game::ActorId ball_id = l.add(ball).id;
  for (int i = 0; i < 3; ++i) {
    game::Actor orb;
    orb.name = "Orb " + std::to_string(i);
    orb.transform.position = {8.0f + 4 * i, 0, 0.6f};
    orb.pickup = game::PickupComponent{};
    game::ColliderComponent c;
    c.shape = game::ColliderShape::sphere;
    c.radius = 7.0f;  // wide enough for every pawn's lane
    c.sensor = true;
    orb.colliders.push_back(c);
    l.add(orb);
  }
  game::Actor crate;
  crate.name = "Crate";
  crate.transform.position = {-5, 3, 3};
  crate.mesh = game::MeshComponent{};
  crate.body = game::BodyComponent{};
  const game::ActorId crate_id = l.add(crate).id;
  l.world.goal = game::GameGoal::collect_all;

  net::LoopbackNetwork hub(11);
  hub.simulator().settings.latency_ms = 30;
  hub.simulator().settings.jitter_ms = 10;
  hub.simulator().settings.loss_percent = 5;
  auto ts = hub.create(), t1 = hub.create(), t2 = hub.create();
  game::GameWorld server_world(renderer, &windows), w1(renderer, &windows), w2(renderer, &windows);
  game::NetGame server(server_world), c1(w1), c2(w2);
  CHECK(server.host(l, *ts));
  CHECK(c1.join(l, *t1, ts->address()));
  CHECK(c2.join(l, *t2, ts->address()));
  f64 time = 0;
  game::PlayInput idle, forward1, host_in;
  auto frame = [&](const game::PlayInput& a, const game::PlayInput& b) {
    time += 1.0 / 60;
    hub.advance_time(time);
    server.update(time, 1.0f / 60, host_in);
    c1.update(time, 1.0f / 60, a);
    c2.update(time, 1.0f / 60, b);
    server_world.sound_cues().clear();
    w1.sound_cues().clear();
    w2.sound_cues().clear();
  };
  for (int i = 0; i < 120; ++i) frame(idle, idle);
  CHECK(c1.playing() && c2.playing());
  CHECK(server.stats().players == 3);
  CHECK(server_world.pawns().size() == 3);
  CHECK(w1.pawns().size() == 3 && w2.pawns().size() == 3);  // everyone sees everyone
  const game::ActorId p1 = w1.player(), p2 = w2.player();
  CHECK(p1 != ball_id && p2 != ball_id && p1 != p2);
  CHECK(w1.pawn_source(p1) == ball_id);
  // Client 1 rolls forward (+X, its camera looks along the ball's facing).
  forward1.move_y = 1;
  Mat4 m0;
  CHECK(server_world.actor_matrix(p1, m0));
  for (int i = 0; i < 240; ++i) frame(forward1, idle);
  Mat4 ms, mr1, mr2;
  CHECK(server_world.actor_matrix(p1, ms));
  CHECK(ms.translation().x > m0.translation().x + 3);  // the server moved client 1's pawn
  for (int i = 0; i < 180; ++i) frame(idle, idle);    // roll out
  CHECK(server_world.actor_matrix(p1, ms));
  // Both clients show it close to where the server has it (they render ~100 ms behind).
  CHECK(w1.actor_matrix(p1, mr1) && w2.actor_matrix(p1, mr2));
  CHECK(distance(mr1.translation(), ms.translation()) < 2.5f);
  CHECK(distance(mr2.translation(), ms.translation()) < 2.5f);
  // The crate fell on the server; the clients show it resting too.
  Mat4 cs, cr;
  CHECK(server_world.actor_matrix(crate_id, cs) && w2.actor_matrix(crate_id, cr));
  CHECK(cs.translation().z < 1 && distance(cs.translation(), cr.translation()) < 0.05f);
  // Client 1 collected orbs on the way: the server counted them and both clients know.
  CHECK(server_world.hud().pickups >= 1);
  for (int i = 0; i < 30; ++i) frame(idle, idle);
  CHECK(w1.hud().pickups == server_world.hud().pickups && w2.hud().pickups == server_world.hud().pickups);
  CHECK(w2.dead_actors().size() == server_world.dead_actors().size());
  CHECK(c1.stats().snapshots_received > 100);
  CHECK(c1.stats().rtt_ms > 40 && c1.stats().rtt_ms < 200);
  // Client 2 leaves: its pawn disappears on the server and for client 1.
  c2.stop();
  for (int i = 0; i < 60; ++i) frame(idle, idle);
  CHECK(server_world.pawns().size() == 2);
  CHECK(w1.pawns().size() == 2);
  CHECK(!w1.actor_matrix(p2, mr2));
  // A client with a different level is refused.
  game::Level other = l;
  other.world.title = "Something else";
  auto t3 = hub.create();
  game::GameWorld w3(renderer, &windows);
  game::NetGame c3(w3);
  CHECK(c3.join(other, *t3, ts->address()));
  for (int i = 0; i < 90; ++i) {
    time += 1.0 / 60;
    hub.advance_time(time);
    server.update(time, 1.0f / 60, host_in);
    c3.update(time, 1.0f / 60, idle);
    c1.update(time, 1.0f / 60, idle);
  }
  CHECK(!c3.playing());
  CHECK(c3.status().find("different level") != std::string::npos);
  c1.stop();
  c3.stop();
  server.stop();
  CHECK(!server_world.running() && !w1.running());
  renderer.shutdown();
}

// ---------------------------------------------------------------------------
// Kern Studio

struct StudioFixture {
  Renderer renderer;
  game::NightWindows windows;
  std::unique_ptr<studio::Studio> s;
  f64 now = 1000;
  explicit StudioFixture(bool player_only = false) {
    ShaderExtension* ext[1] = {&windows};
    RendererOptions o;
    o.shadows = true;
    renderer.init(o, Span<ShaderExtension* const>(ext, 1));
    s = std::make_unique<studio::Studio>();
    s->init(&renderer, &windows, player_only);
    for (u32 i = 0; i < 4; ++i) s->set_viewport_rect(i, {0, 0, 960, 540}, 960, 540, 1, i == 0);
  }
  ~StudioFixture() { s.reset(); renderer.shutdown(); }
  void frames(int n) { for (int i = 0; i < n; ++i) s->frame(now += 1000.0 / 60); }
  json::Value cmd(const char* text) {
    json::Value c;
    json::parse(text, c);
    return s->command(c);
  }
};

TEST(studio_starts_with_orb_run) {
  StudioFixture f;
  CHECK(f.s->level().name == "Orb Run");
  CHECK(f.s->play_state() == studio::PlayState::editing);
  f.frames(3);
  CHECK(f.renderer.mesh_count() > 30);
  const json::Value st = f.s->state();
  CHECK(st["level"]["name"].as_string() == "Orb Run");
  CHECK(st["outliner"]["rows"].size() > 5);
  CHECK(st["menus"]["file"].size() > 5);
  CHECK(st["viewports"].size() == 4);
  // The orbs live in a folder.
  bool folder = false;
  for (const json::Value& r : st["outliner"]["rows"].items()) folder |= r["folder"].as_string() == "Orbs";
  CHECK(folder);
  const json::Value status = f.s->status();
  CHECK(status["play"].as_string() == "editing");
  CHECK(status["camera"]["position"].size() == 3);
}

TEST(studio_fly_and_orbit) {
  StudioFixture f;
  f.frames(1);
  studio::Viewport& v = f.s->viewport(0);
  const Vec3 start = v.camera.position;
  const Vec3 fwd = v.camera.forward();
  // Right mouse + W flies forward.
  f.s->pointer(0, 0, 480, 270, 2, 2, 0, 0);
  CHECK(v.drag == studio::Drag::fly);
  f.s->key("w", "KeyW", true, 0, false);
  f.frames(30);
  f.s->key("w", "KeyW", false, 0, false);
  const Vec3 moved = v.camera.position - start;
  CHECK(dot(moved, fwd) > 1);
  // W while flying did not switch the tool to translate... and mouse look turns.
  const f32 yaw = v.camera.yaw;
  f.s->pointer(0, 1, 520, 270, -1, 2, 0, 0);
  CHECK(v.camera.yaw < yaw);
  // The wheel while flying changes the camera speed.
  const i32 speed = f.s->preferences().camera_speed;
  f.s->pointer(0, 4, 520, 270, -1, 2, 0, -100);
  CHECK(f.s->preferences().camera_speed == min_of(speed + 1, 8));
  f.s->pointer(0, 2, 520, 270, 2, 0, 0, 0);
  CHECK(v.drag == studio::Drag::none);
  // Alt + LMB orbits around the selection, keeping the distance.
  f.cmd(R"({"cmd":"select","ids":[1]})");
  const game::ActorId car = [&] { for (const game::Actor& a : f.s->level().actors) if (a.controller) return a.id; return 0u; }();
  f.s->select({car});
  f.s->focus_selection();
  Aabb box;
  f.s->world().actor_bounds(car, box);
  const f32 d0 = distance(v.camera.position, box.center());
  // (Away from the gizmo in the middle: Alt-dragging a gizmo handle duplicates.)
  f.s->pointer(0, 0, 200, 150, 0, 1, 4, 0);
  CHECK(v.drag == studio::Drag::orbit);
  f.s->pointer(0, 1, 280, 180, -1, 1, 4, 0);
  f.s->pointer(0, 2, 280, 180, 0, 0, 4, 0);
  CHECK_NEAR(distance(v.camera.position, box.center()), d0, d0 * 0.05f + 0.1f);
  // F frames the selection: it ends up in the middle of the view.
  f.s->key("f", "KeyF", true, 0, false);
  f.s->key("f", "KeyF", false, 0, false);
  f32 sx = 0, sy = 0;
  CHECK(f.s->to_screen(v, box.center(), sx, sy));
  CHECK_NEAR(sx, 480, 5);
  CHECK_NEAR(sy, 270, 5);
}

TEST(studio_spawn_every_catalog_item_and_simulate) {
  StudioFixture f;
  f.cmd(R"({"cmd":"new","template":"basic"})");
  f.frames(1);
  const json::Value cat = f.cmd(R"({"cmd":"catalog"})");
  u32 spawned = 0;
  f32 x = -60;
  for (const json::Value& g : cat["groups"].items())
    for (const json::Value& it : g["items"].items()) {
      if (it["id"].as_string() == "city") continue;  // big; covered by Orb Run
      json::Value c = json::Value::object();
      c.set("cmd", "spawn");
      c.set("item", it["id"]);
      json::Value at = json::Value::array();
      at.push(static_cast<f64>(x));
      at.push(0.0);
      at.push(0.0);
      c.set("at", at);
      x += 8;
      const json::Value r = f.s->command(c);
      CHECK(r["ok"].as_bool());
      if (r["ok"].as_bool()) ++spawned;
    }
  CHECK(spawned >= 30);
  bool errors = false;
  for (const game::Issue& i : game::validate(f.s->level())) errors |= i.error;
  CHECK(!errors);
  f.frames(2);
  CHECK(f.s->play(game::SimMode::simulate));
  f.frames(60);
  const json::Value st = f.s->status();
  CHECK(st["sim"]["bodies"].as_number() > 20);
  CHECK(st["sim"]["joints"].as_number() >= 10);
  f.s->stop();
  CHECK(f.s->play_state() == studio::PlayState::editing);
  // Spawning is undoable.
  const std::size_t n = f.s->level().actors.size();
  f.s->spawn("cube", Vec3{0, 0, 0});
  CHECK(f.s->level().actors.size() == n + 1);
  f.s->undo();
  CHECK(f.s->level().actors.size() == n);
  f.s->redo();
  CHECK(f.s->level().actors.size() == n + 1);
}

TEST(studio_gizmo_translate_and_undo) {
  StudioFixture f;
  f.cmd(R"({"cmd":"new","template":"basic"})");
  f.frames(1);
  const game::ActorId id = f.s->spawn("cube", Vec3{0, 0, 0});
  CHECK(id != 0);
  studio::Viewport& v = f.s->viewport(0);
  v.camera.position = {-10, -10, 8};
  v.camera.yaw = 45;
  v.camera.pitch = -30;
  f.s->set_tool(studio::Tool::translate);
  f.s->snap().grid = false;
  f.frames(2);
  const Vec3 before = f.s->level().find(id)->transform.position;
  // Grab the X axis handle a little outside the centre.
  Vec3 pivot;
  Mat4 m;
  f.s->world().actor_matrix(id, m);
  pivot = m.translation();
  f32 cx, cy, ax, ay;
  CHECK(f.s->to_screen(v, pivot, cx, cy));
  CHECK(f.s->to_screen(v, pivot + Vec3{1, 0, 0}, ax, ay));
  const f32 len = std::hypot(ax - cx, ay - cy);
  const f32 hx = cx + (ax - cx) / len * 60, hy = cy + (ay - cy) / len * 60;
  CHECK(f.s->gizmo_hit(v, hx, hy) == 1);
  f.s->pointer(0, 0, hx, hy, 0, 1, 0, 0);
  CHECK(v.drag == studio::Drag::gizmo);
  f.s->pointer(0, 1, hx + (ax - cx) / len * 80, hy + (ay - cy) / len * 80, -1, 1, 0, 0);
  f.s->pointer(0, 2, hx + (ax - cx) / len * 80, hy + (ay - cy) / len * 80, 0, 0, 0, 0);
  const Vec3 after = f.s->level().find(id)->transform.position;
  CHECK(after.x > before.x + 0.3f);
  CHECK_NEAR(after.y, before.y, 1e-3f);
  CHECK_NEAR(after.z, before.z, 1e-3f);
  f.s->key("z", "KeyZ", true, 2, false);
  CHECK_NEAR(f.s->level().find(id)->transform.position.x, before.x, 1e-4f);
  f.s->key("y", "KeyY", true, 2, false);
  CHECK_NEAR(f.s->level().find(id)->transform.position.x, after.x, 1e-4f);
  // Alt-drag duplicates.
  const std::size_t n = f.s->level().actors.size();
  f.s->pointer(0, 0, hx, hy, 0, 1, 0, 0);
  f.s->pointer(0, 2, hx, hy, 0, 0, 0, 0);
  f.frames(1);
  f.s->world().actor_matrix(id, m);
  CHECK(f.s->to_screen(v, m.translation(), cx, cy));
  CHECK(f.s->to_screen(v, m.translation() + Vec3{1, 0, 0}, ax, ay));
  const f32 l2 = std::hypot(ax - cx, ay - cy);
  const f32 gx = cx + (ax - cx) / l2 * 60, gy = cy + (ay - cy) / l2 * 60;
  f.s->pointer(0, 0, gx, gy, 0, 1, 4, 0);
  f.s->pointer(0, 1, gx + 40, gy, -1, 1, 4, 0);
  f.s->pointer(0, 2, gx + 40, gy, 0, 0, 4, 0);
  CHECK(f.s->level().actors.size() == n + 1);
}

TEST(studio_left_mouse_selects_and_places_on_surfaces) {
  StudioFixture f;
  f.cmd(R"({"cmd":"new","template":"basic"})");
  f.frames(1);
  const game::ActorId a = f.s->spawn("cube", Vec3{0, 0, 0});
  const game::ActorId b = f.s->spawn("cube", Vec3{4, 0, 0});
  CHECK(a && b);
  studio::Viewport& v = f.s->viewport(0);
  v.camera.position = {-8, -8, 7};
  v.camera.yaw = 45;
  v.camera.pitch = -32;
  f.s->set_tool(studio::Tool::select);
  f.s->snap().grid = false;
  f.s->deselect();
  f.frames(2);
  const studio::ViewCamera cam_before = v.camera;
  auto center = [&](game::ActorId id) {
    Mat4 m;
    f.s->world().actor_matrix(id, m);
    return m.translation();
  };
  const Vec3 a0 = f.s->level().find(a)->transform.position;
  // A click on an actor selects it and does not move anything (the camera neither).
  f32 sx, sy;
  CHECK(f.s->to_screen(v, center(a), sx, sy));
  f.s->pointer(0, 0, sx, sy, 0, 1, 0, 0);
  f.s->pointer(0, 1, sx + 1, sy, -1, 1, 0, 0);
  f.s->pointer(0, 2, sx + 1, sy, 0, 0, 0, 0);
  CHECK(f.s->selection().size() == 1 && f.s->selection()[0] == a);
  CHECK_NEAR(f.s->level().find(a)->transform.position.x, a0.x, 1e-5f);
  CHECK_NEAR(v.camera.position.x, cam_before.position.x, 1e-5f);
  CHECK_NEAR(v.camera.yaw, cam_before.yaw, 1e-5f);
  const std::size_t steps = f.s->history().size();
  // Dragging the actor onto the other cube stacks it on top.
  f32 tx, ty;
  Aabb bb;
  CHECK(f.s->world().actor_bounds(b, bb));
  CHECK(f.s->to_screen(v, Vec3{center(b).x, center(b).y, bb.max.z}, tx, ty));
  for (int k = 1; k <= 10; ++k) {
    if (k == 1) f.s->pointer(0, 0, sx, sy, 0, 1, 0, 0);
    f.s->pointer(0, 1, sx + (tx - sx) * k / 10, sy + (ty - sy) * k / 10, -1, 1, 0, 0);
  }
  f.s->pointer(0, 2, tx, ty, 0, 0, 0, 0);
  f.frames(1);
  Aabb ab;
  CHECK(f.s->world().actor_bounds(a, ab));
  CHECK_NEAR(ab.min.z, bb.max.z, 0.05f);
  CHECK(std::fabs(center(a).x - center(b).x) < 0.8f);
  CHECK(f.s->history().size() == steps + 1);
  CHECK_NEAR(v.camera.position.x, cam_before.position.x, 1e-5f);
  // Back onto the ground: the bottom rests on it.
  f32 gx, gy;
  CHECK(f.s->to_screen(v, center(a), sx, sy));
  CHECK(f.s->to_screen(v, Vec3{1, -3, 0}, gx, gy));
  f.s->pointer(0, 0, sx, sy, 0, 1, 0, 0);
  for (int k = 1; k <= 10; ++k) f.s->pointer(0, 1, sx + (gx - sx) * k / 10, sy + (gy - sy) * k / 10, -1, 1, 0, 0);
  f.s->pointer(0, 2, gx, gy, 0, 0, 0, 0);
  f.frames(1);
  CHECK(f.s->world().actor_bounds(a, ab));
  CHECK_NEAR(ab.min.z, 0.0f, 0.05f);
  // Undo twice returns to the start.
  f.s->key("z", "KeyZ", true, 2, false);
  f.s->key("z", "KeyZ", true, 2, false);
  CHECK_NEAR(f.s->level().find(a)->transform.position.x, a0.x, 1e-4f);
  CHECK_NEAR(f.s->level().find(a)->transform.position.z, a0.z, 1e-4f);
  // Escape cancels a drag in progress.
  f.frames(1);
  CHECK(f.s->to_screen(v, center(a), sx, sy));
  f.s->pointer(0, 0, sx, sy, 0, 1, 0, 0);
  f.s->pointer(0, 1, sx + 80, sy + 30, -1, 1, 0, 0);
  CHECK(std::fabs(f.s->level().find(a)->transform.position.x - a0.x) > 0.1f || std::fabs(f.s->level().find(a)->transform.position.y - a0.y) > 0.1f);
  f.s->key("Escape", "Escape", true, 0, false);
  f.s->pointer(0, 2, sx + 80, sy + 30, 0, 0, 0, 0);
  CHECK_NEAR(f.s->level().find(a)->transform.position.x, a0.x, 1e-4f);
  // Dragging empty space draws a marquee and selects what is inside.
  f.s->deselect();
  f.s->pointer(0, 0, 2, 2, 0, 1, 0, 0);
  CHECK(v.drag == studio::Drag::marquee);
  f.s->pointer(0, 1, v.css_width * 0.5f, v.css_height * 0.5f, -1, 1, 0, 0);
  f.s->pointer(0, 1, v.css_width - 2, v.css_height - 2, -1, 1, 0, 0);
  f.s->pointer(0, 2, v.css_width - 2, v.css_height - 2, 0, 0, 0, 0);
  bool has_a = false, has_b = false;
  for (game::ActorId id : f.s->selection()) { has_a |= id == a; has_b |= id == b; }
  CHECK(has_a && has_b);
  CHECK_NEAR(v.camera.position.x, cam_before.position.x, 1e-5f);
  // A click into empty space deselects.
  f.s->pointer(0, 0, 2, 2, 0, 1, 0, 0);
  f.s->pointer(0, 2, 2, 2, 0, 0, 0, 0);
  CHECK(f.s->selection().empty());
}

TEST(studio_audio_integration) {
  StudioFixture f;
  CHECK(!f.s->audio_ready());
  std::vector<f32> buf(1024 * 2, 1.0f);
  f.s->audio_render(buf.data(), 1024);
  CHECK(buf[0] == 0 && buf[2047] == 0);  // silent before init
  CHECK(f.s->audio_init(48000));
  CHECK(f.s->audio_ready());
  f.cmd(R"({"cmd":"new","template":"basic"})");
  // The details panel offers the Audio Source component and sound fields.
  const game::ActorId id = f.s->spawn("audio_source", Vec3{0, 0, 0});
  CHECK(id != 0 && f.s->level().find(id)->audio);
  json::Value st = f.s->state();
  bool section = false, sound_field = false;
  for (const json::Value& sec : st["details"]["sections"].items()) {
    if (sec["id"].as_string() != "audio") continue;
    section = true;
    for (const json::Value& fd : sec["fields"].items()) sound_field |= fd["type"].as_string() == "sound" && fd["value"].as_string() == "builtin:hum";
  }
  CHECK(section && sound_field);
  CHECK(st["sounds"]["builtin"].size() >= 25);
  CHECK(st["sounds"]["ready"].as_bool());
  // Editing sound properties is undoable like any other edit.
  CHECK(f.s->set_property("components.audio.sound", json::Value("builtin:wind")));
  CHECK(f.s->level().find(id)->audio->sound == "builtin:wind");
  CHECK(f.s->set_property("components.audio.bus", json::Value("ambience")));
  CHECK(!f.s->set_property("components.audio.bus", json::Value("master")));
  f.s->undo();
  CHECK(f.s->level().find(id)->audio->bus == audio::Bus::sfx);
  CHECK(f.cmd(R"({"cmd":"setWorld","path":"world.music","value":"builtin:music_drive"})")["ok"].as_bool());
  // Preview plays on the UI bus once the clip is ready.
  f.cmd(R"({"cmd":"previewSound","sound":"builtin:quack"})");
  for (int i = 0; i < 120 && !(f.s->mixer().stats().voices > 0); ++i) { f.frames(1); f.s->audio_render(buf.data(), 800); }
  CHECK(f.s->mixer().stats().voices > 0);
  CHECK(f.s->state()["sounds"]["previewing"].as_bool());
  f.cmd(R"({"cmd":"stopSound"})");
  for (int i = 0; i < 4; ++i) f.s->audio_render(buf.data(), 1024);
  CHECK(f.s->mixer().stats().voices == 0);
  // Importing a sound: an undo step, the host is asked for PCM, the clip plays.
  const json::Value imp = f.cmd(R"({"cmd":"importSound","name":"Blip","src":"data:audio/wav;base64,UklGRg==","duration":0.25})");
  CHECK(imp["ok"].as_bool());
  const std::string sid = imp["id"].as_string();
  CHECK(f.s->level().sound(sid));
  CHECK(f.s->set_property("components.audio.sound", json::Value(sid)));
  f.frames(2);
  const std::vector<std::string> pending = f.s->pending_sounds();
  CHECK(std::find(pending.begin(), pending.end(), sid) != pending.end());
  CHECK(f.s->sound_source(sid).rfind("data:audio/wav", 0) == 0);
  const std::vector<f32> pcm = sine(44100, 0.25f, 880);
  CHECK(f.s->sound_pcm(sid, pcm.data(), static_cast<u32>(pcm.size()), 1, 44100));
  CHECK(f.s->pending_sounds().empty());
  // Simulate: the audio source starts, the world's music too.
  for (int i = 0; i < 400 && f.s->sound_bank().queued(); ++i) f.frames(1);
  CHECK(f.s->play(game::SimMode::simulate));
  for (int i = 0; i < 30; ++i) { f.frames(1); f.s->audio_render(buf.data(), 800); }
  CHECK(f.s->game_audio().active());
  CHECK(f.s->mixer().stats().voices >= 2);
  json::Value status = f.s->status();
  CHECK(status["audio"]["voices"].as_number() >= 2);
  // Mute is a master switch.
  f.cmd(R"({"cmd":"mute","muted":true})");
  CHECK(f.s->muted());
  for (int i = 0; i < 4; ++i) f.s->audio_render(buf.data(), 1024);
  f32 peak = 0;
  for (f32 x : buf) peak = std::max(peak, std::fabs(x));
  CHECK(peak < 1e-4f);
  f.cmd(R"({"cmd":"mute","muted":false})");
  f.s->stop();
  f.frames(2);
  CHECK(!f.s->game_audio().active());
  // Removing the sound is undoable too; references then warn.
  CHECK(f.cmd(("{\"cmd\":\"removeSound\",\"id\":\"" + sid + "\"}").c_str())["ok"].as_bool());
  CHECK(!f.s->level().sound(sid));
  bool warned = false;
  for (const game::Issue& i : game::validate(f.s->level())) warned |= i.text.find(sid) != std::string::npos;
  CHECK(warned);
  f.s->undo();
  CHECK(f.s->level().sound(sid));
  // The level saves and loads its sounds.
  const std::string text = f.s->level_text();
  CHECK(text.find("\"sounds\"") != std::string::npos && text.find("Blip") != std::string::npos);
}

TEST(studio_multiplayer_play) {
  StudioFixture f;
  CHECK(f.cmd(R"({"cmd":"netPlayers","players":3})")["ok"].as_bool());
  CHECK(f.cmd(R"({"cmd":"netSimulate","latency":40,"jitter":5,"loss":2})")["ok"].as_bool());
  CHECK(f.cmd(R"({"cmd":"play","mode":"play"})")["ok"].as_bool());
  CHECK(f.s->play_state() == studio::PlayState::playing);
  CHECK(f.s->prefs().layout == studio::Layout::quad);
  for (u32 i = 0; i < 4; ++i) f.s->set_viewport_rect(i, {static_cast<i32>(i % 2) * 480, static_cast<i32>(i / 2) * 270, 480, 270}, 480, 270, 1, true);
  f.frames(150);
  json::Value st = f.s->status();
  CHECK(st["net"]["role"].as_string() == "server");
  CHECK(st["net"]["players"].as_number() == 3);
  CHECK(st["net"]["clients"].size() == 2);
  CHECK(st["net"]["clients"][0]["hud"]["status"].as_string() == "playing");
  CHECK(f.s->world().pawns().size() == 3);
  CHECK(f.s->world_of(1).pawns().size() == 3 && f.s->world_of(2).pawns().size() == 3);
  // The active viewport's player drives: viewport 2 = client 2.
  f.cmd(R"({"cmd":"activeViewport","viewport":2})");
  const game::ActorId pawn = f.s->world_of(2).player();
  Mat4 a, b;
  CHECK(f.s->world().actor_matrix(pawn, a));
  f.s->key("w", "KeyW", true, 0, false);
  f.frames(120);
  f.s->key("w", "KeyW", false, 0, false);
  CHECK(f.s->world().actor_matrix(pawn, b));
  CHECK(distance(a.translation(), b.translation()) > 2);
  f.cmd(R"({"cmd":"stop"})");
  CHECK(f.s->play_state() == studio::PlayState::editing);
  CHECK(!f.s->status().find("net"));
  f.frames(2);
  // Back to one player: plain Play.
  f.cmd(R"({"cmd":"netPlayers","players":1})");
  CHECK(f.cmd(R"({"cmd":"play","mode":"play"})")["ok"].as_bool());
  f.frames(5);
  CHECK(!f.s->status().find("net"));
  f.cmd(R"({"cmd":"stop"})");
}

TEST(studio_shortcuts_and_commands) {
  StudioFixture f;
  f.frames(1);
  f.s->key("e", "KeyE", true, 0, false);
  CHECK(f.s->tool() == studio::Tool::rotate);
  f.s->key("r", "KeyR", true, 0, false);
  CHECK(f.s->tool() == studio::Tool::scale);
  f.s->key("w", "KeyW", true, 0, false);
  CHECK(f.s->tool() == studio::Tool::translate);
  f.s->key("w", "KeyW", false, 0, false);
  f.s->key("a", "KeyA", true, 2, false);
  CHECK(f.s->selection().size() == f.s->level().actors.size());
  f.s->key("Escape", "Escape", true, 0, false);
  CHECK(f.s->selection().empty());
  // Copy / paste / delete via commands.
  const game::ActorId cube = f.s->spawn("cube", Vec3{5, 5, 0});
  const std::size_t n = f.s->level().actors.size();
  f.s->key("d", "KeyD", true, 2, false);
  CHECK(f.s->level().actors.size() == n + 1);
  f.s->key("Delete", "Delete", true, 0, false);
  CHECK(f.s->level().actors.size() == n);
  f.s->select({cube});
  f.cmd(R"({"cmd":"copy"})");
  f.cmd(R"({"cmd":"paste"})");
  CHECK(f.s->level().actors.size() == n + 1);
  json::Value ui = f.s->take_ui();
  bool clip = false;
  for (const json::Value& a : ui.items()) clip |= a["action"].as_string() == "clipboard";
  CHECK(clip);
  // Properties and components.
  f.s->select({cube});
  CHECK(f.cmd(R"({"cmd":"set","path":"transform.position","value":[1,2,3]})")["ok"].as_bool());
  CHECK_NEAR(f.s->level().find(cube)->transform.position.y, 2, 1e-5f);
  CHECK(f.cmd(R"({"cmd":"set","path":"components.mesh.material.color","value":"#ff0000"})")["ok"].as_bool());
  CHECK(f.s->level().find(cube)->mesh->material.color.x > 0.99f);
  CHECK(f.cmd(R"({"cmd":"addComponent","type":"body"})")["ok"].as_bool());
  CHECK(f.s->level().find(cube)->body.has_value());
  CHECK(f.cmd(R"({"cmd":"set","path":"components.body.mass","value":42})")["ok"].as_bool());
  CHECK_NEAR(f.s->level().find(cube)->body->mass, 42, 1e-4f);
  CHECK(f.cmd(R"({"cmd":"removeComponent","type":"body","index":0})")["ok"].as_bool());
  CHECK(!f.s->level().find(cube)->body.has_value());
  CHECK(f.cmd(R"({"cmd":"setWorld","path":"world.timeOfDay","value":0.5})")["ok"].as_bool());
  CHECK_NEAR(f.s->level().world.time_of_day, 0.5f, 1e-4f);
  // Details JSON lists the sections of the selection.
  const json::Value st = f.s->state();
  CHECK(st["details"]["sections"].size() >= 3);
  // Connect two actors with a hinge.
  const game::ActorId a = f.s->spawn("physics_cube", Vec3{20, 0, 0});
  const game::ActorId b = f.s->spawn("cube", Vec3{21.5f, 0, 0});
  f.s->select({a, b});
  CHECK(f.cmd(R"({"cmd":"connect","kind":"hinge"})")["ok"].as_bool());
  CHECK(f.s->level().find(a)->joints.size() == 1 && f.s->level().find(a)->joints[0].target == b);
  // Every menu entry has a command object; unknown commands fail cleanly.
  for (const char* m : {"file", "edit", "window", "play", "help", "context"}) {
    const json::Value menu = f.cmd((std::string(R"({"cmd":"menu","name":")") + m + "\"}").c_str());
    CHECK(menu["entries"].size() > 0);
    for (const json::Value& e : menu["entries"].items()) CHECK(e["separator"].as_bool() || e["cmd"].is_object() || e["submenu"].is_array());
  }
  CHECK(!f.cmd(R"({"cmd":"nonsense"})")["ok"].as_bool());
  // Dialogs.
  for (const char* d : {"new", "preferences", "export", "history", "controls", "about", "rename"})
    CHECK(f.cmd((std::string(R"({"cmd":"dialog","name":")") + d + "\"}").c_str())["dialog"].is_object());
  CHECK(f.cmd(R"({"cmd":"submitDialog","name":"preferences","values":{"cameraSpeed":7,"gridSize":2}})")["ok"].as_bool());
  CHECK(f.s->preferences().camera_speed == 7 && f.s->snap().grid_size == 2);
  // View commands.
  CHECK(f.cmd(R"({"cmd":"layout","layout":"quad"})")["ok"].as_bool());
  CHECK(f.cmd(R"({"cmd":"viewType","viewport":1,"type":"left"})")["ok"].as_bool());
  CHECK(f.s->viewport(1).type == studio::ViewType::left);
  CHECK(f.cmd(R"({"cmd":"viewMode","viewport":0,"mode":"collision"})")["ok"].as_bool());
  CHECK(f.cmd(R"({"cmd":"showFlag","viewport":0,"flag":"colliders","value":true})")["ok"].as_bool());
  for (u32 i = 0; i < 4; ++i) f.s->set_viewport_rect(i, {static_cast<i32>(i % 2) * 480, static_cast<i32>(i / 2) * 270, 480, 270}, 480, 270, 1, true);
  f.frames(3);
  // Bookmarks.
  f.s->key("1", "Digit1", true, 2, false);
  const Vec3 p = f.s->viewport(0).camera.position;
  f.s->viewport(0).camera.position = {0, 0, 100};
  f.s->key("1", "Digit1", true, 0, false);
  CHECK_NEAR(f.s->viewport(0).camera.position.z, p.z, 1e-4f);
  // Console.
  CHECK(f.cmd(R"({"cmd":"console","text":"gravity 0 0 -3"})")["ok"].as_bool());
  CHECK_NEAR(f.s->level().world.gravity.z, -3, 1e-4f);
}

TEST(studio_outliner_hierarchy) {
  StudioFixture f;
  f.cmd(R"({"cmd":"new","template":"empty"})");
  const game::ActorId a = f.s->spawn("cube", Vec3{0, 0, 0});
  const game::ActorId b = f.s->spawn("sphere", Vec3{3, 0, 0});
  CHECK(f.s->attach({b}, a));
  CHECK(f.s->level().find(b)->parent == a);
  // World placement is kept when attaching.
  CHECK_NEAR(f.s->level().world_matrix(b).translation().x, 3, 1e-4f);
  json::Value rows = f.s->state()["outliner"]["rows"];
  CHECK(rows.size() == 2 && rows[1]["depth"].as_number() == 1);
  json::Value c = json::Value::object();
  c.set("cmd", "collapse");
  c.set("key", "a:" + std::to_string(a));
  f.s->command(c);
  CHECK(f.s->state()["outliner"]["rows"].size() == 1);
  f.cmd(R"({"cmd":"filter","text":"sphere"})");
  rows = f.s->state()["outliner"]["rows"];
  CHECK(rows.size() == 1 && rows[0]["id"].as_number() == b);
  f.cmd(R"({"cmd":"filter","text":""})");
  // Folders.
  f.s->select({a});
  CHECK(f.cmd(R"({"cmd":"setFolder","folder":"Props"})")["ok"].as_bool());
  CHECK(f.s->level().find(a)->folder == "Props");
  CHECK(f.cmd(R"({"cmd":"renameFolder","from":"Props","to":"Stuff"})")["ok"].as_bool());
  CHECK(f.s->level().find(a)->folder == "Stuff");
  // Hide / show, lock.
  f.s->key("h", "KeyH", true, 0, false);
  CHECK(f.s->level().find(a)->hidden);
  f.s->key("h", "KeyH", true, 2, false);
  CHECK(!f.s->level().find(a)->hidden);
  f.s->select({a});
  f.cmd(R"({"cmd":"lock","value":true})");
  CHECK(f.s->level().find(a)->locked && f.s->selection().empty());
  // Group keeps world placement.
  f.cmd(R"({"cmd":"lock","value":false,"ids":[1]})");
  f.s->select({b});
  f.s->group_selection();
  CHECK_NEAR(f.s->level().world_matrix(b).translation().x, 3, 1e-4f);
  // Delete takes children along.
  f.s->select({a});
  f.s->delete_selection();
  CHECK(!f.s->level().find(a));
}

TEST(studio_play_orb_run) {
  StudioFixture f;
  f.frames(2);
  f.s->key("p", "KeyP", true, 4, false);  // Alt+P
  CHECK(f.s->play_state() == studio::PlayState::playing);
  const game::ActorId car = f.s->world().player();
  CHECK(car != 0);
  Mat4 m0;
  f.s->world().actor_matrix(car, m0);
  f.s->key("w", "KeyW", true, 0, false);
  f.frames(120);
  f.s->key("w", "KeyW", false, 0, false);
  Mat4 m1;
  f.s->world().actor_matrix(car, m1);
  CHECK(distance(m0.translation(), m1.translation()) > 3);
  json::Value st = f.s->status();
  CHECK(st["hud"]["status"].as_string() == "playing");
  CHECK(st["hud"]["pickupsTotal"].as_number() == 24);
  // F8 ejects to a free camera, F8 again possesses.
  f.s->key("F8", "F8", true, 0, false);
  CHECK(f.s->play_state() == studio::PlayState::simulating);
  f.s->key("F8", "F8", true, 0, false);
  CHECK(f.s->play_state() == studio::PlayState::playing);
  // Editing is blocked while playing; Esc stops and the level is untouched.
  const std::size_t n = f.s->level().actors.size();
  CHECK(f.s->spawn("cube") == 0);
  f.s->key("Escape", "Escape", true, 0, false);
  CHECK(f.s->play_state() == studio::PlayState::editing);
  CHECK(f.s->level().actors.size() == n);
  Mat4 m2;
  f.frames(1);
  f.s->world().actor_matrix(car, m2);
  CHECK(distance(m0.translation(), m2.translation()) < 0.01f);
}

TEST(studio_simulate_keep_changes) {
  StudioFixture f;
  f.cmd(R"({"cmd":"new","template":"basic"})");
  const game::ActorId box = f.s->spawn("physics_cube", Vec3{0, 0, 5});
  f.frames(1);
  CHECK(f.s->play(game::SimMode::simulate));
  f.frames(120);
  f.s->select({box});
  f.s->key("k", "KeyK", true, 0, false);
  CHECK(f.s->play_state() == studio::PlayState::editing);
  CHECK(f.s->level().find(box)->transform.position.z < 1.5f);
  f.s->undo();
  CHECK(f.s->level().find(box)->transform.position.z > 4);
}

TEST(studio_save_load_and_textures) {
  StudioFixture f;
  f.cmd(R"({"cmd":"new","template":"physicsLab"})");
  const json::Value saved = f.cmd(R"({"cmd":"save"})");
  CHECK(saved["text"].as_string().size() > 1000);
  CHECK(saved["file"].as_string().find(".klevel") != std::string::npos);
  const std::size_t n = f.s->level().actors.size();
  f.cmd(R"({"cmd":"new","template":"empty"})");
  CHECK(f.s->level().actors.empty());
  json::Value open = json::Value::object();
  open.set("cmd", "open");
  open.set("text", saved["text"]);
  open.set("name", "lab.klevel");
  CHECK(f.s->command(open)["ok"].as_bool());
  CHECK(f.s->level().actors.size() == n);
  CHECK(!f.s->can_undo());
  // Texture import assigns to the selected mesh and asks the host for pixels.
  const game::ActorId cube = f.s->spawn("cube", Vec3{0, 0, 0});
  f.s->select({cube});
  CHECK(f.cmd(R"({"cmd":"importTexture","name":"checker","src":"data:image/png;base64,AAAA","width":2,"height":2})")["ok"].as_bool());
  CHECK(f.s->level().find(cube)->mesh->material.texture == "tex1");
  f.frames(1);
  const std::vector<std::string> pending = f.s->pending_textures();
  CHECK(pending.size() == 1 && pending[0] == "tex1");
  CHECK(f.s->texture_source("tex1").rfind("data:image/png", 0) == 0);
  const u8 px[16] = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255};
  CHECK(f.s->texture_pixels("tex1", 2, 2, px));
  CHECK(f.s->pending_textures().empty());
  // Autosave becomes due after an edit.
  CHECK(f.s->dirty());
  CHECK(!f.s->take_autosave(f.now + 60000).empty());
  // Packaging hands the level text to the page.
  CHECK(f.cmd(R"({"cmd":"submitDialog","name":"export","values":{"title":"Lab"}})")["text"].as_string().find("\"Lab\"") != std::string::npos);
}

// ---- deterministic undo / redo ---------------------------------------------

namespace {

std::string studio_doc(const studio::Studio& s) {
  game::Level l = s.level();
  l.camera = {};
  return game::to_text(l, 0);
}

/// Replays a pseudo-random editing session (every kind of document edit, selection
/// changes, transactions, undo / redo and history jumps in between) and records, per
/// history step, the document after the step and the selection before / after it.
struct UndoSession {
  StudioFixture f;
  u32 rng;
  std::vector<std::string> doc_after;                 // per history index
  std::vector<std::vector<game::ActorId>> sel_before, sel_after;
  std::vector<std::string> trace;                     // state after every operation (for replay comparison)
  u32 txn = 0;

  explicit UndoSession(u32 seed) : rng(seed) {
    f.cmd(R"({"cmd":"new","template":"basic"})");
    f.frames(1);
    record_base();
  }
  u32 next() { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
  u32 below(u32 n) { return n ? next() % n : 0; }
  f32 range(f32 a, f32 b) { return a + (b - a) * static_cast<f32>(next() % 10000) / 10000.0f; }
  std::vector<game::ActorId> ids() const {
    std::vector<game::ActorId> out;
    for (const game::Actor& a : f.s->level().actors) out.push_back(a.id);
    return out;
  }
  game::ActorId any() { const auto v = ids(); return v.empty() ? 0 : v[below(static_cast<u32>(v.size()))]; }
  void record_base() {
    doc_after.assign(1, studio_doc(*f.s));
    sel_before.assign(1, {});
    sel_after.assign(1, f.s->selection());
  }
  json::Value run(json::Value c) { return f.s->command(c); }
  static json::Value cmd(const char* name) { json::Value c = json::Value::object(); c.set("cmd", name); return c; }
  static json::Value vec(Vec3 v) { json::Value a = json::Value::array(); a.push(v.x); a.push(v.y); a.push(v.z); return a; }

  /// One random operation. Returns false for history navigation (no recording needed).
  void op() {
    const std::size_t index0 = f.s->history_index(), size0 = f.s->history().size();
    const std::vector<game::ActorId> before = f.s->selection();
    const std::string doc0 = studio_doc(*f.s);
    bool navigation = false;
    switch (below(26)) {
      case 0: case 1: {
        static const char* items[] = {"cube", "sphere", "cylinder", "cone", "capsule", "torus", "ramp", "empty", "pendulum", "chain", "orb", "goal", "hazard", "spinner"};
        f.s->spawn(items[below(14)], Vec3{range(-20, 20), range(-20, 20), range(0, 4)});
        break;
      }
      case 2: { std::vector<game::ActorId> sel; for (u32 k = 0, n = 1 + below(3); k < n; ++k) if (game::ActorId id = any()) sel.push_back(id); f.s->select(sel, below(2) == 0); break; }
      case 3: { json::Value c = cmd("set"); c.set("path", "transform.position"); c.set("value", vec({range(-30, 30), range(-30, 30), range(0, 6)})); run(c); break; }
      case 4: {
        static const char* colors[] = {"#ff0000", "#00ff00", "#2266cc", "#abcdef", "#101010"};
        json::Value c = cmd("set"); c.set("path", "components.mesh.material.color"); c.set("value", colors[below(5)]); run(c); break;
      }
      case 5: {
        static const char* types[] = {"mesh", "light", "body", "collider", "joint", "pickup", "trigger", "rotator", "mover"};
        json::Value c = cmd("addComponent"); c.set("type", types[below(9)]); run(c); break;
      }
      case 6: {
        static const char* types[] = {"mesh", "light", "body", "colliders", "joints", "pickup", "trigger", "rotator", "mover"};
        json::Value c = cmd("removeComponent"); c.set("type", types[below(9)]); c.set("index", 0); run(c); break;
      }
      case 7: f.s->duplicate_selection(Vec3{1, 0, 0}); break;
      case 8: if (below(3) == 0) f.s->delete_selection(); break;
      case 9: { std::vector<game::ActorId> kids = f.s->selection(); f.s->attach(kids, below(3) ? any() : 0); break; }
      case 10: { json::Value c = cmd("setFolder"); c.set("folder", below(2) ? "Props" : ""); run(c); break; }
      case 11: f.s->set_hidden(f.s->selection(), below(2) == 0); break;
      case 12: { json::Value c = cmd("lock"); c.set("value", below(2) == 0); run(c); break; }
      case 13: if (game::ActorId id = any()) f.s->rename(id, "Renamed " + std::to_string(below(50))); break;
      case 14: f.s->group_selection(); break;
      case 15: f.frames(1); f.s->snap_to_floor(); break;
      case 16: { json::Value c = cmd("setWorld"); c.set("path", "world.timeOfDay"); c.set("value", range(0, 1)); run(c); break; }
      case 17: {
        // One continuous scrub: many edits, one transaction token.
        const std::string token = "scrub-" + std::to_string(++txn);
        for (u32 k = 0, n = 2 + below(5); k < n; ++k) {
          json::Value c = cmd("set");
          c.set("path", "transform.scale");
          c.set("value", vec({range(0.5f, 2), range(0.5f, 2), range(0.5f, 2)}));
          c.set("transaction", token);
          run(c);
        }
        break;
      }
      case 18: f.s->copy_selection(); f.s->paste(); break;
      case 19: if (below(3) == 0) f.s->cut_selection(); break;
      case 20: {
        json::Value c = cmd("importTexture");
        c.set("name", "tex"); c.set("src", "data:image/png;base64,AAAA"); c.set("width", 1); c.set("height", 1);
        run(c);
        break;
      }
      case 21: if (!f.s->level().textures.empty()) { json::Value c = cmd("removeTexture"); c.set("id", f.s->level().textures[0].id); run(c); } break;
      case 22: if (f.s->selection().size() >= 2) { json::Value c = cmd("connect"); c.set("kind", "hinge"); run(c); } break;
      case 23: navigation = true; if (below(2)) f.s->undo(); else f.s->redo(); break;
      case 24: if (below(4) == 0) { navigation = true; f.s->jump_history(below(static_cast<u32>(f.s->history().size()))); } break;
      default: {
        // Keep Simulation Changes: simulate a few frames, keep the poses of the selection.
        if (f.s->selection().empty()) break;
        if (!f.s->play(game::SimMode::simulate)) break;
        f.frames(4);
        f.s->keep_simulation_changes();
        break;
      }
    }
    f.frames(1);
    const std::size_t index1 = f.s->history_index();
    if (!navigation) {
      if (index1 == index0 + 1 && f.s->history().size() == index1 + 1) {
        doc_after.resize(index1);  // a new step discards the old redo branch
        sel_before.resize(index1);
        sel_after.resize(index1);
        doc_after.push_back(studio_doc(*f.s));
        sel_before.push_back(before);
        sel_after.push_back(f.s->selection());
      } else if (index1 == index0 && f.s->history().size() == size0 && studio_doc(*f.s) != doc0) {
        doc_after[index1] = studio_doc(*f.s);  // a merged transaction step
        sel_after[index1] = f.s->selection();
      } else if (index1 + 1 == index0 && f.s->history().size() == size0 - 1) {
        // A transaction that returned to its start removed its own step.
        doc_after.resize(index1 + 1);
        sel_before.resize(index1 + 1);
        sel_after.resize(index1 + 1);
      }
    }
    std::string t = std::to_string(index1) + "/" + std::to_string(f.s->history().size()) + ":";
    for (game::ActorId id : f.s->selection()) t += std::to_string(id) + ",";
    trace.push_back(t + studio_doc(*f.s));
  }
};

}  // namespace

TEST(studio_undo_redo_is_exact_and_deterministic) {
  for (u32 seed : {0x1234567u, 0xC0FFEEu, 20261022u}) {
    UndoSession a(seed), b(seed);
    for (int i = 0; i < 260; ++i) { a.op(); b.op(); }
    // Same inputs, same results: two editors replaying one session agree at every step.
    CHECK(a.trace == b.trace);
    studio::Studio& s = *a.f.s;
    const std::size_t steps = s.history().size();
    std::printf("    seed %u: %zu history steps, %zu actors\n", seed, steps, s.level().actors.size());
    CHECK(steps > 15);
    CHECK(a.doc_after.size() == steps);
    // Undo to the start: each state equals the one recorded when its step was made, the
    // selection is the one before the undone step.
    s.jump_history(steps - 1);
    CHECK(studio_doc(s) == a.doc_after[steps - 1]);
    for (std::size_t i = steps - 1; i > 0; --i) {
      s.undo();
      CHECK(s.history_index() == i - 1);
      CHECK(studio_doc(s) == a.doc_after[i - 1]);
      std::vector<game::ActorId> expected;
      for (game::ActorId id : a.sel_before[i]) if (s.level().find(id)) expected.push_back(id);
      CHECK(s.selection() == expected);
    }
    CHECK(!s.can_undo());
    // Redo to the end: each state again, with the selection right after the step.
    for (std::size_t i = 1; i < steps; ++i) {
      s.redo();
      CHECK(studio_doc(s) == a.doc_after[i]);
      CHECK(s.selection() == a.sel_after[i]);
    }
    CHECK(!s.can_redo());
    // Random walks and jumps reproduce the same states.
    for (int k = 0; k < 120; ++k) {
      const std::size_t target = a.below(static_cast<u32>(steps));
      if (a.below(2)) s.jump_history(target);
      else {
        while (s.history_index() > target) s.undo();
        while (s.history_index() < target) s.redo();
      }
      CHECK(studio_doc(s) == a.doc_after[target]);
    }
    // Every state survives save and load unchanged.
    s.jump_history(steps / 2);
    StudioFixture g;
    CHECK(g.s->load_level_text(s.level_text(), "copy"));
    CHECK(studio_doc(*g.s) == studio_doc(s));
  }
}

TEST(studio_undo_transactions_and_dirty_state) {
  StudioFixture f;
  f.cmd(R"({"cmd":"new","template":"basic"})");
  f.frames(1);
  CHECK(!f.s->dirty());
  const game::ActorId id = f.s->spawn("cube", Vec3{0, 0, 1});
  CHECK(f.s->dirty());
  const std::size_t base = f.s->history().size();
  // A scrub (one token) is one step, whatever the number of edits.
  for (int i = 1; i <= 10; ++i) f.cmd((R"({"cmd":"set","path":"transform.position","value":[)" + std::to_string(i) + R"(,0,1],"transaction":"t1"})").c_str());
  CHECK(f.s->history().size() == base + 1);
  CHECK_NEAR(f.s->level().find(id)->transform.position.x, 10, 1e-5f);
  // Typed values (no token) are separate steps.
  f.cmd(R"({"cmd":"set","path":"transform.position","value":[3,0,1]})");
  f.cmd(R"({"cmd":"set","path":"transform.position","value":[4,0,1]})");
  CHECK(f.s->history().size() == base + 3);
  // Setting the current value again changes nothing and adds no step.
  f.cmd(R"({"cmd":"set","path":"transform.position","value":[4,0,1]})");
  CHECK(f.s->history().size() == base + 3);
  // A scrub that ends where it started leaves no step behind.
  f.cmd(R"({"cmd":"set","path":"transform.position","value":[9,0,1],"transaction":"t2"})");
  f.cmd(R"({"cmd":"set","path":"transform.position","value":[4,0,1],"transaction":"t2"})");
  CHECK(f.s->history().size() == base + 3);
  // The same token after another edit starts a new step.
  f.cmd(R"({"cmd":"set","path":"transform.position","value":[5,0,1],"transaction":"t3"})");
  f.cmd(R"({"cmd":"set","path":"components.mesh.material.color","value":"#ff0000","transaction":"t3"})");
  f.cmd(R"({"cmd":"set","path":"transform.position","value":[6,0,1],"transaction":"t3"})");
  CHECK(f.s->history().size() == base + 6);
  // Dirty follows the saved state through undo and redo.
  f.cmd(R"({"cmd":"saved"})");
  CHECK(!f.s->dirty());
  f.s->undo();
  CHECK(f.s->dirty());
  f.s->redo();
  CHECK(!f.s->dirty());
  f.s->undo();
  f.cmd(R"({"cmd":"set","path":"transform.position","value":[7,7,1]})");  // new branch: the saved state is gone
  CHECK(f.s->dirty());
  // Alt-drag duplicate + move is one step; undo removes the copy and restores the selection.
  f.s->select({id});
  const std::size_t actors = f.s->level().actors.size(), steps = f.s->history().size();
  studio::Viewport& v = f.s->viewport(0);
  v.camera.position = {-10, -10, 8};
  v.camera.yaw = 45;
  v.camera.pitch = -30;
  f.s->set_tool(studio::Tool::translate);
  f.frames(2);
  Mat4 m;
  f.s->world().actor_matrix(id, m);
  f32 cx, cy, ax, ay;
  CHECK(f.s->to_screen(v, m.translation(), cx, cy));
  CHECK(f.s->to_screen(v, m.translation() + Vec3{1, 0, 0}, ax, ay));
  const f32 len = std::hypot(ax - cx, ay - cy);
  const f32 gx = cx + (ax - cx) / len * 60, gy = cy + (ay - cy) / len * 60;
  CHECK(f.s->gizmo_hit(v, gx, gy) == 1);
  f.s->pointer(0, 0, gx, gy, 0, 1, 4, 0);
  CHECK(v.drag == studio::Drag::gizmo);
  f.s->pointer(0, 1, gx + 40, gy, -1, 1, 4, 0);
  f.s->pointer(0, 2, gx + 40, gy, 0, 0, 4, 0);
  CHECK(f.s->level().actors.size() == actors + 1);
  CHECK(f.s->history().size() == steps + 1);
  CHECK(f.s->history().back().label == "Duplicate and move");
  CHECK(f.s->selection().size() == 1 && f.s->selection()[0] != id);
  f.s->undo();
  CHECK(f.s->level().actors.size() == actors);
  CHECK(f.s->selection().size() == 1 && f.s->selection()[0] == id);
  // An Alt-click without movement leaves nothing behind.
  f.frames(1);
  f.s->pointer(0, 0, gx, gy, 0, 1, 4, 0);
  f.s->pointer(0, 2, gx, gy, 0, 0, 4, 0);
  CHECK(f.s->level().actors.size() == actors);
  CHECK(f.s->history().size() == steps + 1);
  // Undo while playing is refused (the level is not edited during play).
  CHECK(f.s->play(game::SimMode::simulate));
  const std::size_t idx = f.s->history_index();
  f.s->undo();
  CHECK(f.s->history_index() == idx);
  f.s->stop();
}

TEST(studio_player_only) {
  StudioFixture f(true);
  CHECK(f.s->level().actors.empty());
  json::Value open = json::Value::object();
  open.set("cmd", "open");
  open.set("text", game::to_text(game::orb_run_level(), -1));
  CHECK(f.s->command(open)["ok"].as_bool());
  CHECK(f.cmd(R"({"cmd":"play","mode":"play"})")["ok"].as_bool());
  f.frames(10);
  const json::Value st = f.s->status();
  CHECK(st["hud"]["status"].as_string() == "playing");
  CHECK(!st.has("camera"));
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

int main(int argc, char** argv) {
  // kern_tests [name-substring] runs a subset.
  const char* filter = argc > 1 ? argv[1] : nullptr;
  for (const Test& t : registry()) {
    if (filter && !std::strstr(t.name, filter)) continue;
    const int before = g_failures;
    t.fn();
    std::printf("%s %s\n", g_failures == before ? "ok  " : "FAIL", t.name);
  }
  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
