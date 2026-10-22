// Kern Studio transform gizmos: translate (axes, planes, screen), rotate
// (rings) and scale (axes, uniform), in world or local space, with snapping
// and Alt-drag duplication. Drawn on top of the scene per viewport.
#include <algorithm>
#include <cmath>
#include <cstring>

#include "internal.h"
#include "kern/geo/primitives.h"
#include "kern/studio/studio.h"

namespace kern::studio {

namespace {

constexpr f32 kHandlePx = 110;  // gizmo length on screen
constexpr Vec3 kAxisColor[3] = {{0.96f, 0.26f, 0.24f}, {0.45f, 0.86f, 0.25f}, {0.28f, 0.52f, 1.0f}};
constexpr Vec3 kHover{1.0f, 0.86f, 0.2f};
constexpr Vec3 kCenter{0.92f, 0.92f, 0.92f};

Vec3 axis_of(const Quat& frame, int i) { return frame.rotate(i == 0 ? Vec3{1, 0, 0} : i == 1 ? Vec3{0, 1, 0} : Vec3{0, 0, 1}); }

void line(std::vector<f32>& p, std::vector<f32>& c, Vec3 a, Vec3 b, Vec3 col) {
  p.insert(p.end(), {a.x, a.y, a.z, b.x, b.y, b.z});
  c.insert(c.end(), {col.x, col.y, col.z, col.x, col.y, col.z});
}

/// Pivot-relative world matrix of a gizmo part.
void append_solid(MeshData& out, const MeshData& part, const Mat4& m, Vec3 color) {
  MeshData tinted = part;
  for (std::size_t i = 0; i + 2 < tinted.colors.size(); i += 3) { tinted.colors[i] = color.x; tinted.colors[i + 1] = color.y; tinted.colors[i + 2] = color.z; }
  primitives::append(out, tinted, m);
}

/// Rotation taking +Z to `dir`.
Quat z_to(Vec3 dir) {
  const Vec3 from{0, 0, 1};
  const f32 d = dot(from, dir);
  if (d > 0.9999f) return {};
  if (d < -0.9999f) return Quat::axis_angle({1, 0, 0}, kPi);
  const Vec3 c = cross(from, dir);
  const f32 s = std::sqrt((1 + d) * 2);
  return {c.x / s, c.y / s, c.z / s, s * 0.5f};
}

}  // namespace

bool Studio::gizmo_visible(const Viewport& v) const {
  if (selection_.empty() || tool_ == Tool::select || v.game_view || player_only_) return false;
  if (play_ == PlayState::playing && !ejected_) return false;
  return true;
}

bool Studio::gizmo_frame(Vec3& pivot, Quat& frame) const {
  const ActorId id = primary();
  if (!id) return false;
  Mat4 m;
  if (!world_->actor_matrix(id, m)) return false;
  pivot = m.translation();
  frame = Quat{};
  // Scale always works in the actor's own axes (like Unreal).
  if (space_ == Space::local || tool_ == Tool::scale) frame = game::euler_to_quat(game::matrix_transform(m).rotation);
  return true;
}

f32 Studio::gizmo_scale(const Viewport& v, Vec3 pivot) const {
  const Camera c = camera_of(v);
  if (c.orthographic) return c.ortho_height * kHandlePx / v.css_height;
  const f32 d = max_of(0.01f, dot(pivot - c.eye, normalize(c.target - c.eye)));
  return d * std::tan(c.fov * 0.5f) * 2 * kHandlePx / v.css_height;
}

void Studio::build_gizmo_meshes(Viewport& v) {
  Vec3 pivot;
  Quat frame;
  const bool show = gizmo_visible(v) && gizmo_frame(pivot, frame);
  if (!show) {
    if (v.gizmo_mesh) r_->set_visible(v.gizmo_mesh, false);
    if (v.gizmo_solid) r_->set_visible(v.gizmo_solid, false);
    v.gizmo_key = 0;
    return;
  }
  const f32 L = gizmo_scale(v, pivot);
  const u32 hot = gizmo_.active && gizmo_.viewport == v.index ? gizmo_.axes : gizmo_hover_;
  // Rebuild only when something visible changed.
  u64 key = 1469598103934665603ull;
  auto mix = [&](const void* data, std::size_t n) {
    const unsigned char* b = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < n; ++i) { key ^= b[i]; key *= 1099511628211ull; }
  };
  mix(&pivot, sizeof pivot);
  mix(&frame, sizeof frame);
  mix(&L, sizeof L);
  mix(&tool_, sizeof tool_);
  mix(&hot, sizeof hot);
  if (key == v.gizmo_key && v.gizmo_mesh) return;
  v.gizmo_key = key;
  std::vector<f32> lp, lc;
  MeshData solid;
  const MeshData cone = primitives::cone(0.065f, 0.22f, 12);
  const MeshData cube = primitives::box({0.11f, 0.11f, 0.11f});
  const MeshData center = primitives::box({0.1f, 0.1f, 0.1f});
  auto color = [&](u32 bits, Vec3 base) { return hot && hot == bits ? kHover : base; };
  if (tool_ == Tool::translate || tool_ == Tool::scale) {
    for (int i = 0; i < 3; ++i) {
      const Vec3 a = axis_of(frame, i);
      const Vec3 col = color(1u << i, kAxisColor[i]);
      line(lp, lc, pivot + a * (0.12f * L), pivot + a * L, col);
      const Mat4 at = Mat4::trs(pivot + a * L, z_to(a), {L, L, L});
      if (tool_ == Tool::translate) append_solid(solid, cone, at, col);
      else append_solid(solid, cube, Mat4::trs(pivot + a * L, frame, {L, L, L}), col);
    }
    if (tool_ == Tool::translate) {
      // Plane handles: squares between two axes.
      const int planes[3][2] = {{0, 1}, {0, 2}, {1, 2}};
      for (const auto& pl : planes) {
        const u32 bits = (1u << pl[0]) | (1u << pl[1]);
        const Vec3 u = axis_of(frame, pl[0]), w = axis_of(frame, pl[1]);
        const int other = 3 - pl[0] - pl[1];
        const Vec3 col = color(bits, kAxisColor[other]);
        const f32 a0 = 0.22f * L, a1 = 0.38f * L;
        line(lp, lc, pivot + u * a1 + w * a0, pivot + u * a1 + w * a1, col);
        line(lp, lc, pivot + u * a0 + w * a1, pivot + u * a1 + w * a1, col);
        line(lp, lc, pivot + u * a0 + w * a0, pivot + u * a1 + w * a0, col);
        line(lp, lc, pivot + u * a0 + w * a0, pivot + u * a0 + w * a1, col);
      }
    }
    append_solid(solid, center, Mat4::trs(pivot, frame, {L, L, L}), color(7, kCenter));
  } else if (tool_ == Tool::rotate) {
    for (int i = 0; i < 3; ++i) {
      const Vec3 n = axis_of(frame, i), u = axis_of(frame, (i + 1) % 3), w = axis_of(frame, (i + 2) % 3);
      const Vec3 col = color(1u << i, kAxisColor[i]);
      constexpr int kSeg = 64;
      for (int k = 0; k < kSeg; ++k) {
        const f32 a0 = k * 2 * kPi / kSeg, a1 = (k + 1) * 2 * kPi / kSeg;
        line(lp, lc, pivot + (u * std::cos(a0) + w * std::sin(a0)) * L, pivot + (u * std::cos(a1) + w * std::sin(a1)) * L, col);
      }
      (void)n;
    }
    // Live angle while dragging.
    if (gizmo_.active && gizmo_.tool == Tool::rotate && std::fabs(gizmo_.angle) > 0.01f) {
      int i = gizmo_.axes == 1 ? 0 : gizmo_.axes == 2 ? 1 : 2;
      const Vec3 u = axis_of(gizmo_.frame, (i + 1) % 3), w = axis_of(gizmo_.frame, (i + 2) % 3);
      const int steps = max_of(2, static_cast<int>(std::fabs(gizmo_.angle) / 5));
      for (int k = 0; k < steps; ++k) {
        const f32 a0 = gizmo_.angle * kDegToRad * k / steps, a1 = gizmo_.angle * kDegToRad * (k + 1) / steps;
        line(lp, lc, pivot, pivot + (u * std::cos(a1) + w * std::sin(a1)) * (L * 0.9f), kHover);
        (void)a0;
      }
    }
  }
  const u32 layers = layer::gizmo0 << v.index;
  Material m;
  m.unlit = true;
  m.cast_shadow = false;
  m.frustum_culled = false;
  m.viewmodel = true;
  m.lines = true;
  {
    if (!v.gizmo_mesh) v.gizmo_mesh = r_->allocate_id();
    MeshInput in;
    in.positions = Span<const f32>(lp.data(), lp.size());
    in.colors = Span<const f32>(lc.data(), lc.size());
    in.material = m;
    in.layers = layers;
    if (!r_->upsert(v.gizmo_mesh, in) && !r_->mesh(v.gizmo_mesh)) { r_->release_id(v.gizmo_mesh); v.gizmo_mesh = 0; }
    else r_->set_visible(v.gizmo_mesh, true);
  }
  if (solid.vertex_count()) {
    if (!v.gizmo_solid) v.gizmo_solid = r_->allocate_id();
    MeshInput in = solid.as_input(kMaxExtensionAttributes);
    m.lines = false;
    in.material = m;
    in.layers = layers;
    if (!r_->upsert(v.gizmo_solid, in) && !r_->mesh(v.gizmo_solid)) { r_->release_id(v.gizmo_solid); v.gizmo_solid = 0; }
    else r_->set_visible(v.gizmo_solid, true);
  } else if (v.gizmo_solid) {
    r_->set_visible(v.gizmo_solid, false);
  }
}

u32 Studio::gizmo_hit(const Viewport& v, f32 x, f32 y) const {
  Vec3 pivot;
  Quat frame;
  if (!gizmo_frame(pivot, frame)) return 0;
  const f32 L = gizmo_scale(v, pivot);
  f32 px, py;
  if (!to_screen(v, pivot, px, py)) return 0;
  constexpr f32 kTol = 8;
  if (tool_ == Tool::rotate) {
    u32 best = 0;
    f32 best_d = kTol;
    for (int i = 0; i < 3; ++i) {
      const Vec3 u = axis_of(frame, (i + 1) % 3), w = axis_of(frame, (i + 2) % 3);
      constexpr int kSeg = 48;
      f32 ax = 0, ay = 0;
      bool have = false;
      for (int k = 0; k <= kSeg; ++k) {
        const f32 a = k * 2 * kPi / kSeg;
        f32 sx, sy;
        if (!to_screen(v, pivot + (u * std::cos(a) + w * std::sin(a)) * L, sx, sy)) { have = false; continue; }
        if (have) {
          const f32 d = internal::segment_distance(x, y, ax, ay, sx, sy);
          if (d < best_d) { best_d = d; best = 1u << i; }
        }
        ax = sx;
        ay = sy;
        have = true;
      }
    }
    return best;
  }
  // Centre handle.
  if (std::hypot(x - px, y - py) < 9) return 7;
  // Axes.
  u32 best = 0;
  f32 best_d = kTol;
  for (int i = 0; i < 3; ++i) {
    const Vec3 a = axis_of(frame, i);
    f32 ax, ay, bx, by;
    if (!to_screen(v, pivot + a * (0.12f * L), ax, ay) || !to_screen(v, pivot + a * (1.12f * L), bx, by)) continue;
    const f32 d = internal::segment_distance(x, y, ax, ay, bx, by);
    if (d < best_d) { best_d = d; best = 1u << i; }
  }
  if (best) return best;
  // Planes (translate only).
  if (tool_ == Tool::translate) {
    const int planes[3][2] = {{0, 1}, {0, 2}, {1, 2}};
    for (const auto& pl : planes) {
      const Vec3 u = axis_of(frame, pl[0]), w = axis_of(frame, pl[1]);
      const f32 a0 = 0.22f * L, a1 = 0.38f * L;
      Vec3 q[4] = {pivot + u * a0 + w * a0, pivot + u * a1 + w * a0, pivot + u * a1 + w * a1, pivot + u * a0 + w * a1};
      f32 sx[4], sy[4];
      bool ok = true;
      for (int k = 0; k < 4 && ok; ++k) ok = to_screen(v, q[k], sx[k], sy[k]);
      if (!ok) continue;
      // Point in quad (convex) by edge signs.
      int pos = 0, neg = 0;
      for (int k = 0; k < 4; ++k) {
        const int n = (k + 1) % 4;
        const f32 c = (sx[n] - sx[k]) * (y - sy[k]) - (sy[n] - sy[k]) * (x - sx[k]);
        (c >= 0 ? pos : neg)++;
      }
      if (pos == 4 || neg == 4) return (1u << pl[0]) | (1u << pl[1]);
    }
  }
  return 0;
}

void Studio::begin_gizmo(Viewport& v, u32 axes, u32 modifiers) {
  if (play_ == PlayState::editing) {
    gizmo_.before = level_;
    gizmo_.selection_before = selection_;
  }
  gizmo_.duplicated = false;
  gizmo_.surface = false;
  if ((modifiers & 4) && play_ == PlayState::editing) {
    // Alt-drag moves a copy (Unreal). Copy and move form ONE undo step, committed when the drag ends.
    duplicate_in_place();
    gizmo_.duplicated = true;
  }
  if (play_ == PlayState::editing) gizmo_.base = level_;
  Vec3 pivot;
  Quat frame;
  if (!gizmo_frame(pivot, frame)) return;
  gizmo_.active = true;
  gizmo_.moved = false;
  gizmo_.viewport = v.index;
  gizmo_.tool = tool_;
  gizmo_.axes = axes;
  gizmo_.pivot = pivot;
  gizmo_.frame = frame;
  gizmo_.delta = {};
  gizmo_.angle = 0;
  gizmo_.factor = {1, 1, 1};
  gizmo_.start_x = v.x;
  gizmo_.start_y = v.y;
  gizmo_.start.clear();
  for (ActorId id : selection_roots()) {
    Mat4 m;
    if (world_->actor_matrix(id, m)) gizmo_.start.push_back({id, m});
  }
  Ray r;
  ray(v, v.x, v.y, r);
  const Camera cam = camera_of(v);
  const Vec3 view_dir = normalize(cam.target - cam.eye);
  if (tool_ == Tool::translate) {
    if (axes == 1 || axes == 2 || axes == 4) {
      const int i = axes == 1 ? 0 : axes == 2 ? 1 : 2;
      f32 t = 0;
      internal::ray_line_closest(r.origin, r.direction, pivot, axis_of(frame, i), t);
      gizmo_.start_t = t;
    } else {
      Vec3 n = view_dir;
      if (axes != 7) n = axis_of(frame, (axes & 1) == 0 ? 0 : (axes & 2) == 0 ? 1 : 2);
      Vec3 hit;
      if (!internal::ray_plane(r.origin, r.direction, pivot, n, hit)) hit = pivot;
      gizmo_.start_point = hit;
    }
  } else if (tool_ == Tool::rotate) {
    f32 px, py;
    to_screen(v, pivot, px, py);
    gizmo_.start_angle = std::atan2(v.y - py, v.x - px);
    gizmo_.start_t = 0;
  }
  v.drag = Drag::gizmo;
  touch();
}

bool Studio::surface_under(const Viewport& v, f32 x, f32 y, const std::vector<ActorId>& ignore, Vec3& point) {
  Ray r;
  if (!ray(v, x, y, r)) return false;
  auto moving = [&](ActorId id) {
    for (const game::Actor* a = level_.find(id); a; a = a->parent ? level_.find(a->parent) : nullptr)
      for (ActorId i : ignore)
        if (a->id == i) return true;
    return false;
  };
  PickQuery q;
  q.layers = layer::scene;
  PickHit hit;
  for (int attempt = 0; attempt < 16; ++attempt) {
    if (!r_->pick_ray(r, q, hit)) break;
    const ActorId id = world_->actor_for_mesh(hit.mesh);
    const game::Actor* a = id ? level_.find(id) : nullptr;
    if (!id || (a && !a->hidden && !moving(id))) {
      point = hit.point;
      return true;
    }
    q.ignore_id = hit.mesh;  // one of the dragged actors: look behind it
    r.origin = hit.point + r.direction * 0.001f;
  }
  return false;
}

void Studio::begin_surface_drag(Viewport& v, Vec3 grab_point) {
  gizmo_.before = level_;
  gizmo_.selection_before = selection_;
  gizmo_.base = level_;
  gizmo_.duplicated = false;
  gizmo_.surface = true;
  gizmo_.active = true;
  gizmo_.moved = false;
  gizmo_.viewport = v.index;
  gizmo_.tool = Tool::translate;
  gizmo_.axes = 7;
  gizmo_.delta = {};
  gizmo_.start_point = grab_point;
  gizmo_.start_x = v.x;
  gizmo_.start_y = v.y;
  gizmo_.start.clear();
  Aabb box;
  for (ActorId id : selection_roots()) {
    Mat4 m;
    if (world_->actor_matrix(id, m)) gizmo_.start.push_back({id, m});
    Aabb b;
    if (world_->actor_bounds(id, b)) box.extend(b);
  }
  gizmo_.pivot = gizmo_.start.empty() ? grab_point : gizmo_.start.front().second.translation();
  gizmo_.lift = box.valid() ? grab_point.z - box.min.z : 0;
  v.drag = Drag::gizmo;
  touch();
}

void Studio::update_surface_drag(Viewport& v) {
  if (!v.moved || gizmo_.start.empty()) return;
  std::vector<ActorId> ignore;
  for (auto& s : gizmo_.start) ignore.push_back(s.first);
  Vec3 surface, delta;
  if (v.type != ViewType::perspective) {
    // Orthographic views: slide in the view plane.
    Ray r;
    Vec3 hit;
    const Camera cam = camera_of(v);
    const Vec3 dir = normalize(cam.target - cam.eye);
    if (!ray(v, v.x, v.y, r) || !internal::ray_plane(r.origin, r.direction, gizmo_.start_point, dir, hit)) return;
    delta = hit - gizmo_.start_point;
    if (snap_.grid) {
      const Vec3 p = gizmo_.pivot + delta;
      for (int i = 0; i < 3; ++i)
        if (std::fabs(dir[i]) < 0.5f) delta[i] = internal::snap(p[i], snap_.grid_size) - gizmo_.pivot[i];
    }
  } else if (surface_under(v, v.x, v.y, ignore, surface)) {
    // Rest the selection's bottom on the surface, the grab point above the cursor.
    delta = {surface.x - gizmo_.start_point.x, surface.y - gizmo_.start_point.y, surface.z + gizmo_.lift - gizmo_.start_point.z};
  } else {
    // Nothing underneath (sky): slide on the horizontal plane through the grab point.
    Ray r;
    Vec3 hit;
    if (!ray(v, v.x, v.y, r) || !internal::ray_plane(r.origin, r.direction, gizmo_.start_point, {0, 0, 1}, hit)) return;
    if (distance(hit, camera_of(v).eye) > 2000) return;
    delta = {hit.x - gizmo_.start_point.x, hit.y - gizmo_.start_point.y, 0};
  }
  if (snap_.grid && v.type == ViewType::perspective) {
    const Vec3 p = gizmo_.pivot + delta;
    delta.x = internal::snap(p.x, snap_.grid_size) - gizmo_.pivot.x;
    delta.y = internal::snap(p.y, snap_.grid_size) - gizmo_.pivot.y;
  }
  gizmo_.delta = delta;
  gizmo_.moved = true;
  level_ = gizmo_.base;
  for (auto& [id, w0] : gizmo_.start) {
    game::Actor* a = level_.find(id);
    if (!a) continue;
    const Mat4 parent_inv = a->parent ? internal::inverse(level_.world_matrix(a->parent)) : Mat4{};
    game::Transform t = game::matrix_transform(parent_inv * (Mat4::translate(delta) * w0));
    t.rotation = a->transform.rotation;
    t.scale = a->transform.scale;
    a->transform = t;
  }
  ++scene_revision_;
  touch();
}

void Studio::update_gizmo(Viewport& v) {
  if (!gizmo_.active) return;
  if (gizmo_.surface) { update_surface_drag(v); return; }
  Ray r;
  if (!ray(v, v.x, v.y, r)) return;
  const Camera cam = camera_of(v);
  const Vec3 view_dir = normalize(cam.target - cam.eye);
  const u32 axes = gizmo_.axes;
  const Vec3 pivot = gizmo_.pivot;
  const Quat frame = gizmo_.frame;
  switch (gizmo_.tool) {
    case Tool::translate: {
      Vec3 delta;
      if (axes == 1 || axes == 2 || axes == 4) {
        const int i = axes == 1 ? 0 : axes == 2 ? 1 : 2;
        const Vec3 a = axis_of(frame, i);
        f32 t = 0;
        if (!internal::ray_line_closest(r.origin, r.direction, pivot, a, t)) return;
        f32 d = t - gizmo_.start_t;
        if (snap_.grid) {
          // Snap the pivot onto the grid along the axis (world space) or the travel (local).
          if (space_ == Space::world) d = internal::snap(dot(pivot, a) + d, snap_.grid_size) - dot(pivot, a);
          else d = internal::snap(d, snap_.grid_size);
        }
        delta = a * d;
      } else {
        Vec3 n = view_dir;
        if (axes != 7) n = axis_of(frame, (axes & 1) == 0 ? 0 : (axes & 2) == 0 ? 1 : 2);
        Vec3 hit;
        if (!internal::ray_plane(r.origin, r.direction, pivot, n, hit)) return;
        delta = hit - gizmo_.start_point;
        if (snap_.grid) {
          const Vec3 target = pivot + delta;
          Vec3 snapped = target;
          for (int i = 0; i < 3; ++i) {
            const Vec3 a = axis_of(frame, i);
            if (axes != 7 && !(axes & (1u << i))) continue;
            const f32 along = dot(target, a);
            snapped += a * (internal::snap(along, snap_.grid_size) - along);
          }
          delta = snapped - pivot;
        }
      }
      gizmo_.delta = delta;
      break;
    }
    case Tool::rotate: {
      f32 px, py;
      if (!to_screen(v, pivot, px, py)) return;
      // Accumulate the screen angle around the pivot continuously (several turns allowed).
      const f32 a = std::atan2(v.y - py, v.x - px);
      f32 step = a - gizmo_.start_angle;
      while (step > kPi) step -= 2 * kPi;
      while (step < -kPi) step += 2 * kPi;
      gizmo_.start_angle = a;
      gizmo_.start_t += step / kDegToRad;
      const int i = axes == 1 ? 0 : axes == 2 ? 1 : 2;
      const Vec3 axis = axis_of(frame, i);
      // Screen angles grow clockwise (y down); flip when the axis points away from the viewer.
      const f32 sign = dot(axis, view_dir) > 0 ? 1.0f : -1.0f;
      f32 angle = gizmo_.start_t * sign;
      gizmo_.angle = angle;
      if (snap_.rotation) angle = internal::snap(angle, snap_.rotation_step);
      gizmo_.delta = {angle, 0, 0};
      break;
    }
    case Tool::scale: {
      f32 px, py;
      to_screen(v, pivot, px, py);
      const f32 dx = v.x - gizmo_.start_x, dy = v.y - gizmo_.start_y;
      Vec3 f{1, 1, 1};
      if (axes == 7) {
        const f32 s = 1 + (dx - dy) / kHandlePx;
        f = {s, s, s};
      } else {
        for (int i = 0; i < 3; ++i) {
          if (!(axes & (1u << i))) continue;
          f32 ax, ay;
          to_screen(v, pivot + axis_of(frame, i) * gizmo_scale(v, pivot), ax, ay);
          const f32 sx = ax - px, sy = ay - py, len = max_of(1.0f, std::hypot(sx, sy));
          f[i] = 1 + (dx * sx + dy * sy) / (len * len);
        }
      }
      for (int i = 0; i < 3; ++i) {
        if (snap_.scale) f[i] = 1 + internal::snap(f[i] - 1, snap_.scale_step);
        if (std::fabs(f[i]) < 0.01f) f[i] = 0.01f;
      }
      gizmo_.factor = f;
      break;
    }
    case Tool::select: break;
  }
  gizmo_.moved = true;
  // Apply to the selection.
  auto world_after = [&](const Mat4& w0) -> Mat4 {
    switch (gizmo_.tool) {
      case Tool::translate: return Mat4::translate(gizmo_.delta) * w0;
      case Tool::rotate: {
        const int i = axes == 1 ? 0 : axes == 2 ? 1 : 2;
        const Quat q = Quat::axis_angle(axis_of(frame, i), gizmo_.delta.x * kDegToRad);
        Mat4 rot = Mat4::trs({}, q, {1, 1, 1});
        return Mat4::translate(pivot) * rot * Mat4::translate(pivot * -1.0f) * w0;
      }
      case Tool::scale: {
        Mat4 m = w0 * Mat4::scale(gizmo_.factor);
        // Multi-selection: positions spread from the pivot by the average factor.
        const f32 k = (gizmo_.factor.x + gizmo_.factor.y + gizmo_.factor.z) / 3;
        const Vec3 p = pivot + (w0.translation() - pivot) * k;
        m.m[12] = p.x;
        m.m[13] = p.y;
        m.m[14] = p.z;
        return m;
      }
      default: return w0;
    }
  };
  if (play_ == PlayState::simulating) {
    // Simulate: teleport the bodies.
    for (auto& [id, w0] : gizmo_.start) {
      const physics::Body b = world_->actor_body(id);
      if (!b.valid()) continue;
      const game::Transform t = game::matrix_transform(world_after(w0));
      world_->physics().set_pose(b, {t.position, game::euler_to_quat(t.rotation)});
      world_->physics().set_linear_velocity(b, {});
      world_->physics().set_angular_velocity(b, {});
      world_->physics().set_awake(b, true);
    }
  } else if (play_ == PlayState::editing) {
    level_ = gizmo_.base;
    for (auto& [id, w0] : gizmo_.start) {
      game::Actor* a = level_.find(id);
      if (!a) continue;
      const Mat4 parent_inv = a->parent ? internal::inverse(level_.world_matrix(a->parent)) : Mat4{};
      game::Transform t = game::matrix_transform(parent_inv * world_after(w0));
      if (gizmo_.tool == Tool::translate) { t.rotation = a->transform.rotation; t.scale = a->transform.scale; }
      if (gizmo_.tool == Tool::scale) t.rotation = a->transform.rotation;
      a->transform = t;
    }
    ++scene_revision_;
  }
  touch();
}

void Studio::end_gizmo(bool accept) {
  if (!gizmo_.active) return;
  gizmo_.active = false;
  if (play_ != PlayState::editing) return;
  if (!accept || (!gizmo_.moved && gizmo_.duplicated)) {
    // Cancelled (or an Alt-click without movement): back to the state before the drag, copies included.
    level_ = gizmo_.before;
    selection_ = gizmo_.selection_before;
    changed();
    return;
  }
  if (!gizmo_.moved) return;
  const char* label = gizmo_.tool == Tool::translate ? "Move" : gizmo_.tool == Tool::rotate ? "Rotate" : "Scale";
  if (gizmo_.duplicated) label = gizmo_.tool == Tool::translate ? "Duplicate and move" : gizmo_.tool == Tool::rotate ? "Duplicate and rotate" : "Duplicate and scale";
  commit(label, std::move(gizmo_.before), std::move(gizmo_.selection_before));
}

}  // namespace kern::studio
