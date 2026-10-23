#include "kern/game/geometry.h"

#include <cmath>

#include "kern/game/builtin_meshes.h"
#include "kern/geo/primitives.h"
#include "kern/geo/roads.h"

namespace kern::game {

namespace {

constexpr Vec3 kWhite{1, 1, 1};

/// Quad with UVs in metres (times tiling) along its two edges.
void metric_quad(MeshData& m, Vec3 a, Vec3 b, Vec3 c, Vec3 d, f32 tiling) {
  const Vec3 n = normalize(cross(b - a, d - a));
  const f32 u = length(b - a) * tiling, v = length(d - a) * tiling;
  const u32 base = m.vertex_count();
  m.push(a, n, kWhite, {0, 0});
  m.push(b, n, kWhite, {u, 0});
  m.push(c, n, kWhite, {u, v});
  m.push(d, n, kWhite, {0, v});
  m.indices.insert(m.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
}

void metric_tri(MeshData& m, Vec3 a, Vec3 b, Vec3 c, f32 tiling) {
  const Vec3 n = normalize(cross(b - a, c - a));
  // Planar projection onto the triangle's dominant plane.
  auto uv = [&](Vec3 p) {
    const Vec3 an{std::fabs(n.x), std::fabs(n.y), std::fabs(n.z)};
    if (an.x >= an.y && an.x >= an.z) return Vec2{p.y * tiling, p.z * tiling};
    if (an.y >= an.z) return Vec2{p.x * tiling, p.z * tiling};
    return Vec2{p.x * tiling, p.y * tiling};
  };
  const u32 base = m.vertex_count();
  m.push(a, n, kWhite, uv(a));
  m.push(b, n, kWhite, uv(b));
  m.push(c, n, kWhite, uv(c));
  m.indices.insert(m.indices.end(), {base, base + 1, base + 2});
}

void shift_z(MeshData& m, f32 dz) {
  for (std::size_t i = 2; i < m.positions.size(); i += 3) m.positions[i] += dz;
}

void scale_uv(MeshData& m, f32 s) {
  for (f32& v : m.uvs) v *= s;
}

}  // namespace

MeshData shape_mesh(const MeshComponent& mc) {
  const f32 t = max_of(0.001f, mc.material.tiling);
  const u32 seg = static_cast<u32>(clamp(mc.segments, 3, 128));
  const Vec3 s{max_of(0.001f, mc.size.x), max_of(0.001f, mc.size.y), max_of(0.001f, mc.size.z)};
  const f32 r = max_of(0.001f, mc.radius), h = max_of(0.001f, mc.height);
  MeshData m;
  switch (mc.shape) {
    case Shape::box: {
      const f32 x = s.x * 0.5f, y = s.y * 0.5f, z = s.z * 0.5f;
      metric_quad(m, {x, -y, -z}, {x, y, -z}, {x, y, z}, {x, -y, z}, t);
      metric_quad(m, {-x, y, -z}, {-x, -y, -z}, {-x, -y, z}, {-x, y, z}, t);
      metric_quad(m, {x, y, -z}, {-x, y, -z}, {-x, y, z}, {x, y, z}, t);
      metric_quad(m, {-x, -y, -z}, {x, -y, -z}, {x, -y, z}, {-x, -y, z}, t);
      metric_quad(m, {-x, -y, z}, {x, -y, z}, {x, y, z}, {-x, y, z}, t);
      metric_quad(m, {-x, y, -z}, {x, y, -z}, {x, -y, -z}, {-x, -y, -z}, t);
      break;
    }
    case Shape::plane: {
      const f32 x = s.x * 0.5f, y = s.y * 0.5f;
      metric_quad(m, {-x, -y, 0}, {x, -y, 0}, {x, y, 0}, {-x, y, 0}, t);
      break;
    }
    case Shape::ramp: {
      // Wedge: full height at +X, zero at -X.
      const f32 x = s.x * 0.5f, y = s.y * 0.5f, z = s.z * 0.5f;
      const Vec3 a{-x, -y, -z}, b{x, -y, -z}, c{x, y, -z}, d{-x, y, -z}, e{x, -y, z}, f{x, y, z};
      metric_quad(m, d, c, b, a, t);        // bottom
      metric_quad(m, b, c, f, e, t);        // back (+X)
      metric_quad(m, a, e, f, d, t);        // slope
      metric_tri(m, a, b, e, t);            // side -Y
      metric_tri(m, c, d, f, t);            // side +Y
      break;
    }
    case Shape::sphere:
      m = primitives::sphere(r, seg, max_of<u32>(4, seg * 2 / 3), kWhite);
      scale_uv(m, t);
      break;
    case Shape::cylinder:
      m = primitives::cylinder(r, h, seg, kWhite);
      shift_z(m, -h * 0.5f);
      scale_uv(m, t);
      break;
    case Shape::cone:
      m = primitives::cone(r, h, seg, kWhite);
      shift_z(m, -h * 0.5f);
      scale_uv(m, t);
      break;
    case Shape::capsule:
      m = primitives::capsule(r, max_of(0.0f, h - 2 * r), seg, max_of<u32>(4, seg / 2), kWhite);
      scale_uv(m, t);
      break;
    case Shape::torus:
      m = primitives::torus(r, min_of(h, r * 0.95f), seg, max_of<u32>(6, seg / 2), kWhite);
      scale_uv(m, t);
      break;
    case Shape::mesh: {
      // A built-in mesh, scaled per axis by size (normals by the inverse).
      const BuiltinMesh* b = builtin_mesh(mc.asset);
      if (!b) {  // unknown asset: a small box, so it can be seen and fixed
        MeshComponent box = mc;
        box.shape = Shape::box;
        box.size = {0.1f, 0.1f, 0.1f};
        return shape_mesh(box);
      }
      m = b->mesh;
      const Vec3 k = mc.size;
      for (std::size_t i = 0; i < m.positions.size(); i += 3) {
        m.positions[i] *= k.x; m.positions[i + 1] *= k.y; m.positions[i + 2] *= k.z;
        Vec3 n{m.normals[i] / k.x, m.normals[i + 1] / k.y, m.normals[i + 2] / k.z};
        n = normalize(n);
        m.normals[i] = n.x; m.normals[i + 1] = n.y; m.normals[i + 2] = n.z;
      }
      if (k.x * k.y * k.z < 0)
        for (std::size_t i = 0; i + 2 < m.indices.size(); i += 3) std::swap(m.indices[i + 1], m.indices[i + 2]);
      break;
    }
  }
  return m;
}

Aabb shape_bounds(const MeshComponent& mc) {
  const Vec3 s{max_of(0.001f, mc.size.x), max_of(0.001f, mc.size.y), max_of(0.001f, mc.size.z)};
  const f32 r = max_of(0.001f, mc.radius), h = max_of(0.001f, mc.height);
  Aabb b;
  switch (mc.shape) {
    case Shape::box:
    case Shape::ramp: b.extend(s * -0.5f); b.extend(s * 0.5f); break;
    case Shape::plane: b.extend({-s.x * 0.5f, -s.y * 0.5f, 0}); b.extend({s.x * 0.5f, s.y * 0.5f, 0}); break;
    case Shape::sphere: b.extend({-r, -r, -r}); b.extend({r, r, r}); break;
    case Shape::cylinder:
    case Shape::cone: b.extend({-r, -r, -h * 0.5f}); b.extend({r, r, h * 0.5f}); break;
    case Shape::capsule: { const f32 hz = max_of(h * 0.5f, r); b.extend({-r, -r, -hz}); b.extend({r, r, hz}); break; }
    case Shape::torus: { const f32 m = min_of(h, r * 0.95f); b.extend({-r - m, -r - m, -m}); b.extend({r + m, r + m, m}); break; }
    case Shape::mesh:
      if (const BuiltinMesh* bm = builtin_mesh(mc.asset)) {
        for (int i = 0; i < 8; ++i)
          b.extend({(i & 1 ? bm->hi.x : bm->lo.x) * mc.size.x, (i & 2 ? bm->hi.y : bm->lo.y) * mc.size.y, (i & 4 ? bm->hi.z : bm->lo.z) * mc.size.z});
      } else {
        b.extend({-0.05f, -0.05f, -0.05f});
        b.extend({0.05f, 0.05f, 0.05f});
      }
      break;
  }
  return b;
}

std::vector<Vec3> shape_hull_points(const MeshComponent& mc) {
  std::vector<Vec3> pts;
  const f32 r = max_of(0.001f, mc.radius), h = max_of(0.001f, mc.height);
  const Vec3 s{max_of(0.001f, mc.size.x), max_of(0.001f, mc.size.y), max_of(0.001f, mc.size.z)};
  constexpr int kRing = 16;
  switch (mc.shape) {
    case Shape::cylinder:
      for (int i = 0; i < kRing; ++i) {
        const f32 a = static_cast<f32>(i) / kRing * 2 * kPi;
        pts.push_back({std::cos(a) * r, std::sin(a) * r, -h * 0.5f});
        pts.push_back({std::cos(a) * r, std::sin(a) * r, h * 0.5f});
      }
      break;
    case Shape::cone:
      for (int i = 0; i < kRing; ++i) {
        const f32 a = static_cast<f32>(i) / kRing * 2 * kPi;
        pts.push_back({std::cos(a) * r, std::sin(a) * r, -h * 0.5f});
      }
      pts.push_back({0, 0, h * 0.5f});
      break;
    case Shape::ramp: {
      const f32 x = s.x * 0.5f, y = s.y * 0.5f, z = s.z * 0.5f;
      pts = {{-x, -y, -z}, {x, -y, -z}, {x, y, -z}, {-x, y, -z}, {x, -y, z}, {x, y, z}};
      break;
    }
    case Shape::torus: {
      const f32 m = min_of(h, r * 0.95f);
      for (int i = 0; i < kRing; ++i) {
        const f32 a = static_cast<f32>(i) / kRing * 2 * kPi;
        for (f32 z : {-m, m}) pts.push_back({std::cos(a) * r, std::sin(a) * r, z});
        pts.push_back({std::cos(a) * (r + m), std::sin(a) * (r + m), 0});
      }
      break;
    }
    case Shape::mesh:
      if (const BuiltinMesh* bm = builtin_mesh(mc.asset)) {
        for (const Vec3& p : bm->hull) pts.push_back({p.x * mc.size.x, p.y * mc.size.y, p.z * mc.size.z});
        break;
      }
      [[fallthrough]];
    default: {
      const Aabb b = shape_bounds(mc);
      for (int i = 0; i < 8; ++i) pts.push_back({i & 1 ? b.max.x : b.min.x, i & 2 ? b.max.y : b.min.y, i & 4 ? b.max.z : b.min.z});
      break;
    }
  }
  return pts;
}

ColliderComponent fit_collider(const MeshComponent& mc) {
  ColliderComponent c;
  const f32 r = max_of(0.001f, mc.radius), h = max_of(0.001f, mc.height);
  const Vec3 s{max_of(0.001f, mc.size.x), max_of(0.001f, mc.size.y), max_of(0.001f, mc.size.z)};
  switch (mc.shape) {
    case Shape::box: c.shape = ColliderShape::box; c.size = s; break;
    case Shape::plane: c.shape = ColliderShape::box; c.size = {s.x, s.y, 0.02f}; c.center = {0, 0, -0.01f}; break;
    case Shape::sphere: c.shape = ColliderShape::sphere; c.radius = r; break;
    case Shape::capsule: c.shape = ColliderShape::capsule; c.radius = r; c.length = max_of(0.0f, h - 2 * r); break;
    default: c.shape = ColliderShape::hull; break;
  }
  return c;
}

// ---------------------------------------------------------------------------
// procedural city (Orb Run's generator)

namespace {

struct CityLayout {
  const CityComponent& p;
  CityGeometry& out;
  FootprintStream footprints;
  RoadStream roads;
  Rng rng;
  f32 half;

  void add_box_collider(f32 a0, f32 b0, f32 a1, f32 b1, f32 height) {
    out.colliders.push_back({{(a0 + a1) * 0.5f, (b0 + b1) * 0.5f, height * 0.5f}, {a1 - a0, b1 - b0, height}});
  }
  void add_box(f32 a0, f32 b0, f32 a1, f32 b1, f32 height) {
    const Vec2 ring[4] = {{a0, b0}, {a1, b0}, {a1, b1}, {a0, b1}};
    footprints.begin(rng.next(), 0, height);
    footprints.ring(ring, 4);
    add_box_collider(a0, b0, a1, b1, height);
    ++out.building_count;
  }

  void generate() {
    const int blocks = p.blocks;
    const f32 pitch = p.pitch, street = p.street;
    for (int bx = 0; bx < blocks; ++bx)
      for (int by = 0; by < blocks; ++by) {
        const f32 x0 = -half + bx * pitch + street * 0.5f, y0 = -half + by * pitch + street * 0.5f;
        const f32 size = pitch - street;
        primitives::append(out.plates, primitives::box_on_ground({size, size, 0.18f}, {0.56f, 0.56f, 0.54f}),
                           Mat4::translate({x0 + size * 0.5f, y0 + size * 0.5f, 0}));
        out.colliders.push_back({{x0 + size * 0.5f, y0 + size * 0.5f, 0.09f}, {size, size, 0.18f}});
        const Vec2 center{x0 + size * 0.5f, y0 + size * 0.5f};
        const f32 centrality = 1 - clamp(length(center) / (half * 1.2f), 0.0f, 1.0f);
        const f32 tall = 14 + centrality * centrality * (p.max_height - 14);
        const f32 inset = 3;
        const f32 a0 = x0 + inset, b0 = y0 + inset, a1 = x0 + size - inset, b1 = y0 + size - inset;
        switch (rng.below(4)) {
          case 0: {  // one block-sized building, often with a courtyard
            const Vec2 outer[4] = {{a0, b0}, {a1, b0}, {a1, b1}, {a0, b1}};
            const f32 height = rng.range(10, tall);
            footprints.begin(rng.next(), 0, height);
            footprints.ring(outer, 4);
            ++out.building_count;
            if (rng.unit() < 0.6f) {
              const f32 c = 14;
              const Vec2 court[4] = {{a0 + c, b0 + c}, {a0 + c, b1 - c}, {a1 - c, b1 - c}, {a1 - c, b0 + c}};
              footprints.ring(court, 4);
              // Four wings around the courtyard.
              add_box_collider(a0, b0, a1, b0 + c, height);
              add_box_collider(a0, b1 - c, a1, b1, height);
              add_box_collider(a0, b0 + c, a0 + c, b1 - c, height);
              add_box_collider(a1 - c, b0 + c, a1, b1 - c, height);
            } else {
              add_box_collider(a0, b0, a1, b1, height);
            }
            break;
          }
          case 1: {  // four lots
            const f32 mx = (a0 + a1) * 0.5f, my = (b0 + b1) * 0.5f, g = 2.5f;
            const f32 lots[4][4] = {{a0, b0, mx - g, my - g}, {mx + g, b0, a1, my - g}, {a0, my + g, mx - g, b1}, {mx + g, my + g, a1, b1}};
            for (const auto& l : lots) add_box(l[0], l[1], l[2], l[3], rng.range(8, tall));
            break;
          }
          case 2: {  // L-shaped block
            const f32 w = rng.range(16, 24);
            const Vec2 outer[6] = {{a0, b0}, {a1, b0}, {a1, b0 + w}, {a0 + w, b0 + w}, {a0 + w, b1}, {a0, b1}};
            const f32 height = rng.range(12, tall);
            footprints.begin(rng.next(), 0, height);
            footprints.ring(outer, 6);
            ++out.building_count;
            add_box_collider(a0, b0, a1, b0 + w, height);
            add_box_collider(a0, b0 + w, a0 + w, b1, height);
            add_box(a0 + w + 6, b0 + w + 6, a1, b1, rng.range(8, tall * 0.7f));
            break;
          }
          default: {  // slabs
            const f32 step = (a1 - a0) / 3;
            for (int k = 0; k < 3; ++k) add_box(a0 + k * step + 1.5f, b0, a0 + (k + 1) * step - 1.5f, b1, rng.range(8, tall));
            break;
          }
        }
      }
    for (int k = 0; k <= blocks; ++k) {
      const f32 c = -half + k * pitch;
      const Vec2 h[2] = {{-half - 60, c}, {half + 60, c}}, v[2] = {{c, -half - 60}, {c, half + 60}};
      roads.road(h, 2, street * 0.75f, true, 0, 1);
      roads.road(v, 2, street * 0.75f, true, 0, 1);
    }
  }

  void trees() {
    const int blocks = p.blocks;
    const f32 pitch = p.pitch, street = p.street;
    auto put = [](std::vector<f32>& out, const Mat4& m) { out.insert(out.end(), m.m, m.m + 16); };
    for (int bx = 0; bx < blocks; ++bx)
      for (int by = 0; by < blocks; ++by) {
        const f32 x0 = -half + bx * pitch + street * 0.5f + 1.2f, y0 = -half + by * pitch + street * 0.5f + 1.2f;
        const f32 x1 = x0 + pitch - street - 2.4f, y1 = y0 + pitch - street - 2.4f;
        for (f32 t = 4; t < x1 - x0 - 2; t += 11) {
          const Vec2 spots[4] = {{x0 + t, y0}, {x0 + t, y1}, {x0, y0 + t}, {x1, y0 + t}};
          for (Vec2 s : spots) {
            if (rng.unit() < 0.35f) continue;
            const f32 scale = rng.range(0.8f, 1.3f);
            put(out.trunk_transforms, Mat4::trs({s.x, s.y, 0.18f}, Quat{}, {scale, scale, scale}));
            put(out.crown_transforms,
                Mat4::trs({s.x, s.y, 0.18f + 2.0f * scale}, Quat::axis_angle({0, 0, 1}, rng.range(0, 6)), {scale, scale, scale * rng.range(0.9f, 1.4f)}));
            const f32 g = rng.range(0.75f, 1.1f);
            out.crown_tints.insert(out.crown_tints.end(), {0.55f * g, 0.85f * g, 0.45f * g, 1});
          }
        }
      }
    out.trunk = primitives::cylinder(0.16f, 2.2f, 8, {0.36f, 0.25f, 0.16f});
    out.crown = primitives::cone(1.5f, 4.2f, 10, {0.22f, 0.42f, 0.18f});
  }
};

}  // namespace

bool build_city(const CityComponent& city, CityGeometry& out) {
  if (city.blocks < 1 || city.blocks > 16 || !(city.pitch >= 30) || !(city.street >= 6) || city.street > city.pitch * 0.5f)
    return fail("invalid city parameters");
  out = CityGeometry{};
  CityLayout layout{city, out, {}, {}, Rng(city.seed), city.blocks * city.pitch * 0.5f};
  out.half = layout.half;
  layout.generate();

  BuildingOptions bo;
  bo.chunk_size = 200;
  bo.style.walls = {{0.78, 0.74, 0.68}, {0.62, 0.60, 0.58}, {0.70, 0.52, 0.42}, {0.48, 0.52, 0.57}, {0.84, 0.82, 0.77}, {0.55, 0.40, 0.33}};
  bo.style.roofs = {{0.27, 0.28, 0.30}, {0.22, 0.23, 0.25}, {0.36, 0.30, 0.27}};
  BuildingBuilder buildings;
  if (!buildings.begin(layout.footprints.data(), layout.footprints.size(), bo)) return false;
  while (!buildings.step(1e9)) {}
  for (GeoChunk& chunk : buildings.chunks()) {
    weld(chunk.mesh);
    out.buildings.push_back(std::move(chunk.mesh));
  }
  RoadOptions ro;
  ro.radius = 4000;
  ro.chunk_size = 200;
  ro.palette = {{0.20, 0.21, 0.23}, {0.90, 0.89, 0.86}};
  RoadBuilder road_builder;
  if (!road_builder.begin(layout.roads.data(), layout.roads.size(), ro, HeightSampler{})) return false;
  while (!road_builder.step(1e9)) {}
  for (GeoChunk& chunk : road_builder.chunks()) {
    weld(chunk.mesh);
    out.roads.push_back(std::move(chunk.mesh));
  }
  if (city.trees) layout.trees();
  return true;
}

Vec2 city_street_point(const CityComponent& city, Rng& rng) {
  const f32 half = city.blocks * city.pitch * 0.5f;
  const f32 c = -half + static_cast<f32>(rng.below(static_cast<u32>(city.blocks) + 1)) * city.pitch;
  const f32 along = rng.range(-half + 10, half - 10);
  return rng.unit() < 0.5f ? Vec2{c, along} : Vec2{along, c};
}

}  // namespace kern::game
