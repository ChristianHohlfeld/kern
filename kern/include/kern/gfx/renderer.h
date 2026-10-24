// Kern scene renderer (WebGL2).
//
// Meshes are uploaded once (upsert) and afterwards only their instance
// streams change (update_instances), so per-frame CPU cost follows what moves.
// One standard program draws every mesh; games add looks through
// ShaderExtensions.
//
// Frame:
//   1. sun shadow map - redrawn only when a caster, the sun or the
//      texel-snapped focus changed
//   2. opaque meshes, front to back (early depth rejection); into the scene
//      target they also write their share of indirect light
//   3. sky at the far plane, only where nothing was drawn
//   4. transparent meshes, back to front by world bounds
//   5. GTAO from the scene depth (before the view-model clears it)
//   6. view-model layer (first-person items) after a depth clear
//   7. bloom/grade composite, which also applies the ambient occlusion
//
// Context loss: rendering pauses; context_restored() rebuilds every program
// and re-uploads all meshes and engine-owned textures from their CPU copies.
#pragma once

#include <memory>
#include <vector>

#include "kern/gfx/mesh.h"
#include "kern/gfx/passes.h"
#include "kern/gfx/program.h"
#include "kern/gfx/shader.h"
#include "kern/scene/frustum.h"
#include "kern/scene/picking.h"

namespace kern {

struct RendererOptions {
  i32 width = 960, height = 540;
  bool shadows = false;
  ShadowOptions shadow;
  BloomOptions bloom;
  AoOptions ao;
  bool grade = true;  // production colour grade after the tone map
};

/// Linear light colours (x intensity) and atmosphere. Defaults: production day.
struct Lighting {
  Vec3 sun_direction{-0.45f, -0.52f, 0.72f};
  Vec3 sun_color{2.8f, 2.47f, 2.11f};
  Vec3 sky_color{0.469f, 0.609f, 0.85f};
  Vec3 ground_color{0.125f, 0.094f, 0.054f};
  Vec3 fog_color{0.474f, 0.584f, 0.716f};
  f32 fog_density = 0.00085f;
  f32 exposure = 1;
  f32 env_intensity = 0.9f;
  Vec3 env_zenith{0.05f, 0.2f, 0.62f};
  Vec3 env_horizon{0.52f, 0.66f, 0.86f};
  Vec3 env_ground{0.105f, 0.127f, 0.084f};
  // Atmosphere. All 0 = the uniform FogExp2 of three.js (unchanged look).
  f32 fog_height_falloff = 0;  // per world unit (z up): fog thins with height, e.g. 0.012
  f32 fog_sun_scatter = 0;     // 0..1: fog brightens toward the sun (in-scattering)
  f32 fog_horizon = 0;         // 0..1: the sky fades into the fog colour at the horizon
};

struct PointLights {
  u32 count = 0;
  f32 pos_range[kMaxPointLights * 4] = {};        // xyz position, w range
  f32 color_intensity[kMaxPointLights * 4] = {};  // rgb colour, w intensity
};

struct Viewport {
  i32 x = 0, y = 0, width = 0, height = 0;
};

struct RenderParams {
  Vec4 clear_color{0.045f, 0.07f, 0.12f, 1};
  bool has_viewport = false;  // stereo eye / editor view: scissored sub-rectangle, no bloom
  Viewport viewport;
  bool page_hidden = false;
  u32 layers = ~0u;           // only meshes with (mesh.layers & layers) != 0 are drawn (and cast shadows)
  bool sky = true;            // false: the clear colour stays behind the scene
};

struct RenderStats {
  u32 draw_calls = 0, shadow_draw_calls = 0, shadow_resolution = 0;
  bool ao = false;           // ambient occlusion applied this frame
  bool shadow_reused = false;
  f64 triangles = 0;
  u32 instances = 0, culled_meshes = 0, visible_meshes = 0;
  f64 cpu_ms = 0, gpu_ms = -1;  // gpu_ms < 0: not available
  BloomState bloom = BloomState::off;
};

struct TextureDesc {
  const u8* rgba = nullptr;  // width * height * 4 bytes
  i32 width = 0, height = 0;
  bool mipmaps = true;
  bool repeat = false;
  bool nearest = false;  // nearest-neighbour filtering (pixel art), no mipmaps
};

class Renderer {
 public:
  Renderer();
  ~Renderer();
  Renderer(const Renderer&) = delete;
  Renderer& operator=(const Renderer&) = delete;

  /// Extensions must outlive the renderer.
  bool init(const RendererOptions& options, Span<ShaderExtension* const> extensions = {});
  void shutdown();
  bool resize(i32 width, i32 height);
  i32 width() const { return width_; }
  i32 height() const { return height_; }

  // ---- meshes ----
  /// A free mesh id (> 0). Ids are recycled after remove(); upsert() only
  /// accepts ids obtained here, so two owners can never share a slot.
  u32 allocate_id();
  /// Returns an allocated id that never received a mesh (failed first upsert).
  void release_id(u32 id);
  bool id_allocated(u32 id) const { return id < allocated_.size() && allocated_[id]; }
  /// Creates or atomically replaces mesh `id` (validation happens before
  /// anything changes; on failure the previous mesh stays drawn).
  bool upsert(u32 id, const MeshInput& input);
  /// Replaces the instance transforms (16 floats each; empty = keep the current
  /// instances, i.e. a tint-only update) and tints (4 floats each per instance,
  /// anything else = white). `changed` reports whether anything differed.
  bool update_instances(u32 id, Span<const f32> transforms, Span<const f32> tints, bool* changed = nullptr);
  bool update_bones(u32 id, Span<const f32> bones);
  bool set_visible(u32 id, bool visible);
  bool remove(u32 id);
  /// Updates the material of a live mesh without re-uploading geometry.
  bool set_material(u32 id, const Material& material);
  /// Moves a live mesh to other view layers.
  bool set_layers(u32 id, u32 layers);
  Mesh* mesh(u32 id) const { return id < slots_.size() ? slots_[id].get() : nullptr; }
  Span<Mesh* const> meshes() const { return {live_.data(), live_.size()}; }
  u32 mesh_count() const { return static_cast<u32>(live_.size()); }

  // ---- textures ----
  /// Engine-owned RGBA8 texture, re-created automatically after context loss.
  u32 create_texture(const TextureDesc& desc);
  /// Registers a texture whose GPU object the host owns (and restores in place).
  u32 register_external_texture(u32 gl_handle);
  bool delete_texture(u32 texture);

  // ---- scene state ----
  Camera camera;
  Lighting lighting;
  SkyState sky;
  bool set_point_lights(const PointLights& lights);
  const PointLights& point_lights() const { return point_lights_; }
  bool set_shadows(const ShadowOptions* options);  // nullptr disables
  /// Shadow distance in metres (0 = the shadow map's fixed extent around the camera target): the map
  /// covers the bounding sphere of the view up to this distance - its size does not change as the
  /// camera turns, and its centre snaps to texels, so edges do not shimmer (stable shadow maps).
  void set_shadow_distance(f32 metres) { shadow_distance_ = std::isfinite(metres) && metres > 0 ? metres : 0; }
  /// 1, 4 or 9 PCF taps (phones: 4, desktop: 9).
  void set_shadow_filter(i32 taps) { shadow_options_.filter = taps; if (shadow_) shadow_->set_filter(taps); }
  void set_ambient_occlusion(const AoOptions& options) { ao_.configure(options); }
  /// The scene's occlusion radius in world units (0 = none); keeps the device's quality settings.
  void set_ambient_occlusion_radius(f32 radius) { ao_.set_radius(radius); }
  const AmbientOcclusionPass& ambient_occlusion() const { return ao_; }
  const SunShadowMap* shadow() const { return shadow_ ? &*shadow_ : nullptr; }
  /// Marks the scene changed; `shadows` also invalidates the cached shadow map.
  void invalidate(bool shadows) { ++scene_revision_; if (shadows) ++shadow_revision_; }
  u64 scene_revision() const { return scene_revision_; }

  // ---- frame ----
  bool render(const RenderParams& params, RenderStats& stats);
  const RenderStats& last_stats() const { return stats_; }

  // ---- queries ----
  bool screen_ray(f32 x, f32 y, Ray& ray) const;
  bool pick(f32 x, f32 y, const PickQuery& query, PickHit& hit, PickStats* stats = nullptr) const;
  bool pick_ray(const Ray& ray, const PickQuery& query, PickHit& hit, PickStats* stats = nullptr) const;

  // ---- context loss ----
  void context_lost();
  bool context_restored();
  bool lost() const { return lost_; }
  /// No GPU at all (a headless simulation, e.g. the Lehrer): meshes stay on the CPU and nothing
  /// touches GL, as while the context is lost. Call instead of init().
  void set_headless() { if (!initialised_) lost_ = true; }

  Span<ShaderExtension* const> extensions() const { return {extensions_.data(), extensions_.size()}; }
  /// Names of all extension material parameters in index order.
  const std::vector<const char*>& material_keys() const { return material_keys_; }
  /// Keys of the extension vertex streams in MeshInput::extension order.
  const std::vector<const ExtensionAttribute*>& extension_attributes() const { return extension_attributes_; }

 private:
  struct Texture {
    u32 handle = 0;
    bool external = false, alive = false;
    TextureDesc desc;
    std::vector<u8> pixels;
  };

  bool init_gpu();
  void upload(Mesh& m);
  void free_gpu(Mesh& m);
  void upload_texture(Texture& t);
  bool build_instances(Span<const f32> transforms, Span<const f32> tints, std::vector<f32>& out_t, std::vector<f32>& out_tint,
                       std::vector<f32>& out_normals, u32& count) const;
  void write_instance_streams(Mesh& m, u32 mask = 7);  // bit 0 transforms, 1 tints, 2 normal matrices
  void set_frame_uniforms(const Mat4& view_projection);
  void draw_mesh(const Mesh& m, RenderStats& stats);
  void touch(const Mesh& m) { ++scene_revision_; if (m.material.casts_shadow()) ++shadow_revision_; }
  u32 texture_handle(u32 id) const { return id && id < textures_.size() && textures_[id].alive ? textures_[id].handle : 0; }

  RendererOptions options_;
  i32 width_ = 0, height_ = 0;
  bool initialised_ = false, lost_ = false;
  std::vector<ShaderExtension*> extensions_;
  std::vector<const char*> material_keys_;
  std::vector<const ExtensionAttribute*> extension_attributes_;

  std::vector<std::unique_ptr<Mesh>> slots_;
  std::vector<Mesh*> live_;
  std::vector<u32> free_ids_;
  std::vector<u8> allocated_;
  std::vector<f32> scratch_transforms_, scratch_tints_, scratch_normals_;  // per slot: handed out by allocate_id() and not yet released
  std::vector<Texture> textures_;

  Program program_;
  UniformCache cache_;
  struct Locations {
    u32 view_projection, material, emissive, alpha_cutoff, unlit, linear_color, pbr, has_texture, texture, skin_enabled, bones;
    u32 sun_direction, sun_color, sky_color, ground_color, fog_color, eye, env_zenith, env_horizon, env_ground;
    u32 fog_density, exposure, env_intensity, grade, fog_height_falloff, fog_sun_scatter;
    u32 point_count, point_pos_range, point_color_intensity;
    u32 shadow_enabled, shadow_map, shadow_projection, shadow_bias, shadow_filter, shadow_texel, shadow_fade;
    u32 ao_out;
  } u_{};

  std::unique_ptr<SunShadowMap> shadow_;
  bool shadow_requested_ = false;
  f32 shadow_distance_ = 0;
  f32 shadow_fit_radius_ = 0;
  Vec3 shadow_fit_eye_;
  ShadowOptions shadow_options_;
  u64 shadow_key_ = 0;
  bool shadow_key_valid_ = false;
  SkyPass sky_pass_;
  BloomPass bloom_;
  AmbientOcclusionPass ao_;
  PointLights point_lights_;
  u64 scene_revision_ = 0, shadow_revision_ = 0;

  // GPU timer
  bool timer_supported_ = false;
  std::vector<u32> pending_queries_, free_queries_;
  f64 last_gpu_ms_ = -1;

  // per-frame scratch, reused
  std::vector<Mesh*> opaque_, transparent_, viewmodel_;
  u32 bound_texture_ = ~0u;
  i32 cull_state_ = -1;
  RenderStats stats_;
  std::vector<u16> index16_scratch_;
};

}  // namespace kern
