// The soldier: a first-person shooter player on Box3D's character mover.
//
// Movement, look, weapon and health follow the on-foot controller of
// SpawnCity (walk 5.5 m/s, sprint 8.1 m/s, 3.6 m/s jump with a 140 ms jump
// buffer, head bob and shot camera impulses, gamepad response curves, touch
// look) and add what a complete shooter needs: crouch with headroom checks,
// coyote time, magazines and reloads, spread that blooms with movement and
// firing, aiming down sights, headshots, fall damage, regeneration, respawns
// with spawn protection and server-side lag compensation.
//
// Everything that decides the game (movement, hits, damage) runs in fixed
// steps on the authority (the standalone game or the network server); the
// camera feel (bob, recoil kick, shake, field of view) is local presentation.
#include <algorithm>
#include <cmath>

#include "kern/game/geometry.h"
#include "kern/game/runtime.h"
#include "runtime_internal.h"

namespace kern::game {

using rt::kDegree;

namespace {

constexpr f32 kCoyote = 0.12f;       // s a jump still works after walking off a ledge
constexpr f32 kJumpBuffer = 0.14f;   // s a jump press waits for the ground
constexpr f32 kFallSafe = 7.7f;      // m/s landing speed without damage
constexpr f32 kFallLethal = 15.0f;   // m/s landing speed that kills
constexpr f32 kSpawnProtection = 2.5f;

Vec3 aim_direction(f32 yaw_deg, f32 pitch_deg) {
  const f32 y = yaw_deg * kDegree, p = pitch_deg * kDegree;
  return {std::cos(p) * std::cos(y), std::cos(p) * std::sin(y), -std::sin(p)};
}

/// Ray against a vertical capsule (feet, height, radius): distance along the unit ray or -1.
f32 ray_capsule(Vec3 o, Vec3 d, Vec3 feet, f32 height, f32 radius) {
  const Vec3 a = feet + Vec3{0, 0, radius}, b = feet + Vec3{0, 0, max_of(radius, height - radius)};
  f32 best = -1;
  // Cylinder part (infinite, then clipped to the segment).
  const Vec3 ao = o - a;
  const f32 dx = d.x, dy = d.y, ox = ao.x, oy = ao.y;
  const f32 qa = dx * dx + dy * dy, qb = 2 * (dx * ox + dy * oy), qc = ox * ox + oy * oy - radius * radius;
  if (qa > 1e-9f) {
    const f32 disc = qb * qb - 4 * qa * qc;
    if (disc >= 0) {
      const f32 t = (-qb - std::sqrt(disc)) / (2 * qa);
      const f32 z = o.z + d.z * t;
      if (t >= 0 && z >= a.z && z <= b.z) best = t;
    }
  }
  for (const Vec3& c : {a, b}) {
    const Vec3 oc = o - c;
    const f32 bb = dot(oc, d), cc = dot(oc, oc) - radius * radius;
    const f32 disc = bb * bb - cc;
    if (disc < 0) continue;
    const f32 t = -bb - std::sqrt(disc);
    if (t >= 0 && (best < 0 || t < best)) best = t;
  }
  return best;
}

/// Rotation of `v` about the unit `axis` by `radians`.
Vec3 rotate_about(Vec3 v, Vec3 axis, f32 radians) {
  const f32 c = std::cos(radians), s = std::sin(radians);
  return v * c + cross(axis, v) * s + axis * (dot(axis, v) * (1 - c));
}

/// Box part of the view model (camera space: x right, y up, z forward).
void add_box(MeshData& out, Vec3 center, Vec3 size, Vec3 color) {
  MeshComponent box;
  box.shape = Shape::box;
  box.size = size;
  const MeshData m = shape_mesh(box);
  const u32 base = out.vertex_count();
  for (u32 i = 0; i < m.vertex_count(); ++i) {
    // shape_mesh is Z-up (x forward); the view model uses x right, y up, z forward:
    const Vec3 p{m.positions[i * 3], m.positions[i * 3 + 1], m.positions[i * 3 + 2]};
    const Vec3 n{m.normals[i * 3], m.normals[i * 3 + 1], m.normals[i * 3 + 2]};
    out.positions.insert(out.positions.end(), {center.x + p.y, center.y + p.z, center.z + p.x});
    out.normals.insert(out.normals.end(), {n.y, n.z, n.x});
    out.colors.insert(out.colors.end(), {color.x, color.y, color.z});
    out.uvs.insert(out.uvs.end(), {0, 0});
  }
  if (m.indices.empty()) for (u32 i = 0; i < m.vertex_count(); ++i) out.indices.push_back(base + i);
  else for (u32 i : m.indices) out.indices.push_back(base + i);
}

}  // namespace

// ---------------------------------------------------------------------------
// health

f32 GameWorld::health_of(ActorId id) const {
  const auto it = health_.find(id);
  return it == health_.end() ? -1.0f : (it->second.dead ? 0.0f : it->second.hp);
}

void GameWorld::set_health(ActorId id, f32 health, f32 armor, bool dead) {
  HealthState& h = health_[id];
  h.hp = health;
  h.armor = armor;
  h.dead = dead;
}

void GameWorld::damage(ActorId target, f32 amount, ActorId attacker, bool head, Vec3 from) {
  auto it = health_.find(target);
  if (it == health_.end() || it->second.dead || !(amount > 0)) return;
  HealthState& h = it->second;
  Pawn* victim = pawn_of(target);
  if (victim && sim_time_ < victim->protected_until) return;
  const Actor* a = level_.find(target);
  const HealthComponent hc = a && a->health ? *a->health : HealthComponent{};
  const f32 absorbed = min_of(h.armor, amount * 0.66f);
  h.armor -= absorbed;
  h.hp -= amount - absorbed;
  h.last_damage = sim_time_;
  const Vec3 at = world_.count(target) ? world_[target].translation() : from;
  cue(hc.hit_sound, at + Vec3{0, 0, 1}, head ? 1.0f : 0.7f, head ? 1.35f : 1.0f, true, audio::Bus::sfx, target);
  Pawn* shooter = attacker ? pawn_of(attacker) : nullptr;
  if (shooter && shooter == local_) {
    hud_.hit_age = 0;
    hud_.hit_head = head;
    hud_.hit_kill = false;
  }
  if (victim && victim == local_) {
    hud_.damage_age = 0;
    const Vec3 d = from - at;
    hud_.damage_yaw = rt::wrap180(std::atan2(d.y, d.x) / kDegree - victim->yaw);
  }
  if (h.hp > 0) return;
  // Down.
  h.hp = 0;
  h.dead = true;
  cue(hc.death_sound, at + Vec3{0, 0, 1}, 1, 1, true, audio::Bus::sfx, target);
  if (shooter && shooter == local_) hud_.hit_kill = true;
  if (shooter && hud_.status == GameStatus::playing) hud_.score += hc.points;
  const std::string who = a ? a->name : std::string("target");
  if (shooter) say(who + (head ? " down - headshot!" : " down"));
  if (victim) {
    victim->respawn_at = sim_time_ + max_of(0.5f, hc.respawn_delay);
    if (victim == local_) hud_.respawn_in = hc.respawn_delay;
  } else {
    burst(at + Vec3{0, 0, 0.8f}, Vec3{1.6f, 0.5f, 0.2f});
    set_actor_alive(target, false);
    h.respawn_at = hc.respawn ? sim_time_ + max_of(0.1f, hc.respawn_delay) : -1;
  }
}

void GameWorld::update_health(f32 dt) {
  for (auto& [id, h] : health_) {
    const Actor* a = level_.find(id);
    const HealthComponent hc = a && a->health ? *a->health : HealthComponent{};
    if (h.dead) {
      if (pawn_of(id)) continue;  // pawns respawn below
      if (h.respawn_at >= 0 && sim_time_ >= h.respawn_at) {
        h = HealthState{hc.max_health, hc.armor};
        set_actor_alive(id, true);
      }
      continue;
    }
    if (hc.regen_delay > 0 && hc.regen_rate > 0 && sim_time_ - h.last_damage > hc.regen_delay)
      h.hp = min_of(hc.max_health, h.hp + hc.regen_rate * dt);
  }
  for (auto& pawn : pawns_) {
    if (pawn->respawn_at < 0 || sim_time_ < pawn->respawn_at) continue;
    pawn->respawn_at = -1;
    const Actor* a = level_.find(pawn->actor);
    const HealthComponent hc = a && a->health ? *a->health : HealthComponent{};
    health_[pawn->actor] = HealthState{hc.max_health, hc.armor};
    pawn->protected_until = sim_time_ + kSpawnProtection;
    if (a && a->weapon) { pawn->mag = a->weapon->magazine; pawn->reserve = a->weapon->reserve; }
    pawn->reload_until = -1;
    Pawn* keep = pw_;
    pw_ = pawn.get();
    respawn_player();
    pw_ = keep;
    if (pawn.get() == local_) hud_.respawn_in = -1;
  }
}

// ---------------------------------------------------------------------------
// movement and weapon (fixed steps on the authority)

void GameWorld::drive_soldier(const PlayInput& in, f32 dt) {
  const Actor* a = level_.find(pw_->actor);
  auto it = body_of_actor_.find(pw_->actor);
  if (!a || !a->controller || it == body_of_actor_.end() || !it->second->body.valid()) return;
  const ControllerComponent& c = *a->controller;
  const physics::Body body = it->second->body;
  if (!pw_->use_mover) possess_character(true);
  if (!pw_->use_mover) return;
  physics::CharacterMover& mover = pw_->mover;
  const auto hs = health_.find(pw_->actor);
  const bool dead = hs != health_.end() && hs->second.dead;
  const bool local = pw_ == local_;
  // History for lag compensation (feet over the last second).
  pw_->history.push_back({sim_time_, mover.position()});
  if (pw_->history.size() > 128) pw_->history.erase(pw_->history.begin(), pw_->history.begin() + 32);

  // Camera-relative wish direction.
  const f32 yaw = pw_->yaw * kDegree;
  const Vec3 forward{std::cos(yaw), std::sin(yaw), 0}, right{std::sin(yaw), -std::cos(yaw), 0};
  Vec3 wish = right * clamp(in.move_x, -1.0f, 1.0f) + forward * clamp(in.move_y, -1.0f, 1.0f);
  if (length(wish) > 1) wish = normalize(wish);
  if (dead) wish = {};

  // Crouch (hold), standing up only with headroom.
  const f32 stand = max_of(0.6f, mover.settings.height / max_of(0.4f, 1 - 0.4f * pw_->crouch));
  const bool want_crouch = in.crouch && !dead;
  if (want_crouch) pw_->crouch = min_of(1.0f, pw_->crouch + dt * 8);
  else if (pw_->crouch > 0) {
    const physics::Capsule cap = mover.capsule();
    const f32 rise = stand - mover.settings.height;
    const f32 free = rise > 0.001f ? physics_.cast_mover(mover.position(), cap, Vec3{0, 0, rise}, mover.settings.mask_bits, body) : 1.0f;
    if (free > 0.99f) pw_->crouch = max_of(0.0f, pw_->crouch - dt * 8);
  }
  mover.settings.height = stand * (1 - 0.4f * pw_->crouch);
  mover.settings.step_height = clamp(mover.settings.height * 0.22f, 0.05f, max_of(0.05f, mover.settings.height - 2 * mover.settings.radius - 0.02f));

  // Speeds: walk, sprint (forward only, not while aiming or crouched), crouch, aim.
  const bool sprint = in.boost && c.boost && in.move_y > 0.3f && !in.aim && pw_->crouch < 0.5f && !dead;
  const f32 walk = max_of(0.1f, c.max_speed);
  mover.settings.max_speed = pw_->crouch > 0.5f ? min_of(walk, c.crouch_speed) : in.aim ? walk * 0.6f : walk;
  mover.settings.sprint = sprint ? max_of(1.0f, c.boost_speed / mover.settings.max_speed) : 1.0f;

  // Jump: buffered presses and coyote time.
  const bool jump_pressed = in.jump && !pw_->jump_latch && !dead;
  pw_->jump_latch = in.jump;
  if (jump_pressed) pw_->jump_buffer = kJumpBuffer;
  else pw_->jump_buffer = max_of(0.0f, pw_->jump_buffer - dt);
  pw_->coyote = mover.on_ground() ? kCoyote : max_of(0.0f, pw_->coyote - dt);
  if (pw_->jump_buffer > 0 && pw_->coyote > 0 && c.jump_speed > 0 && pw_->crouch < 0.5f) {
    mover.jump(c.jump_speed);
    pw_->jump_buffer = 0;
    pw_->coyote = 0;
    cue(c.jump_sound, mover.position(), 0.5f, 1, true, audio::Bus::sfx, pw_->actor);
  }
  const bool was_ground = mover.on_ground();
  const f32 fall = -mover.velocity().z;
  mover.step(physics_, dt, wish, false, sprint, body);
  if (mover.on_ground() && !was_ground) {
    if (fall > 3.5f) cue("builtin:land", mover.position(), clamp(fall / 14.0f, 0.25f, 1.0f), 1, true, audio::Bus::sfx, pw_->actor);
    if (local) land_dip_ = min_of(0.12f, 0.02f + fall * 0.008f), shake_energy_ = min_of(1.35f, shake_energy_ + 0.12f + 0.05f * fall);
    if (fall > kFallSafe && !dead) damage(pw_->actor, 100 * (fall - kFallSafe) / (kFallLethal - kFallSafe), 0, false, mover.position());
  }
  // Face the view (the body turns with the camera).
  physics_.set_target_pose(body, physics::Pose{mover.position() + Vec3{0, 0, pw_->mover_lift}, Quat::axis_angle({0, 0, 1}, yaw)}, dt);
  // Footsteps on an interval that shortens with speed.
  const Vec3 hv{mover.velocity().x, mover.velocity().y, 0};
  const f32 speed = length(hv);
  if (mover.on_ground() && speed > 0.55f) {
    pw_->stride += dt;
    const f32 interval = clamp(0.50f - 0.025f * speed, 0.27f, 0.48f) * (pw_->crouch > 0.5f ? 1.4f : 1.0f);
    if (pw_->stride >= interval) {
      pw_->stride = 0;
      ++pw_->steps;
      const f32 vary = 0.9f + 0.2f * static_cast<f32>((pw_->steps * 2654435761u >> 16) % 1000) / 1000.0f;
      cue("builtin:step", mover.position(), clamp(0.2f + speed * 0.05f, 0.2f, 0.6f) * (pw_->crouch > 0.5f ? 0.5f : 1.0f), vary, true, audio::Bus::sfx, pw_->actor);
    }
  } else {
    pw_->stride = 0.2f;
  }
  for (const physics::MoverPlane& mp : mover.planes()) touched(mp.shape, mp.body);

  // Weapon.
  if (a->weapon && !dead) {
    const WeaponComponent& w = *a->weapon;
    if (pw_->reload_until >= 0 && sim_time_ >= pw_->reload_until) {
      const i32 take = min_of(w.magazine - pw_->mag, pw_->reserve);
      pw_->mag += take;
      pw_->reserve -= take;
      pw_->reload_until = -1;
    }
    const bool reloading = pw_->reload_until >= 0;
    const bool want_reload = (in.reload || (in.fire && pw_->mag == 0)) && !reloading && pw_->mag < w.magazine && pw_->reserve > 0;
    if (want_reload) {
      pw_->reload_until = sim_time_ + w.reload_time;
      cue(w.reload_sound, mover.position() + Vec3{0, 0, 1.2f}, 0.8f, 1, true, audio::Bus::sfx, pw_->actor);
    }
    const bool trigger = w.automatic ? in.fire : (in.fire && !pw_->fire_latch);
    if (in.fire && !pw_->fire_latch && pw_->mag == 0 && pw_->reserve == 0) cue("builtin:click", mover.position() + Vec3{0, 0, 1.2f}, 0.6f, 1, true, audio::Bus::sfx, pw_->actor);
    pw_->fire_latch = in.fire;
    if (trigger && !sprint && pw_->reload_until < 0 && pw_->mag > 0 && sim_time_ >= pw_->next_fire) fire_weapon(in, w);
    // Spread: base by stance, bloom from movement, air and shots (decays).
    f32 base = in.aim ? w.aim_spread : w.spread;
    base *= 1 + clamp(speed / max_of(1.0f, walk), 0.0f, 1.6f) * (in.aim ? 0.5f : 1.0f);
    if (!mover.on_ground()) base *= 2.5f;
    if (pw_->crouch > 0.5f) base *= 0.7f;
    pw_->spread = rt::damp(pw_->spread, base, 10, dt);
  }
  if (local) {
    hud_.soldier = true;
    hud_.speed = speed;
    hud_.aiming = in.aim && !sprint && !dead;
    if (a->weapon) {
      hud_.ammo = pw_->mag;
      hud_.magazine = a->weapon->magazine;
      hud_.reserve = pw_->reserve;
      hud_.reload = pw_->reload_until >= 0 ? clamp(1 - (pw_->reload_until - sim_time_) / max_of(0.01f, a->weapon->reload_time), 0.0f, 1.0f) : -1;
      hud_.spread = pw_->spread;
    }
    if (hs != health_.end()) {
      hud_.health = hs->second.dead ? 0 : hs->second.hp;
      hud_.max_health = a->health ? a->health->max_health : 100;
      hud_.armor = hs->second.armor;
    }
    if (pw_->respawn_at >= 0) hud_.respawn_in = max_of(0.0f, pw_->respawn_at - sim_time_);
  }
}

void GameWorld::fire_weapon(const PlayInput& in, const WeaponComponent& w) {
  physics::CharacterMover& mover = pw_->mover;
  const physics::Body self = body_of_actor_[pw_->actor]->body;
  pw_->next_fire = sim_time_ + 60.0f / max_of(1.0f, w.fire_rate);
  --pw_->mag;
  ++pw_->shots;
  const f32 eye_h = (a_eye_height(pw_->actor)) * (1 - 0.4f * pw_->crouch);
  const Vec3 eye = mover.position() + Vec3{0, 0, eye_h};
  const Vec3 aim = aim_direction(pw_->yaw, pw_->pitch);
  const Vec3 side = normalize(cross(aim, {0, 0, 1}), {0, 1, 0});
  const Vec3 up = cross(side, aim);
  const Vec3 muzzle = eye + aim * 0.6f + side * 0.12f - up * 0.12f;
  cue(w.fire_sound, muzzle, 1, 0.95f + 0.1f * (rt::noise(pw_->shots * 7 + static_cast<u32>(pw_->actor)) * 0.5f + 0.5f), true, audio::Bus::sfx, pw_->actor);
  const f32 rewind = clamp(in.rewind, 0.0f, 0.5f);
  for (i32 pellet = 0; pellet < max_of(1, w.pellets); ++pellet) {
    // Deterministic cone: uniform over the disc of the spread angle.
    const u32 seed = static_cast<u32>(pw_->actor) * 7919u + pw_->shots * 104729u + static_cast<u32>(pellet) * 31u;
    const f32 r = std::sqrt(rt::noise(seed) * 0.5f + 0.5f) * pw_->spread * kDegree;
    const f32 t = (rt::noise(seed + 1) * 0.5f + 0.5f) * 6.2831853f;
    const Vec3 dir = normalize(rotate_about(rotate_about(aim, up, r * std::cos(t)), side, r * std::sin(t)));
    // World hit (Box3D ray from the eye; the shooter's own body is skipped).
    physics::RayHit hit = physics_.cast_ray(eye, dir * w.range, ~0ull, self);
    f32 best = hit.hit ? hit.fraction * w.range : w.range;
    ActorId target = 0;
    bool head = false;
    Vec3 point = eye + dir * best;
    // Other pawns where the shooter saw them (lag compensation), as capsules.
    for (auto& other : pawns_) {
      if (other.get() == pw_ || other->history.empty()) continue;
      const auto oh = health_.find(other->actor);
      if (oh != health_.end() && oh->second.dead) continue;
      Vec3 feet = other->mover.position();
      if (rewind > 0) {
        const f32 when = sim_time_ - rewind;
        for (std::size_t k = other->history.size(); k-- > 0;)
          if (other->history[k].first <= when) { feet = other->history[k].second; break; }
      }
      if (!other->use_mover) {
        Mat4 m;
        if (actor_matrix(other->actor, m)) feet = m.translation() - Vec3{0, 0, other->mover_lift};
      }
      const f32 height = other->mover.settings.height;
      const f32 d = ray_capsule(eye, dir, feet, height, other->mover.settings.radius + 0.05f);
      if (d >= 0 && d < best) {
        best = d;
        target = other->actor;
        point = eye + dir * d;
        head = point.z > feet.z + height - 0.32f;
        hit.hit = false;  // the pawn is in front of whatever the ray hit
      }
    }
    if (hit.hit) {
      // Actor of the hit shape (a welded child like a head) and the nearest actor with health.
      ActorId hit_actor = static_cast<ActorId>(physics_.shape_user(hit.shape));
      if (!hit_actor) hit_actor = actor_for_body(hit.body);
      for (const Actor* x = level_.find(hit_actor); x; x = level_.find(x->parent)) {
        if (std::find(x->tags.begin(), x->tags.end(), "head") != x->tags.end()) head = true;
        if (health_.count(x->id)) { target = x->id; break; }
      }
      point = hit.point;
      if (physics_.body_valid(hit.body) && physics_.body_type(hit.body) == physics::BodyType::dynamic)
        physics_.apply_linear_impulse(hit.body, dir * w.impulse, hit.point);
    }
    if (target) damage(target, w.damage * (head ? w.headshot : 1.0f), pw_->actor, head, eye);
    ShotEvent s;
    s.from = muzzle;
    s.to = point;
    s.shooter = pw_->actor;
    s.hit = target;
    s.impact = best < w.range;
    s.head = head;
    if (shots_.size() < 256) shots_.push_back(s);
  }
  // Bloom and (local) camera kick.
  pw_->spread += w.recoil * 0.35f;
  if (pw_ == local_) {
    recoil_pitch_ = min_of(0.05f + w.recoil * kDegree, recoil_pitch_ + 0.016f + w.recoil * kDegree * 0.5f);
    recoil_yaw_ = clamp(recoil_yaw_ + 0.0042f * rt::noise(pw_->shots * 13u), -0.012f, 0.012f);
    recoil_roll_ = clamp(recoil_roll_ - 0.006f * rt::noise(pw_->shots * 17u + 3), -0.018f, 0.018f);
    shake_energy_ = min_of(1.35f, shake_energy_ + 0.78f);
    flash_until_ = now_ms_ + 45;
  }
}

f32 GameWorld::a_eye_height(ActorId actor) const {
  const Actor* a = level_.find(actor);
  return a && a->controller ? a->controller->eye_height : 1.62f;
}

// ---------------------------------------------------------------------------
// camera (local presentation)

void GameWorld::soldier_camera(Camera& cam, f32 dt, const PlayInput& in, const ControllerComponent& c) {
  Pawn* p = local_;
  if (!p) return;
  const Actor* a = level_.find(p->actor);
  const WeaponComponent* w = a && a->weapon ? &*a->weapon : nullptr;
  const bool aiming = hud_.aiming && w;
  const f32 zoom = aiming ? std::tan(c.camera_fov * 0.5f * kDegree) / std::tan(w->aim_fov * 0.5f * kDegree) : 1.0f;
  const f32 sens = c.look_sensitivity / max_of(1.0f, zoom);
  // Mouse / touch deltas arrive in degrees.
  camera_yaw_ -= in.look_x * sens;
  camera_pitch_ += in.look_y * sens;
  // Gamepad right stick: radial deadzone, outer saturation, a dynamic response curve and rate ramping.
  {
    f32 sx = in.look_stick_x, sy = in.look_stick_y;
    const f32 m = std::sqrt(sx * sx + sy * sy);
    f32 mag = 0;
    if (m > 0.05f) {
      const f32 n = clamp((m - 0.05f) / (0.97f - 0.05f), 0.0f, 1.0f);
      // Hermite through (0,0) (.3,.24) (.7,.5) (.9,.89) (1,1), blended with linear.
      const f32 xs[5] = {0, 0.3f, 0.7f, 0.9f, 1}, ys[5] = {0, 0.24f, 0.5f, 0.89f, 1};
      f32 curve = 1;
      for (int i = 0; i < 4; ++i)
        if (n <= xs[i + 1]) { const f32 t = (n - xs[i]) / (xs[i + 1] - xs[i]); const f32 s = t * t * (3 - 2 * t); curve = ys[i] + (ys[i + 1] - ys[i]) * s; break; }
      mag = clamp(0.045f + (1 - 0.045f) * (0.5f * curve + 0.5f * n), 0.0f, 1.0f);
      sx = sx / m * mag;
      sy = sy / m * mag;
    } else {
      sx = sy = 0;
    }
    const f32 target_x = sx * 3.75f / kDegree * sens, target_y = sy * 3.40f / kDegree * sens;
    const f32 kx = std::fabs(target_x) > std::fabs(look_rate_x_) && target_x * look_rate_x_ >= 0 ? 26.0f : 40.0f;
    const f32 ky = std::fabs(target_y) > std::fabs(look_rate_y_) && target_y * look_rate_y_ >= 0 ? 26.0f : 40.0f;
    look_rate_x_ = rt::damp(look_rate_x_, target_x, kx, dt);
    look_rate_y_ = rt::damp(look_rate_y_, target_y, ky, dt);
    camera_yaw_ -= look_rate_x_ * dt;
    camera_pitch_ += look_rate_y_ * dt;
  }
  camera_pitch_ = clamp(camera_pitch_, -89.3f, 89.3f);
  camera_yaw_ = rt::wrap180(camera_yaw_);
  // Eye: interpolated pawn position, crouch, death fall.
  Mat4 m;
  Vec3 feet = p->mover.position();
  if (actor_matrix(p->actor, m)) feet = m.translation() - Vec3{0, 0, p->mover_lift};
  const auto hs = health_.find(p->actor);
  const bool dead = hs != health_.end() && hs->second.dead;
  f32 eye_h = c.eye_height * (1 - 0.4f * p->crouch);
  f32 roll = 0;
  if (dead) { eye_h = 0.3f; roll = 1.2f; }
  // Head bob (weight follows speed, phase follows speed).
  const Vec3 hv{p->mover.velocity().x, p->mover.velocity().y, 0};
  const f32 speed = length(hv);
  const bool sprinting = speed > c.max_speed * 1.1f;
  const f32 bob_target = c.head_bob && p->mover.on_ground() && speed > 0.12f ? clamp(speed / (sprinting ? 7.2f : 4.8f), 0.0f, 1.0f) : 0.0f;
  bob_weight_ = rt::damp(bob_weight_, bob_target, bob_target > bob_weight_ ? 9.0f : 13.0f, dt);
  bob_phase_ += 6.2831853f * (1.42f + 0.105f * speed) * dt;
  const f32 bs = bob_weight_ * (sprinting ? 1.2f : 1.0f) * (aiming ? 0.25f : 1.0f);
  // Shot impulse and shake decay.
  recoil_pitch_ *= std::exp(-8.5f * dt);
  recoil_yaw_ *= std::exp(-11.5f * dt);
  recoil_roll_ *= std::exp(-13.0f * dt);
  shake_energy_ *= std::exp(-15.0f * dt);
  land_dip_ *= std::exp(-9.0f * dt);
  const f32 sp = bob_phase_;
  const f32 sh = shake_energy_ * shake_energy_;
  const f32 noise_t = static_cast<f32>(now_ms_ * 0.054);
  const f32 yaw = camera_yaw_ * kDegree + std::sin(sp) * 0.0017f * bs + recoil_yaw_ + std::sin(noise_t * 1.3f) * 0.0015f * sh;
  const f32 pitch = camera_pitch_ * kDegree - std::sin(2 * sp) * 0.0031f * bs - recoil_pitch_ + std::sin(noise_t * 1.7f) * 0.0012f * sh;
  roll += std::sin(sp) * 0.0044f * bs + recoil_roll_ + std::sin(noise_t * 1.1f) * 0.0028f * sh;
  const Vec3 dir{std::cos(pitch) * std::cos(yaw), std::cos(pitch) * std::sin(yaw), -std::sin(pitch)};
  const Vec3 side = normalize(cross(dir, {0, 0, 1}), {0, 1, 0});
  Vec3 eye = feet + Vec3{0, 0, eye_h - land_dip_} + side * (std::sin(sp) * 0.014f * bs) + Vec3{0, 0, -std::cos(2 * sp) * 0.017f * bs};
  eye += side * (std::sin(noise_t) * 0.0024f * sh) + Vec3{0, 0, std::sin(noise_t * 0.9f) * 0.002f * sh};
  cam.eye = eye;
  cam.target = eye + dir * 10;
  cam.up = rotate_about(Vec3{0, 0, 1}, dir, roll);
  cam.near = 0.03f;
  cam.far = 4000;
  cam.orthographic = false;
  // Field of view: aim zooms in (75 ms), sprint widens a little.
  const f32 fov_target = aiming ? w->aim_fov : c.camera_fov + (sprinting ? 5.0f : 0.0f);
  if (fov_now_ <= 0) fov_now_ = fov_target;
  fov_now_ = rt::damp(fov_now_, fov_target, 1 / 0.075f, dt);
  cam.fov = fov_now_ * kDegree;
  hud_.fov = fov_now_;
  camera_eye_ = eye;
  hud_.hit_age += dt;
  hud_.damage_age += dt;
}

// ---------------------------------------------------------------------------
// view model, tracers, muzzle flash

void GameWorld::present_soldier(f64 now_ms) {
  // Hide the local player's own body in first person (the view model replaces it).
  const bool fp = local_ && player_ == local_->actor && mode_ != SimMode::simulate && view_valid_ && level_.find(local_->actor) &&
                  level_.find(local_->actor)->controller && level_.find(local_->actor)->controller->mode == ControlMode::soldier;
  if (local_)
    for (ActorId id : local_->actors)
      if (auto v = visuals_.find(id); v != visuals_.end() && v->second->mesh) {
        const bool show = !fp && v->second->visible;
        r_.set_visible(v->second->mesh, show);
      }
  // View model.
  const Actor* a = local_ ? level_.find(local_->actor) : nullptr;
  const bool has_gun = fp && a && a->weapon && !(health_.count(local_->actor) && health_[local_->actor].dead);
  if (has_gun && !viewmodel_mesh_) {
    MeshData gun;
    const Vec3 dark{0.11f, 0.12f, 0.13f}, metal{0.32f, 0.34f, 0.36f}, accent{0.75f, 0.42f, 0.16f};
    switch (a->weapon->kind) {
      case WeaponKind::pistol:
        add_box(gun, {0, 0.012f, 0.06f}, {0.032f, 0.04f, 0.17f}, metal);
        add_box(gun, {0, -0.045f, 0.0f}, {0.028f, 0.09f, 0.045f}, dark);
        break;
      case WeaponKind::shotgun:
        add_box(gun, {0, 0, 0.1f}, {0.045f, 0.05f, 0.52f}, dark);
        add_box(gun, {0, -0.03f, 0.18f}, {0.04f, 0.035f, 0.22f}, accent);
        add_box(gun, {0, -0.05f, -0.15f}, {0.04f, 0.09f, 0.16f}, accent);
        break;
      default:
        add_box(gun, {0, 0, 0.05f}, {0.045f, 0.06f, 0.42f}, dark);        // receiver
        add_box(gun, {0, 0.008f, 0.36f}, {0.022f, 0.022f, 0.24f}, metal);  // barrel
        add_box(gun, {0, -0.07f, 0.08f}, {0.035f, 0.09f, 0.05f}, metal);   // magazine
        add_box(gun, {0, -0.055f, -0.03f}, {0.03f, 0.07f, 0.035f}, dark);  // grip
        add_box(gun, {0, -0.01f, -0.2f}, {0.04f, 0.07f, 0.16f}, accent);   // stock
        add_box(gun, {0, 0.05f, 0.02f}, {0.02f, 0.03f, 0.08f}, dark);      // sight
        if (a->weapon->kind == WeaponKind::sniper) add_box(gun, {0, 0.065f, 0.05f}, {0.035f, 0.035f, 0.2f}, metal);
        break;
    }
    MeshInput in = gun.as_input(kMaxExtensionAttributes);
    in.material.roughness = 0.45f;
    in.material.metalness = 0.4f;
    in.layers = layers_;
    in.material.cast_shadow = false;
    const Mat4 id;
    in.transforms = Span<const f32>(id.m, 16);
    viewmodel_mesh_ = r_.allocate_id();
    if (!r_.upsert(viewmodel_mesh_, in)) { r_.release_id(viewmodel_mesh_); viewmodel_mesh_ = 0; }
  }
  if (viewmodel_mesh_) {
    r_.set_visible(viewmodel_mesh_, has_gun);
    if (has_gun) {
      const Camera& c = view_camera_;
      const Vec3 f = normalize(c.target - c.eye, {1, 0, 0});
      const Vec3 r = normalize(cross(f, c.up), {0, -1, 0});
      const Vec3 u = cross(r, f);
      const f32 aim = hud_.aiming ? 1.0f : 0.0f;
      aim_blend_ += (aim - aim_blend_) * 0.25f;
      const f32 aim_blend = aim_blend_;
      const f32 kick = recoil_pitch_ * 1.6f;
      const f32 reload_dip = hud_.reload >= 0 ? std::sin(hud_.reload * 3.14159f) * 0.12f : 0.0f;
      const f32 sway = std::sin(bob_phase_) * 0.006f * bob_weight_;
      const Vec3 hip{0.17f, -0.16f, 0.36f}, ads{0.0f, -0.058f, 0.27f};
      const Vec3 o = hip + (ads - hip) * aim_blend;
      Vec3 pos = c.eye + r * (o.x + sway) + u * (o.y - reload_dip + std::fabs(sway) * 0.5f) + f * (o.z - kick * 0.5f);
      // Columns: x = right, y = up (kicked), z = forward.
      const Vec3 fwd = normalize(f + u * (kick + reload_dip * 0.8f));
      const Vec3 up = normalize(cross(cross(fwd, u), fwd) * -1.0f);
      Mat4 m;
      m.m[0] = r.x; m.m[1] = r.y; m.m[2] = r.z;
      m.m[4] = up.x; m.m[5] = up.y; m.m[6] = up.z;
      m.m[8] = fwd.x; m.m[9] = fwd.y; m.m[10] = fwd.z;
      m.m[12] = pos.x; m.m[13] = pos.y; m.m[14] = pos.z;
      r_.update_instances(viewmodel_mesh_, Span<const f32>(m.m, 16), {});
    }
  }
  // Tracers and impacts of this frame's shots (from the simulation or the network).
  for (const ShotEvent& s : shots_) {
    if (particles_ready_) {
      const Vec3 d = s.to - s.from;
      const f32 len = length(d);
      const int n = clamp(static_cast<int>(len / 1.5f), 2, 24);
      for (int i = 0; i < n; ++i) {
        ParticleSpec t;
        t.position = s.from + d * (static_cast<f32>(i) / static_cast<f32>(n));
        t.velocity = d / max_of(0.001f, len) * 30.0f;
        t.color = {1.0f, 0.86f, 0.55f, 0.9f};
        t.start_ms = now_ms + i * 1.2;
        t.life_ms = 70;
        t.size = 0.06f;
        particles_.emit(t);
      }
      ParticleSpec flash;
      flash.position = s.from;
      flash.color = {1.0f, 0.75f, 0.35f, 1};
      flash.start_ms = now_ms;
      flash.life_ms = 45;
      flash.size = 0.28f;
      flash.grow = 0.6f;
      particles_.emit(flash);
      if (s.impact) {
        Rng rng(static_cast<u32>(now_ms) + static_cast<u32>(s.to.x * 977));
        for (int i = 0; i < 6; ++i) {
          ParticleSpec spark;
          spark.position = s.to;
          spark.velocity = Vec3{rng.range(-2, 2), rng.range(-2, 2), rng.range(0.5f, 3)};
          spark.color = s.hit ? Vec4{1.0f, 0.2f, 0.15f, 1} : Vec4{1.0f, 0.8f, 0.5f, 1};
          spark.start_ms = now_ms;
          spark.life_ms = rng.range(120, 260);
          spark.size = s.head ? 0.12f : 0.07f;
          spark.gravity = 9;
          particles_.emit(spark);
        }
      }
    }
    if (s.shooter && (!local_ || s.shooter != local_->actor)) flash_until_ = max_of(flash_until_, now_ms + 30), flash_at_ = s.from;
    else flash_at_ = s.from;
  }
  shots_.clear();
}

}  // namespace kern::game
