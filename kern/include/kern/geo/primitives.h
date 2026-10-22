// Indexed primitive meshes (Z up, centred on the origin unless noted).
#pragma once

#include "kern/geo/mesh_data.h"

namespace kern::primitives {

/// Axis-aligned box with flat faces (24 vertices).
MeshData box(Vec3 size, Vec3 color = {1, 1, 1});
/// Box whose base sits on z = 0 (buildings, crates).
MeshData box_on_ground(Vec3 size, Vec3 color = {1, 1, 1});
/// UV sphere with smooth normals.
MeshData sphere(f32 radius, u32 segments = 24, u32 rings = 16, Vec3 color = {1, 1, 1});
/// Capped cylinder standing on z = 0.
MeshData cylinder(f32 radius, f32 height, u32 segments = 20, Vec3 color = {1, 1, 1});
/// Capped cone standing on z = 0.
MeshData cone(f32 radius, f32 height, u32 segments = 16, Vec3 color = {1, 1, 1});
/// Flat rectangle at z = 0 facing +Z.
MeshData plane(f32 width, f32 depth, Vec3 color = {1, 1, 1}, f32 uv_scale = 1);
/// Appends `src` transformed by `m` to `dst` (merging static props into one mesh).
void append(MeshData& dst, const MeshData& src, const Mat4& m);

}  // namespace kern::primitives
