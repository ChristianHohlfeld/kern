// Kern engine unit tests (host build, see kern/tests/run.sh).
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "gl_stub.h"
#include "kern/kern.h"

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

// ---------------------------------------------------------------------------
// app runtime

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
