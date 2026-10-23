// Soldier art in the world (see soldier_models.h for the meshes): SpawnCity's
// articulated soldiers for every soldier actor (animated from the pawn while
// playing, standing at rest while editing), the first-person weapon with its
// gloves and sleeves (rest placement, sight alignment, kick, aim down sights,
// Glock slide and brass), muzzle flashes, travelling tracers, impact flashes,
// bullet-hole decals and chips. Pure presentation: nothing here changes the
// simulation, so authority, replicas and the editor draw the same.
#include <algorithm>
#include "kern/core/dmath.h"
#include <cmath>
#include <cstring>

#include "kern/game/runtime.h"
#include "runtime_internal.h"
#include "soldier_models.h"

namespace kern::game {

namespace {

constexpr f32 kPi = 3.14159265f;
using rt::kDegree;
constexpr u32 kDecals = 420, kChips = 220, kTracers = 24, kRemoteTracers = 16, kBalls = 16, kFlashes = 8, kBrass = 4;

Vec3 hex(u32 c) { return {((c >> 16) & 255) / 255.0f, ((c >> 8) & 255) / 255.0f, (c & 255) / 255.0f}; }

/// Rotation taking unit `from` to unit `to`, scaled by `t` (0..1).
Quat rotation_between(Vec3 from, Vec3 to, f32 t = 1) {
  const Vec3 axis = cross(from, to);
  const f32 s = length(axis), c = dot(from, to);
  if (s < 1e-6f) return Quat{};
  return Quat::axis_angle(axis * (1 / s), kern::dm::atan2(s, c) * t);
}

/// Matrix whose +Z points along `dir` (any roll).
Mat4 basis_z(Vec3 dir) {
  const Vec3 z = normalize(dir, {0, 0, 1});
  const Vec3 ref = std::fabs(z.z) < 0.95f ? Vec3{0, 0, 1} : Vec3{1, 0, 0};
  const Vec3 x = normalize(cross(ref, z)), y = cross(z, x);
  Mat4 m;
  m.m[0] = x.x; m.m[1] = x.y; m.m[2] = x.z;
  m.m[4] = y.x; m.m[5] = y.y; m.m[6] = y.z;
  m.m[8] = z.x; m.m[9] = z.y; m.m[10] = z.z;
  return m;
}

Mat4 quat_matrix(Quat q) { return Mat4::trs({}, q, {1, 1, 1}); }

struct Rng32 {
  u32 s;
  f32 next() { s = s * 1664525u + 1013904223u; return static_cast<f32>(s >> 8) / 16777216.0f; }
  f32 range(f32 a, f32 b) { return a + (b - a) * next(); }
};

}  // namespace

/// Mesh ids and transient effect state of the soldier art.
struct SoldierArt {
  struct Meshes { u32 matte = 0, metal = 0, glow = 0; };
  struct Rig {
    Meshes mesh;
    models::CharacterAnimator anim;
    WeaponKind weapon = WeaponKind::rifle;
    bool armed = false;
    Vec3 feet, vel;
    bool has_feet = false;
    f64 last_shot = -1e9;
    bool seen = false;
    explicit Rig(u32 seed) : anim(seed) {}
  };
  std::unordered_map<ActorId, std::unique_ptr<Rig>> rigs;
  // First person.
  Meshes view, slide;
  WeaponKind view_kind = WeaponKind::rifle;
  bool view_built = false;
  models::WeaponModel weapon;  // markers of the built weapon
  f64 local_shot = -1e9;
  f32 rest_depth = 30;
  f64 rest_resolved = -1e9;
  Vec3 aim_smooth{0, 0, -30};
  u32 flash_view = 0;
  f64 flash_start = -1e9;
  f32 flash_roll = 0, flash_size = 1, flash_ms = 34;
  struct Brass { Vec3 pos, vel, rot, spin; f64 born = -1e9; };
  std::array<Brass, kBrass> brass{};
  u32 brass_next = 0, brass_mesh = 0;
  Mat4 view_matrix;       // camera-space weapon group -> world, this frame
  bool view_valid = false;
  // World effects.
  u32 tracer_core = 0, tracer_halo = 0, tracer_remote = 0, balls = 0, flashes = 0, decals = 0, chips = 0;
  struct Tracer { Vec3 from, to; f64 born; f32 speed; bool remote; };
  std::vector<Tracer> tracers;
  struct Ball { Vec3 at; f64 born; };
  std::vector<Ball> ball_list;
  struct Flash { Vec3 at, dir; f64 born; };
  std::vector<Flash> flash_list;
  std::vector<f32> decal_xf, decal_tint;
  u32 decal_next = 0, decal_count = 0;
  bool decals_dirty = false;
  struct Chip { Vec3 pos, vel, spin, rot, color; f32 size, life, age, floor; };
  std::vector<Chip> chip_list;
  u32 chip_next = 0;
  Rng32 rng{0x5eed};
};

void SoldierArtDelete::operator()(SoldierArt* a) const { delete a; }

namespace {

u32 upsert_mesh(Renderer& r, const MeshData& m, const Material& mat, u32 layers, const std::vector<f32>* bone_of_vertex = nullptr,
                u32 instances = 1) {
  if (m.vertex_count() == 0) return 0;
  MeshInput in = m.as_input(kMaxExtensionAttributes);
  in.material = mat;
  in.layers = layers;
  std::vector<f32> joints, weights, bones;
  if (bone_of_vertex) {
    joints.reserve(m.vertex_count() * 4);
    weights.reserve(m.vertex_count() * 4);
    for (u32 i = 0; i < m.vertex_count(); ++i) {
      joints.insert(joints.end(), {(*bone_of_vertex)[i], 0, 0, 0});
      weights.insert(weights.end(), {1, 0, 0, 0});
    }
    in.joints = Span<const f32>(joints.data(), joints.size());
    in.weights = Span<const f32>(weights.data(), weights.size());
    bones.resize(models::kJointCount * 16);
    for (u32 j = 0; j < models::kJointCount; ++j) {
      const Mat4 id;
      std::copy(id.m, id.m + 16, bones.begin() + j * 16);
    }
    in.bones = Span<const f32>(bones.data(), bones.size());
  }
  std::vector<f32> xf;
  for (u32 i = 0; i < instances; ++i) { const Mat4 id; xf.insert(xf.end(), id.m, id.m + 16); }
  in.transforms = Span<const f32>(xf.data(), xf.size());
  const u32 id = r.allocate_id();
  if (!r.upsert(id, in)) { r.release_id(id); return 0; }
  return id;
}

void drop(Renderer& r, u32& id) {
  if (id) { r.remove(id); r.release_id(id); id = 0; }
}

Material matte(bool viewmodel) {
  Material m;
  m.roughness = 0.85f;
  m.metalness = 0.0f;
  m.viewmodel = viewmodel;
  m.cast_shadow = !viewmodel;
  m.frustum_culled = false;
  return m;
}
Material metal(bool viewmodel) {
  Material m = matte(viewmodel);
  m.roughness = 0.3f;
  m.metalness = 0.75f;
  return m;
}
Material glow(bool viewmodel) {
  Material m = matte(viewmodel);
  m.unlit = true;
  m.emissive = {0.6f, 0.6f, 0.6f};
  m.cast_shadow = false;
  return m;
}
Material effect(bool viewmodel, bool lit = false) {
  Material m;
  m.alpha = AlphaMode::blend;
  m.unlit = !lit;
  m.double_sided = true;
  m.cast_shadow = false;
  m.frustum_culled = false;
  m.viewmodel = viewmodel;
  m.roughness = 1;
  return m;
}

SoldierArt::Meshes upsert_parts(Renderer& r, const models::PartMeshes& p, bool viewmodel, u32 layers, bool skinned) {
  SoldierArt::Meshes m;
  m.matte = upsert_mesh(r, p.matte, matte(viewmodel), layers, skinned ? &p.matte_bones : nullptr);
  m.metal = upsert_mesh(r, p.metal, metal(viewmodel), layers, skinned ? &p.metal_bones : nullptr);
  m.glow = upsert_mesh(r, p.glow, glow(viewmodel), layers, skinned ? &p.glow_bones : nullptr);
  return m;
}

void drop(Renderer& r, SoldierArt::Meshes& m) { drop(r, m.matte); drop(r, m.metal); drop(r, m.glow); }

void each(const SoldierArt::Meshes& m, auto&& fn) {
  for (u32 id : {m.matte, m.metal, m.glow})
    if (id) fn(id);
}

/// Appends `src` (all surfaces) transformed by `xf` and bound to `bone`.
void append(models::PartMeshes& dst, const models::PartMeshes& src, const Mat4& xf, u32 bone) {
  auto add = [&](MeshData& d, std::vector<f32>& b, const MeshData& s) {
    for (u32 i = 0; i < s.vertex_count(); ++i) {
      const Vec3 p = xf.transform_point({s.positions[i * 3], s.positions[i * 3 + 1], s.positions[i * 3 + 2]});
      const Vec3 n = normalize(xf.transform_vector({s.normals[i * 3], s.normals[i * 3 + 1], s.normals[i * 3 + 2]}), {0, 0, 1});
      d.push(p, n, {s.colors[i * 3], s.colors[i * 3 + 1], s.colors[i * 3 + 2]}, {0, 0});
      b.push_back(static_cast<f32>(bone));
    }
  };
  add(dst.matte, dst.matte_bones, src.matte);
  add(dst.metal, dst.metal_bones, src.metal);
  add(dst.glow, dst.glow_bones, src.glow);
}

/// The soldier with its real weapon in the right hand (SpawnCity draws a two-box stand-in;
/// Kern shows the model the player carries, at .55 scale).
models::PartMeshes rig_mesh(u32 id, bool armed, WeaponKind kind) {
  models::PartMeshes p = models::character_mesh(models::player_outfit(models::shirt_for(id)), false);
  if (armed) {
    const models::WeaponModel w = models::weapon_model(kind);
    Vec3 wrist;
    for (i32 j = models::kWristR; j >= 0; j = models::joint_parents()[static_cast<u32>(j)]) wrist += models::joint_offsets()[static_cast<u32>(j)];
    // The grip sits in the glove (0, .01, -.06 below the wrist); the barrel continues the forearm (-Z).
    const f32 s = 0.55f;
    const Mat4 xf = Mat4::translate(wrist + Vec3{0, .015f, -.07f}) * Mat4::scale({s, s, s}) * Mat4::translate(w.grip * -1.0f);
    append(p, w.parts, xf, models::kWristR);
    append(p, w.slide, xf, models::kWristR);
  }
  return p;
}

}  // namespace

GameWorld::ArtStats GameWorld::art_stats() const {
  ArtStats s;
  if (!art_) return s;
  s.rigs = static_cast<u32>(art_->rigs.size());
  s.decals = art_->decal_count;
  s.tracers = static_cast<u32>(art_->tracers.size());
  s.chips = static_cast<u32>(art_->chip_list.size());
  s.view_model = art_->view_valid;
  return s;
}

void GameWorld::release_soldier_art() {
  if (!art_) return;
  SoldierArt& a = *art_;
  for (auto& [id, rig] : a.rigs) {
    each(rig->mesh, [&](u32 m) { mesh_owner_.erase(m); });
    drop(r_, rig->mesh);
  }
  a.rigs.clear();
  drop(r_, a.view);
  drop(r_, a.slide);
  for (u32* id : {&a.flash_view, &a.brass_mesh, &a.tracer_core, &a.tracer_halo, &a.tracer_remote, &a.balls, &a.flashes, &a.decals, &a.chips}) drop(r_, *id);
  art_.reset();
}

void GameWorld::present_soldier_art(f64 now_ms, f32 dt) {
  // Which actors are soldiers (root actors with a soldier controller)?
  std::vector<const Actor*> soldiers;
  for (const Actor& a : level_.actors)
    if (a.controller && a.controller->mode == ControlMode::soldier) soldiers.push_back(&a);
  if (soldiers.empty() && !art_) return;
  if (!art_) art_.reset(new SoldierArt());
  SoldierArt& art = *art_;
  dt = clamp(dt, 0.0f, 0.1f);

  // ---- rigs --------------------------------------------------------------------------------
  art_hidden_.clear();
  for (auto& [id, rig] : art.rigs) rig->seen = false;
  const bool fp_local = running() && local_ && player_ == local_->actor && mode_ != SimMode::simulate && view_valid_;
  for (const Actor* a : soldiers) {
    // The rig replaces the actor's own meshes (capsule, head ...) and those of its children.
    std::vector<ActorId> stack{a->id};
    while (!stack.empty()) {
      const ActorId id = stack.back();
      stack.pop_back();
      art_hidden_.insert(id);
      for (const Actor& c : level_.actors)
        if (c.parent == id) stack.push_back(c.id);
    }
    const bool armed = a->weapon.has_value();
    const WeaponKind kind = armed ? a->weapon->kind : WeaponKind::rifle;
    auto& slot = art.rigs[a->id];
    if (slot && (slot->armed != armed || slot->weapon != kind)) {
      each(slot->mesh, [&](u32 m) { mesh_owner_.erase(m); });
      drop(r_, slot->mesh);
      slot.reset();
    }
    if (!slot) {
      slot = std::make_unique<SoldierArt::Rig>(a->id);
      slot->armed = armed;
      slot->weapon = kind;
      slot->mesh = upsert_parts(r_, rig_mesh(a->id, armed, kind), false, layers_, true);
      each(slot->mesh, [&](u32 m) { mesh_owner_[m] = a->id; });
    }
    SoldierArt::Rig& rig = *slot;
    rig.seen = true;
    // Where it stands and where it looks.
    Mat4 m;
    if (!actor_matrix(a->id, m)) m = level_.world_matrix(a->id);
    Pawn* p = running() ? pawn_of(a->id) : nullptr;
    f32 lift = p ? p->mover_lift : (a->mesh && a->mesh->shape == Shape::capsule ? a->mesh->height * 0.5f : 0.9f);
    if (p && p->mover_lift <= 0) lift = a->mesh && a->mesh->shape == Shape::capsule ? a->mesh->height * 0.5f : 0.9f;
    const Vec3 feet = m.translation() - Vec3{0, 0, lift};
    f32 yaw = kern::dm::atan2(m.m[1], m.m[0]);
    if (p) yaw = (p == local_ ? camera_yaw_ : p->yaw) * kDegree;
    // Velocity from the presented feet (works the same for the authority and replicas).
    if (rig.has_feet && dt > 0) rig.vel = rig.vel + ((feet - rig.feet) / dt - rig.vel) * min_of(1.0f, dt * 12);
    rig.feet = feet;
    rig.has_feet = true;
    models::CharacterAnimator::Input in;
    in.dt = dt;
    const f32 hspeed = kern::dm::hypot(rig.vel.x, rig.vel.y);
    in.speed = hspeed > 12 ? 0 : hspeed;  // teleports and respawns are not running
    const auto hs = health_.find(a->id);
    in.dead = running() && hs != health_.end() && hs->second.dead;
    in.air = running() && (p && p->use_mover ? !p->mover.on_ground() : std::fabs(rig.vel.z) > 1.5f);
    in.armed = armed;
    in.pistol = kind == WeaponKind::pistol;
    in.crouch = p ? p->crouch : 0;
    in.pitch = p ? (p == local_ ? camera_pitch_ : p->pitch) * kDegree : 0;
    const f64 since = now_ms - rig.last_shot;
    in.kick = since >= 0 && since < 120 ? kern::dm::sin(kPi * static_cast<f32>(since) / 120) : 0;
    rig.anim.update(in);
    f32 bones[models::kJointCount * 16];
    rig.anim.bones(bones);
    const Mat4 model = Mat4::translate(feet) * Mat4::trs({}, Quat::axis_angle({0, 0, 1}, yaw - kPi / 2), {1, 1, 1});
    const bool visible = !a->hidden && !(fp_local && a->id == local_->actor) && (!running() || alive_[a->id] || in.dead);
    each(rig.mesh, [&](u32 id) {
      r_.update_bones(id, Span<const f32>(bones, models::kJointCount * 16));
      r_.update_instances(id, Span<const f32>(model.m, 16), {});
      r_.set_visible(id, visible);
    });
  }
  for (auto it = art.rigs.begin(); it != art.rigs.end();) {
    if (it->second->seen) { ++it; continue; }
    each(it->second->mesh, [&](u32 m) { mesh_owner_.erase(m); });
    drop(r_, it->second->mesh);
    it = art.rigs.erase(it);
  }
  // Hide what the rigs replace (while editing the sync shows them again each time it runs).
  for (ActorId id : art_hidden_)
    if (auto v = visuals_.find(id); v != visuals_.end() && v->second->mesh) {
      r_.set_visible(v->second->mesh, false);
      v->second->visible = false;
    }
  if (!running()) return;

  // ---- shots of this frame ------------------------------------------------------------------
  auto weapon_of = [&](ActorId id) {
    const Actor* a = level_.find(id);
    return a && a->weapon ? a->weapon->kind : WeaponKind::rifle;
  };
  for (const ShotEvent& s : shots_) {
    const bool mine = fp_local && s.shooter == local_->actor;
    const WeaponKind kind = weapon_of(s.shooter);
    if (auto r = art.rigs.find(s.shooter); r != art.rigs.end()) r->second->last_shot = now_ms;
    Vec3 from = s.from;
    if (mine) {
      if (now_ms - art.local_shot > 8) {
        art.local_shot = now_ms;
        art.flash_start = now_ms + 0;
        art.flash_roll = art.rng.range(0, 2 * kPi);
        art.flash_size = art.rng.range(.82f, 1.24f);
        // Brass from the ejection port.
        if (kind == WeaponKind::pistol || kind == WeaponKind::rifle) {
          SoldierArt::Brass& b = art.brass[art.brass_next++ % kBrass];
          b.pos = art.weapon.eject + art.weapon.mount;
          b.vel = {art.rng.range(.75f, 1.1f), art.rng.range(.55f, .85f), art.rng.range(.12f, .3f)};
          b.rot = {art.rng.range(0, 3), 0, kPi / 2};
          b.spin = {art.rng.range(18, 28), art.rng.range(0, 6), art.rng.range(22, 34)};
          b.born = now_ms;
        }
      }
      if (art.view_valid) from = art.view_matrix.transform_point(art.weapon.muzzle + art.weapon.mount);
    } else {
      SoldierArt::Flash f;
      f.at = s.from;
      f.dir = normalize(s.to - s.from, {1, 0, 0});
      f.born = now_ms;
      if (art.flash_list.size() < kFlashes) art.flash_list.push_back(f);
    }
    SoldierArt::Tracer t;
    t.from = from;
    t.to = s.to;
    t.born = now_ms;
    t.speed = kind == WeaponKind::sniper ? 850.0f : 240.0f;
    t.remote = !mine;
    if (length(t.to - t.from) > 0.5f) {
      if (art.tracers.size() >= kTracers + kRemoteTracers) art.tracers.erase(art.tracers.begin());
      art.tracers.push_back(t);
    }
    if (!s.impact) continue;
    if (art.ball_list.size() >= kBalls) art.ball_list.erase(art.ball_list.begin());
    // Mine arrive when the tracer does; others at once.
    art.ball_list.push_back({s.to, now_ms + (mine ? length(s.to - from) / t.speed * 1000.0 : 0.0)});
    // Decal and chips by surface (world_impacts.mjs).
    const Vec3 n = normalize(s.normal, {0, 0, 1});
    const bool ground = n.z > 0.7f;
    u32 c0 = 0xcfc6b4, c1 = 0xa89f90, decal = ground ? 0x2e2a22 : 0x34302a;
    f32 radius = .085f, speed = 3.5f;
    u32 count = 6;
    if (ground) { c0 = 0x6b5a44; c1 = 0x7a9a52; }
    if (s.surface == 2) { c0 = 0xffd27a; c1 = 0xfff2c4; radius = .06f; decal = 0x1c1c1c; speed = 5; count = 5; }
    if (s.surface == 3 || s.hit) { c0 = 0x8a1a1a; c1 = 0x5a1010; radius = 0; count = s.head ? 9 : 5; speed = 2.4f; }
    if (kind == WeaponKind::pistol) { count += 9; speed = 6; }
    if (kind == WeaponKind::sniper) { count += 16; speed = 9; }
    if (radius > 0 && s.surface == 1) {
      // Oriented to the surface, random spin, .012 off it; far ones a little bigger to stay readable.
      const f32 far = clamp(length(s.to - view_camera_.eye) / 60.0f, 1.0f, 4.0f);
      const Mat4 xf = Mat4::translate(s.to + n * .012f) * basis_z(n) * quat_matrix(Quat::axis_angle({0, 0, 1}, art.rng.range(0, 2 * kPi))) *
                      Mat4::scale({radius * far, radius * far, 1});
      if (art.decal_xf.size() < kDecals * 16) { art.decal_xf.resize(kDecals * 16, 0); art.decal_tint.resize(kDecals * 4, 0); }
      const u32 slot = art.decal_next++ % kDecals;
      std::copy(xf.m, xf.m + 16, art.decal_xf.begin() + slot * 16);
      const Vec3 c = hex(decal);
      const f32 tint[4] = {c.x * 2.2f, c.y * 2.2f, c.z * 2.2f, .86f};
      std::copy(tint, tint + 4, art.decal_tint.begin() + slot * 4);
      art.decal_count = min_of(kDecals, art.decal_count + 1);
      art.decals_dirty = true;
    }
    // Chips: one ray finds the floor they bounce on.
    f32 floor = s.to.z;
    if (!ground) {
      const physics::RayHit down = physics_.cast_ray(s.to + n * .05f, {0, 0, -30});
      floor = down.hit ? down.point.z : s.to.z - 30;
    }
    for (u32 i = 0; i < count; ++i) {
      SoldierArt::Chip ch;
      ch.pos = s.to + n * .02f;
      const Vec3 d = normalize(n + Vec3{art.rng.range(-.7f, .7f), art.rng.range(-.7f, .7f), art.rng.range(-.2f, .9f)}, n);
      ch.vel = d * (speed * art.rng.range(.5f, 1.2f));
      ch.spin = {art.rng.range(-14, 14), art.rng.range(-14, 14), art.rng.range(-14, 14)};
      ch.rot = {};
      ch.color = hex(i % 2 ? c1 : c0);
      ch.size = art.rng.range(.04f, .11f) * (s.surface == 3 || s.hit ? .7f : 1.0f);
      ch.life = art.rng.range(.5f, 1.1f);
      ch.age = 0;
      ch.floor = floor;
      if (art.chip_list.size() < kChips) art.chip_list.push_back(ch);
      else art.chip_list[art.chip_next++ % kChips] = ch;
    }
  }

  // ---- first-person weapon -------------------------------------------------------------------
  const Actor* me = fp_local ? level_.find(local_->actor) : nullptr;
  const bool dead = me && health_.count(me->id) && health_[me->id].dead;
  const bool has_gun = me && me->weapon && !dead;
  if (has_gun && (!art.view_built || art.view_kind != me->weapon->kind)) {
    drop(r_, art.view);
    drop(r_, art.slide);
    art.weapon = models::weapon_model(me->weapon->kind);
    models::PartMeshes parts = art.weapon.parts;
    // Gloves and sleeves hold the grip (in the group frame: grip + mount).
    {
      models::Builder b(parts);
      b.set_transform(Mat4::translate(art.weapon.mount * -1.0f));
      models::view_arms(b, art.weapon.grip + art.weapon.mount);
    }
    art.view = upsert_parts(r_, parts, true, layers_, false);
    art.slide = upsert_parts(r_, art.weapon.slide, true, layers_, false);
    art.view_kind = me->weapon->kind;
    art.view_built = true;
    if (!art.flash_view) art.flash_view = upsert_mesh(r_, models::flash_mesh(), effect(true), layers_);
    if (!art.brass_mesh) {
      Material bm = metal(true);
      bm.roughness = .28f;
      bm.metalness = .85f;
      MeshData brass = models::brass_mesh();
      for (std::size_t i = 0; i < brass.colors.size(); i += 3) { brass.colors[i] = .85f; brass.colors[i + 1] = .64f; brass.colors[i + 2] = .25f; }
      art.brass_mesh = upsert_mesh(r_, brass, bm, layers_, nullptr, kBrass);
    }
  }
  art.view_valid = false;
  const bool sniper_scoped = has_gun && me->weapon->kind == WeaponKind::sniper && aim_blend_ > .85f;
  const bool show_view = has_gun && !sniper_scoped;
  each(art.view, [&](u32 id) { r_.set_visible(id, show_view); });
  each(art.slide, [&](u32 id) { r_.set_visible(id, show_view); });
  if (art.brass_mesh) r_.set_visible(art.brass_mesh, show_view);
  if (art.flash_view) r_.set_visible(art.flash_view, false);
  if (has_gun) {
    const Camera& c = view_camera_;
    const Vec3 f = normalize(c.target - c.eye, {1, 0, 0});
    const Vec3 r = normalize(cross(f, c.up), {0, -1, 0});
    const Vec3 u = cross(r, f);
    Mat4 cam;  // camera space (x right, y up, z back) -> world
    cam.m[0] = r.x; cam.m[1] = r.y; cam.m[2] = r.z;
    cam.m[4] = u.x; cam.m[5] = u.y; cam.m[6] = u.z;
    cam.m[8] = -f.x; cam.m[9] = -f.y; cam.m[10] = -f.z;
    cam.m[12] = c.eye.x; cam.m[13] = c.eye.y; cam.m[14] = c.eye.z;
    // Rest placement (placeWeapon): bob, recoil, looking far up or down.
    const f32 bs = bob_weight_;
    const f32 bob_x = kern::dm::sin(bob_phase_) * .014f * bs, bob_z = -kern::dm::cos(2 * bob_phase_) * .017f * bs, bob_roll = kern::dm::sin(bob_phase_) * .0044f * bs;
    const f64 age = now_ms - art.local_shot;
    const f32 recoil = age >= 0 && age < 125 ? kern::dm::sin(static_cast<f32>(age) / 125 * kPi) : 0;
    const f32 vertical = clamp((std::fabs(camera_pitch_ * kDegree) - 1.15f) / (kPi / 2 - .012f - 1.15f), 0.0f, 1.0f);
    const Vec3 local{.245f + bob_x * .45f, -.225f + bob_z * .2f - vertical * .02f, -.445f + recoil * .065f + vertical * .025f};
    Mat4 g = Mat4::translate(local) * models::euler({-.045f - recoil * .055f, 0, 0}) * models::euler({0, -.065f, 0}) *
             models::euler({0, 0, -.018f + bob_roll * .35f});
    // The sights look at what the crosshair is on (rest depth from a centre ray every 160 ms).
    if (now_ms - art.rest_resolved > 160) {
      art.rest_resolved = now_ms;
      const physics::RayHit h = physics_.cast_ray(c.eye, f * 180.0f, ~0ull, actor_body(local_->actor));
      art.rest_depth = h.hit ? clamp(h.fraction * 180.0f, 1.2f, 180.0f) : 30.0f;
    }
    const Vec3 want{0, 0, -art.rest_depth};
    art.aim_smooth = art.aim_smooth + (want - art.aim_smooth) * (1 - kern::dm::exp(-22 * dt));
    const Vec3 mount = art.weapon.mount;
    for (int it = 0; it < 2; ++it) {
      const Vec3 grip = g.transform_point(art.weapon.grip + mount), rear = g.transform_point(art.weapon.rear_sight + mount),
                 front = g.transform_point(art.weapon.front_sight + mount);
      const Quat q = rotation_between(normalize(front - rear), normalize(art.aim_smooth - rear));
      g = Mat4::translate(grip) * quat_matrix(q) * Mat4::translate(grip * -1.0f) * g;
    }
    // Kick: 16 ms after the shot, 130 ms, muzzle up and back toward the eye.
    const f64 ka = age - 16;
    const f32 kick = ka >= 0 && ka < 130 ? kern::dm::sin(kPi * static_cast<f32>(ka) / 130) : 0;
    g = g * models::euler({.075f * kick, 0, 0}) * Mat4::translate({0, 0, .035f * kick});
    // Reload (Kern): the weapon dips and tilts while the magazine changes.
    if (hud_.reload >= 0) {
      const f32 rl = kern::dm::sin(clamp(hud_.reload, 0.0f, 1.0f) * kPi);
      g = Mat4::translate({0, -.12f * rl, .04f * rl}) * g * models::euler({-.5f * rl, 0, .35f * rl});
    }
    // Aim down sights: the rear sight .46 m in front of the eye, the sight line on the view axis.
    if (aim_blend_ > 0.001f) {
      const f32 t = aim_blend_;
      const Vec3 rear = g.transform_point(art.weapon.rear_sight + mount), front = g.transform_point(art.weapon.front_sight + mount);
      const Quat q = rotation_between(normalize(front - rear), {0, 0, -1}, t);
      g = Mat4::translate(rear) * quat_matrix(q) * Mat4::translate(rear * -1.0f) * g;
      const Vec3 rear2 = g.transform_point(art.weapon.rear_sight + mount);
      g = Mat4::translate((Vec3{0, 0, -.46f} - rear2) * t) * g;
    }
    const Mat4 world = cam * g;
    art.view_matrix = world;
    art.view_valid = true;
    const Mat4 body = world * Mat4::translate(mount);
    each(art.view, [&](u32 id) { r_.update_instances(id, Span<const f32>(body.m, 16), {}); });
    // Glock slide: back 34 ms, held 26 ms, home over 105 ms.
    f32 slide = 0;
    if (art.weapon.slide_travel > 0 && age >= 0) {
      const f32 a = static_cast<f32>(age);
      if (a < 34) slide = kern::dm::sin(a / 34 * kPi / 2);
      else if (a < 60) slide = 1;
      else if (a < 165) { const f32 v = (a - 60) / 105; slide = (1 - v) * (1 - v) * (1 - .15f * kern::dm::sin(kPi * v)); }
    }
    const Mat4 sm = body * Mat4::translate({0, 0, art.weapon.slide_travel * slide});
    each(art.slide, [&](u32 id) { r_.update_instances(id, Span<const f32>(sm.m, 16), {}); });
    // Brass (group space, gravity 6.5 along -Y, 620 ms).
    if (art.brass_mesh) {
      std::vector<f32> xf, tint;
      for (SoldierArt::Brass& b : art.brass) {
        const f32 t = static_cast<f32>(now_ms - b.born) / 1000.0f;
        const bool live = t >= 0 && t < .62f;
        const Vec3 p = b.pos + b.vel * t + Vec3{0, -3.25f * t * t, 0};
        const Mat4 m = world * Mat4::translate(p) * models::euler(b.rot + b.spin * t);
        xf.insert(xf.end(), m.m, m.m + 16);
        tint.insert(tint.end(), {1, 1, 1, live ? 1.0f : 0.0f});
      }
      r_.update_instances(art.brass_mesh, Span<const f32>(xf.data(), xf.size()), Span<const f32>(tint.data(), tint.size()));
    }
    // Muzzle flash at the muzzle: star + side tongues, (1-t)^1.6 fade, growing 35 %.
    const f32 ft = static_cast<f32>(now_ms - art.flash_start) / art.weapon.flash_ms;
    if (art.flash_view && ft >= 0 && ft < 1 && show_view) {
      const f32 fade = kern::dm::pow(1 - ft, 1.6f), sc = art.flash_size * art.weapon.flash_scale * (1 + .35f * ft);
      const Mat4 m = world * Mat4::translate(art.weapon.muzzle + mount) * models::euler({0, 0, art.flash_roll}) * Mat4::scale({sc, sc, sc});
      const f32 tint[4] = {1, 1, 1, min_of(1.0f, fade * 1.15f)};
      r_.update_instances(art.flash_view, Span<const f32>(m.m, 16), Span<const f32>(tint, 4));
      r_.set_visible(art.flash_view, true);
    }
  }

  // ---- world effects -------------------------------------------------------------------------
  auto ensure = [&](u32& id, MeshData mesh, Material mat, u32 n) { if (!id) id = upsert_mesh(r_, mesh, mat, layers_, nullptr, n); };
  ensure(art.tracer_core, models::tracer_mesh(6), effect(false), 1);
  ensure(art.tracer_halo, models::tracer_mesh(8), effect(false), 1);
  ensure(art.balls, models::ball_mesh(), effect(false), 1);
  ensure(art.flashes, models::flash_mesh(), effect(false), 1);
  {
    Material dm = effect(false, true);
    dm.double_sided = false;
    ensure(art.decals, models::decal_mesh(), dm, 1);
  }
  ensure(art.chips, models::chip_mesh(), [] { Material m = matte(false); m.cast_shadow = false; return m; }(), 1);
  // Tracers: the local one travels (240 m/s, sniper 850) as a 9 m segment and fades 130 ms after
  // arriving; remote ones are drawn full length and fade over 160 ms.
  {
    std::vector<f32> core, core_tint, halo, halo_tint;
    for (std::size_t i = 0; i < art.tracers.size();) {
      SoldierArt::Tracer& t = art.tracers[i];
      const f32 age = static_cast<f32>(now_ms - t.born);
      const Vec3 d = t.to - t.from;
      const f32 dist = length(d);
      const Vec3 dir = d / max_of(dist, 1e-4f);
      if (t.remote) {
        const f32 u = age / 160;
        if (u >= 1) { art.tracers.erase(art.tracers.begin() + static_cast<std::ptrdiff_t>(i)); continue; }
        const f32 r = .03f * (1 - .7f * u);
        const Mat4 m = Mat4::translate(t.from) * basis_z(dir) * Mat4::scale({r, r, dist});
        core.insert(core.end(), m.m, m.m + 16);
        const Vec3 c = hex(0xffc070);
        core_tint.insert(core_tint.end(), {c.x, c.y, c.z, .9f * (1 - u)});
      } else {
        const f32 head = min_of(dist, age / 1000 * t.speed), tail = max_of(0.0f, head - 9);
        const f32 arrived = age - dist / t.speed * 1000;
        if (arrived > 130) { art.tracers.erase(art.tracers.begin() + static_cast<std::ptrdiff_t>(i)); continue; }
        const f32 fade = arrived > 0 ? max_of(.05f, 1 - arrived / 130) : 1.0f;
        const f32 len = max_of(.01f, head - tail);
        for (int layer = 0; layer < 2; ++layer) {
          const f32 r = (layer ? .06f : .017f) * fade;
          const Mat4 m = Mat4::translate(t.from + dir * tail) * basis_z(dir) * Mat4::scale({r, r, len});
          const Vec3 c = hex(layer ? 0xff9a3c : 0xfff6e0);
          (layer ? halo : core).insert((layer ? halo : core).end(), m.m, m.m + 16);
          (layer ? halo_tint : core_tint).insert((layer ? halo_tint : core_tint).end(), {c.x, c.y, c.z, layer ? .42f : 1.0f});
        }
      }
      ++i;
    }
    auto put = [&](u32 id, const std::vector<f32>& xf, const std::vector<f32>& tint) {
      if (!id) return;
      r_.set_visible(id, !xf.empty());
      if (!xf.empty()) r_.update_instances(id, Span<const f32>(xf.data(), xf.size()), Span<const f32>(tint.data(), tint.size()));
    };
    put(art.tracer_core, core, core_tint);
    put(art.tracer_halo, halo, halo_tint);
    // Impact flashes: .06 -> .28 m over 110 ms.
    std::vector<f32> bx, bt;
    for (std::size_t i = 0; i < art.ball_list.size();) {
      const f32 u = static_cast<f32>(now_ms - art.ball_list[i].born) / 110;
      if (u >= 1) { art.ball_list.erase(art.ball_list.begin() + static_cast<std::ptrdiff_t>(i)); continue; }
      if (u >= 0) {
        const f32 s = .06f + .22f * u;
        const Mat4 m = Mat4::translate(art.ball_list[i].at) * Mat4::scale({s, s, s});
        bx.insert(bx.end(), m.m, m.m + 16);
        const Vec3 c = hex(0xffd9a0);
        bt.insert(bt.end(), {c.x, c.y, c.z, .95f * (1 - u)});
      }
      ++i;
    }
    put(art.balls, bx, bt);
    // Other players' muzzle flashes: 85 ms, pointing along the shot.
    std::vector<f32> fx, ft;
    for (std::size_t i = 0; i < art.flash_list.size();) {
      const f32 u = static_cast<f32>(now_ms - art.flash_list[i].born) / 85;
      if (u >= 1) { art.flash_list.erase(art.flash_list.begin() + static_cast<std::ptrdiff_t>(i)); continue; }
      const f32 s = (.8f + .55f * kern::dm::sin(kPi * u)) * 1.6f;
      // The flash mesh points along -Z: turn -Z onto the shot direction.
      const Mat4 m = Mat4::translate(art.flash_list[i].at) * basis_z(art.flash_list[i].dir * -1.0f) * Mat4::scale({s, s, s});
      fx.insert(fx.end(), m.m, m.m + 16);
      ft.insert(ft.end(), {1, 1, 1, .95f * (1 - u)});
      ++i;
    }
    put(art.flashes, fx, ft);
  }
  // Decals (ring buffer of 420; only the written part is drawn).
  if (art.decals && art.decals_dirty) {
    art.decals_dirty = false;
    r_.set_visible(art.decals, art.decal_count > 0);
    if (art.decal_count)
      r_.update_instances(art.decals, Span<const f32>(art.decal_xf.data(), art.decal_count * 16), Span<const f32>(art.decal_tint.data(), art.decal_count * 4));
  } else if (art.decals && art.decal_count == 0) {
    r_.set_visible(art.decals, false);
  }
  // Chips: gravity, a bounce on the floor, spin, shrinking at the end of their life.
  if (art.chips) {
    std::vector<f32> xf, tint;
    for (std::size_t i = 0; i < art.chip_list.size();) {
      SoldierArt::Chip& ch = art.chip_list[i];
      ch.age += dt;
      if (ch.age >= ch.life) { art.chip_list.erase(art.chip_list.begin() + static_cast<std::ptrdiff_t>(i)); continue; }
      ch.vel.z -= 9.81f * dt;
      ch.pos += ch.vel * dt;
      if (ch.pos.z < ch.floor && ch.vel.z < 0) { ch.pos.z = ch.floor; ch.vel = ch.vel * .35f; ch.vel.z = std::fabs(ch.vel.z) * .3f; ch.spin = ch.spin * .5f; }
      ch.rot += ch.spin * dt;
      const f32 s = ch.size * min_of(1.0f, (ch.life - ch.age) * 3);
      const Mat4 m = Mat4::translate(ch.pos) * models::euler(ch.rot) * Mat4::scale({s, s, s});
      xf.insert(xf.end(), m.m, m.m + 16);
      tint.insert(tint.end(), {ch.color.x, ch.color.y, ch.color.z, 1});
      ++i;
    }
    r_.set_visible(art.chips, !xf.empty());
    if (!xf.empty()) r_.update_instances(art.chips, Span<const f32>(xf.data(), xf.size()), Span<const f32>(tint.data(), tint.size()));
  }
}

}  // namespace kern::game
