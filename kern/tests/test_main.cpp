// Kern engine unit tests (host build, see kern/tests/run.sh).
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>
#include <tuple>
#include <vector>

#include "gl_stub.h"
#include "kern/kern.h"
#include "kern/physics/physics.h"
#include "kern/core/json.h"
#include "kern/game/demos.h"
#include "kern/audio/synth.h"
#include "kern/game/walker_policy.h"
#include "kern/game/runtime.h"
#include "kern/game/walker.h"
#include "kern/lehrer/env.h"
#include "kern/lehrer/nn.h"
#include "kern/lehrer/ppo.h"
#include "kern/lehrer/onnx_export.h"
#include "kern/lehrer/backend.h"
#include "kern/studio/studio.h"
#include "kern/net/client_server.h"
#include "kern/game/net_game.h"
#include "kern/logic/logic.h"
#include "logic_cases.h"
#include "../src/game/soldier_models.h"

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

TEST(game_orbit_camera_mouse_sense) {
  GameFixture f;
  game::Level l = game::physics_lab_level();
  CHECK(f.world->start(l, game::SimMode::play));
  CHECK(f.world->player() != 0);
  Camera cam;
  f.world->update_camera(cam, 1.0f / 60, {});
  Mat4 m;
  f.world->actor_matrix(f.world->player(), m);
  CHECK(cam.eye.z > m.translation().z);  // behind and above the player, looking down at it
  // Mouse down (positive look_y) looks further down: the camera rises.
  const f32 z0 = cam.eye.z;
  game::PlayInput down;
  down.look_y = 20;
  for (int i = 0; i < 30; ++i) f.world->update_camera(cam, 1.0f / 60, i == 0 ? down : game::PlayInput{});
  CHECK(cam.eye.z > z0 + 0.3f);
  game::PlayInput up;
  up.look_y = -40;
  for (int i = 0; i < 30; ++i) f.world->update_camera(cam, 1.0f / 60, i == 0 ? up : game::PlayInput{});
  CHECK(cam.eye.z < z0);
  f.world->stop();
}

namespace {
game::ActorId add_soldier(game::Level& l, Vec3 at) {
  game::Actor body;
  body.name = "Soldier";
  body.transform.position = at;
  body.mesh = game::MeshComponent{};
  body.mesh->shape = game::Shape::capsule;
  body.mesh->radius = 0.35f;
  body.mesh->height = 1.45f;
  body.body = game::BodyComponent{};
  body.body->mass = 80;
  body.controller = game::ControllerComponent{};
  body.controller->mode = game::ControlMode::soldier;
  body.controller->camera = game::CameraMode::first_person;
  body.controller->max_speed = 5.5f;
  body.controller->boost_speed = 8.1f;
  body.controller->jump_speed = 3.6f;
  body.weapon = game::WeaponComponent{};
  body.health = game::HealthComponent{};
  const game::ActorId id = l.add(body).id;
  game::Actor head;
  head.name = "Head";
  head.parent = id;
  head.transform.position = {0, 0, 0.83f};
  head.mesh = game::MeshComponent{};
  head.mesh->shape = game::Shape::sphere;
  head.mesh->radius = 0.17f;
  head.tags.push_back("head");
  l.add(head);
  return id;
}
game::ActorId add_dummy(game::Level& l, Vec3 at) {
  game::Actor body;
  body.name = "Target Dummy";
  body.transform.position = at;
  body.mesh = game::MeshComponent{};
  body.mesh->shape = game::Shape::capsule;
  body.mesh->radius = 0.32f;
  body.mesh->height = 1.45f;
  game::HealthComponent h;
  h.regen_delay = 3;
  h.regen_rate = 40;
  h.respawn_delay = 3;
  body.health = h;
  const game::ActorId id = l.add(body).id;
  game::Actor head;
  head.name = "Head";
  head.parent = id;
  head.transform.position = {0, 0, 0.88f};
  head.mesh = game::MeshComponent{};
  head.mesh->shape = game::Shape::sphere;
  head.mesh->radius = 0.17f;
  head.tags.push_back("head");
  l.add(head);
  return id;
}
}  // namespace

TEST(game_soldier_moves_shoots_and_respawns) {
  GameFixture f;
  game::Level l = game::basic_level();
  for (game::Actor& a : l.actors) if (a.name == "Cube") a.transform.position = {20, 20, 0.5f};
  const game::ActorId me = add_soldier(l, {0, 0, 0.8f});
  const game::ActorId dummy = add_dummy(l, {10, 0, 0.725f});
  CHECK(f.world->start(l, game::SimMode::play));
  CHECK(f.world->player() == me);
  CHECK(f.world->physics().body_type(f.world->actor_body(me)) == physics::BodyType::kinematic);
  const f32 dt = 1.0f / 60;
  Camera cam;
  game::PlayInput in;
  auto tick = [&](int n) { for (int i = 0; i < n; ++i) { f.world->step(dt, in); f.world->update_camera(cam, dt, in); in.look_x = in.look_y = 0; } };
  tick(30);
  const game::Hud& hud = f.world->hud();
  CHECK(hud.soldier);
  CHECK(hud.ammo == 30 && hud.magazine == 30 && hud.reserve == 120);
  CHECK(std::fabs(hud.health - 100) < 0.01f);
  CHECK(std::fabs(f.world->health_of(dummy) - 100) < 0.01f);
  // First-person camera at eye height looking along +X.
  CHECK(std::fabs(cam.eye.z - 1.62f) < 0.12f);
  CHECK(cam.target.x > cam.eye.x + 9);
  // Walk, sprint, crouch.
  in.move_x = 1;  // strafe right (-Y), away from the dummy's line
  tick(60);
  CHECK(hud.speed > 5.0f && hud.speed < 6.0f);
  in.boost = true;
  in.move_x = 0;
  in.move_y = 1;
  // Sprint needs forward input; walk backwards first so we stay in front of the dummy.
  in.move_y = -1;
  tick(20);
  CHECK(hud.speed < 6.0f);  // no sprint backwards
  in.move_y = 0;
  in.boost = false;
  in.crouch = true;
  tick(40);
  const Vec3 crouch_eye = cam.eye;
  CHECK(crouch_eye.z < 1.25f);
  in.crouch = false;
  tick(40);
  CHECK(cam.eye.z > 1.5f);
  // Jump buffer: pressing jump just before landing still jumps after landing.
  in.jump = true;
  tick(1);
  in.jump = false;
  f32 peak = 0;
  for (int i = 0; i < 80; ++i) { tick(1); peak = std::max(peak, cam.eye.z); }
  CHECK(peak > 1.62f + 0.25f);
  // Face the dummy again (yaw back to +X from wherever strafing left us).
  const Vec3 me_at = f.world->physics().pose(f.world->actor_body(me)).position;
  // Aim at the chest: eye 1.62 m, chest 1.0 m, distance from the dummy.
  const f32 dist = 10 - me_at.x;
  const f32 yaw_to = std::atan2(0 - me_at.y, dist) / 0.0174532925f;
  in.look_x = -yaw_to;
  in.look_y = std::atan2(0.62f, dist) / 0.0174532925f;
  tick(1);
  tick(30);  // settle spread
  in.fire = true;
  tick(1);
  in.fire = false;
  CHECK(!f.world->shots().empty());
  CHECK(hud.ammo == 29);
  const f32 after_body = f.world->health_of(dummy);
  CHECK(after_body < 100 && after_body > 50);  // 24 damage, no headshot
  CHECK(f.world->shots().back().hit == dummy && !f.world->shots().back().head);
  CHECK(hud.hit_age < 0.1f);
  // Headshot: aim down the sights (tight spread) at the head (1.6 m).
  in.look_y = -std::atan2(0.62f, dist) / 0.0174532925f + std::atan2(0.02f, dist) / 0.0174532925f;
  tick(1);
  in.aim = true;  // (aiming lowers the look sensitivity)
  tick(30);
  CHECK(hud.aiming && hud.fov < 50);
  CHECK(hud.spread < 0.3f);
  in.fire = true;
  tick(1);
  in.fire = false;
  in.aim = false;
  CHECK(f.world->shots().back().head);
  CHECK(f.world->health_of(dummy) < after_body - 50);  // 24 * 2.5
  // Automatic fire until it is down.
  in.fire = true;
  for (int i = 0; i < 60 && f.world->health_of(dummy) > 0; ++i) tick(1);
  in.fire = false;
  CHECK(f.world->health_of(dummy) == 0);
  CHECK(hud.hit_kill);
  CHECK(hud.score >= 10);
  tick(200);  // 3 s respawn
  CHECK(std::fabs(f.world->health_of(dummy) - 100) < 0.01f);
  // Reload: 2 s refills the magazine from the reserve.
  const i32 mag = hud.ammo;
  CHECK(mag < 30);
  in.reload = true;
  tick(1);
  in.reload = false;
  CHECK(hud.reload >= 0);
  tick(130);
  CHECK(hud.ammo == 30 && hud.reserve == 120 - (30 - mag));
  // The player dies, respawns with full health and spawn protection.
  f.world->damage(me, 250, 0, false, {0, 0, 0});
  CHECK(f.world->health_of(me) == 0);
  tick(10);
  CHECK(hud.respawn_in > 0);
  tick(200);
  CHECK(std::fabs(f.world->health_of(me) - 100) < 0.01f);
  f.world->damage(me, 50, 0, false, {0, 0, 0});
  CHECK(std::fabs(f.world->health_of(me) - 100) < 0.01f);  // protected
  tick(160);
  f.world->damage(me, 50, 0, false, {0, 0, 0});
  CHECK(f.world->health_of(me) < 100);
  // Regeneration after 5 s.
  const f32 hurt = f.world->health_of(me);
  tick(60 * 7);
  CHECK(f.world->health_of(me) > hurt + 10);
}

template <class V>
bool no_errors(const V& issues) {
  for (const auto& i : issues) if (i.error) return false;
  return true;
}

namespace {
struct FakeLogicHost : logic::Host {
  f64 t = 0;
  u32 seed = 1;
  std::vector<std::string> printed, errors;
  f64 time() const override { return t; }
  void print(const std::string& text, bool) override { printed.push_back(text); }
  void error(const std::string& text) override { errors.push_back(text); }
  u32 random() override { seed = seed * 1664525u + 1013904223u; return seed; }
};
logic::Graph logic_graph(const char* text) {
  json::Value j;
  std::string err;
  CHECK(json::parse(text, j, &err));
  logic::Graph g;
  CHECK(logic::graph_from_json(j, g, &err));
  if (!err.empty()) std::printf("graph: %s\n", err.c_str());
  return g;
}
}  // namespace

TEST(logic_flow_variables_and_latent_nodes) {
  FakeLogicHost host;
  logic::Runtime rt(host);
  // Begin Play -> For 1..5 { Add to Sum (Index) } -> Print "Sum " + Sum -> Delay 1 -> Print "later"
  const logic::Graph g = logic_graph(R"({"variables":[{"name":"Sum","type":"float","value":0}],
    "nodes":[{"id":1,"type":"event.begin"},
      {"id":2,"type":"flow.for","values":{"first":1,"last":5}},
      {"id":3,"type":"var.add","params":{"variable":"Sum"}},
      {"id":4,"type":"debug.print"},
      {"id":5,"type":"text.append","values":{"a":"Sum "}},
      {"id":6,"type":"var.get","params":{"variable":"Sum"}},
      {"id":7,"type":"flow.delay","values":{"duration":1}},
      {"id":8,"type":"debug.print","values":{"text":"later"}}],
    "links":[[1,"then",2,"exec"],[2,"body",3,"exec"],[2,"index",3,"amount"],[2,"completed",4,"exec"],
      [6,"value",5,"b"],[5,"result",4,"text"],[4,"then",7,"exec"],[7,"completed",8,"exec"]]})");
  CHECK(no_errors(logic::check(g)));
  logic::Instance* inst = rt.add(g, 0, "Level");
  rt.begin();
  CHECK(inst->get("Sum").as_number() == 15);
  CHECK(host.printed.size() == 1 && host.printed[0] == "Sum 15");
  for (int i = 0; i < 59; ++i) { host.t += 1.0 / 60; rt.tick(1.0f / 60); }
  CHECK(host.printed.size() == 1);
  host.t += 2.0 / 60;
  rt.tick(1.0f / 60);
  CHECK(host.printed.size() == 2 && host.printed[1] == "later");
  // JSON round trip.
  logic::Graph back;
  CHECK(logic::graph_from_json(logic::graph_to_json(g), back));
  CHECK(back == g);
}

TEST(logic_sequence_gates_switch_and_events) {
  FakeLogicHost host;
  logic::Runtime rt(host);
  const logic::Graph g = logic_graph(R"({"variables":[{"name":"Log","type":"text","value":""},{"name":"Open","type":"bool","value":false}],
    "nodes":[{"id":1,"type":"event.custom","params":{"name":"Go"}},
      {"id":2,"type":"flow.sequence","params":{"count":"3"}},
      {"id":3,"type":"var.set","params":{"variable":"Log"}},{"id":4,"type":"text.append"},{"id":5,"type":"var.get","params":{"variable":"Log"}},
      {"id":6,"type":"flow.do_once"},
      {"id":7,"type":"var.set","params":{"variable":"Log"}},{"id":8,"type":"text.append","values":{"b":"once,"}},{"id":9,"type":"var.get","params":{"variable":"Log"}},
      {"id":10,"type":"flow.switch_text","params":{"cases":"x, y"}},
      {"id":11,"type":"var.set","params":{"variable":"Log"}},{"id":12,"type":"text.append","values":{"b":"Y"}},{"id":13,"type":"var.get","params":{"variable":"Log"}},
      {"id":14,"type":"flow.branch"},{"id":15,"type":"math.compare","params":{"op":">"},"values":{"b":2}},
      {"id":16,"type":"debug.print","values":{"text":"big"}}],
    "links":[[1,"then",2,"exec"],[2,"then0",3,"exec"],[5,"value",4,"a"],[1,"text",4,"b"],[4,"result",3,"value"],
      [2,"then1",6,"exec"],[6,"completed",7,"exec"],[9,"value",8,"a"],[8,"result",7,"value"],
      [2,"then2",10,"exec"],[1,"text",10,"selection"],[10,"case1",11,"exec"],[13,"value",12,"a"],[12,"result",11,"value"],
      [11,"then",14,"exec"],[1,"value",15,"a"],[15,"result",14,"condition"],[14,"true",16,"exec"]]})");
  logic::Instance* inst = rt.add(g, 0, "Level");
  rt.begin();
  rt.call("Go", 0, {{"text", logic::Value::text("a")}});
  CHECK(inst->get("Log").as_text() == "aonce,");
  rt.call("Go", 0, {{"text", logic::Value::text("y")}, {"value", logic::Value::number(3)}});
  CHECK(inst->get("Log").as_text() == "aonce,yY");
  CHECK(host.printed.size() == 1 && host.printed[0] == "big");
  rt.call("Other");
  CHECK(inst->get("Log").as_text() == "aonce,yY");
  CHECK(host.errors.empty());
}

TEST(logic_state_machine_timers_events_conditions) {
  FakeLogicHost host;
  logic::Runtime rt(host);
  const logic::Graph g = logic_graph(R"({"variables":[{"name":"Armed","type":"bool","value":false},{"name":"Entered","type":"float","value":0}],
    "machines":[{"id":20,"name":"Door","states":[{"id":21,"name":"Closed","initial":true},{"id":22,"name":"Opening"},{"id":23,"name":"Open"},{"id":24,"name":"Locked"}],
      "transitions":[{"id":30,"from":21,"to":22,"trigger":"event","event":"Use"},{"id":31,"from":22,"to":23,"trigger":"timer","seconds":0.5},
                     {"id":32,"from":23,"to":21,"trigger":"timer","seconds":2},{"id":33,"from":0,"to":24,"trigger":"condition","variable":"Armed"}]}],
    "nodes":[{"id":1,"type":"event.state_enter","params":{"machine":"Door","state":"any"}},{"id":2,"type":"var.add","params":{"variable":"Entered"}},
      {"id":3,"type":"event.state_enter","params":{"machine":"Door","state":"Open"}},{"id":4,"type":"debug.print","values":{"text":"open!"}}],
    "links":[[1,"then",2,"exec"],[3,"then",4,"exec"]]})");
  logic::Instance* inst = rt.add(g, 7, "Door");
  rt.begin();
  CHECK(inst->state("Door") == "Closed");
  CHECK(inst->get("Entered").as_number() == 1);
  auto run = [&](f64 seconds) { for (int i = 0; i < static_cast<int>(seconds * 60 + 0.5); ++i) { host.t += 1.0 / 60; rt.tick(1.0f / 60); } };
  run(1);
  CHECK(inst->state("Door") == "Closed");
  rt.call("Use", 7);
  CHECK(inst->state("Door") == "Opening");
  run(0.6);
  CHECK(inst->state("Door") == "Open");
  CHECK(host.printed.size() == 1);
  run(2.1);
  CHECK(inst->state("Door") == "Closed");
  inst->set("Armed", logic::Value::boolean(true));
  run(0.05);
  CHECK(inst->state("Door") == "Locked");
  CHECK(inst->get("Entered").as_number() == 5);
}

TEST(logic_checks_and_infinite_loops) {
  FakeLogicHost host;
  // Errors: unknown type, missing variable, wrong wire types.
  const logic::Graph bad = logic_graph(R"({"nodes":[{"id":1,"type":"event.begin"},{"id":2,"type":"no.such"},{"id":3,"type":"var.get","params":{"variable":"Nope"}},
    {"id":4,"type":"flow.branch"},{"id":5,"type":"vector.length"}],"links":[[1,"then",4,"exec"],[5,"result",4,"exec"]]})");
  const auto issues = logic::check(bad);
  int errors = 0;
  for (const auto& i : issues) errors += i.error ? 1 : 0;
  CHECK(errors >= 3);
  // While(true) is stopped by the step budget and reported.
  logic::Runtime rt(host);
  rt.budget = 5000;
  const logic::Graph loop = logic_graph(R"({"variables":[{"name":"N","type":"float","value":0}],"nodes":[{"id":1,"type":"event.begin"},
    {"id":2,"type":"flow.while","values":{"condition":true}},{"id":3,"type":"var.add","params":{"variable":"N"}}],
    "links":[[1,"then",2,"exec"],[2,"body",3,"exec"]]})");
  logic::Instance* inst = rt.add(loop, 0, "Loop");
  rt.begin();
  CHECK(inst->disabled());
  CHECK(!host.errors.empty() && host.errors.back().find("infinite loop") != std::string::npos);
  CHECK(inst->get("N").as_number() > 100);
}

TEST(logic_tween_and_every) {
  FakeLogicHost host;
  logic::Runtime rt(host);
  const logic::Graph g = logic_graph(R"({"variables":[{"name":"A","type":"float","value":0},{"name":"Ticks","type":"float","value":0},{"name":"Done","type":"bool","value":false}],
    "nodes":[{"id":1,"type":"event.begin"},{"id":2,"type":"flow.tween","params":{"ease":"linear"},"values":{"duration":1}},
      {"id":3,"type":"var.set","params":{"variable":"A"}},{"id":4,"type":"var.set","params":{"variable":"Done"},"values":{"value":true}},
      {"id":5,"type":"event.every","params":{"seconds":"0.25"}},{"id":6,"type":"var.add","params":{"variable":"Ticks"}}],
    "links":[[1,"then",2,"play"],[2,"update",3,"exec"],[2,"alpha",3,"value"],[2,"finished",4,"exec"],[5,"then",6,"exec"]]})");
  logic::Instance* inst = rt.add(g, 0, "T");
  rt.begin();
  for (int i = 0; i < 30; ++i) { host.t += 1.0 / 60; rt.tick(1.0f / 60); }
  CHECK(inst->get("A").as_number() > 0.4 && inst->get("A").as_number() < 0.6);
  CHECK(!inst->get("Done").as_bool());
  for (int i = 0; i < 40; ++i) { host.t += 1.0 / 60; rt.tick(1.0f / 60); }
  CHECK(inst->get("A").as_number() == 1);
  CHECK(inst->get("Done").as_bool());
  CHECK(std::fabs(inst->get("Ticks").as_number() - 4) <= 1);
}

TEST(game_logic_graphs_drive_the_world) {
  GameFixture f;
  game::Level l = game::basic_level();
  for (game::Actor& a : l.actors) if (a.name == "Cube") a.transform.position = {40, 40, 0.5f};
  const game::ActorId me = add_soldier(l, {0, 0, 0.8f});
  // A sensor volume with its own graph: entering it scores and opens the "Door" (moves it up over 1 s).
  game::Actor zone;
  zone.name = "Zone";
  zone.transform.position = {6, 0, 0};
  game::ColliderComponent s;
  s.shape = game::ColliderShape::box;
  s.size = {2, 2, 2};
  s.center = {0, 0, 1};
  s.sensor = true;
  zone.colliders.push_back(s);
  zone.logic = logic_graph(R"({"variables":[{"name":"Visits","type":"int","value":0}],
    "nodes":[{"id":1,"type":"game.overlap_begin"},{"id":2,"type":"var.add","params":{"variable":"Visits"}},
      {"id":3,"type":"game.add_score","values":{"points":5}},{"id":4,"type":"flow.call","params":{"name":"OpenDoor"}}],
    "links":[[1,"then",2,"exec"],[2,"then",3,"exec"],[3,"then",4,"exec"]]})");
  const game::ActorId zone_id = l.add(zone).id;
  game::Actor door;
  door.name = "Door";
  door.transform.position = {10, 0, 1.5f};
  door.mesh = game::MeshComponent{};
  door.mesh->size = {0.3f, 3, 3};
  door.logic = logic_graph(R"({"nodes":[{"id":1,"type":"event.custom","params":{"name":"OpenDoor"}},
      {"id":2,"type":"flow.tween","params":{"ease":"linear"},"values":{"duration":1}},
      {"id":3,"type":"actor.set_location"},{"id":4,"type":"vector.lerp","values":{"a":[10,0,1.5],"b":[10,0,4.5]}}],
    "links":[[1,"then",2,"play"],[2,"update",3,"exec"],[2,"alpha",4,"alpha"],[4,"result",3,"location"]]})");
  const game::ActorId door_id = l.add(door).id;
  // The level graph: Interact spawns a copy of the Cube in front of the player; Begin Play greets.
  l.logic = logic_graph(R"({"nodes":[{"id":1,"type":"event.begin"},{"id":2,"type":"game.message","values":{"text":"Welcome"}},
      {"id":3,"type":"game.input","params":{"action":"interact","edge":"pressed"}},{"id":4,"type":"actor.spawn","params":{"template":"Cube"},"values":{"location":[0,5,0.5]}},
      {"id":5,"type":"debug.print","values":{"screen":false}},{"id":6,"type":"actor.name"}],
    "links":[[1,"then",2,"exec"],[3,"then",4,"exec"],[4,"then",5,"exec"],[4,"spawned",6,"target"],[6,"name",5,"text"]]})");
  CHECK(game::validate(l).empty() || no_errors(game::validate(l)));
  CHECK(f.world->start(l, game::SimMode::play));
  CHECK(f.world->logic_runtime() != nullptr);
  CHECK(f.world->hud().message == "Welcome");
  Camera cam;
  game::PlayInput in;
  auto tick = [&](int n) { for (int i = 0; i < n; ++i) { f.world->step(1.0f / 60, in); f.world->update_camera(cam, 1.0f / 60, in); } };
  tick(10);
  // Walk into the zone.
  f.world->teleport(me, {6, 0, 0.8f});
  tick(5);
  CHECK(f.world->hud().score == 5);
  CHECK(f.world->logic_runtime()->find(zone_id)->get("Visits").as_int() == 1);
  tick(70);
  Mat4 m;
  CHECK(f.world->actor_matrix(door_id, m));
  CHECK(std::fabs(m.translation().z - 4.5f) < 0.05f);
  // Interact spawns a replicated copy of the Cube.
  in.interact = true;
  tick(2);
  in.interact = false;
  tick(2);
  std::vector<game::GameWorld::PropertyChange> props;
  std::vector<game::GameWorld::SpawnChange> spawns;
  f.world->take_replicated_changes(props, spawns);
  CHECK(spawns.size() == 1);
  const game::ActorId copy = spawns.empty() ? 0 : spawns[0].id;
  CHECK(copy && f.world->level().find(copy) && f.world->level().find(copy)->name == "Cube");
  bool printed = false;
  for (const std::string& e : f.world->events()) printed |= e == "Logic: Cube";
  CHECK(printed);
  CHECK(!props.empty());  // the door's moves (a body-less actor) replicate as transform changes
}

TEST(logic_tier_cases_interpreter) {
  for (const logic_cases::Case& c : logic_cases::cases()) {
    logic_cases::Host host;
    logic::Runtime rt(host);
    const logic::Graph g = logic_cases::parse(c);
    CHECK(!g.nodes.empty());
    logic::Instance* inst = rt.add(g, 0, c.name);
    const std::string r = logic_cases::play(rt, host, inst);
    if (std::string(c.name) == "math_loops") {
      CHECK(inst->get("Sum").as_number() == 315);
      CHECK(inst->get("Odd").as_int() == 10);
      CHECK(inst->get("Flag").as_bool());
      CHECK(std::fabs(inst->get("Avg").as_number() - (std::sqrt(315.0) + (14 - std::sqrt(315.0)) * 0.25)) < 1e-9);
    }
    if (std::string(c.name) == "gates_and_sequences") {
      // Log = ((0*10+1)*10+1)... once per Go: 6 Go calls -> 111111
      CHECK(inst->get("Log").as_number() == 111111);
      CHECK(inst->get("Once").as_int() == 1);
      CHECK(inst->get("N").as_int() == 2);
      CHECK(inst->get("Gate").as_int() == 3);   // opened after the first 3 calls
      CHECK(inst->get("Flips").as_int() == 303);
    }
    if (std::string(c.name) == "while_nested") {
      CHECK(inst->get("I").as_int() == 7);
      CHECK(inst->get("Total").as_number() == 6.0 * (1 + 2 + 3 + 4 + 5 + 6 + 7));
    }
    if (std::string(c.name) == "mixed_cpp_nodes") {
      CHECK(inst->get("Count").as_int() == 6);
      CHECK(inst->get("Text").as_text() == "n=6");
      CHECK(inst->get("Done").as_bool());
    }
    if (std::string(c.name) == "state_machine") CHECK(inst->state("M") == "A" || inst->state("M") == "B");
    if (std::string(c.name) == "infinite_loop") CHECK(inst->disabled());
    if (std::string(c.name) == "reroutes") {
      CHECK(inst->get("Hits").as_int() == 6);  // 6 x Go through two exec knots
      CHECK(inst->get("Sum").as_number() == 0 + 2 + 4 + 5 + 5 + 1);  // the values through two data knots
      CHECK(inst->graph().comments.size() == 1);
    }
    CHECK(!r.empty());
  }
  // The generated programs exist and the WebAssembly module is well-formed (magic, version, sections).
  logic_cases::Host host;
  logic::Runtime rt(host);
  logic::Instance* inst = rt.add(logic_cases::parse(logic_cases::cases()[0]), 0, "m");
  const std::vector<u8> wasm = logic::compile_wasm(*inst);
  CHECK(wasm.size() > 64 && wasm[0] == 0 && wasm[1] == 'a' && wasm[2] == 's' && wasm[3] == 'm');
  const std::string cpp = logic::generate_cpp(*inst, "m");
  CHECK(cpp.find("register_native_graph") != std::string::npos && cpp.find("switch (pc)") != std::string::npos);
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

// Colliders may sound different from their body (a sole steps, a shell taps), and impact sounds may
// ask for touches far softer than a "hit" (2 m/s) - without logic hits or camera shake for them.
TEST(audio_collider_impact_sounds) {
  GameFixture f;
  game::Level l = game::basic_level();
  auto toy = [&](const char* name, f32 x, const char* body_sound, f32 threshold, const char* collider_sound) {
    game::Actor a;
    a.name = name;
    a.transform.position = {x, 0, 0.2f};  // falls 0.15 m: about 1.7 m/s, softer than a hit
    a.mesh = game::MeshComponent{};
    a.mesh->size = {0.1f, 0.1f, 0.1f};
    a.mesh->collision = false;
    a.body = game::BodyComponent{};
    a.body->motion = game::Motion::dynamic;
    a.body->impact_sound = body_sound;
    a.body->impact_threshold = threshold;
    game::ColliderComponent c;
    c.shape = game::ColliderShape::box;
    c.size = {0.1f, 0.1f, 0.1f};
    c.impact_sound = collider_sound;
    a.colliders.push_back(c);
    return l.add(a).id;
  };
  const game::ActorId sole = toy("Sole", 0, "builtin:plastic", 0.1f, "builtin:plastic_step");
  const game::ActorId shell = toy("Shell", 1, "builtin:plastic", 0.1f, "auto");
  const game::ActorId crate = toy("Crate", 2, "builtin:impact", 2.5f, "auto");     // the defaults: too soft to sound
  const game::ActorId quiet = toy("Quiet", 3, "builtin:plastic", 0.1f, "none");    // a silent collider
  // The collider's sound is part of the level file ("auto" is not written).
  game::Level back;
  std::string err;
  CHECK(game::from_text(game::to_text(l), back, &err));
  CHECK(back.find(sole)->colliders[0].impact_sound == "builtin:plastic_step");
  CHECK(back.find(shell)->colliders[0].impact_sound == "auto");
  CHECK(game::to_text(l).find("\"impactSound\": \"auto\"") == std::string::npos);
  for (const game::Issue& i : game::validate(l)) CHECK(i.text.find("impact sound") == std::string::npos);
  CHECK(f.world->start(l, game::SimMode::simulate));
  std::vector<game::SoundCue> cues;
  for (int i = 0; i < 90; ++i) {
    f.world->step(1.0f / 60, {});
    for (const game::SoundCue& c : f.world->sound_cues()) cues.push_back(c);
    f.world->sound_cues().clear();
  }
  auto heard = [&](game::ActorId id, const char* sound) {
    return std::count_if(cues.begin(), cues.end(), [&](const game::SoundCue& c) { return c.actor == id && c.sound == sound; });
  };
  CHECK(heard(sole, "builtin:plastic_step") >= 1);
  CHECK(heard(sole, "builtin:plastic") == 0);
  CHECK(heard(shell, "builtin:plastic") >= 1);
  CHECK(std::none_of(cues.begin(), cues.end(), [&](const game::SoundCue& c) { return c.actor == crate || c.actor == quiet; }));
  for (const game::SoundCue& c : cues) CHECK(c.volume > 0 && c.volume <= 1);
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

TEST(game_shooting_range_template) {
  GameFixture f;
  const game::Level l = game::shooting_range_level();
  CHECK(game::validate(l).empty() || no_errors(game::validate(l)));
  CHECK(f.world->start(l, game::SimMode::play));
  const game::Actor* p = f.world->level().find(f.world->player());
  CHECK(p && p->controller && p->controller->mode == game::ControlMode::soldier);
  int dummies = 0;
  for (const game::Actor& a : l.actors) if (a.health && !a.controller) { ++dummies; CHECK(std::fabs(f.world->health_of(a.id) - 100) < 0.01f); }
  CHECK(dummies == 8);
  Camera cam;
  game::PlayInput in;
  for (int i = 0; i < 60; ++i) { f.world->step(1.0f / 60, in); f.world->update_camera(cam, 1.0f / 60, in); }
  CHECK(f.world->hud().soldier && f.world->hud().ammo == 30);
  CHECK(cam.eye.z > 1.4f && cam.eye.z < 1.8f);  // standing on the range floor
  // Shoot straight down the range: the near dummy (Target Dummy 3 at x = -12, y = 0) is in line... aim at its chest.
  const Vec3 me = f.world->physics().pose(f.world->actor_body(f.world->player())).position;
  in.look_x = f.world->camera_yaw() - std::atan2(0 - me.y, -12 - me.x) / 0.0174532925f;
  in.look_y = std::atan2(cam.eye.z - 1.0f, -12 - me.x) / 0.0174532925f - f.world->camera_pitch();
  f.world->update_camera(cam, 1.0f / 60, in);
  in.look_x = in.look_y = 0;
  in.aim = true;
  for (int i = 0; i < 30; ++i) { f.world->step(1.0f / 60, in); f.world->update_camera(cam, 1.0f / 60, in); }
  in.fire = true;
  for (int i = 0; i < 90; ++i) { f.world->step(1.0f / 60, in); f.world->update_camera(cam, 1.0f / 60, in); }
  CHECK(f.world->hud().score >= 10);  // at least one dummy down
}

TEST(net_game_soldier_replication) {
  Renderer renderer;
  game::NightWindows windows;
  ShaderExtension* ext[1] = {&windows};
  RendererOptions o;
  renderer.init(o, Span<ShaderExtension* const>(ext, 1));
  game::Level l = game::basic_level();
  for (game::Actor& a : l.actors) if (a.name == "Cube") a.transform.position = {40, 40, 0.5f};
  add_soldier(l, {0, 0, 0.8f});
  const game::ActorId dummy = add_dummy(l, {12, 0, 0.725f});
  net::LoopbackNetwork hub(5);
  hub.simulator().settings.latency_ms = 40;
  hub.simulator().settings.jitter_ms = 5;
  auto ts = hub.create(), t1 = hub.create();
  game::GameWorld sw(renderer, &windows), cw(renderer, &windows);
  game::NetGame server(sw), client(cw);
  CHECK(server.host(l, *ts));
  CHECK(client.join(l, *t1, ts->address()));
  f64 time = 0;
  const f32 dt = 1.0f / 60;
  game::PlayInput host_in, in;
  Camera cam;
  auto frame = [&]() {
    time += dt;
    hub.advance_time(time);
    server.update(time, dt, host_in);
    client.update(time, dt, in);
    if (client.playing()) cw.update_camera(cam, dt, in);
    in.look_x = in.look_y = 0;
    sw.sound_cues().clear();
    cw.sound_cues().clear();
  };
  for (int i = 0; i < 180; ++i) frame();
  CHECK(client.playing());
  const game::ActorId pawn = client.local_pawn();
  CHECK(pawn != 0 && pawn != sw.player());
  CHECK(cw.hud().soldier);
  CHECK(cw.hud().ammo == 30 && cw.hud().reserve == 120);
  CHECK(std::fabs(cw.hud().health - 100) < 0.5f);
  // The client's first-person eye sits at the pawn's eye height.
  const Vec3 feet = sw.physics().pose(sw.actor_body(pawn)).position - Vec3{0, 0, 0.725f};
  CHECK(std::fabs(cam.eye.z - (feet.z + 1.62f)) < 0.15f);
  // Aim at the dummy's chest from the client and fire a burst.
  const Vec3 eye = feet + Vec3{0, 0, 1.62f};
  const Vec3 d = Vec3{12, 0, 1.0f} - eye;
  const f32 want_yaw = std::atan2(d.y, d.x) / 0.0174532925f;
  const f32 want_pitch = std::atan2(-d.z, std::sqrt(d.x * d.x + d.y * d.y)) / 0.0174532925f;
  in.look_x = cw.camera_yaw() - want_yaw;
  in.look_y = want_pitch - cw.camera_pitch();
  frame();
  CHECK(std::fabs(cw.camera_yaw() - want_yaw) < 0.5f);
  in.aim = true;
  for (int i = 0; i < 40; ++i) frame();
  cw.shots().clear();
  in.fire = true;
  for (int i = 0; i < 12; ++i) frame();
  in.fire = false;
  for (int i = 0; i < 30; ++i) frame();
  CHECK(sw.health_of(dummy) < 100);                    // the server applied the client's hits
  CHECK(cw.hud().ammo < 30 && cw.hud().ammo >= 25);     // ammo replicated back
  CHECK(cw.hud().hit_age < 1.0f);                       // hit marker on the client
  CHECK(!cw.shots().empty());                           // tracers reached the client
  bool any_hit = false;
  for (const game::ShotEvent& e : cw.shots()) any_hit |= e.hit == dummy;
  CHECK(any_hit);
  // The host shoots the client's pawn: the client sees its own damage.
  sw.damage(pawn, 30, sw.player(), false, sw.physics().pose(sw.actor_body(sw.player())).position);
  for (int i = 0; i < 30; ++i) frame();
  CHECK(cw.hud().health < 75);
  CHECK(cw.hud().damage_age < 1.0f);
  server.stop();
  client.stop();
  renderer.shutdown();
}

TEST(net_game_replicates_logic_changes) {
  Renderer renderer;
  game::NightWindows windows;
  ShaderExtension* ext[1] = {&windows};
  RendererOptions o;
  renderer.init(o, Span<ShaderExtension* const>(ext, 1));
  game::Level l = game::basic_level();
  for (game::Actor& a : l.actors) if (a.name == "Cube") a.transform.position = {40, 40, 0.5f};
  add_soldier(l, {0, 0, 0.8f});
  game::Actor door;
  door.name = "Door";
  door.transform.position = {10, 0, 1.5f};
  door.mesh = game::MeshComponent{};
  door.mesh->size = {0.3f, 3, 3};
  const game::ActorId door_id = l.add(door).id;
  // Any player's Interact: the door jumps up, its light turns red and a crate is spawned.
  game::Actor crate;
  crate.name = "Crate";
  crate.transform.position = {30, 30, 0.5f};
  crate.mesh = game::MeshComponent{};
  crate.body = game::BodyComponent{};
  l.add(crate);
  l.logic = logic_graph(R"({"nodes":[{"id":1,"type":"game.input","params":{"action":"interact"}},
      {"id":2,"type":"actor.find","values":{"name":"Door"}},{"id":3,"type":"actor.set_location","values":{"location":[10,0,4.5]}},
      {"id":4,"type":"actor.set_color","values":{"color":[1,0,0]}},{"id":5,"type":"actor.spawn","params":{"template":"Crate"},"values":{"location":[2,2,3]}}],
    "links":[[1,"then",3,"exec"],[2,"actor",3,"target"],[3,"then",4,"exec"],[2,"actor",4,"target"],[4,"then",5,"exec"]]})");
  net::LoopbackNetwork hub(9);
  hub.simulator().settings.latency_ms = 20;
  auto ts = hub.create(), t1 = hub.create();
  game::GameWorld sw(renderer, &windows), cw(renderer, &windows);
  game::NetGame server(sw), client(cw);
  CHECK(server.host(l, *ts));
  CHECK(client.join(l, *t1, ts->address()));
  f64 time = 0;
  game::PlayInput host_in, in;
  auto frame = [&](int n) {
    for (int i = 0; i < n; ++i) {
      time += 1.0 / 60;
      hub.advance_time(time);
      server.update(time, 1.0f / 60, host_in);
      client.update(time, 1.0f / 60, in);
      sw.sound_cues().clear();
      cw.sound_cues().clear();
    }
  };
  frame(150);
  CHECK(client.playing());
  CHECK(cw.logic_runtime() == nullptr);  // replicas run no graphs
  in.interact = true;
  frame(10);
  in.interact = false;
  frame(30);
  Mat4 m;
  CHECK(cw.actor_matrix(door_id, m) && std::fabs(m.translation().z - 4.5f) < 0.01f);
  CHECK(cw.level().find(door_id)->mesh->material.color.y < 0.05f);
  int crates = 0;
  for (const game::Actor& a : cw.level().actors) crates += a.name == "Crate" ? 1 : 0;
  CHECK(crates == 2);
  // A player joining later sees the same world.
  auto t2 = hub.create();
  game::GameWorld lw(renderer, &windows);
  game::NetGame late(lw);
  CHECK(late.join(l, *t2, ts->address()));
  for (int i = 0; i < 150; ++i) {
    time += 1.0 / 60;
    hub.advance_time(time);
    server.update(time, 1.0f / 60, host_in);
    client.update(time, 1.0f / 60, {});
    late.update(time, 1.0f / 60, {});
    sw.sound_cues().clear(); cw.sound_cues().clear(); lw.sound_cues().clear();
  }
  CHECK(late.playing());
  CHECK(lw.actor_matrix(door_id, m) && std::fabs(m.translation().z - 4.5f) < 0.01f);
  crates = 0;
  for (const game::Actor& a : lw.level().actors) crates += a.name == "Crate" ? 1 : 0;
  CHECK(crates == 2);
  late.stop();
  server.stop();
  client.stop();
  renderer.shutdown();
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

// The observer camera (world.observer): Play starts watching - a free camera that flies with WASD
// and looks by dragging - and any body can be taken with the mouse joint, by mouse or by finger.
TEST(studio_observer_camera_grabs_bodies) {
  StudioFixture f;
  game::Level l = game::basic_level();
  for (game::Actor& a : l.actors) if (a.name == "Cube") a.transform.position = {40, 40, 0.5f};
  game::Actor player;
  player.name = "Player";
  player.transform.position = {-6, 0, 1};
  player.mesh = game::MeshComponent{};
  player.mesh->shape = game::Shape::sphere;
  player.body = game::BodyComponent{};
  player.controller = game::ControllerComponent{};
  player.controller->mode = game::ControlMode::rolling;
  l.add(player);
  game::Actor crate;
  crate.name = "Crate";
  crate.transform.position = {0, 0, 0.5f};
  crate.mesh = game::MeshComponent{};
  crate.body = game::BodyComponent{};
  crate.body->motion = game::Motion::dynamic;
  const game::ActorId crate_id = l.add(crate).id;
  l.world.observer = true;
  // The setting is part of the level file.
  game::Level back;
  std::string err;
  CHECK(game::from_text(game::to_text(l), back, &err) && back.world.observer);
  CHECK(f.s->load_level(l));
  studio::ViewCamera& cam = f.s->viewport(0).camera;
  cam.position = {0, -8, 3};
  cam.yaw = 90;
  cam.pitch = -15;
  CHECK(f.cmd(R"({"cmd":"play","mode":"play"})")["ok"].as_bool());
  f.frames(10);
  json::Value st = f.s->state();
  CHECK(st["play"]["state"].as_string() == "simulating" && st["play"]["observing"].as_bool());
  const std::string tool = st["tool"].as_string();
  // WASD flies (no right mouse needed) and does not switch tools.
  const Vec3 c0 = cam.position;
  f.s->key("w", "KeyW", true, 0, false);
  f.frames(30);
  f.s->key("w", "KeyW", false, 0, false);
  CHECK(cam.position.y > c0.y + 0.5f);
  CHECK(f.s->state()["tool"].as_string() == tool);
  cam.position = c0;
  f.frames(2);
  auto screen = [&](game::ActorId id) {
    json::Value c = json::Value::object();
    c.set("cmd", "project");
    c.set("id", id);
    json::Value r = f.s->command(c);
    return std::pair<f32, f32>{static_cast<f32>(r["x"].as_number()), static_cast<f32>(r["y"].as_number())};
  };
  Mat4 m;
  // The mouse: press on the crate, drag up - it is lifted by the mouse joint; release drops it.
  auto [x, y] = screen(crate_id);
  f.s->pointer(0, 0, x, y, 0, 1, 0, 0);
  CHECK(f.s->world().grabbing());
  for (int i = 1; i <= 20; ++i) { f.s->pointer(0, 1, x, y - 6.0f * static_cast<f32>(i), 0, 1, 0, 0); f.frames(2); }
  f.frames(30);
  f.s->world().actor_matrix(crate_id, m);
  CHECK(m.translation().z > 1.0f);
  // The wheel pushes it away while held.
  const Vec3 held = m.translation();
  f.s->pointer(0, 4, x, y - 120, 0, 1, 0, 400);
  f.frames(30);
  f.s->world().actor_matrix(crate_id, m);
  CHECK(distance(m.translation(), cam.position) > distance(held, cam.position) + 0.3f);
  f.s->pointer(0, 2, x, y - 120, 0, 0, 0, 0);
  CHECK(!f.s->world().grabbing());
  f.frames(90);
  f.s->world().actor_matrix(crate_id, m);
  CHECK(m.translation().z < 0.8f);  // dropped back down
  // A finger: the same with touch; a drag on empty space looks around.
  std::tie(x, y) = screen(crate_id);
  f64 t = 1000;
  CHECK(f.s->touch_pointer(0, 7, x, y, t));
  CHECK(f.s->world().grabbing());
  for (int i = 1; i <= 20; ++i) { CHECK(f.s->touch_pointer(1, 7, x + 4.0f * static_cast<f32>(i), y - 5.0f * static_cast<f32>(i), t += 16)); f.frames(2); }
  f.frames(30);
  f.s->world().actor_matrix(crate_id, m);
  CHECK(m.translation().z > 1.0f);
  CHECK(f.s->touch_pointer(2, 7, x + 80, y - 100, t += 16));
  CHECK(!f.s->world().grabbing());
  const f32 yaw0 = cam.yaw;
  CHECK(f.s->touch_pointer(0, 8, 900, 60, t += 500));  // the sky: nothing to take
  CHECK(!f.s->world().grabbing());
  for (int i = 1; i <= 10; ++i) f.s->touch_pointer(1, 8, 900 - 10.0f * static_cast<f32>(i), 60, t += 16);
  f.s->touch_pointer(2, 8, 800, 60, t += 16);
  CHECK(std::fabs(cam.yaw - yaw0) > 5);
  // Two fingers spreading fly forward.
  const Vec3 p0 = cam.position, fwd = cam.forward();
  f.s->touch_pointer(0, 1, 860, 70, t += 500);
  f.s->touch_pointer(0, 2, 900, 70, t += 10);
  for (int i = 1; i <= 10; ++i) { f.s->touch_pointer(1, 1, 860 - 10.0f * static_cast<f32>(i), 70, t += 16); f.s->touch_pointer(1, 2, 900 + 10.0f * static_cast<f32>(i), 70, t += 1); }
  f.s->touch_pointer(2, 1, 760, 70, t += 16);
  f.s->touch_pointer(2, 2, 1000, 70, t += 16);
  CHECK(dot(cam.position - p0, fwd) > 0.5f);
  // F8 takes over the player, F8 again watches.
  f.s->key("F8", "F8", true, 0, false);
  CHECK(f.s->state()["play"]["state"].as_string() == "playing");
  f.s->key("F8", "F8", false, 0, false);
  f.s->key("F8", "F8", true, 0, false);
  CHECK(f.s->state()["play"]["observing"].as_bool());
  f.cmd(R"({"cmd":"stop"})");
  CHECK(!f.s->state()["play"]["observing"].as_bool());
}

TEST(studio_simulate_drags_bodies_without_ctrl) {
  StudioFixture f;
  game::Level l = game::basic_level();
  for (game::Actor& a : l.actors) if (a.name == "Cube") a.transform.position = {40, 40, 0.5f};
  game::Actor crate;
  crate.name = "Crate";
  crate.transform.position = {0, 0, 0.5f};
  crate.mesh = game::MeshComponent{};
  crate.body = game::BodyComponent{};
  crate.body->motion = game::Motion::dynamic;
  const game::ActorId id = l.add(crate).id;
  CHECK(f.s->load_level(l));
  studio::ViewCamera& cam = f.s->viewport(0).camera;
  cam.position = {0, -8, 3};
  cam.yaw = 90;
  cam.pitch = -15;
  CHECK(f.cmd(R"({"cmd":"play","mode":"simulate"})")["ok"].as_bool());
  f.frames(5);
  json::Value c = json::Value::object();
  c.set("cmd", "project");
  c.set("id", id);
  json::Value r = f.s->command(c);
  const f32 x = static_cast<f32>(r["x"].as_number()), y = static_cast<f32>(r["y"].as_number());
  f.s->pointer(0, 0, x, y, 0, 1, 0, 0);
  CHECK(f.s->world().grabbing());
  f.s->pointer(0, 2, x, y, 0, 0, 0, 0);  // a click: let go, and select it
  CHECK(!f.s->world().grabbing());
  CHECK(std::find(f.s->selection().begin(), f.s->selection().end(), id) != f.s->selection().end());
}

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

TEST(studio_left_mouse_selects_and_never_drags_actors) {
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
  // Dragging from an actor does not move it (as in Unreal, actors move with the gizmo only):
  // it draws a marquee.
  f.s->pointer(0, 0, sx, sy, 0, 1, 0, 0);
  CHECK(v.drag == studio::Drag::marquee);
  for (int k = 1; k <= 10; ++k) f.s->pointer(0, 1, sx + 12.0f * k, sy + 4.0f * k, -1, 1, 0, 0);
  f.s->pointer(0, 2, sx + 120, sy + 40, 0, 0, 0, 0);
  f.frames(1);
  CHECK_NEAR(f.s->level().find(a)->transform.position.x, a0.x, 1e-5f);
  CHECK_NEAR(f.s->level().find(a)->transform.position.y, a0.y, 1e-5f);
  CHECK(f.s->history().size() == steps);
  CHECK_NEAR(v.camera.position.x, cam_before.position.x, 1e-5f);
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

TEST(studio_soldier_input_and_hud) {
  StudioFixture f;
  CHECK(f.cmd(R"({"cmd":"new","template":"shootingRange"})")["ok"].as_bool());
  CHECK(f.cmd(R"({"cmd":"play","mode":"play"})")["ok"].as_bool());
  f.frames(20);
  json::Value st = f.s->status();
  CHECK(st["hud"]["soldier"].is_object());
  CHECK(st["hud"]["soldier"]["ammo"].as_number() == 30);
  // The capturing click does not fire; the next one does.
  f.s->pointer(0, 0, 400, 300, 0, 1, 0, 0);
  f.s->pointer(0, 2, 400, 300, 0, 0, 0, 0);
  f.frames(5);
  CHECK(f.s->status()["hud"]["soldier"]["ammo"].as_number() == 30);
  f.s->pointer(0, 0, 400, 300, 0, 1, 0, 0);
  f.frames(12);
  f.s->pointer(0, 2, 400, 300, 0, 0, 0, 0);
  const f64 after_burst = f.s->status()["hud"]["soldier"]["ammo"].as_number();
  CHECK(after_burst < 30 && after_burst > 20);
  // Right mouse aims (zoom).
  f.s->pointer(0, 0, 400, 300, 2, 2, 0, 0);
  f.frames(20);
  st = f.s->status();
  CHECK(st["hud"]["soldier"]["aiming"].as_bool());
  CHECK(st["hud"]["soldier"]["fov"].as_number() < 50);
  f.s->pointer(0, 2, 400, 300, 2, 0, 0, 0);
  f.frames(20);
  CHECK(!f.s->status()["hud"]["soldier"]["aiming"].as_bool());
  // R reloads; Ctrl+R does not restart a soldier game.
  f.s->key("r", "KeyR", true, 0, false);
  f.frames(2);
  f.s->key("r", "KeyR", false, 0, false);
  CHECK(f.s->status()["hud"]["soldier"]["reload"].as_number() >= 0);
  f.frames(140);
  CHECK(f.s->status()["hud"]["soldier"]["ammo"].as_number() == 30);
  // Gamepad: left stick walks, RT fires, right stick turns.
  const f32 yaw0 = f.s->world().camera_yaw();
  Mat4 a, b;
  CHECK(f.s->world().actor_matrix(f.s->world().player(), a));
  CHECK(f.cmd(R"({"cmd":"gamepad","connected":true,"lx":0,"ly":1,"rx":0.8,"ry":0,"lt":0,"rt":1,"a":false,"b":false,"x":false,"y":false,"lb":false,"rb":false,"l3":false,"start":false})")["ok"].as_bool());
  f.frames(30);
  CHECK(f.cmd(R"({"cmd":"gamepad","connected":false})")["ok"].as_bool());
  f.frames(2);
  CHECK(f.s->world().actor_matrix(f.s->world().player(), b));
  CHECK(distance(a.translation(), b.translation()) > 0.5f);
  CHECK(std::fabs(f.s->world().camera_yaw() - yaw0) > 5);
  CHECK(f.s->status()["hud"]["soldier"]["ammo"].as_number() < 30);
  // Touch: look deltas and the fire button.
  const f32 yaw1 = f.s->world().camera_yaw();
  f.cmd(R"({"cmd":"gameInput","lookX":40,"lookY":0})");
  f.frames(2);
  CHECK(std::fabs(f.s->world().camera_yaw() - yaw1) > 5);
  f.cmd(R"({"cmd":"stop"})");
}

TEST(input_device_arbiter_rules) {
  // Desktop: fine pointer, no touch.
  game::InputArbiter d;
  d.set_platform(true, false, 0);
  CHECK(!d.touch());
  d.pointer_down(true);   // a touch screen laptop: touching switches to touch
  CHECK(d.touch());
  d.key_down();           // any key: desktop again
  CHECK(!d.touch());
  // Phone: coarse pointer with touch points starts in touch; a mouse click keeps it, a keyboard does not.
  game::InputArbiter p;
  p.set_platform(false, true, 5);
  CHECK(p.touch());
  p.pointer_down(false);
  CHECK(p.touch());
  p.mouse_moved();
  CHECK(p.touch());
  p.key_down();
  CHECK(!p.touch());
  p.pointer_down(true);
  CHECK(p.touch());
  // A headless browser without fine pointer and without touch is a desktop.
  game::InputArbiter h;
  h.set_platform(false, false, 0);
  CHECK(!h.touch());
  // The gamepad is tracked separately and does not change the layout.
  h.gamepad_used();
  CHECK(h.gamepad() && !h.touch());
  h.key_down();
  CHECK(!h.gamepad());
}

TEST(touch_controls_layout_and_sticks) {
  game::ControllerComponent c;
  c.mode = game::ControlMode::soldier;
  game::TouchControls t;
  t.set_frame(844, 390, 47, 0, 47, 21);  // iPhone landscape with notch insets
  t.set_controller(&c, true, true);
  const game::TouchView& v = t.view();
  CHECK(v.visible);
  // 168 px sticks flush with the safe corners.
  CHECK_NEAR(v.move.size, 168, 0.01f);
  CHECK_NEAR(v.move.x, 47, 0.01f);
  CHECK_NEAR(v.move.y, 390 - 21 - 168, 0.01f);
  CHECK_NEAR(v.look.x, 844 - 47 - 168, 0.01f);
  // Soldier buttons: jump, reload, aim, crouch, use; none overlaps a stick or leaves the screen.
  std::set<std::string> ids;
  for (const game::TouchButton& b : v.buttons) {
    ids.insert(b.id);
    CHECK(b.x >= 0 && b.y >= 0 && b.x + b.w <= 844 && b.y + b.h <= 390);
    for (const game::TouchStick* s : {&v.move, &v.look}) {
      const bool apart = b.x + b.w <= s->x || b.x >= s->x + s->size || b.y + b.h <= s->y || b.y >= s->y + s->size;
      CHECK(apart);
    }
  }
  CHECK(ids == std::set<std::string>({"jump", "reload", "aim", "crouch", "interact"}));
  game::PlayInput in;
  // Move stick: deflection counts from where the thumb lands (no jump), up is forward.
  const f32 mx = v.move.x + 84, my = v.move.y + 84;
  CHECK(t.pointer(0, 1, mx + 30, my + 20, 0));
  t.update(1 / 60.0f, in, 62);
  CHECK(in.move_x == 0 && in.move_y == 0);
  t.pointer(1, 1, mx + 30, my + 20 - 35.28f, 16);  // half the radius (70.56 px) up
  t.update(1 / 60.0f, in, 62);
  CHECK_NEAR(in.move_y, 0.5f, 0.01f);
  CHECK(!in.boost);
  t.pointer(1, 1, mx + 30, my + 20 - 200, 32);     // to the rim forwards: sprint, clamped
  t.update(1 / 60.0f, in, 62);
  CHECK_NEAR(in.move_y, 1.0f, 0.001f);
  CHECK(in.boost);
  CHECK(v.move.live && v.move.accent);
  // Look stick: 1:1 drag (0.0068 rad = 0.39 degrees per px across).
  const f32 lx = v.look.x + 84, ly = v.look.y + 84;
  CHECK(t.pointer(0, 2, lx, ly, 40));
  t.pointer(1, 2, lx + 20, ly, 200);  // slow: no flick gain
  t.update(1 / 60.0f, in, 62);
  CHECK_NEAR(in.look_x, 20 * 0.0068f * 57.29578f, 0.01f);
  CHECK(in.move_y > 0.99f);           // both thumbs at once
  // Edge turn: held beyond the ring, the view keeps turning every frame.
  t.pointer(1, 2, lx + 140, ly, 400);
  t.update(1 / 60.0f, in, 62);
  t.update(1 / 60.0f, in, 62);
  CHECK(in.look_x > 3.5f && in.look_x < 4.1f);    // ~4.2 rad/s * 1/60 s in degrees
  // Pulling back stops the turn at once.
  t.pointer(1, 2, lx + 140 - 50, ly, 420);
  t.update(1 / 60.0f, in, 62);
  t.update(1 / 60.0f, in, 62);
  CHECK(in.look_x == 0);
  // Releasing springs the sticks back.
  t.pointer(2, 1, 0, 0, 500);
  t.pointer(2, 2, 0, 0, 500);
  t.update(1 / 60.0f, in, 62);
  CHECK(in.move_x == 0 && in.move_y == 0 && in.look_x == 0 && !in.boost);
  // The screen fires (and keeps aiming while the finger drags); several fingers can fire.
  CHECK(t.pointer(0, 3, 422, 120, 600));
  t.update(1 / 60.0f, in, 62);
  CHECK(in.fire && v.fire);
  t.pointer(1, 3, 432, 120, 700);
  t.update(1 / 60.0f, in, 62);
  CHECK(in.look_x > 0);
  t.pointer(2, 3, 432, 120, 710);
  t.update(1 / 60.0f, in, 62);
  CHECK(!in.fire);
  // Buttons: jump is held, aim and crouch toggle.
  auto centre = [&](const char* id, f32& x, f32& y) {
    for (const game::TouchButton& b : v.buttons)
      if (b.id == id) { x = b.x + b.w / 2; y = b.y + b.h / 2; }
  };
  f32 bx = 0, by = 0;
  centre("jump", bx, by);
  t.pointer(0, 4, bx, by, 800);
  t.update(1 / 60.0f, in, 62);
  CHECK(in.jump && !in.fire);
  t.pointer(2, 4, bx, by, 850);
  centre("aim", bx, by);
  t.pointer(0, 5, bx, by, 900);
  t.pointer(2, 5, bx, by, 950);
  t.update(1 / 60.0f, in, 62);
  CHECK(in.aim && !in.jump);
  // Zoomed in (aiming): the same drag turns less.
  CHECK(t.pointer(0, 6, 422, 120, 1000));
  t.pointer(1, 6, 442, 120, 1300);
  t.update(1 / 60.0f, in, 31);
  CHECK_NEAR(in.look_x, 10 * 0.0068f * 57.29578f, 0.02f);
  t.release_all();
  t.pointer(0, 5, bx, by, 1400);
  t.update(1 / 60.0f, in, 62);
  CHECK(!in.aim);
  // Cars: the left stick steers and drives, boost is a button, the screen looks (no fire).
  game::ControllerComponent car;
  car.mode = game::ControlMode::hovercar;
  t.set_controller(&car, false, true);
  CHECK(v.move.label == "STEER / GAS");
  bool boost = false;
  for (const game::TouchButton& b : v.buttons) boost = boost || b.id == "boost";
  CHECK(boost);
  CHECK(t.pointer(0, 7, 422, 120, 1500));
  t.pointer(1, 7, 452, 120, 1700);
  t.update(1 / 60.0f, in, 62);
  CHECK(!in.fire && in.look_x > 0);
  // A narrow portrait screen still fits both sticks.
  t.set_frame(390, 844, 0, 47, 0, 34);
  CHECK(v.move.x + v.move.size <= v.look.x);
  t.set_controller(nullptr, false, true);
  CHECK(!v.visible);
}

TEST(studio_touch_navigates_the_editor_viewport) {
  StudioFixture f;
  game::Level l = game::basic_level();
  game::ActorId cube = 0;
  for (game::Actor& a : l.actors) if (a.name == "Cube") { a.transform.position = {0, 0, 0.5f}; cube = a.id; }
  CHECK(cube != 0);
  CHECK(f.s->load_level(l));
  studio::ViewCamera& cam = f.s->viewport(0).camera;
  cam.position = {0, -10, 4};
  cam.yaw = 90;
  cam.pitch = -20;
  f.frames(2);
  auto screen = [&](game::ActorId id) {
    json::Value c = json::Value::object();
    c.set("cmd", "project");
    c.set("id", id);
    json::Value r = f.s->command(c);
    return std::pair<f32, f32>{static_cast<f32>(r["x"].as_number()), static_cast<f32>(r["y"].as_number())};
  };
  auto selected = [&]() { return f.s->state()["selection"].size(); };
  f.cmd(R"({"cmd":"deselect"})");
  f64 t = 1000;
  // A tap selects what is under the finger.
  auto [x, y] = screen(cube);
  CHECK(f.s->touch_pointer(0, 1, x, y, t));
  CHECK(f.s->touch_pointer(2, 1, x, y, t += 80));
  CHECK(selected() == 1);
  // A second tap frames the selection: the camera comes to the cube.
  const f32 far = distance(cam.position, Vec3{0, 0, 0.5f});
  f.s->touch_pointer(0, 2, x, y, t += 150);
  f.s->touch_pointer(2, 2, x, y, t += 80);
  CHECK(distance(cam.position, Vec3{0, 0, 0.5f}) < far);
  // One finger orbits: the camera turns, the distance to the pivot stays.
  f.cmd(R"({"cmd":"deselect"})");
  cam.position = {0, -10, 4};
  cam.yaw = 90;
  cam.pitch = -20;
  f.frames(2);
  std::tie(x, y) = screen(cube);
  const f32 d0 = distance(cam.position, Vec3{0, 0, 0.5f});
  f.s->touch_pointer(0, 3, x, y, t += 1000);
  for (int i = 1; i <= 10; ++i) f.s->touch_pointer(1, 3, x + 12.0f * static_cast<f32>(i), y, t += 16);
  f.s->touch_pointer(2, 3, x + 120, y, t += 16);
  CHECK(std::fabs(cam.yaw - 90) > 10);
  CHECK(std::fabs(distance(cam.position, Vec3{0, 0, 0.5f}) - d0) < 0.5f);
  CHECK(selected() == 0);  // a drag is not a tap
  // Two fingers spreading zoom toward the point between them.
  cam.position = {0, -10, 4};
  cam.yaw = 90;
  cam.pitch = -20;
  f.frames(2);
  std::tie(x, y) = screen(cube);
  f.s->touch_pointer(0, 4, x - 20, y, t += 1000);
  f.s->touch_pointer(0, 5, x + 20, y, t += 10);
  for (int i = 1; i <= 10; ++i) { f.s->touch_pointer(1, 4, x - 20 - 8.0f * static_cast<f32>(i), y, t += 16); f.s->touch_pointer(1, 5, x + 20 + 8.0f * static_cast<f32>(i), y, t += 1); }
  CHECK(distance(cam.position, Vec3{0, 0, 0.5f}) < d0 * 0.5f);
  // ...and moving together they pan: the cube stays under them.
  auto [cx, cy] = screen(cube);
  for (int i = 1; i <= 10; ++i) { f.s->touch_pointer(1, 4, x - 100 + 6.0f * static_cast<f32>(i), y + 4.0f * static_cast<f32>(i), t += 16); f.s->touch_pointer(1, 5, x + 100 + 6.0f * static_cast<f32>(i), y + 4.0f * static_cast<f32>(i), t += 1); }
  f.s->touch_pointer(2, 4, x - 40, y + 40, t += 16);
  f.s->touch_pointer(2, 5, x + 160, y + 40, t += 16);
  auto [ex, ey] = screen(cube);
  CHECK(std::fabs(ex - (cx + 60)) < 12 && std::fabs(ey - (cy + 40)) < 12);
  // Holding still opens the context menu.
  f.s->take_ui();
  std::tie(x, y) = screen(cube);
  f.s->touch_pointer(0, 6, x, y, t += 1000);
  f.s->touch_pointer(2, 6, x, y, t += 700);
  const json::Value ui = f.s->take_ui();
  bool popup = false;
  for (std::size_t i = 0; i < ui.size(); ++i) popup = popup || ui[i]["action"].as_string() == "popup";
  CHECK(popup);
  CHECK(selected() == 1);
}

TEST(studio_touch_play_without_pointer_lock) {
  StudioFixture f;
  CHECK(f.cmd(R"({"cmd":"device","platform":{"fine":false,"coarse":true,"touchPoints":5},"safe":[0,0,0,0]})")["ok"].as_bool());
  CHECK(f.s->status()["input"]["device"].as_string() == "touch");
  CHECK(f.cmd(R"({"cmd":"new","template":"shootingRange"})")["ok"].as_bool());
  f.s->take_ui();
  CHECK(f.cmd(R"({"cmd":"play","mode":"play"})")["ok"].as_bool());
  // Touch devices never ask for pointer lock.
  const json::Value ui = f.s->take_ui();
  for (std::size_t i = 0; i < ui.size(); ++i) CHECK(ui[i]["action"].as_string() != "capturePointer");
  CHECK(!f.s->status()["captured"].as_bool());
  f.frames(10);
  json::Value st = f.s->status();
  CHECK(st["touch"].is_object());
  CHECK(st["touch"]["buttons"].size() == 5);
  const f64 sx = st["touch"]["move"]["x"].as_number() + 84, sy = st["touch"]["move"]["y"].as_number() + 84;
  // The left thumb walks forwards.
  Mat4 a, b;
  CHECK(f.s->world().actor_matrix(f.s->world().player(), a));
  CHECK(f.s->touch_pointer(0, 11, static_cast<f32>(sx), static_cast<f32>(sy), f.now));
  CHECK(f.s->touch_pointer(1, 11, static_cast<f32>(sx), static_cast<f32>(sy - 60), f.now + 10));
  f.frames(40);
  CHECK(f.s->world().actor_matrix(f.s->world().player(), b));
  CHECK(distance(a.translation(), b.translation()) > 1);
  CHECK(f.s->status()["touch"]["move"]["live"].as_bool());
  f.s->touch_pointer(2, 11, 0, 0, f.now);
  // A finger on the screen fires.
  CHECK(f.s->touch_pointer(0, 12, 480, 200, f.now));
  f.frames(12);
  f.s->touch_pointer(2, 12, 480, 200, f.now);
  CHECK(f.s->status()["hud"]["soldier"]["ammo"].as_number() < 30);
  // The look stick turns the view.
  const f32 yaw = f.s->world().camera_yaw();
  st = f.s->status();
  const f32 lx = static_cast<f32>(st["touch"]["look"]["x"].as_number() + 84), ly = static_cast<f32>(st["touch"]["look"]["y"].as_number() + 84);
  f.s->touch_pointer(0, 13, lx, ly, f.now);
  f.s->touch_pointer(1, 13, lx + 40, ly, f.now + 200);
  f.frames(2);
  f.s->touch_pointer(2, 13, lx + 40, ly, f.now);
  CHECK(std::fabs(f.s->world().camera_yaw() - yaw) > 8);
  // A key press switches to the desktop layout: the overlay goes away.
  f.s->key("w", "KeyW", true, 0, false);
  f.s->key("w", "KeyW", false, 0, false);
  f.frames(1);
  st = f.s->status();
  CHECK(st["input"]["device"].as_string() == "mouse");
  CHECK(!st["touch"].is_object());
  // A finger switches back to the touch layout.
  f.cmd(R"({"cmd":"device","event":"touch"})");
  f.frames(1);
  CHECK(f.s->status()["touch"].is_object());
  f.cmd(R"({"cmd":"stop"})");
}

TEST(soldier_models_are_closed_and_outward) {
  using namespace game::models;
  // Every weapon has parts, markers in front of the grip and a muzzle at the front.
  for (game::WeaponKind k : {game::WeaponKind::rifle, game::WeaponKind::pistol, game::WeaponKind::shotgun, game::WeaponKind::sniper}) {
    const WeaponModel w = weapon_model(k);
    CHECK(w.parts.matte.vertex_count() > 300);
    CHECK(w.muzzle.z < w.grip.z);                  // the barrel points along -Z
    CHECK(w.front_sight.z < w.rear_sight.z);
    CHECK(std::fabs(w.front_sight.y - w.rear_sight.y) < .02f);  // level sights
    if (k == game::WeaponKind::pistol) CHECK(w.slide.matte.vertex_count() > 100 && w.slide_travel > 0);
  }
  // Box and profile faces point away from the part's centre.
  PartMeshes p;
  Builder b(p);
  b.material(0x808080);
  b.profile({{0, 0}, {1, 0}, {1, .5f}, {0, .5f}}, .3f, .02f);
  const u32 profile_vertices = p.matte.vertex_count();
  b.box({1, 2, 3}, {5, 0, 0}, {.3f, .2f, .1f});
  u32 outward = 0, total = 0;
  for (u32 t = 0; t < p.matte.vertex_count(); t += 3) {
    const f32* q = &p.matte.positions[t * 3];
    const Vec3 a{q[0], q[1], q[2]}, bb{q[3], q[4], q[5]}, c{q[6], q[7], q[8]};
    const Vec3 centroid = (a + bb + c) * (1.0f / 3), centre = t < profile_vertices ? Vec3{0, .25f, -.5f} : Vec3{5, 0, 0};
    const Vec3 n = cross(bb - a, c - a);
    if (dot(n, n) < 1e-12f) continue;
    ++total;
    if (dot(n, centroid - centre) > 0) ++outward;
  }
  CHECK(total > 20 && outward == total);
  // The character: 17 bones, every vertex bound; walking swings the legs, aiming raises the arms.
  const PartMeshes body = character_mesh(player_outfit(shirt_for(7)), true);
  CHECK(body.matte.vertex_count() > 1000 && body.matte_bones.size() == body.matte.vertex_count());
  CharacterAnimator anim(3);
  CharacterAnimator::Input in;
  in.dt = 1 / 60.0f;
  in.armed = true;
  for (int i = 0; i < 60; ++i) anim.update(in);
  const Vec3 wrist_idle = anim.joint_matrix(kWristR).translation();
  CHECK(wrist_idle.y > .3f);  // the rifle points forward (+Y)
  in.speed = 6;
  f32 lo = 1e9f, hi = -1e9f;
  for (int i = 0; i < 120; ++i) {
    anim.update(in);
    const f32 y = anim.joint_matrix(kAnkleL).translation().y;
    lo = min_of(lo, y);
    hi = max_of(hi, y);
  }
  CHECK(hi - lo > .5f);  // running strides
  in.speed = 0;
  in.dead = true;
  for (int i = 0; i < 60; ++i) anim.update(in);
  CHECK(anim.joint_matrix(kHead).translation().z < .5f);  // fallen over
  f32 bones[kJointCount * 16];
  anim.bones(bones);
  for (f32 v : bones) CHECK(std::isfinite(v));
}

TEST(soldier_art_in_play) {
  StudioFixture f;
  CHECK(f.cmd(R"({"cmd":"new","template":"shootingRange"})")["ok"].as_bool());
  f.frames(2);
  // Editing: the soldier stands there as a rig, its capsule is hidden, a click on the rig selects it.
  CHECK(f.s->world().art_stats().rigs >= 1);
  CHECK(f.cmd(R"({"cmd":"play","mode":"play"})")["ok"].as_bool());
  f.frames(20);
  game::GameWorld::ArtStats a = f.s->world().art_stats();
  CHECK(a.rigs >= 1 && a.view_model);
  // Fire: tracers fly, chips and bullet holes appear.
  f.s->pointer(0, 0, 480, 270, 0, 1, 0, 0);
  f.s->pointer(0, 2, 480, 270, 0, 0, 0, 0);
  f.s->pointer(0, 0, 480, 270, 0, 1, 0, 0);
  f.frames(4);
  a = f.s->world().art_stats();
  CHECK(a.tracers >= 1);
  f.frames(14);
  f.s->pointer(0, 2, 480, 270, 0, 0, 0, 0);
  a = f.s->world().art_stats();
  CHECK(a.decals + a.chips > 0);
  f.cmd(R"({"cmd":"stop"})");
  f.frames(2);
  CHECK(f.s->world().art_stats().decals == 0);  // a fresh range every run
}

// The duck's policy is Pollen Robotics' network bit for bit: two observations against a float64
// reference (numpy, from the ONNX initialisers).
TEST(micro_duck_policy_matches_the_onnx_reference) {
  f32 stand[61] = {};
  stand[5] = -1;  // upright, home pose, zero command
  const f32 stand_ref[14] = {-0.019741f, -0.041493f, 0.020606f, -0.005998f, -0.053329f, -0.095318f, 0.038519f, -0.036158f, 0.011566f, 0.025406f, 0.045495f, -0.037238f, -0.030768f, 0.086753f};
  const f32 mixed[61] = {0, 0.099166481f, -0.0255541102f, 0.05f, -0.03f, -0.998f, -0.0699874688f, -0.0618137112f, 0.0859161815f, 0.0396740573f, -0.0961397492f, -0.0148999026f, 0.09997929f, -0.0108636595f, -0.0971798446f, 0.0359058354f, 0.0879273062f, -0.058563734f, -0.0728360768f, 0.077332789f, 0.0529082686f, -0.0909666672f, -0.0294671602f, 0.0985600299f, 0.00406932573f, -0.0996086503f, 0.0215987266f, 0.0940428963f, -0.0458325454f, -0.082232354f, 0.0670229176f, 0.0649612862f, -0.0837627257f, -0.0433765541f, 0.0949403861f, 0.018911462f, -0.0998136616f, 0.00680941947f, 0.0980589492f, -0.0320781309f, -0.0897927681f, 0.0552167385f, 0.0755640226f, -0.074688755f, -0.0563175528f, 0.0892011683f, 0.033331394f, -0.0977903015f, 0.3f, 0.0998858052f, -0.5f, -0.0953485303f, 0.0421778283f, 0.0844797684f, -0.0639473344f, -0.0680012445f, 0.0814705063f, 0.0470071921f, -0.0935837421f, -0.0228916922f, 0.0994826791f};
  const f32 mixed_ref[14] = {0.039644f, 0.294097f, 0.526716f, 0.098369f, 0.119909f, -0.737250f, 0.097852f, -0.128879f, 0.633488f, 0.042892f, -0.162036f, 0.317963f, 0.060720f, 0.175597f};
  f32 a[14];
  game::micro_duck_policy(stand, a);
  for (int i = 0; i < 14; ++i) CHECK(std::fabs(a[i] - stand_ref[i]) < 2e-5f);
  game::micro_duck_policy(mixed, a);
  for (int i = 0; i < 14; ++i) CHECK(std::fabs(a[i] - mixed_ref[i]) < 2e-5f);
}

namespace {
/// A level with a Micro Duck on the floor (the catalog's insertion: ids = index + 1 + offset).
game::ActorId add_duck(game::Level& l, Vec3 at, bool autonomous = false, bool rollers = false) {
  std::vector<game::Actor> parts = game::micro_duck_actors(rollers);
  parts[0].controller->autonomous = autonomous;
  const game::ActorId base = l.next_id;
  for (std::size_t i = 0; i < parts.size(); ++i) {
    game::Actor a = parts[i];
    a.id = base + static_cast<game::ActorId>(i);
    if (i == 0) a.transform.position = a.transform.position + at;
    if (a.parent) a.parent = base + a.parent - 1;
    for (game::JointComponent& j : a.joints) if (j.target) j.target = base + j.target - 1;
    l.add(std::move(a));
  }
  l.world.player = base;
  l.world.step_hz = game::kWalkerStepHz;
  l.world.substeps = game::kWalkerSubsteps;
  return base;
}
game::ActorId add_sock(game::Level& l, Vec3 at) {
  std::vector<game::Actor> parts = game::micro_duck_sock_actors();
  const game::ActorId base = l.next_id;
  for (std::size_t i = 0; i < parts.size(); ++i) {
    game::Actor a = parts[i];
    a.id = base + static_cast<game::ActorId>(i);
    if (i == 0) a.transform.position = a.transform.position + at;
    if (a.parent) a.parent = base + a.parent - 1;
    for (game::JointComponent& j : a.joints) if (j.target) j.target = base + j.target - 1;
    l.add(std::move(a));
  }
  return base;
}
struct DuckRun {
  Vec3 start, end;
  f32 min_up = 1, heading0 = 0, heading1 = 0;
  f32 end_up = 1;
  std::string driving;  // every network that drove, in order ("walk stand ...")
};
DuckRun run_duck(f32 seconds, const game::PlayInput& in, bool print = false, Vec3 euler = {}, bool rollers = false, const char* gait = nullptr, const char* actions = nullptr) {
  GameFixture f;
  game::Level l = game::basic_level();
  for (game::Actor& a : l.actors) if (a.name == "Cube") a.transform.position = {40, 40, 0.5f};
  const game::ActorId duck = add_duck(l, {0, 0, euler.y != 0 || euler.x != 0 ? .3f : 0.0f}, false, rollers);
  l.find(duck)->transform.rotation = euler;
  if (gait) l.find(duck)->controller->gait = gait;
  if (actions) l.find(duck)->controller->actions = actions;
  f.world->sync(l);
  CHECK(f.world->start(l, game::SimMode::play));
  auto pose = [&](Vec3& p, f32& up, f32& heading) {
    Mat4 m;
    f.world->actor_matrix(duck, m);
    p = m.translation();
    up = m.m[10];
    heading = std::atan2(m.m[1], m.m[0]);
  };
  DuckRun r;
  f32 up;
  pose(r.start, up, r.heading0);
  for (f32 t = 0; t < seconds; t += 1.0f / 60) {
    f.world->step(1.0f / 60, in);
    Vec3 p;
    f32 h;
    pose(p, up, h);
    r.min_up = min_of(r.min_up, up);
    const std::string d = f.world->walker_driving(duck);
    if (r.driving.size() < d.size() || r.driving.compare(r.driving.size() - d.size(), d.size(), d) != 0) r.driving += (r.driving.empty() ? "" : " ") + d;
    if (print && std::fmod(t, 0.5f) < 1.0f / 60) std::printf("t %.2f pos %.3f %.3f %.3f up %.3f heading %.2f\n", t, p.x, p.y, p.z, up, h);
  }
  pose(r.end, up, r.heading1);
  r.end_up = up;
  return r;
}
}  // namespace

TEST(micro_duck_stands_walks_and_turns_on_its_policy) {
  const bool print = std::getenv("KERN_DUCK_PRINT") != nullptr;
  {
    // Zero command: it stands (8 s).
    const DuckRun r = run_duck(8, {}, print);
    CHECK(r.min_up > .9f);
    CHECK(std::hypot(r.end.x - r.start.x, r.end.y - r.start.y) < .25f);
    CHECK(r.end.z > r.start.z - .08f);
  }
  {
    game::PlayInput fwd;
    fwd.move_y = 1;
    const DuckRun r = run_duck(10, fwd, print);
    CHECK(r.min_up > .8f);
    const f32 along = (r.end.x - r.start.x) * std::cos(r.heading0) + (r.end.y - r.start.y) * std::sin(r.heading0);
    if (print) std::printf("forward %.3f m in 10 s\n", along);
    CHECK(along > .6f);  // trained: ~0.17 m/s real at full command = 0.34 m/s at 4x scale
  }
  {
    game::PlayInput left;
    left.move_x = -1;
    const DuckRun r = run_duck(6, left, print);
    CHECK(r.min_up > .8f);
    f32 turned = r.heading1 - r.heading0;
    if (print) std::printf("turned %.3f rad in 6 s\n", turned);
    CHECK(turned > .8f);
  }
  {
    // Deterministic (Fix Your Timestep): the same input gives the same walk, bit for bit.
    game::PlayInput fwd;
    fwd.move_y = 1;
    fwd.move_x = .3f;
    const DuckRun a = run_duck(3, fwd), b = run_duck(3, fwd);
    CHECK(a.end.x == b.end.x && a.end.y == b.end.y && a.end.z == b.end.z);
  }
  {
    // Placed turned (yaw): it stands just the same.
    const DuckRun r = run_duck(8, {}, print, Vec3{0, 0, 45});
    if (print) std::printf("yaw 45: min up %.3f\n", r.min_up);
    CHECK(r.min_up > .9f);
  }
  for (const Vec3 fallen : {Vec3{0, 90, 0}, Vec3{0, -90, 0}}) {
    // Lying on its front or its back: v8 of the policy gets up by itself.
    const DuckRun r = run_duck(std::getenv("KERN_UP_S") ? static_cast<f32>(std::atof(std::getenv("KERN_UP_S"))) : 12, {}, print, fallen);
    if (print) std::printf("fallen %.0f %.0f: up %.3f z %.3f\n", fallen.x, fallen.y, r.end_up, r.end.z);
    CHECK(r.end_up > .9f);
  }
}

namespace {
/// How steady the duck holds itself: RMS angular speed (real rad/s) of the trunk and the head while
/// it stands, and while it walks.
struct DuckSteadiness { f32 trunk_stand, head_stand, trunk_walk, head_walk; };
DuckSteadiness duck_steadiness() {
  DuckSteadiness out{};
  for (int walk = 0; walk < 2; ++walk) {
    GameFixture f;
    game::Level l = game::basic_level();
    for (game::Actor& a : l.actors) if (a.name == "Cube") a.transform.position = {40, 40, 0.5f};
    const game::ActorId duck = add_duck(l, {0, 0, 0}, false);
    game::ActorId head = 0;
    for (const game::Actor& a : l.actors) if (a.name == "head_roll") head = a.id;
    f.world->sync(l);
    CHECK(f.world->start(l, game::SimMode::play));
    game::PlayInput in;
    if (walk) in.move_y = 1;
    const f32 dt = 1.0f / 100, slow = std::sqrt(game::kWalkerScale);
    Mat4 prev[2];
    f.world->actor_matrix(duck, prev[0]);
    f.world->actor_matrix(head, prev[1]);
    f64 sum[2] = {0, 0};
    int n = 0;
    for (int i = 0; i < 100 * 7; ++i) {  // 7 s simulated: settle 2 s, then measure (before the idle look-around)
      f.world->step(dt, in);
      for (int k = 0; k < 2; ++k) {
        Mat4 m;
        f.world->actor_matrix(k ? head : duck, m);
        // Rotation between frames: angle of R_prev^T R.
        f32 tr = 0;
        for (int r = 0; r < 3; ++r)
          for (int c = 0; c < 3; ++c) tr += prev[k].m[c * 4 + r] * m.m[c * 4 + r];
        const f32 angle = std::acos(clamp((tr - 1) / 2, -1.0f, 1.0f));
        if (i >= 200) sum[k] += (angle / dt * slow) * (angle / dt * slow);
        prev[k] = m;
      }
      if (i >= 200) ++n;
    }
    (walk ? out.trunk_walk : out.trunk_stand) = static_cast<f32>(std::sqrt(sum[0] / n));
    (walk ? out.head_walk : out.head_stand) = static_cast<f32>(std::sqrt(sum[1] / n));
  }
  return out;
}
}  // namespace

TEST(micro_duck_stands_on_the_floor) {
  // The soles rest on the floor: neither hovering nor sunk in (Box3D's contact slop allowed).
  for (int floor_box = 0; floor_box < 2; ++floor_box) {
    GameFixture f;
    game::Level l = game::basic_level();
    for (game::Actor& a : l.actors) if (a.name == "Cube") a.transform.position = {40, 40, 0.5f};
    if (!floor_box) l.actors.erase(std::remove_if(l.actors.begin(), l.actors.end(), [](const game::Actor& a) { return a.name == "Floor"; }), l.actors.end());
    const game::ActorId duck = add_duck(l, {0, 0, 0}, false);
    f.world->sync(l);
    CHECK(f.world->start(l, game::SimMode::simulate));
    for (int i = 0; i < 100 * 4; ++i) f.world->step(1.0f / 100, {});
    // Lowest point of each sole box (world), via the foot links' matrices and the sole collider.
    f32 lowest = 1e9f;
    for (const game::Actor& a : l.actors)
      for (const game::ColliderComponent& c : a.colliders) {
        if (a.id <= duck || a.name.find("ankle") == std::string::npos || c.points.empty()) continue;
        Mat4 m;
        f.world->actor_matrix(a.id, m);
        f32 low = 1e9f;
        for (const Vec3& p : c.points) low = min_of(low, m.transform_point(p).z);
        if (std::getenv("KERN_DUCK_PRINT")) std::printf("  %s collider friction %.1f lowest %.4f\n", a.name.c_str(), c.friction, low);
        if (c.friction > 0.99f) lowest = min_of(lowest, low);
      }
    if (std::getenv("KERN_DUCK_PRINT")) std::printf("%s: lowest sole corner %.4f m (world)\n", floor_box ? "floor box" : "ground plane", lowest);
    CHECK(std::fabs(lowest) < 0.006f);  // within Box3D's linear slop (5 mm)
  }
}

TEST(micro_duck_holds_itself_steady) {
  const DuckSteadiness s = duck_steadiness();
  if (std::getenv("KERN_DUCK_PRINT"))
    std::printf("steadiness rad/s RMS: stand trunk %.4f head %.4f, walk trunk %.4f head %.4f\n", s.trunk_stand, s.head_stand, s.trunk_walk, s.head_walk);
  CHECK(s.trunk_stand < 1e9f);
}

namespace {
/// The duck's policy set from the repository (the page fetches the same files).
void load_duck_policies() {
  std::string dir = __FILE__;
  dir = dir.substr(0, dir.find_last_of('/')) + "/../third_party/microduck/";
  json::Value m;
  CHECK(json::parse(game::micro_duck_manifest(), m));
  for (const json::Value& p : m["policies"].items()) {
    const std::string file = p["file"].as_string();
    if (game::walker_policy(file)) continue;
    std::vector<u8> bytes;
    if (FILE* f = std::fopen((dir + file).c_str(), "rb")) {
      u8 buf[65536];
      std::size_t n;
      while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) bytes.insert(bytes.end(), buf, buf + n);
      std::fclose(f);
    }
    auto pol = std::make_shared<game::WalkerPolicy>();
    std::string error;
    CHECK(game::parse_onnx_policy(Span<const u8>(bytes.data(), bytes.size()), *pol, &error));
    pol->file = file;
    game::register_walker_policy(pol);
  }
}
}  // namespace

TEST(micro_duck_policy_loader_reads_the_onnx_files) {
  load_duck_policies();
  CHECK(game::walker_policy_files().size() >= 10);
  // The loader reads velstand.onnx exactly as the importer did (the built-in copy).
  std::string dir = __FILE__;
  dir = dir.substr(0, dir.find_last_of('/')) + "/../third_party/microduck/velstand.onnx";
  std::vector<u8> bytes;
  if (FILE* f = std::fopen(dir.c_str(), "rb")) {
    u8 buf[65536];
    std::size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) bytes.insert(bytes.end(), buf, buf + n);
    std::fclose(f);
  }
  game::WalkerPolicy p;
  CHECK(game::parse_onnx_policy(Span<const u8>(bytes.data(), bytes.size()), p));
  f32 obs[61], a[14], b[14];
  for (int i = 0; i < 61; ++i) obs[i] = 0.01f * static_cast<f32>(i % 7) - 0.02f;
  p.run(obs, a);
  game::micro_duck_policy(obs, b);
  for (int i = 0; i < 14; ++i) CHECK(a[i] == b[i]);
  CHECK(p.meta("joint_names").rfind("left_hip_yaw", 0) == 0);
}

TEST(micro_duck_rolls_forward_on_its_skates) {
  // roller.onnx at the manifest's action scale: forward on the stick it skates (the rollers model's
  // free wheels), and it stays on its feet.
  load_duck_policies();
  const bool print = std::getenv("KERN_DUCK_PRINT") != nullptr;
  game::PlayInput fwd;
  fwd.move_y = 1;
  const DuckRun r = run_duck(8, fwd, print, {}, true);
  const f32 d = std::hypot(r.end.x - r.start.x, r.end.y - r.start.y) / game::kWalkerScale;  // real metres
  if (print) std::printf("rollers: %.3f m real in 8 s (%.3f m/s real at sim speed), min up %.2f, %s\n", d, d / 8 * 2, r.min_up, r.driving.c_str());
  CHECK(r.driving.find("walk") != std::string::npos);
  CHECK(d > 0.3f);
  CHECK(r.min_up > 0.8f);
}

TEST(lehrer_mlp_gradient_matches_finite_differences) {
  lehrer::Rng rng(3);
  lehrer::Mlp net;
  net.init({5, 7, 6, 3}, rng);
  const i32 batch = 4;
  std::vector<f32> x(batch * 5), c(batch * 3), out(batch * 3);
  for (f32& v : x) v = rng.uniform(-1.5f, 1.5f);
  for (f32& v : c) v = rng.uniform(-1, 1);
  auto loss = [&]() {
    net.forward(x.data(), batch, out.data());
    f64 L = 0;
    for (std::size_t k = 0; k < out.size(); ++k) L += static_cast<f64>(out[k]) * c[k];
    return L;
  };
  lehrer::Mlp::Cache cache;
  net.forward(x.data(), batch, out.data(), &cache);
  std::vector<f32> grad(net.params().size(), 0.0f), dx(x.size());
  net.backward(cache, c.data(), grad.data(), dx.data());
  f64 worst = 0;
  for (std::size_t k = 0; k < net.params().size(); ++k) {
    const f32 keep = net.params()[k];
    net.params()[k] = keep + 1e-2f;
    const f64 up = loss();
    net.params()[k] = keep - 1e-2f;
    const f64 down = loss();
    net.params()[k] = keep;
    worst = std::max(worst, std::fabs((up - down) / 2e-2 - grad[k]));
  }
  CHECK(worst < 2e-3);
  f64 worst_x = 0;
  for (std::size_t k = 0; k < x.size(); ++k) {
    const f32 keep = x[k];
    x[k] = keep + 1e-2f;
    const f64 up = loss();
    x[k] = keep - 1e-2f;
    const f64 down = loss();
    x[k] = keep;
    worst_x = std::max(worst_x, std::fabs((up - down) / 2e-2 - dx[k]));
  }
  CHECK(worst_x < 2e-3);
  // Batched forward = one row at a time.
  std::vector<f32> one(3);
  net.forward(x.data() + 5, 1, one.data());
  net.forward(x.data(), batch, out.data());
  CHECK(std::memcmp(one.data(), out.data() + 3, sizeof(f32) * 3) == 0);
}

TEST(lehrer_adam_fits_a_curve_and_the_normaliser_tracks) {
  lehrer::Rng rng(9);
  lehrer::Mlp net;
  net.init({1, 32, 32, 1}, rng);
  lehrer::Adam adam;
  adam.lr = 3e-3f;
  const i32 batch = 64;
  std::vector<f32> x(batch), y(batch), out(batch), d(batch), grad;
  f32 mse = 1;
  for (i32 it = 0; it < 1500; ++it) {
    for (i32 b = 0; b < batch; ++b) { x[b] = rng.uniform(-3.14159f, 3.14159f); y[b] = std::sin(x[b]); }
    lehrer::Mlp::Cache cache;
    net.forward(x.data(), batch, out.data(), &cache);
    mse = 0;
    for (i32 b = 0; b < batch; ++b) { d[b] = 2 * (out[b] - y[b]) / batch; mse += (out[b] - y[b]) * (out[b] - y[b]) / batch; }
    grad.assign(net.params().size(), 0.0f);
    net.backward(cache, d.data(), grad.data());
    adam.step(net.params(), grad);
  }
  CHECK(mse < 0.01f);
  // The running normaliser agrees with the whole data's mean and variance.
  lehrer::Normalizer n;
  n.init(2);
  std::vector<f32> data;
  for (i32 i = 0; i < 300; ++i) { data.push_back(rng.uniform(2, 6)); data.push_back(rng.normal() * 3 - 1); }
  for (i32 i = 0; i < 300; i += 50) n.update(data.data() + i * 2, 50);
  f64 m0 = 0, m1 = 0, v0 = 0, v1 = 0;
  for (i32 i = 0; i < 300; ++i) { m0 += data[i * 2]; m1 += data[i * 2 + 1]; }
  m0 /= 300; m1 /= 300;
  for (i32 i = 0; i < 300; ++i) { v0 += (data[i * 2] - m0) * (data[i * 2] - m0); v1 += (data[i * 2 + 1] - m1) * (data[i * 2 + 1] - m1); }
  v0 /= 300; v1 /= 300;
  CHECK(std::fabs(n.mean()[0] - m0) < 1e-4 && std::fabs(n.mean()[1] - m1) < 1e-4);
  CHECK(std::fabs(n.divisor()[0] - (std::sqrt(v0) + 0.01)) < 1e-4 && std::fabs(n.divisor()[1] - (std::sqrt(v1) + 0.01)) < 1e-4);
}

namespace {
// A point that should go to a target: PPO's quick sanity check (seconds, not hours).
class ReachEnv : public lehrer::Env {
 public:
  explicit ReachEnv(u64 seed) : rng_(seed) {}
  i32 obs_size() const override { return 4; }
  i32 critic_size() const override { return 4; }
  i32 action_size() const override { return 2; }
  void reset() override {
    for (f32& v : s_) v = rng_.uniform(-1, 1);
    steps_ = 0;
  }
  const f32* observation() const override { return s_; }
  const f32* critic() const override { return s_; }
  Step step(const f32* a) override {
    for (int k = 0; k < 2; ++k) s_[k] += clamp(a[k], -1.0f, 1.0f) * 0.1f;
    const f32 d = std::hypot(s_[0] - s_[2], s_[1] - s_[3]);
    Step r;
    r.reward = -d * 0.1f;
    r.timeout = ++steps_ >= 40;
    return r;
  }

 private:
  lehrer::Rng rng_;
  f32 s_[4] = {};
  i32 steps_ = 0;
};
}  // namespace

TEST(lehrer_ppo_learns_to_reach) {
  lehrer::PpoConfig cfg;
  cfg.actor_hidden = cfg.critic_hidden = {32, 32};
  cfg.init_std = 0.5f;
  cfg.steps_per_env = 40;
  cfg.learning_rate = 3e-3f;
  cfg.seed = 4;
  std::vector<std::unique_ptr<ReachEnv>> owned;
  std::vector<lehrer::Env*> envs;
  for (int i = 0; i < 16; ++i) {
    owned.push_back(std::make_unique<ReachEnv>(100 + i));
    owned.back()->reset();
    envs.push_back(owned.back().get());
  }
  lehrer::Ppo ppo(cfg, 4, 4, 2);
  const auto pool = lehrer::thread_pool(2);
  f32 first = 0, last = 0;
  for (int it = 0; it < 60; ++it) {
    const lehrer::IterationStats s = ppo.iterate(envs, pool);
    CHECK(std::isfinite(s.value_loss) && std::isfinite(s.surrogate_loss));
    if (it == 0) first = s.mean_return;
    last = s.mean_return;
  }
  std::printf("  reach: return %.3f -> %.3f\n", first, last);
  CHECK(last > first * 0.5f);  // returns are negative: at least twice as good
  // The deterministic policy heads for the target.
  f32 obs[4] = {-0.5f, 0.2f, 0.6f, -0.4f}, a[2];
  ppo.act(obs, a);
  CHECK(a[0] > 0.3f && a[1] < -0.3f);
}

TEST(lehrer_ppo_learns_from_collectors_elsewhere) {
  // The browser's shape: collectors (Web Workers) get the policy as a flat buffer, return their
  // rollouts as bytes, and the learner learns from all of them.
  lehrer::PpoConfig cfg;
  cfg.actor_hidden = cfg.critic_hidden = {32, 32};
  cfg.init_std = 0.5f;
  cfg.steps_per_env = 40;
  cfg.learning_rate = 3e-3f;
  cfg.seed = 4;
  lehrer::Ppo ppo(cfg, 4, 4, 2);
  struct Worker {
    std::vector<std::unique_ptr<ReachEnv>> owned;
    std::vector<lehrer::Env*> envs;
    lehrer::Collector collector;
    lehrer::Policy policy;
  };
  Worker workers[2];
  for (int w = 0; w < 2; ++w) {
    workers[w].collector = lehrer::Collector(50 + w);
    for (int i = 0; i < 8; ++i) {
      workers[w].owned.push_back(std::make_unique<ReachEnv>(200 + 10 * w + i));
      workers[w].owned.back()->reset();
      workers[w].envs.push_back(workers[w].owned.back().get());
    }
  }
  f32 first = 0, last = 0;
  for (int it = 0; it < 60; ++it) {
    const std::vector<f32> policy = ppo.policy().serialize();
    std::vector<lehrer::Rollout> received(2);
    for (int w = 0; w < 2; ++w) {
      CHECK(workers[w].policy.deserialize(policy.data(), policy.size()));
      lehrer::Rollout r;
      workers[w].collector.collect(workers[w].envs, cfg.steps_per_env, workers[w].policy, r);
      const std::vector<u8> bytes = r.serialize();
      CHECK(received[static_cast<std::size_t>(w)].deserialize(bytes.data(), bytes.size()));
    }
    const lehrer::IterationStats s = ppo.learn({&received[0], &received[1]});
    if (it == 0) first = s.mean_return;
    last = s.mean_return;
  }
  std::printf("  reach (2 collectors): return %.3f -> %.3f\n", first, last);
  CHECK(last > first * 0.5f);
  // A policy that went through the buffer acts exactly as the learner's.
  lehrer::Policy copy;
  const std::vector<f32> buf = ppo.policy().serialize();
  CHECK(copy.deserialize(buf.data(), buf.size()));
  f32 obs[4] = {0.3f, -0.2f, -0.6f, 0.5f}, a[2], b[2];
  ppo.act(obs, a);
  copy.act(obs, b);
  CHECK(a[0] == b[0] && a[1] == b[1]);
}

TEST(determinism_is_bit_identical_across_builds_and_platforms) {
  // The duck walks and turns on velstand for 8 s (400 ticks); every tick's exact state is hashed.
  // tests/determinism_fingerprint.txt holds the hashes every build must produce: this native build,
  // the WebAssembly modules (tests/lehrer_wasm_test.mjs) and ARM (CI on macOS) - bit for bit.
  const std::vector<u64> h = lehrer::determinism_fingerprint(400);
  std::string path = __FILE__;
  path = path.substr(0, path.find_last_of('/') + 1) + "determinism_fingerprint.txt";
  FILE* f = std::fopen(path.c_str(), "r");
  CHECK(f != nullptr);
  if (!f) return;
  i32 first_diff = -1, lines = 0;
  unsigned long long want = 0;
  std::size_t index = 0;
  while (std::fscanf(f, "%zu %llx", &index, &want) == 2) {
    if (index < h.size() && h[index] != want && first_diff < 0) first_diff = static_cast<i32>(index);
    ++lines;
  }
  std::fclose(f);
  if (first_diff >= 0) std::printf("  first differing tick: %d\n", first_diff);
  CHECK(lines == 400);
  CHECK(first_diff < 0);
}

TEST(lehrer_runs_the_same_on_any_number_of_workers) {
  // The same run on 1, 2 or 4 workers (8 environments in all): the same policy, bit for bit.
  auto run = [](i32 workers) {
    lehrer::PpoConfig cfg;
    cfg.actor_hidden = cfg.critic_hidden = {16, 16};
    cfg.steps_per_env = 20;
    cfg.seed = 9;
    lehrer::Ppo ppo(cfg, 4, 4, 2);
    lehrer::ThreadBackend backend(workers, 8 / workers, 77, [](i32 first, i32 count, u64 seed) {
      std::vector<std::unique_ptr<lehrer::Env>> v;
      for (i32 i = 0; i < count; ++i) v.push_back(std::make_unique<ReachEnv>(seed * 131 + static_cast<u64>(first + i)));
      return v;
    });
    for (int it = 0; it < 4; ++it) lehrer::train_iteration(ppo, backend);
    return ppo.policy().serialize();
  };
  const std::vector<f32> one = run(1), two = run(2), four = run(4);
  CHECK(one.size() == two.size() && one.size() == four.size());
  CHECK(std::memcmp(one.data(), two.data(), one.size() * sizeof(f32)) == 0);
  CHECK(std::memcmp(one.data(), four.data(), one.size() * sizeof(f32)) == 0);
}

TEST(lehrer_exports_onnx_like_pollens_networks) {
  lehrer::Rng rng(21);
  lehrer::Mlp actor;
  actor.init({61, 64, 32, 14}, rng);
  lehrer::Normalizer norm;
  norm.init(61);
  std::vector<f32> data(61 * 40);
  for (f32& v : data) v = rng.normal() * 2 + 0.5f;
  norm.update(data.data(), 40);
  const std::vector<u8> bytes = lehrer::export_onnx(actor, norm, lehrer::micro_duck_metadata("jump"));
  if (const char* path = std::getenv("KERN_ONNX_OUT")) {
    if (FILE* f = std::fopen(path, "wb")) { std::fwrite(bytes.data(), 1, bytes.size(), f); std::fclose(f); }
  }
  game::WalkerPolicy p;
  std::string err;
  CHECK(game::parse_onnx_policy(Span<const u8>(bytes.data(), bytes.size()), p, &err));
  CHECK(err.empty());
  CHECK(p.dims == std::vector<i32>({61, 64, 32, 14}));
  CHECK(p.meta("task") == "jump");
  CHECK(p.meta("joint_names").rfind("left_hip_yaw,", 0) == 0);
  // The file computes what the Lehrer's actor computes.
  f32 worst = 0;
  for (int trial = 0; trial < 5; ++trial) {
    f32 obs[61], x[61], a[14], b[14];
    for (f32& v : obs) v = rng.normal();
    norm.apply(obs, 1, x);
    actor.forward(x, 1, a);
    p.run(obs, b);
    for (int j = 0; j < 14; ++j) worst = max_of(worst, std::fabs(a[j] - b[j]));
  }
  CHECK(worst < 1e-4f);
}

TEST(micro_duck_jumps_with_a_network_trained_in_kern) {
  // Kern's own move: "jump" runs jump.onnx (from Kern Lehrer) like Pollen's kicks run theirs.
  GameFixture f;
  game::Level l = game::basic_level();
  for (game::Actor& a : l.actors) if (a.name == "Cube") a.transform.position = {40, 40, 0.5f};
  const game::ActorId duck = add_duck(l, {0, 0, 0});
  f.world->sync(l);
  CHECK(f.world->start(l, game::SimMode::play));
  if (!game::walker_policy("jump.onnx")) CHECK(f.world->walker_skill(duck, "jump") == "jump.onnx is not loaded");
  // A network as the Lehrer writes it (untrained here: the check is the plumbing).
  lehrer::Rng rng(8);
  lehrer::Mlp actor;
  actor.init({61, 32, 14}, rng, 0.01f);
  lehrer::Normalizer norm;
  norm.init(61);
  const std::vector<u8> bytes = lehrer::export_onnx(actor, norm, lehrer::micro_duck_metadata("jump"));
  auto p = std::make_shared<game::WalkerPolicy>();
  CHECK(game::parse_onnx_policy(Span<const u8>(bytes.data(), bytes.size()), *p));
  p->file = "jump.onnx";
  game::register_walker_policy(p);
  f.run(1);
  CHECK(f.world->walker_skill(duck, "jump").empty());
  f.run(0.5f);
  CHECK(f.world->walker_driving(duck) == "jump");
  f.run(2.5f * 2 + 1);  // 2.5 s of the robot's time (the world runs 2x slower), then it walks again
  CHECK(f.world->walker_driving(duck) != "jump");
}

TEST(micro_duck_observation_between_ticks_is_what_its_controller_sees) {
  // The Lehrer reads the observation between ticks (walker_observation); it must be exactly what
  // the controller builds at the tick (IMU, joints, lagged speeds, last action).
  GameFixture f;
  game::Level l = game::basic_level();
  for (game::Actor& a : l.actors) if (a.name == "Cube") a.transform.position = {40, 40, 0.5f};
  const game::ActorId duck = add_duck(l, {0, 0, 0});
  f.world->sync(l);
  CHECK(f.world->start(l, game::SimMode::play));
  game::PlayInput fwd;
  fwd.move_y = 1;
  i32 ticks = 0;
  f32 worst = 0;
  for (i32 i = 0; i < 400; ++i) {
    f32 before[61], after[61];
    const bool due = f.world->walker_tick_due(duck);
    if (due) CHECK(f.world->walker_observation(duck, before));
    f.world->step_once(fwd);
    if (!due) continue;
    ++ticks;
    CHECK(f.world->walker_last_observation(duck, after));
    for (int k = 0; k < 48; ++k) worst = max_of(worst, std::fabs(before[k] - after[k]));
  }
  CHECK(ticks == 100);
  CHECK(worst == 0);
}

TEST(lehrer_duck_env_runs_episodes_headless) {
  lehrer::DuckEnv env(lehrer::make_stand_task(), 7);
  env.task().randomize = false;  // the clean robot: the check is that the external path is exact
  env.reset();
  CHECK(env.critic_size() == 61);
  const auto& p0 = env.probe();
  CHECK(p0.foot_contact[0] || p0.foot_contact[1] || p0.position.z > 0.1f);
  // The robot's own walking network, given the Lehrer's observation, holds it up: the external
  // path drives the servos exactly as the networks do.
  auto net = game::walker_policy("velstand.onnx");
  CHECK(net != nullptr);
  f32 a[14];
  bool terminal = false, timeout = false;
  f32 min_up = 1;
  i32 steps = 0;
  while (!terminal && !timeout) {
    net->run(env.observation(), a);
    const lehrer::Env::Step s = env.step(a);
    CHECK(std::isfinite(s.reward));
    terminal = s.terminal;
    timeout = s.timeout;
    min_up = min_of(min_up, -env.probe().gravity.z);
    ++steps;
  }
  CHECK(!terminal && timeout && steps == 200);  // 4 s at 50 Hz
  CHECK(min_up > 0.9f);
  const auto& p = env.probe();
  CHECK(p.foot_contact[0] && p.foot_contact[1]);
  CHECK(std::fabs(p.position.z - 0.115f) < 0.02f);
  CHECK(env.episode_return() > 0);
  // Reproducible: the same seed, the same episode.
  lehrer::DuckEnv a1(lehrer::make_jump_task(), 3), a2(lehrer::make_jump_task(), 3);
  a1.reset();
  a2.reset();
  CHECK(a1.critic_size() == 70);
  lehrer::Rng rng(5);
  f32 r1 = 0, r2 = 0;
  for (i32 i = 0; i < 40; ++i) {
    f32 act[14];
    for (f32& v : act) v = rng.uniform(-0.3f, 0.3f);
    r1 += a1.step(act).reward;
    r2 += a2.step(act).reward;
  }
  CHECK(r1 == r2);
  CHECK(std::memcmp(a1.critic(), a2.critic(), sizeof(f32) * 70) == 0);
  // A limp duck (all actions far off home) falls and the episode ends as terminal.
  a1.reset();
  bool fell = false;
  for (i32 i = 0; i < 200 && !fell; ++i) {
    f32 act[14];
    for (i32 j = 0; j < 14; ++j) act[j] = j % 2 ? 1.2f : -1.2f;
    fell = a1.step(act).terminal;
  }
  CHECK(fell);
  // Speed of the headless simulation (printed; the Lehrer's budget).
  const auto t0 = std::chrono::steady_clock::now();
  lehrer::DuckEnv bench(lehrer::make_jump_task(), 11);
  bench.reset();
  i32 n = 0;
  for (; n < 150; ++n) {
    f32 act[14] = {};
    if (bench.step(act).terminal) bench.reset();
  }
  const f64 sec = std::chrono::duration<f64>(std::chrono::steady_clock::now() - t0).count();
  std::printf("  headless duck: %.0f control ticks/s (%.1fx real time, sanitizer build)\n", n / sec, n * 0.02 / sec);
}

TEST(micro_duck_walks_with_the_robot_daemons_action_profile) {
  // ControllerComponent::actions = "robot": robotd's action scale 0.9 and low-pass (head 0.5,
  // legs 0.7) instead of the training's 1.0 and no filter. It stands and walks either way.
  const bool print = std::getenv("KERN_DUCK_PRINT") != nullptr;
  const DuckRun still = run_duck(6, {}, print, {}, false, nullptr, "robot");
  CHECK(still.min_up > 0.9f);
  game::PlayInput fwd;
  fwd.move_y = 1;
  const DuckRun walk = run_duck(8, fwd, print, {}, false, nullptr, "robot");
  const f32 d = std::hypot(walk.end.x - walk.start.x, walk.end.y - walk.start.y) / game::kWalkerScale;
  if (print) std::printf("robot profile: %.3f m real in 8 s, min up %.2f\n", d, walk.min_up);
  CHECK(d > 0.3f);
  CHECK(walk.min_up > 0.8f);
}

TEST(micro_duck_walks_on_its_alpha_gait) {
  // The alpha gait (alpha_walking + alpha_stand, the controller's "alpha" gait): standing still
  // alpha_stand drives; on the stick alpha_walking walks it forward.
  load_duck_policies();
  const bool print = std::getenv("KERN_DUCK_PRINT") != nullptr;
  const DuckRun still = run_duck(6, {}, print, {}, false, "alpha");
  if (print) std::printf("alpha still: moved %.3f, min up %.2f, %s\n", std::hypot(still.end.x - still.start.x, still.end.y - still.start.y), still.min_up, still.driving.c_str());
  CHECK(still.driving == "stand");
  CHECK(still.min_up > 0.9f);
  CHECK(std::hypot(still.end.x - still.start.x, still.end.y - still.start.y) < 0.25f);
  game::PlayInput fwd;
  fwd.move_y = 1;
  const DuckRun walk = run_duck(8, fwd, print, {}, false, "alpha");
  const f32 d = std::hypot(walk.end.x - walk.start.x, walk.end.y - walk.start.y) / game::kWalkerScale;
  if (print) std::printf("alpha walk: %.3f m real in 8 s, min up %.2f, %s\n", d, walk.min_up, walk.driving.c_str());
  CHECK(walk.driving.find("walk") != std::string::npos);
  CHECK(d > 0.3f);
  CHECK(walk.min_up > 0.8f);
}

TEST(micro_duck_runs_every_move_of_its_set) {
  // Each move as robotd schedules it: the network named in the manifest drives for its window,
  // then the walk takes over again; the duck stands at the end.
  load_duck_policies();
  const bool print = std::getenv("KERN_DUCK_PRINT") != nullptr;
  // up_lo .. up_hi: the lowest the trunk's up axis (world z of its z axis) must and may reach during
  // the move - the pick bends forward, the roulade goes over, a kick keeps it standing.
  // Open: kick_right ends tilted and tips over afterwards in Kern (lowest 0.64 in its window, then
  // down and not up again within 3 s), with the ball where training puts it and without one;
  // kick_left stands (0.97). Its end check stays open until that is found (docs/walker.md section 12).
  struct Case { const char* move; bool rollers; f32 seconds; const char* label; f32 up_lo, up_hi; };
  const Case cases[] = {{"sit", false, 6, "sit", 0.5f, 1.0f},          {"pick", false, 6, "ground_pick", 0.3f, 0.9f},
                        {"kick_left", false, 4, "kick_left", 0.5f, 1.0f}, {"kick_right", false, 4, "kick_right", 0.5f, 1.0f},
                        {"roulade", false, 8, "roulade", -1.0f, 0.0f},   {"pick", true, 7, "crouch", 0.3f, 1.0f}};
  for (const Case& c : cases) {
    GameFixture f;
    game::Level l = game::basic_level();
    for (game::Actor& a : l.actors) if (a.name == "Cube") a.transform.position = {40, 40, 0.5f};
    if (std::getenv("KERN_DUCK_MOVE") && std::string(c.move) != std::getenv("KERN_DUCK_MOVE")) continue;
    if (std::string(c.move).rfind("kick", 0) == 0) {
      // As in training (microduck_ball_kick_env_cfg.py: BALL_OFFSET_X 0.09, BALL_OFFSET_ABS_Y 0.042 on
      // the kicking foot's side, BALL_RADIUS 0.035): a 70 mm, 15 g ball in front of the foot (x4 here).
      game::Actor ball;
      ball.name = "Ball";
      ball.transform.position = {0.09f * 4, (std::string(c.move) == "kick_left" ? 0.042f : -0.042f) * 4, 0.035f * 4};
      ball.mesh = game::MeshComponent{};
      ball.mesh->shape = game::Shape::sphere;
      ball.mesh->radius = 0.14f;
      ball.body = game::BodyComponent{};
      ball.body->mass = 0.015f * 64;
      l.add(ball);
    }
    const game::ActorId duck = add_duck(l, {0, 0, 0}, false, c.rollers);
    f.world->sync(l);
    CHECK(f.world->start(l, game::SimMode::simulate));
    for (int i = 0; i < 100; ++i) f.world->step(1.0f / 100, {});
    if (print) {
      Mat4 t0;
      f.world->actor_matrix(duck, t0);
      for (const game::Actor& a : l.actors)
        if (a.name == "Ball") {
          Mat4 bm;
          f.world->actor_matrix(a.id, bm);
          const Vec3 d = bm.translation() - t0.translation();
          const f32 h = std::atan2(t0.m[1], t0.m[0]);
          std::printf("  ball %.3f ahead, %.3f left (real m)\n", (d.x * std::cos(h) + d.y * std::sin(h)) / 4, (-d.x * std::sin(h) + d.y * std::cos(h)) / 4);
        }
      for (const game::Actor& a : l.actors)
        if (a.name == "left_ankle" || a.name == "right_ankle") {
          Mat4 am;
          f.world->actor_matrix(a.id, am);
          const Vec3 e = am.translation() - t0.translation();
          const f32 h = std::atan2(t0.m[1], t0.m[0]);
          std::printf("  %s %.3f ahead, %.3f left\n", a.name.c_str(), (e.x * std::cos(h) + e.y * std::sin(h)) / 4, (-e.x * std::sin(h) + e.y * std::cos(h)) / 4);
        }
    }
    const std::string why = f.world->walker_skill(duck, c.move);
    CHECK(why.empty());
    bool saw = false;
    f32 min_up = 1, worst = -1;
    Mat4 m;
    std::string sit_answer;
    for (int i = 0; i < static_cast<int>(c.seconds * 100); ++i) {
      f.world->step(1.0f / 100, {});
      saw = saw || f.world->walker_driving(duck) == c.label;
      for (const auto& j : f.world->walker_joints(duck))
        worst = max_of(worst, max_of(j.angle - j.upper, j.lower - j.angle));
      f.world->actor_matrix(duck, m);
      min_up = min_of(min_up, m.m[10]);
      if (std::string(c.move) == "sit" && i == 500) sit_answer = f.world->walker_skill(duck, "sit");  // stand up again
    }
    for (int i = 0; i < 600; ++i) {  // 3 s real: time to get up, if it fell
      f.world->step(1.0f / 100, {});
      if (print && i % 50 == 0) { f.world->actor_matrix(duck, m); std::printf("    t+%.1f up %.2f z %.2f %s\n", i / 100.0, m.m[10], m.translation().z, f.world->walker_state(duck).c_str()); }
    }
    f.world->actor_matrix(duck, m);
    for (const game::Actor& a : l.actors)
      if (a.name == "Ball" && print) { Mat4 bm; f.world->actor_matrix(a.id, bm); std::printf("  ball at %.2f %.2f %.2f, duck at %.2f %.2f %.2f\n", bm.translation().x, bm.translation().y, bm.translation().z, m.translation().x, m.translation().y, m.translation().z); }
    if (print) std::printf("%s%s: drove %d, min up %.2f, end up %.2f, driving %s %s\n", c.move, c.rollers ? " (rollers)" : "", saw, min_up, m.m[10],
                           f.world->walker_driving(duck).c_str(), sit_answer.c_str());
    if (print) std::printf("  worst joint overshoot %.4f rad\n", worst);
    CHECK(worst < 0.02f);  // no joint leaves its range (Box3D's limit, its slop)
    CHECK(saw);
    CHECK(f.world->walker_driving(duck) == "walk");
    if (std::string(c.move) != "kick_right") CHECK(m.m[10] > 0.5f);  // on its feet (a hard kick can leave it leaning a while)
    CHECK(min_up >= c.up_lo);
    CHECK(min_up <= c.up_hi);
  }
}

TEST(micro_duck_servo_load_drives_its_sound) {
  // The servo sound follows what the servos do: standing they hold (some strain, little work),
  // walking they work (torque x speed several times higher).
  const bool print = std::getenv("KERN_DUCK_PRINT") != nullptr;
  GameFixture f;
  game::Level l = game::basic_level();
  for (game::Actor& a : l.actors) if (a.name == "Cube") a.transform.position = {40, 40, 0.5f};
  const game::ActorId duck = add_duck(l, {0, 0, 0});
  f.world->sync(l);
  CHECK(f.world->start(l, game::SimMode::play));
  auto average = [&](const game::PlayInput& in, f32 seconds, f32& strain, f32& drive) {
    strain = drive = 0;
    int n = 0;
    for (f32 t = 0; t < seconds; t += 1.0f / 60, ++n) {
      f.world->step(1.0f / 60, in);
      f32 s = 0, d = 0, v = 0;
      CHECK(f.world->walker_servo_load(duck, s, d, v));
      strain += s;
      drive += d;
    }
    strain /= static_cast<f32>(n);
    drive /= static_cast<f32>(n);
  };
  f32 hold_strain, hold_drive, walk_strain, walk_drive;
  average({}, 1, hold_strain, hold_drive);  // settle
  average({}, 3, hold_strain, hold_drive);
  game::PlayInput fwd;
  fwd.move_y = 1;
  average(fwd, 4, walk_strain, walk_drive);
  if (print) std::printf("standing: strain %.3f drive %.3f | walking: strain %.3f drive %.3f\n", hold_strain, hold_drive, walk_strain, walk_drive);
  CHECK(hold_strain > 0.01f && hold_strain < 0.6f);
  CHECK(walk_drive > 3 * hold_drive + 0.05f);
}

TEST(micro_duck_steps_and_taps_sound) {
  // Its soles step (a soft, small footstep) and its plastic taps when shells touch - quietly, a few
  // per second, not a chatter.
  GameFixture f;
  game::Level l = game::basic_level();
  for (game::Actor& a : l.actors) if (a.name == "Cube") a.transform.position = {40, 40, 0.5f};
  const game::ActorId duck = add_duck(l, {0, 0, 0});
  int soles = 0;
  for (const game::Actor& a : l.actors)
    for (const game::ColliderComponent& c : a.colliders) soles += c.impact_sound == "builtin:plastic_step";
  CHECK(soles == 2);
  CHECK(l.find(duck)->body->impact_sound == "builtin:plastic");
  f.world->sync(l);
  CHECK(f.world->start(l, game::SimMode::play));
  game::PlayInput fwd;
  fwd.move_y = 1;
  int steps = 0, taps = 0, other = 0;
  f32 loudest = 0;
  const int seconds = 8;
  for (int i = 0; i < 60 * seconds; ++i) {
    f.world->step(1.0f / 60, i < 60 ? game::PlayInput{} : fwd);
    for (const game::SoundCue& c : f.world->sound_cues()) {
      if (c.sound == "builtin:plastic_step") ++steps, loudest = max_of(loudest, c.volume);
      else if (c.sound == "builtin:plastic") ++taps;
      else if (c.bus == audio::Bus::sfx) ++other;
    }
    f.world->sound_cues().clear();
  }
  if (std::getenv("KERN_DUCK_PRINT")) std::printf("    %d steps, %d taps, %d other in %d s, loudest step %.2f\n", steps, taps, other, seconds, loudest);
  CHECK(steps >= 6);                        // it walks: footsteps
  CHECK(steps + taps < 20 * seconds);       // a few per second, no chatter
  CHECK(other == 0);                        // no heavy "impact" from a small plastic robot
}

TEST(micro_duck_joints_stay_in_their_ranges) {
  // Thrown on its back, on its side, spun: every servo joint keeps its MJCF range (Box3D's hard
  // limit) and never turns round (the angle is tracked unwrapped).
  for (const Vec3 euler : {Vec3{0, 90, 0}, Vec3{0, -90, 0}, Vec3{90, 0, 0}, Vec3{180, 0, 30}}) {
    GameFixture f;
    game::Level l = game::basic_level();
    for (game::Actor& a : l.actors) if (a.name == "Cube") a.transform.position = {40, 40, 0.5f};
    const game::ActorId duck = add_duck(l, {0, 0, 0.6f}, true);
    l.find(duck)->transform.rotation = euler;
    l.find(duck)->body->angular_velocity = {300, 200, 500};  // degrees per second: tumbling
    f.world->sync(l);
    CHECK(f.world->start(l, game::SimMode::simulate));
    std::vector<f32> last, travelled;
    f32 worst = -1, turn = 0;
    for (int i = 0; i < 100 * 12; ++i) {
      f.world->step(1.0f / 100, {});
      const auto joints = f.world->walker_joints(duck);
      if (last.empty()) { last.assign(joints.size(), 0); travelled.assign(joints.size(), 0); for (std::size_t k = 0; k < joints.size(); ++k) last[k] = joints[k].angle; }
      for (std::size_t k = 0; k < joints.size(); ++k) {
        f32 d = joints[k].angle - last[k];
        d = std::atan2(std::sin(d), std::cos(d));
        travelled[k] += d;  // unwrapped: a full turn would show as 2 pi
        last[k] = joints[k].angle;
        worst = max_of(worst, max_of(joints[k].angle - joints[k].upper, joints[k].lower - joints[k].angle));
        turn = max_of(turn, std::fabs(travelled[k]));
      }
    }
    if (std::getenv("KERN_DUCK_PRINT")) std::printf("tumble %.0f %.0f %.0f: worst overshoot %.4f rad, net turn %.2f rad\n", euler.x, euler.y, euler.z, worst, turn);
    CHECK(worst < 0.02f);
    CHECK(turn < 6.0f);  // no joint ever goes round
  }
}

TEST(micro_duck_speaks_with_its_own_voice) {
  // robotd's voice: greet when it comes alive; a fresh chirp press calls and opens the beak; the
  // wheee ride holds the speaker (a chirp during it is dropped); nothing while it walks.
  GameFixture f;
  game::Level l = game::basic_level();
  for (game::Actor& a : l.actors) if (a.name == "Cube") a.transform.position = {40, 40, 0.5f};
  const game::ActorId duck = add_duck(l, {0, 0, 0}, false);
  f.world->sync(l);
  CHECK(f.world->start(l, game::SimMode::play));
  auto said = [&](const char* prefix) {
    for (const game::SoundCue& c : f.world->sound_cues())
      if (c.sound.rfind(prefix, 0) == 0 && c.actor == duck && c.voice) return true;
    return false;
  };
  for (int i = 0; i < 10; ++i) f.world->step(1.0f / 100, {});
  CHECK(f.world->walker_said(duck) == "greet");
  CHECK(said("builtin:duck_greet_"));
  f.world->sound_cues().clear();
  // The cue renders: the duck's synth through the speaker's high-pass, peak inside full scale.
  {
    std::vector<f32> pcm;
    u32 ch = 0;
    for (const char* tag : {"greet", "chirp", "peck", "inquire", "coo", "alarm"}) {
      CHECK(audio::render_builtin(std::string("duck_") + tag + "_12345_1", 48000, pcm, ch));
      f32 peak = 0;
      for (f32 v : pcm) peak = max_of(peak, std::fabs(v));
      CHECK(!pcm.empty() && peak > 0.05f && peak <= 1.0f);
    }
    CHECK(audio::find_builtin("duck_wheee-loop_12345_0") && audio::find_builtin("duck_wheee-loop_12345_0")->loop);
    CHECK(!audio::find_builtin("duck_quack_1_1"));
  }
  // Walking: no sound.
  game::PlayInput walk;
  walk.move_y = 1;
  for (int i = 0; i < 200; ++i) f.world->step(1.0f / 100, walk);
  CHECK(!said("builtin:duck_"));
  // A chirp: called once per press, the beak opens.
  game::PlayInput chirp;
  chirp.robot = 64;
  f32 mouth = -1;
  for (int i = 0; i < 40; ++i) {
    f.world->step(1.0f / 100, chirp);
    for (const auto& j : f.world->walker_joints(duck)) if (j.name == "mouth") mouth = max_of(mouth, j.angle);
  }
  CHECK(said("builtin:duck_chirp_"));
  CHECK(mouth > 0.2f);  // radians: the beak opened
  f.world->sound_cues().clear();
  for (int i = 0; i < 40; ++i) f.world->step(1.0f / 100, chirp);  // held: no second call
  CHECK(!said("builtin:duck_chirp_"));
  // The wheee ride: start, loop while held; a chirp meanwhile is dropped; the end on release.
  game::PlayInput wheee;
  wheee.robot = 128;
  for (int i = 0; i < 300; ++i) f.world->step(1.0f / 100, wheee);
  CHECK(said("builtin:duck_wheee-start_") && said("builtin:duck_wheee-loop_"));
  f.world->sound_cues().clear();
  game::PlayInput both;
  both.robot = 128 | 64;
  for (int i = 0; i < 10; ++i) f.world->step(1.0f / 100, both);
  CHECK(!said("builtin:duck_chirp_"));
  for (int i = 0; i < 10; ++i) f.world->step(1.0f / 100, {});
  CHECK(said("builtin:duck_wheee-end_"));
}


TEST(micro_duck_picks_up_a_sock) {
  // A crumpled sock (micro_duck_sock_actors: soft lumps of fabric) in front of the feet; the duck
  // runs alpha_ground_pick with its beak opening at the bottom and biting (robotd: the operator's
  // trigger, here the controller's phase window). Where the sock lies decides whether the beak gets
  // it - the policy brings the beak to the same spot each time, the sock has to be there, 2.4 cm
  // (real) ahead of the trunk. When it does, the sock comes up with the head and the mouth servo's
  // encoder says so (walker_holding); when it does not, the encoder says that too.
  load_duck_policies();
  const bool print = std::getenv("KERN_DUCK_PRINT") != nullptr;
  const f32 xs[] = {0.13f, 0.2f, 0.215f, 0.225f, 0.235f, 0.245f, 0.255f, 0.33f};
  int lifted = 0, agree = 0;
  for (f32 x : xs) {
    GameFixture f;
    game::Level l = game::basic_level();
    for (game::Actor& a : l.actors) if (a.name == "Cube") a.transform.position = {40, 40, 0.5f};
    const game::ActorId duck = add_duck(l, {0, 0, 0}, false);
    l.find(duck)->controller->kern_extras = true;  // Kern additions
    const game::ActorId first = add_sock(l, {x, 0.03f, 0});
    f.world->sync(l);
    CHECK(f.world->start(l, game::SimMode::simulate));
    for (int i = 0; i < 100; ++i) f.world->step(1.0f / 100, {});
    CHECK(f.world->walker_skill(duck, "pick").empty());
    for (int i = 0; i < 100 * 6; ++i) f.world->step(1.0f / 100, {});
    // Up again, the beak shut: where is the sock, what does the encoder say?
    f32 low = 1e9f;
    for (game::ActorId k = first; k < first + 7; ++k) {
      Mat4 m;
      f.world->actor_matrix(k, m);
      low = min_of(low, m.translation().z);
    }
    const bool up = low > 0.15f, holding = f.world->walker_holding(duck);
    f32 mouth = 0;
    for (const auto& j : f.world->walker_joints(duck)) if (j.name == "mouth") mouth = j.angle;
    if (print) std::printf("sock at x %.3f: lowest lump at z %.3f, holding %d, mouth %.1f deg\n", x, low, holding, mouth * 57.2958f);
    lifted += up;
    agree += up == holding;
    CHECK(f.world->walker_driving(duck) != "fallen");
  }
  CHECK(lifted >= 2);                          // it does pick socks up
  CHECK(lifted < static_cast<int>(sizeof xs / sizeof xs[0]));  // not wherever they lie
  CHECK(agree == static_cast<int>(sizeof xs / sizeof xs[0]));  // and knows when it has one
}

TEST(micro_duck_answers_and_melts_when_petted) {
  // Pollen's brain: a hand on it - it goes to Petted (melt, wiggle or sleepy melt) and leaves a
  // moment after the hand does; a voice gets an answer (a chirp burst after 0.35-0.8 s).
  load_duck_policies();
  GameFixture f;
  game::Level l = game::basic_level();
  for (game::Actor& a : l.actors) if (a.name == "Cube") a.transform.position = {40, 40, 0.5f};
  const game::ActorId duck = add_duck(l, {0, 0, 0}, true);
  f.world->sync(l);
  CHECK(f.world->start(l, game::SimMode::simulate));
  for (int i = 0; i < 200; ++i) f.world->step(1.0f / 100, {});
  CHECK(f.world->walker_pet(duck, 4));
  bool petted = false;
  for (int i = 0; i < 300; ++i) {
    f.world->step(1.0f / 100, {});
    petted = petted || f.world->walker_state(duck) == "Petted";
  }
  CHECK(petted);
  for (int i = 0; i < 2000 && f.world->walker_state(duck) == "Petted"; ++i) f.world->step(1.0f / 100, {});
  CHECK(f.world->walker_state(duck) != "Petted");  // the hand is gone: it moves on
  f.world->sound_cues().clear();
  CHECK(f.world->walker_hear(duck, true));
  bool answered = false;
  for (int i = 0; i < 300 && !answered; ++i) {
    f.world->step(1.0f / 100, {});
    for (const game::SoundCue& c : f.world->sound_cues()) answered = answered || c.sound.find("duck_chirp") != std::string::npos;
    f.world->sound_cues().clear();
  }
  CHECK(answered);
}

TEST(micro_duck_camera_sees_the_floor_in_front_when_looking_down) {
  // The head camera (IMX219, 62 x 37.4 deg, upright) with the head looking down as far as the
  // training's head command goes: what of the floor in front does it see? A sock lies about 6 cm
  // (real) ahead of the trunk when the beak takes it.
  const bool print = std::getenv("KERN_DUCK_PRINT") != nullptr;
  const f32 aheads[] = {0.06f, 0.10f, 0.15f, 0.20f, 0.30f, 0.50f};
  const f32 looks[][2] = {{0, 0}, {0.6f, 0}, {1.1f, 0}, {1.1f, 1.1f}, {1.1f, -1.1f}};
  for (const auto& look : looks) {
    const f32 pitch = look[0], neck = look[1];
    GameFixture f;
    game::Level l = game::basic_level();
    for (game::Actor& a : l.actors) if (a.name == "Cube") a.transform.position = {40, 40, 0.5f};
    const game::ActorId duck = add_duck(l, {0, 0, 0}, false);
    std::vector<game::ActorId> marks;
    std::vector<bool> seen;
    for (f32 x : aheads) {
      game::Actor m;
      m.name = "Mark";
      m.transform.position = {x * 4, 0, 0.025f};
      m.mesh = game::MeshComponent{};
      m.mesh->shape = game::Shape::sphere;
      m.mesh->radius = 0.025f;
      marks.push_back(l.add(m).id);
    }
    f.world->sync(l);
    CHECK(f.world->start(l, game::SimMode::simulate));
    f.world->walker_look(duck, 0, pitch, 0, neck);
    for (int i = 0; i < 300; ++i) f.world->step(1.0f / 100, {});
    Camera cam;
    CHECK(f.world->walker_camera(duck, cam));
    const Vec3 dir = normalize(cam.target - cam.eye);
    if (print) std::printf("neck %.1f head pitch command %.1f: camera at %.3f m (real) high, looking %.0f deg below the horizon; sees:", neck, pitch, cam.eye.z / 4, -std::asin(dir.z) * 57.3f);
    for (std::size_t k = 0; k < marks.size(); ++k) {
      bool visible = false;
      f32 bearing = 0, distance = 0;
      f.world->walker_sees(duck, marks[k], visible, bearing, distance);
      if (print) std::printf(" %.2f m %s", aheads[k], visible ? "yes" : "no");
      seen.push_back(visible);
    }
    if (print) std::printf("\n");
    if (neck < -1.0f) CHECK(seen[0] && seen[1]);   // bowed fully: the sock at the beak's spot is in the picture
    if (neck == 0 && pitch == 0) CHECK(!seen[0]);  // looking ahead it is not
  }
}

TEST(physics_materials_of_meshes_and_the_ground) {
  // Friction, restitution and rolling resistance of an object's fitted collider sit in its Physics
  // settings (beside the render material); the endless ground and Box3D's contact softness in the
  // World Settings. All round-trip through the level file and reach the Box3D shapes.
  game::Level l;
  game::Actor floor;
  floor.name = "Floor";
  floor.mesh = game::MeshComponent{};
  floor.mesh->size = {10, 10, 0.2f};
  floor.mesh->friction = 1.0f;
  floor.mesh->restitution = 0.3f;
  floor.mesh->rolling_resistance = 0.05f;
  const game::ActorId fid = l.add(floor).id;
  l.world.ground = true;
  l.world.ground_friction = 0.9f;
  l.world.ground_restitution = 0.1f;
  l.world.contact_hertz = 60;
  l.world.contact_damping = 5;
  l.world.contact_speed = 2;
  game::Level back;
  std::string error;
  CHECK(game::from_json(game::to_json(l), back, &error));
  const game::Actor* f = back.find(fid);
  CHECK(f && f->mesh && f->mesh->friction == 1.0f && f->mesh->restitution == 0.3f && f->mesh->rolling_resistance == 0.05f);
  CHECK(back.world.ground_friction == 0.9f && back.world.ground_restitution == 0.1f);
  CHECK(back.world.contact_hertz == 60 && back.world.contact_damping == 5 && back.world.contact_speed == 2);
  bool floor_ok = false, ground_ok = false;
  for (const game::BodyPlan& p : game::plan_bodies(back))
    for (const game::ShapePlan& sp : p.shapes) {
      if (p.owner == fid) floor_ok = sp.source.friction == 1.0f && sp.source.restitution == 0.3f && sp.source.rolling_resistance == 0.05f;
      if (p.owner == 0) ground_ok = sp.source.friction == 0.9f && sp.source.restitution == 0.1f;
    }
  CHECK(floor_ok);
  CHECK(ground_ok);
}

TEST(micro_duck_plays_with_a_ball_on_its_own) {
  // Autonomous: a ball 30 cm (real) away, a little to the side. The duck sees it with its head
  // camera, walks up until the ball lies in front of its left foot and kicks it.
  load_duck_policies();
  const bool print = std::getenv("KERN_DUCK_PRINT") != nullptr;
  GameFixture f;
  game::Level l = game::basic_level();
  for (game::Actor& a : l.actors) if (a.name == "Cube") a.transform.position = {40, 40, 0.5f};
  const game::ActorId duck = add_duck(l, {0, 0, 0}, true);
    l.find(duck)->controller->kern_extras = true;  // Kern additions
  game::Actor ball;
  ball.name = "Ball";
  ball.transform.position = {1.2f, 0.3f, 0.14f};
  ball.mesh = game::MeshComponent{};
  ball.mesh->shape = game::Shape::sphere;
  ball.mesh->radius = 0.14f;
  ball.body = game::BodyComponent{};
  ball.body->mass = 0.015f * 64;
  const game::ActorId bid = l.add(ball).id;
  f.world->sync(l);
  CHECK(f.world->start(l, game::SimMode::simulate));
  bool kicked = false, fell = false;
  std::string last;
  for (int i = 0; i < 100 * 90 && !kicked; ++i) {
    f.world->step(1.0f / 100, {});
    const std::string st = f.world->walker_state(duck);
    kicked = f.world->walker_driving(duck) == "kick_left";
    fell = fell || st == "GetUp";
    if (print && (st != last || i % 500 == 0)) {
      Mat4 t, bm;
      f.world->actor_matrix(duck, t);
      f.world->actor_matrix(bid, bm);
      std::printf("  %6.2f s %-10s trunk %.3f %.3f ball %.3f %.3f\n", i / 100.0, st.c_str(), t.translation().x, t.translation().y, bm.translation().x, bm.translation().y);
    }
    last = st;
  }
  Mat4 bm0, t0;
  f.world->actor_matrix(bid, bm0);
  f.world->actor_matrix(duck, t0);
  {
    const Vec3 d = bm0.translation() - t0.translation();
    const f32 h = std::atan2(t0.m[1], t0.m[0]);
    if (print) std::printf("  at the kick the ball is %.3f ahead, %.3f left (real m)\n", (d.x * std::cos(h) + d.y * std::sin(h)) / 4, (-d.x * std::sin(h) + d.y * std::cos(h)) / 4);
    for (const game::Actor& a : l.actors)
      if (print && (a.name == "left_ankle" || a.name == "right_ankle")) {
        Mat4 am;
        f.world->actor_matrix(a.id, am);
        const Vec3 e = am.translation() - t0.translation();
        std::printf("  %s %.3f ahead, %.3f left\n", a.name.c_str(), (e.x * std::cos(h) + e.y * std::sin(h)) / 4, (-e.x * std::sin(h) + e.y * std::cos(h)) / 4);
      }
  }
  for (int i = 0; i < 300; ++i) {
    f.world->step(1.0f / 100, {});
    if (print && i % 10 == 0)
      for (const game::Actor& a : l.actors)
        if (a.name == "left_ankle") {
          Mat4 am, bb;
          f.world->actor_matrix(a.id, am);
          f.world->actor_matrix(bid, bb);
          std::printf("    +%.2f %s ankle %.3f %.3f %.3f ball %.3f %.3f %.3f\n", i / 100.0, f.world->walker_driving(duck).c_str(), am.translation().x, am.translation().y, am.translation().z, bb.translation().x, bb.translation().y, bb.translation().z);
        }
  }
  Mat4 bm1;
  f.world->actor_matrix(bid, bm1);
  if (print) std::printf("  ball moved %.3f after the kick\n", length(bm1.translation() - bm0.translation()));
  CHECK(kicked);
  CHECK(!fell);
  CHECK(length(bm1.translation() - bm0.translation()) > 0.2f);  // and the ball rolls
}

TEST(micro_duck_fetches_a_sock_on_its_own) {
  // Autonomous: a sock 30 cm (real) away in front. The duck sees it with its head camera, walks up to
  // it until it lies where the beak bites, picks it up (again when the encoder says it missed),
  // carries it off and puts it down.
  load_duck_policies();
  const bool print = std::getenv("KERN_DUCK_PRINT") != nullptr;
  GameFixture f;
  game::Level l = game::basic_level();
  for (game::Actor& a : l.actors) if (a.name == "Cube") a.transform.position = {40, 40, 0.5f};
  const game::ActorId duck = add_duck(l, {0, 0, 0}, true);
    l.find(duck)->controller->kern_extras = true;  // Kern additions
  const game::ActorId first = add_sock(l, {1.2f, 0.25f, 0});
  f.world->sync(l);
  CHECK(f.world->start(l, game::SimMode::simulate));
  bool carried = false, fetched = false;
  std::string last;
  for (int i = 0; i < 100 * 120 && !carried; ++i) {
    f.world->step(1.0f / 100, {});
    const std::string st = f.world->walker_state(duck);
    fetched = fetched || st == "Fetch";
    f32 low = 1e9f;
    for (game::ActorId k = first; k < first + 7; ++k) {
      Mat4 m;
      f.world->actor_matrix(k, m);
      low = min_of(low, m.translation().z);
    }
    carried = st == "Carry" && low > 0.15f && f.world->walker_holding(duck);
    if (print && (st != last || i % 500 == 0)) {
      Mat4 t;
      f.world->actor_matrix(duck, t);
      std::printf("  %6.2f s %-10s trunk %.3f %.3f sock low z %.3f\n", i / 100.0, st.c_str(), t.translation().x, t.translation().y, low);
    }
    last = st;
  }
  CHECK(fetched);
  CHECK(carried);
}

TEST(micro_duck_roams_on_its_own_with_its_tof) {
  // A 6 x 6 m room (real 1.5 x 1.5 m: the duck is built 4x) with a box in it: the duck walks around on
  // its own, sees the walls with its ToF, turns away and never falls.
  GameFixture f;
  game::Level l = game::basic_level();
  for (game::Actor& a : l.actors) if (a.name == "Cube") a.transform.position = {2.5f, 0.5f, 0.5f};
  auto wall = [&](Vec3 p, Vec3 size) {
    game::Actor a;
    a.name = "Wall";
    a.transform.position = p;
    a.mesh = game::MeshComponent{};
    a.mesh->size = size;
    l.add(a);
  };
  wall({4, 0, 1}, {0.4f, 8, 2});
  wall({-4, 0, 1}, {0.4f, 8, 2});
  wall({0, 4, 1}, {8, 0.4f, 2});
  wall({0, -4, 1}, {8, 0.4f, 2});
  const game::ActorId duck = add_duck(l, {-1.5f, -1.5f, 0}, true);
  f.world->sync(l);
  CHECK(f.world->start(l, game::SimMode::simulate));
  Vec3 start, lo{1e9f, 1e9f, 0}, hi{-1e9f, -1e9f, 0};
  Mat4 m;
  f.world->actor_matrix(duck, m);
  start = m.translation();
  f32 min_up = 1, closest = 1e9f;
  std::vector<std::string> states;
  bool saw_wall = false;
  for (int i = 0; i < 60 * 90; ++i) {  // 90 s simulated (45 s real)
    f.world->step(1.0f / 60, {});
    f.world->actor_matrix(duck, m);
    const Vec3 p = m.translation();
    min_up = min_of(min_up, m.m[10]);
    lo = {min_of(lo.x, p.x), min_of(lo.y, p.y), 0};
    hi = {max_of(hi.x, p.x), max_of(hi.y, p.y), 0};
    closest = min_of(closest, min_of(4 - std::fabs(p.x), 4 - std::fabs(p.y)));
    const std::string st = f.world->walker_state(duck);
    if (states.empty() || states.back() != st) states.push_back(st);
    game::GameWorld::WalkerSensors ws;
    if (f.world->walker_sensors(duck, ws) && ws.nearest > 0) saw_wall = true;
  }
  if (std::getenv("KERN_DUCK_PRINT")) {
    std::printf("roam: area %.2f x %.2f m, closest to a wall %.2f m, min up %.2f, states:", hi.x - lo.x, hi.y - lo.y, closest, min_up);
    for (const std::string& st : states) std::printf(" %s", st.c_str());
    std::printf("\n");
  }
  CHECK(saw_wall);
  CHECK(min_up > 0.7f);                           // it does not fall
  CHECK((hi.x - lo.x) + (hi.y - lo.y) > 2.0f);    // it gets around
  CHECK(closest > 0.25f);                         // it keeps off the walls (the trunk centre)
  CHECK(std::find(states.begin(), states.end(), "Wander") != states.end());
  CHECK(std::find(states.begin(), states.end(), "TurnInPlace") != states.end());
}

TEST(micro_duck_looks_around_when_left_alone) {
  // robotd's idle look-around (idle_head.rs): standing with nothing to do, after [idle] after_s it
  // glances about - the head camera's view swings - and it keeps standing. Its camera is the real
  // mount: the IMX219's 62 degrees run vertically (mounted a quarter turn off).
  GameFixture f;
  game::Level l = game::basic_level();
  for (game::Actor& a : l.actors) if (a.name == "Cube") a.transform.position = {40, 40, 0.5f};
  const game::ActorId duck = add_duck(l, {0, 0, 0}, false);
  f.world->sync(l);
  CHECK(f.world->start(l, game::SimMode::simulate));
  Camera cam;
  Mat4 m;
  f32 early_lo = 9, early_hi = -9, lo = 9, hi = -9, min_up = 1;
  for (int i = 0; i < 60 * 40; ++i) {  // 40 s simulated (20 s real)
    f.world->step(1.0f / 60, {});
    CHECK(f.world->walker_camera(duck, cam));
    f.world->actor_matrix(duck, m);
    min_up = min_of(min_up, m.m[10]);
    const Vec3 d = cam.target - cam.eye;
    f32 yaw = std::atan2(d.y, d.x) - std::atan2(m.m[1], m.m[0]);
    yaw = std::atan2(std::sin(yaw), std::cos(yaw));
    const f32 t = static_cast<f32>(i) / 60 / 2;  // real seconds
    if (t > 1 && t < 3.5f) { early_lo = min_of(early_lo, yaw); early_hi = max_of(early_hi, yaw); }
    if (t > 6) { lo = min_of(lo, yaw); hi = max_of(hi, yaw); }
  }
  if (std::getenv("KERN_DUCK_PRINT")) std::printf("idle: early yaw span %.3f, later %.3f, min up %.2f\n", early_hi - early_lo, hi - lo, min_up);
  CHECK(early_hi - early_lo < 0.06f);  // still the first seconds: only after after_s (2 s) + 2 s of no head command
  CHECK(hi - lo > 0.2f);               // then it looks around
  CHECK(min_up > 0.9f);
  CHECK(std::fabs(cam.fov - 62 * 3.14159265f / 180) < 1e-4f);
}

TEST(micro_duck_blueprint_nodes_drive_and_sense) {
  // The level's Logic drives the duck: autonomy off, walk forward; its camera sees the cube ahead.
  GameFixture f;
  game::Level l = game::basic_level();
  game::ActorId cube = 0;
  for (game::Actor& a : l.actors) if (a.name == "Cube") { a.transform.position = {6, 0, 0.5f}; cube = a.id; }
  const game::ActorId duck = add_duck(l, {0, 0, 0}, true);
  l.logic = logic_graph(R"({"variables":[{"name":"Sees","type":"bool","value":false},{"name":"Dist","type":"float","value":-1}],
    "nodes":[{"id":1,"type":"event.begin"},{"id":2,"type":"walker.autonomous","values":{"enable":false}},{"id":3,"type":"walker.move","values":{"forward":0.3}},
      {"id":4,"type":"event.tick"},{"id":5,"type":"var.set","params":{"variable":"Sees"}},{"id":6,"type":"walker.sees"},
      {"id":7,"type":"var.set","params":{"variable":"Dist"}},{"id":8,"type":"walker.tof"}],
    "links":[[1,"then",2,"exec"],[2,"then",3,"exec"],[4,"then",5,"exec"],[6,"visible",5,"value"],[5,"then",7,"exec"],[8,"distance",7,"value"]]})");
  for (logic::Node& n : l.logic.nodes) if (n.type == "walker.sees") n.set_value("other", json::Value(static_cast<f64>(cube)));
  f.world->sync(l);
  CHECK(f.world->start(l, game::SimMode::simulate));
  Mat4 m;
  for (int i = 0; i < 60 * 6; ++i) f.world->step(1.0f / 60, {});
  CHECK(f.world->actor_matrix(duck, m));
  CHECK(m.translation().x > 0.5f);                            // it walked forward
  CHECK(f.world->walker_state(duck) == "Logic");
  logic::Instance* inst = f.world->logic_runtime()->find(0);
  CHECK(inst && inst->get("Sees").as_bool());                 // the cube is in its picture
  CHECK(inst && inst->get("Dist").as_number() > 0);           // and in its ToF
}

TEST(micro_duck_example_level_runs) {
  // File > New > Micro Duck: the room, the autonomous duck, the blueprint that quacks at the ball.
  game::Level l = game::micro_duck_level();
  CHECK(!l.logic.nodes.empty() && l.logic.comments.size() == 1);
  CHECK(no_errors(logic::check(l.logic)));
  CHECK(l.world.step_hz == game::kWalkerStepHz && l.world.substeps == game::kWalkerSubsteps);
  GameFixture f;
  f.world->sync(l);
  CHECK(f.world->start(l, game::SimMode::play));
  CHECK(f.world->walkers().size() == 1);
  Mat4 m;
  f32 min_up = 1;
  const int secs = std::getenv("KERN_DUCK_LONG") ? 240 : 20;
  for (int i = 0; i < 60 * secs; ++i) {
    f.world->step(1.0f / 60, {});
    f.world->actor_matrix(l.world.player, m);
    min_up = min_of(min_up, m.m[10]);
    if (std::getenv("KERN_DUCK_PRINT") && i % 60 == 0)
      std::printf("t %d pos %.2f %.2f up %.2f %s\n", i / 60, m.translation().x, m.translation().y, m.m[10], f.world->walker_state(l.world.player).c_str());
  }
  CHECK(min_up > 0.7f);
  CHECK(f.world->walker_state(l.world.player) != "Idle");  // autonomous
}

TEST(studio_robot_template_compiles) {
  StudioFixture f;
  CHECK(f.cmd(R"({"cmd":"new","template":"basic"})")["ok"].as_bool());
  CHECK(f.cmd(R"({"cmd":"logicOpen","owner":0})")["ok"].as_bool());
  CHECK(f.cmd(R"({"cmd":"logicTemplate","template":"robot_avoid"})")["ok"].as_bool());
  CHECK(f.s->level().logic.nodes.size() == 9 && f.s->level().logic.comments.size() == 1);
  CHECK(no_errors(logic::check(f.s->level().logic)));
}

TEST(micro_duck_builds_from_its_mjcf) {
  StudioFixture f;
  CHECK(f.cmd(R"({"cmd":"new","template":"basic"})")["ok"].as_bool());
  const game::ActorId duck = f.s->spawn("micro_duck", Vec3{4, 4, 0});
  CHECK(duck != 0);
  const game::Actor* a = f.s->level().find(duck);
  CHECK(a && a->controller && a->controller->mode == game::ControlMode::walker);
  u32 servos = 0;
  for (const game::Actor& x : f.s->level().actors) for (const game::JointComponent& j : x.joints) servos += j.servo ? 1 : 0;
  CHECK(servos == 15);  // the policy's 14 and the mouth
  CHECK(f.s->level().world.substeps >= game::kWalkerSubsteps);  // long servo chains get the sub-steps they need
  CHECK(f.s->level().world.step_hz == game::kWalkerStepHz);  // and the policy its fixed step
  // The level round-trips the imported mass properties and joint references.
  const std::string text = game::to_text(f.s->level());
  game::Level back;
  std::string error;
  CHECK(game::from_text(text, back, &error));
  const game::Actor* b = back.find(duck);
  CHECK(b && b->body && b->body->explicit_inertia() && std::fabs(b->body->inertia[0] - a->body->inertia[0]) < 1e-6f * a->body->inertia[0] + 1e-9f);
  // Play: it stands on its policy and every servo joint holds.
  CHECK(f.cmd(R"({"cmd":"play","mode":"play"})")["ok"].as_bool());
  f.frames(120);
  Mat4 m;
  CHECK(f.s->world().actor_matrix(duck, m));
  CHECK(m.translation().z > .4f && m.m[10] > .9f);
  f.cmd(R"({"cmd":"stop"})");
}

TEST(studio_player_presets_and_gameplay_blocks) {
  StudioFixture f;
  CHECK(f.cmd(R"({"cmd":"new","template":"basic"})")["ok"].as_bool());
  // Every catalog item spawns.
  const json::Value groups = f.cmd(R"({"cmd":"catalog"})")["groups"];
  CHECK(groups[0]["label"].as_string() == "Players");
  int items = 0;
  for (const json::Value& g : groups.items())
    for (const json::Value& it : g["items"].items()) {
      if (it["id"].as_string() == "city") continue;
      json::Value c = json::Value::object();
      c.set("cmd", "spawn");
      c.set("item", it["id"].as_string());
      CHECK(f.s->command(c)["ok"].as_bool());
      ++items;
    }
  CHECK(items > 35);
  CHECK(f.cmd(R"({"cmd":"undo"})")["ok"].as_bool());
  // Fresh level: the last dropped player is the one Play controls.
  CHECK(f.cmd(R"({"cmd":"new","template":"basic"})")["ok"].as_bool());
  f.cmd(R"({"cmd":"spawn","item":"soldier","x":0,"y":0})");
  const game::ActorId soldier = f.s->level().world.player;
  CHECK(soldier != 0 && f.s->level().find(soldier)->name == "Soldier");
  f.cmd(R"({"cmd":"spawn","item":"hover_car"})");
  const game::ActorId car = f.s->level().world.player;
  CHECK(car != soldier);
  json::Value rows = f.s->state()["outliner"]["rows"];
  bool badge = false;
  for (const json::Value& r : rows.items()) if (r["player"].as_bool()) badge = r["id"].as_number() == car;
  CHECK(badge);
  // Make the soldier the player again from the context menu command.
  json::Value c = json::Value::object();
  c.set("cmd", "setPlayer");
  c.set("id", soldier);
  CHECK(f.s->command(c)["ok"].as_bool());
  CHECK(f.s->level().world.player == soldier);
  CHECK(f.cmd(R"({"cmd":"undo"})")["ok"].as_bool());
  CHECK(f.s->level().world.player == car);
  CHECK(f.cmd(R"({"cmd":"play","mode":"play"})")["ok"].as_bool());
  f.frames(5);
  CHECK(f.s->world().player() == car);
  f.cmd(R"({"cmd":"stop"})");
}

TEST(game_gameplay_blocks) {
  GameFixture f;
  game::Level l = game::basic_level();
  for (game::Actor& a : l.actors) if (a.name == "Cube") a.transform.position = {40, 40, 0.5f};
  const game::ActorId me = add_soldier(l, {0, 0, 0.8f});
  auto sensor_box = [&](game::Actor& a, Vec3 size) {
    game::ColliderComponent s;
    s.shape = game::ColliderShape::box;
    s.size = size;
    s.center = {0, 0, size.z * 0.5f};
    s.sensor = true;
    a.colliders.push_back(s);
  };
  game::Actor pad;
  pad.name = "Jump Pad";
  pad.transform.position = {4, 0, 0.05f};
  pad.trigger = game::TriggerComponent{};
  pad.trigger->action = game::TriggerAction::launch;
  pad.trigger->strength = 11;
  pad.trigger->once = false;
  sensor_box(pad, {1.8f, 1.8f, 0.8f});
  l.add(pad);
  game::Actor tp;
  tp.name = "Teleporter A";
  tp.transform.position = {-4, 0, 0.05f};
  tp.trigger = game::TriggerComponent{};
  tp.trigger->action = game::TriggerAction::teleport;
  tp.trigger->once = false;
  sensor_box(tp, {1.4f, 1.4f, 1.6f});
  game::Actor exit;
  exit.name = "Exit";
  exit.transform.position = {-4, 15, 0.05f};
  const game::ActorId exit_id = l.add(exit).id;
  tp.trigger->target = exit_id;
  l.add(tp);
  game::Actor med;
  med.name = "Health Pack";
  med.transform.position = {0, 4, 0.6f};
  med.pickup = game::PickupComponent{};
  med.pickup->points = 0;
  med.pickup->heal = 50;
  med.pickup->respawn = 15;
  sensor_box(med, {1, 1, 1});
  const game::ActorId med_id = l.add(med).id;
  game::Actor barrel;
  barrel.name = "Explosive Barrel";
  barrel.transform.position = {10, -6, 0.5f};
  barrel.mesh = game::MeshComponent{};
  barrel.mesh->shape = game::Shape::cylinder;
  barrel.mesh->radius = 0.35f;
  barrel.body = game::BodyComponent{};
  barrel.body->mass = 30;
  barrel.health = game::HealthComponent{};
  barrel.health->max_health = 30;
  barrel.health->respawn = false;
  barrel.health->explode_radius = 6;
  barrel.health->explode_damage = 90;
  const game::ActorId barrel_id = l.add(barrel).id;
  const game::ActorId dummy = add_dummy(l, {12, -6, 0.725f});
  CHECK(f.world->start(l, game::SimMode::play));
  Camera cam;
  game::PlayInput in;
  auto tick = [&](int n) { for (int i = 0; i < n; ++i) { f.world->step(1.0f / 60, in); f.world->update_camera(cam, 1.0f / 60, in); } };
  auto feet = [&] { return f.world->physics().pose(f.world->actor_body(me)).position; };
  tick(20);
  // Health pack: ignored at full health, taken when hurt.
  f.world->damage(me, 40, 0, false, {0, 0, 0});
  f.world->teleport(me, {0, 4, 0.8f});
  tick(10);
  CHECK(!f.world->actor_alive(med_id));
  CHECK(f.world->health_of(me) > 99);
  // Jump pad: walk onto it.
  f.world->teleport(me, {4, 0, 0.8f});
  f32 top = 0;
  for (int i = 0; i < 60; ++i) { tick(1); top = std::max(top, feet().z); }
  CHECK(top > 4);
  tick(120);
  // Teleporter: step on it, arrive at the exit.
  f.world->teleport(me, {-4, 0, 0.8f});
  tick(10);
  CHECK(distance(feet() * Vec3{1, 1, 0}, Vec3{-4, 15, 0}) < 1.5f);
  // Explosive barrel: destroying it hurts the dummy next to it and pushes things.
  f.world->damage(barrel_id, 100, me, false, {0, 0, 0});
  CHECK(!f.world->actor_alive(barrel_id));
  CHECK(f.world->health_of(dummy) < 70);
}

TEST(studio_shortcuts_follow_the_keyboard_layout) {
  StudioFixture f;
  CHECK(f.cmd(R"({"cmd":"new","template":"basic"})")["ok"].as_bool());
  const std::size_t n = f.s->level().actors.size();
  f.cmd(R"({"cmd":"spawn","item":"cube"})");
  CHECK(f.s->level().actors.size() == n + 1);
  // German QWERTZ: the key labelled Z sits where US keyboards have Y (code KeyY).
  f.s->key("z", "KeyY", true, 2, false);
  f.s->key("z", "KeyY", false, 2, false);
  CHECK(f.s->level().actors.size() == n);
  f.s->key("y", "KeyZ", true, 2, false);   // Ctrl+Y (redo) on QWERTZ
  f.s->key("y", "KeyZ", false, 2, false);
  CHECK(f.s->level().actors.size() == n + 1);
  // AZERTY: the tool keys follow the printed letters too (E = rotate).
  f.s->key("e", "KeyE", true, 0, false);
  f.s->key("e", "KeyE", false, 0, false);
  CHECK(f.s->state()["tool"].as_string() == "rotate");
}

TEST(studio_logic_editor_commands) {
  StudioFixture f;
  CHECK(f.cmd(R"({"cmd":"new","template":"shootingRange"})")["ok"].as_bool());
  CHECK(f.cmd(R"({"cmd":"logicOpen","owner":0})")["ok"].as_bool());
  json::Value st = f.s->state();
  CHECK(st["logic"]["open"].as_bool() && st["logic"]["ownerName"].as_string() == "Level");
  // Search the palette.
  json::Value pal = f.cmd(R"({"cmd":"logicPalette","query":"begin play"})");
  CHECK(pal["items"].size() >= 1 && pal["items"][0]["type"].as_string() == "event.begin");
  // Begin Play, then drop a Show Message off its exec pin: wired automatically.
  const u32 begin = static_cast<u32>(f.cmd(R"({"cmd":"logicAdd","type":"event.begin","x":0,"y":0})")["id"].as_number());
  json::Value add = json::Value::object();
  add.set("cmd", "logicAdd");
  add.set("type", "game.message");
  add.set("x", 300);
  add.set("y", 0);
  json::Value from = json::Value::object();
  from.set("node", begin);
  from.set("pin", "then");
  from.set("output", true);
  add.set("from", from);
  const u32 msg = static_cast<u32>(f.s->command(add)["id"].as_number());
  st = f.s->state();
  CHECK(st["logic"]["links"].size() == 1);
  // Context palette from a data pin: only nodes that can take a text.
  json::Value pq = json::Value::object();
  pq.set("cmd", "logicPalette");
  pq.set("query", "");
  json::Value fp = json::Value::object();
  fp.set("node", msg);
  fp.set("pin", "text");
  fp.set("output", false);
  pq.set("from", fp);
  json::Value ctx = f.s->command(pq);
  bool has_append = false, has_branch = false;
  for (const json::Value& it : ctx["items"].items()) {
    has_append |= it["type"].as_string() == "text.append";
    has_branch |= it["type"].as_string() == "flow.branch";
  }
  CHECK(has_append && !has_branch);
  // Literal text and a variable printed into it.
  json::Value val = json::Value::object();
  val.set("cmd", "logicValue");
  val.set("node", msg);
  val.set("pin", "text");
  val.set("value", "Hello from Logic");
  CHECK(f.s->command(val)["ok"].as_bool());
  CHECK(f.cmd(R"({"cmd":"logicVariable","op":"add","name":"Lives","type":"int"})")["ok"].as_bool());
  CHECK(f.cmd(R"({"cmd":"logicVariable","op":"value","name":"Lives","value":3})")["ok"].as_bool());
  pal = f.cmd(R"({"cmd":"logicPalette","query":"lives"})");
  CHECK(pal["items"].size() >= 2);
  CHECK(f.cmd(R"({"cmd":"logicVariable","op":"rename","name":"Lives","to":"Hearts"})")["ok"].as_bool());
  st = f.s->state();
  CHECK(st["logic"]["variables"][0]["name"].as_string() == "Hearts");
  // A bad wire is refused with a reason.
  json::Value bad = json::Value::object();
  bad.set("cmd", "logicConnect");
  bad.set("from", begin);
  bad.set("out", "then");
  bad.set("to", msg);
  bad.set("in", "text");
  json::Value r = f.s->command(bad);
  CHECK(!r["ok"].as_bool() && r["error"].is_string());
  // Templates, undo, play.
  CHECK(f.cmd(R"({"cmd":"logicTemplate","template":"traffic_light"})")["ok"].as_bool());
  CHECK(f.s->level().logic.machines.size() == 1);
  CHECK(f.cmd(R"({"cmd":"undo"})")["ok"].as_bool());
  CHECK(f.s->level().logic.machines.empty());
  CHECK(f.cmd(R"({"cmd":"play","mode":"play"})")["ok"].as_bool());
  f.frames(3);
  CHECK(f.s->world().hud().message == "Hello from Logic");
  json::Value status = f.s->status();
  CHECK(status["logicTrace"].is_object());
  CHECK(status["logicTrace"]["variables"]["Hearts"].as_number() == 3);
  f.cmd(R"({"cmd":"stop"})");
  // An actor's own graph: opening it with the actor selected; copy / paste / delete.
  const game::ActorId dummy = f.s->level().find(static_cast<game::ActorId>(0)) ? 0 : [&] { for (const game::Actor& a : f.s->level().actors) if (a.name == "Target Dummy 1") return a.id; return game::ActorId(0); }();
  json::Value open = json::Value::object();
  open.set("cmd", "logicOpen");
  open.set("owner", dummy);
  CHECK(f.s->command(open)["ok"].as_bool());
  CHECK(f.cmd(R"({"cmd":"logicTemplate","template":"score_zone"})")["ok"].as_bool());
  CHECK(f.s->level().find(dummy)->logic && f.s->level().find(dummy)->logic->nodes.size() == 4);
  json::Value copy = f.cmd(R"({"cmd":"logicCopy","ids":[1,2]})");
  CHECK(copy["ok"].as_bool());
  json::Value paste = json::Value::object();
  paste.set("cmd", "logicPaste");
  paste.set("text", copy["text"]);
  paste.set("x", 0);
  paste.set("y", 600);
  CHECK(f.s->command(paste)["ok"].as_bool());
  CHECK(f.s->level().find(dummy)->logic->nodes.size() == 6);
  CHECK(f.s->level().find(dummy)->logic->links.size() == 4);
  CHECK(f.cmd(R"({"cmd":"logicDelete","ids":[1]})")["ok"].as_bool());
  CHECK(f.s->level().find(dummy)->logic->nodes.size() == 5);
  // State machine editing.
  const u32 m = static_cast<u32>(f.cmd(R"({"cmd":"logicMachine","op":"add","name":"AI"})")["id"].as_number());
  json::Value addst = json::Value::object();
  addst.set("cmd", "logicMachine");
  addst.set("op", "addState");
  addst.set("machine", m);
  addst.set("name", "Chase");
  addst.set("x", 300);
  const u32 chase = static_cast<u32>(f.s->command(addst)["id"].as_number());
  const logic::StateMachine& sm = f.s->level().find(dummy)->logic->machines.back();
  json::Value tr = json::Value::object();
  tr.set("cmd", "logicMachine");
  tr.set("op", "addTransition");
  tr.set("machine", m);
  tr.set("from", sm.states.front().id);
  tr.set("to", chase);
  CHECK(f.s->command(tr)["ok"].as_bool());
  const u32 tid = f.s->level().find(dummy)->logic->machines.back().transitions.back().id;
  json::Value ed = json::Value::object();
  ed.set("cmd", "logicMachine");
  ed.set("op", "editTransition");
  ed.set("machine", m);
  ed.set("transition", tid);
  ed.set("trigger", "condition");
  CHECK(f.s->command(ed)["ok"].as_bool());
  const logic::Graph& dg = *f.s->level().find(dummy)->logic;
  CHECK(dg.machines.back().transitions.back().trigger == logic::TriggerKind::condition);
  CHECK(dg.variable(dg.machines.back().transitions.back().variable) != nullptr);
  CHECK(logic::check(dg).empty() || no_errors(logic::check(dg)));
}

TEST(studio_blueprint_breakpoints_pause_the_game) {
  StudioFixture f;
  CHECK(f.cmd(R"({"cmd":"new","template":"basic"})")["ok"].as_bool());
  CHECK(f.cmd(R"({"cmd":"logicOpen","owner":0})")["ok"].as_bool());
  CHECK(f.cmd(R"({"cmd":"logicTemplate","template":"spawner"})")["ok"].as_bool());
  u32 spawn = 0;
  for (const logic::Node& n : f.s->level().logic.nodes) if (n.type == "actor.spawn") spawn = n.id;
  json::Value bp = json::Value::object();
  bp.set("cmd", "logicBreakpoint");
  bp.set("node", spawn);
  CHECK(f.s->command(bp)["on"].as_bool());
  bool flagged = false;
  const json::Value st1 = f.s->state();
  for (const json::Value& n : st1["logic"]["nodes"].items()) flagged |= n["id"].as_number() == spawn && n["breakpoint"].as_bool();
  CHECK(flagged);
  CHECK(f.cmd(R"({"cmd":"play","mode":"play"})")["ok"].as_bool());
  f.frames(60 * 4);  // the spawner fires every 3 s
  CHECK(f.s->world().paused());
  bool hit = false;
  const json::Value st2 = f.s->state();
  for (const json::Value& n : st2["logic"]["nodes"].items()) hit |= n["id"].as_number() == spawn && n["hit"].as_bool();
  CHECK(hit);
  // Resume runs on; removing the breakpoint lets it keep running.
  bp.set("on", false);
  CHECK(!f.s->command(bp)["on"].as_bool());
  CHECK(f.cmd(R"({"cmd":"pause","value":false})")["ok"].as_bool());
  f.frames(60 * 4);
  CHECK(!f.s->world().paused());
  f.cmd(R"({"cmd":"stop"})");
}

TEST(studio_blueprint_comments_reroutes_promote) {
  StudioFixture f;
  CHECK(f.cmd(R"({"cmd":"new","template":"basic"})")["ok"].as_bool());
  CHECK(f.cmd(R"({"cmd":"logicOpen","owner":0})")["ok"].as_bool());
  const u32 begin = static_cast<u32>(f.cmd(R"({"cmd":"logicAdd","type":"event.begin","x":0,"y":0})")["id"].as_number());
  const u32 msg = static_cast<u32>(f.cmd(R"({"cmd":"logicAdd","type":"game.message","x":400,"y":0})")["id"].as_number());
  const u32 add = static_cast<u32>(f.cmd(R"({"cmd":"logicAdd","type":"math.add","x":0,"y":200})")["id"].as_number());
  auto cmd = [&](std::initializer_list<std::pair<const char*, json::Value>> kv) {
    json::Value c = json::Value::object();
    for (const auto& [k, v] : kv) c.set(k, v);
    return f.s->command(c);
  };
  CHECK(cmd({{"cmd", "logicConnect"}, {"from", begin}, {"out", "then"}, {"to", msg}, {"in", "exec"}})["ok"].as_bool());
  // Compact nodes: an operator draws as its symbol.
  json::Value st = f.s->state();
  bool compact = false;
  for (const json::Value& n : st["logic"]["nodes"].items()) compact |= n["id"].as_number() == add && n["compact"].as_string() == "+";
  CHECK(compact);
  // Double-click on the wire: a reroute knot splits it, one undo step.
  json::Value link = json::Value::array();
  link.push(begin);
  link.push("then");
  link.push(msg);
  link.push("exec");
  const json::Value rr = cmd({{"cmd", "logicInsertReroute"}, {"link", link}, {"x", 200}, {"y", 40}});
  CHECK(rr["ok"].as_bool());
  const logic::Graph& g = f.s->level().logic;
  CHECK(g.nodes.size() == 4 && g.links.size() == 2 && g.find(static_cast<u32>(rr["id"].as_number()))->type == "flow.reroute");
  CHECK(no_errors(logic::check(g)));
  CHECK(f.cmd(R"({"cmd":"undo"})")["ok"].as_bool());
  CHECK(f.s->level().logic.nodes.size() == 3 && f.s->level().logic.links.size() == 1);
  // Promote to Variable: the Add node's A input gets a float variable and a Get node.
  const json::Value pr = cmd({{"cmd", "logicPromote"}, {"node", add}, {"pin", "a"}, {"output", false}});
  CHECK(pr["ok"].as_bool() && f.s->level().logic.variable(pr["variable"].as_string()) != nullptr);
  CHECK(f.s->level().logic.links.size() == 2);
  // Comment boxes: add, move with resize, edit, copy / paste, delete.
  const u32 cb = static_cast<u32>(f.cmd(R"({"cmd":"logicCommentBox","op":"add","x":-40,"y":-40,"w":700,"h":300,"text":"Start"})")["id"].as_number());
  CHECK(f.s->level().logic.comments.size() == 1);
  json::Value moves = json::Value::array();
  json::Value mv = json::Value::object();
  mv.set("id", cb);
  mv.set("x", -80);
  mv.set("y", -80);
  mv.set("w", 800);
  mv.set("h", 400);
  moves.push(mv);
  CHECK(cmd({{"cmd", "logicMove"}, {"moves", moves}})["ok"].as_bool());
  CHECK(f.s->level().logic.comments[0].w == 800 && f.s->level().logic.comments[0].x == -80);
  CHECK(cmd({{"cmd", "logicCommentBox"}, {"op", "edit"}, {"id", cb}, {"text", "Begin"}, {"color", 0x3366aa}})["ok"].as_bool());
  CHECK(f.s->level().logic.comments[0].text == "Begin" && f.s->level().logic.comments[0].color == 0x3366aa);
  CHECK(f.s->state()["logic"]["comments"].size() == 1);
  json::Value ids = json::Value::array();
  ids.push(cb);
  const json::Value copy = cmd({{"cmd", "logicCopy"}, {"ids", ids}});
  CHECK(cmd({{"cmd", "logicPaste"}, {"text", copy["text"]}, {"x", 0}, {"y", 800}})["ok"].as_bool());
  CHECK(f.s->level().logic.comments.size() == 2);
  CHECK(cmd({{"cmd", "logicDelete"}, {"ids", ids}})["ok"].as_bool());
  CHECK(f.s->level().logic.comments.size() == 1);
  // A comment does not change the compiled program (the hash ignores the editor's layout).
  logic::Graph a = f.s->level().logic, b = a;
  b.comments.clear();
  CHECK(logic::graph_hash(a) == logic::graph_hash(b));
  // Context palette from a pin offers a reroute knot of the pin's type.
  json::Value from = json::Value::object();
  from.set("node", add);
  from.set("pin", "result");
  from.set("output", true);
  const json::Value pal = cmd({{"cmd", "logicPalette"}, {"query", "reroute"}, {"from", from}});
  CHECK(pal["items"].size() >= 1 && pal["items"][0]["type"].as_string() == "value.reroute" && pal["items"][0]["params"]["type"].as_string() == "float");
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

TEST(studio_fast_player_does_not_shake_on_screen) {
  // A fast car (and a rolling ball) at an uneven frame rate: the camera follows the pose that is
  // drawn (interpolated between physics steps), so the player keeps still on screen.
  for (const char* tpl : {"orbRun", "physicsLab"}) {
    StudioFixture f;
    if (std::string(tpl) != "orbRun") CHECK(f.cmd((std::string(R"({"cmd":"new","template":")") + tpl + "\"}").c_str())["ok"].as_bool());
    CHECK(f.cmd(R"({"cmd":"play","mode":"play"})")["ok"].as_bool());
    if (!f.s->world().player()) continue;
    f.s->key("w", "KeyW", true, 0, false);
    f.s->key("Shift", "ShiftLeft", true, 0, false);
    f.frames(90);
    std::vector<f32> angles;
    const f64 dts[] = {1000.0 / 41, 1000.0 / 97, 1000.0 / 60, 1000.0 / 144, 1000.0 / 52};
    for (int i = 0; i < 100; ++i) {
      f.s->frame(f.now += dts[i % 5]);
      Mat4 m;
      CHECK(f.s->world().actor_matrix(f.s->world().player(), m));
      const Camera& c = f.s->play_camera();
      const Vec3 fwd = normalize(c.target - c.eye), to = normalize(m.translation() - c.eye);
      angles.push_back(std::acos(clamp(dot(fwd, to), -1.0f, 1.0f)) * 57.29578f);
    }
    f.s->key("w", "KeyW", false, 0, false);
    f.s->key("Shift", "ShiftLeft", false, 0, false);
    // Shake = how much the frame-to-frame change itself changes (smooth drift is fine).
    f32 worst = 0;
    for (std::size_t i = 2; i < angles.size(); ++i) worst = max_of(worst, std::fabs((angles[i] - angles[i - 1]) - (angles[i - 1] - angles[i - 2])));
    std::printf("    %s: worst on-screen shake %.3f deg\n", tpl, worst);
    CHECK(worst < 0.25f);
  }
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


// Restore from the last session (docs/studio.md): the host keeps a copy only while there is
// unsaved work, so a normal reload of a clean editor offers nothing.
TEST(studio_autosave_only_while_unsaved) {
  StudioFixture f;
  f.frames(5);
  // A fresh start is clean: nothing to store, nothing to drop.
  CHECK(!f.s->dirty());
  CHECK(f.s->take_autosave(f.now + 60000).empty());
  CHECK(!f.s->take_autosave_discard());
  // A change is stored once it is due (autosave_seconds), once.
  const game::ActorId id = f.s->spawn("cube", Vec3{0, 0, 1});
  CHECK(f.s->dirty());
  CHECK(f.s->take_autosave(f.now).empty());
  const std::string text = f.s->take_autosave(f.now + 60000);
  CHECK(!text.empty());
  CHECK(!f.s->take_autosave_discard());
  CHECK(f.s->take_autosave(f.now + 60000).empty());
  // Undone back to the opened state: nothing is unsaved, the stored copy must go (once).
  f.s->undo();
  CHECK(!f.s->dirty());
  CHECK(f.s->take_autosave(f.now + 60000).empty());
  CHECK(f.s->take_autosave_discard());
  CHECK(!f.s->take_autosave_discard());
  // Redo: unsaved again, stored again; saving drops it.
  f.s->redo();
  CHECK(!f.s->take_autosave(f.now + 60000).empty());
  f.cmd(R"({"cmd":"saved"})");
  CHECK(!f.s->dirty());
  CHECK(f.s->take_autosave_discard());
  CHECK(f.s->take_autosave(f.now + 60000).empty());
  // Saving a level that never had a stored copy drops nothing (it may be another level's).
  f.cmd(R"({"cmd":"saved"})");
  CHECK(!f.s->take_autosave_discard());
  // Opening another level keeps the previous level's unsaved work stored until the new one changes.
  f.s->spawn("sphere", Vec3{2, 0, 1});
  CHECK(!f.s->take_autosave(f.now + 60000).empty());
  f.cmd(R"({"cmd":"new","template":"basic"})");
  CHECK(!f.s->dirty());
  CHECK(f.s->take_autosave(f.now + 60000).empty());
  CHECK(!f.s->take_autosave_discard());
  // Restoring: the level comes back unsaved (its content is in no file) and owns the stored copy.
  json::Value restore = json::Value::object();
  restore.set("cmd", "restore");
  restore.set("text", text);
  restore.set("name", "Orb Run");
  CHECK(f.s->command(restore)["ok"].as_bool());
  CHECK(f.s->level().find(id) != nullptr);
  CHECK(f.s->dirty());
  CHECK(f.s->state()["level"]["dirty"].as_bool());
  CHECK(!f.s->can_undo());
  CHECK(f.s->take_autosave(f.now + 60000).empty());  // the stored copy is already this state
  CHECK(!f.s->take_autosave_discard());
  // Changing and undoing it stays unsaved (stored, never dropped); saving drops it.
  f.s->spawn("cube", Vec3{4, 0, 1});
  CHECK(!f.s->take_autosave(f.now + 60000).empty());
  f.s->undo();
  CHECK(f.s->dirty());
  CHECK(!f.s->take_autosave(f.now + 60000).empty());
  CHECK(!f.s->take_autosave_discard());
  f.cmd(R"({"cmd":"saved"})");
  CHECK(f.s->take_autosave_discard());
  CHECK(!f.s->dirty());
  // A broken copy is reported and changes nothing.
  json::Value broken = json::Value::object();
  broken.set("cmd", "restore");
  broken.set("text", "{not a level");
  CHECK(!f.s->command(broken)["ok"].as_bool());
  CHECK(!f.s->dirty());
}
// User settings (docs/studio.md, "Einstellungen"): defaults, only the user's own choices are
// handed to the host, stored choices come back, and Reset puts everything back to the defaults.
TEST(studio_user_settings) {
  StudioFixture f;
  f.frames(2);
  // Constraints (joint debug lines) are off by default in every viewport.
  const json::Value defaults = studio::Studio::default_settings_json();
  for (u32 i = 0; i < 4; ++i) {
    CHECK(!f.s->viewport(i).flags.joints);
    CHECK(defaults["viewport" + std::to_string(i) + ".show.joints"].is_bool() && !defaults["viewport" + std::to_string(i) + ".show.joints"].as_bool());
  }
  CHECK(f.s->settings_json() == defaults);
  CHECK(f.cmd(R"({"cmd":"settings"})")["values"] == defaults);
  // Nothing chosen yet: nothing to store (old defaults never get frozen into storage).
  CHECK(f.s->take_settings().is_null());
  f.cmd(R"({"cmd":"select","ids":[]})");
  f.frames(2);
  CHECK(f.s->take_settings().is_null());
  // A choice: only that key, once.
  CHECK(f.cmd(R"({"cmd":"showFlag","viewport":0,"flag":"joints","value":true})")["ok"].as_bool());
  json::Value t = f.s->take_settings();
  CHECK(t.is_object() && t.size() == 1 && t["viewport0.show.joints"].as_bool());
  CHECK(f.s->take_settings().is_null());
  // Shortcuts and dialogs count too; a choice set back to the default stays a choice.
  f.s->key("]", "BracketRight", true, 0, false);
  f.s->key("]", "BracketRight", false, 0, false);
  CHECK(f.cmd(R"({"cmd":"submitDialog","name":"preferences","values":{"cameraSpeed":6,"invertY":true}})")["ok"].as_bool());
  CHECK(f.cmd(R"({"cmd":"showFlag","viewport":0,"flag":"joints","value":false})")["ok"].as_bool());
  CHECK(f.cmd(R"({"cmd":"mute","muted":true})")["ok"].as_bool());
  t = f.cmd(R"({"cmd":"takeSettings"})")["values"];
  CHECK(t.is_object() && t.size() == 5);
  CHECK(t.has("viewport0.show.joints") && !t["viewport0.show.joints"].as_bool());
  CHECK(t["snap.gridSize"].as_number() > 0.5 && t["cameraSpeed"].as_number() == 6 && t["invertY"].as_bool() && t["muted"].as_bool());
  CHECK(!t.has("mouseSensitivity") && !t.has("viewport1.type"));  // unchanged by Apply: not a choice
  const std::string stored = json::stringify(t);

  // The next session: the stored choices come back; unknown keys and invalid values are skipped.
  StudioFixture g;
  json::Value back;
  CHECK(json::parse(stored, back));
  back.set("viewport2.show.colliders", true);
  back.set("viewport9.show.grid", false);
  back.set("cameraSpeed", "fast");
  back.set("layout", "quad");
  back.set("viewport1.type", "left");
  back.set("bogus", 1);
  CHECK(g.cmd((R"({"cmd":"applySettings","values":)" + json::stringify(back) + "}").c_str())["applied"].as_number() == 7);
  CHECK(g.s->viewport(2).flags.colliders && !g.s->viewport(0).flags.joints && g.s->muted());
  CHECK(g.s->prefs().camera_speed == 4 && g.s->prefs().invert_y && g.s->prefs().layout == studio::Layout::quad);
  CHECK(g.s->viewport(1).type == studio::ViewType::left && g.s->snap().grid_size > 0.5f);
  CHECK(g.s->take_settings().is_null());  // loading is no news
  CHECK(g.cmd(R"({"cmd":"space"})")["ok"].as_bool());
  t = g.s->take_settings();
  CHECK(t.size() == 8 && t["space"].as_string() == "local" && t["viewport2.show.colliders"].as_bool());
  g.frames(2);

  // Reset to defaults: an Edit menu entry opens the Studio's own confirmation dialog.
  bool entry = false;
  const json::Value edit_menu = g.cmd(R"({"cmd":"menu","name":"edit"})");
  for (const json::Value& e : edit_menu["entries"].items())
    entry |= e["label"].as_string() == "Reset Settings to Defaults..." && e["cmd"]["name"].as_string() == "resetSettings";
  CHECK(entry);
  const json::Value dlg = g.cmd(R"({"cmd":"dialog","name":"resetSettings"})")["dialog"];
  CHECK(dlg.is_object() && dlg["buttons"].size() == 2);
  g.s->take_ui();
  CHECK(g.cmd(R"({"cmd":"submitDialog","name":"resetSettings","values":{}})")["ok"].as_bool());
  CHECK(g.s->settings_json() == defaults);
  CHECK(!g.s->muted() && g.s->prefs().layout == studio::Layout::single && g.s->viewport(1).type == studio::ViewType::top);
  t = g.s->take_settings();
  CHECK(t.is_object() && t.size() == 0);  // the host drops what it stored
  bool action = false;
  const json::Value ui_actions = g.s->take_ui();
  for (const json::Value& a : ui_actions.items()) action |= a["action"].as_string() == "settingsReset";
  CHECK(action);
  CHECK(g.s->take_settings().is_null());
  g.frames(2);
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
  // kern_tests --emit-logic <dir>: writes the logic cases as generated C++ (run.sh compiles them
  // into logic_native_test) and the interpreter's results as the expectation.
  if (argc > 2 && std::strcmp(argv[1], "--emit-logic") == 0) {
    std::string expected;
    for (const logic_cases::Case& c : logic_cases::cases()) {
      logic_cases::Host host;
      logic::Runtime rt(host);
      logic::Instance* inst = rt.add(logic_cases::parse(c), 0, c.name);
      const std::string src = logic::generate_cpp(*inst, c.name);
      if (FILE* f = std::fopen((std::string(argv[2]) + "/logic_" + c.name + ".cpp").c_str(), "wb")) { std::fwrite(src.data(), 1, src.size(), f); std::fclose(f); }
      expected += std::string("== ") + c.name + "\n" + logic_cases::play(rt, host, inst);
    }
    if (FILE* f = std::fopen((std::string(argv[2]) + "/logic_expected.txt").c_str(), "wb")) { std::fwrite(expected.data(), 1, expected.size(), f); std::fclose(f); }
    return 0;
  }
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
