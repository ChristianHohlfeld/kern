// Editor core: documents, selection, undo/redo, editing commands and the
// transform operations (a faithful port of the Editor3D workflow).
#include <cstdio>
#include <cstdlib>
#include <set>

#include "internal.h"

namespace kern::editor {

// ---------------------------------------------------------------------------
// small helpers

namespace {
constexpr const char* kModeNames[] = {"body", "fixture", "vertex", "joint", "image", "sampler", "world"};
constexpr const char* kModeLabels[] = {"Body", "Fixture", "Vertex", "Joint", "Image", "Sampler", "World"};
constexpr const char* kOperationNames[] = {"translate", "rotate", "scale", "cursor", "limit"};
}  // namespace

const char* mode_name(Mode m) { return kModeNames[mode_index(m)]; }
const char* mode_label(Mode m) { return kModeLabels[mode_index(m)]; }
bool parse_mode(std::string_view s, Mode& out) {
  for (u32 i = 0; i < kModeCount; ++i)
    if (s == kModeNames[i]) { out = static_cast<Mode>(i); return true; }
  return false;
}
const char* operation_name(OperationKind k) { return kOperationNames[static_cast<u32>(k)]; }

bool Selection::operator==(const Selection& o) const {
  for (u32 i = 0; i < kModeCount; ++i)
    if (ids[i] != o.ids[i]) return false;
  return true;
}

f64 js_number(std::string_view text) {
  std::size_t a = 0, b = text.size();
  while (a < b && std::isspace(static_cast<unsigned char>(text[a]))) ++a;
  while (b > a && std::isspace(static_cast<unsigned char>(text[b - 1]))) --b;
  if (a == b) return 0;
  const std::string t(text.substr(a, b - a));
  char* end = nullptr;
  const f64 v = std::strtod(t.c_str(), &end);
  if (end != t.c_str() + t.size()) return NAN;
  // strtod accepts "inf"/"nan"/hex, Number() does not (except "Infinity").
  for (char c : t)
    if (!(std::isdigit(static_cast<unsigned char>(c)) || c == '.' || c == '-' || c == '+' || c == 'e' || c == 'E')) return NAN;
  return v;
}

std::string clean_number(f64 v) {
  if (std::fabs(v) < 1e-9) return "0";
  const f64 r = std::round(v * 1e6) / 1e6;
  std::string s;
  json::append_number(s, r);
  return s;
}

std::string capitalize(std::string s) {
  if (!s.empty()) s[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(s[0])));
  return s;
}

json::Value ok_result() {
  json::Value r = json::Value::object();
  r.set("ok", true);
  return r;
}

json::Value error_result(const std::string& message) {
  json::Value r = json::Value::object();
  r.set("ok", false);
  r.set("error", message);
  return r;
}

std::string html_escape(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&#39;"; break;
      default: out += c;
    }
  }
  return out;
}

void add_fixture_bounds(Bounds& out, const DPose& body, const Fixture& f) {
  const DPose fp = compose(body, f.center, f.rotation);
  auto add_local = [&](const D3& p) { out.add(local_to_world(fp, p)); };
  switch (f.shape) {
    case FixtureShape::box: {
      const D3 h = mul(f.size, 0.5);
      for (int i = 0; i < 8; ++i) add_local({i & 1 ? h[0] : -h[0], i & 2 ? h[1] : -h[1], i & 4 ? h[2] : -h[2]});
      break;
    }
    case FixtureShape::sphere: {
      const D3 c = fp.position;
      const f64 r = std::max(0.001, f.radius);
      out.add(sub(c, {r, r, r}));
      out.add(add(c, {r, r, r}));
      break;
    }
    case FixtureShape::capsule: {
      const f64 r = std::max(0.001, f.radius);
      for (f64 s : {-0.5, 0.5}) {
        const D3 c = local_to_world(fp, {0, 0, s * f.length});
        out.add(sub(c, {r, r, r}));
        out.add(add(c, {r, r, r}));
      }
      break;
    }
    case FixtureShape::hull:
      for (const D3& p : f.vertices)
        if (finite(p)) add_local(p);
      break;
  }
}

// ---------------------------------------------------------------------------
// construction

Editor::Editor() = default;
Editor::~Editor() { shutdown(); }

bool Editor::init(Renderer* renderer, bool player_only) {
  if (!renderer) return fail("editor needs a renderer");
  renderer_ = renderer;
  player_only_ = player_only;
  if (!player_only) {
    report("Ready. Space opens the action menu; F1 lists keyboard controls.");
  }
  return true;
}

void Editor::shutdown() {
  for (auto& v : views_) {
    destroy_runtime(*v);
    clear_view_meshes(*v);
  }
  views_.clear();
  documents_.clear();
  if (renderer_) {
    for (u32 m : shared_meshes_) renderer_->remove(m);
    for (auto& [key, img] : images_) {
      if (img.texture) renderer_->delete_texture(img.texture);
      if (img.texture_nearest) renderer_->delete_texture(img.texture_nearest);
    }
  }
  shared_meshes_.clear();
  images_.clear();
  active_ = nullptr;
}

// ---------------------------------------------------------------------------
// documents, views, selection

Document* Editor::doc() {
  if (active_) return document(active_->doc);
  return documents_.empty() ? nullptr : documents_.front().get();
}

world::Scene* Editor::scene() {
  Document* d = doc();
  return d ? &d->scene : nullptr;
}

Document* Editor::document(u32 id) {
  for (auto& d : documents_)
    if (d->id == id) return d.get();
  return nullptr;
}

View* Editor::view(u32 id) {
  for (auto& v : views_)
    if (v->id == id) return v.get();
  return nullptr;
}

View* Editor::active_view() { return active_; }

std::vector<std::string>& Editor::selection_ref(Mode m) { return doc()->selection[m]; }

const std::vector<std::string>& Editor::selection(Mode m) const {
  static const std::vector<std::string> empty;
  const Document* d = const_cast<Editor*>(this)->doc();
  return d ? d->selection[m] : empty;
}

Snapshot Editor::snapshot(const Document* d) const {
  if (!d) d = const_cast<Editor*>(this)->doc();
  return {d->scene, d->selection};
}

void Editor::restore(const Snapshot& s, Document* d) {
  if (!d) d = doc();
  d->scene = s.scene;
  d->selection = s.selection;
  ++d->rev;
  touch();
}

namespace {
bool same_scene(const world::Scene& a, const world::Scene& b) {
  if (a.images.size() != b.images.size()) return false;
  for (std::size_t i = 0; i < a.images.size(); ++i) {
    const auto& sa = a.images[i].src;
    const auto& sb = b.images[i].src;
    if (sa != sb && !(sa && sb && *sa == *sb)) return false;
  }
  return json::stringify(to_json(a, false)) == json::stringify(to_json(b, false));
}
}  // namespace

void Editor::commit(const std::string& label, const Snapshot& before, Document* d) {
  if (!d) d = doc();
  Snapshot after = snapshot(d);
  if (before.selection == after.selection && same_scene(before.scene, after.scene)) return;
  d->history.resize(d->index);
  d->history.push_back({label, before, std::move(after)});
  if (d->history.size() > 150) d->history.erase(d->history.begin());
  d->index = d->history.size();
  d->dirty = true;
  ++d->rev;
  touch();
  schedule_save();
}

bool Editor::edit(const std::string& label, const std::function<bool()>& fn) {
  if (operation_) finish_operation(false);
  Document* d = doc();
  if (!d) return false;
  const Snapshot before = snapshot(d);
  const std::vector<std::string> errors_before = world::validate(d->scene);
  if (!fn()) {
    restore(before, d);
    return false;
  }
  // Roll back edits that introduce new problems (pre-existing ones do not block editing).
  std::vector<std::string> fresh;
  for (const std::string& e : world::validate(d->scene))
    if (!contains(errors_before, e)) fresh.push_back(e);
  if (!fresh.empty()) {
    std::string msg;
    for (const std::string& e : fresh) msg += (msg.empty() ? "" : "\n") + e;
    restore(before, d);
    report(msg, "error");
    return false;
  }
  commit(label, before, d);
  return true;
}

void Editor::undo() {
  if (operation_) { finish_operation(false); return; }
  Document* d = doc();
  if (!d || !d->index) return;
  restore(d->history[--d->index].before, d);
  d->dirty = true;
  touch();
  schedule_save();
}

void Editor::redo() {
  Document* d = doc();
  if (!d || d->index >= d->history.size()) return;
  restore(d->history[d->index++].after, d);
  d->dirty = true;
  touch();
  schedule_save();
}

void Editor::jump_history(i64 index) {
  Document* d = doc();
  if (!d || d->history.empty()) return;
  if (operation_) finish_operation(false);
  if (index < 0) {
    restore(d->history[0].before, d);
    d->index = 0;
  } else if (static_cast<std::size_t>(index) < d->history.size()) {
    restore(d->history[static_cast<std::size_t>(index)].after, d);
    d->index = static_cast<std::size_t>(index) + 1;
  }
  touch();
  schedule_save();
}

void Editor::set_selection(const std::vector<std::string>& ids, bool shift, bool toggle, bool record) {
  Document* d = doc();
  if (!d) return;
  const Snapshot before = snapshot(d);
  std::vector<std::string> result = ids;
  std::vector<std::string>& sel = d->selection[mode_];
  if (shift) {
    result = sel;
    for (const std::string& id : ids)
      if (!contains(result, id)) result.push_back(id);
  }
  if (toggle) {
    result = sel;
    for (const std::string& id : ids) {
      auto it = std::find(result.begin(), result.end(), id);
      if (it != result.end()) result.erase(it);
      else result.push_back(id);
    }
  }
  sel = result;
  if (record) commit(std::string("Select ") + mode_name(mode_), before, d);
  else touch();
}

void Editor::set_selection_in(Mode m, const std::vector<std::string>& ids) {
  const Mode old = mode_;
  mode_ = m;
  set_selection(ids);
  mode_ = old;
}

void Editor::set_mode(Mode m) {
  if (operation_) finish_operation(false);
  mode_ = m;
  for (auto& v : views_) v->needs_sync = true;
  touch();
}

void Editor::report(const std::string& message, const std::string& type) {
  log_.push_back({message, type});
  if (log_.size() > 1000) log_.erase(log_.begin(), log_.begin() + 250);
  status_message_ = message;
  touch();
}

void Editor::schedule_save() { save_due_ = now_ + 350; }

std::string Editor::take_autosave(f64 now_ms) {
  now_ = now_ms;
  if (save_due_ < 0 || now_ms < save_due_) return {};
  save_due_ = -1;
  json::Value all = json::Value::array();
  for (const auto& d : documents_) all.push(to_json(d->scene));
  return json::stringify(all);
}

bool Editor::any_dirty() const {
  for (const auto& d : documents_)
    if (d->dirty) return true;
  return false;
}

Document& Editor::add_document(world::Scene s) {
  auto d = std::make_unique<Document>();
  d->id = next_doc_++;
  d->scene = std::move(s);
  Document& ref = *d;
  documents_.push_back(std::move(d));
  View& v = create_view(ref, ViewType::editor);
  v.needs_fit = true;
  touch();
  schedule_save();
  return ref;
}

View& Editor::create_view(Document& d, ViewType type) {
  // Every view owns one renderer layer bit (0..30; bit 31 is the shared grid),
  // so at most 31 views exist at once. At the limit the oldest view whose
  // closing loses nothing is closed: a player first, then an editor view of
  // an unmodified document or of a document shown in another view as well.
  if (views_.size() >= kMaxViews) {
    auto doc_views = [&](u32 doc) { u32 n = 0; for (auto& w : views_) n += w->doc == doc; return n; };
    View* victim = nullptr;
    for (int pass = 0; pass < 2 && !victim; ++pass)
      for (auto& w : views_) {
        if (w.get() == active_ || w->doc == d.id) continue;
        const Document* wd = document(w->doc);
        const bool safe = pass == 0 ? w->type == ViewType::player : (!wd || !wd->dirty || doc_views(w->doc) > 1);
        if (safe) { victim = w.get(); break; }
      }
    if (victim) {
      const Document* vd = document(victim->doc);
      report("View limit (" + std::to_string(kMaxViews) + ") reached: closed " + std::string(victim->type == ViewType::player ? "(Run) " : "") +
                 (vd ? vd->scene.name : std::string("view")), "warn");
      remove_view(victim);
    }
  }
  u32 used = 0;
  for (auto& v : views_) used |= 1u << v->layer;
  u32 layer = 0;
  while (layer < 31 && (used & (1u << layer))) ++layer;
  auto v = std::make_unique<View>();
  v->id = next_view_++;
  v->doc = d.id;
  v->type = type;
  v->layer = layer < 31 ? layer : 30;
  v->camera.set_eye({8, -11, 7});
  v->touch_orbit = mobile_;
  View& ref = *v;
  views_.push_back(std::move(v));
  activate(&ref);
  return ref;
}

void Editor::activate(View* v) {
  if (active_ == v) return;
  if (operation_) finish_operation(false);
  active_ = v;
  touch();
}

void Editor::close_view(View* v, bool force) {
  if (!v) return;
  u32 same = 0;
  for (auto& w : views_) same += w->doc == v->doc;
  Document* d = document(v->doc);
  if (!force && same == 1 && d && d->dirty) {
    json::Value dlg = dialog_json("closeScene");
    dlg.set("view", v->id);
    json::Value a = json::Value::object();
    a.set("action", "dialog");
    a.set("dialog", std::move(dlg));
    pending_ui_.push_back({std::move(a)});
    return;
  }
  remove_view(v);
}

void Editor::remove_view(View* v) {
  end_grab(*v);
  destroy_runtime(*v);
  clear_view_meshes(*v);
  const u32 doc_id = v->doc;
  views_.erase(std::remove_if(views_.begin(), views_.end(), [&](const std::unique_ptr<View>& w) { return w.get() == v; }), views_.end());
  bool doc_used = false;
  for (auto& w : views_) doc_used |= w->doc == doc_id;
  if (!doc_used)
    documents_.erase(std::remove_if(documents_.begin(), documents_.end(), [&](const std::unique_ptr<Document>& d) { return d->id == doc_id; }),
                     documents_.end());
  active_ = views_.empty() ? nullptr : views_.back().get();
  if (views_.empty() && !player_only_) add_document(new_scene());
  touch();
  schedule_save();
}

void Editor::clone_view() {
  if (!active_) return;
  if (active_->type == ViewType::player) { open_player(); return; }
  View* old = active_;
  Document* d = doc();
  View& n = create_view(*d, ViewType::editor);
  n.camera = old->camera;
  tiled_ = true;
  touch();
}

// ---------------------------------------------------------------------------
// items

std::vector<Editor::ItemRef> Editor::all_items(Mode m, const world::Scene* scene_in) const {
  std::vector<ItemRef> out;
  const Document* d = const_cast<Editor*>(this)->doc();
  if (!d && !scene_in) return out;
  const world::Scene& s = scene_in ? *scene_in : d->scene;
  switch (m) {
    case Mode::body:
      for (const Body& b : s.bodies) out.push_back({m, b.id, {}, -1, {}, b.name});
      break;
    case Mode::fixture:
      for (const Body& b : s.bodies)
        for (const Fixture& f : b.fixtures) out.push_back({m, f.id, {}, -1, {}, f.name});
      break;
    case Mode::vertex:
      for (const Body& b : s.bodies)
        for (const Fixture& f : b.fixtures)
          for (std::size_t i = 0; i < f.vertices.size(); ++i)
            out.push_back({m, f.id + ":" + std::to_string(i), f.id, static_cast<i32>(i), f.vertices[i], {}});
      break;
    case Mode::joint:
      for (const Joint& j : s.joints) out.push_back({m, j.id, {}, -1, {}, j.name});
      break;
    case Mode::image:
      for (const ImagePlane& i : s.images) out.push_back({m, i.id, {}, -1, {}, i.name});
      break;
    case Mode::sampler:
      for (const Sampler& i : s.samplers) out.push_back({m, i.id, {}, -1, {}, i.name});
      break;
    case Mode::world: break;
  }
  return out;
}

std::vector<Editor::ItemRef> Editor::chosen(Mode m) const {
  std::vector<ItemRef> out;
  const std::vector<std::string>& sel = selection(m);
  for (ItemRef& i : all_items(m))
    if (contains(sel, i.id)) out.push_back(std::move(i));
  return out;
}

D3 Editor::item_point(const ItemRef& item, Mode m, const world::Scene& s) const {
  auto pose_of = [&](const std::string& body) -> std::optional<DPose> {
    if (const Body* b = body.empty() ? nullptr : s.body(body)) return b->pose();
    return std::nullopt;
  };
  switch (m) {
    case Mode::body:
      if (const Body* b = s.body(item.id)) return b->position;
      break;
    case Mode::fixture:
      if (const Body* b = s.body_of_fixture(item.id))
        for (const Fixture& f : b->fixtures)
          if (f.id == item.id) return local_to_world(b->pose(), f.center);
      break;
    case Mode::vertex:
      if (const Body* b = s.body_of_fixture(item.fixture)) return local_to_world(b->pose(), item.position);
      return item.position;
    case Mode::joint:
      for (const Joint& j : s.joints)
        if (j.id == item.id) {
          const auto p = pose_of(j.body_a);
          return p ? local_to_world(*p, j.anchor_a) : j.anchor_a;
        }
      break;
    case Mode::image:
      for (const ImagePlane& i : s.images)
        if (i.id == item.id) {
          const auto p = pose_of(i.body);
          return p ? local_to_world(*p, i.center) : i.center;
        }
      break;
    case Mode::sampler:
      for (const Sampler& i : s.samplers)
        if (i.id == item.id) {
          const auto p = pose_of(i.body);
          return p ? local_to_world(*p, i.center) : i.center;
        }
      break;
    case Mode::world: break;
  }
  return s.cursor;
}

D3 Editor::center_of(const world::Scene& s, Mode m) const {
  // Items as they exist in `s` (the operation's before-state or the live scene).
  const std::vector<std::string>& sel = selection(m);
  D3 sum{};
  u32 n = 0;
  for (const ItemRef& i : all_items(m, &s))
    if (contains(sel, i.id)) { sum = add(sum, item_point(i, m, s)); ++n; }
  return n ? mul(sum, 1.0 / n) : D3{};
}

// ---------------------------------------------------------------------------
// editing commands

void Editor::add_body(world::BodyKind type, const std::string& shape) {
  static const char* labels[] = {"static", "kinematic", "dynamic"};
  static const char* names[] = {"staticbody", "kinematicbody", "dynamicbody"};
  const i32 t = std::clamp(static_cast<i32>(type), 0, 2);
  edit(std::string("Add ") + labels[t] + " body", [&] {
    world::Scene& s = *scene();
    Body b = new_body(type, s.cursor, names[t] + std::to_string(s.bodies.size()));
    FixtureShape fs;
    if (!shape.empty() && parse_shape(shape, fs)) b.fixtures.push_back(new_fixture(fs, "fixture" + std::to_string(s.fixture_count())));
    doc()->selection[Mode::body] = {b.id};
    s.bodies.push_back(std::move(b));
    mode_ = Mode::body;
    return true;
  });
}

void Editor::add_fixture(world::FixtureShape shape) {
  std::vector<ItemRef> bodies = chosen(Mode::body);
  if (bodies.empty()) { report("Select the body that should own the fixture first (B).", "warn"); return; }
  edit(std::string("Add ") + shape_name(shape) + " fixture", [&] {
    world::Scene& s = *scene();
    std::vector<std::string> ids;
    for (const ItemRef& br : bodies) {
      Body* b = s.body(br.id);
      Fixture f = new_fixture(shape, "fixture" + std::to_string(s.fixture_count()));
      f.center = world_to_local(b->pose(), s.cursor);
      ids.push_back(f.id);
      b->fixtures.push_back(std::move(f));
    }
    mode_ = Mode::fixture;
    doc()->selection[Mode::fixture] = ids;
    return true;
  });
}

void Editor::add_joint(world::JointKind type) {
  const std::vector<std::string> ids = selection(Mode::body);
  if (ids.size() != 2) {
    report("Select exactly two bodies (B, Shift+click). Selection order defines body A and B.", "warn");
    return;
  }
  edit(std::string("Add ") + joint_kind_name(type) + " joint", [&] {
    world::Scene& s = *scene();
    const Body* a = s.body(ids[0]);
    const Body* b = s.body(ids[1]);
    if (!a || !b) return false;
    const D3 p = s.cursor;
    Joint j;
    j.id = make_uid();
    j.name = std::string(joint_kind_name(type)) + "joint" + std::to_string(s.joints.size());
    j.type = type;
    j.body_a = a->id;
    j.body_b = b->id;
    j.anchor_a = world_to_local(a->pose(), p);
    j.anchor_b = world_to_local(b->pose(), p);
    j.axis_a = {1, 0, 0};
    j.axis_b = D3{1, 0, 0};
    j.lower_limit = -1;
    j.upper_limit = 1;
    j.motor_speed = 0;
    j.max_motor_torque = 40;
    j.max_motor_force = 100;
    j.max_force = 100;
    j.max_torque = 40;
    j.max_length = 1;
    j.length = std::max(0.001, length(sub(a->position, b->position)));
    j.spring_frequency = 4;
    j.spring_damping_ratio = 0.7;
    j.frequency = 5;
    j.damping_ratio = 1;
    doc()->selection[Mode::joint] = {j.id};
    s.joints.push_back(std::move(j));
    mode_ = Mode::joint;
    return true;
  });
}

void Editor::add_sampler() {
  edit("Add sampler", [&] {
    world::Scene& s = *scene();
    Sampler i;
    i.id = make_uid();
    i.name = "sampler" + std::to_string(s.samplers.size());
    i.center = s.cursor;
    i.opacity = 1;
    doc()->selection[Mode::sampler] = {i.id};
    s.samplers.push_back(std::move(i));
    mode_ = Mode::sampler;
    return true;
  });
}

bool Editor::add_image(std::string_view file_name, std::string_view data_url, i32 width, i32 height) {
  static const char* types[] = {"data:image/png", "data:image/jpeg", "data:image/webp", "data:image/gif"};
  bool ok = false;
  for (const char* t : types) ok |= data_url.substr(0, std::strlen(t)) == t;
  if (!ok) { report("Use PNG, JPEG, WebP or GIF images", "error"); return false; }
  if (data_url.size() > 21u * 1024 * 1024) { report("Image exceeds 15 MB", "error"); return false; }
  if (width < 1 || height < 1) { report("Could not read image", "error"); return false; }
  Document* d = doc();
  if (!d) return false;
  if (operation_) finish_operation(false);
  const Snapshot before = snapshot(d);
  ImagePlane i;
  i.id = make_uid();
  i.name = std::string(file_name);
  i.file = std::string(file_name);
  i.src = std::make_shared<const std::string>(data_url);
  i.center = d->scene.cursor;
  i.size[0] = std::max(0.1, static_cast<f64>(width) / height * 2);
  i.size[1] = 2;
  i.opacity = 1;
  d->selection[Mode::image] = {i.id};
  d->scene.images.push_back(std::move(i));
  mode_ = Mode::image;
  commit("Add image", before, d);
  report("Added " + std::string(file_name));
  return true;
}

void Editor::delete_selected() {
  if (selection(mode_).empty()) return;
  edit(std::string("Delete ") + mode_name(mode_), [&] {
    world::Scene& s = *scene();
    const std::vector<std::string> ids = selection(mode_);
    auto has = [&](const std::string& id) { return contains(ids, id); };
    if (mode_ == Mode::body) {
      const std::vector<Body> old = s.bodies;
      s.bodies.erase(std::remove_if(s.bodies.begin(), s.bodies.end(), [&](const Body& b) { return has(b.id); }), s.bodies.end());
      s.joints.erase(std::remove_if(s.joints.begin(), s.joints.end(), [&](const Joint& j) { return has(j.body_a) || has(j.body_b); }), s.joints.end());
      auto detach = [&](std::string& body, D3& center, DQ& rotation) {
        if (!has(body)) return;
        for (const Body& ob : old)
          if (ob.id == body) {
            const DPose wt = compose(ob.pose(), center, rotation);
            center = wt.position;
            rotation = wt.rotation;
          }
        body.clear();
      };
      for (ImagePlane& i : s.images) detach(i.body, i.center, i.rotation);
      for (Sampler& i : s.samplers) detach(i.body, i.center, i.rotation);
    }
    if (mode_ == Mode::fixture)
      for (Body& b : s.bodies)
        b.fixtures.erase(std::remove_if(b.fixtures.begin(), b.fixtures.end(), [&](const Fixture& f) { return has(f.id); }), b.fixtures.end());
    if (mode_ == Mode::vertex)
      for (Body& b : s.bodies)
        for (Fixture& f : b.fixtures) {
          if (f.shape != FixtureShape::hull) continue;
          std::vector<D3> keep;
          for (std::size_t i = 0; i < f.vertices.size(); ++i)
            if (!has(f.id + ":" + std::to_string(i))) keep.push_back(f.vertices[i]);
          f.vertices = std::move(keep);
        }
    if (mode_ == Mode::joint) s.joints.erase(std::remove_if(s.joints.begin(), s.joints.end(), [&](const Joint& j) { return has(j.id); }), s.joints.end());
    if (mode_ == Mode::image) s.images.erase(std::remove_if(s.images.begin(), s.images.end(), [&](const ImagePlane& i) { return has(i.id); }), s.images.end());
    if (mode_ == Mode::sampler) s.samplers.erase(std::remove_if(s.samplers.begin(), s.samplers.end(), [&](const Sampler& i) { return has(i.id); }), s.samplers.end());
    doc()->selection[mode_].clear();
    return true;
  });
}

void Editor::duplicate(bool offset) {
  const Mode m = mode_;
  if (selection(m).empty() || m == Mode::vertex || m == Mode::world) return;
  const bool ok = edit(std::string("Duplicate ") + mode_name(m), [&] {
    world::Scene& s = *scene();
    const std::vector<std::string> sel = selection(m);
    std::vector<std::string> ids;
    if (m == Mode::body) {
      std::vector<std::pair<std::string, std::string>> map;
      auto mapped = [&](const std::string& id) -> std::string {
        for (auto& [a, b] : map) if (a == id) return b;
        return {};
      };
      const std::vector<Body> originals = s.bodies;
      for (const Body& b : originals) {
        if (!contains(sel, b.id)) continue;
        Body q = b;
        q.id = make_uid();
        map.push_back({b.id, q.id});
        q.name += " copy";
        for (Fixture& f : q.fixtures) f.id = make_uid();
        ids.push_back(q.id);
        s.bodies.push_back(std::move(q));
      }
      const std::vector<Joint> joints = s.joints;
      for (const Joint& j : joints)
        if (!mapped(j.body_a).empty() && !mapped(j.body_b).empty()) {
          Joint q = j;
          q.id = make_uid();
          q.name += " copy";
          q.body_a = mapped(j.body_a);
          q.body_b = mapped(j.body_b);
          s.joints.push_back(std::move(q));
        }
      const std::vector<ImagePlane> images = s.images;
      for (const ImagePlane& i : images)
        if (!mapped(i.body).empty()) { ImagePlane q = i; q.id = make_uid(); q.body = mapped(i.body); s.images.push_back(std::move(q)); }
      const std::vector<Sampler> samplers = s.samplers;
      for (const Sampler& i : samplers)
        if (!mapped(i.body).empty()) { Sampler q = i; q.id = make_uid(); q.body = mapped(i.body); s.samplers.push_back(std::move(q)); }
    } else if (m == Mode::fixture) {
      for (Body& b : s.bodies) {
        const std::vector<Fixture> fs = b.fixtures;
        for (const Fixture& f : fs)
          if (contains(sel, f.id)) { Fixture q = f; q.id = make_uid(); q.name += " copy"; ids.push_back(q.id); b.fixtures.push_back(std::move(q)); }
      }
    } else if (m == Mode::joint) {
      const std::vector<Joint> js = s.joints;
      for (const Joint& j : js)
        if (contains(sel, j.id)) { Joint q = j; q.id = make_uid(); q.name += " copy"; ids.push_back(q.id); s.joints.push_back(std::move(q)); }
    } else if (m == Mode::image) {
      const std::vector<ImagePlane> is = s.images;
      for (const ImagePlane& i : is)
        if (contains(sel, i.id)) { ImagePlane q = i; q.id = make_uid(); q.name += " copy"; ids.push_back(q.id); s.images.push_back(std::move(q)); }
    } else if (m == Mode::sampler) {
      const std::vector<Sampler> is = s.samplers;
      for (const Sampler& i : is)
        if (contains(sel, i.id)) { Sampler q = i; q.id = make_uid(); q.name += " copy"; ids.push_back(q.id); s.samplers.push_back(std::move(q)); }
    }
    doc()->selection[m] = ids;
    return true;
  });
  if (!ok) return;
  begin_operation(OperationKind::translate);
  if (offset && operation_) {
    operation_->typed = "1";
    operation_->axis = 'x';
    apply_typed_operation();
  }
}

void Editor::copy_selection() {
  if (selection(mode_).empty()) return;
  const world::Scene& s = *scene();
  const std::vector<std::string>& sel = selection(mode_);
  Clipboard c;
  c.mode = mode_;
  c.valid = true;
  if (mode_ == Mode::body) {
    c.scene = new_scene("Clipboard");
    for (const Body& b : s.bodies) if (contains(sel, b.id)) c.scene.bodies.push_back(b);
    for (const Joint& j : s.joints) if (contains(sel, j.body_a) && contains(sel, j.body_b)) c.scene.joints.push_back(j);
    for (const ImagePlane& i : s.images) if (contains(sel, i.body)) c.scene.images.push_back(i);
    for (const Sampler& i : s.samplers) if (contains(sel, i.body)) c.scene.samplers.push_back(i);
  } else if (mode_ == Mode::fixture) {
    for (const Body& b : s.bodies) for (const Fixture& f : b.fixtures) if (contains(sel, f.id)) c.fixtures.push_back(f);
  } else if (mode_ == Mode::joint) {
    for (const Joint& j : s.joints) if (contains(sel, j.id)) c.joints.push_back(j);
  } else if (mode_ == Mode::image) {
    for (const ImagePlane& i : s.images) if (contains(sel, i.id)) c.images.push_back(i);
  } else if (mode_ == Mode::sampler) {
    for (const Sampler& i : s.samplers) if (contains(sel, i.id)) c.samplers.push_back(i);
  } else {
    return;
  }
  clipboard_ = std::move(c);
  // The system clipboard gets a JSON copy (as the previous editor did).
  json::Value text = json::Value::object();
  text.set("format", "editor3d-clipboard");
  text.set("mode", mode_name(mode_));
  if (mode_ == Mode::body) text.set("scene", to_json(clipboard_.scene));
  json::Value a = json::Value::object();
  a.set("action", "clipboard");
  a.set("text", json::stringify(text));
  pending_ui_.push_back({std::move(a)});
  report("Copied " + std::to_string(sel.size()) + " " + mode_name(mode_) + " item(s)");
}

void Editor::paste() {
  if (!clipboard_.valid) { report("Clipboard is empty.", "warn"); return; }
  const Mode m = clipboard_.mode;
  if (m == Mode::fixture && chosen(Mode::body).empty()) { report("Select target bodies before pasting fixtures.", "warn"); return; }
  const bool ok = edit(std::string("Paste ") + mode_name(m), [&] {
    world::Scene& s = *scene();
    std::vector<std::string> ids;
    if (m == Mode::body) {
      world::Scene c = clipboard_.scene;
      std::vector<std::pair<std::string, std::string>> map;
      auto mapped = [&](const std::string& id) -> std::string {
        for (auto& [a, b] : map) if (a == id) return b;
        return {};
      };
      for (Body& b : c.bodies) {
        const std::string old = b.id;
        b.id = make_uid();
        map.push_back({old, b.id});
        for (Fixture& f : b.fixtures) f.id = make_uid();
        ids.push_back(b.id);
        s.bodies.push_back(b);
      }
      for (Joint& j : c.joints) { j.id = make_uid(); j.body_a = mapped(j.body_a); j.body_b = mapped(j.body_b); s.joints.push_back(j); }
      for (ImagePlane& i : c.images) { i.id = make_uid(); i.body = mapped(i.body); s.images.push_back(i); }
      for (Sampler& i : c.samplers) { i.id = make_uid(); i.body = mapped(i.body); s.samplers.push_back(i); }
    } else if (m == Mode::fixture) {
      for (const ItemRef& br : chosen(Mode::body)) {
        Body* b = s.body(br.id);
        for (Fixture f : clipboard_.fixtures) { f.id = make_uid(); ids.push_back(f.id); b->fixtures.push_back(std::move(f)); }
      }
    } else if (m == Mode::joint) {
      for (Joint j : clipboard_.joints) {
        j.id = make_uid();
        if (!s.body(j.body_a) || !s.body(j.body_b)) continue;
        ids.push_back(j.id);
        s.joints.push_back(std::move(j));
      }
    } else if (m == Mode::image) {
      for (ImagePlane i : clipboard_.images) { i.id = make_uid(); if (!i.body.empty() && !s.body(i.body)) i.body.clear(); ids.push_back(i.id); s.images.push_back(std::move(i)); }
    } else if (m == Mode::sampler) {
      for (Sampler i : clipboard_.samplers) { i.id = make_uid(); if (!i.body.empty() && !s.body(i.body)) i.body.clear(); ids.push_back(i.id); s.samplers.push_back(std::move(i)); }
    }
    mode_ = m;
    doc()->selection[m] = ids;
    return true;
  });
  if (ok) begin_operation(OperationKind::translate);
}

void Editor::select_all(bool clear) {
  std::vector<std::string> ids;
  if (!clear)
    for (const ItemRef& i : all_items(mode_)) ids.push_back(i.id);
  set_selection(ids);
}

void Editor::add_hull_vertex() {
  std::vector<std::string> hulls;
  for (const ItemRef& i : chosen(Mode::fixture))
    if (const Fixture* f = scene()->fixture(i.id); f && f->shape == FixtureShape::hull) hulls.push_back(i.id);
  if (hulls.empty()) { report("Select a hull fixture first.", "warn"); return; }
  edit("Add hull vertex", [&] {
    world::Scene& s = *scene();
    std::vector<std::string> ids;
    for (const std::string& fid : hulls) {
      Body* b = s.body_of_fixture(fid);
      Fixture* f = s.fixture(fid);
      f->vertices.push_back(world_to_local(b->pose(), s.cursor));
      ids.push_back(fid + ":" + std::to_string(f->vertices.size() - 1));
    }
    mode_ = Mode::vertex;
    doc()->selection[Mode::vertex] = ids;
    return true;
  });
}

void Editor::weld_vertices() {
  if (mode_ != Mode::vertex || selection(Mode::vertex).size() < 2) return;
  const D3 c = center_of(*scene(), Mode::vertex);
  edit("Weld vertices together", [&] {
    world::Scene& s = *scene();
    for (const ItemRef& v : chosen(Mode::vertex)) {
      Fixture* f = s.fixture(v.fixture);
      Body* b = s.body_of_fixture(v.fixture);
      if (f && b && v.index >= 0 && static_cast<std::size_t>(v.index) < f->vertices.size()) f->vertices[static_cast<std::size_t>(v.index)] = world_to_local(b->pose(), c);
    }
    return true;
  });
}

void Editor::make_chain(i32 n, const D3& off, world::JointKind type) {
  const std::vector<ItemRef> bodies = chosen(Mode::body);
  if (bodies.size() != 1) { report("Select one body to create a chain of copies.", "warn"); return; }
  if (n < 2 || n > 100 || !finite(off)) { report("Invalid chain settings", "error"); return; }
  edit("Make chain", [&] {
    world::Scene& s = *scene();
    const Body original = *s.body(bodies[0].id);
    std::vector<std::string> ids{original.id};
    std::string prev = original.id;
    for (i32 k = 1; k < n; ++k) {
      Body b = original;
      b.id = make_uid();
      b.name = original.name + " " + std::to_string(k);
      b.position = add(original.position, mul(off, k));
      for (Fixture& f : b.fixtures) f.id = make_uid();
      const Body* pb = s.body(prev);
      const D3 wp = mul(add(pb->position, b.position), 0.5);
      Joint j;
      j.id = make_uid();
      j.name = std::string(joint_kind_name(type)) + " chain " + std::to_string(k);
      j.type = type;
      j.body_a = prev;
      j.body_b = b.id;
      j.anchor_a = world_to_local(pb->pose(), wp);
      j.anchor_b = world_to_local(b.pose(), wp);
      j.axis_a = {1, 0, 0};
      j.axis_b = D3{1, 0, 0};
      j.lower_limit = -1;
      j.upper_limit = 1;
      j.max_motor_torque = 20;
      j.max_motor_force = 100;
      j.max_force = 100;
      j.max_torque = 20;
      j.max_length = length(off);
      j.length = length(off);
      j.spring_frequency = 4;
      j.spring_damping_ratio = 0.7;
      j.frequency = 5;
      j.damping_ratio = 1;
      ids.push_back(b.id);
      prev = b.id;
      s.bodies.push_back(std::move(b));
      s.joints.push_back(std::move(j));
    }
    doc()->selection[Mode::body] = ids;
    return true;
  });
}

void Editor::set_cursor(const D3& p) {
  if (!finite(p)) { report("Invalid position", "error"); return; }
  edit("Set cursor", [&] { scene()->cursor = p; return true; });
}

void Editor::cursor_to_selection() {
  const D3 c = center_of(*scene(), mode_);
  edit("Cursor to selection", [&] { scene()->cursor = c; return true; });
}

void Editor::rename(const std::string& id, const std::string& name) {
  edit("Rename item", [&] {
    world::Scene& s = *scene();
    if (Body* b = s.body(id)) { b->name = name; return true; }
    if (Fixture* f = s.fixture(id)) { f->name = name; return true; }
    if (Joint* j = s.joint(id)) { j->name = name; return true; }
    if (ImagePlane* i = s.image(id)) { i->name = name; return true; }
    if (Sampler* i = s.sampler(id)) { i->name = name; return true; }
    return false;
  });
}

void Editor::sampler_to_fixture() {
  const std::vector<ItemRef> items = chosen(Mode::sampler);
  if (items.empty()) return;
  edit("Create fixtures from sampler", [&] {
    world::Scene& s = *scene();
    std::vector<std::string> ids;
    for (const ItemRef& ir : items) {
      const Sampler smp = *s.sampler(ir.id);
      Body* b = smp.body.empty() ? nullptr : s.body(smp.body);
      if (!b) {
        s.bodies.push_back(new_body(BodyKind::fixed, {}, "staticbody" + std::to_string(s.bodies.size())));
        b = &s.bodies.back();
      }
      Fixture f = new_fixture(FixtureShape::box, "sampled box");
      f.size = smp.size;
      const Body* sb = smp.body.empty() ? nullptr : s.body(smp.body);
      const D3 wp = sb ? local_to_world(sb->pose(), smp.center) : smp.center;
      f.center = world_to_local(b->pose(), wp);
      f.rotation = smp.rotation;
      ids.push_back(f.id);
      b->fixtures.push_back(std::move(f));
    }
    mode_ = Mode::fixture;
    doc()->selection[Mode::fixture] = ids;
    return true;
  });
}

bool Editor::load_document(std::string_view text, std::string_view name) {
  world::Scene s;
  std::string err;
  bool migrated = false;
  if (!world::from_text(text, s, &err, name, &migrated)) {
    report(std::string(name) + ": " + err, "error");
    return false;
  }
  if (migrated) report("Migrated legacy Editor3D scene");
  Document& d = add_document(std::move(s));
  const std::vector<std::string> errors = world::validate(d.scene);
  if (!errors.empty()) {
    std::string msg;
    for (const std::string& e : errors) msg += (msg.empty() ? "" : "; ") + e;
    report("Opened with validation warnings: " + msg, "warn");
  }
  report("Opened " + std::string(name));
  return true;
}

std::string Editor::scene_text(bool) { return to_text(*scene(), 2); }

std::vector<std::string> Editor::validate_scene() const {
  const Document* d = const_cast<Editor*>(this)->doc();
  return d ? world::validate(d->scene) : std::vector<std::string>{};
}

// ---------------------------------------------------------------------------
// transform operations

void Editor::begin_operation(OperationKind kind) {
  if (active_ && active_->type == ViewType::player) return;
  if (!doc()) return;
  if (kind == OperationKind::limit && mode_ != Mode::joint) { report("Joint limit editing requires Joint mode (J).", "warn"); return; }
  if (kind != OperationKind::cursor && mode_ != Mode::world && selection(mode_).empty()) { report("Select items first.", "warn"); return; }
  if (operation_) {
    if (operation_->kind == kind) {
      if (mode_ == Mode::joint && (kind == OperationKind::translate || kind == OperationKind::rotate || kind == OperationKind::limit)) {
        operation_->stage = (operation_->stage + 1) % (kind == OperationKind::limit ? 2 : 3);
        operation_->typed.clear();
        operation_->has_start = false;
        report(std::string(operation_name(kind)) + " joint stage " + std::to_string(operation_->stage + 1));
        touch();
        return;
      }
      operation_->use_cursor = !operation_->use_cursor;
      operation_->pivot = operation_->use_cursor ? scene()->cursor : center_of(operation_->before.scene, mode_);
      touch();
      return;
    }
    finish_operation(false);
  }
  Operation op;
  op.kind = kind;
  op.before = snapshot();
  op.pivot = kind == OperationKind::cursor || options_.pivot_cursor ? scene()->cursor : center_of(*scene(), mode_);
  op.use_cursor = options_.pivot_cursor;
  operation_ = std::move(op);
  touch();
}

void Editor::transform_selection(const world::Scene& source, const Operation& op, const D3& delta, f64 angle, const D3& scale) {
  world::Scene& s = *scene();
  const std::vector<std::string> ids = selection(mode_);
  auto has = [&](const std::string& id) { return contains(ids, id); };
  const char axis = op.axis ? op.axis : 'z';
  const DQ q = qaxis_named(axis, angle);
  const f64 sx = scale[0], sy = scale[1], sz = scale[2];
  auto tf = [&](const D3& p) -> D3 {
    if (op.kind == OperationKind::translate) return add(p, delta);
    if (op.kind == OperationKind::rotate) return add(op.pivot, qrotate(q, sub(p, op.pivot)));
    return {op.pivot[0] + (p[0] - op.pivot[0]) * sx, op.pivot[1] + (p[1] - op.pivot[1]) * sy, op.pivot[2] + (p[2] - op.pivot[2]) * sz};
  };
  auto scale_fixture = [&](Fixture& f, const Fixture& of) {
    if (f.shape == FixtureShape::box) f.size = {of.size[0] * std::fabs(sx), of.size[1] * std::fabs(sy), of.size[2] * std::fabs(sz)};
    if (f.shape == FixtureShape::sphere) f.radius = of.radius * std::cbrt(std::fabs(sx * sy * sz));
    if (f.shape == FixtureShape::capsule) { f.radius = of.radius * std::sqrt(std::fabs(sx * sy)); f.length = of.length * std::fabs(sz); }
    if (f.shape == FixtureShape::hull) {
      f.vertices.clear();
      for (const D3& p : of.vertices) f.vertices.push_back({p[0] * sx, p[1] * sy, p[2] * sz});
    }
  };
  auto pose_in = [](const world::Scene& sc, const std::string& id) -> std::optional<DPose> {
    if (const Body* b = id.empty() ? nullptr : sc.body(id)) return b->pose();
    if (const Body* b = id.empty() ? nullptr : sc.body_of_fixture(id)) return b->pose();
    return std::nullopt;
  };
  auto l2w = [](const std::optional<DPose>& p, const D3& v) { return p ? local_to_world(*p, v) : v; };
  auto w2l = [](const std::optional<DPose>& p, const D3& v) { return p ? world_to_local(*p, v) : v; };

  if (mode_ == Mode::world) {
    if (op.kind == OperationKind::rotate) s.world.gravity = qrotate(q, source.world.gravity);
    if (op.kind == OperationKind::scale) s.world.gravity = {source.world.gravity[0] * sx, source.world.gravity[1] * sy, source.world.gravity[2] * sz};
    return;
  }
  if (mode_ == Mode::body) {
    for (const Body& old : source.bodies) {
      if (!has(old.id)) continue;
      Body* b = s.body(old.id);
      if (!b) continue;
      b->position = tf(old.position);
      if (op.kind == OperationKind::rotate) b->rotation = qmul(q, old.rotation);
      if (op.kind == OperationKind::scale)
        for (Fixture& f : b->fixtures)
          for (const Fixture& of : old.fixtures)
            if (of.id == f.id) {
              f.center = {of.center[0] * sx, of.center[1] * sy, of.center[2] * sz};
              scale_fixture(f, of);
            }
    }
    for (const Joint& old : source.joints) {
      Joint* j = s.joint(old.id);
      if (!j) continue;
      if (has(old.body_a)) j->anchor_a = w2l(pose_in(s, old.body_a), tf(l2w(pose_in(source, old.body_a), old.anchor_a)));
      if (has(old.body_b)) j->anchor_b = w2l(pose_in(s, old.body_b), tf(l2w(pose_in(source, old.body_b), old.anchor_b)));
      if (op.kind == OperationKind::scale && has(old.body_a) && has(old.body_b)) {
        const Body* oa = source.body(old.body_a);
        const Body* na = s.body(old.body_a);
        auto mirror_vec = [&](const D3& ax) {
          const D3 w = qrotate(oa->rotation, ax);
          const D3 mw{w[0] * sx, w[1] * sy, w[2] * sz};
          return normalized(qrotate(qinv(na->rotation), mw));
        };
        j->axis_a = mirror_vec(old.axis_a);
        if (old.axis_b) j->axis_b = mirror_vec(*old.axis_b);
        const D3 old_a = local_to_world(oa->pose(), old.anchor_a), old_b = local_to_world(source.body(old.body_b)->pose(), old.anchor_b);
        const D3 new_a = local_to_world(na->pose(), j->anchor_a), new_b = local_to_world(s.body(old.body_b)->pose(), j->anchor_b);
        const f64 factor = length(sub(new_b, new_a)) / std::max(0.001, length(sub(old_b, old_a)));
        if (j->type == JointKind::distance || j->type == JointKind::rope) {
          j->length = std::max(0.001, old.length * factor);
          if (j->type == JointKind::rope) j->max_length = std::max(0.001, (old.max_length != 0 ? old.max_length : old.length) * factor);
        }
        const bool reflected = sx * sy * sz < 0;
        if (reflected && (j->type == JointKind::revolute || j->type == JointKind::spherical)) {
          j->lower_limit = -old.upper_limit;
          j->upper_limit = -old.lower_limit;
          j->motor_speed = -old.motor_speed;
        } else if (reflected && j->type == JointKind::wheel) {
          j->motor_speed = -old.motor_speed;
        }
      }
    }
    if (op.kind == OperationKind::scale) {
      for (const ImagePlane& oi : source.images)
        if (has(oi.body))
          if (ImagePlane* i = s.image(oi.id)) {
            i->center = {oi.center[0] * sx, oi.center[1] * sy, oi.center[2] * sz};
            i->size[0] = oi.size[0] * std::fabs(sx);
            i->size[1] = oi.size[1] * std::fabs(sy);
          }
      for (const Sampler& oi : source.samplers)
        if (has(oi.body))
          if (Sampler* i = s.sampler(oi.id)) {
            i->center = {oi.center[0] * sx, oi.center[1] * sy, oi.center[2] * sz};
            i->size = {oi.size[0] * std::fabs(sx), oi.size[1] * std::fabs(sy), oi.size[2] * std::fabs(sz)};
          }
    }
    return;
  }
  if (mode_ == Mode::fixture) {
    for (const Body& ob : source.bodies)
      for (const Fixture& of : ob.fixtures) {
        if (!has(of.id)) continue;
        Fixture* f = s.fixture(of.id);
        Body* b = s.body_of_fixture(of.id);
        if (!f || !b) continue;
        f->center = world_to_local(b->pose(), tf(local_to_world(ob.pose(), of.center)));
        if (op.kind == OperationKind::rotate) f->rotation = qmul(qmul_raw(qmul_raw(qinv(b->rotation), q), ob.rotation), of.rotation);
        if (op.kind == OperationKind::scale) scale_fixture(*f, of);
      }
    return;
  }
  if (mode_ == Mode::vertex) {
    for (const Body& ob : source.bodies)
      for (const Fixture& of : ob.fixtures)
        for (std::size_t i = 0; i < of.vertices.size(); ++i) {
          if (!has(of.id + ":" + std::to_string(i))) continue;
          Fixture* f = s.fixture(of.id);
          Body* b = s.body_of_fixture(of.id);
          if (f && b && i < f->vertices.size()) f->vertices[i] = world_to_local(b->pose(), tf(local_to_world(ob.pose(), of.vertices[i])));
        }
    return;
  }
  if (mode_ == Mode::joint) {
    for (const Joint& old : source.joints) {
      if (!has(old.id)) continue;
      Joint* j = s.joint(old.id);
      if (!j) continue;
      if (op.kind == OperationKind::translate) {
        if (op.stage != 1) j->anchor_a = w2l(pose_in(s, old.body_a), tf(l2w(pose_in(source, old.body_a), old.anchor_a)));
        if (op.stage != 0) j->anchor_b = w2l(pose_in(s, old.body_b), tf(l2w(pose_in(source, old.body_b), old.anchor_b)));
      }
      if (op.kind == OperationKind::rotate) {
        if (op.stage == 0) j->axis_a = qrotate(q, old.axis_a);
        else if (op.stage == 1) j->axis_b = qrotate(q, old.axis_b_or_a());
        else { j->axis_a = qrotate(q, old.axis_a); j->axis_b = qrotate(q, old.axis_b_or_a()); }
      }
      if (op.kind == OperationKind::scale && (j->type == JointKind::distance || j->type == JointKind::rope)) {
        j->length = std::max(0.001, old.length * std::cbrt(std::fabs(sx * sy * sz)));
        if (j->type == JointKind::rope) j->max_length = j->length;
      }
    }
    return;
  }
  if (mode_ == Mode::image || mode_ == Mode::sampler) {
    auto move_item = [&](const std::string& body_old, const D3& center_old, const DQ& rot_old, std::string& body, D3& center, DQ& rotation) {
      const D3 wp = l2w(pose_in(source, body_old), center_old);
      center = w2l(pose_in(s, body), tf(wp));
      if (op.kind == OperationKind::rotate) rotation = qmul(q, rot_old);
    };
    if (mode_ == Mode::image) {
      for (const ImagePlane& old : source.images)
        if (has(old.id))
          if (ImagePlane* i = s.image(old.id)) {
            move_item(old.body, old.center, old.rotation, i->body, i->center, i->rotation);
            if (op.kind == OperationKind::scale) { i->size[0] = old.size[0] * std::fabs(sx); i->size[1] = old.size[1] * std::fabs(sy); }
          }
    } else {
      for (const Sampler& old : source.samplers)
        if (has(old.id))
          if (Sampler* i = s.sampler(old.id)) {
            move_item(old.body, old.center, old.rotation, i->body, i->center, i->rotation);
            if (op.kind == OperationKind::scale) i->size = {old.size[0] * std::fabs(sx), old.size[1] * std::fabs(sy), old.size[2] * std::fabs(sz)};
          }
    }
  }
}

namespace {
bool is_angular(JointKind t) { return t == JointKind::revolute || t == JointKind::spherical; }
}  // namespace

void Editor::apply_joint_limit_typed(f64 n) {
  restore(operation_->before);
  world::Scene& s = *scene();
  for (const Joint& old : operation_->before.scene.joints) {
    if (!contains(selection(Mode::joint), old.id)) continue;
    Joint* j = s.joint(old.id);
    if (!j) continue;
    j->enable_limit = true;
    const f64 v = is_angular(j->type) ? n * (options_.degrees ? kPiD / 180 : 1) : n;
    if (j->type == JointKind::rope) {
      j->max_length = std::max(0.001, std::fabs(v));
      j->length = std::min(j->length, j->max_length);
    } else if (operation_->stage == 0) {
      j->lower_limit = v;
    } else {
      j->upper_limit = v;
    }
  }
  touch();
}

void Editor::apply_joint_limit_pointer(View& v, f32 x, f32 y) {
  restore(operation_->before);
  world::Scene& s = *scene();
  const D3 p = project_on_plane(v, x, y, operation_->pivot);
  for (const Joint& old : operation_->before.scene.joints) {
    if (!contains(selection(Mode::joint), old.id)) continue;
    Joint* j = s.joint(old.id);
    const Body* a = operation_->before.scene.body(old.body_a);
    if (!j || !a) continue;
    const D3 wp = local_to_world(a->pose(), old.anchor_a);
    j->enable_limit = true;
    f64 value;
    if (is_angular(j->type)) value = (x - operation_->start_x) * 0.01;
    else if (j->type == JointKind::prismatic || j->type == JointKind::wheel) value = dot(sub(p, wp), normalized(qrotate(a->rotation, old.axis_a)));
    else value = length(sub(p, wp));
    if (j->type == JointKind::rope) {
      j->max_length = std::max(0.001, value);
      j->length = std::min(j->length, j->max_length);
    } else if (operation_->stage == 0) {
      j->lower_limit = value;
    } else {
      j->upper_limit = value;
    }
  }
  touch();
}

void Editor::apply_typed_operation() {
  if (!operation_) return;
  Operation& op = *operation_;
  const f64 n = js_number(op.typed);
  const bool finite_n = std::isfinite(n);
  if (op.kind == OperationKind::limit && finite_n) { apply_joint_limit_typed(n); return; }
  if (mode_ == Mode::joint && op.kind == OperationKind::translate && op.stage == 2 && finite_n) {
    restore(op.before);
    for (const Joint& old : op.before.scene.joints)
      if (contains(selection(Mode::joint), old.id))
        if (Joint* j = scene()->joint(old.id); j && (j->type == JointKind::distance || j->type == JointKind::rope)) {
          j->length = std::max(0.001, std::fabs(n));
          if (j->type == JointKind::rope) j->max_length = j->length;
        }
    touch();
    return;
  }
  restore(op.before);
  D3 delta{};
  f64 angle = 0;
  D3 scale{1, 1, 1};
  const i32 ai = axis_index(op.axis ? op.axis : 'x');
  if (op.kind == OperationKind::translate && finite_n) delta[static_cast<std::size_t>(ai)] = n;
  if (op.kind == OperationKind::rotate && finite_n) angle = n * (options_.degrees ? kPiD / 180 : 1);
  if (op.kind == OperationKind::scale && finite_n) {
    if (op.axis) scale[static_cast<std::size_t>(ai)] = n;
    else scale = {n, n, n};
  }
  if (op.kind == OperationKind::cursor && finite_n) {
    D3 p = op.before.scene.cursor;
    p[static_cast<std::size_t>(ai)] += n;
    scene()->cursor = p;
  } else {
    transform_selection(op.before.scene, op, delta, angle, scale);
  }
  touch();
}

void Editor::type_operation(std::string_view text) {
  if (!operation_) return;
  operation_->typed = std::string(text);
  apply_typed_operation();
}

void Editor::set_operation_axis(char axis) {
  if (!operation_) return;
  operation_->axis = operation_->axis == axis ? 0 : axis;
  if (!operation_->typed.empty()) apply_typed_operation();
  touch();
}

void Editor::pointer_operation(u32 view_id, f32 x, f32 y, bool shift, bool ctrl) {
  if (!operation_) return;
  View* v = view(view_id);
  if (!v) return;
  Operation& op = *operation_;
  last_pointer_y_ = y;
  const D3 p = project_on_plane(*v, x, y, op.pivot);
  if (!op.has_start) {
    op.start = p;
    op.start_x = x;
    op.start_y = y;
    op.has_start = true;
    return;
  }
  if (op.kind == OperationKind::limit) { apply_joint_limit_pointer(*v, x, y); return; }
  if (mode_ == Mode::joint && op.kind == OperationKind::translate && op.stage == 2) {
    restore(op.before);
    for (const Joint& old : op.before.scene.joints)
      if (contains(selection(Mode::joint), old.id))
        if (Joint* j = scene()->joint(old.id); j && (j->type == JointKind::distance || j->type == JointKind::rope)) {
          const Body* a = op.before.scene.body(old.body_a);
          const D3 wp = a ? local_to_world(a->pose(), old.anchor_a) : old.anchor_a;
          j->length = std::max(0.001, length(sub(p, wp)));
          if (j->type == JointKind::rope) j->max_length = j->length;
        }
    touch();
    return;
  }
  restore(op.before);
  D3 delta = sub(p, op.start);
  f64 angle = (x - op.start_x) * 0.01;
  f64 fac = std::exp((x - op.start_x - y + op.start_y) * 0.005);
  if (shift) {
    delta = mul(delta, 0.1);
    angle *= 0.1;
    fac = 1 + (fac - 1) * 0.1;
  }
  if (ctrl) {
    for (f64& c : delta) c = std::round(c / options_.snap) * options_.snap;
    angle = std::round(angle / deg2rad(options_.angle_snap)) * deg2rad(options_.angle_snap);
    fac = std::round(fac / options_.scale_snap) * options_.scale_snap;
  }
  if (op.axis) {
    const i32 ai = axis_index(op.axis);
    for (i32 k = 0; k < 3; ++k)
      if (k != ai) delta[static_cast<std::size_t>(k)] = 0;
  }
  D3 scale{fac, fac, fac};
  if (op.axis) scale = op.axis == 'x' ? D3{fac, 1, 1} : op.axis == 'y' ? D3{1, fac, 1} : D3{1, 1, fac};
  if (op.kind == OperationKind::cursor) scene()->cursor = add(op.before.scene.cursor, delta);
  else transform_selection(op.before.scene, op, delta, angle, scale);
  touch();
}

void Editor::finish_operation(bool accept) {
  if (!operation_) return;
  const Operation op = std::move(*operation_);
  operation_.reset();
  if (accept) commit(capitalize(operation_name(op.kind)) + " " + mode_name(mode_), op.before);
  else restore(op.before);
  touch();
}

}  // namespace kern::editor
