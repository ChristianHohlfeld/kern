// Soldier art after SpawnCity (sim/character_model.mjs, sim/weapon_models.mjs,
// player_walk_mode_v4.mjs): the articulated soldier with its walk / run / air /
// aim / dead animation, the first-person weapons (Volt SMG, Glock, .50 sniper,
// 40 mm launcher) part by part, the gloves and sleeves of the view model, and the
// effect meshes (muzzle flash, tracer, decal, chip, brass). Everything is built
// procedurally, as in SpawnCity, from boxes, cylinders, capsules, spheres and
// bevelled polygon extrusions.
//
// Frames: the character is Z up with forward +Y and the root at the feet; weapons
// and the view model are x right, y up, barrel along -Z (camera space).
#pragma once

#include <array>
#include <vector>

#include "kern/geo/mesh_data.h"
#include "kern/game/level.h"

namespace kern::game::models {

/// A mesh split by surface: matte, metal and glowing (unlit) parts get their own materials.
struct PartMeshes {
  MeshData matte, metal, glow;
  // Skinned meshes: the bone of every vertex (one per vertex, weight 1).
  std::vector<f32> matte_bones, metal_bones, glow_bones;
};

/// Builds geometry into PartMeshes with a current transform, bone and surface.
class Builder {
 public:
  explicit Builder(PartMeshes& out) : out_(out) {}
  enum class Surface : u8 { matte, metal, glow };
  void set_transform(const Mat4& m) { xf_ = m; }
  const Mat4& transform() const { return xf_; }
  void set_bone(u32 bone) { bone_ = bone; }
  /// Picks the surface from a SpawnCity material (metalness >= .5: metal).
  void material(u32 rgb, f32 metalness = 0.2f, bool glow = false);

  /// Three.js-style parts; `rot` is an XYZ Euler rotation (radians), applied before `pos`.
  void box(Vec3 size, Vec3 pos, Vec3 rot = {}, Vec3 scale = {1, 1, 1});
  /// CylinderGeometry: axis Y, radius `top` at +Y.
  void cylinder(f32 top, f32 bottom, f32 len, u32 seg, Vec3 pos, Vec3 rot, Vec3 scale = {1, 1, 1});
  /// CapsuleGeometry: axis Y, `len` = the straight part.
  void capsule(f32 r, f32 len, u32 seg, Vec3 pos, Vec3 rot = {}, Vec3 scale = {1, 1, 1});
  /// Sphere; theta_end < pi gives a cap around +Y (a helmet).
  void sphere(f32 r, u32 seg, u32 rings, Vec3 pos, Vec3 rot = {}, Vec3 scale = {1, 1, 1}, f32 theta_end = 3.14159265f);
  /// Disc facing +Z.
  void circle(f32 r, u32 seg, Vec3 pos, Vec3 rot = {});
  /// weapon_models.mjs profile(): closed polygon (sx, sy) extruded `w` wide along X, shape +x -> -Z,
  /// with a bevel that grows the outline by `bevel` between the chamfered caps.
  void profile(std::initializer_list<Vec2> points, f32 w, f32 bevel = 0.008f, Vec3 pos = {}, Vec3 rot = {});

 private:
  void tri(Vec3 a, Vec3 b, Vec3 c, Vec3 na, Vec3 nb, Vec3 nc);
  void flat(Vec3 a, Vec3 b, Vec3 c, Vec3 outward);
  Mat4 part(Vec3 pos, Vec3 rot, Vec3 scale) const;
  PartMeshes& out_;
  Mat4 xf_;
  u32 bone_ = 0;
  Surface surface_ = Surface::matte;
  Vec3 color_{1, 1, 1};
  Mat4 cur_;
};

/// XYZ Euler rotation matrix (Three.js order).
Mat4 euler(Vec3 r);

// ---------------------------------------------------------------------------
// weapons

struct WeaponModel {
  PartMeshes parts;     // static parts
  PartMeshes slide;     // Glock slide (moves on each shot), empty otherwise
  // Markers in the model frame (after the builder's own scale / offset).
  Vec3 grip, rear_sight, front_sight, muzzle, eject;
  f32 flash_scale = 1;  // muzzle flash size (SpawnCity base scale x the model scale)
  f32 flash_ms = 34;    // flash duration
  f32 slide_travel = 0; // m the slide moves back
  Vec3 mount;           // offset under the view model root
};

WeaponModel weapon_model(WeaponKind kind);

/// Gloves, cuffs, sleeves and the skin hand of the first-person arms, posed for a grip at `grip`.
void view_arms(Builder& b, Vec3 grip);

// ---------------------------------------------------------------------------
// character

enum Joint : u8 {
  kRoot, kPelvis, kSpine, kNeck, kHead, kShoulderL, kElbowL, kWristL, kShoulderR, kElbowR, kWristR,
  kHipL, kKneeL, kAnkleL, kHipR, kKneeR, kAnkleR, kJointCount
};

/// Rest offsets of the joints (relative to the parent) and the parents.
const std::array<Vec3, kJointCount>& joint_offsets();
const std::array<i8, kJointCount>& joint_parents();

struct Outfit { u32 shirt, vest, pants, boots, skin, gloves, helmet; };
Outfit player_outfit(u32 shirt);
/// FNV-1a shirt colour of a player id (SpawnCity shirtColorFor).
u32 shirt_for(u32 id);

/// The articulated soldier as one skinned mesh (bone = joint index), with the small
/// third-person gun on the right wrist.
PartMeshes character_mesh(const Outfit& outfit, bool gun);

enum class CharState : u8 { idle, walk, run, air, aim, dead };

/// animateCharacter: joint rotations (XYZ Euler) and the pelvis height for a state.
class CharacterAnimator {
 public:
  struct Input {
    f32 dt = 0, speed = 0;      // horizontal m/s
    bool air = false, dead = false, armed = true, pistol = false;
    f32 crouch = 0;             // 0..1 (Kern)
    f32 pitch = 0;              // aim pitch, radians, positive looks down (Kern replicates it)
    f32 kick = 0;               // 0..1 right shoulder shot kick
  };
  explicit CharacterAnimator(u32 seed = 1);
  void update(const Input& in);
  /// Model-space bone matrices (joint pose x inverse bind), 16 floats per joint.
  void bones(f32* out) const;
  /// Model-space pose of a joint.
  Mat4 joint_matrix(u32 j) const;
  f32 phase() const { return phase_; }

 private:
  void set(u32 j, Vec3 r, f32 k);
  std::array<Vec3, kJointCount> rot_{};
  f32 pelvis_z_ = 0.96f, run_w_ = 0, phase_ = 0, fall_ = 0;
};

// ---------------------------------------------------------------------------
// effects (unit meshes, placed and faded per instance)

/// Muzzle flash: a star facing -Z and two crossed side flames reaching forward along -Z (length 1).
MeshData flash_mesh();
/// Unit cylinder along +Z from 0 to 1, radius 1.
MeshData tracer_mesh(u32 seg);
/// Unit disc facing +Z.
MeshData decal_mesh();
/// Unit cube.
MeshData chip_mesh();
/// Low sphere, radius 1.
MeshData ball_mesh();
/// Brass case (cylinder r .0048, h .019 along Y).
MeshData brass_mesh();

}  // namespace kern::game::models
