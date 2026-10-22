// C ABI of the geometry builders: incremental jobs for footprint extrusion
// and road ribbons, plus synchronous terrain tiles. Results are committed
// straight into the renderer (welded), so the host never touches vertices.
#include <cmath>
#include <memory>
#include <vector>

#include "kern/api/c_api.h"
#include "kern/geo/buildings.h"
#include "kern/geo/roads.h"
#include "kern/gfx/renderer.h"

using namespace kern;

#if defined(__wasm__)
extern "C" __attribute__((import_module("kern_env"), import_name("terrain_height"))) f64 kern_env_terrain_height(f64 x, f64 y);
#else
static f64 (*g_native_height)(f64, f64) = nullptr;
extern "C" void kern_set_native_height_sampler(f64 (*fn)(f64, f64)) { g_native_height = fn; }
static f64 kern_env_terrain_height(f64 x, f64 y) { return g_native_height ? g_native_height(x, y) : 0.0; }
#endif

namespace {

HeightSampler host_heights() {
  HeightSampler s;
  s.fn = [](void*, f64 x, f64 y) { return kern_env_terrain_height(x, y); };
  return s;
}

struct GeoJob {
  enum Kind { kBuildings, kRoads } kind = kBuildings;
  std::vector<f64> stream;  // own copy: the host reuses its staging memory
  BuildingBuilder buildings;
  RoadBuilder roads;
  bool failed = false;
  std::vector<GeoChunk>& chunks() { return kind == kBuildings ? buildings.chunks() : roads.chunks(); }
};

std::vector<std::unique_ptr<GeoJob>> g_jobs(1);

GeoJob* job(u32 id) { return id < g_jobs.size() ? g_jobs[id].get() : nullptr; }

u32 store(std::unique_ptr<GeoJob> j) {
  for (u32 i = 1; i < g_jobs.size(); ++i)
    if (!g_jobs[i]) { g_jobs[i] = std::move(j); return i; }
  g_jobs.push_back(std::move(j));
  return static_cast<u32>(g_jobs.size() - 1);
}

Rgb rgb_at(const f64* o, u32 slot) { return {o[slot], o[slot + 1], o[slot + 2]}; }

bool all_finite(const f64* o, u32 n) {
  for (u32 i = 0; i < n; ++i)
    if (!std::isfinite(o[i])) return false;
  return true;
}

}  // namespace

/// options (f64): chunk_size, max_buildings, cornice, wall_records, trim rgb, trim_mix,
///                wall_count, roof_count, walls rgb..., roofs rgb...
KERN_EXPORT(kern_geo_buildings_begin) u32 kern_geo_buildings_begin(const f64* stream, u32 length, const f64* o, u32 option_count) {
  if (!stream || !length || !o || option_count < 10 || !all_finite(o, option_count)) { fail("invalid building job arguments"); return 0; }
  u32 walls = 0, roofs = 0, max_buildings = 0;
  if (!stream_count(o[8], 4096, walls) || !stream_count(o[9], 4096, roofs) || !stream_count(o[1], 1e7, max_buildings) ||
      10 + (walls + roofs) * 3 > option_count) {
    fail("building options do not match their length");
    return 0;
  }
  auto j = std::make_unique<GeoJob>();
  j->kind = GeoJob::kBuildings;
  j->stream.assign(stream, stream + length);
  BuildingOptions opt;
  opt.chunk_size = o[0];
  opt.max_buildings = max_buildings;
  opt.cornice = o[2] != 0;
  opt.wall_records = o[3] != 0;
  opt.style.trim = rgb_at(o, 4);
  opt.style.trim_mix = o[7];
  opt.style.walls.clear();
  opt.style.roofs.clear();
  for (u32 i = 0; i < walls; ++i) opt.style.walls.push_back(rgb_at(o, 10 + i * 3));
  for (u32 i = 0; i < roofs; ++i) opt.style.roofs.push_back(rgb_at(o, 10 + walls * 3 + i * 3));
  if (!j->buildings.begin(j->stream.data(), j->stream.size(), opt)) return 0;
  return store(std::move(j));
}

/// options (f64): center x/y, radius, chunk_size, segment_length, curb_extra, curb_z, surface_z, marking_z,
///                marking_width, dash_start, dash_length, dash_period, uv_scale, curb rgb, palette_count, palette rgb...
KERN_EXPORT(kern_geo_roads_begin) u32 kern_geo_roads_begin(const f64* stream, u32 length, const f64* o, u32 option_count) {
  u32 palette = 0;
  if (!stream || !length || !o || option_count < 18 || !all_finite(o, option_count)) { fail("invalid road job arguments"); return 0; }
  if (!stream_count(o[17], 4096, palette) || 18 + palette * 3 > option_count) { fail("road options do not match their length"); return 0; }
  auto j = std::make_unique<GeoJob>();
  j->kind = GeoJob::kRoads;
  j->stream.assign(stream, stream + length);
  RoadOptions opt;
  opt.center_x = o[0]; opt.center_y = o[1]; opt.radius = o[2]; opt.chunk_size = o[3]; opt.segment_length = o[4];
  opt.curb_extra = o[5]; opt.curb_z = o[6]; opt.surface_z = o[7]; opt.marking_z = o[8]; opt.marking_width = o[9];
  opt.dash_start = o[10]; opt.dash_length = o[11]; opt.dash_period = o[12]; opt.uv_scale = o[13];
  opt.curb_color = rgb_at(o, 14);
  for (u32 i = 0; i < palette; ++i) opt.palette.push_back(rgb_at(o, 18 + i * 3));
  if (!j->roads.begin(j->stream.data(), j->stream.size(), opt, host_heights())) return 0;
  return store(std::move(j));
}

/// 1 finished, 0 more work, -1 failed (see kern_last_error).
KERN_EXPORT(kern_geo_step) i32 kern_geo_step(u32 id, f64 budget_ms) {
  GeoJob* j = job(id);
  if (!j) { fail("unknown geometry job %u", id); return -1; }
  if (j->kind == GeoJob::kBuildings) return j->buildings.step(budget_ms) ? 1 : 0;
  const bool done = j->roads.step(budget_ms);
  if (j->roads.failed()) return -1;
  return done ? 1 : 0;
}

/// out (f64): processed, built (buildings / roads), skipped, triangles, chunk_count
KERN_EXPORT(kern_geo_stats) i32 kern_geo_stats(u32 id, f64* out) {
  GeoJob* j = job(id);
  if (!j) return 0;
  if (j->kind == GeoJob::kBuildings) {
    out[0] = j->buildings.processed(); out[1] = j->buildings.built(); out[2] = j->buildings.skipped(); out[3] = j->buildings.triangles();
  } else {
    out[0] = j->roads.roads(); out[1] = j->roads.roads(); out[2] = 0; out[3] = j->roads.triangles();
  }
  out[4] = static_cast<f64>(j->chunks().size());
  return 1;
}

/// out (f64): grid x, grid y, items, triangles, content hash (of the soup)
KERN_EXPORT(kern_geo_chunk) i32 kern_geo_chunk(u32 id, u32 index, f64* out) {
  GeoJob* j = job(id);
  if (!j || index >= j->chunks().size()) return 0;
  GeoChunk& c = j->chunks()[index];
  if (!c.hash) c.hash = content_hash(c.mesh) | 1u;  // never 0 (0 = "no hash")
  out[0] = c.x; out[1] = c.y; out[2] = c.items; out[3] = c.triangles; out[4] = c.hash;
  return 1;
}

/// Welds chunk `index` and upserts it as mesh `mesh_id` with the material of
/// descriptor `desc` (KERN_MD_* words). out (f64): bytes before, bytes after welding.
KERN_EXPORT(kern_geo_commit) i32 kern_geo_commit(u32 id, u32 index, u32 mesh_id, const u32* desc, u32 extra_slot, f64* out) {
  GeoJob* j = job(id);
  if (!j || index >= j->chunks().size()) { fail("unknown geometry chunk"); return 0; }
  GeoChunk& c = j->chunks()[index];
  if (!c.hash) c.hash = content_hash(c.mesh) | 1u;
  WeldStats ws;
  if (!c.welded) { ws = weld(c.mesh); c.welded = true; }
  else ws.bytes_before = ws.bytes_after = c.mesh.bytes();
  MeshInput in = c.mesh.as_input(extra_slot);
  read_material(desc, in);
  if (!c_api_renderer().upsert(mesh_id, in)) return 0;
  if (out) { out[0] = static_cast<f64>(ws.bytes_before); out[1] = static_cast<f64>(ws.indexed ? ws.bytes_after : ws.bytes_before); }
  // The renderer holds its own copy now: release the chunk memory.
  c.mesh = MeshData{};
  return 1;
}

KERN_EXPORT(kern_geo_free) void kern_geo_free(u32 id) {
  if (id && id < g_jobs.size()) g_jobs[id].reset();
}

/// Builds terrain tile (x0, y0) with `cells` x `cells` quads of `step` metres
/// from the host height sampler and upserts it as `mesh_id`.
/// out (f64): min height, max height, bytes.
KERN_EXPORT(kern_geo_terrain_tile) i32 kern_geo_terrain_tile(u32 mesh_id, f64 x0, f64 y0, f64 step, u32 cells, f64 r, f64 g, f64 b,
                                                             const u32* desc, f64* out) {
  static MeshData tile;
  f64 lo = 0, hi = 0;
  if (!build_terrain_tile(x0, y0, step, cells, Rgb{r, g, b}, host_heights(), tile, &lo, &hi)) return 0;
  MeshInput in = tile.as_input();
  read_material(desc, in);
  if (!c_api_renderer().upsert(mesh_id, in)) return 0;
  if (out) { out[0] = lo; out[1] = hi; out[2] = static_cast<f64>(tile.bytes()); }
  return 1;
}

/// Read access to a chunk's (unwelded or welded) streams for tools and tests:
/// stream 0 positions, 1 normals, 2 colors, 3 uvs, 4 extra (f32), 5 indices (u32).
/// Returns the address and writes the element count to `count`.
KERN_EXPORT(kern_geo_chunk_stream) const void* kern_geo_chunk_stream(u32 id, u32 index, u32 stream, u32* count) {
  GeoJob* j = job(id);
  *count = 0;
  if (!j || index >= j->chunks().size()) return nullptr;
  const MeshData& m = j->chunks()[index].mesh;
  const std::vector<f32>* f[5] = {&m.positions, &m.normals, &m.colors, &m.uvs, &m.extra};
  if (stream < 5) { *count = static_cast<u32>(f[stream]->size()); return f[stream]->data(); }
  if (stream == 5) { *count = static_cast<u32>(m.indices.size()); return m.indices.data(); }
  return nullptr;
}

/// Welds chunk `index` in place without uploading (tools and tests).
KERN_EXPORT(kern_geo_weld) i32 kern_geo_weld(u32 id, u32 index) {
  GeoJob* j = job(id);
  if (!j || index >= j->chunks().size()) return 0;
  GeoChunk& c = j->chunks()[index];
  if (!c.hash) c.hash = content_hash(c.mesh) | 1u;
  if (!c.welded) { weld(c.mesh); c.welded = true; }
  return 1;
}

namespace {
MeshData g_probe;
}

/// Builds a terrain tile into a probe buffer without uploading (tools and tests).
KERN_EXPORT(kern_geo_terrain_probe) i32 kern_geo_terrain_probe(f64 x0, f64 y0, f64 step, u32 cells, f64 r, f64 g, f64 b) {
  return build_terrain_tile(x0, y0, step, cells, Rgb{r, g, b}, host_heights(), g_probe) ? 1 : 0;
}

/// Streams of the probe tile: 0 positions, 1 normals, 2 colors, 3 uvs (f32), 5 indices (u32).
KERN_EXPORT(kern_geo_terrain_probe_stream) const void* kern_geo_terrain_probe_stream(u32 stream, u32* count) {
  const std::vector<f32>* f[4] = {&g_probe.positions, &g_probe.normals, &g_probe.colors, &g_probe.uvs};
  if (stream < 4) { *count = static_cast<u32>(f[stream]->size()); return f[stream]->data(); }
  *count = static_cast<u32>(g_probe.indices.size());
  return g_probe.indices.data();
}
