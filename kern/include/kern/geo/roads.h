// Street ribbons draped on the terrain: optional curb band, surface band and
// dashed centre marking per road polyline, grouped into square chunks and
// built incrementally under a time budget.
#pragma once

#include <unordered_map>
#include <vector>

#include "kern/geo/buildings.h"
#include "kern/geo/mesh_data.h"

namespace kern {

struct RoadOptions {
  f64 center_x = 0, center_y = 0;
  f64 radius = 800;         // segments farther than this (+ margin) are skipped
  f64 chunk_size = 420;
  f64 segment_length = 6;   // ribbons are split so they follow the terrain
  f64 curb_extra = 3.4;     // curb band = road width + curb_extra
  f64 curb_z = 0.09, surface_z = 0.112, marking_z = 0.135;
  f64 marking_width = 0.42, dash_start = 1, dash_length = 3, dash_period = 7;
  f64 uv_scale = 0.2;
  Rgb curb_color{0.525, 0.514, 0.486};
  std::vector<Rgb> palette;  // surface / marking colours referenced by index
};

/// Road stream (f64 values): count, then per road
///   width, flags (1 = curb, 2 = marking), surface_color, marking_color, point_count, x0, y0, ...
class RoadBuilder {
 public:
  bool begin(const f64* stream, std::size_t length, const RoadOptions& options, HeightSampler heights);
  bool step(f64 budget_ms);
  bool done() const { return done_; }
  bool failed() const { return failed_; }
  std::vector<GeoChunk>& chunks() { return chunks_; }
  u32 roads() const { return roads_; }
  u32 triangles() const { return triangles_; }

 private:
  bool build_one();
  void band(f64 ax, f64 ay, f64 bx, f64 by, f64 width, f64 z, const Rgb& color);
  void tri(const f64* a, const f64* b, const f64* c, f64 z, Vec3 color);
  GeoChunk& chunk_for(f64 x, f64 y);

  const f64* stream_ = nullptr;
  std::size_t length_ = 0, cursor_ = 0;
  u32 count_ = 0, index_ = 0;
  RoadOptions options_;
  HeightSampler heights_;
  std::vector<GeoChunk> chunks_;
  std::unordered_map<u64, u32> chunk_index_;
  u32 roads_ = 0, triangles_ = 0;
  bool done_ = true, failed_ = false;
};

/// Regular-grid terrain tile with one vertex per height node (cells x cells
/// quads, both halves split along the SW-NE diagonal). Normals from central
/// differences of the neighbouring nodes. Writes an indexed mesh.
bool build_terrain_tile(f64 x0, f64 y0, f64 step, u32 cells, Rgb albedo, HeightSampler heights, MeshData& out, f64* min_height = nullptr,
                        f64* max_height = nullptr);

/// WGS84 local tangent plane: metres east/north of an origin (degrees).
struct GeoProjector {
  f64 lon0 = 0, lat0 = 0, cos_lat = 1;
  static constexpr f64 kEarthRadius = 6378137.0;
  static constexpr f64 kRad = 3.14159265358979323846 / 180.0;
  bool init(f64 lon, f64 lat) {
    if (!std::isfinite(lon) || !std::isfinite(lat) || std::fabs(lon) > 180 || std::fabs(lat) > 85) return fail("invalid projection origin");
    lon0 = lon;
    lat0 = lat;
    cos_lat = max_of(0.01, std::cos(lat * kRad));
    return true;
  }
  void project(f64 lon, f64 lat, f64& x, f64& y) const {
    x = (lon - lon0) * kRad * kEarthRadius * cos_lat;
    y = (lat - lat0) * kRad * kEarthRadius;
  }
};

}  // namespace kern
