#include "kern/geo/primitives.h"

#include <cmath>

namespace kern::primitives {

namespace {

void quad(MeshData& m, Vec3 a, Vec3 b, Vec3 c, Vec3 d, Vec3 n, Vec3 color) {
  const u32 base = m.vertex_count();
  m.push(a, n, color, {0, 0});
  m.push(b, n, color, {1, 0});
  m.push(c, n, color, {1, 1});
  m.push(d, n, color, {0, 1});
  m.indices.insert(m.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
}

}  // namespace

MeshData box(Vec3 s, Vec3 color) {
  MeshData m;
  const f32 x = s.x * 0.5f, y = s.y * 0.5f, z = s.z * 0.5f;
  quad(m, {x, -y, -z}, {x, y, -z}, {x, y, z}, {x, -y, z}, {1, 0, 0}, color);
  quad(m, {-x, y, -z}, {-x, -y, -z}, {-x, -y, z}, {-x, y, z}, {-1, 0, 0}, color);
  quad(m, {x, y, -z}, {-x, y, -z}, {-x, y, z}, {x, y, z}, {0, 1, 0}, color);
  quad(m, {-x, -y, -z}, {x, -y, -z}, {x, -y, z}, {-x, -y, z}, {0, -1, 0}, color);
  quad(m, {-x, -y, z}, {x, -y, z}, {x, y, z}, {-x, y, z}, {0, 0, 1}, color);
  quad(m, {-x, y, -z}, {x, y, -z}, {x, -y, -z}, {-x, -y, -z}, {0, 0, -1}, color);
  return m;
}

MeshData box_on_ground(Vec3 s, Vec3 color) {
  MeshData m = box(s, color);
  for (std::size_t i = 2; i < m.positions.size(); i += 3) m.positions[i] += s.z * 0.5f;
  return m;
}

MeshData sphere(f32 r, u32 segments, u32 rings, Vec3 color) {
  MeshData m;
  segments = max_of<u32>(3, segments);
  rings = max_of<u32>(2, rings);
  for (u32 j = 0; j <= rings; ++j) {
    const f32 v = static_cast<f32>(j) / rings, theta = v * kPi;
    for (u32 i = 0; i <= segments; ++i) {
      const f32 u = static_cast<f32>(i) / segments, phi = u * 2 * kPi;
      const Vec3 n{std::sin(theta) * std::cos(phi), std::sin(theta) * std::sin(phi), std::cos(theta)};
      m.push(n * r, n, color, {u, v});
    }
  }
  for (u32 j = 0; j < rings; ++j)
    for (u32 i = 0; i < segments; ++i) {
      const u32 a = j * (segments + 1) + i, b = a + segments + 1;
      if (j != 0) m.indices.insert(m.indices.end(), {a, b, a + 1});
      if (j != rings - 1) m.indices.insert(m.indices.end(), {a + 1, b, b + 1});
    }
  return m;
}

MeshData cylinder(f32 r, f32 h, u32 segments, Vec3 color) {
  MeshData m;
  segments = max_of<u32>(3, segments);
  for (u32 i = 0; i <= segments; ++i) {
    const f32 a = static_cast<f32>(i) / segments * 2 * kPi;
    const Vec3 n{std::cos(a), std::sin(a), 0};
    m.push(n * r, n, color, {static_cast<f32>(i) / segments, 0});
    m.push(n * r + Vec3{0, 0, h}, n, color, {static_cast<f32>(i) / segments, 1});
  }
  for (u32 i = 0; i < segments; ++i) {
    const u32 a = i * 2;
    m.indices.insert(m.indices.end(), {a, a + 2, a + 3, a, a + 3, a + 1});
  }
  for (int cap = 0; cap < 2; ++cap) {
    const f32 z = cap ? h : 0;
    const Vec3 n{0, 0, cap ? 1.0f : -1.0f};
    const u32 center = m.vertex_count();
    m.push({0, 0, z}, n, color, {0.5f, 0.5f});
    for (u32 i = 0; i <= segments; ++i) {
      const f32 a = static_cast<f32>(i) / segments * 2 * kPi;
      m.push({std::cos(a) * r, std::sin(a) * r, z}, n, color, {0.5f + std::cos(a) * 0.5f, 0.5f + std::sin(a) * 0.5f});
    }
    for (u32 i = 0; i < segments; ++i) {
      if (cap) m.indices.insert(m.indices.end(), {center, center + 1 + i, center + 2 + i});
      else m.indices.insert(m.indices.end(), {center, center + 2 + i, center + 1 + i});
    }
  }
  return m;
}

MeshData cone(f32 r, f32 h, u32 segments, Vec3 color) {
  MeshData m;
  segments = max_of<u32>(3, segments);
  const f32 slope = r / h;
  for (u32 i = 0; i < segments; ++i) {
    const f32 a0 = static_cast<f32>(i) / segments * 2 * kPi, a1 = static_cast<f32>(i + 1) / segments * 2 * kPi, am = (a0 + a1) * 0.5f;
    const Vec3 n0 = normalize({std::cos(a0), std::sin(a0), slope}), n1 = normalize({std::cos(a1), std::sin(a1), slope});
    const Vec3 nm = normalize({std::cos(am), std::sin(am), slope});
    const u32 base = m.vertex_count();
    m.push({std::cos(a0) * r, std::sin(a0) * r, 0}, n0, color, {0, 0});
    m.push({std::cos(a1) * r, std::sin(a1) * r, 0}, n1, color, {1, 0});
    m.push({0, 0, h}, nm, color, {0.5f, 1});
    m.indices.insert(m.indices.end(), {base, base + 1, base + 2});
  }
  const u32 center = m.vertex_count();
  m.push({0, 0, 0}, {0, 0, -1}, color, {0.5f, 0.5f});
  for (u32 i = 0; i <= segments; ++i) {
    const f32 a = static_cast<f32>(i) / segments * 2 * kPi;
    m.push({std::cos(a) * r, std::sin(a) * r, 0}, {0, 0, -1}, color, {0, 0});
  }
  for (u32 i = 0; i < segments; ++i) m.indices.insert(m.indices.end(), {center, center + 2 + i, center + 1 + i});
  return m;
}

MeshData plane(f32 w, f32 d, Vec3 color, f32 uv_scale) {
  MeshData m;
  const f32 x = w * 0.5f, y = d * 0.5f;
  const u32 base = 0;
  m.push({-x, -y, 0}, {0, 0, 1}, color, {0, 0});
  m.push({x, -y, 0}, {0, 0, 1}, color, {w * uv_scale, 0});
  m.push({x, y, 0}, {0, 0, 1}, color, {w * uv_scale, d * uv_scale});
  m.push({-x, y, 0}, {0, 0, 1}, color, {0, d * uv_scale});
  m.indices = {base, base + 1, base + 2, base, base + 2, base + 3};
  return m;
}

void append(MeshData& dst, const MeshData& src, const Mat4& m) {
  f32 nm[9];
  normal_matrix3(m.m, nm);
  const u32 base = dst.vertex_count();
  for (u32 v = 0; v < src.vertex_count(); ++v) {
    const Vec3 p = m.transform_point(Vec3::load(&src.positions[v * 3]));
    const Vec3 n0 = Vec3::load(&src.normals[v * 3]);
    const Vec3 n = normalize({nm[0] * n0.x + nm[3] * n0.y + nm[6] * n0.z, nm[1] * n0.x + nm[4] * n0.y + nm[7] * n0.z,
                              nm[2] * n0.x + nm[5] * n0.y + nm[8] * n0.z});
    dst.push(p, n, Vec3::load(&src.colors[v * 3]), {src.uvs[v * 2], src.uvs[v * 2 + 1]});
  }
  if (src.indices.empty()) {
    for (u32 v = 0; v < src.vertex_count(); ++v) dst.indices.push_back(base + v);
  } else {
    for (u32 i : src.indices) dst.indices.push_back(base + i);
  }
}

}  // namespace kern::primitives
