// Auxiliary GPU passes of the renderer: sun shadow map, procedural sky and
// the HDR bloom composite.
#pragma once

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
  f32 extent = 120;      // half size of the covered square in metres
  f32 bias = 0.0012f;
};

class SunShadowMap {
 public:
  bool init(const ShadowOptions& options);
  void destroy(bool contextLost);
  bool valid() const { return fbo_ != 0; }

  /// Snaps `focus` for `sun` and returns a cache key of the resulting map.
  u64 focus_key(Vec3 focus, Vec3 sun);
  /// Renders the casters among `meshes`; returns the number of draw calls.
  u32 draw(Span<Mesh* const> meshes, Vec3 focus, Vec3 sun);

  const Mat4& matrix() const { return matrix_; }
  u32 depth_texture() const { return depth_; }
  const ShadowOptions& options() const { return options_; }

 private:
  ShadowOptions options_;
  Program program_;
  u32 u_matrix_ = 0, u_bones_ = 0, u_skinned_ = 0;
  u32 depth_ = 0, fbo_ = 0;
  Mat4 matrix_;
  Vec3 snapped_focus_;
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

class SkyPass {
 public:
  bool init();
  void destroy(bool contextLost);
  void draw(const Camera& camera, f32 aspect, const SkyState& state, f32 exposure, f64 time_seconds);

 private:
  Program program_;
  u32 vao_ = 0;
  u32 u_forward_ = 0, u_right_ = 0, u_up_ = 0, u_tan_half_ = 0, u_night_ = 0, u_sun_up_ = 0, u_time_ = 0;
  u32 u_clouds_ = 0, u_exposure_ = 0, u_blast_age_ = 0, u_sun_ = 0, u_moon_ = 0, u_blast_dir_ = 0, u_wind_ = 0;
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
  void end(f32 exposure);
  void destroy(bool contextLost);
  BloomState state() const;

 private:
  struct Target { u32 texture = 0, framebuffer = 0, hdr = 0, depth = 0; i32 w = 0, h = 0; };
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
};

}  // namespace kern
