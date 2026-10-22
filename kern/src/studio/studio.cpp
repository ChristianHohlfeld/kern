// Kern Studio core: the document, undo history, selection and actor operations.
#include "kern/studio/studio.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "internal.h"
#include "kern/game/demos.h"

namespace kern::studio {

const char* tool_name(Tool t) {
  static const char* names[] = {"select", "translate", "rotate", "scale"};
  return names[static_cast<u32>(t)];
}
const char* view_type_name(ViewType t) {
  static const char* names[] = {"Perspective", "Top", "Bottom", "Left", "Right", "Front", "Back"};
  return names[static_cast<u32>(t)];
}
const char* view_mode_name(ViewMode m) {
  static const char* names[] = {"Lit", "Unlit", "Wireframe", "Collision"};
  return names[static_cast<u32>(m)];
}

Vec3 ViewCamera::forward() const {
  const f32 y = yaw * kDegToRad, p = pitch * kDegToRad;
  return {std::cos(y) * std::cos(p), std::sin(y) * std::cos(p), std::sin(p)};
}
Vec3 ViewCamera::right() const {
  const f32 y = yaw * kDegToRad;
  return {std::sin(y), -std::cos(y), 0};
}

Studio::Studio() {
  mixer_ = std::make_unique<audio::Mixer>();
  bank_ = std::make_unique<game::SoundBank>(*mixer_);
  game_audio_ = std::make_unique<game::GameAudio>(*mixer_, *bank_);
  for (u32 i = 0; i < 4; ++i) views_[i].index = i;
  // Quad layout defaults like Unreal: perspective, top, front, side.
  views_[1].type = ViewType::top;
  views_[2].type = ViewType::front;
  views_[3].type = ViewType::right;
  for (u32 i = 1; i < 4; ++i) {
    views_[i].mode = ViewMode::wireframe;
    views_[i].flags.fog = false;
  }
}

Studio::~Studio() {
  if (world_) world_->stop();
  world_.reset();
  game_audio_.reset();
  bank_.reset();
  mixer_.reset();
}

// ---------------------------------------------------------------------------
// audio

bool Studio::audio_init(u32 sample_rate) {
  if (mixer_->ready() && mixer_->sample_rate() == sample_rate) return true;
  game_audio_->end(0);
  bank_->clear();
  preview_ = 0;
  if (!mixer_->init(sample_rate, 64)) return false;
  game_audio_->set_muted(game_audio_->muted());
  audio_scene_ = ~0ull;
  return true;
}

void Studio::audio_render(f32* out, u32 frames) {
  if (!out || !frames) return;
  if (!mixer_->ready()) {
    for (u32 i = 0; i < frames * audio::kOutputChannels; ++i) out[i] = 0;
    return;
  }
  mixer_->render(out, frames);
}

std::vector<std::string> Studio::pending_sounds() const { return bank_->pending_imports(); }

std::string Studio::sound_source(const std::string& id) const {
  if (const std::string* s = bank_->source(id)) return *s;
  if (const game::SoundAsset* a = level_.sound(id)) return a->src ? *a->src : std::string();
  return {};
}

bool Studio::sound_pcm(const std::string& id, const f32* samples, u32 frames, u32 channels, u32 rate) {
  const bool ok = bank_->set_pcm(id, samples, frames, channels, rate);
  if (!ok) report("Could not use the sound \"" + id + "\": " + last_error(), "error");
  return ok;
}

void Studio::sound_failed(const std::string& id, const std::string& reason) {
  bank_->set_failed(id, reason);
  const game::SoundAsset* a = level_.sound(id);
  report("Could not decode the sound \"" + (a ? a->name : id) + "\"" + (reason.empty() ? std::string() : ": " + reason), "error");
}

void Studio::preview_sound(const std::string& ref, f32 volume, f32 pitch) {
  stop_preview();
  if (ref.empty() || ref == "none" || ref == "auto") return;
  preview_ref_ = ref;
  preview_volume_ = clamp(volume, 0.0f, 4.0f);
  preview_pitch_ = clamp(pitch, 0.05f, 8.0f);
  bank_->sync_assets(level_);
  bank_->request(ref);
  touch();
}

void Studio::stop_preview() {
  if (preview_) mixer_->stop(preview_, 0.05f);
  preview_ = 0;
  preview_ref_.clear();
  touch();
}

bool Studio::previewing() const { return !preview_ref_.empty() || (preview_ && mixer_->playing(preview_)); }

std::string Studio::import_sound(const std::string& name, const std::string& src, f32 duration) {
  if (src.rfind("data:audio/", 0) != 0 && src.rfind("data:video/", 0) != 0 && src.rfind("data:application/ogg", 0) != 0) {
    report("Only audio files can be imported as sounds", "error");
    return {};
  }
  std::string id;
  edit("Import Sound", [&](game::Level& l) {
    u32 n = 1;
    for (;;) {
      id = "snd" + std::to_string(n++);
      if (!l.sound(id)) break;
    }
    game::SoundAsset s;
    s.id = id;
    s.name = name.empty() ? id : name;
    s.src = std::make_shared<const std::string>(src);
    s.duration = clamp(duration, 0.0f, 1e6f);
    l.sounds.push_back(std::move(s));
    return true;
  });
  if (!id.empty()) report("Imported sound \"" + (name.empty() ? id : name) + "\"");
  return id;
}

bool Studio::remove_sound(const std::string& id) {
  return edit("Remove Sound", [&](game::Level& l) {
    for (std::size_t i = 0; i < l.sounds.size(); ++i)
      if (l.sounds[i].id == id) {
        l.sounds.erase(l.sounds.begin() + static_cast<std::ptrdiff_t>(i));
        return true;
      }
    return false;
  });
}

void Studio::set_muted(bool muted) {
  game_audio_->set_muted(muted);
  touch();
}

void Studio::update_audio(const Camera& listener, f32 dt) {
  if (!mixer_->ready()) return;
  if (play_ == PlayState::editing) {
    if (audio_scene_ != scene_revision_) {
      // Keep the level's sounds prepared so Play starts with everything in place.
      bank_->request_level(level_);
      audio_scene_ = scene_revision_;
    }
    game_audio_->apply_settings(level_.world);
  }
  bank_->prepare(play_ == PlayState::editing ? 6 : 3);
  if (!preview_ref_.empty()) {
    if (const audio::ClipId clip = bank_->clip(preview_ref_)) {
      audio::PlayParams p;
      p.bus = audio::Bus::ui;
      p.volume = preview_volume_;
      p.pitch = preview_pitch_;
      p.priority = 1000;
      preview_ = mixer_->play(clip, p);
      preview_ref_.clear();
      touch();
    } else if (bank_->failed(preview_ref_)) {
      report("The sound \"" + preview_ref_ + "\" is not available", "warn");
      preview_ref_.clear();
      touch();
    }
  }
  if (preview_ && !mixer_->playing(preview_)) { preview_ = 0; touch(); }
  game_audio_->update(*world_, listener, dt);
  if (play_ == PlayState::editing) {
    audio::Listener l;
    l.position = listener.eye;
    l.forward = normalize(listener.target - listener.eye, {1, 0, 0});
    l.up = listener.up;
    mixer_->set_listener(l);
  }
}

bool Studio::init(Renderer* renderer, game::NightWindows* windows, bool player_only) {
  if (!renderer) return fail("Studio needs a renderer");
  r_ = renderer;
  windows_ = windows;
  player_only_ = player_only;
  world_ = std::make_unique<game::GameWorld>(*r_, windows_);
  world_->set_layers(layer::scene);
  ready_ = true;
  if (!player_only) {
    load_level(game::orb_run_level());
    report("Kern Studio ready - Orb Run loaded. Press Play (Alt+P) to drive, or fly around with the right mouse button + WASD.");
  }
  return true;
}

void Studio::report(const std::string& text, const char* level) {
  log_.push_back({level, text, now_});
  if (log_.size() > 500) log_.erase(log_.begin(), log_.begin() + 100);
  status_text_ = text;
  touch();
}

void Studio::push_ui(json::Value action) { ui_.push_back({std::move(action)}); touch(); }

json::Value Studio::take_ui() {
  json::Value list = json::Value::array();
  for (UiAction& a : ui_) list.push(std::move(a.data));
  ui_.clear();
  return list;
}

// ---------------------------------------------------------------------------
// document and history

bool Studio::load_level(game::Level level, bool reset_history) {
  if (play_ != PlayState::editing) stop();
  end_gizmo(false);
  level_ = std::move(level);
  selection_.clear();
  if (reset_history) {
    history_.clear();
    HistoryEntry e;
    e.label = "Open " + level_.name;
    e.level = level_;
    e.serial = next_serial_++;
    history_.push_back(std::move(e));
    history_index_ = 0;
    saved_serial_ = history_[0].serial;
    save_due_ = -1;
  }
  // Viewport cameras from the level.
  ViewCamera& c = views_[0].camera;
  c.position = level_.camera.position;
  c.yaw = level_.camera.yaw;
  c.pitch = level_.camera.pitch;
  for (u32 i = 1; i < 4; ++i) {
    views_[i].camera.ortho_center = {level_.camera.position.x, level_.camera.position.y, 0};
    views_[i].camera.ortho_height = 60;
  }
  changed();
  return true;
}

bool Studio::load_level_text(std::string_view text, std::string_view name) {
  game::Level l;
  std::string error;
  if (!game::from_text(text, l, &error)) {
    report(std::string(name) + ": " + error, "error");
    return fail("%s", error.c_str());
  }
  load_level(std::move(l));
  for (const game::Issue& i : game::validate(level_)) report(i.text, i.error ? "error" : "warn");
  report("Opened " + level_.name);
  return true;
}

std::string Studio::level_text() const {
  game::Level copy = level_;
  const ViewCamera& c = views_[0].camera;
  copy.camera.position = c.position;
  copy.camera.yaw = c.yaw;
  copy.camera.pitch = c.pitch;
  return game::to_text(copy, 2);
}

namespace {

/// Document equality for the history: everything that is saved except the
/// editor camera (camera moves are not undo steps).
bool same_document(const game::Level& a, const game::Level& b) {
  if (a.name != b.name || a.next_id != b.next_id || a.actors.size() != b.actors.size() || a.textures.size() != b.textures.size() ||
      a.sounds.size() != b.sounds.size())
    return false;
  for (std::size_t i = 0; i < a.sounds.size(); ++i) {
    const game::SoundAsset &x = a.sounds[i], &y = b.sounds[i];
    if (x.id != y.id || x.name != y.name || x.duration != y.duration) return false;
    if (x.src != y.src && (!x.src || !y.src || *x.src != *y.src)) return false;
  }
  if (json::stringify(game::world_to_json(a.world)) != json::stringify(game::world_to_json(b.world))) return false;
  if (!(a.logic == b.logic)) return false;
  for (std::size_t i = 0; i < a.textures.size(); ++i) {
    const game::TextureAsset &x = a.textures[i], &y = b.textures[i];
    if (x.id != y.id || x.name != y.name || x.width != y.width || x.height != y.height) return false;
    if (x.src != y.src && (!x.src || !y.src || *x.src != *y.src)) return false;
  }
  for (std::size_t i = 0; i < a.actors.size(); ++i)
    if (json::stringify(game::actor_to_json(a.actors[i])) != json::stringify(game::actor_to_json(b.actors[i]))) return false;
  return true;
}

}  // namespace

bool Studio::commit(const char* label, game::Level before, std::vector<ActorId> selection_before) {
  const std::string key = std::move(coalesce_), transaction = std::move(transaction_);
  coalesce_.clear();
  transaction_.clear();
  if (same_document(before, level_)) {  // nothing to undo (the selection may still have changed)
    changed();
    return false;
  }
  const bool merge = !key.empty() && !transaction.empty() && history_index_ > 0 && history_index_ + 1 == history_.size() &&
                     history_.back().merge_key == key && history_.back().transaction == transaction;
  if (merge) {
    HistoryEntry& e = history_.back();
    e.level = level_;
    e.selection_after = selection_;
    e.serial = next_serial_++;
    // A continuous edit that ends where it started is no step at all.
    if (same_document(history_[history_index_ - 1].level, level_)) {
      history_.pop_back();
      --history_index_;
    }
  } else {
    history_.resize(history_index_ + 1);  // a new edit discards the redo branch
    HistoryEntry e;
    e.label = label;
    e.level = level_;
    e.selection_before = std::move(selection_before);
    e.selection_after = selection_;
    e.merge_key = key;
    e.transaction = transaction;
    e.serial = next_serial_++;
    history_.push_back(std::move(e));
    // The oldest state becomes the new base: every remaining step still undoes exactly.
    while (history_.size() > kMaxHistory) history_.erase(history_.begin());
    history_index_ = history_.size() - 1;
  }
  save_due_ = now_ + prefs_.autosave_seconds * 1000;
  changed();
  return true;
}

void Studio::changed() {
  // Drop selected ids that no longer exist.
  selection_.erase(std::remove_if(selection_.begin(), selection_.end(), [&](ActorId id) { return !level_.find(id); }), selection_.end());
  ++scene_revision_;
  touch();
}

void Studio::step_history(bool forward) {
  const game::EditorCamera camera = level_.camera;  // the viewport camera is not part of the history
  if (forward) {
    ++history_index_;
    level_ = history_[history_index_].level;
    selection_ = history_[history_index_].selection_after;
  } else {
    selection_ = history_[history_index_].selection_before;
    --history_index_;
    level_ = history_[history_index_].level;
  }
  level_.camera = camera;
  save_due_ = now_ + prefs_.autosave_seconds * 1000;
  changed();
}

void Studio::undo() {
  if (play_ != PlayState::editing) { report("Stop playing to undo", "warn"); return; }
  end_gizmo(false);
  if (!can_undo()) return;
  report("Undo " + history_[history_index_].label);
  step_history(false);
}

void Studio::redo() {
  if (play_ != PlayState::editing) { report("Stop playing to redo", "warn"); return; }
  end_gizmo(false);
  if (!can_redo()) return;
  step_history(true);
  report("Redo " + history_[history_index_].label);
}

bool Studio::jump_history(std::size_t index) {
  if (play_ != PlayState::editing || index >= history_.size()) return false;
  end_gizmo(false);
  while (history_index_ > index) step_history(false);
  while (history_index_ < index) step_history(true);
  report("History: " + history_[history_index_].label);
  return true;
}

void Studio::mark_saved() {
  if (!history_.empty()) saved_serial_ = history_[history_index_].serial;
  save_due_ = -1;
  touch();
}

std::string Studio::take_autosave(f64 now_ms) {
  if (save_due_ < 0 || now_ms < save_due_ || play_ != PlayState::editing) return {};
  save_due_ = -1;
  return level_text();
}

// ---------------------------------------------------------------------------
// selection

bool Studio::selected(ActorId id) const { return std::find(selection_.begin(), selection_.end(), id) != selection_.end(); }

void Studio::select(const std::vector<ActorId>& ids, bool add, bool toggle) {
  if (!add && !toggle) selection_.clear();
  for (ActorId id : ids) {
    if (!level_.find(id)) continue;
    auto it = std::find(selection_.begin(), selection_.end(), id);
    if (toggle && it != selection_.end()) selection_.erase(it);
    else if (it == selection_.end()) selection_.push_back(id);
    else if (add) { selection_.erase(it); selection_.push_back(id); }  // becomes primary
  }
  ++scene_revision_;
  touch();
}

void Studio::select_all() {
  std::vector<ActorId> ids;
  for (const game::Actor& a : level_.actors) if (!a.locked) ids.push_back(a.id);
  select(ids);
}

void Studio::deselect() {
  selection_.clear();
  ++scene_revision_;
  touch();
}

std::vector<ActorId> Studio::selection_roots() const {
  std::vector<ActorId> out;
  for (ActorId id : selection_) {
    bool ancestor_selected = false;
    for (const game::Actor* p = level_.find(level_.find(id) ? level_.find(id)->parent : 0); p && !ancestor_selected; p = level_.find(p->parent))
      ancestor_selected = selected(p->id);
    if (!ancestor_selected) out.push_back(id);
  }
  return out;
}

// ---------------------------------------------------------------------------
// actors

namespace {

std::string unique_name(const game::Level& l, std::string base) {
  auto taken = [&](const std::string& n) {
    for (const game::Actor& a : l.actors) if (a.name == n) return true;
    return false;
  };
  if (!taken(base)) return base;
  // "Cube 3" -> "Cube", then count up.
  std::size_t sp = base.find_last_of(' ');
  if (sp != std::string::npos && sp + 1 < base.size() && std::all_of(base.begin() + static_cast<std::ptrdiff_t>(sp) + 1, base.end(), ::isdigit)) base.resize(sp);
  for (int i = 2; i < 100000; ++i) {
    const std::string n = base + " " + std::to_string(i);
    if (!taken(n)) return n;
  }
  return base;
}

/// Copies actors (and the subtree order) into `out`, remapping ids so they are new.
void clone_into(game::Level& level, const std::vector<game::Actor>& source, std::vector<ActorId>& roots, Vec3 offset) {
  std::unordered_map<ActorId, ActorId> map;
  for (const game::Actor& a : source) map[a.id] = level.next_id++;
  for (game::Actor a : source) {
    const ActorId old_parent = a.parent;
    a.id = map[a.id];
    a.name = unique_name(level, a.name);
    if (map.count(old_parent)) a.parent = map[old_parent];
    else {
      if (!level.find(old_parent)) a.parent = 0;
      a.transform.position = a.transform.position + offset;
      roots.push_back(a.id);
    }
    for (game::JointComponent& j : a.joints)
      if (map.count(j.target)) j.target = map[j.target];
    level.actors.push_back(std::move(a));
  }
}

}  // namespace

ActorId Studio::add_actor(game::Actor actor, bool select_it) {
  ActorId id = 0;
  edit("Add actor", [&](game::Level& l) {
    actor.name = unique_name(l, actor.name.empty() ? std::string("Actor") : actor.name);
    actor.id = 0;
    id = l.add(std::move(actor)).id;
    if (select_it) select({id});
    return true;
  });
  return id;
}

void Studio::delete_selection() { delete_selection("Delete"); }

void Studio::delete_selection(const char* label) {
  if (selection_.empty()) return;
  std::unordered_set<ActorId> doomed(selection_.begin(), selection_.end());
  // Children go with their parents.
  bool grew = true;
  while (grew) {
    grew = false;
    for (const game::Actor& a : level_.actors)
      if (a.parent && doomed.count(a.parent) && !doomed.count(a.id)) { doomed.insert(a.id); grew = true; }
  }
  const std::size_t n = doomed.size();
  edit(label, [&](game::Level& l) {
    l.actors.erase(std::remove_if(l.actors.begin(), l.actors.end(), [&](const game::Actor& a) { return doomed.count(a.id) > 0; }), l.actors.end());
    for (game::Actor& a : l.actors)
      for (game::JointComponent& j : a.joints)
        if (doomed.count(j.target)) j.target = 0;
    selection_.clear();
    return true;
  });
  report("Deleted " + std::to_string(n) + (n == 1 ? " actor" : " actors"));
}

namespace {
/// Actors of the given roots with their whole subtrees, in document order.
std::vector<game::Actor> subtree_copy(const game::Level& l, const std::vector<ActorId>& roots) {
  std::unordered_set<ActorId> take(roots.begin(), roots.end());
  bool grew = true;
  while (grew) {
    grew = false;
    for (const game::Actor& a : l.actors)
      if (a.parent && take.count(a.parent) && !take.count(a.id)) { take.insert(a.id); grew = true; }
  }
  std::vector<game::Actor> out;
  for (const game::Actor& a : l.actors) if (take.count(a.id)) out.push_back(a);
  return out;
}
}  // namespace

void Studio::duplicate_selection(Vec3 offset) {
  const std::vector<ActorId> roots = selection_roots();
  if (roots.empty()) return;
  const std::vector<game::Actor> copy = subtree_copy(level_, roots);
  std::vector<ActorId> fresh;
  edit("Duplicate", [&](game::Level& l) { clone_into(l, copy, fresh, offset); select(fresh); return true; });
}

void Studio::duplicate_in_place() {
  const std::vector<ActorId> roots = selection_roots();
  if (roots.empty()) return;
  const std::vector<game::Actor> copy = subtree_copy(level_, roots);
  std::vector<ActorId> fresh;
  clone_into(level_, copy, fresh, {});
  select(fresh);
  changed();
  world_->sync(level_);  // the gizmo reads the copies' placement right away
}

void Studio::copy_selection() {
  clipboard_ = subtree_copy(level_, selection_roots());
  json::Value doc = json::Value::object();
  doc.set("format", "kern-actors");
  json::Value list = json::Value::array();
  for (const game::Actor& a : clipboard_) list.push(game::actor_to_json(a));
  doc.set("actors", std::move(list));
  json::Value a = json::Value::object();
  a.set("action", "clipboard");
  a.set("text", json::stringify(doc));
  push_ui(std::move(a));
  report("Copied " + std::to_string(clipboard_.size()) + " actors");
}

bool Studio::paste() {
  if (clipboard_.empty()) return false;
  std::vector<ActorId> fresh;
  edit("Paste", [&](game::Level& l) { clone_into(l, clipboard_, fresh, {1, 1, 0}); select(fresh); return true; });
  return true;
}

void Studio::cut_selection() {
  copy_selection();
  delete_selection("Cut");
}

void Studio::reparent(game::Level& l, const std::vector<ActorId>& children, ActorId parent) {
  const Mat4 parent_inv = parent ? internal::inverse(l.world_matrix(parent)) : Mat4{};
  for (ActorId c : children) {
    game::Actor* a = l.find(c);
    if (!a || a->parent == parent) continue;
    // Keep the world placement.
    const Mat4 world = l.world_matrix(c);
    a->parent = parent;
    a->transform = game::matrix_transform(parent_inv * world);
    if (parent) a->folder.clear();
  }
}

bool Studio::attach(const std::vector<ActorId>& children, ActorId parent) {
  if (parent && !level_.find(parent)) return false;
  for (ActorId c : children)
    if (c == parent || (parent && level_.is_ancestor(c, parent))) { report("Cannot attach an actor to its own child", "warn"); return false; }
  return edit(parent ? "Attach" : "Detach", [&](game::Level& l) { reparent(l, children, parent); return true; });
}

bool Studio::rename(ActorId id, const std::string& name) {
  if (!level_.find(id) || name.empty()) return false;
  return edit("Rename", [&](game::Level& l) { l.find(id)->name = name; return true; });
}

void Studio::group_selection() {
  const std::vector<ActorId> roots = selection_roots();
  if (roots.empty()) return;
  Aabb box;
  for (ActorId id : roots) box.extend(level_.world_matrix(id).translation());
  game::Actor group;
  group.name = "Group";
  group.transform.position = box.center();
  ActorId gid = 0;
  edit("Group", [&](game::Level& l) {
    group.name = unique_name(l, "Group");
    gid = l.add(group).id;
    const Mat4 inv = internal::inverse(l.world_matrix(gid));
    for (ActorId id : roots) {
      game::Actor* a = l.find(id);
      const Mat4 world = l.world_matrix(id);
      a->parent = gid;
      a->transform = game::matrix_transform(inv * world);
    }
    select({gid});
    return true;
  });
}

void Studio::snap_to_floor() {
  const std::vector<ActorId> roots = selection_roots();
  if (roots.empty()) return;
  edit("Snap to floor", [&](game::Level& l) {
    for (ActorId id : roots) {
      game::Actor* a = l.find(id);
      Aabb b;
      if (!world_->actor_bounds(id, b)) continue;
      const Mat4 world = l.world_matrix(id);
      const Vec3 origin = world.translation();
      // Cast down from the bottom of the bounds through the scene meshes (not the actor itself).
      Ray r;
      r.origin = {origin.x, origin.y, b.min.z - 0.001f};
      r.direction = {0, 0, -1};
      r.near = 0;
      r.far = 10000;
      PickQuery q;
      q.layers = layer::scene;
      std::vector<u32> own;
      world_->actor_meshes(id, own);
      PickHit hit;
      f32 floor_z = 0;
      bool found = false;
      // Skip hits on the actor's own meshes by re-casting below them.
      for (int attempt = 0; attempt < 8; ++attempt) {
        if (!r_->pick_ray(r, q, hit)) break;
        if (std::find(own.begin(), own.end(), hit.mesh) == own.end()) { floor_z = hit.point.z; found = true; break; }
        r.origin = hit.point - Vec3{0, 0, 0.001f};
      }
      if (!found) floor_z = level_.world.ground ? 0.0f : b.min.z;
      const f32 dz = floor_z - b.min.z;
      Mat4 moved = Mat4::translate({0, 0, dz}) * world;
      const Mat4 parent_inv = a->parent ? internal::inverse(l.world_matrix(a->parent)) : Mat4{};
      a->transform.position = (parent_inv * moved).translation();
    }
    return true;
  });
}

void Studio::set_hidden(const std::vector<ActorId>& ids, bool hidden) {
  edit(hidden ? "Hide" : "Show", [&](game::Level& l) {
    for (ActorId id : ids) if (game::Actor* a = l.find(id)) a->hidden = hidden;
    return true;
  });
}

void Studio::keep_simulation_changes() {
  if (play_ == PlayState::editing || selection_.empty()) { report("Keep Simulation Changes works on selected actors while simulating", "warn"); return; }
  std::vector<std::pair<ActorId, Mat4>> poses;
  for (ActorId id : selection_roots()) {
    Mat4 m;
    if (world_->actor_matrix(id, m)) poses.push_back({id, m});
  }
  // Applied to the edited level after the simulation stops (undoable).
  stop();
  edit("Keep simulation changes", [&](game::Level& l) {
    for (auto& [id, m] : poses) {
      game::Actor* a = l.find(id);
      if (!a) continue;
      const Mat4 parent_inv = a->parent ? internal::inverse(l.world_matrix(a->parent)) : Mat4{};
      a->transform = game::matrix_transform(parent_inv * m);
    }
    return true;
  });
  report("Kept the simulated placement of " + std::to_string(poses.size()) + " actors");
}

// ---------------------------------------------------------------------------
// play

game::GameWorld& Studio::world_of(u32 viewport) {
  if (viewport >= 1 && viewport - 1 < clients_.size()) return *clients_[viewport - 1].world;
  return *world_;
}

void Studio::start_local_clients() {
  clients_.clear();
  for (i32 i = 1; i < clamp(net_play_.players, 1, 4); ++i) {
    LocalClient c;
    c.world = std::make_unique<game::GameWorld>(*r_, windows_);
    c.world->set_layers(layer::client0 << (i - 1));
    c.transport = hub_->create();
    c.game = std::make_unique<game::NetGame>(*c.world);
    c.game->join(level_, *c.transport, hub_server_->address(), {}, 1000 + static_cast<u64>(i));
    clients_.push_back(std::move(c));
  }
}

void Studio::stop_net() {
  for (LocalClient& c : clients_) {
    if (c.game) c.game->stop();
    if (c.world) { c.world->stop(); c.world->clear(); }
  }
  clients_.clear();
  if (net_) net_->stop();
  net_.reset();
  hub_server_.reset();
  hub_.reset();
  net_remote_ = false;
}

bool Studio::net_host() {
  if (play_ != PlayState::editing) stop();
  net_remote_ = true;
  net_play_.players = 1;
  return play(game::SimMode::play);
}

bool Studio::net_join(u64 server) {
  if (play_ != PlayState::editing) stop();
  if (!server) { report("No server to join", "error"); return false; }
  stop_net();
  net_ = std::make_unique<game::NetGame>(*world_);
  net_remote_ = true;
  net_->join(level_, host_transport_, net::Address{server}, {}, static_cast<u64>(now_ * 1000.0) | 1);
  play_ = PlayState::playing;
  paused_ = false;
  ejected_ = false;
  capture_play_pointer();
  report("Joining the game...");
  ++scene_revision_;
  return true;
}

void Studio::net_stop() { stop(); }

void Studio::update_net(f32 dt, const game::PlayInput& in) {
  game::PlayInput idle;
  // While in-process players are still joining, exchange packets several times per
  // frame (the time step is split), so the handshake does not wait for rendered frames.
  bool joining = false;
  for (const LocalClient& c : clients_) joining |= !c.game->playing();
  const int passes = joining && hub_ ? 4 : 1;
  for (int pass = 0; pass < passes; ++pass) {
    const f32 h = dt / static_cast<f32>(passes);
    net_time_ += h;
    if (hub_) {
      hub_->simulator().settings = net_play_.network;
      hub_->advance_time(net_time_);
    }
    net_->update(net_time_, h, active_view_ == 0 || clients_.empty() ? in : idle);
    for (std::size_t i = 0; i < clients_.size(); ++i)
      clients_[i].game->update(net_time_, h, active_view_ == i + 1 ? in : idle);
  }
  for (std::size_t i = 0; i < clients_.size(); ++i) {
    LocalClient& c = clients_[i];
    const bool active = active_view_ == i + 1;
    const game::PlayInput& cin = active ? in : idle;
    if (c.game->playing()) {
      Camera cam = c.camera_valid ? c.camera : camera_of(views_[i + 1]);
      c.camera_valid = c.world->update_camera(cam, dt, cin);
      c.camera = cam;
    }
    c.world->present(now_, c.camera.eye);
  }
}

void Studio::capture_play_pointer() {
  // Touch devices never lock the pointer (SpawnCity: requestPointerLock only on desktop).
  if (input_.touch()) { mouse_captured_ = false; return; }
  mouse_captured_ = true;
  json::Value a = json::Value::object();
  a.set("action", "capturePointer");
  a.set("viewport", active_view_);
  push_ui(std::move(a));
}

void Studio::update_touch_controls(f32 dt) {
  touch_ctl_in_ = game::PlayInput{};
  const bool on = play_ == PlayState::playing && !ejected_ && input_.touch() && !paused_;
  const game::Actor* a = on ? world_->level().find(world_->player()) : nullptr;
  const game::ControllerComponent* c = a && a->controller ? &*a->controller : nullptr;
  const Viewport& v = views_[active_view_];
  touch_ctl_.set_frame(v.css_width, v.css_height, safe_[0], safe_[1], safe_[2], safe_[3]);
  const game::Hud& h = world_->hud();
  touch_ctl_.set_controller(c, c && a->weapon.has_value(), !h.soldier || h.health != 0.0f);
  if (!c) { touch_ctl_.release_all(); return; }
  const f32 fov = play_camera_valid_ ? play_camera_.fov * 57.29578f : c->camera_fov;
  touch_ctl_.update(dt, touch_ctl_in_, fov);
}

bool Studio::soldier_pawn() const {
  if (!world_->running()) return false;
  const game::Actor* a = world_->level().find(world_->player());
  return a && a->controller && a->controller->mode == game::ControlMode::soldier;
}

bool Studio::play(game::SimMode mode) {
  play_buttons_ = 0;
  if (play_ != PlayState::editing) {
    // Simulate <-> Play while running.
    if (!world_->set_mode(mode)) { report(last_error(), "warn"); return false; }
    play_ = mode == game::SimMode::play ? PlayState::playing : PlayState::simulating;
    ejected_ = false;
    mouse_captured_ = play_ == PlayState::playing && !input_.touch();
    touch();
    return true;
  }
  end_gizmo(false);
  for (auto& v : views_) end_drag(v);
  bool errors = false;
  for (const game::Issue& i : game::validate(level_)) {
    report(i.text, i.error ? "error" : "warn");
    errors |= i.error;
  }
  if (errors) { report("Fix the errors above to play", "error"); return false; }
  if (mode == game::SimMode::play) {
    bool player = false;
    for (const game::Actor& a : level_.actors) player |= a.controller.has_value();
    if (!player) { report("Nothing to control yet: drop a player from Content > Players (Soldier, Hover Car...) - simulating instead", "warn"); mode = game::SimMode::simulate; }
  }
  const bool networked = mode == game::SimMode::play && (net_remote_ || net_play_.players > 1);
  if (networked) {
    // Listen server: this world hosts; local players join through the in-process network.
    const bool remote = net_remote_;
    stop_net();
    net_ = std::make_unique<game::NetGame>(*world_);
    bool ok = false;
    if (remote) {
      net_remote_ = true;
      ok = net_->host(level_, host_transport_, {}, static_cast<u64>(now_ * 1000.0) | 1);
    } else {
      hub_ = std::make_unique<net::LoopbackNetwork>(7);
      hub_->simulator().settings = net_play_.network;
      hub_server_ = hub_->create();
      ok = net_->host(level_, *hub_server_);
    }
    if (!ok) { stop_net(); report(std::string("Could not start: ") + last_error(), "error"); return false; }
    net_time_ = 0;
    if (!remote) {
      start_local_clients();
      if (prefs_.layout == Layout::single) prefs_.layout = Layout::quad;
    }
  } else if (!world_->start(level_, mode)) { report(std::string("Could not start: ") + last_error(), "error"); return false; }
  for (const std::string& e : world_->events()) report(e, "warn");
  world_->events().clear();
  play_ = mode == game::SimMode::play ? PlayState::playing : PlayState::simulating;
  paused_ = false;
  ejected_ = false;  // true only after F8 (Eject) from Play
  mouse_captured_ = false;
  if (play_ == PlayState::playing) capture_play_pointer();
  touch_ctl_.release_all();
  if (play_ != PlayState::playing) report("Simulating - physics runs, Ctrl+drag grabs bodies, K keeps the selected actors' poses");
  else if (input_.touch()) report("Playing - left stick moves, right stick or a drag looks around, the buttons act");
  else report("Playing - WASD / arrows to move, mouse to look, Shift+F1 frees the mouse, F8 ejects, Esc stops");
  ++scene_revision_;
  return true;
}

void Studio::stop() {
  if (play_ == PlayState::editing) return;
  stop_net();
  world_->stop();
  play_ = PlayState::editing;
  paused_ = false;
  ejected_ = false;
  mouse_captured_ = false;
  keys_.clear();
  json::Value a = json::Value::object();
  a.set("action", "releasePointer");
  push_ui(std::move(a));
  report("Stopped - back to editing");
  ++scene_revision_;
  synced_revision_ = ~0ull;
  world_synced_ = ~0ull;
}

void Studio::toggle_pause() {
  if (play_ == PlayState::editing) return;
  paused_ = !paused_;
  world_->set_paused(paused_);
  report(paused_ ? "Paused" : "Resumed");
}

void Studio::eject() {
  if (play_ == PlayState::editing) return;
  if (play_ == PlayState::playing) {
    // Detach from the pawn: the simulation keeps running, the camera is free.
    world_->set_mode(game::SimMode::simulate);
    play_ = PlayState::simulating;
    ejected_ = true;
    mouse_captured_ = false;
    json::Value a = json::Value::object();
    a.set("action", "releasePointer");
    push_ui(std::move(a));
    report("Ejected - free camera, F8 possesses the player again");
  } else if (world_->set_mode(game::SimMode::play)) {
    play_ = PlayState::playing;
    ejected_ = false;
    mouse_captured_ = !input_.touch();
    report("Possessed the player");
  } else {
    report("No player to possess", "warn");
  }
  touch();
}

void Studio::set_tool(Tool t) {
  tool_ = t;
  touch();
}

void Studio::set_layout(Layout l) {
  prefs_.layout = l;
  touch();
}

void Studio::set_view_type(u32 i, ViewType t) {
  Viewport& v = viewport(i);
  if (v.type == t) return;
  if (v.type == ViewType::perspective && t != ViewType::perspective) {
    // Keep looking at the same spot.
    v.camera.ortho_center = v.camera.position + v.camera.forward() * 20;
  }
  v.type = t;
  if (t != ViewType::perspective && v.mode == ViewMode::lit) v.mode = ViewMode::wireframe;
  if (t == ViewType::perspective && v.mode == ViewMode::wireframe) v.mode = ViewMode::lit;
  v.grid_key = 0;
  touch();
}

std::vector<std::string> Studio::pending_textures() { return world_ ? world_->pending_textures() : std::vector<std::string>{}; }
bool Studio::texture_pixels(const std::string& id, i32 w, i32 h, const u8* rgba) { return world_->set_texture_pixels(id, w, h, rgba); }
std::string Studio::texture_source(const std::string& id) const {
  const game::TextureAsset* t = level_.texture(id);
  return t && t->src ? *t->src : std::string();
}

}  // namespace kern::studio
