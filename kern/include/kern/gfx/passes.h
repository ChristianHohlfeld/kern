// Auxiliary GPU passes of the renderer: sun shadow map, procedural sky and
// the HDR bloom composite.
#pragma once

#include <cmath>
#include <vector>

#include "kern/gfx/mesh.h"
#include "kern/gfx/program.h"
#include "kern/scene/frustum.h"

namespace kern {

// ---------------------------------------------------------------------------
// Sun shadow map: orthographic depth map with hardware PCF
// (COMPARE_REF_TO_TEXTURE). The focus is snapped to the light-space texel
// grid, so the map does not shimmer while the camera moves and the renderer
// can reuse it until the focus crosses a texel or a shadow caster changes.

struct ShadowOptions {
  i32 resolution = 512;  // power of two, 128..4096
  f32 extent = 120;      // half size of the covered square in metres (without a shadow distance)
  f32 bias = 0.0012f;
  // Percentage-closer filtering: 1 hardware 2x2 tap, 4 taps (3x3 kernel) or 9 taps (5x5 kernel),
  // Castano's optimised PCF (bilinear-weighted comparison taps).
  i32 filter = 9;
};

class SunShadowMap {
 public:
  bool init(const ShadowOptions& options);
  void destroy(bool contextLost);
  bool valid() const { return fbo_ != 0; }

  /// Snaps `focus` for `sun` and returns a cache key of the resulting map. `extent` (half size in
  /// metres) overrides the options' when > 0.
  u64 focus_key(Vec3 focus, Vec3 sun, f32 extent = 0);
  /// Renders the casters among `meshes`; returns the number of draw calls.
  u32 draw(Span<Mesh* const> meshes, Vec3 focus, Vec3 sun, u32 layers = ~0u, f32 extent = 0);

  const Mat4& matrix() const { return matrix_; }
  u32 depth_texture() const { return depth_; }
  const ShadowOptions& options() const { return options_; }
  void set_filter(i32 taps) { options_.filter = taps >= 9 ? 9 : taps >= 4 ? 4 : 1; }
  /// Half size of the square the map covers now (metres).
  f32 extent() const { return extent_; }

 private:
  ShadowOptions options_;
  Program program_;
  u32 u_matrix_ = 0, u_bones_ = 0, u_skinned_ = 0;
  u32 depth_ = 0, fbo_ = 0;
  Mat4 matrix_;
  Vec3 snapped_focus_;
  f32 extent_ = 0;
  f32 extent_for(f32 e) const { return e > 0 ? e : options_.extent; }
};

// ---------------------------------------------------------------------------
// Procedural sky (production model): Rayleigh-like gradient, sun disc and Mie
// glow, golden-hour band, moon and stars at night and a drifting 5-octave
// fBm cloud layer. Drawn as a fullscreen triangle at the far plane AFTER the
// opaque scene, so only uncovered pixels run the sky shader.

struct SkyState {
  Vec3 sun{-0.45f, -0.52f, 0.72f};
  Vec3 moon{0.4f, 0.5f, 0.76f};
  f32 night = 0;
  bool clouds = true;
  Vec3 blast_direction{0, 0, -1};
  f32 blast_age = -1;          // < 0: no shock wave in the clouds
  Vec2 wind{0.012f, 0.004f};
};

/// The scene fog as seen by the sky, so distant geometry and the horizon meet.
struct SkyAtmosphere {
  Vec3 fog_color{0.474f, 0.584f, 0.716f};  // sRGB, as Lighting::fog_color
  Vec3 sun_color{2.8f, 2.47f, 2.11f};
  Vec3 sun_direction{-0.45f, -0.52f, 0.72f};
  f32 sun_scatter = 0;  // 0..1, Lighting::fog_sun_scatter
  f32 horizon = 0;      // 0..1, Lighting::fog_horizon; 0 = sky as before
};

class SkyPass {
 public:
  bool init();
  void destroy(bool contextLost);
  void draw(const Camera& camera, f32 aspect, const SkyState& state, f32 exposure, f64 time_seconds,
            const SkyAtmosphere& atmosphere = {});

 private:
  Program program_;
  u32 vao_ = 0;
  u32 u_forward_ = 0, u_right_ = 0, u_up_ = 0, u_tan_half_ = 0, u_night_ = 0, u_sun_up_ = 0, u_time_ = 0;
  u32 u_clouds_ = 0, u_exposure_ = 0, u_blast_age_ = 0, u_sun_ = 0, u_moon_ = 0, u_blast_dir_ = 0, u_wind_ = 0;
  u32 u_fog_color_ = 0, u_sun_color_ = 0, u_sun_light_ = 0, u_fog_sun_scatter_ = 0, u_fog_horizon_ = 0;
  UniformCache cache_;
};

// ---------------------------------------------------------------------------
// Bloom: the scene pass renders into an RGBA8 target plus a linear RGBA16F
// copy (MRT). Bright parts (threshold .88, soft knee .35) are extracted at
// quarter resolution, blurred on two levels (1/4 and 1/8) with a 9-tap
// kernel and composited as grade(hdr + bloom) - grade(hdr) on top of the
// finished picture. A frame-time guard switches the glow off for the session
// on devices that cannot keep up (avg > 27 ms for 3.5 s) unless forced.

struct BloomOptions {
  bool enabled = true;
  bool forced = false;   // never auto-disable
  bool grade = true;     // composite through the production grade
};

enum class BloomState : u8 { off = 0, active = 1, disabled_slow_device = 2, unsupported = 3 };

class BloomPass {
 public:
  void configure(const BloomOptions& options) { options_ = options; }
  /// Binds the scene target for (width, height); false = render directly to the screen.
  bool begin(i32 width, i32 height, f64 now_ms, bool page_hidden);
  /// Ambient occlusion for the composite: the GTAO target (visibility, view depth) and how it maps to the scene.
  struct Occlusion {
    u32 texture = 0;
    f32 scale_x = 1, scale_y = 1;  // AO size / scene size
    Mat4 projection;
  };
  /// Composites to the screen; `occlusion` darkens the indirect share the scene recorded in its third target.
  void end(f32 exposure, const Occlusion* occlusion = nullptr);
  void destroy(bool contextLost);
  BloomState state() const;
  /// The scene target's depth (DEPTH_COMPONENT24 texture, nearest) and rebinding it after other passes.
  u32 depth_texture() const { return main_.depth; }
  void bind_scene() const;

 private:
  struct Target { u32 texture = 0, framebuffer = 0, hdr = 0, depth = 0, ao = 0; i32 w = 0, h = 0; };
  bool init_programs();
  bool resize(i32 w, i32 h);
  void free_targets();
  Target make_target(i32 w, i32 h, bool hdr);
  void blur(Target& src, Target& tmp);
  void draw(const Program& p, const Target* target);

  BloomOptions options_;
  bool supported_ = true, initialised_ = false, glow_on_ = true;
  f64 frame_avg_ = 16, last_frame_at_ = 0, slow_since_ = 0;
  i32 w_ = 0, h_ = 0;
  Target main_, a1_, b1_, a2_, b2_;
  Program bright_, blur_, copy_, combine_;
  u32 vao_ = 0;
  // uniform locations
  u32 bright_src_ = 0, bright_threshold_ = 0, blur_src_ = 0, blur_dir_ = 0, copy_src_ = 0;
  u32 c_scene_ = 0, c_hdr_ = 0, c_b1_ = 0, c_b2_ = 0, c_s1_ = 0, c_s2_ = 0, c_exposure_ = 0, c_grade_ = 0;
  u32 c_ao_on_ = 0, c_ao_share_ = 0, c_ao_ = 0, c_depth_ = 0, c_ao_scale_ = 0, c_proj_ = 0;
};

// ---------------------------------------------------------------------------
// Ground-Truth Ambient Occlusion (Jimenez, Wu, Pesce, Jarabo 2016) after
// Intel's XeGTAO reference: horizon search on view-space slices of the scene
// depth, cosine-weighted visibility against the projected normal, view-space
// falloff, normals rebuilt from depth. A 4x4 interleaved pattern rotates the
// slices; a depth-aware 4x4 filter integrates exactly that pattern away. The
// result (RG16F: visibility, view depth) is read by the forward pass with a
// depth-aware upsample and applied to indirect light only: diffuse through the
// multi-bounce fit of the paper (one factor for the whole indirect light,
// reflections included). Presets as XeGTAO: low 1x2, medium 2x2, high 3x3,
// ultra 9x3 (slices x steps per side).

enum class AoQuality : u8 { low = 0, medium = 1, high = 2, ultra = 3 };

struct AoOptions {
  bool enabled = true;
  AoQuality quality = AoQuality::medium;
  bool half_resolution = true;  // mobile default; desktop: full resolution, high
  f32 radius = 1.6f;            // world units
  f32 power = 2.2f;             // XeGTAO final value power
  bool adaptive = true;         // steps down (ultra->low, then half resolution) on slow devices
};

class AmbientOcclusionPass {
 public:
  void configure(const AoOptions& options);
  /// World units; 0 = no ambient occlusion (a per-scene setting, unlike the device-dependent quality).
  void set_radius(f32 radius) { options_.radius = std::isfinite(radius) && radius > 0 ? radius : 0; }
  const AoOptions& options() const { return options_; }
  /// Computes the AO target from `depth` (full size width x height). projection = GL perspective matrix.
  /// Leaves no framebuffer bound. False = no AO this frame.
  bool compute(u32 depth, i32 width, i32 height, const Mat4& projection, f64 now_ms);
  u32 texture() const { return b_.texture; }
  i32 width() const { return b_.w; }
  i32 height() const { return b_.h; }
  void destroy(bool contextLost);
  /// Effective quality after the frame-time guard.
  AoQuality quality() const { return quality_; }
  bool half_resolution() const { return half_; }

 private:
  struct Target { u32 texture = 0, framebuffer = 0; i32 w = 0, h = 0; };
  bool init_programs();
  bool resize(i32 w, i32 h);
  void free_targets();

  AoOptions options_;
  AoQuality quality_ = AoQuality::medium;
  bool half_ = true;
  bool supported_ = true, initialised_ = false;
  f64 frame_avg_ = 16, last_frame_at_ = 0, slow_since_ = 0;
  i32 full_w_ = 0, full_h_ = 0;
  Target a_, b_;
  Program main_, denoise_;
  u32 vao_ = 0;
  u32 m_depth_ = 0, m_full_ = 0, m_ao_size_ = 0, m_proj_ = 0, m_radius_ = 0, m_power_ = 0, m_slices_ = 0, m_steps_ = 0;
  u32 d_src_ = 0;
};

}  // namespace kern
