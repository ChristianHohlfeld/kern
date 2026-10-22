#include "kern/geo/mesh_data.h"

#include <cstring>

namespace kern {

MeshInput MeshData::as_input(u32 extra_slot) const {
  MeshInput in;
  in.positions = Span<const f32>(positions.data(), positions.size());
  in.normals = Span<const f32>(normals.data(), normals.size());
  in.colors = Span<const f32>(colors.data(), colors.size());
  in.uvs = Span<const f32>(uvs.data(), uvs.size());
  in.indices = Span<const u32>(indices.data(), indices.size());
  if (!extra.empty() && extra_slot < kMaxExtensionAttributes) in.extension[extra_slot] = Span<const f32>(extra.data(), extra.size());
  return in;
}

namespace {

constexpr u32 kMaxStride = 3 + 3 + 3 + 2 + 4;

u64 mix64(u64 h) {
  h ^= h >> 33; h *= 0xff51afd7ed558ccdull;
  h ^= h >> 33; h *= 0xc4ceb9fe1a85ec53ull;
  h ^= h >> 33;
  return h;
}

}  // namespace

WeldStats weld(MeshData& m, std::size_t min_saving) {
  WeldStats s;
  const u32 n = m.vertex_count();
  s.source_vertices = n;
  s.bytes_before = m.bytes();
  if (!n || !m.indices.empty() || m.extra_size > 4) return s;
  const u32 es = m.extra.empty() ? 0 : m.extra_size;
  const u32 stride = 11 + es;
  if (stride > kMaxStride) return s;

  // Gather each vertex's attribute bits into one contiguous key row.
  std::vector<u32> keys(static_cast<std::size_t>(n) * stride);
  for (u32 v = 0; v < n; ++v) {
    u32* k = &keys[static_cast<std::size_t>(v) * stride];
    std::memcpy(k, &m.positions[v * 3], 12);
    std::memcpy(k + 3, &m.normals[v * 3], 12);
    std::memcpy(k + 6, &m.colors[v * 3], 12);
    std::memcpy(k + 9, &m.uvs[v * 2], 8);
    if (es) std::memcpy(k + 11, &m.extra[static_cast<std::size_t>(v) * es], es * 4);
  }
  // Open-addressing table of first-occurrence vertex ids.
  u32 capacity = 16;
  while (capacity < n * 2) capacity <<= 1;
  std::vector<u32> table(capacity, ~0u);
  std::vector<u32> remap(n);
  std::vector<u32> unique;
  unique.reserve(n);
  for (u32 v = 0; v < n; ++v) {
    const u32* k = &keys[static_cast<std::size_t>(v) * stride];
    u64 h = 1469598103934665603ull;
    for (u32 i = 0; i < stride; ++i) h = (h ^ k[i]) * 1099511628211ull;
    u32 slot = static_cast<u32>(mix64(h)) & (capacity - 1);
    for (;;) {
      const u32 entry = table[slot];
      if (entry == ~0u) {
        table[slot] = v;
        remap[v] = static_cast<u32>(unique.size());
        unique.push_back(v);
        break;
      }
      if (std::memcmp(&keys[static_cast<std::size_t>(entry) * stride], k, stride * 4) == 0) {
        remap[v] = remap[entry];
        break;
      }
      slot = (slot + 1) & (capacity - 1);
    }
  }
  const u32 u = static_cast<u32>(unique.size());
  const std::size_t after = static_cast<std::size_t>(u) * stride * 4 + static_cast<std::size_t>(n) * (u <= 65536 ? 2 : 4);
  s.unique_vertices = u;
  if (!(s.bytes_before > after + min_saving)) { s.bytes_after = s.bytes_before; return s; }

  MeshData out;
  out.extra_size = m.extra_size;
  out.positions.resize(static_cast<std::size_t>(u) * 3);
  out.normals.resize(static_cast<std::size_t>(u) * 3);
  out.colors.resize(static_cast<std::size_t>(u) * 3);
  out.uvs.resize(static_cast<std::size_t>(u) * 2);
  if (es) out.extra.resize(static_cast<std::size_t>(u) * es);
  for (u32 i = 0; i < u; ++i) {
    const u32 v = unique[i];
    std::memcpy(&out.positions[i * 3], &m.positions[v * 3], 12);
    std::memcpy(&out.normals[i * 3], &m.normals[v * 3], 12);
    std::memcpy(&out.colors[i * 3], &m.colors[v * 3], 12);
    std::memcpy(&out.uvs[i * 2], &m.uvs[v * 2], 8);
    if (es) std::memcpy(&out.extra[static_cast<std::size_t>(i) * es], &m.extra[static_cast<std::size_t>(v) * es], es * 4);
  }
  out.indices = std::move(remap);
  m = std::move(out);
  s.bytes_after = after;
  s.indexed = true;
  return s;
}

u32 content_hash(const MeshData& m) {
  u32 h = 2166136261u;
  auto feed = [&](const void* data, std::size_t words) {
    const u32* w = static_cast<const u32*>(data);
    for (std::size_t i = 0; i < words; ++i) h = (h ^ w[i]) * 16777619u;
  };
  feed(m.positions.data(), m.positions.size());
  feed(m.normals.data(), m.normals.size());
  feed(m.colors.data(), m.colors.size());
  feed(m.uvs.data(), m.uvs.size());
  feed(m.extra.data(), m.extra.size());
  feed(m.indices.data(), m.indices.size());
  return h;
}

}  // namespace kern
