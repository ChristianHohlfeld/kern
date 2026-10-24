// Built-in meshes: triangle meshes compiled into the engine, referenced by name from a
// MeshComponent (Shape::mesh, MeshComponent::asset). Today these are Plumo's links
// ("duck/<MJCF body>", Pollen Robotics, CC BY-NC-SA 4.0: kern/third_party/pollen).
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "kern/geo/mesh_data.h"

namespace kern::game {

struct BuiltinMesh {
  MeshData mesh;              // positions (metres), normals, sRGB vertex colours, indices
  std::vector<Vec3> hull;     // points of its convex hull (for hull colliders)
  Vec3 lo, hi;                // bounds
};

/// The mesh with this name, decoded on first use; nullptr when there is none.
const BuiltinMesh* builtin_mesh(std::string_view name);
/// Every built-in mesh name.
std::vector<std::string> builtin_mesh_names();

}  // namespace kern::game
