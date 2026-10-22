// CPU mesh assembly: triangle-soup builders, lossless welding into indexed
// meshes and content hashing for change detection.
#pragma once

#include <vector>

#include "kern/gfx/mesh.h"

namespace kern {

/// Plain geometry container (triangle soup when `indices` is empty).
struct MeshData {
  std::vector<f32> positions, normals, colors, uvs;
  std::vector<f32> extra;  // optional per-vertex stream (extra_size floats each)
  u32 extra_size = 0;
  std::vector<u32> indices;

  u32 vertex_count() const { return static_cast<u32>(positions.size() / 3); }
  u32 triangle_count() const { return indices.empty() ? vertex_count() / 3 : static_cast<u32>(indices.size() / 3); }
  void clear() {
    positions.clear(); normals.clear(); colors.clear(); uvs.clear(); extra.clear(); indices.clear();
  }
  void push(Vec3 p, Vec3 n, Vec3 c, Vec2 uv) {
    positions.insert(positions.end(), {p.x, p.y, p.z});
    normals.insert(normals.end(), {n.x, n.y, n.z});
    colors.insert(colors.end(), {c.x, c.y, c.z});
    uvs.insert(uvs.end(), {uv.x, uv.y});
  }
  void push_extra(const f32* values) { extra.insert(extra.end(), values, values + extra_size); }
  std::size_t bytes() const {
    return (positions.size() + normals.size() + colors.size() + uvs.size() + extra.size()) * 4 + indices.size() * (vertex_count() <= 65536 ? 2 : 4);
  }
  /// Borrowed MeshInput view (geometry only; material etc. stay default).
  MeshInput as_input(u32 extra_slot = 0) const;
};

struct WeldStats {
  u32 source_vertices = 0, unique_vertices = 0;
  std::size_t bytes_before = 0, bytes_after = 0;
  bool indexed = false;
};

/// Merges vertices whose position, normal, colour, uv and extra stream are
/// bit-identical (IEEE-754 patterns, never rounded), producing an indexed
/// mesh. Hard edges stay hard. Keeps the soup when indexing would not save
/// bytes (`min_saving`). The mesh must be a triangle soup.
WeldStats weld(MeshData& mesh, std::size_t min_saving = 0);

/// FNV-1a over the bit patterns of all float streams (and indices).
u32 content_hash(const MeshData& mesh);

/// Reads a non-negative integer count from a f64 stream value; false for NaN,
/// negative, fractional or values above `limit` (never casts out of range).
inline bool stream_count(f64 v, f64 limit, u32& out) {
  if (!(v >= 0 && v <= limit) || v != static_cast<f64>(static_cast<u64>(v))) return false;
  out = static_cast<u32>(v);
  return true;
}
/// A u32 hash carried in a f64 (0 for anything that is not an integer in range).
inline u32 stream_u32(f64 v) { return v >= 0 && v <= 4294967295.0 ? static_cast<u32>(v) : 0u; }

/// Terrain/ground height lookup supplied by the game (physics truth).
struct HeightSampler {
  f64 (*fn)(void* user, f64 x, f64 y) = nullptr;
  void* user = nullptr;
  f64 operator()(f64 x, f64 y) const { return fn ? fn(user, x, y) : 0.0; }
};

}  // namespace kern
