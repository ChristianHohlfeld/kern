#include "kern/geo/buildings.h"

#include <algorithm>
#include <cmath>

namespace kern {

f64 ring_area(const f64* xy, u32 n) {
  f64 s = 0;
  for (u32 i = 0; i < n; ++i) {
    const u32 j = (i + 1) % n;
    s += xy[i * 2] * xy[j * 2 + 1] - xy[j * 2] * xy[i * 2 + 1];
  }
  return s * 0.5;
}

void clean_ring(std::vector<f64>& xy) {
  std::size_t out = 0;
  for (std::size_t i = 0; i + 1 < xy.size(); i += 2) {
    const f64 x = xy[i], y = xy[i + 1];
    if (!std::isfinite(x) || !std::isfinite(y)) continue;
    if (out && std::hypot(x - xy[out - 2], y - xy[out - 1]) <= 0.015) continue;
    xy[out++] = x;
    xy[out++] = y;
  }
  xy.resize(out);
  const u32 n = static_cast<u32>(out / 2);
  if (n > 2 && std::hypot(xy[0] - xy[out - 2], xy[1] - xy[out - 1]) < 0.015) xy.resize(out - 2);
  const u32 m = static_cast<u32>(xy.size() / 2);
  if (m < 3 || !(std::fabs(ring_area(xy.data(), m)) > 0.08)) xy.clear();
}

namespace {

void reverse_ring(f64* xy, u32 n) {
  for (u32 i = 0, j = n - 1; i < j; ++i, --j) {
    std::swap(xy[i * 2], xy[j * 2]);
    std::swap(xy[i * 2 + 1], xy[j * 2 + 1]);
  }
}

Vec3 v3(f64 x, f64 y, f64 z) { return {static_cast<f32>(x), static_cast<f32>(y), static_cast<f32>(z)}; }
Vec2 v2(f64 x, f64 y) { return {static_cast<f32>(x), static_cast<f32>(y)}; }

}  // namespace

bool BuildingBuilder::begin(const f64* stream, std::size_t length, const BuildingOptions& options) {
  if (!(options.chunk_size >= 20) || !(options.max_buildings > 0)) return fail("invalid city geometry budget");
  if (options.style.walls.empty() || options.style.roofs.empty()) return fail("building palette must not be empty");
  if (!stream || length < 1) return fail("empty footprint stream");
  stream_ = stream;
  length_ = length;
  cursor_ = 1;
  if (!stream_count(stream[0], 1e9, count_)) return fail("invalid footprint count");
  count_ = min_of(count_, options.max_buildings);
  options_ = options;
  chunks_.clear();
  chunk_index_.clear();
  processed_ = built_ = skipped_ = triangles_ = 0;
  done_ = count_ == 0;
  return true;
}

GeoChunk& BuildingBuilder::chunk_for(f64 x, f64 y) {
  const i32 cx = static_cast<i32>(std::floor(x / options_.chunk_size)), cy = static_cast<i32>(std::floor(y / options_.chunk_size));
  const u64 key = (static_cast<u64>(static_cast<u32>(cx)) << 32) | static_cast<u32>(cy);
  auto it = chunk_index_.find(key);
  if (it != chunk_index_.end()) return chunks_[it->second];
  chunk_index_.emplace(key, static_cast<u32>(chunks_.size()));
  chunks_.emplace_back();
  GeoChunk& c = chunks_.back();
  c.x = cx;
  c.y = cy;
  c.mesh.extra_size = options_.wall_records ? 4 : 0;
  return c;
}

void BuildingBuilder::tri(GeoChunk& c, Vec3 a, Vec3 b, Vec3 cc, Vec3 n, Vec3 color, Vec2 ua, Vec2 ub, Vec2 uc) {
  c.mesh.push(a, n, color, ua);
  c.mesh.push(b, n, color, ub);
  c.mesh.push(cc, n, color, uc);
  if (options_.wall_records)
    for (int k = 0; k < 3; ++k) c.mesh.push_extra(record_);
}

bool BuildingBuilder::step(f64 budget_ms) {
  if (done_) return true;
  const f64 start = now_ms();
  do {
    if (!build_one()) { done_ = true; return true; }
    if (processed_ >= count_) { done_ = true; return true; }
  } while (now_ms() - start < budget_ms);
  return false;
}

bool BuildingBuilder::build_one() {
  // ---- parse ----
  if (cursor_ + 7 > length_) return false;
  const f64* s = stream_ + cursor_;
  const u32 seed_hash = stream_u32(s[0]);
  const f64 base = s[1], top = s[2];
  const bool has_center = s[3] != 0;
  f64 center_x = s[4], center_y = s[5];
  u32 ring_count = 0;
  if (!stream_count(s[6], 1e6, ring_count)) return false;
  cursor_ += 7;
  xy_.clear();
  ring_starts_.clear();
  bool reject = false;
  u32 outer_first_count = 0;
  f64 outer_first_x = 0, outer_first_y = 0;
  for (u32 r = 0; r < ring_count; ++r) {
    if (cursor_ + 1 > length_) return false;
    u32 n = 0;
    const f64 count_value = stream_[cursor_++];
    if (!stream_count(count_value, static_cast<f64>((length_ - cursor_) / 2), n)) return false;
    ring_.assign(stream_ + cursor_, stream_ + cursor_ + n * 2);
    cursor_ += n * 2;
    clean_ring(ring_);
    if (ring_.empty()) { reject = true; continue; }
    const u32 m = static_cast<u32>(ring_.size() / 2);
    if (r == 0) { outer_first_count = m; outer_first_x = ring_[0]; outer_first_y = ring_[1]; }
    // Outer ring counter-clockwise, holes clockwise.
    const f64 area = ring_area(ring_.data(), m);
    if (r == 0 ? area <= 0 : area >= 0) reverse_ring(ring_.data(), m);
    ring_starts_.push_back(static_cast<u32>(xy_.size() / 2));
    xy_.insert(xy_.end(), ring_.begin(), ring_.end());
  }
  ++processed_;
  if (!ring_count || !outer_first_count) reject = true;
  const f64 wall_height = top - base;
  if (!(wall_height > 0.1)) reject = true;
  const u32 corners = static_cast<u32>(xy_.size() / 2);
  if (!reject) {
    holes_.assign(ring_starts_.begin() + 1, ring_starts_.end());
    tris_.clear();
    earcut_.triangulate(xy_.data(), corners, holes_.data(), static_cast<u32>(holes_.size()), tris_);
    if (tris_.empty() ||
        Earcut::deviation(xy_.data(), corners, holes_.data(), static_cast<u32>(holes_.size()), tris_.data(), static_cast<u32>(tris_.size())) > 0.007)
      reject = true;
  }
  if (reject) { ++skipped_; return true; }

  if (!has_center) { center_x = outer_first_x; center_y = outer_first_y; }
  GeoChunk& chunk = chunk_for(center_x, center_y);
  const BuildingStyle& st = options_.style;
  const u32 h = seed_hash;
  const Rgb wall64 = st.walls[h % st.walls.size()];
  const Vec3 wall = wall64.f();
  const Vec3 roof = st.roofs[(h >> 8) % st.roofs.size()].f();
  const f64 seed = static_cast<f64>(h % 997) / 997.0;
  const f64 floor_h = 3.1 + 0.5 * (static_cast<f64>((h >> 12) % 1000) / 1000.0);
  const Vec3 trim = Rgb{wall64.r + (st.trim.r - wall64.r) * st.trim_mix, wall64.g + (st.trim.g - wall64.g) * st.trim_mix,
                        wall64.b + (st.trim.b - wall64.b) * st.trim_mix}.f();
  const Vec3 up{0, 0, 1}, down{0, 0, -1};

  // Roof: every face points up whatever the source winding; courtyards stay open.
  record_[0] = -1; record_[1] = static_cast<f32>(wall_height); record_[2] = static_cast<f32>(seed); record_[3] = static_cast<f32>(floor_h);
  const f64* P = xy_.data();
  for (std::size_t i = 0; i < tris_.size(); i += 3) {
    const u32 a = tris_[i], b = tris_[i + 1], c = tris_[i + 2];
    const f64 ax = P[a * 2], ay = P[a * 2 + 1], bx = P[b * 2], by = P[b * 2 + 1], cx = P[c * 2], cy = P[c * 2 + 1];
    const f64 cross = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
    if (std::fabs(cross) < 0.01) continue;
    const Vec3 pa = v3(ax, ay, top), pb = v3(bx, by, top), pc = v3(cx, cy, top);
    if (cross > 0) tri(chunk, pa, pb, pc, up, roof, v2(ax, ay), v2(bx, by), v2(cx, cy));
    else tri(chunk, pa, pc, pb, up, roof, v2(ax, ay), v2(cx, cy), v2(bx, by));
    chunk.triangles++;
    triangles_++;
  }
  // Walls (+ cornice) along every ring edge.
  for (std::size_t r = 0; r < ring_starts_.size(); ++r) {
    const u32 first = ring_starts_[r], end = r + 1 < ring_starts_.size() ? ring_starts_[r + 1] : corners, n = end - first;
    for (u32 i = 0; i < n; ++i) {
      const u32 ia = first + i, ib = first + (i + 1) % n;
      const f64 ax = P[ia * 2], ay = P[ia * 2 + 1], bx = P[ib * 2], by = P[ib * 2 + 1];
      const f64 dx = bx - ax, dy = by - ay, len = std::hypot(dx, dy);
      if (len < 0.02) continue;
      const f64 nx = dy / len, ny = -dx / len;
      const Vec3 normal = v3(nx, ny, 0);
      const Vec3 a0 = v3(ax, ay, base), b0 = v3(bx, by, base), a1 = v3(ax, ay, top), b1 = v3(bx, by, top);
      record_[0] = static_cast<f32>(len);
      tri(chunk, a0, b0, b1, normal, wall, {0, 0}, v2(len, 0), v2(len, wall_height));
      tri(chunk, a0, b1, a1, normal, wall, {0, 0}, v2(len, wall_height), v2(0, wall_height));
      if (options_.cornice && len > 1.2 && wall_height > 3.5) {
        const f64 o = 0.24, oax = ax + nx * o, oay = ay + ny * o, obx = bx + nx * o, oby = by + ny * o, z0 = top - 0.55, z1 = top;
        record_[0] = -2;
        tri(chunk, v3(oax, oay, z0), v3(obx, oby, z0), v3(obx, oby, z1), normal, trim, {0, 0}, v2(len, 0), v2(len, 1));
        tri(chunk, v3(oax, oay, z0), v3(obx, oby, z1), v3(oax, oay, z1), normal, trim, {0, 0}, v2(len, 1), v2(0, 1));
        tri(chunk, v3(ax, ay, z1), v3(oax, oay, z1), v3(obx, oby, z1), up, trim, {0, 0}, {0, 0}, {0, 0});
        tri(chunk, v3(ax, ay, z1), v3(obx, oby, z1), v3(bx, by, z1), up, trim, {0, 0}, {0, 0}, {0, 0});
        tri(chunk, v3(oax, oay, z0), v3(ax, ay, z0), v3(bx, by, z0), down, trim, {0, 0}, {0, 0}, {0, 0});
        tri(chunk, v3(oax, oay, z0), v3(bx, by, z0), v3(obx, oby, z0), down, trim, {0, 0}, {0, 0}, {0, 0});
        chunk.triangles += 6;
        triangles_ += 6;
      }
      chunk.triangles += 2;
      triangles_ += 2;
    }
  }
  chunk.items++;
  built_++;
  return true;
}

}  // namespace kern
