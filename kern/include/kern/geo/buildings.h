// Extruded buildings from 2D footprints (outer ring + courtyard holes):
// earcut roofs, walls, an optional cornice band, grouped into square chunks
// (one mesh per chunk) and built incrementally under a time budget.
//
// Each vertex can carry a 4-float "wall record" for facade shaders:
//   (wall length | -1 roof | -2 cornice, wall height, building seed, storey height)
// with u = metres along the wall and v = metres above the base in the UVs.
#pragma once

#include <unordered_map>
#include <vector>

#include "kern/geo/earcut.h"
#include "kern/geo/mesh_data.h"

namespace kern {

/// Colours are kept in double precision so derived colours (the cornice mix)
/// round to exactly the same float values as a double-precision reference.
struct Rgb {
  f64 r = 1, g = 1, b = 1;
  Vec3 f() const { return {static_cast<f32>(r), static_cast<f32>(g), static_cast<f32>(b)}; }
};

struct BuildingStyle {
  std::vector<Rgb> walls{{0.75, 0.73, 0.70}};
  std::vector<Rgb> roofs{{0.30, 0.30, 0.32}};
  Rgb trim{0.957, 0.937, 0.894};
  f64 trim_mix = 0.45;  // cornice colour = mix(wall, trim, trim_mix)
};

struct BuildingOptions {
  f64 chunk_size = 320;
  u32 max_buildings = 2600;
  bool cornice = true;
  bool wall_records = true;
  BuildingStyle style;
};

struct GeoChunk {
  i32 x = 0, y = 0;  // chunk grid coordinates
  u32 items = 0;     // buildings / roads in the chunk
  u32 triangles = 0;
  u32 hash = 0;      // content hash of the soup (before welding)
  bool welded = false;
  MeshData mesh;
};

/// Incremental footprint extrusion.
///
/// Footprint stream (f64 values): count, then per building
///   seed_hash, base, top, has_center, center_x, center_y, ring_count,
///   { vertex_count, x0, y0, x1, y1, ... } x ring_count   (first ring = outer)
/// `seed_hash` (u32) drives colours and facade variation.
class BuildingBuilder {
 public:
  bool begin(const f64* stream, std::size_t length, const BuildingOptions& options);
  /// Processes buildings until `budget_ms` elapsed. Returns true when finished.
  bool step(f64 budget_ms);
  bool done() const { return done_; }

  std::vector<GeoChunk>& chunks() { return chunks_; }
  u32 processed() const { return processed_; }
  u32 built() const { return built_; }
  u32 skipped() const { return skipped_; }
  u32 triangles() const { return triangles_; }

 private:
  bool build_one();
  GeoChunk& chunk_for(f64 x, f64 y);
  void tri(GeoChunk& c, Vec3 a, Vec3 b, Vec3 cc, Vec3 n, Vec3 color, Vec2 ua, Vec2 ub, Vec2 uc);

  const f64* stream_ = nullptr;
  std::size_t length_ = 0, cursor_ = 0;
  u32 count_ = 0;
  BuildingOptions options_;
  std::vector<GeoChunk> chunks_;
  std::unordered_map<u64, u32> chunk_index_;  // grid key -> chunk
  u32 processed_ = 0, built_ = 0, skipped_ = 0, triangles_ = 0;
  bool done_ = true;
  f32 record_[4] = {0, 0, 0, 3.3f};
  // scratch
  Earcut earcut_;
  std::vector<f64> ring_, xy_;
  std::vector<u32> ring_starts_, holes_, tris_;
};

/// Removes repeated points (< 1.5 cm apart) and the closing duplicate; empties
/// the ring when fewer than 3 points or |area| <= 0.08 m^2 remain. xy pairs.
void clean_ring(std::vector<f64>& xy);
f64 ring_area(const f64* xy, u32 count);

}  // namespace kern
