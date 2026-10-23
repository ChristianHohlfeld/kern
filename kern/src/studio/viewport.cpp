// Kern Studio viewports: cameras with Unreal-style navigation, picking,
// selection by click and marquee, playing in the viewport, and rendering.
#include <algorithm>
#include <cmath>
#include <cstdio>

#include "internal.h"
#include "kern/studio/studio.h"

namespace kern::studio {

namespace {

constexpr f32 kSpeeds[8] = {1, 2.5f, 5, 10, 20, 40, 80, 160};  // m/s per camera speed setting

enum PointerType : u32 { down = 0, move = 1, up = 2, cancel = 3, wheel = 4, leave = 5 };
enum Mods : u32 { shift = 1, ctrl = 2, alt = 4 };

/// Orthographic view direction (from the camera into the scene) and up vector.
void ortho_basis(ViewType t, Vec3& dir, Vec3& up) {
  switch (t) {
    case ViewType::top: dir = {0, 0, -1}; up = {0, 1, 0}; break;
    case ViewType::bottom: dir = {0, 0, 1}; up = {0, 1, 0}; break;
    case ViewType::front: dir = {0, 1, 0}; up = {0, 0, 1}; break;
    case ViewType::back: dir = {0, -1, 0}; up = {0, 0, 1}; break;
    case ViewType::left: dir = {1, 0, 0}; up = {0, 0, 1}; break;
    case ViewType::right: dir = {-1, 0, 0}; up = {0, 0, 1}; break;
    default: dir = {0, 1, 0}; up = {0, 0, 1}; break;
  }
}

}  // namespace

f32 Studio::camera_speed() const {
  const i32 s = clamp(prefs_.camera_speed, 1, 8);
  return kSpeeds[s - 1];
}

Camera Studio::camera_of(const Viewport& v) const {
  Camera c;
  if (v.type == ViewType::perspective) {
    c.eye = v.camera.position;
    c.target = v.camera.position + v.camera.forward();
    c.up = {0, 0, 1};
    c.fov = clamp(v.camera.fov + v.fov_boost, 10.0f, 150.0f) * kDegToRad;
    c.near = 0.05f;
    c.far = 6000;
    if (std::fabs(v.camera.pitch) > 89.5f) c.up = v.camera.right() * -1.0f;  // avoid a degenerate basis straight up/down
    return c;
  }
  Vec3 dir, up;
  ortho_basis(v.type, dir, up);
  c.orthographic = true;
  c.ortho_height = max_of(0.01f, v.camera.ortho_height);
  c.target = v.camera.ortho_center;
  c.eye = v.camera.ortho_center - dir * 3000;
  c.up = up;
  c.near = 1;
  c.far = 6000;
  return c;
}

bool Studio::ray(const Viewport& v, f32 x, f32 y, Ray& out) const {
  Camera c = camera_of(v);
  if (play_ == PlayState::playing && !ejected_ && v.index == active_view_ && play_camera_valid_) c = play_camera_;
  return screen_ray(c, x, y, v.css_width, v.css_height, out);
}

bool Studio::to_screen(const Viewport& v, Vec3 p, f32& x, f32& y) const {
  Camera c = camera_of(v);
  if (play_ == PlayState::playing && !ejected_ && v.index == active_view_ && play_camera_valid_) c = play_camera_;
  bool ok = false;
  const Mat4 view = Mat4::look_at(c.eye, c.target, c.up, &ok);
  if (!ok) return false;
  const Vec4 clip = (projection_matrix(c, v.css_width / max_of(1.0f, v.css_height)) * view).transform({p.x, p.y, p.z, 1});
  if (clip.w <= 1e-6f) return false;
  x = (clip.x / clip.w * 0.5f + 0.5f) * v.css_width;
  y = (0.5f - clip.y / clip.w * 0.5f) * v.css_height;
  return true;
}

void Studio::set_viewport_rect(u32 index, Rect rect, f32 css_width, f32 css_height, f32 dpr, bool visible) {
  if (index >= 4) return;
  Viewport& v = views_[index];
  v.rect = rect;
  v.css_width = max_of(1.0f, css_width);
  v.css_height = max_of(1.0f, css_height);
  v.dpr = dpr > 0 ? dpr : 1;
  v.visible = visible && rect.w > 0 && rect.h > 0;
}

// ---------------------------------------------------------------------------
// picking

ActorId Studio::pick(u32 index, f32 x, f32 y, Vec3* hit_point) {
  ensure_world_synced();
  Viewport& v = viewport(index);
  Ray r;
  if (!ray(v, x, y, r)) return 0;
  // Actors without meshes (lights, empties) are picked by their icon on screen.
  ActorId icon = 0;
  f32 best = 14;
  for (const game::Actor& a : level_.actors) {
    if (a.mesh || a.city || a.locked || a.hidden) continue;
    Mat4 m;
    if (!world_->actor_matrix(a.id, m)) continue;
    f32 sx, sy;
    if (!to_screen(v, m.translation(), sx, sy)) continue;
    const f32 d = std::hypot(sx - x, sy - y);
    if (d < best) { best = d; icon = a.id; }
  }
  PickQuery q;
  q.layers = layer::scene;
  PickHit hit;
  ActorId mesh_actor = 0;
  // Skip locked actors and the ground by continuing the ray past them.
  for (int attempt = 0; attempt < 12; ++attempt) {
    if (!r_->pick_ray(r, q, hit)) break;
    const ActorId id = world_->actor_for_mesh(hit.mesh);
    const game::Actor* a = level_.find(id);
    if (a && !a->locked) {
      mesh_actor = id;
      if (hit_point) *hit_point = hit.point;
      break;
    }
    q.ignore_id = hit.mesh;  // the ground or a locked actor: look behind it
    r.origin = hit.point + r.direction * 0.001f;
  }
  if (icon) {
    if (hit_point && !mesh_actor) { Mat4 m; world_->actor_matrix(icon, m); *hit_point = m.translation(); }
    return icon;
  }
  return mesh_actor;
}

// ---------------------------------------------------------------------------
// input

game::PlayInput Studio::play_input() const {
  game::PlayInput in;
  auto held = [&](const char* code) { return keys_.count(code) > 0; };
  in.move_y = (held("KeyW") || held("ArrowUp") ? 1.0f : 0.0f) - (held("KeyS") || held("ArrowDown") ? 1.0f : 0.0f);
  in.move_x = (held("KeyD") || held("ArrowRight") ? 1.0f : 0.0f) - (held("KeyA") || held("ArrowLeft") ? 1.0f : 0.0f);
  in.up = (held("KeyE") || held("Space") ? 1.0f : 0.0f) - (held("KeyQ") || held("ShiftLeft") || held("ControlLeft") ? 1.0f : 0.0f);
  in.jump = held("Space");
  in.boost = held("ShiftLeft") || held("ShiftRight");
  in.look_x = look_dx_ * 0.2f * prefs_.mouse_sensitivity;
  in.look_y = look_dy_ * 0.2f * prefs_.mouse_sensitivity * (prefs_.invert_y ? -1.0f : 1.0f);
  in.zoom = play_zoom_;
  // Touch controls add to the keyboard.
  in.move_x = clamp(in.move_x + touch_.move_x, -1.0f, 1.0f);
  in.move_y = clamp(in.move_y + touch_.move_y, -1.0f, 1.0f);
  in.up = clamp(in.up + touch_.up, -1.0f, 1.0f);
  in.jump = in.jump || touch_.jump;
  in.boost = in.boost || touch_.boost;
  // Shooter keys and mouse buttons (while the mouse is captured).
  in.fire = (play_buttons_ & 1) || touch_.fire;
  in.aim = (play_buttons_ & 4) || touch_.aim;
  in.reload = held("KeyR") || touch_.reload;
  in.crouch = held("KeyC") || held("ControlLeft") || touch_.crouch;
  in.interact = held("KeyF") || touch_.interact;
  // Robot moves (walker): 1 sit / stand, 2 pick, 3 kick left, 4 kick right, 5 roll (hold), 6 beak.
  for (u32 k = 0; k < 6; ++k) {
    const char code[] = {'D', 'i', 'g', 'i', 't', static_cast<char>('1' + k), 0};
    if (held(code)) in.robot |= 1u << k;
  }
  if (held("KeyV")) in.robot |= 128;  // the wheee ride (Space: the chirp, via jump)
  // Engine touch controls (sticks, look drag, buttons; see game/touch.h).
  const game::PlayInput& t = touch_ctl_in_;
  in.move_x = clamp(in.move_x + t.move_x, -1.0f, 1.0f);
  in.move_y = clamp(in.move_y + t.move_y, -1.0f, 1.0f);
  in.up = clamp(in.up + t.up, -1.0f, 1.0f);
  in.jump = in.jump || t.jump;
  in.boost = in.boost || t.boost;
  in.fire = in.fire || t.fire;
  in.aim = in.aim || t.aim;
  in.reload = in.reload || t.reload;
  in.crouch = in.crouch || t.crouch;
  in.interact = in.interact || t.interact;
  in.robot |= t.robot;
  in.look_x += t.look_x * prefs_.mouse_sensitivity;
  in.look_y += t.look_y * prefs_.mouse_sensitivity * (prefs_.invert_y ? -1.0f : 1.0f);
  in.look_x += touch_look_x_ * prefs_.mouse_sensitivity;
  in.look_y += touch_look_y_ * prefs_.mouse_sensitivity * (prefs_.invert_y ? -1.0f : 1.0f);
  // Gamepad on top.
  if (pad_connected_ && pad_.gamepad) {
    // Move stick: radial deadzone .10 and a 1.28 power curve (SpawnCity).
    const f32 mx = pad_.move_x, my = pad_.move_y, m = std::sqrt(mx * mx + my * my);
    if (m > 0.10f) {
      const f32 k = std::pow(clamp((m - 0.10f) / 0.90f, 0.0f, 1.0f), 1.28f) / m;
      in.move_x = clamp(in.move_x + mx * k, -1.0f, 1.0f);
      in.move_y = clamp(in.move_y + my * k, -1.0f, 1.0f);
    }
    in.look_stick_x = pad_.look_stick_x;
    in.look_stick_y = pad_.look_stick_y * (prefs_.invert_y ? -1.0f : 1.0f);
    if (!soldier_pawn()) {
      // Other controllers: the right stick turns the camera at a fixed rate.
      in.look_x += pad_.look_stick_x * 3.2f;
      in.look_y += in.look_stick_y * 2.4f;
    }
    in.gamepad = true;
    in.fire = in.fire || pad_.fire;
    in.aim = in.aim || pad_.aim;
    in.jump = in.jump || pad_.jump;
    in.crouch = in.crouch || pad_.crouch;
    in.reload = in.reload || pad_.reload;
    in.interact = in.interact || pad_.interact;
    in.boost = in.boost || pad_.boost;
    // Robot on a pad, as the duck's own pad mapping: RT chirps, LT rides the wheee (the beak opens
    // with them); crouch (B) sits / stands, use (X) picks, reload (Y) rolls.
    if (pad_.fire) in.robot |= 64;
    if (pad_.aim) in.robot |= 128;
    if (pad_.crouch) in.robot |= 1;
    if (pad_.interact) in.robot |= 2;
    if (pad_.reload) in.robot |= 16;
    in.up = clamp(in.up + pad_.up, -1.0f, 1.0f);
  }
  return in;
}

void Studio::look(f32 dx, f32 dy) {
  look_dx_ += dx;
  look_dy_ += dy;
}

Vec3 Studio::orbit_pivot(Viewport& v) {
  if (prefs_.orbit_around_selection && !selection_.empty()) {
    Aabb box;
    for (ActorId id : selection_) {
      Aabb b;
      if (world_->actor_bounds(id, b)) box.extend(b);
    }
    if (box.valid()) return box.center();
  }
  Vec3 hit;
  if (pick(v.index, v.x, v.y, &hit)) return hit;
  // A point in front of the camera on the ground, or 15 m ahead.
  const Vec3 f = v.camera.forward();
  if (f.z < -0.05f) {
    const f32 t = -v.camera.position.z / f.z;
    if (t > 0 && t < 500) return v.camera.position + f * t;
  }
  return v.camera.position + f * 15;
}

void Studio::begin_drag(Viewport& v, Drag d) {
  v.drag = d;
  if (d == Drag::orbit || d == Drag::dolly) v.pivot = orbit_pivot(v);
}

void Studio::end_drag(Viewport& v) {
  if (v.drag == Drag::gizmo) end_gizmo(true);
  if (v.drag == Drag::grab) world_->end_grab();
  if (v.drag == Drag::fly) v.fov_boost = 0;
  v.drag = Drag::none;
}

void Studio::apply_drag(Viewport& v, f32 dx, f32 dy, u32 mods) {
  ViewCamera& c = v.camera;
  const f32 sens = 0.25f * prefs_.mouse_sensitivity;
  const f32 inv = prefs_.invert_y ? -1.0f : 1.0f;
  const f32 move = camera_speed() * 0.02f;  // metres per pixel for drags
  switch (v.drag) {
    case Drag::look_free:
    case Drag::fly:
      c.yaw -= dx * sens;
      c.pitch = clamp(c.pitch - dy * sens * inv, -89.9f, 89.9f);
      break;
    case Drag::pedestal:
      c.position.z -= dy * move;
      c.position += c.right() * (dx * move);
      break;
    case Drag::pan: {
      const Vec3 up = cross(c.right(), c.forward());
      // Pan speed follows the distance to what is under the cursor.
      c.position += c.right() * (-dx * move) + up * (dy * move);
      break;
    }
    case Drag::orbit: {
      const f32 dist = max_of(0.5f, distance(c.position, v.pivot));
      c.yaw -= dx * sens * 1.2f;
      c.pitch = clamp(c.pitch - dy * sens * 1.2f * inv, -89.5f, 89.5f);
      c.position = v.pivot - c.forward() * dist;
      break;
    }
    case Drag::dolly: {
      const f32 dist = max_of(0.3f, distance(c.position, v.pivot) * (1 + dy * 0.01f));
      c.position = v.pivot - c.forward() * dist;
      break;
    }
    case Drag::ortho_pan: {
      Vec3 dir, up;
      ortho_basis(v.type, dir, up);
      const Vec3 right = cross(dir, up);
      const f32 per_px = c.ortho_height / v.css_height;
      c.ortho_center += right * (-dx * per_px) + up * (dy * per_px);
      break;
    }
    case Drag::gizmo: update_gizmo(v); break;
    case Drag::grab: {
      Ray r;
      if (!ray(v, v.x, v.y, r)) break;
      const f32 d = distance(camera_of(v).eye, v.pivot);
      world_->move_grab(r.origin + r.direction * d);
      break;
    }
    case Drag::look:
      look(dx, dy);
      break;
    default: break;
  }
  (void)mods;
}

void Studio::click(Viewport& v, f32 x, f32 y, u32 mods) {
  const ActorId id = pick(v.index, x, y);
  if (!id) {
    if (!(mods & (shift | ctrl))) deselect();
    return;
  }
  // Clicking a child of a selected actor selects the child (Unreal selects the actor under the cursor).
  if (mods & ctrl) select({id}, false, true);
  else if (mods & shift) select({id}, true);
  else select({id});
}

void Studio::marquee_select(Viewport& v, u32 mods) {
  const f32 x0 = min_of(v.press_x, v.x), x1 = max_of(v.press_x, v.x), y0 = min_of(v.press_y, v.y), y1 = max_of(v.press_y, v.y);
  std::vector<ActorId> ids;
  for (const game::Actor& a : level_.actors) {
    if (a.locked || a.hidden || a.city) continue;
    Mat4 m;
    if (!world_->actor_matrix(a.id, m)) continue;
    f32 sx, sy;
    if (!to_screen(v, m.translation(), sx, sy)) continue;
    if ((sx >= x0 && sx <= x1 && sy >= y0 && sy <= y1) || a.id == v.press_actor) ids.push_back(a.id);
  }
  if (mods & ctrl) select(ids, false, true);
  else select(ids, (mods & shift) != 0);
  report("Selected " + std::to_string(selection_.size()) + " actors");
}

bool Studio::grab_at(Viewport& v, u32 index, f32 x, f32 y) {
  Vec3 hit;
  const ActorId id = pick(index, x, y, &hit);
  if (!id || !world_->begin_grab(id, hit)) return false;
  v.pivot = hit;
  v.press_actor = id;
  begin_drag(v, Drag::grab);
  return true;
}

void Studio::grab_depth(Viewport& v, f32 factor) {
  const Vec3 eye = camera_of(v).eye;
  const f32 d = clamp(distance(eye, v.pivot) * factor, 0.3f, 500.0f);
  Ray r;
  if (!ray(v, v.x, v.y, r)) return;
  v.pivot = eye + normalize(v.pivot - eye) * d;
  world_->move_grab(r.origin + r.direction * d);
}

// Fingers on a free camera (editing, Simulate, ejected, the observer camera), as in mobile 3D viewers:
// - one finger orbits about what is under it (or the selection); in a top / side view it pans;
//   while watching (observer) it looks around;
// - two fingers pinch to zoom toward the point between them and pan the view with them - the point
//   under the fingers stays under them, at any scale;
// - a tap selects, a second tap soon after frames the selection, holding still opens the context menu;
// - running (Simulate): one finger on a body takes it with the mouse joint and drags it, a second
//   finger pinches it nearer or farther;
// - a finger on a gizmo handle drags the gizmo (it goes the mouse's way: returns false).
bool Studio::free_touch_pointer(u32 type, i32 id, f32 x, f32 y, f64 time_ms) {
  Viewport& v = views_[active_view_];
  const bool persp = v.type == ViewType::perspective;
  FreeTouch& t = free_touch_;
  using Mode = FreeTouch::Mode;
  auto tapped = [&](f32 tx, f32 ty, f64 at) {  // true on the second of two quick taps
    if (at - t.last_tap < 400 && std::hypot(tx - t.tap_x, ty - t.tap_y) < 40) { t.last_tap = -1e9; return true; }
    t.last_tap = at;
    t.tap_x = tx;
    t.tap_y = ty;
    return false;
  };
  if (id == t.gizmo) {
    // A quick tap on the gizmo (it sits on the selection) still counts toward a double tap.
    if (type == 1 && std::hypot(x - t.f[0].x0, y - t.f[0].y0) > 8) t.moved = true;
    if (type >= 2) {
      t.gizmo = -1;
      if (type == 2 && !t.moved && time_ms - t.f[0].t0 < 350 && tapped(x, y, time_ms) && !selection_.empty()) focus_selection(static_cast<i32>(active_view_));
    }
    return false;
  }
  auto slot = [&](i32 pid) -> FreeTouch::Finger* {
    for (auto& f : t.f) if (f.id == pid) return &f;
    return nullptr;
  };
  auto spread = [&]() { return std::hypot(t.f[0].x - t.f[1].x, t.f[0].y - t.f[1].y); };
  const f32 sens = 0.3f * prefs_.mouse_sensitivity, inv = prefs_.invert_y ? -1.0f : 1.0f;
  if (type == 0) {  // down
    if (t.count >= 2 || t.gizmo >= 0) return true;
    if (t.count == 0 && gizmo_visible(v) && gizmo_hit(v, x, y)) {
      t.gizmo = id;
      t.moved = false;
      t.f[0].x0 = x;
      t.f[0].y0 = y;
      t.f[0].t0 = time_ms;
      return false;
    }
    FreeTouch::Finger& f = t.f[t.f[0].id < 0 ? 0 : 1];
    f = {id, x, y, x, y, time_ms};
    ++t.count;
    if (t.count == 1) {
      v.x = v.press_x = x;
      v.y = v.press_y = y;
      t.moved = false;
      t.actor = 0;
      if (play_ == PlayState::simulating && persp && grab_at(v, active_view_, x, y)) { t.mode = Mode::grab; t.actor = v.press_actor; }
      else if (!persp) t.mode = Mode::pan;
      else if (observing_ && ejected_) t.mode = Mode::look;
      else {
        t.mode = Mode::orbit;
        const ViewCamera& c = v.camera;
        const bool kept = t.kept && t.kept_view == active_view_ && t.kept_selection == selection_ &&
                          distance(c.position, t.kept_position) < 1e-4f && c.yaw == t.kept_yaw && c.pitch == t.kept_pitch;
        t.pivot = kept ? t.kept_pivot : orbit_pivot(v);
      }
    } else {
      t.moved = true;
      t.spread = max_of(1.0f, spread());
      t.mx = (t.f[0].x + t.f[1].x) * 0.5f;
      t.my = (t.f[0].y + t.f[1].y) * 0.5f;
      if (t.mode != Mode::grab) {
        t.mode = Mode::pinch;
        v.x = t.mx;
        v.y = t.my;
        Vec3 hit;
        t.pivot = persp && pick(active_view_, t.mx, t.my, &hit) ? hit : orbit_pivot(v);
      }
    }
    touch();
    return true;
  }
  FreeTouch::Finger* f = slot(id);
  if (!f) return true;
  if (type == 1) {  // move
    const f32 dx = x - f->x, dy = y - f->y;
    f->x = x;
    f->y = y;
    if (std::hypot(x - f->x0, y - f->y0) > 8) t.moved = true;
    ViewCamera& c = v.camera;
    if (t.mode == Mode::grab) {
      if (t.count == 2) {
        const f32 s = max_of(1.0f, spread());
        grab_depth(v, t.spread / s);  // fingers apart: nearer; together: farther
        t.spread = s;
      } else if (f == &t.f[0] || t.count == 1) {
        v.x = x;
        v.y = y;
        Ray r;
        if (ray(v, x, y, r)) world_->move_grab(r.origin + r.direction * distance(camera_of(v).eye, v.pivot));
      }
    } else if (!t.moved) {
      // Jitter of a tap does not move the camera.
    } else if (t.mode == Mode::look && t.count == 1) {
      c.yaw -= dx * sens;
      c.pitch = clamp(c.pitch - dy * sens * inv, -89.9f, 89.9f);
    } else if (t.mode == Mode::orbit && t.count == 1) {
      // Turn about the pivot where it is on screen: the camera keeps the pivot's place in its own frame,
      // so the point under the finger (or the pinch point) stays put instead of jumping to the centre.
      const Vec3 off = c.position - t.pivot;
      const Vec3 r0 = c.right(), f0 = c.forward(), u0 = cross(r0, f0);
      const f32 lr = dot(off, r0), lf = dot(off, f0), lu = dot(off, u0);
      c.yaw -= dx * sens;
      c.pitch = clamp(c.pitch - dy * sens * inv, -89.5f, 89.5f);
      const Vec3 r1 = c.right(), f1 = c.forward(), u1 = cross(r1, f1);
      c.position = t.pivot + r1 * lr + f1 * lf + u1 * lu;
    } else if (t.mode == Mode::pan && t.count == 1) {
      Vec3 dir, up;
      ortho_basis(v.type, dir, up);
      const f32 per_px = c.ortho_height / max_of(1.0f, v.css_height);
      c.ortho_center += cross(dir, up) * (-dx * per_px) + up * (dy * per_px);
      v.grid_key = 0;
    } else if (t.mode == Mode::pinch && t.count == 2) {
      const f32 s = max_of(1.0f, spread());
      const f32 mx = (t.f[0].x + t.f[1].x) * 0.5f, my = (t.f[0].y + t.f[1].y) * 0.5f;
      const f32 ratio = clamp(t.spread / s, 0.5f, 2.0f);  // fingers apart: nearer
      const f32 mdx = mx - t.mx, mdy = my - t.my;
      if (persp) {
        // Zoom toward the point under the fingers, then move the view with them: the scale is the
        // distance to that point, so it feels the same on a sock and on a city block.
        const Vec3 off = c.position - t.pivot;
        const f32 d = length(off), nd = clamp(d * ratio, 0.05f, 5000.0f);
        if (d > 1e-4f) c.position = t.pivot + off * (nd / d);
        const f32 per_px = 2 * nd * std::tan(c.fov * 0.5f * kDegToRad) / max_of(1.0f, v.css_height);
        const Vec3 up = cross(c.right(), c.forward());
        const Vec3 shift = c.right() * (-mdx * per_px) + up * (mdy * per_px);
        c.position += shift;
        t.pivot += shift;
      } else {
        Ray before;
        ray(v, mx, my, before);
        c.ortho_height = clamp(c.ortho_height * ratio, 0.5f, 20000.0f);
        Ray after;
        ray(v, mx, my, after);
        Vec3 dir, up;
        ortho_basis(v.type, dir, up);
        const Vec3 zoom = before.origin - after.origin;
        const f32 per_px = c.ortho_height / max_of(1.0f, v.css_height);
        c.ortho_center += zoom - dir * dot(zoom, dir) + cross(dir, up) * (-mdx * per_px) + up * (mdy * per_px);
        v.grid_key = 0;
      }
      t.spread = s;
      t.mx = mx;
      t.my = my;
    }
    touch();
    return true;
  }
  // up / cancel
  if (t.mode == Mode::pinch && persp) {
    // The pinch ends: what lies under the fingers now is where it zoomed to - the next orbit turns about it.
    Vec3 hit;
    if (pick(active_view_, t.mx, t.my, &hit)) t.pivot = hit;
  }
  const bool still = type == 2 && t.count == 1 && !t.moved;
  const f64 held = time_ms - f->t0;
  f->id = -1;
  t.count = max_of(0, t.count - 1);
  if (t.mode == Mode::grab && (t.count == 0 || f == &t.f[0])) {
    end_drag(v);
    t.mode = t.count ? Mode::orbit : Mode::none;
    if (t.count) t.pivot = orbit_pivot(v);
  }
  if (still && held < 350) {
    click(v, x, y, 0);
    if (tapped(x, y, time_ms) && !selection_.empty()) focus_selection(static_cast<i32>(active_view_));
  } else if (still && held >= 500 && play_ == PlayState::editing) {
    // Holding still: the context menu, as the right click.
    const ActorId hit = pick(active_view_, x, y);
    if (hit && !selected(hit)) select({hit});
    json::Value a = json::Value::object();
    a.set("action", "popup");
    a.set("menu", "context");
    a.set("entries", menu_json("context"));
    a.set("viewport", active_view_);
    a.set("x", x);
    a.set("y", y);
    push_ui(std::move(a));
  }
  if (t.count == 0) {
    // The point the camera last turned about or zoomed to stays its centre for the next orbit,
    // unless something else moves the camera or changes the selection first.
    if (persp && (t.mode == Mode::orbit || t.mode == Mode::pinch) && t.moved) {
      t.kept = true;
      t.kept_pivot = t.pivot;
      t.kept_view = active_view_;
      t.kept_position = v.camera.position;
      t.kept_yaw = v.camera.yaw;
      t.kept_pitch = v.camera.pitch;
      t.kept_selection = selection_;
    }
    t.mode = Mode::none;
  } else if (t.f[0].id < 0) { t.f[0] = t.f[1]; t.f[1] = {}; }  // the remaining finger leads
  if (t.mode == Mode::pinch && t.count == 1) {
    // One finger left: it carries on from here without a jump.
    v.x = t.f[0].x;
    v.y = t.f[0].y;
    if (!persp) t.mode = Mode::pan;
    else if (observing_ && ejected_) t.mode = Mode::look;
    else t.mode = Mode::orbit;
  }
  touch();
  return true;
}

bool Studio::touch_pointer(u32 type, i32 id, f32 x, f32 y, f64 time_ms) {
  if (type == 0) {
    const bool switched = !input_.touch();
    input_.pointer_down(true);
    if (switched) {
      // Touch from now on: give the mouse back and lay the sticks out before this finger lands.
      if (mouse_captured_) {
        mouse_captured_ = false;
        json::Value a = json::Value::object();
        a.set("action", "releasePointer");
        push_ui(std::move(a));
      }
      play_buttons_ = 0;
      update_touch_controls(0);
      touch();
    }
  }
  if (play_ != PlayState::playing || ejected_) return free_touch_pointer(type, id, x, y, time_ms);
  return touch_ctl_.pointer(type, id, x, y, time_ms);
}

json::Value Studio::pointer(u32 index, u32 type, f32 x, f32 y, i32 button, i32 buttons, u32 mods, f32 wheel_delta) {
  json::Value result = json::Value::object();
  if (index >= 4) return result;
  Viewport& v = views_[index];
  const f32 dx = x - v.x, dy = y - v.y;
  modifiers_ = mods;
  // Playing: the mouse looks around (captured) and clicks recapture it.
  if (play_ == PlayState::playing && !ejected_ && index == active_view_) {
    if (type == down && !input_.touch()) {
      if (!mouse_captured_) {
        mouse_captured_ = true;
        json::Value a = json::Value::object();
        a.set("action", "capturePointer");
        a.set("viewport", index);
        push_ui(std::move(a));
      }
      else if (button >= 0 && button < 8) play_buttons_ |= 1u << button;  // the capturing click does not fire
      v.drag = Drag::look;
    } else if (type == move && (buttons || mouse_captured_)) {
      if (v.drag == Drag::look || buttons) look(dx, dy);
    } else if (type == up || type == cancel) {
      if (type == cancel || button < 0 || button >= 8) play_buttons_ = 0;
      else play_buttons_ &= ~(1u << button);
      v.drag = Drag::none;
    } else if (type == wheel) {
      play_zoom_ += wheel_delta;  // zooms the play camera
    }
    v.x = x;
    v.y = y;
    return result;
  }
  switch (type) {
    case down: {
      if (active_view_ != index) { active_view_ = index; touch(); }
      v.x = v.press_x = x;
      v.y = v.press_y = y;
      v.moved = false;
      v.buttons = static_cast<u32>(buttons);
      const bool persp = v.type == ViewType::perspective;
      if (button == 0) {
        const u32 handle = gizmo_visible(v) ? gizmo_hit(v, x, y) : 0;
        if ((mods & alt) && !(mods & ctrl) && persp && !handle) begin_drag(v, Drag::orbit);
        else if (handle) begin_gizmo(v, handle, mods);
        else if (play_ == PlayState::simulating && persp && v.drag != Drag::fly && grab_at(v, index, x, y)) {
          // Running: a body under the cursor is taken by the mouse joint (a click still selects it).
        } else if (play_ == PlayState::simulating && observing_ && ejected_ && persp && v.drag != Drag::fly) {
          begin_drag(v, Drag::look_free);  // watching: dragging the empty view looks around
        } else if (v.drag == Drag::fly && persp) begin_drag(v, Drag::pedestal);
        else {
          // Left button (as in Unreal): a click selects, a drag draws a marquee. Actors move only
          // with the gizmo (W / E / R), never by dragging them directly.
          v.press_actor = pick(index, x, y);
          begin_drag(v, Drag::marquee);
        }
      } else if (button == 2) {
        if ((mods & alt) && persp) begin_drag(v, Drag::dolly);
        else if (!persp) begin_drag(v, Drag::ortho_pan);
        else if (v.drag == Drag::gizmo || v.drag == Drag::marquee) break;  // left drag in progress
        else begin_drag(v, Drag::fly);
      } else if (button == 1) {
        begin_drag(v, persp ? Drag::pan : Drag::ortho_pan);
      }
      result.set("capture", true);
      break;
    }
    case move: {
      v.x = x;
      v.y = y;
      if (std::hypot(x - v.press_x, y - v.press_y) > 3) v.moved = true;
      if (v.drag != Drag::none) {
        // Small jitter before a click does not move the camera.
        if (v.moved || v.drag == Drag::fly || v.drag == Drag::gizmo) apply_drag(v, dx, dy, mods);
      } else {
        const u32 h = gizmo_visible(v) ? gizmo_hit(v, x, y) : 0;
        if (h != gizmo_hover_) { gizmo_hover_ = h; ++scene_revision_; }
      }
      break;
    }
    case up:
    case cancel: {
      v.x = x;
      v.y = y;
      const Drag d = v.drag;
      const bool click_now = type == up && !v.moved;
      // Releasing one button of a two-button drag continues with the other.
      v.buttons = static_cast<u32>(buttons);
      if (d == Drag::pedestal && type == up) {
        v.drag = (buttons & 2) ? Drag::fly : Drag::none;
        break;
      }
      const bool surface = d == Drag::gizmo && gizmo_.surface;
      if (d == Drag::marquee && v.moved && type == up) marquee_select(v, mods);
      end_drag(v);
      if (click_now && button == 0 && (surface || d == Drag::marquee || d == Drag::grab || d == Drag::look_free)) click(v, x, y, mods);
      if (click_now && button == 2 && (d == Drag::fly || d == Drag::ortho_pan) && play_ != PlayState::playing) {
        // Right click: select what is under the cursor and open the context menu.
        const ActorId id = pick(index, x, y);
        if (id && !selected(id)) select({id});
        json::Value a = json::Value::object();
        a.set("action", "popup");
        a.set("menu", "context");
        a.set("entries", menu_json("context"));
        a.set("viewport", index);
        a.set("x", x);
        a.set("y", y);
        push_ui(std::move(a));
      }
      break;
    }
    case wheel: {
      ViewCamera& c = v.camera;
      if (v.type != ViewType::perspective) {
        // Zoom about the cursor.
        Ray before;
        ray(v, x, y, before);
        c.ortho_height = clamp(c.ortho_height * (wheel_delta > 0 ? 1.15f : 1 / 1.15f), 0.5f, 20000.0f);
        Ray after;
        ray(v, x, y, after);
        Vec3 dir, up;
        ortho_basis(v.type, dir, up);
        const Vec3 shift = before.origin - after.origin;
        c.ortho_center += shift - dir * dot(shift, dir);
        v.grid_key = 0;
      } else if (v.drag == Drag::grab) {
        grab_depth(v, std::exp(wheel_delta * 0.0015f));  // the wheel pushes the held body away (down) / brings it nearer (up)
      } else if (v.drag == Drag::fly) {
        prefs_.camera_speed = clamp(prefs_.camera_speed + (wheel_delta < 0 ? 1 : -1), 1, 8);
        report("Camera speed " + std::to_string(prefs_.camera_speed));
      } else {
        c.position += c.forward() * (-wheel_delta * 0.01f * camera_speed());
      }
      break;
    }
    case leave:
      if (gizmo_hover_) { gizmo_hover_ = 0; ++scene_revision_; }
      break;
    default: break;
  }
  return result;
}

void Studio::focus_selection(i32 index) {
  ensure_world_synced();
  Viewport& v = viewport(index < 0 ? active_view_ : static_cast<u32>(index));
  Aabb box;
  for (ActorId id : selection_) {
    Aabb b;
    if (world_->actor_bounds(id, b)) box.extend(b);
  }
  if (!box.valid()) {
    for (const game::Actor& a : level_.actors) {
      Aabb b;
      if (!a.hidden && world_->actor_bounds(a.id, b)) box.extend(b);
    }
  }
  if (!box.valid()) return;
  const f32 radius = max_of(0.5f, length(box.extent()) * 0.5f);
  if (v.type == ViewType::perspective) {
    const f32 dist = radius / std::tan(v.camera.fov * 0.5f * kDegToRad) * 1.15f;
    v.camera.position = box.center() - v.camera.forward() * dist;
  } else {
    v.camera.ortho_center = box.center();
    v.camera.ortho_height = radius * 2.4f;
    v.grid_key = 0;
  }
  touch();
}

// ---------------------------------------------------------------------------
// per frame

void Studio::update_cameras(f32 dt) {
  for (Viewport& v : views_) {
    // Flying: the right mouse held - or always in the active viewport while watching (observer camera).
    const bool watching = observing_ && ejected_ && v.index == active_view_ && v.drag != Drag::grab;
    if (v.type != ViewType::perspective || (v.drag != Drag::fly && !watching)) continue;
    auto held = [&](const char* code) { return keys_.count(code) > 0; };
    Vec3 move;
    const Vec3 f = v.camera.forward(), r = v.camera.right();
    if (held("KeyW") || held("ArrowUp")) move += f;
    if (held("KeyS") || held("ArrowDown")) move -= f;
    if (held("KeyD") || held("ArrowRight")) move += r;
    if (held("KeyA") || held("ArrowLeft")) move -= r;
    if (held("KeyE") || held("PageUp")) move += Vec3{0, 0, 1};
    if (held("KeyQ") || held("PageDown")) move -= Vec3{0, 0, 1};
    f32 speed = camera_speed();
    if (held("ShiftLeft") || held("ShiftRight")) speed *= 2.5f;
    if (length(move) > 0) v.camera.position += normalize(move) * (speed * dt);
    // Z / C zoom while flying.
    if (held("KeyZ")) v.fov_boost = max_of(-60.0f, v.fov_boost - 90 * dt);
    else if (held("KeyC")) v.fov_boost = min_of(60.0f, v.fov_boost + 90 * dt);
  }
}

void Studio::frame(f64 now_ms) {
  if (!ready_) return;
  now_ = now_ms;
  const f64 raw = last_frame_ < 0 ? 1000.0 / 60 : now_ms - last_frame_;
  const f32 dt = static_cast<f32>(clamp(raw / 1000.0, 0.0, 0.1));
  last_frame_ = now_ms;
  if (raw > 0) fps_ = fps_ <= 0 ? static_cast<f32>(1000.0 / raw) : fps_ * 0.9f + static_cast<f32>(100.0 / raw);
  update_cameras(dt);
  update_touch_controls(dt);
  if (play_ != PlayState::editing) {
    const game::PlayInput in = play_input();
    // In multi-player Play the active viewport's player gets the input; the others idle.
    const bool main_has_input = !net_ || clients_.empty() || active_view_ == 0;
    const game::PlayInput main_in = main_has_input ? in : game::PlayInput{};
    if (net_) update_net(dt, in);
    else world_->step(dt, in);
    check_logic_breakpoints();
    if (play_ == PlayState::playing && !ejected_) {
      Camera c = play_camera_valid_ ? play_camera_ : camera_of(views_[0]);
      play_camera_valid_ = world_->update_camera(c, dt, main_in);
      play_camera_ = c;
    }
    look_dx_ = look_dy_ = play_zoom_ = 0;
    touch_look_x_ = touch_look_y_ = 0;
    for (const std::string& e : world_->events()) report(e, e.rfind("Game lost", 0) == 0 ? "warn" : "info");
    world_->events().clear();
  } else {
    ensure_world_synced();
  }
  sync_overlays();
  // Lighting and the presentation of the moving world use the active viewport's eye.
  const Viewport& active = views_[active_view_];
  const bool client_view = active_view_ >= 1 && active_view_ - 1 < clients_.size() && clients_[active_view_ - 1].camera_valid;
  const Camera eye_cam = client_view ? clients_[active_view_ - 1].camera
                         : play_ == PlayState::playing && !ejected_ && play_camera_valid_ ? play_camera_ : camera_of(active);
  world_->present(now_ms, eye_cam.eye);
  update_audio(eye_cam, dt);
  robot_view_ = {};  // render_robot_camera sets it again
  // Clear the whole drawing buffer once (gaps between quad viewports).
  for (Viewport& v : views_) {
    if (!v.visible) continue;
    if (prefs_.layout == Layout::single && v.index != active_view_ && !player_only_) continue;
    render_viewport(v, now_ms);
  }
}

void Studio::render_viewport(Viewport& v, f64 now_ms) {
  (void)now_ms;
  // Multi-player Play: viewports 1..3 show the local clients' worlds through their cameras.
  LocalClient* client = v.index >= 1 && v.index - 1 < clients_.size() ? &clients_[v.index - 1] : nullptr;
  const bool playing_here = client ? client->camera_valid
                                   : play_ == PlayState::playing && !ejected_ && play_camera_valid_ && (v.index == active_view_ || (!clients_.empty() && v.index == 0));
  Camera cam = client && client->camera_valid ? client->camera : playing_here ? play_camera_ : camera_of(v);
  game::GameWorld& world = client ? *client->world : *world_;
  const f32 aspect = static_cast<f32>(v.rect.w) / static_cast<f32>(max_of(1, v.rect.h));
  (void)aspect;
  r_->camera = cam;
  world.apply_lighting(cam.eye);
  Lighting& l = r_->lighting;
  const bool helpers = !v.game_view && !playing_here && !player_only_;
  if (!v.flags.fog || cam.orthographic) l.fog_density = 0;
  RenderParams p;
  p.layers = layer::scene;
  p.sky = v.flags.sky && !cam.orthographic;
  p.clear_color = {0.09f, 0.1f, 0.12f, 1};
  switch (v.mode) {
    case ViewMode::lit: break;
    case ViewMode::unlit:
      // Base colour only: no sun, points or environment; the hemisphere term carries pi * base.
      l.sun_color = {0, 0, 0};
      l.env_intensity = 0;
      l.sky_color = l.ground_color = {kPi, kPi, kPi};
      l.exposure = 1;
      l.fog_density = 0;
      r_->set_point_lights({});
      break;
    case ViewMode::wireframe:
      p.layers = layer::wireframe;
      p.sky = false;
      p.clear_color = {0.05f, 0.055f, 0.065f, 1};
      break;
    case ViewMode::collision:
      p.layers = layer::scene | layer::colliders;
      l.exposure *= 0.55f;
      break;
  }
  if (helpers) {
    if (v.flags.grid) p.layers |= cam.orthographic ? (layer::ortho_grid0 << v.index) : layer::grid;
    if (v.flags.icons) p.layers |= layer::icons;
    if (v.flags.selection) p.layers |= layer::selection;
    if (v.flags.colliders) p.layers |= layer::colliders;
    if (v.flags.joints || v.flags.contacts) p.layers |= layer::joints;
    if (v.flags.bounds) p.layers |= layer::bounds;
    if (gizmo_visible(v)) p.layers |= layer::gizmo0 << v.index;
  } else if (v.mode == ViewMode::collision) {
    p.layers |= layer::colliders;
  }
  // A single full-size viewport renders without a scissor rectangle so bloom runs.
  const bool full = v.rect.x == 0 && v.rect.y == 0 && v.rect.w == r_->width() && v.rect.h == r_->height();
  if (!full) {
    p.has_viewport = true;
    p.viewport = {v.rect.x, v.rect.y, v.rect.w, v.rect.h};
    {
      kern::Viewport& vp = p.viewport;
      if (vp.x < 0) { vp.width += vp.x; vp.x = 0; }
      if (vp.y < 0) { vp.height += vp.y; vp.y = 0; }
      vp.width = min_of(vp.width, r_->width() - vp.x);
      vp.height = min_of(vp.height, r_->height() - vp.y);
      if (vp.width <= 0 || vp.height <= 0) return;
    }
  }
  if (client) {
    // The client's world only (its own meshes on its layer), no editor helpers.
    const u32 bit = layer::client0 << (v.index - 1);
    p.layers = (p.layers & ~layer::scene & ~(layer::grid | layer::icons | layer::selection | layer::colliders | layer::joints | layer::bounds |
                                               (layer::gizmo0 << v.index))) | bit;
    if (v.mode == ViewMode::wireframe) p.layers = bit, p.sky = true, p.clear_color = {0.09f, 0.1f, 0.12f, 1};
  }
  RenderStats stats;
  if (!r_->render(p, stats)) {
    const std::string err = last_error();
    if (status_text_ != "Render: " + err) report("Render: " + err, "error");
  }
  render_robot_camera(v);
}

void Studio::render_robot_camera(Viewport& v) {
  // What the robot sees: its head camera (the real duck's IMX219, 62 degrees across 16:9, mounted a
  // quarter turn off, so upright the picture is 9:16) in the corner of the viewport that plays, while
  // a walker runs.
  if (!v.flags.robot_camera || play_ == PlayState::editing || v.index != active_view_ || world_->walkers().empty()) return;
  ActorId who = 0;
  for (ActorId w : world_->walkers()) if (w == world_->player()) who = w;
  Camera cam;
  if (!world_->walker_camera(who, cam)) return;
  const i32 margin = static_cast<i32>(12 * v.dpr);
  const i32 h = clamp(v.rect.h * 2 / 5, static_cast<i32>(160 * v.dpr), static_cast<i32>(420 * v.dpr)), w = static_cast<i32>(static_cast<f32>(h) * 9 / 16);
  if (v.rect.w < w + 2 * margin || v.rect.h < h + 2 * margin) return;
  RenderParams p;
  p.layers = layer::scene;
  p.sky = true;
  p.clear_color = {0.09f, 0.1f, 0.12f, 1};
  p.has_viewport = true;
  // Top right of the viewport (drawing buffer pixels, y up from the bottom like the viewports).
  p.viewport = {v.rect.x + v.rect.w - w - margin, v.rect.y + v.rect.h - h - margin, w, h};
  robot_view_ = {v.index, w / v.dpr, h / v.dpr, who};  // the page frames it and shows the ToF beside it
  r_->camera = cam;
  world_->apply_lighting(cam.eye);
  RenderStats stats;
  r_->render(p, stats);
}

}  // namespace kern::studio
