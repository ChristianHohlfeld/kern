// C ABI implementation over the process-wide Renderer.
#include "kern/api/c_api.h"

#include <cstdlib>
#include <vector>

#include "kern/geo/mesh_data.h"
#include "kern/gfx/renderer.h"

using namespace kern;

extern "C" __attribute__((weak)) u32 kern_game_extensions(ShaderExtension*** out) {
  *out = nullptr;
  return 0;
}

namespace kern {
Renderer& c_api_renderer() {
  static Renderer renderer;
  return renderer;
}
}  // namespace kern

namespace {

Renderer& R() { return c_api_renderer(); }

template <class T>
Span<const T> span_at(const u32* d, u32 slot) {
  return Span<const T>(reinterpret_cast<const T*>(static_cast<std::uintptr_t>(d[slot])), d[slot + 1]);
}
f32 f32_at(const u32* d, u32 slot) {
  f32 v;
  std::memcpy(&v, d + slot, 4);
  return v;
}
Vec3 vec3_at(const f32* f, u32 slot) { return {f[slot], f[slot + 1], f[slot + 2]}; }
void store3(f32* f, u32 slot, Vec3 v) { f[slot] = v.x; f[slot + 1] = v.y; f[slot + 2] = v.z; }

std::vector<u32> g_index_scratch;

}  // namespace

namespace kern {
void read_material(const u32* d, MeshInput& in) {
  in.texture = d[KERN_MD_TEXTURE];
  const u32 flags = d[KERN_MD_FLAGS];
  in.visible = flags & KERN_MF_VISIBLE;
  Material& m = in.material;
  m.unlit = flags & KERN_MF_UNLIT;
  m.double_sided = flags & KERN_MF_DOUBLE_SIDED;
  m.cast_shadow = flags & KERN_MF_CAST_SHADOW;
  m.frustum_culled = flags & KERN_MF_FRUSTUM_CULLED;
  m.viewmodel = flags & KERN_MF_VIEWMODEL;
  m.linear_colors = flags & KERN_MF_LINEAR_COLORS;
  m.alpha = static_cast<AlphaMode>(min_of<u32>(d[KERN_MD_ALPHA_MODE], 2));
  m.color = {f32_at(d, KERN_MD_COLOR), f32_at(d, KERN_MD_COLOR + 1), f32_at(d, KERN_MD_COLOR + 2), f32_at(d, KERN_MD_COLOR + 3)};
  m.alpha_cutoff = f32_at(d, KERN_MD_ALPHA_CUTOFF);
  m.roughness = f32_at(d, KERN_MD_ROUGHNESS);
  m.metalness = f32_at(d, KERN_MD_METALNESS);
  for (u32 i = 0; i < kMaxMaterialParams; ++i) m.params[i] = static_cast<i32>(d[KERN_MD_PARAMS + i]);
  m.emissive = {f32_at(d, KERN_MD_EMISSIVE), f32_at(d, KERN_MD_EMISSIVE + 1), f32_at(d, KERN_MD_EMISSIVE + 2)};
}
}  // namespace kern

namespace {

void write_pick(const PickHit& hit, const PickStats& stats, bool found, f64* out) {
  for (u32 i = 0; i < KERN_PK_SLOTS; ++i) out[i] = 0;
  out[KERN_PK_TRIANGLES_EXAMINED] = stats.triangles_examined;
  out[KERN_PK_BOUNDS_REJECTED] = stats.bounds_rejected;
  if (!found) return;
  out[KERN_PK_MESH] = hit.mesh;
  out[KERN_PK_INSTANCE] = hit.instance;
  out[KERN_PK_TRIANGLE] = hit.triangle;
  out[KERN_PK_DISTANCE] = hit.distance;
  for (int k = 0; k < 3; ++k) {
    out[KERN_PK_POINT + k] = hit.point[k];
    out[KERN_PK_NORMAL + k] = hit.normal[k];
    out[KERN_PK_BARYCENTRIC + k] = hit.barycentric[k];
  }
}

}  // namespace

// ---- memory & errors ----
KERN_EXPORT(kern_alloc) void* kern_alloc(u32 bytes) { return std::malloc(bytes ? bytes : 1); }
KERN_EXPORT(kern_free) void kern_free(void* p) { std::free(p); }
KERN_EXPORT(kern_last_error) const char* kern_last_error() { return last_error(); }
KERN_EXPORT(kern_clear_error) void kern_clear_error() { clear_error(); }

// ---- renderer ----
KERN_EXPORT(kern_renderer_init) i32 kern_renderer_init(const u32* d) {
  RendererOptions o;
  o.width = static_cast<i32>(d[KERN_ID_WIDTH]);
  o.height = static_cast<i32>(d[KERN_ID_HEIGHT]);
  o.shadows = d[KERN_ID_SHADOWS] != 0;
  o.shadow.resolution = static_cast<i32>(d[KERN_ID_SHADOW_RESOLUTION]);
  o.shadow.extent = f32_at(d, KERN_ID_SHADOW_EXTENT);
  o.shadow.bias = f32_at(d, KERN_ID_SHADOW_BIAS);
  o.bloom.enabled = d[KERN_ID_BLOOM] != 0;
  o.bloom.forced = d[KERN_ID_BLOOM_FORCED] != 0;
  o.grade = d[KERN_ID_GRADE] != 0;
  o.bloom.grade = o.grade;
  ShaderExtension** list = nullptr;
  const u32 n = kern_game_extensions(&list);
  return R().init(o, Span<ShaderExtension* const>(list, n)) ? 1 : 0;
}
KERN_EXPORT(kern_renderer_shutdown) void kern_renderer_shutdown() { R().shutdown(); }
KERN_EXPORT(kern_renderer_resize) i32 kern_renderer_resize(i32 w, i32 h) { return R().resize(w, h) ? 1 : 0; }
KERN_EXPORT(kern_material_key_count) u32 kern_material_key_count() { return static_cast<u32>(R().material_keys().size()); }
KERN_EXPORT(kern_material_key) const char* kern_material_key(u32 i) {
  return i < R().material_keys().size() ? R().material_keys()[i] : "";
}
KERN_EXPORT(kern_extension_attribute_count) u32 kern_extension_attribute_count() {
  return static_cast<u32>(R().extension_attributes().size());
}
KERN_EXPORT(kern_extension_attribute_key) const char* kern_extension_attribute_key(u32 i) {
  return i < R().extension_attributes().size() ? R().extension_attributes()[i]->key : "";
}
KERN_EXPORT(kern_extension_attribute_size) u32 kern_extension_attribute_size(u32 i) {
  return i < R().extension_attributes().size() ? R().extension_attributes()[i]->size : 0;
}

// ---- meshes ----
KERN_EXPORT(kern_mesh_alloc_id) u32 kern_mesh_alloc_id() { return R().allocate_id(); }
KERN_EXPORT(kern_mesh_release_id) void kern_mesh_release_id(u32 id) { R().release_id(id); }

KERN_EXPORT(kern_mesh_upsert) i32 kern_mesh_upsert(u32 id, const u32* d) {
  MeshInput in;
  in.positions = span_at<f32>(d, KERN_MD_POSITIONS);
  in.normals = span_at<f32>(d, KERN_MD_NORMALS);
  in.colors = span_at<f32>(d, KERN_MD_COLORS);
  in.uvs = span_at<f32>(d, KERN_MD_UVS);
  if (d[KERN_MD_INDICES + 1]) {
    if (d[KERN_MD_INDEX_BYTES] == 4) {
      in.indices = span_at<u32>(d, KERN_MD_INDICES);
    } else {
      const Span<const u16> i16 = span_at<u16>(d, KERN_MD_INDICES);
      g_index_scratch.assign(i16.begin(), i16.end());
      in.indices = Span<const u32>(g_index_scratch.data(), g_index_scratch.size());
    }
  }
  in.joints = span_at<f32>(d, KERN_MD_JOINTS);
  in.weights = span_at<f32>(d, KERN_MD_WEIGHTS);
  in.bones = span_at<f32>(d, KERN_MD_BONES);
  in.extension[0] = span_at<f32>(d, KERN_MD_EXT0);
  in.extension[1] = span_at<f32>(d, KERN_MD_EXT1);
  in.transforms = span_at<f32>(d, KERN_MD_TRANSFORMS);
  in.tints = span_at<f32>(d, KERN_MD_TINTS);
  read_material(d, in);
  bool ok;
  if ((d[KERN_MD_FLAGS] & KERN_MF_WELD) && in.indices.empty() && in.bones.empty() && !in.positions.empty()) {
    // Lossless welding of a triangle soup inside the engine (no JS-side packing).
    MeshData soup;
    soup.positions.assign(in.positions.begin(), in.positions.end());
    const std::size_t p = in.positions.size;
    if (in.normals.size == p) soup.normals.assign(in.normals.begin(), in.normals.end());
    if (in.colors.size == p) soup.colors.assign(in.colors.begin(), in.colors.end()); else soup.colors.assign(p, 1.0f);
    if (in.uvs.size == p / 3 * 2) soup.uvs.assign(in.uvs.begin(), in.uvs.end()); else soup.uvs.assign(p / 3 * 2, 0.0f);
    u32 slot = kMaxExtensionAttributes;
    for (u32 a = 0; a < kMaxExtensionAttributes; ++a)
      if (!in.extension[a].empty() && slot == kMaxExtensionAttributes) slot = a;
    if (slot < kMaxExtensionAttributes && in.extension[slot].size % (p / 3) == 0) {
      soup.extra_size = static_cast<u32>(in.extension[slot].size / (p / 3));
      soup.extra.assign(in.extension[slot].begin(), in.extension[slot].end());
    }
    if (soup.normals.empty()) {
      ok = R().upsert(id, in);  // flat normals are generated per face; welding would not merge anything
    } else {
      weld(soup);
      MeshInput welded = soup.as_input(slot < kMaxExtensionAttributes ? slot : 0);
      welded.transforms = in.transforms;
      welded.tints = in.tints;
      welded.material = in.material;
      welded.texture = in.texture;
      welded.visible = in.visible;
      ok = R().upsert(id, welded);
    }
  } else {
    ok = R().upsert(id, in);
  }
  if (g_index_scratch.capacity() > (1u << 22)) std::vector<u32>().swap(g_index_scratch);
  return ok ? 1 : 0;
}

/// Returns -1 on error, 0 when unchanged, 1 when the instances changed.
KERN_EXPORT(kern_mesh_instances) i32 kern_mesh_instances(u32 id, const f32* t, u32 t_count, const f32* tint, u32 tint_count) {
  bool changed = false;
  if (!R().update_instances(id, Span<const f32>(t, t_count), Span<const f32>(tint, tint_count), &changed)) return -1;
  return changed ? 1 : 0;
}
KERN_EXPORT(kern_mesh_bones) i32 kern_mesh_bones(u32 id, const f32* bones, u32 count) {
  return R().update_bones(id, Span<const f32>(bones, count)) ? 1 : 0;
}
KERN_EXPORT(kern_mesh_visible) i32 kern_mesh_visible(u32 id, i32 visible) { return R().set_visible(id, visible != 0) ? 1 : 0; }
KERN_EXPORT(kern_mesh_remove) i32 kern_mesh_remove(u32 id) { return R().remove(id) ? 1 : 0; }
KERN_EXPORT(kern_mesh_exists) i32 kern_mesh_exists(u32 id) { return R().mesh(id) ? 1 : 0; }
KERN_EXPORT(kern_mesh_count) u32 kern_mesh_count() { return R().mesh_count(); }
/// out: vertexCount, triangleCount, instanceCount, visible, boundsCenter xyz, boundsRadius (f64 x 8)
KERN_EXPORT(kern_mesh_info) i32 kern_mesh_info(u32 id, f64* out) {
  const Mesh* m = R().mesh(id);
  if (!m) return 0;
  out[0] = m->vertex_count(); out[1] = m->triangle_count(); out[2] = m->instance_count; out[3] = m->visible ? 1 : 0;
  out[4] = m->world_bounds.center.x; out[5] = m->world_bounds.center.y; out[6] = m->world_bounds.center.z; out[7] = m->world_bounds.radius;
  return 1;
}

/// Read access to a live mesh's CPU streams (tools and tests): 0 positions,
/// 1 normals, 2 colors, 3 uvs, 4 transforms, 5 tints (f32), 6 indices (u32).
KERN_EXPORT(kern_mesh_stream) const void* kern_mesh_stream(u32 id, u32 stream, u32* count) {
  *count = 0;
  const Mesh* m = R().mesh(id);
  if (!m) return nullptr;
  const std::vector<f32>* f[6] = {&m->positions, &m->normals, &m->colors, &m->uvs, &m->transforms, &m->tints};
  if (stream < 6) { *count = static_cast<u32>(f[stream]->size()); return f[stream]->data(); }
  if (stream == 6) { *count = static_cast<u32>(m->indices.size()); return m->indices.data(); }
  return nullptr;
}

// ---- textures ----
KERN_EXPORT(kern_texture_external) u32 kern_texture_external(u32 handle) { return R().register_external_texture(handle); }
KERN_EXPORT(kern_texture_create) u32 kern_texture_create(const u8* rgba, i32 w, i32 h, i32 mipmaps, i32 repeat) {
  return R().create_texture({rgba, w, h, mipmaps != 0, repeat != 0});
}
KERN_EXPORT(kern_texture_delete) i32 kern_texture_delete(u32 id) { return R().delete_texture(id) ? 1 : 0; }

// ---- scene state ----
/// eye xyz, target xyz, up xyz, fov, near, far
KERN_EXPORT(kern_camera_set) i32 kern_camera_set(const f32* c) {
  Camera cam{vec3_at(c, 0), vec3_at(c, 3), vec3_at(c, 6), c[9], c[10], c[11]};
  if (!finite(cam.eye) || !finite(cam.target) || !finite(cam.up) || !(cam.fov > 0 && cam.fov < kPi) || !(cam.near > 0) ||
      !(cam.far > cam.near)) {
    fail("invalid camera");
    return 0;
  }
  R().camera = cam;
  return 1;
}
KERN_EXPORT(kern_lighting_set) void kern_lighting_set(const f32* f) {
  Lighting& l = R().lighting;
  l.sun_direction = vec3_at(f, KERN_LI_SUN_DIRECTION);
  l.sun_color = vec3_at(f, KERN_LI_SUN_COLOR);
  l.sky_color = vec3_at(f, KERN_LI_SKY_COLOR);
  l.ground_color = vec3_at(f, KERN_LI_GROUND_COLOR);
  l.fog_color = vec3_at(f, KERN_LI_FOG_COLOR);
  l.fog_density = f[KERN_LI_FOG_DENSITY];
  l.exposure = f[KERN_LI_EXPOSURE];
  l.env_intensity = f[KERN_LI_ENV_INTENSITY];
  l.env_zenith = vec3_at(f, KERN_LI_ENV_ZENITH);
  l.env_horizon = vec3_at(f, KERN_LI_ENV_HORIZON);
  l.env_ground = vec3_at(f, KERN_LI_ENV_GROUND);
}
KERN_EXPORT(kern_lighting_get) void kern_lighting_get(f32* f) {
  const Lighting& l = R().lighting;
  store3(f, KERN_LI_SUN_DIRECTION, l.sun_direction);
  store3(f, KERN_LI_SUN_COLOR, l.sun_color);
  store3(f, KERN_LI_SKY_COLOR, l.sky_color);
  store3(f, KERN_LI_GROUND_COLOR, l.ground_color);
  store3(f, KERN_LI_FOG_COLOR, l.fog_color);
  f[KERN_LI_FOG_DENSITY] = l.fog_density;
  f[KERN_LI_EXPOSURE] = l.exposure;
  f[KERN_LI_ENV_INTENSITY] = l.env_intensity;
  store3(f, KERN_LI_ENV_ZENITH, l.env_zenith);
  store3(f, KERN_LI_ENV_HORIZON, l.env_horizon);
  store3(f, KERN_LI_ENV_GROUND, l.env_ground);
}
KERN_EXPORT(kern_sky_set) void kern_sky_set(const f32* f) {
  SkyState& s = R().sky;
  s.sun = vec3_at(f, KERN_SK_SUN);
  s.moon = vec3_at(f, KERN_SK_MOON);
  s.night = f[KERN_SK_NIGHT];
  s.clouds = f[KERN_SK_CLOUDS] > 0.5f;
  s.blast_direction = vec3_at(f, KERN_SK_BLAST_DIRECTION);
  s.blast_age = f[KERN_SK_BLAST_AGE];
  s.wind = {f[KERN_SK_WIND], f[KERN_SK_WIND + 1]};
}
KERN_EXPORT(kern_point_lights_set) i32 kern_point_lights_set(u32 count, const f32* pos_range, const f32* color_intensity) {
  PointLights l;
  l.count = count;
  std::memcpy(l.pos_range, pos_range, sizeof l.pos_range);
  std::memcpy(l.color_intensity, color_intensity, sizeof l.color_intensity);
  return R().set_point_lights(l) ? 1 : 0;
}
KERN_EXPORT(kern_shadows_set) i32 kern_shadows_set(i32 enabled, i32 resolution, f32 extent, f32 bias) {
  if (!enabled) return R().set_shadows(nullptr) ? 1 : 0;
  const ShadowOptions o{resolution, extent, bias};
  return R().set_shadows(&o) ? 1 : 0;
}
KERN_EXPORT(kern_invalidate) void kern_invalidate(i32 shadows) { R().invalidate(shadows != 0); }

// ---- frame ----
KERN_EXPORT(kern_render) i32 kern_render(const u32* p, f64* out) {
  RenderParams params;
  params.clear_color = {f32_at(p, KERN_RP_CLEAR), f32_at(p, KERN_RP_CLEAR + 1), f32_at(p, KERN_RP_CLEAR + 2), f32_at(p, KERN_RP_CLEAR + 3)};
  params.has_viewport = p[KERN_RP_HAS_VIEWPORT] != 0;
  params.viewport = {static_cast<i32>(p[KERN_RP_X]), static_cast<i32>(p[KERN_RP_Y]), static_cast<i32>(p[KERN_RP_W]),
                     static_cast<i32>(p[KERN_RP_H])};
  params.page_hidden = p[KERN_RP_PAGE_HIDDEN] != 0;
  RenderStats s;
  if (!R().render(params, s)) return 0;
  out[KERN_RS_DRAW_CALLS] = s.draw_calls;
  out[KERN_RS_SHADOW_DRAW_CALLS] = s.shadow_draw_calls;
  out[KERN_RS_SHADOW_RESOLUTION] = s.shadow_resolution;
  out[KERN_RS_SHADOW_REUSED] = s.shadow_reused ? 1 : 0;
  out[KERN_RS_TRIANGLES] = s.triangles;
  out[KERN_RS_INSTANCES] = s.instances;
  out[KERN_RS_CULLED] = s.culled_meshes;
  out[KERN_RS_CPU_MS] = s.cpu_ms;
  out[KERN_RS_GPU_MS] = s.gpu_ms;
  out[KERN_RS_BLOOM] = static_cast<f64>(s.bloom);
  out[KERN_RS_VISIBLE] = s.visible_meshes;
  return 1;
}

// ---- queries ----
/// out: origin xyz, direction xyz, near, far (f64 x 8)
KERN_EXPORT(kern_screen_ray) i32 kern_screen_ray(f32 x, f32 y, f64* out) {
  Ray r;
  if (!R().screen_ray(x, y, r)) return 0;
  for (int k = 0; k < 3; ++k) { out[k] = r.origin[k]; out[3 + k] = r.direction[k]; }
  out[6] = r.near;
  out[7] = r.far;
  return 1;
}
/// Screen ray for an explicit camera (eye, target, up, fov, near, far: f32 x 12) and viewport size.
KERN_EXPORT(kern_camera_screen_ray) i32 kern_camera_screen_ray(const f32* c, f32 x, f32 y, f32 w, f32 h, f64* out) {
  const Camera cam{vec3_at(c, 0), vec3_at(c, 3), vec3_at(c, 6), c[9], c[10], c[11]};
  Ray r;
  if (!screen_ray(cam, x, y, w, h, r)) return 0;
  for (int k = 0; k < 3; ++k) { out[k] = r.origin[k]; out[3 + k] = r.direction[k]; }
  out[6] = r.near;
  out[7] = r.far;
  return 1;
}
KERN_EXPORT(kern_pick) i32 kern_pick(f32 x, f32 y, f32 max_distance, u32 ignore_id, f64* out) {
  PickHit hit;
  PickStats stats;
  const bool found = R().pick(x, y, {max_distance, ignore_id, false}, hit, &stats);
  write_pick(hit, stats, found, out);
  return found ? 1 : 0;
}
/// ray: origin xyz, direction xyz (f32 x 6)
KERN_EXPORT(kern_pick_ray) i32 kern_pick_ray(const f32* ray, f32 max_distance, u32 ignore_id, f64* out) {
  Ray r;
  r.origin = vec3_at(ray, 0);
  r.direction = vec3_at(ray, 3);
  PickHit hit;
  PickStats stats;
  const bool found = R().pick_ray(r, {max_distance, ignore_id, false}, hit, &stats);
  write_pick(hit, stats, found, out);
  return found ? 1 : 0;
}

// ---- context loss ----
KERN_EXPORT(kern_context_lost) void kern_context_lost() { R().context_lost(); }
KERN_EXPORT(kern_context_restored) i32 kern_context_restored() { return R().context_restored() ? 1 : 0; }
