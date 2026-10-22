// Views: orbit cameras, drawing the scene with the Kern renderer (one shared
// canvas, one viewport and layer bit per view), picking, pointer input, the
// Box3D player and mouse grabbing.
#include "kern/geo/primitives.h"
#include "kern/scene/picking.h"
#include "kern/world/scene_meshes.h"
#include "internal.h"

namespace kern::editor {

namespace {
constexpr u32 kSharedLayer = 31;  // grid + floor, drawn in every view
constexpr f32 kDamping = 0.05f;   // OrbitControls.dampingFactor
constexpr f32 kEps = 1e-6f;

Vec3 hex(std::string_view h) { return parse_hex_color(h); }
}  // namespace

// ---------------------------------------------------------------------------
// orbit camera

Vec3 OrbitCamera::eye() const {
  const f32 s = std::sin(phi);
  return target + Vec3{s * std::cos(theta), s * std::sin(theta), std::cos(phi)} * radius;
}

void OrbitCamera::set_eye(Vec3 e) {
  const Vec3 o = e - target;
  radius = max_of(1e-4f, length(o));
  theta = std::atan2(o.y, o.x);
  phi = std::acos(clamp(o.z / radius, -1.0f, 1.0f));
  phi = clamp(phi, kEps, kPi - kEps);
}

void OrbitCamera::rotate(f32 dx, f32 dy, f32 h) {
  h = max_of(1.0f, h);
  pending_theta -= 2 * kPi * dx / h;
  pending_phi -= 2 * kPi * dy / h;
  if (!damping) update();
}

void OrbitCamera::pan(f32 dx, f32 dy, f32 h) {
  h = max_of(1.0f, h);
  const Camera c = camera();
  const Vec3 forward = normalize(c.target - c.eye, {0, 1, 0});
  Vec3 right = cross(forward, Vec3{0, 0, 1});
  right = length(right) > 1e-6f ? normalize(right) : Vec3{1, 0, 0};
  const Vec3 up = cross(right, forward);
  const f32 distance = radius * std::tan(fov_degrees * kPi / 360);
  pending_pan = pending_pan - right * (2 * dx * distance / h) + up * (2 * dy * distance / h);
  if (!damping) update();
}

void OrbitCamera::zoom(f32 scale) {
  if (scale > 0 && std::isfinite(scale)) pending_scale *= scale;
  if (!damping) update();
}

bool OrbitCamera::update() {
  const f32 k = damping ? kDamping : 1.0f;
  theta += pending_theta * k;
  phi = clamp(phi + pending_phi * k, kEps, kPi - kEps);
  target = target + pending_pan * k;
  radius = max_of(1e-4f, radius * pending_scale);
  pending_scale = 1;
  if (damping) {
    pending_theta *= 1 - k;
    pending_phi *= 1 - k;
    pending_pan = pending_pan * (1 - k);
  } else {
    pending_theta = pending_phi = 0;
    pending_pan = {};
  }
  if (std::fabs(pending_theta) < 1e-5f) pending_theta = 0;
  if (std::fabs(pending_phi) < 1e-5f) pending_phi = 0;
  if (length(pending_pan) < 1e-6f * max_of(1.0f, radius)) pending_pan = {};
  return pending_theta != 0 || pending_phi != 0 || length(pending_pan) > 0;
}

Camera OrbitCamera::camera() const {
  Camera c;
  c.eye = eye();
  c.target = target;
  c.up = {0, 0, 1};
  c.fov = fov_degrees * kPi / 180;
  c.near = near;
  c.far = far;
  return c;
}

// ---------------------------------------------------------------------------
// view placement and the frame loop

void Editor::set_view_rect(u32 id, i32 x, i32 y, i32 w, i32 h, f32 css_w, f32 css_h, f32 dpr, bool visible) {
  View* v = view(id);
  if (!v) return;
  v->rect = {x, y, max_of(1, w), max_of(1, h)};
  v->css_width = max_of(1.0f, css_w);
  v->css_height = max_of(1.0f, css_h);
  v->dpr = dpr > 0 ? dpr : 1;
  v->visible = visible && w > 0 && h > 0;
}

DPose display_pose(const View& v, const Body& b) {
  DPose p = b.pose();
  if (v.type == ViewType::player && v.runtime) v.runtime->body_pose(b.id, p);
  return p;
}

void Editor::frame(f64 now_ms) {
  now_ = now_ms;
  if (!renderer_) return;
  sync_shared();
  for (auto& vp : views_) {
    View& v = *vp;
    Document* d = document(v.doc);
    if (!d) continue;
    if (v.type == ViewType::player && v.runtime && v.playing) {
      const f64 dt = v.last_time < 0 ? 0 : std::min((now_ms - v.last_time) / 1000.0, 0.1) * v.speed;
      if (v.runtime->game().status != world::GameStatus::inactive) {
        // Camera-relative game controls.
        const Camera c = v.camera.camera();
        v.game_input.camera_forward = c.target - c.eye;
        v.runtime->set_input(v.game_input);
      }
      if (v.runtime->advance(dt)) v.needs_sync = v.needs_sync || false;
    }
    v.last_time = now_ms;
    // Game camera follows its body.
    if (v.type == ViewType::player && v.runtime && v.follow_camera) {
      const std::string& follow = v.runtime->game().camera_body;
      DPose p;
      if (!follow.empty() && v.runtime->body_pose(follow, p)) {
        const Vec3 t = to_f(p.position);
        v.camera.target = v.camera.target + (t - v.camera.target) * 0.15f;
      }
    }
    if (v.needs_fit && v.rect.width > 1) { fit_view(&v); v.needs_fit = false; }
    v.camera.update();
    const bool rev_changed = v.synced_rev != d->rev || v.synced_mode != mode_;
    if (v.needs_sync || rev_changed) sync_view(v);
    else if (v.type == ViewType::player) update_player_meshes(v);
    if (v.visible && (tiled_ || &v == active_)) render_view(v);
  }
}

// ---------------------------------------------------------------------------
// drawing

void Editor::clear_view_meshes(View& v) {
  if (renderer_)
    for (const ViewMesh& m : v.meshes) renderer_->remove(m.mesh);
  v.meshes.clear();
  v.joint_lines = 0;
}

namespace {

struct MeshBuilder {
  Renderer& r;
  View& v;
  u32 add(const MeshData& geometry, const Material& material, const Mat4& transform, ViewMesh info, u32 texture = 0) {
    MeshInput in = geometry.as_input();
    in.material = material;
    in.transforms = Span<const f32>(transform.m, 16);
    in.layers = 1u << v.layer;
    in.texture = texture;
    const u32 id = r.allocate_id();
    if (!r.upsert(id, in)) { r.release_id(id); return 0; }
    info.mesh = id;
    v.meshes.push_back(std::move(info));
    return id;
  }
};

Material solid(Vec3 color, f32 alpha, bool selected, f32 roughness = 0.62f) {
  Material m;
  m.color = {color.x, color.y, color.z, alpha};
  m.alpha = alpha < 1 ? AlphaMode::blend : AlphaMode::opaque;
  m.roughness = roughness;
  m.metalness = 0;
  if (selected) m.emissive = {0.2f, 0.18f, 0.0f};
  return m;
}

Material basic(Vec3 color, f32 alpha = 1) {
  Material m;
  m.color = {color.x, color.y, color.z, alpha};
  m.alpha = alpha < 1 ? AlphaMode::blend : AlphaMode::opaque;
  m.unlit = true;
  m.cast_shadow = false;
  return m;
}

Material line_material() {
  Material m;
  m.lines = true;
  m.unlit = true;
  m.cast_shadow = false;
  m.frustum_culled = false;
  return m;
}

/// Edges of all triangles (wireframe display).
MeshData wireframe(const MeshData& m, Vec3 color) {
  MeshData out;
  auto vtx = [&](u32 i) { return Vec3::load(&m.positions[i * 3]); };
  auto edge = [&](u32 a, u32 b) {
    out.push(vtx(a), {0, 0, 1}, color, {});
    out.push(vtx(b), {0, 0, 1}, color, {});
  };
  if (!m.indices.empty()) {
    for (std::size_t t = 0; t + 2 < m.indices.size(); t += 3) {
      edge(m.indices[t], m.indices[t + 1]);
      edge(m.indices[t + 1], m.indices[t + 2]);
      edge(m.indices[t + 2], m.indices[t]);
    }
  } else {
    for (u32 t = 0; t + 2 < m.vertex_count(); t += 3) { edge(t, t + 1); edge(t + 1, t + 2); edge(t + 2, t); }
  }
  return out;
}

MeshData box_edges(const D3& size, Vec3 color) {
  MeshData out;
  const Vec3 h = to_f(mul(size, 0.5));
  auto corner = [&](int i) { return Vec3{i & 1 ? h.x : -h.x, i & 2 ? h.y : -h.y, i & 4 ? h.z : -h.z}; };
  const int edges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
  for (const auto& e : edges) {
    out.push(corner(e[0]), {0, 0, 1}, color, {});
    out.push(corner(e[1]), {0, 0, 1}, color, {});
  }
  return out;
}

}  // namespace

void Editor::sync_shared() {
  if (shared_synced_ || !renderer_) return;
  shared_synced_ = true;
  // Grid (80 x 80 m, 1 m cells) slightly above the floor, and the floor slab.
  MeshData grid;
  const Vec3 centre_line = hex("#68727c"), cell = hex("#424a52");
  for (i32 i = -40; i <= 40; ++i) {
    const Vec3 c = i == 0 ? centre_line : cell;
    const f32 k = static_cast<f32>(i);
    grid.push({k, -40, 0.006f}, {0, 0, 1}, c, {});
    grid.push({k, 40, 0.006f}, {0, 0, 1}, c, {});
    grid.push({-40, k, 0.006f}, {0, 0, 1}, c, {});
    grid.push({40, k, 0.006f}, {0, 0, 1}, c, {});
  }
  MeshInput gi = grid.as_input();
  gi.material = line_material();
  gi.layers = 1u << kSharedLayer;
  const u32 gid = renderer_->allocate_id();
  if (renderer_->upsert(gid, gi)) shared_meshes_.push_back(gid);
  MeshData floor = primitives::box({80, 80, 0.04f}, hex("#343941"));
  MeshInput fi = floor.as_input();
  const Mat4 ft = Mat4::translate({0, 0, -0.07f});
  fi.transforms = Span<const f32>(ft.m, 16);
  fi.material.roughness = 0.95f;
  fi.material.cast_shadow = false;
  fi.layers = 1u << kSharedLayer;
  const u32 fid = renderer_->allocate_id();
  if (renderer_->upsert(fid, fi)) shared_meshes_.push_back(fid);
}

void Editor::sync_view(View& v) {
  clear_view_meshes(v);
  Document* d = document(v.doc);
  if (!d) return;
  v.needs_sync = false;
  v.synced_rev = d->rev;
  v.synced_mode = mode_;
  const world::Scene& s = v.type == ViewType::player && v.runtime ? v.runtime->source() : d->scene;
  const Selection& sel = d->selection;
  auto selected = [&](Mode m, const std::string& id) { return contains(sel[m], id); };
  const f64 span = physical_span(s);
  const f32 helper = static_cast<f32>(std::max(0.003, std::min(0.07, span * 0.012)));
  const f32 selected_helper = max_of(helper, min_of(0.1f, helper * 1.5f));
  const f32 cursor_len = static_cast<f32>(std::max(0.04, std::min(0.35, span * 0.08)));
  MeshBuilder mb{*renderer_, v};
  const bool player = v.type == ViewType::player;
  const bool colors = player || options_.fixture_colors;

  for (const Body& b0 : s.bodies) {
    if (player && v.runtime && !v.runtime->body_alive(b0.id)) continue;
    const DPose root = display_pose(v, b0);
    for (const Fixture& f : b0.fixtures) {
      if (!options_.shapes) continue;
      const bool sel_f = selected(Mode::fixture, f.id) || selected(Mode::body, b0.id);
      const MeshData geometry = fixture_geometry(f);
      const Vec3 color = sel_f ? body_type_color(b0.type, true) : colors ? parse_hex_color(f.color) : body_type_color(b0.type, false);
      const Mat4 t = pose_matrix(compose(root, f.center, f.rotation));
      ViewMesh info;
      info.mode = Mode::fixture;
      info.id = f.id;
      info.body = b0.id;
      info.follows_body = true;
      info.center = f.center;
      info.rotation = f.rotation;
      if (options_.wireframe) mb.add(wireframe(geometry, color), line_material(), t, info);
      else mb.add(geometry, solid(color, f.sensor ? 0.45f : 1.0f, sel_f), t, info);
      if (mode_ == Mode::vertex && f.shape == FixtureShape::hull) {
        for (std::size_t i = 0; i < f.vertices.size(); ++i) {
          const std::string vid = f.id + ":" + std::to_string(i);
          const bool sv = selected(Mode::vertex, vid);
          const MeshData ball = primitives::sphere(sv ? selected_helper : helper, 10, 8);
          ViewMesh vi;
          vi.mode = Mode::vertex;
          vi.id = vid;
          vi.body = b0.id;
          vi.fixture = f.id;
          vi.follows_body = true;
          vi.center = local_to_world({f.center, f.rotation}, f.vertices[i]);
          mb.add(ball, basic(sv ? hex("#ffc4f2") : hex("#cc87bd")), pose_matrix(compose(root, vi.center, kIdentityQ)), vi);
        }
      }
    }
    if (!player && mode_ == Mode::body && options_.centers) {
      const bool sb = selected(Mode::body, b0.id);
      const MeshData nub = primitives::sphere(sb ? selected_helper : helper, 12, 8);
      ViewMesh ni;
      ni.mode = Mode::body;
      ni.id = b0.id;
      ni.body = b0.id;
      ni.follows_body = true;
      mb.add(nub, basic(sb ? hex("#ffff44") : hex("#d6d6d6")), pose_matrix(root), ni);
    }
    if (!player && mode_ == Mode::fixture && options_.centers) {
      for (const Fixture& f : b0.fixtures) {
        const bool sf = selected(Mode::fixture, f.id);
        const MeshData dot = primitives::sphere(helper * 0.8f, 8, 6);
        ViewMesh ci;
        ci.mode = Mode::fixture;
        ci.id = f.id;
        ci.body = b0.id;
        ci.follows_body = true;
        ci.center = f.center;
        mb.add(dot, basic(sf ? hex("#ffff44") : hex("#e5d3ef")), pose_matrix(compose(root, f.center, kIdentityQ)), ci);
      }
    }
  }

  if (options_.joints) {
    MeshData lines;
    for (const Joint& j : s.joints) {
      const Body* a0 = s.body(j.body_a);
      const Body* b0 = s.body(j.body_b);
      if (!a0 || !b0) continue;
      const bool sj = selected(Mode::joint, j.id);
      const D3 pa = local_to_world(display_pose(v, *a0), j.anchor_a), pb = local_to_world(display_pose(v, *b0), j.anchor_b);
      const Vec3 lc = sj ? hex("#ffff44") : hex("#82b5cc");
      lines.push(to_f(pa), {0, 0, 1}, lc, {});
      lines.push(to_f(pb), {0, 0, 1}, lc, {});
      for (int k = 0; k < 2; ++k) {
        ViewMesh ai;
        ai.mode = Mode::joint;
        ai.id = j.id;
        ai.body = k == 0 ? j.body_a : j.body_b;
        ai.follows_body = true;
        ai.center = k == 0 ? j.anchor_a : j.anchor_b;
        const MeshData ball = primitives::sphere(sj ? selected_helper : helper, 10, 8);
        mb.add(ball, basic(sj ? hex("#ffff44") : hex("#d0a96f")), pose_matrix({k == 0 ? pa : pb, kIdentityQ}), ai);
      }
    }
    if (lines.vertex_count()) {
      ViewMesh li;
      li.mode = Mode::world;  // not pickable
      v.joint_lines = mb.add(lines, line_material(), Mat4{}, li);
    }
  }

  if (options_.images)
    for (const ImagePlane& i : s.images) {
      const Body* b0 = i.body.empty() ? nullptr : s.body(i.body);
      const DPose base = b0 ? display_pose(v, *b0) : DPose{};
      const DPose t = compose(base, i.center, i.rotation);
      const MeshData plane = primitives::plane(static_cast<f32>(i.size[0]), static_cast<f32>(i.size[1]));
      Material m = basic(selected(Mode::image, i.id) ? hex("#ffdc76") : parse_hex_color(i.tint, {1, 1, 1}), static_cast<f32>(i.opacity.value_or(1)));
      m.alpha = AlphaMode::blend;
      m.double_sided = true;
      m.linear_colors = true;
      ViewMesh ii;
      ii.mode = Mode::image;
      ii.id = i.id;
      ii.body = i.body;
      ii.follows_body = b0 != nullptr;
      ii.center = i.center;
      ii.rotation = i.rotation;
      ii.scale = {i.flip ? -1.0 : 1.0, 1, 1};
      mb.add(plane, m, pose_matrix(t, ii.scale), ii, image_texture(i.src, i.filter == 1));
    }

  for (const Sampler& i : s.samplers) {
    const Body* b0 = i.body.empty() ? nullptr : s.body(i.body);
    const DPose base = b0 ? display_pose(v, *b0) : DPose{};
    const bool ss = selected(Mode::sampler, i.id);
    ViewMesh si;
    si.mode = Mode::sampler;
    si.id = i.id;
    si.body = i.body;
    si.follows_body = b0 != nullptr;
    si.center = i.center;
    si.rotation = i.rotation;
    mb.add(box_edges(i.size, ss ? hex("#ffff44") : hex("#61c6b7")), line_material(), pose_matrix(compose(base, i.center, i.rotation)), si);
  }

  if (!player) {
    MeshData axes;
    axes.push({0, 0, 0}, {0, 0, 1}, hex("#ff6666"), {});
    axes.push({cursor_len, 0, 0}, {0, 0, 1}, hex("#ff6666"), {});
    axes.push({0, 0, 0}, {0, 0, 1}, hex("#66ff66"), {});
    axes.push({0, cursor_len, 0}, {0, 0, 1}, hex("#66ff66"), {});
    axes.push({0, 0, 0}, {0, 0, 1}, hex("#6688ff"), {});
    axes.push({0, 0, cursor_len}, {0, 0, 1}, hex("#6688ff"), {});
    const Mat4 ct = Mat4::translate(to_f(s.cursor));
    ViewMesh cx;
    cx.mode = Mode::world;
    mb.add(axes, line_material(), ct, cx);
    const MeshData ring = primitives::torus(cursor_len * 0.38f, max_of(0.001f, cursor_len * 0.035f), 24, 6, {1, 1, 1});
    mb.add(ring, basic({1, 1, 1}), ct, cx);
  }
  // Shadow extent follows the scale of the scene.
  const f32 extent = static_cast<f32>(std::clamp(span * 1.6, 2.0, 120.0));
  if (&v == active_ && std::fabs(extent - shadow_extent_) > shadow_extent_ * 0.25f) {
    ShadowOptions so;
    so.extent = extent;
    so.resolution = 2048;
    renderer_->set_shadows(&so);
    shadow_extent_ = extent;
  }
}

void Editor::update_player_meshes(View& v) {
  if (!v.runtime) return;
  const world::Scene& s = v.runtime->source();
  DPose pose;
  for (ViewMesh& m : v.meshes) {
    if (!m.follows_body || m.body.empty()) continue;
    if (!v.runtime->body_pose(m.body, pose)) {
      if (m.visible) { renderer_->set_visible(m.mesh, false); m.visible = false; }
      continue;
    }
    if (!m.visible) { renderer_->set_visible(m.mesh, true); m.visible = true; }
    const Mat4 t = pose_matrix(compose(pose, m.center, m.rotation), m.scale);
    renderer_->update_instances(m.mesh, Span<const f32>(t.m, 16), {});
  }
  // Joint lines move with both bodies.
  if (v.joint_lines) {
    Document* d = document(v.doc);
    MeshData lines;
    for (const Joint& j : s.joints) {
      DPose pa, pb;
      if (!v.runtime->body_pose(j.body_a, pa) || !v.runtime->body_pose(j.body_b, pb)) continue;
      const bool sj = d && contains(d->selection[Mode::joint], j.id);
      const Vec3 lc = sj ? hex("#ffff44") : hex("#82b5cc");
      lines.push(to_f(local_to_world(pa, j.anchor_a)), {0, 0, 1}, lc, {});
      lines.push(to_f(local_to_world(pb, j.anchor_b)), {0, 0, 1}, lc, {});
    }
    if (lines.vertex_count()) {
      MeshInput in = lines.as_input();
      in.material = line_material();
      in.layers = 1u << v.layer;
      renderer_->upsert(v.joint_lines, in);
    } else {
      renderer_->set_visible(v.joint_lines, false);
    }
  }
}

void Editor::render_view(View& v) {
  Lighting& l = renderer_->lighting;
  l.sun_direction = normalize(Vec3{8, -6, 14});
  l.sun_color = {2.2f, 2.2f, 2.2f};
  l.sky_color = {1.0f, 1.0f, 1.0f};
  l.ground_color = {0.19f, 0.2f, 0.23f};
  l.fog_density = 0;
  l.exposure = 1;
  l.env_intensity = 0.55f;
  renderer_->camera = v.camera.camera();
  RenderParams p;
  p.has_viewport = true;
  p.viewport = v.rect;
  // The page may report a rect from before a canvas resize: clip to the buffer.
  {
    Viewport& r = p.viewport;
    const i32 bw = renderer_->width(), bh = renderer_->height();
    if (r.x < 0) { r.width += r.x; r.x = 0; }
    if (r.y < 0) { r.height += r.y; r.y = 0; }
    r.width = min_of(r.width, bw - r.x);
    r.height = min_of(r.height, bh - r.y);
    if (r.width <= 0 || r.height <= 0) return;
  }
  const Vec3 bg = v.type == ViewType::player ? hex("#11131b") : hex("#252a31");
  // Clear colours are in display space; the renderer writes sRGB.
  p.clear_color = {bg.x, bg.y, bg.z, 1};
  p.layers = (1u << v.layer) | (options_.grid ? 1u << kSharedLayer : 0u);
  p.sky = false;
  RenderStats stats;
  if (!renderer_->render(p, stats)) {
    const std::string err = last_error();
    if (err != last_engine_error_) { last_engine_error_ = err; report("Render: " + err, "warn"); }
  }
}

// ---------------------------------------------------------------------------
// rays, projection, picking

bool Editor::ray_from(const View& v, f32 x, f32 y, Vec3& origin, Vec3& direction) const {
  Ray r;
  if (!kern::screen_ray(v.camera.camera(), x, y, v.css_width, v.css_height, r)) return false;
  origin = r.origin;
  direction = normalize(r.direction);
  return true;
}

D3 Editor::project_on_plane(const View& v, f32 x, f32 y, const D3& pivot) const {
  Vec3 o, dir;
  if (!ray_from(v, x, y, o, dir)) return pivot;
  const Camera c = v.camera.camera();
  const D3 n = normalized(to_d(c.target - c.eye));
  const D3 od = to_d(o), dd = to_d(dir);
  const f64 denom = dot(n, dd);
  if (std::fabs(denom) < 1e-12) return pivot;
  const f64 t = dot(sub(pivot, od), n) / denom;
  if (t < 0) return pivot;
  return add(od, mul(dd, t));
}

bool Editor::screen_point(const View& v, const D3& p, f32& x, f32& y) const {
  const Camera c = v.camera.camera();
  bool ok = false;
  const Mat4 view = Mat4::look_at(c.eye, c.target, c.up, &ok);
  if (!ok) return false;
  const Mat4 vp = Mat4::perspective(c.fov, v.css_width / v.css_height, c.near, c.far) * view;
  const Vec3 q = to_f(p);
  const f32 cx = vp.m[0] * q.x + vp.m[4] * q.y + vp.m[8] * q.z + vp.m[12];
  const f32 cy = vp.m[1] * q.x + vp.m[5] * q.y + vp.m[9] * q.z + vp.m[13];
  const f32 cw = vp.m[3] * q.x + vp.m[7] * q.y + vp.m[11] * q.z + vp.m[15];
  if (!(std::fabs(cw) > 1e-9f)) return false;
  x = (cx / cw + 1) * 0.5f * v.css_width;
  y = (1 - cy / cw) * 0.5f * v.css_height;
  return cw > 0;
}

Editor::Hit Editor::hit_test(View& v, f32 x, f32 y, Mode m) {
  Hit result;
  if (v.needs_sync || v.synced_rev != (document(v.doc) ? document(v.doc)->rev : 0)) sync_view(v);
  Ray ray;
  if (!kern::screen_ray(v.camera.camera(), x, y, v.css_width, v.css_height, ray)) return result;
  std::vector<Mesh*> candidates;
  std::vector<const ViewMesh*> infos;
  for (const ViewMesh& vm : v.meshes) {
    const bool match = m == Mode::body ? !vm.body.empty() && vm.mode != Mode::joint && vm.mode != Mode::image && vm.mode != Mode::sampler
                                       : vm.mode == m;
    if (!match || !vm.visible) continue;
    if (Mesh* mesh = renderer_->mesh(vm.mesh); mesh && !mesh->material.lines) {
      candidates.push_back(mesh);
      infos.push_back(&vm);
    }
  }
  PickHit hit;
  PickQuery q;
  q.max_distance = 1e6f;
  f32 best = INFINITY;
  if (!candidates.empty() && pick_meshes(Span<Mesh* const>(candidates.data(), candidates.size()), ray, q, hit)) {
    for (std::size_t k = 0; k < candidates.size(); ++k)
      if (candidates[k]->id == hit.mesh) {
        result.ok = true;
        result.id = m == Mode::body ? infos[k]->body : infos[k]->id;
        result.point = to_d(hit.point);
        best = hit.distance;
      }
  }
  const world::Scene& s = v.type == ViewType::player && v.runtime ? v.runtime->source() : document(v.doc)->scene;
  if (m == Mode::joint) {
    // Joint lines: within 6 CSS pixels of the drawn segment.
    f32 best_px = 6;
    for (const Joint& j : s.joints) {
      const Body* a = s.body(j.body_a);
      const Body* b = s.body(j.body_b);
      if (!a || !b) continue;
      const D3 pa = local_to_world(display_pose(v, *a), j.anchor_a), pb = local_to_world(display_pose(v, *b), j.anchor_b);
      f32 ax, ay, bx, by;
      if (!screen_point(v, pa, ax, ay) || !screen_point(v, pb, bx, by)) continue;
      const f32 dx = bx - ax, dy = by - ay, len2 = dx * dx + dy * dy;
      const f32 t = len2 > 0 ? clamp(((x - ax) * dx + (y - ay) * dy) / len2, 0.0f, 1.0f) : 0;
      const f32 d = std::hypot(x - (ax + t * dx), y - (ay + t * dy));
      if (d < best_px && !result.ok) {
        best_px = d;
        result.ok = true;
        result.id = j.id;
        result.point = add(pa, mul(sub(pb, pa), t));
      }
    }
  }
  if (m == Mode::sampler) {
    // Sampler volumes are drawn as edges: test the ray against the box itself.
    const D3 o = to_d(ray.origin), dir = normalized(to_d(ray.direction));
    for (const Sampler& smp : s.samplers) {
      const DPose pose = item_world_pose(s, smp.body, smp.center, smp.rotation);
      const D3 lo = world_to_local(pose, o), ld = qrotate(qinv(pose.rotation), dir);
      f64 tmin = 0, tmax = 1e9;
      bool hit_box = true;
      for (int k = 0; k < 3 && hit_box; ++k) {
        const f64 h = smp.size[k] * 0.5;
        if (std::fabs(ld[k]) < 1e-12) { if (lo[k] < -h || lo[k] > h) hit_box = false; continue; }
        f64 t1 = (-h - lo[k]) / ld[k], t2 = (h - lo[k]) / ld[k];
        if (t1 > t2) std::swap(t1, t2);
        tmin = std::max(tmin, t1);
        tmax = std::min(tmax, t2);
        if (tmin > tmax) hit_box = false;
      }
      if (hit_box && tmin < best) {
        best = static_cast<f32>(tmin);
        result.ok = true;
        result.id = smp.id;
        result.point = add(o, mul(dir, tmin));
      }
    }
  }
  return result;
}

void Editor::select_in_box(View& v, const SelectBox& b) {
  const f32 lx = min_of(b.x0, b.x1), hx = max_of(b.x0, b.x1), ly = min_of(b.y0, b.y1), hy = max_of(b.y0, b.y1);
  std::vector<std::string> ids;
  const world::Scene& s = document(v.doc)->scene;
  for (const ItemRef& i : all_items(mode_)) {
    f32 x, y;
    if (!screen_point(v, item_point(i, mode_, s), x, y)) continue;
    if (x >= lx && x <= hx && y >= ly && y <= hy) ids.push_back(i.id);
  }
  set_selection(ids, b.shift, b.toggle);
}

// ---------------------------------------------------------------------------
// fitting

void Editor::fit_view(View* v, bool selection_only) {
  if (!v) v = active_;
  if (!v) return;
  Document* d = document(v->doc);
  if (!d) return;
  const world::Scene& s = v->type == ViewType::player && v->runtime ? v->runtime->source() : d->scene;
  Bounds box;
  if (selection_only) {
    for (const ItemRef& i : chosen(mode_)) box.add(item_point(i, mode_, s));
  } else {
    for (const Body& b : s.bodies)
      for (const Fixture& f : b.fixtures) add_fixture_bounds(box, display_pose(*v, b), f);
    for (const ImagePlane& i : s.images) {
      const DPose p = item_world_pose(s, i.body, i.center, i.rotation);
      for (int k = 0; k < 4; ++k) box.add(local_to_world(p, {(k & 1 ? 0.5 : -0.5) * i.size[0], (k & 2 ? 0.5 : -0.5) * i.size[1], 0}));
    }
    for (const Sampler& i : s.samplers) {
      const DPose p = item_world_pose(s, i.body, i.center, i.rotation);
      for (int k = 0; k < 8; ++k)
        box.add(local_to_world(p, {(k & 1 ? 0.5 : -0.5) * i.size[0], (k & 2 ? 0.5 : -0.5) * i.size[1], (k & 4 ? 0.5 : -0.5) * i.size[2]}));
    }
    if (box.empty())
      for (const Body& b : s.bodies) box.add(display_pose(*v, b).position);
  }
  if (box.empty()) { box.add({-2, -2, 0}); box.add({2, 2, 4}); }
  const D3 c = mul(add(box.lo, box.hi), 0.5), dims = sub(box.hi, box.lo);
  const f64 span = std::max({dims[0], dims[1], dims[2], 0.05});
  OrbitCamera& cam = v->camera;
  Vec3 dir = cam.eye() - cam.target;
  if (length(dir) < 0.1f) dir = {1, -1, 0.8f};
  dir = normalize(dir);
  const f64 vfov = cam.fov_degrees * kPiD / 180;
  const f64 aspect = v->css_width / v->css_height;
  const f64 hfov = 2 * std::atan(std::tan(vfov / 2) * std::max(0.2, aspect));
  const f64 fit_fov = std::max(0.08, std::min(vfov, hfov));
  const f64 distance = std::max({span / (2 * std::tan(fit_fov / 2)) * 1.35, span * 1.15, 0.08});
  cam.pending_theta = cam.pending_phi = 0;
  cam.pending_pan = {};
  cam.target = to_f(c);
  cam.set_eye(to_f(add(c, mul(to_d(dir), distance))));
  cam.near = static_cast<f32>(std::max(0.0005, distance / 2000));
  cam.far = static_cast<f32>(std::max(20.0, distance + span * 100));
}

// ---------------------------------------------------------------------------
// the player

void Editor::destroy_runtime(View& v) {
  if (v.runtime) v.runtime->destroy();
  v.runtime.reset();
  v.grab_pointer = -1;
}

bool Editor::open_player() {
  Document* d = doc();
  if (!d) return false;
  auto rt = std::make_unique<world::SceneRuntime>();
  if (!rt->build(d->scene)) {
    report(rt->error(), "error");
    json::Value a = json::Value::object();
    a.set("action", "dialog");
    a.set("dialog", dialog_json("validation"));
    pending_ui_.push_back({std::move(a)});
    return false;
  }
  const OrbitCamera cam = active_ ? active_->camera : OrbitCamera{};
  View& v = create_view(*d, ViewType::player);
  v.runtime = std::move(rt);
  v.playing = true;
  v.speed = 1;
  v.last_time = -1;
  v.camera = cam;
  const world::SceneGame& g = d->scene.game;
  if (v.runtime->game().status != world::GameStatus::inactive) {
    v.camera.radius = static_cast<f32>(std::max(1.0, std::hypot(g.camera_distance, g.camera_height)));
    v.camera.phi = static_cast<f32>(std::atan2(std::max(0.1, g.camera_distance), std::max(0.0, g.camera_height)));
    v.camera.far = max_of(v.camera.far, 2000.0f);
    report("Player opened · game running · WASD/arrows move, Space jumps, R restarts");
  } else {
    v.needs_fit = true;
    report("Player opened · independent Box3D simulation");
  }
  touch();
  return true;
}

bool Editor::reload_player(View* v) {
  if (!v) v = active_;
  if (!v || v->type != ViewType::player) return false;
  Document* d = document(v->doc);
  end_grab(*v);
  auto rt = std::make_unique<world::SceneRuntime>();
  if (!rt->build(d->scene)) { report(rt->error(), "error"); return false; }
  destroy_runtime(*v);
  v->runtime = std::move(rt);
  v->playing = true;
  v->last_time = -1;
  v->needs_sync = true;
  report("Player reloaded");
  touch();
  return true;
}

void Editor::step_player(View* v) {
  if (!v) v = active_;
  if (!v || v->type != ViewType::player || !v->runtime) return;
  v->playing = false;
  v->runtime->step_once();
  update_player_meshes(*v);
  touch();
}

bool Editor::begin_grab(View& v, f32 x, f32 y, i32 pointer) {
  if (v.type != ViewType::player || !v.runtime) return false;
  const Hit h = hit_test(v, x, y, Mode::body);
  if (!h.ok) return false;
  const Body* src = v.runtime->source().body(h.id);
  if (!src || src->type != BodyKind::dynamic) return false;
  end_grab(v);
  if (!v.runtime->begin_grab(h.id, to_f(h.point))) return false;
  v.grab_pointer = pointer;
  v.grab_distance = length(v.camera.eye() - to_f(h.point));
  report("Grab " + src->name + " · Box3D MotorJoint");
  return true;
}

void Editor::move_grab(View& v, f32 x, f32 y) {
  Vec3 o, dir;
  if (!v.runtime || !ray_from(v, x, y, o, dir)) return;
  v.runtime->set_grab_target(o + dir * max_of(0.05f, v.grab_distance));
}

void Editor::end_grab(View& v) {
  if (v.runtime) v.runtime->end_grab();
  v.grab_pointer = -1;
}

// ---------------------------------------------------------------------------
// pointer input

json::Value Editor::pointer(u32 id, std::string_view type, f32 x, f32 y, i32 button, i32 buttons, i32 pid, std::string_view ptype,
                            bool shift, bool ctrl, bool alt, f32 wheel) {
  json::Value result = ok_result();
  View* vp = view(id);
  if (!vp) return error_result("unknown view");
  View& v = *vp;
  (void)buttons;
  (void)alt;
  const bool touch_input = ptype == "touch";
  auto update_mouse_world = [&] {
    if (!scene()) return;
    mouse_world_ = project_on_plane(v, x, y, scene()->cursor);
    coord_status_ = "World location: " + clean_number(mouse_world_[0]) + ", " + clean_number(mouse_world_[1]) + ", " + clean_number(mouse_world_[2]);
  };

  if (type == "wheel") {
    const f32 delta = options_.reverse_zoom ? -wheel : wheel;
    const f32 zs = std::pow(0.95f, std::fabs(delta * 0.01f));
    v.camera.zoom(delta > 0 ? 1 / zs : zs);
    return result;
  }

  // Touch navigation (one finger orbits, two fingers pan and pinch) when the view is in orbit mode.
  if (touch_input && v.touch_orbit) {
    auto find_touch = [&]() -> View::Touch* {
      for (View::Touch& t : v.touches) if (t.id == pid) return &t;
      return nullptr;
    };
    auto pair = [&](f32& cx, f32& cy, f32& dist) {
      if (v.touches.size() < 2) return false;
      const View::Touch &a = v.touches[0], &b = v.touches[1];
      cx = (a.x + b.x) / 2;
      cy = (a.y + b.y) / 2;
      dist = std::hypot(a.x - b.x, a.y - b.y);
      return true;
    };
    if (type == "down") {
      activate(&v);
      v.touches.push_back({pid, x, y});
      pair(v.pinch_cx, v.pinch_cy, v.pinch_distance);
    } else if (type == "move") {
      View::Touch* t = find_touch();
      if (!t) return result;
      const f32 ox = t->x, oy = t->y;
      t->x = x;
      t->y = y;
      if (v.touches.size() == 1) {
        // As the previous mobile orbit helper: 0.007 rad per pixel, pitch clamped to +-85 degrees.
        v.camera.theta -= (x - ox) * 0.007f;
        v.camera.phi = clamp(v.camera.phi - (y - oy) * 0.007f, kPi / 2 - 1.48f, kPi / 2 + 1.48f);
      } else {
        f32 cx, cy, d;
        if (pair(cx, cy, d)) {
          v.camera.damping = false;
          v.camera.pan(cx - v.pinch_cx, cy - v.pinch_cy, v.css_height);
          if (d > 2 && v.pinch_distance > 2) v.camera.zoom(v.pinch_distance / d);
          v.camera.damping = true;
          v.pinch_cx = cx;
          v.pinch_cy = cy;
          v.pinch_distance = d;
        }
      }
    } else {
      v.touches.erase(std::remove_if(v.touches.begin(), v.touches.end(), [&](const View::Touch& t) { return t.id == pid; }), v.touches.end());
      pair(v.pinch_cx, v.pinch_cy, v.pinch_distance);
    }
    return result;
  }

  if (type == "down") {
    activate(&v);
    update_mouse_world();
    if (button == 2 || button == 1) {
      v.drag_button = button == 2 ? 2 : 1;
      v.drag_pointer = pid;
      v.last_x = x;
      v.last_y = y;
      return result;
    }
    if (button != 0) return result;
    if (v.type == ViewType::player) {
      if (begin_grab(v, x, y, pid)) result.set("capture", true);
      return result;
    }
    if (operation_) {
      if (operation_->has_start) finish_operation(true);
      return result;
    }
    const Hit h = hit_test(v, x, y, mode_);
    if (h.ok) {
      set_selection({h.id}, shift, ctrl);
      return result;
    }
    v.box = {true, x, y, x, y, pid, shift, ctrl};
    result.set("capture", true);
    return result;
  }

  if (type == "move") {
    update_mouse_world();
    if (v.grab_pointer == pid && v.runtime && v.runtime->grabbing()) { move_grab(v, x, y); return result; }
    if (v.drag_pointer == pid && v.drag_button > 0) {
      const f32 dx = x - v.last_x, dy = y - v.last_y;
      v.last_x = x;
      v.last_y = y;
      if (v.drag_button == 2) v.camera.rotate(dx, dy, v.css_height);
      else v.camera.pan(dx, dy, v.css_height);
      return result;
    }
    if (&v != active_) return result;
    if (operation_) { pointer_operation(v.id, x, y, shift, ctrl); return result; }
    if (v.box.active && v.box.pointer == pid) {
      v.box.x1 = x;
      v.box.y1 = y;
      touch();
    }
    return result;
  }

  if (type == "up" || type == "cancel" || type == "leave") {
    if (v.drag_pointer == pid) { v.drag_pointer = -1; v.drag_button = -1; }
    if (type == "up" && operation_ && v.type == ViewType::editor && button == 0 && operation_->has_start) {
      finish_operation(true);
      return result;
    }
    if (v.grab_pointer == pid) end_grab(v);
    if (v.box.active && v.box.pointer == pid) {
      const SelectBox b = v.box;
      v.box.active = false;
      if (type != "leave") {
        if (std::hypot(b.x1 - b.x0, b.y1 - b.y0) > 5) select_in_box(v, b);
        else set_selection({});
      }
      touch();
    }
    return result;
  }
  return result;
}

}  // namespace kern::editor
