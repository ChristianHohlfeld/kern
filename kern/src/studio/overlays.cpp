// Kern Studio viewport helpers: grids, selection outlines, actor icons,
// physics debug lines, bounds and the wireframe view mode.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <unordered_set>

#include "internal.h"
#include "kern/studio/studio.h"

namespace kern::studio {

namespace {

void line(std::vector<f32>& p, std::vector<f32>& c, Vec3 a, Vec3 b, Vec3 col) {
  p.insert(p.end(), {a.x, a.y, a.z, b.x, b.y, b.z});
  c.insert(c.end(), {col.x, col.y, col.z, col.x, col.y, col.z});
}

void box_lines(std::vector<f32>& p, std::vector<f32>& c, const Aabb& b, Vec3 col) {
  Vec3 k[8];
  for (int i = 0; i < 8; ++i) k[i] = {i & 1 ? b.max.x : b.min.x, i & 2 ? b.max.y : b.min.y, i & 4 ? b.max.z : b.min.z};
  const int e[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
  for (const auto& x : e) line(p, c, k[x[0]], k[x[1]], col);
}

void circle(std::vector<f32>& p, std::vector<f32>& c, Vec3 center, Vec3 u, Vec3 v, f32 r, Vec3 col, int n = 20) {
  for (int i = 0; i < n; ++i) {
    const f32 a0 = i * 2 * kPi / n, a1 = (i + 1) * 2 * kPi / n;
    line(p, c, center + (u * std::cos(a0) + v * std::sin(a0)) * r, center + (u * std::cos(a1) + v * std::sin(a1)) * r, col);
  }
}

/// Feature edges of a triangle mesh (edges between faces that are not coplanar, and borders).
void feature_edges(const Mesh& m, std::vector<Vec3>& out) {
  struct Key {
    std::array<i32, 3> a, b;
    bool operator<(const Key& o) const { return a != o.a ? a < o.a : b < o.b; }
  };
  auto q = [](Vec3 p) { return std::array<i32, 3>{static_cast<i32>(std::lround(p.x * 1e3f)), static_cast<i32>(std::lround(p.y * 1e3f)), static_cast<i32>(std::lround(p.z * 1e3f))}; };
  struct Edge {
    Vec3 a, b, normal;
    u32 faces = 0;
    bool crease = false;
  };
  std::map<Key, Edge> edges;
  const u32 n = m.indices.empty() ? m.vertex_count() : static_cast<u32>(m.indices.size());
  if (n > 600000) return;  // too dense to outline
  for (u32 t = 0; t + 2 < n; t += 3) {
    Vec3 v[3];
    for (int k = 0; k < 3; ++k) {
      const u32 idx = m.indices.empty() ? t + k : m.indices[t + k];
      v[k] = Vec3::load(&m.positions[idx * 3]);
    }
    const Vec3 fn = normalize(cross(v[1] - v[0], v[2] - v[0]));
    for (int k = 0; k < 3; ++k) {
      Vec3 a = v[k], b = v[(k + 1) % 3];
      Key key{q(a), q(b)};
      if (key.b < key.a) { std::swap(key.a, key.b); std::swap(a, b); }
      Edge& e = edges[key];
      if (e.faces == 0) { e.a = a; e.b = b; e.normal = fn; }
      else if (dot(e.normal, fn) < 0.996f) e.crease = true;  // not coplanar: a visible edge
      ++e.faces;
    }
  }
  for (const auto& [k, e] : edges)
    if (e.crease || e.faces == 1) { out.push_back(e.a); out.push_back(e.b); }
}

}  // namespace

u32 Studio::upload_lines(u32& id, const std::vector<f32>& positions, const std::vector<f32>& colors, u32 layers, bool on_top) {
  if (positions.empty()) {
    if (id) r_->set_visible(id, false);
    return id;
  }
  if (!id) id = r_->allocate_id();
  MeshInput in;
  in.positions = Span<const f32>(positions.data(), positions.size());
  in.colors = Span<const f32>(colors.data(), colors.size());
  in.material.lines = true;
  in.material.unlit = true;
  in.material.cast_shadow = false;
  in.material.frustum_culled = false;
  in.material.viewmodel = on_top;
  in.layers = layers;
  if (!r_->upsert(id, in)) {
    if (!r_->mesh(id)) { r_->release_id(id); id = 0; }
    return id;
  }
  r_->set_visible(id, true);
  return id;
}

void Studio::sync_perspective_grid(const Viewport& v) {
  // Spacing from the camera height; rebuilt when the camera leaves the centre cell.
  const f32 h = std::fabs(v.camera.position.z) + 1;
  const f32 spacing = h < 40 ? 1.0f : h < 400 ? 10.0f : 100.0f;
  const f32 major = spacing * 10;
  const i64 cx = static_cast<i64>(std::floor(v.camera.position.x / major)), cy = static_cast<i64>(std::floor(v.camera.position.y / major));
  if (cx == grid_cell_x_ && cy == grid_cell_y_ && spacing == grid_level_ && grid_mesh_) return;
  grid_cell_x_ = cx;
  grid_cell_y_ = cy;
  grid_level_ = spacing;
  std::vector<f32> p, c;
  const int n = 80;
  const f32 ox = static_cast<f32>(cx) * major, oy = static_cast<f32>(cy) * major;
  const f32 ext = n * spacing;
  const Vec3 minor_col{0.26f, 0.28f, 0.31f}, major_col{0.4f, 0.42f, 0.46f};
  for (int i = -n; i <= n; ++i) {
    const f32 x = ox + i * spacing, y = oy + i * spacing;
    const bool mx = std::fabs(std::fmod(x, major)) < 1e-3f, my = std::fabs(std::fmod(y, major)) < 1e-3f;
    if (std::fabs(x) > 1e-4f) line(p, c, {x, oy - ext, 0.005f}, {x, oy + ext, 0.005f}, mx ? major_col : minor_col);
    if (std::fabs(y) > 1e-4f) line(p, c, {ox - ext, y, 0.005f}, {ox + ext, y, 0.005f}, my ? major_col : minor_col);
  }
  // World axes through the origin.
  line(p, c, {ox - ext, 0, 0.006f}, {ox + ext, 0, 0.006f}, {0.75f, 0.25f, 0.25f});
  line(p, c, {0, oy - ext, 0.006f}, {0, oy + ext, 0.006f}, {0.3f, 0.7f, 0.25f});
  upload_lines(grid_mesh_, p, c, layer::grid);
}

void Studio::sync_grid(Viewport& v) {
  if (v.type == ViewType::perspective || !v.visible) return;
  const ViewCamera& cam = v.camera;
  // About 40 minor lines across the view.
  const f32 raw = cam.ortho_height / 40;
  f32 spacing = std::pow(10.0f, std::floor(std::log10(max_of(raw, 1e-3f))));
  if (raw / spacing > 5) spacing *= 5;
  else if (raw / spacing > 2) spacing *= 2;
  const u64 key = (static_cast<u64>(std::lround(cam.ortho_center.x / spacing)) * 73856093ull) ^ (static_cast<u64>(std::lround(cam.ortho_center.y / spacing)) * 19349663ull) ^
                  (static_cast<u64>(std::lround(cam.ortho_center.z / spacing)) * 83492791ull) ^ (static_cast<u64>(spacing * 1000) << 40) ^ (static_cast<u64>(v.type) << 60) ^
                  static_cast<u64>(cam.ortho_height * v.css_width / v.css_height);
  if (key == v.grid_key && !v.grid_meshes.empty()) return;
  v.grid_key = key;
  Vec3 u, w;
  switch (v.type) {
    case ViewType::top:
    case ViewType::bottom: u = {1, 0, 0}; w = {0, 1, 0}; break;
    case ViewType::front:
    case ViewType::back: u = {1, 0, 0}; w = {0, 0, 1}; break;
    default: u = {0, 1, 0}; w = {0, 0, 1}; break;
  }
  const f32 half_h = cam.ortho_height * 0.75f, half_w = half_h * v.css_width / v.css_height + spacing * 2;
  const f32 cu = std::round(dot(cam.ortho_center, u) / spacing) * spacing, cw = std::round(dot(cam.ortho_center, w) / spacing) * spacing;
  const int nu = static_cast<int>(half_w / spacing) + 2, nw = static_cast<int>(half_h / spacing) + 2;
  std::vector<f32> p, c;
  const Vec3 minor_col{0.2f, 0.215f, 0.24f}, major_col{0.32f, 0.34f, 0.38f};
  const f32 major = spacing * 10;
  for (int i = -nu; i <= nu; ++i) {
    const f32 x = cu + i * spacing;
    const bool m = std::fabs(std::remainder(x, major)) < spacing * 0.01f;
    const Vec3 col = std::fabs(x) < spacing * 0.01f ? (w.z > 0 ? Vec3{0.3f, 0.7f, 0.25f} : Vec3{0.3f, 0.7f, 0.25f}) : m ? major_col : minor_col;
    line(p, c, u * x + w * (cw - nw * spacing), u * x + w * (cw + nw * spacing), col);
  }
  for (int i = -nw; i <= nw; ++i) {
    const f32 y = cw + i * spacing;
    const bool m = std::fabs(std::remainder(y, major)) < spacing * 0.01f;
    const Vec3 col = std::fabs(y) < spacing * 0.01f ? Vec3{0.75f, 0.25f, 0.25f} : m ? major_col : minor_col;
    line(p, c, w * y + u * (cu - nu * spacing), w * y + u * (cu + nu * spacing), col);
  }
  if (v.grid_meshes.empty()) v.grid_meshes.push_back(0);
  upload_lines(v.grid_meshes[0], p, c, layer::ortho_grid0 << v.index);
}

void Studio::sync_selection_outline() {
  std::vector<f32> p, c;
  std::vector<Vec3> edges;
  const Vec3 kOutline{1.0f, 0.58f, 0.12f}, kPrimary{1.0f, 0.78f, 0.3f};
  for (ActorId id : selection_) {
    const game::Actor* a = level_.find(id);
    if (!a) continue;
    const Vec3 col = id == primary() ? kPrimary : kOutline;
    if (a->city) {
      Aabb b;
      if (world_->actor_bounds(id, b)) box_lines(p, c, b, col);
      continue;
    }
    std::vector<u32> meshes;
    world_->actor_meshes(id, meshes);
    for (u32 mid : meshes) {
      const Mesh* m = r_->mesh(mid);
      if (!m || m->instance_count == 0) continue;
      edges.clear();
      feature_edges(*m, edges);
      const Mat4 w = Mat4::load(m->transforms.data());
      for (std::size_t i = 0; i + 1 < edges.size(); i += 2) line(p, c, w.transform_point(edges[i]), w.transform_point(edges[i + 1]), col);
    }
  }
  upload_lines(selection_mesh_, p, c, layer::selection, false);
}

void Studio::sync_icons() {
  std::vector<f32> p, c;
  const Vec3 kSelected{1.0f, 0.62f, 0.15f};
  for (const game::Actor& a : level_.actors) {
    if (a.hidden || a.city) continue;
    Mat4 m;
    if (!world_->actor_matrix(a.id, m)) continue;
    const Vec3 o = m.translation();
    const bool sel = selected(a.id);
    if (a.light) {
      const Vec3 col = sel ? kSelected : a.light->color;
      circle(p, c, o, {1, 0, 0}, {0, 1, 0}, 0.3f, col, 16);
      circle(p, c, o, {1, 0, 0}, {0, 0, 1}, 0.3f, col, 16);
      circle(p, c, o, {0, 1, 0}, {0, 0, 1}, 0.3f, col, 16);
      for (Vec3 d : {Vec3{1, 0, 0}, Vec3{-1, 0, 0}, Vec3{0, 1, 0}, Vec3{0, -1, 0}, Vec3{0, 0, 1}, Vec3{0, 0, -1}}) line(p, c, o + d * 0.4f, o + d * 0.6f, col);
      if (sel) circle(p, c, o, {1, 0, 0}, {0, 1, 0}, a.light->range, {0.6f, 0.5f, 0.2f}, 48);
    }
    if (a.controller) {
      // Player start arrow along the actor's forward (+X).
      const Vec3 f = normalize(m.transform_vector({1, 0, 0}));
      const Vec3 col = sel ? kSelected : Vec3{0.3f, 0.85f, 1.0f};
      const Vec3 tip = o + f * 2.5f, side = normalize(cross(f, {0, 0, 1})) * 0.5f;
      line(p, c, o, tip, col);
      line(p, c, tip, tip - f * 0.7f + side, col);
      line(p, c, tip, tip - f * 0.7f - side, col);
    }
    if (!a.mesh && !a.light && !a.controller) {
      // Empty actor: axis tripod.
      const f32 s = 0.6f;
      line(p, c, o, o + normalize(m.transform_vector({1, 0, 0})) * s, sel ? kSelected : Vec3{0.9f, 0.3f, 0.3f});
      line(p, c, o, o + normalize(m.transform_vector({0, 1, 0})) * s, sel ? kSelected : Vec3{0.4f, 0.85f, 0.3f});
      line(p, c, o, o + normalize(m.transform_vector({0, 0, 1})) * s, sel ? kSelected : Vec3{0.35f, 0.55f, 1.0f});
      if (a.trigger || a.pickup) circle(p, c, o, {1, 0, 0}, {0, 1, 0}, 1.0f, a.trigger && a.trigger->action == game::TriggerAction::hazard ? Vec3{1, 0.3f, 0.3f} : Vec3{0.85f, 0.4f, 1.0f}, 24);
    }
  }
  upload_lines(icons_mesh_, p, c, layer::icons, false);
}

void Studio::sync_debug_lines() {
  bool colliders = false, joints = false, contacts = false;
  for (const Viewport& v : views_) {
    if (!v.visible) continue;
    colliders |= v.flags.colliders || v.mode == ViewMode::collision;
    joints |= v.flags.joints;
    contacts |= v.flags.contacts;
  }
  std::vector<f32> p, c;
  if (colliders) world_->debug_lines(true, false, false, p, c);
  upload_lines(debug_mesh_, p, c, layer::colliders, false);
  p.clear();
  c.clear();
  if (joints || contacts) world_->debug_lines(false, contacts, joints, p, c);
  upload_lines(joints_mesh_, p, c, layer::joints, false);
}

void Studio::sync_bounds() {
  bool want = false;
  for (const Viewport& v : views_) want |= v.visible && v.flags.bounds;
  std::vector<f32> p, c;
  if (want)
    for (const game::Actor& a : level_.actors) {
      if (a.hidden || a.city) continue;
      Aabb b;
      if (world_->actor_bounds(a.id, b)) box_lines(p, c, b, {0.85f, 0.8f, 0.3f});
    }
  upload_lines(bounds_mesh_, p, c, layer::bounds, false);
}

void Studio::sync_wireframe() {
  // One line mesh per scene mesh (triples: source id, line mesh id, source vertex
  // count); geometry once, instances follow the source every frame.
  std::unordered_set<u32> live;
  for (Mesh* m : r_->meshes()) {
    if (!(m->layers & layer::scene) || m->material.lines) continue;
    live.insert(m->id);
  }
  // Drop wires whose source disappeared.
  for (std::size_t i = 0; i < wireframe_meshes_.size();) {
    const u32 src = wireframe_meshes_[i], lines = wireframe_meshes_[i + 1];
    if (!live.count(src) || !r_->mesh(src)) {
      if (lines) r_->remove(lines);
      wireframe_meshes_.erase(wireframe_meshes_.begin() + static_cast<std::ptrdiff_t>(i), wireframe_meshes_.begin() + static_cast<std::ptrdiff_t>(i) + 3);
    } else i += 3;
  }
  for (u32 src : live) {
    const Mesh* m = r_->mesh(src);
    std::size_t slot = wireframe_meshes_.size();
    for (std::size_t i = 0; i < wireframe_meshes_.size(); i += 3)
      if (wireframe_meshes_[i] == src) { slot = i; break; }
    const bool fresh = slot == wireframe_meshes_.size() || wireframe_meshes_[slot + 2] != m->vertex_count();
    if (fresh) {
      if (slot == wireframe_meshes_.size()) wireframe_meshes_.insert(wireframe_meshes_.end(), {src, 0, 0});
      std::vector<Vec3> edges;
      feature_edges(*m, edges);
      std::vector<f32> pos, col;
      const Vec3 base = m->material.color.xyz();
      const Vec3 wc = lerp(base, {0.85f, 0.9f, 1.0f}, 0.45f) * 0.9f;
      for (const Vec3& e : edges) { pos.insert(pos.end(), {e.x, e.y, e.z}); col.insert(col.end(), {wc.x, wc.y, wc.z}); }
      u32& id = wireframe_meshes_[slot + 1];
      if (pos.empty()) continue;
      if (!id) id = r_->allocate_id();
      MeshInput in;
      in.positions = Span<const f32>(pos.data(), pos.size());
      in.colors = Span<const f32>(col.data(), col.size());
      in.material.lines = true;
      in.material.unlit = true;
      in.material.cast_shadow = false;
      in.layers = layer::wireframe;
      in.transforms = Span<const f32>(m->transforms.data(), m->transforms.size());
      if (!r_->upsert(id, in) && !r_->mesh(id)) { r_->release_id(id); id = 0; }
      wireframe_meshes_[slot + 2] = m->vertex_count();
    } else if (u32 id = wireframe_meshes_[slot + 1]) {
      const Mesh* w = r_->mesh(id);
      if (w && (w->transforms.size() != m->transforms.size() || std::memcmp(w->transforms.data(), m->transforms.data(), m->transforms.size() * 4) != 0))
        r_->update_instances(id, Span<const f32>(m->transforms.data(), m->transforms.size()), {});
      if (w && w->visible != m->visible) r_->set_visible(id, m->visible);
    }
  }
}

void Studio::sync_overlays() {
  if (player_only_) return;
  const bool scene_changed = synced_revision_ != scene_revision_;
  const bool moving = play_ != PlayState::editing;
  // Perspective grid follows the active perspective camera.
  const Viewport* persp = nullptr;
  if (views_[active_view_].type == ViewType::perspective) persp = &views_[active_view_];
  for (const Viewport& v : views_) if (!persp && v.type == ViewType::perspective && v.visible) persp = &v;
  if (persp) sync_perspective_grid(*persp);
  for (Viewport& v : views_) sync_grid(v);
  if (scene_changed || moving) {
    sync_selection_outline();
    sync_icons();
    sync_debug_lines();
    sync_bounds();
  }
  any_wireframe_ = false;
  for (const Viewport& v : views_) any_wireframe_ |= v.visible && v.mode == ViewMode::wireframe && (prefs_.layout != Layout::single || v.index == active_view_);
  if (any_wireframe_) sync_wireframe();
  for (Viewport& v : views_) build_gizmo_meshes(v);
  synced_revision_ = scene_revision_;
}

}  // namespace kern::studio
