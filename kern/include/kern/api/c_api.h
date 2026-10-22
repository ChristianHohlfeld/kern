// Flat C ABI of the engine for hosts that cannot use the C++ API directly
// (the browser platform layer, kern/web/kern.mjs). Every function is
// exported from the WebAssembly module under the same name.
//
// Data crosses the boundary through linear memory: the host allocates with
// kern_alloc, writes typed arrays and fixed-layout descriptors (see the
// KERN_* word offsets below) and passes pointers.
#pragma once

#include "kern/core/base.h"

#if defined(__wasm__)
#define KERN_EXPORT(name) extern "C" __attribute__((export_name(#name), used))
#else
#define KERN_EXPORT(name) extern "C"
#endif

namespace kern {
class ShaderExtension;
class Renderer;

/// The process-wide renderer used by the C API.
Renderer& c_api_renderer();
struct MeshInput;
/// Reads the material / flags / texture words of a KERN_MD_* descriptor.
void read_material(const u32* descriptor, MeshInput& input);
}  // namespace kern

/// Games linked into the module may provide their shader extensions by
/// defining this function (weak default: none).
extern "C" kern::u32 kern_game_extensions(kern::ShaderExtension*** out);

// ---- mesh descriptor (u32 words; f32 where noted) ----
enum KernMeshDesc : kern::u32 {
  KERN_MD_POSITIONS = 0,   // ptr, count (floats)
  KERN_MD_NORMALS = 2,
  KERN_MD_COLORS = 4,
  KERN_MD_UVS = 6,
  KERN_MD_INDICES = 8,     // ptr, count (indices)
  KERN_MD_INDEX_BYTES = 10,  // 2 or 4
  KERN_MD_JOINTS = 11,
  KERN_MD_WEIGHTS = 13,
  KERN_MD_BONES = 15,
  KERN_MD_EXT0 = 17,
  KERN_MD_EXT1 = 19,
  KERN_MD_TRANSFORMS = 21,
  KERN_MD_TINTS = 23,
  KERN_MD_TEXTURE = 25,
  KERN_MD_FLAGS = 26,      // KernMeshFlags
  KERN_MD_ALPHA_MODE = 27, // 0 opaque, 1 mask, 2 blend
  KERN_MD_COLOR = 28,      // 4 x f32
  KERN_MD_ALPHA_CUTOFF = 32,  // f32
  KERN_MD_ROUGHNESS = 33,  // f32
  KERN_MD_METALNESS = 34,  // f32
  KERN_MD_PARAMS = 35,     // 8 x i32 extension material parameters
  KERN_MD_WORDS = 43,
};

enum KernMeshFlags : kern::u32 {
  KERN_MF_VISIBLE = 1u << 0,
  KERN_MF_UNLIT = 1u << 1,
  KERN_MF_DOUBLE_SIDED = 1u << 2,
  KERN_MF_CAST_SHADOW = 1u << 3,
  KERN_MF_FRUSTUM_CULLED = 1u << 4,
  KERN_MF_VIEWMODEL = 1u << 5,
  KERN_MF_LINEAR_COLORS = 1u << 6,
  KERN_MF_WELD = 1u << 7,  // weld a triangle soup into an indexed mesh before upload
};

// ---- renderer init descriptor (u32 words; f32 where noted) ----
enum KernInitDesc : kern::u32 {
  KERN_ID_WIDTH = 0, KERN_ID_HEIGHT = 1,
  KERN_ID_SHADOWS = 2, KERN_ID_SHADOW_RESOLUTION = 3,
  KERN_ID_SHADOW_EXTENT = 4,  // f32
  KERN_ID_SHADOW_BIAS = 5,    // f32
  KERN_ID_BLOOM = 6, KERN_ID_BLOOM_FORCED = 7, KERN_ID_GRADE = 8,
  KERN_ID_WORDS = 9,
};

// ---- render params (u32 words; f32 where noted) ----
enum KernRenderParams : kern::u32 {
  KERN_RP_CLEAR = 0,  // 4 x f32
  KERN_RP_HAS_VIEWPORT = 4, KERN_RP_X = 5, KERN_RP_Y = 6, KERN_RP_W = 7, KERN_RP_H = 8,
  KERN_RP_PAGE_HIDDEN = 9,
  KERN_RP_WORDS = 10,
};

// ---- render stats (f64 slots) ----
enum KernRenderStats : kern::u32 {
  KERN_RS_DRAW_CALLS = 0, KERN_RS_SHADOW_DRAW_CALLS, KERN_RS_SHADOW_RESOLUTION, KERN_RS_SHADOW_REUSED,
  KERN_RS_TRIANGLES, KERN_RS_INSTANCES, KERN_RS_CULLED, KERN_RS_CPU_MS, KERN_RS_GPU_MS, KERN_RS_BLOOM,
  KERN_RS_VISIBLE, KERN_RS_SLOTS,
};

// ---- lighting (f32 slots) ----
enum KernLighting : kern::u32 {
  KERN_LI_SUN_DIRECTION = 0, KERN_LI_SUN_COLOR = 3, KERN_LI_SKY_COLOR = 6, KERN_LI_GROUND_COLOR = 9,
  KERN_LI_FOG_COLOR = 12, KERN_LI_FOG_DENSITY = 15, KERN_LI_EXPOSURE = 16, KERN_LI_ENV_INTENSITY = 17,
  KERN_LI_ENV_ZENITH = 18, KERN_LI_ENV_HORIZON = 21, KERN_LI_ENV_GROUND = 24, KERN_LI_SLOTS = 27,
};

// ---- sky (f32 slots) ----
enum KernSky : kern::u32 {
  KERN_SK_SUN = 0, KERN_SK_MOON = 3, KERN_SK_NIGHT = 6, KERN_SK_CLOUDS = 7, KERN_SK_BLAST_DIRECTION = 8,
  KERN_SK_BLAST_AGE = 11, KERN_SK_WIND = 12, KERN_SK_SLOTS = 14,
};

// ---- pick result (f64 slots) ----
enum KernPick : kern::u32 {
  KERN_PK_MESH = 0, KERN_PK_INSTANCE, KERN_PK_TRIANGLE, KERN_PK_DISTANCE,
  KERN_PK_POINT = 4, KERN_PK_NORMAL = 7, KERN_PK_BARYCENTRIC = 10,
  KERN_PK_TRIANGLES_EXAMINED = 13, KERN_PK_BOUNDS_REJECTED = 14, KERN_PK_SLOTS = 15,
};
