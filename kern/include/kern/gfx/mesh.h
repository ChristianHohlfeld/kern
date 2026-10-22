// Mesh, material and the CPU-side geometry the renderer keeps for picking,
// shadow/culling bounds and re-upload after a lost GPU context.
#pragma once

#include <memory>
#include <vector>

#include "kern/math/math.h"

namespace kern {

class Bvh;

constexpr u32 kMaxBones = 32;
constexpr u32 kMaxPointLights = 6;
constexpr u32 kMaxExtensionAttributes = 2;   // vertex locations 14 and 15
constexpr u32 kMaxMaterialParams = 8;        // integer parameters for shader extensions

/// Fixed vertex attribute locations of the standard shader.
namespace attr {
enum : u32 {
  position = 0, normal = 1, color = 2, uv = 3,
  model0 = 4,   // 4..7 instance model matrix columns
  tint = 8,
  normal0 = 9,  // 9..11 instance normal matrix columns
  joints = 12, weights = 13,
  extension0 = 14, extension1 = 15,
};
}

enum class AlphaMode : u8 { opaque = 0, mask = 1, blend = 2 };

struct Material {
  Vec4 color{1, 1, 1, 1};
  AlphaMode alpha = AlphaMode::opaque;
  f32 alpha_cutoff = 0.5f;
  f32 roughness = 1.0f;
  f32 metalness = 0.0f;
  Vec3 emissive{0, 0, 0};      // linear HDR colour added after lighting (feeds bloom)
  bool unlit = false;
  bool double_sided = false;
  bool cast_shadow = true;
  bool frustum_culled = true;
  bool viewmodel = false;      // first-person layer: drawn last after a depth clear
  bool linear_colors = false;  // vertex colours are linear (skip the sRGB decode)
  /// Parameters owned by shader extensions (see ShaderExtension::material_keys).
  i32 params[kMaxMaterialParams] = {};

  bool casts_shadow() const { return cast_shadow && alpha == AlphaMode::opaque; }
};

/// Borrowed input for Renderer::upsert. Spans must stay valid for the call only.
struct MeshInput {
  Span<const f32> positions;             // xyz per vertex (required)
  Span<const f32> normals;               // optional: flat normals for non-indexed meshes, +Z otherwise
  Span<const f32> colors;                // optional rgb per vertex, default white
  Span<const f32> uvs;                   // optional uv per vertex
  Span<const u32> indices;               // optional triangle list
  Span<const f32> joints, weights;       // optional skinning, 4 per vertex
  Span<const f32> bones;                 // bone palette, 16 floats per bone (<= 32)
  Span<const f32> extension[kMaxExtensionAttributes];  // extension streams in declaration order
  Span<const f32> transforms;            // instance matrices, 16 floats each (default: one identity)
  Span<const f32> tints;                 // instance rgba (default white)
  Material material;
  u32 texture = 0;                       // GPU texture handle (0 = none)
  bool visible = true;
};

struct MeshGpu {
  u32 vao = 0;
  u32 position = 0, normal = 0, color = 0, uv = 0;
  u32 transforms = 0, tints = 0, instance_normals = 0;
  u32 joints = 0, weights = 0;
  u32 extension[kMaxExtensionAttributes] = {};
  u32 element = 0;
  u32 element_type = 0;
  u32 capacity[3] = {};  // floats allocated in transforms / tints / instance_normals
};

struct Mesh {
  u32 id = 0;
  bool visible = true;
  bool linear_colors = false;
  Material material;
  u32 texture = 0;

  std::vector<f32> positions, normals, colors, uvs;
  std::vector<u32> indices;
  std::vector<f32> extension[kMaxExtensionAttributes];
  u8 extension_size[kMaxExtensionAttributes] = {};
  std::vector<f32> joints, weights, bones;

  std::vector<f32> transforms, tints, instance_normals;
  u32 instance_count = 0;
  u8 mirror = 0;  // instances with negative determinant: 0 none, 1 all, 2 some

  Sphere local_bounds, world_bounds;
  std::unique_ptr<Bvh> bvh;  // static world-space meshes only (picking)
  MeshGpu gpu;
  f32 sort_distance = 0;
  u32 live_index = 0;  // position in Renderer's dense live list

  Mesh();
  ~Mesh();
  Mesh(const Mesh&) = delete;
  Mesh& operator=(const Mesh&) = delete;

  bool skinned() const { return !bones.empty(); }
  /// Face culling for this mesh: 0 off, 1 back faces, 2 front faces (all
  /// instances mirrored flip the winding; a mix cannot be culled in one draw).
  i32 cull_mode() const { return material.double_sided || mirror == 2 ? 0 : mirror == 1 ? 2 : 1; }
  u32 vertex_count() const { return static_cast<u32>(positions.size() / 3); }
  u32 element_count() const { return indices.empty() ? vertex_count() : static_cast<u32>(indices.size()); }
  u32 triangle_count() const { return element_count() / 3; }
  /// Vertex `v` in mesh space with the current skin pose applied.
  Vec3 posed_vertex(u32 v) const;
};

/// Bounding sphere of a position array.
Sphere bounding_sphere(const f32* positions, u32 vertex_count);
/// 0 when no instance mirrors (negative 3x3 determinant), 1 when all do, 2 for a mix.
u8 mirror_state(const f32* transforms, u32 instance_count);
/// Applies a cull_mode() value to the GL state.
void apply_cull_mode(i32 mode);
/// World sphere enclosing every instance transform of a local sphere.
Sphere instanced_bounds(const Sphere& local, const f32* transforms, u32 instance_count);

}  // namespace kern
