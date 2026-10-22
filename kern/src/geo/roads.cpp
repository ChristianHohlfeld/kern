#include "kern/geo/roads.h"

#include <cmath>

namespace kern {

bool RoadBuilder::begin(const f64* stream, std::size_t length, const RoadOptions& options, HeightSampler heights) {
  if (!(options.radius > 0) || !(options.chunk_size > 0) || !(options.segment_length > 0)) return fail("invalid road options");
  if (!stream || length < 1) return fail("empty road stream");
  stream_ = stream;
  length_ = length;
  cursor_ = 1;
  count_ = static_cast<u32>(stream[0]);
  index_ = 0;
  options_ = options;
  heights_ = heights;
  chunks_.clear();
  chunk_index_.clear();
  roads_ = triangles_ = 0;
  done_ = count_ == 0;
  failed_ = false;
  return true;
}

GeoChunk& RoadBuilder::chunk_for(f64 x, f64 y) {
  const i32 cx = static_cast<i32>(std::floor(x / options_.chunk_size)), cy = static_cast<i32>(std::floor(y / options_.chunk_size));
  const u64 key = (static_cast<u64>(static_cast<u32>(cx)) << 32) | static_cast<u32>(cy);
  auto it = chunk_index_.find(key);
  if (it != chunk_index_.end()) return chunks_[it->second];
  chunk_index_.emplace(key, static_cast<u32>(chunks_.size()));
  chunks_.emplace_back();
  chunks_.back().x = cx;
  chunks_.back().y = cy;
  return chunks_.back();
}

void RoadBuilder::tri(const f64* a, const f64* b, const f64* c, f64 z, Vec3 color) {
  if (failed_) return;
  GeoChunk& chunk = chunk_for((a[0] + b[0] + c[0]) / 3, (a[1] + b[1] + c[1]) / 3);
  const Vec3 up{0, 0, 1};
  for (const f64* q : {a, b, c}) {
    const f64 h = heights_(q[0], q[1]);
    if (!std::isfinite(h)) { failed_ = true; fail("invalid terrain elevation at %.2f, %.2f", q[0], q[1]); return; }
    chunk.mesh.push({static_cast<f32>(q[0]), static_cast<f32>(q[1]), static_cast<f32>(h + z)}, up, color,
                    {static_cast<f32>(q[0] * options_.uv_scale), static_cast<f32>(q[1] * options_.uv_scale)});
  }
  chunk.triangles++;
  triangles_++;
}

void RoadBuilder::band(f64 ax, f64 ay, f64 bx, f64 by, f64 width, f64 z, const Rgb& color) {
  const f64 dx = bx - ax, dy = by - ay, len = std::hypot(dx, dy);
  if (len < 0.05) return;
  const f64 nx = -dy / len * width * 0.5, ny = dx / len * width * 0.5;
  const u32 segments = max_of<u32>(1, static_cast<u32>(std::ceil(len / options_.segment_length)));
  const f64 cx = options_.center_x, cy = options_.center_y;
  const f64 limit = options_.radius + options_.segment_length + width;
  const Vec3 col = color.f();
  for (u32 i = 0; i < segments; ++i) {
    const f64 t = static_cast<f64>(i) / segments, u = static_cast<f64>(i + 1) / segments;
    const f64 px = ax + dx * t, py = ay + dy * t, qx = ax + dx * u, qy = ay + dy * u;
    if (min_of(std::hypot(px - cx, py - cy), std::hypot(qx - cx, qy - cy)) > limit) continue;
    const f64 A[2] = {px + nx, py + ny}, B[2] = {px - nx, py - ny}, C[2] = {qx - nx, qy - ny}, D[2] = {qx + nx, qy + ny};
    tri(A, B, C, z, col);
    tri(A, C, D, z, col);
  }
}

bool RoadBuilder::build_one() {
  if (cursor_ + 6 > length_) return false;
  const f64* s = stream_ + cursor_;
  const f64 width = s[0];
  const u32 flags = static_cast<u32>(s[1]);
  const u32 surface = static_cast<u32>(s[2]), marking = static_cast<u32>(s[3]), n = static_cast<u32>(s[4]);
  cursor_ += 5;
  if (cursor_ + n * 2 > length_) return false;
  const f64* p = stream_ + cursor_;
  cursor_ += n * 2;
  if (n < 2 || surface >= options_.palette.size() || ((flags & 2) && marking >= options_.palette.size())) return true;
  roads_++;
  if (flags & 1)
    for (u32 i = 0; i + 1 < n; ++i) band(p[i * 2], p[i * 2 + 1], p[i * 2 + 2], p[i * 2 + 3], width + options_.curb_extra, options_.curb_z, options_.curb_color);
  const Rgb& surface_color = options_.palette[surface];
  for (u32 i = 0; i + 1 < n; ++i) {
    const f64 ax = p[i * 2], ay = p[i * 2 + 1], bx = p[i * 2 + 2], by = p[i * 2 + 3];
    band(ax, ay, bx, by, width, options_.surface_z, surface_color);
    if (flags & 2) {
      const f64 dx = bx - ax, dy = by - ay, len = std::hypot(dx, dy);
      if (len < 3) continue;
      const f64 ux = dx / len, uy = dy / len;
      const Rgb& mc = options_.palette[marking];
      for (f64 t = options_.dash_start; t + options_.dash_length < len; t += options_.dash_period)
        band(ax + ux * t, ay + uy * t, ax + ux * (t + options_.dash_length), ay + uy * (t + options_.dash_length), options_.marking_width,
             options_.marking_z, mc);
    }
  }
  return !failed_;
}

bool RoadBuilder::step(f64 budget_ms) {
  if (done_) return true;
  const f64 start = now_ms();
  do {
    if (index_ >= count_ || !build_one()) { done_ = true; return true; }
    ++index_;
    if (index_ >= count_) { done_ = true; return true; }
  } while (now_ms() - start < budget_ms);
  return false;
}

bool build_terrain_tile(f64 x0, f64 y0, f64 step, u32 cells, Rgb albedo, HeightSampler heights, MeshData& out, f64* min_height,
                        f64* max_height) {
  if (!std::isfinite(x0) || !std::isfinite(y0) || !(step > 0) || cells < 1 || cells > 128) return fail("invalid terrain tile");
  const u32 n = cells + 1, g = n + 2;  // node grid with a one-node border for normals
  std::vector<f64> h(static_cast<std::size_t>(g) * g);
  for (u32 j = 0; j < g; ++j)
    for (u32 i = 0; i < g; ++i) {
      const f64 v = heights(x0 + (static_cast<f64>(i) - 1) * step, y0 + (static_cast<f64>(j) - 1) * step);
      if (!std::isfinite(v)) return fail("non-finite terrain height");
      h[static_cast<std::size_t>(j) * g + i] = v;
    }
  out.clear();
  out.extra_size = 0;
  const std::size_t vertices = static_cast<std::size_t>(n) * n;
  out.positions.resize(vertices * 3);
  out.normals.resize(vertices * 3);
  out.colors.resize(vertices * 3);
  out.uvs.resize(vertices * 2);
  const Vec3 c{static_cast<f32>(clamp(albedo.r, 0.0, 1.0)), static_cast<f32>(clamp(albedo.g, 0.0, 1.0)), static_cast<f32>(clamp(albedo.b, 0.0, 1.0))};
  f64 lo = INFINITY, hi = -INFINITY;
  auto H = [&](u32 i, u32 j) { return h[static_cast<std::size_t>(j + 1) * g + (i + 1)]; };
  for (u32 j = 0; j < n; ++j)
    for (u32 i = 0; i < n; ++i) {
      const std::size_t v = static_cast<std::size_t>(j) * n + i, k = v * 3;
      const f64 z = H(i, j);
      lo = min_of(lo, z);
      hi = max_of(hi, z);
      out.positions[k] = static_cast<f32>(x0 + i * step);
      out.positions[k + 1] = static_cast<f32>(y0 + j * step);
      out.positions[k + 2] = static_cast<f32>(z);
      const f64 dx = h[static_cast<std::size_t>(j + 1) * g + (i + 2)] - h[static_cast<std::size_t>(j + 1) * g + i];
      const f64 dy = h[static_cast<std::size_t>(j + 2) * g + (i + 1)] - h[static_cast<std::size_t>(j) * g + (i + 1)];
      f64 len = std::hypot(dx, dy, 2 * step);
      if (!(len > 0)) len = 1;
      out.normals[k] = static_cast<f32>(-dx / len);
      out.normals[k + 1] = static_cast<f32>(-dy / len);
      out.normals[k + 2] = static_cast<f32>(2 * step / len);
      out.colors[k] = c.x; out.colors[k + 1] = c.y; out.colors[k + 2] = c.z;
      out.uvs[v * 2] = static_cast<f32>(static_cast<f64>(i) / cells);
      out.uvs[v * 2 + 1] = static_cast<f32>(static_cast<f64>(j) / cells);
    }
  out.indices.resize(static_cast<std::size_t>(cells) * cells * 6);
  std::size_t o = 0;
  for (u32 j = 0; j < cells; ++j)
    for (u32 i = 0; i < cells; ++i) {
      const u32 sw = j * n + i, se = sw + 1, nw = sw + n, ne = nw + 1;
      out.indices[o++] = sw; out.indices[o++] = se; out.indices[o++] = ne;
      out.indices[o++] = sw; out.indices[o++] = ne; out.indices[o++] = nw;
    }
  if (min_height) *min_height = lo;
  if (max_height) *max_height = hi;
  return true;
}

}  // namespace kern
