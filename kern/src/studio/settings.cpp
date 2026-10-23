// Kern Studio user settings: what the host stores between sessions (docs/studio.md, "Einstellungen").
//
// All settings form one flat object {key: value}. The host stores only the user's own choices
// (keys whose value the user changed, or that came from the store): a setting the user never
// touched is not frozen into storage, so a new default (Constraints off, say) reaches everyone
// who kept the old one without choosing it.

#include <algorithm>
#include <cstring>
#include <string>
#include <type_traits>

#include "internal.h"
#include "kern/studio/studio.h"

namespace kern::studio {

namespace {

const char* const kTypeIds[] = {"perspective", "top", "bottom", "left", "right", "front", "back"};
const char* const kModeIds[] = {"lit", "unlit", "wireframe", "collision"};

struct FlagName {
  const char* id;
  bool ShowFlags::*member;
};
const FlagName kFlags[] = {{"grid", &ShowFlags::grid},     {"icons", &ShowFlags::icons},   {"selection", &ShowFlags::selection},
                           {"colliders", &ShowFlags::colliders}, {"joints", &ShowFlags::joints}, {"contacts", &ShowFlags::contacts},
                           {"bounds", &ShowFlags::bounds}, {"fog", &ShowFlags::fog},       {"sky", &ShowFlags::sky},
                           {"stats", &ShowFlags::stats},   {"robotCamera", &ShowFlags::robot_camera}};

const char* layout_id(Layout l) { return l == Layout::single ? "single" : l == Layout::quad ? "quad" : "twoColumns"; }

json::Value collect(const Preferences& p, const Snap& s, Space space, bool muted, const std::array<Viewport, 4>& views) {
  json::Value o = json::Value::object();
  o.set("cameraSpeed", p.camera_speed);
  o.set("mouseSensitivity", static_cast<f64>(p.mouse_sensitivity));
  o.set("invertY", p.invert_y);
  o.set("orbitAroundSelection", p.orbit_around_selection);
  o.set("layout", layout_id(p.layout));
  o.set("snap.grid", s.grid);
  o.set("snap.gridSize", static_cast<f64>(s.grid_size));
  o.set("snap.rotation", s.rotation);
  o.set("snap.rotationStep", static_cast<f64>(s.rotation_step));
  o.set("snap.scale", s.scale);
  o.set("snap.scaleStep", static_cast<f64>(s.scale_step));
  o.set("space", space == Space::world ? "world" : "local");
  o.set("muted", muted);
  for (const Viewport& v : views) {
    const std::string k = "viewport" + std::to_string(v.index) + ".";
    o.set(k + "type", kTypeIds[static_cast<u32>(v.type)]);
    o.set(k + "mode", kModeIds[static_cast<u32>(v.mode)]);
    o.set(k + "fov", static_cast<f64>(v.camera.fov));
    for (const FlagName& f : kFlags) o.set(k + "show." + f.id, v.flags.*f.member);
  }
  return o;
}

std::array<Viewport, 4> default_views() {
  std::array<Viewport, 4> v;
  for (u32 i = 0; i < 4; ++i) internal::default_view_settings(v[i], i);
  return v;
}

}  // namespace

void internal::default_view_settings(Viewport& v, u32 index) {
  v.index = index;
  // Quad layout defaults like Unreal: perspective, top, front, side.
  static const ViewType types[] = {ViewType::perspective, ViewType::top, ViewType::front, ViewType::right};
  v.type = types[index & 3];
  v.mode = index == 0 ? ViewMode::lit : ViewMode::wireframe;
  v.flags = ShowFlags{};
  if (index != 0) v.flags.fog = false;
  v.camera.fov = ViewCamera{}.fov;
  v.grid_key = 0;
}

json::Value Studio::settings_json() const { return collect(prefs_, snap_, space_, muted(), views_); }

json::Value Studio::default_settings_json() { return collect(Preferences{}, Snap{}, Space::world, false, default_views()); }

u32 Studio::apply_settings(const json::Value& values) {
  if (!values.is_object()) return 0;
  u32 applied = 0;
  std::vector<std::string> keys;
  // One setting; true when the key is known and the value valid.
  auto apply = [&](const std::string& k, const json::Value& v) -> bool {
    auto number = [&](f64 lo, f64 hi, auto& out) {
      if (!v.is_number()) return false;
      out = static_cast<std::remove_reference_t<decltype(out)>>(clamp(v.as_number(), lo, hi));
      return true;
    };
    auto boolean = [&](bool& out) {
      if (!v.is_bool()) return false;
      out = v.as_bool();
      return true;
    };
    if (k == "cameraSpeed") return number(1, 8, prefs_.camera_speed);
    if (k == "mouseSensitivity") return number(0.1, 5, prefs_.mouse_sensitivity);
    if (k == "invertY") return boolean(prefs_.invert_y);
    if (k == "orbitAroundSelection") return boolean(prefs_.orbit_around_selection);
    if (k == "layout") {
      const std::string& s = v.as_string();
      if (s != "single" && s != "quad" && s != "twoColumns") return false;
      prefs_.layout = s == "quad" ? Layout::quad : s == "twoColumns" ? Layout::two_columns : Layout::single;
      return true;
    }
    if (k == "snap.grid") return boolean(snap_.grid);
    if (k == "snap.gridSize") return number(0.001, 1000, snap_.grid_size);
    if (k == "snap.rotation") return boolean(snap_.rotation);
    if (k == "snap.rotationStep") return number(0.1, 180, snap_.rotation_step);
    if (k == "snap.scale") return boolean(snap_.scale);
    if (k == "snap.scaleStep") return number(0.001, 100, snap_.scale_step);
    if (k == "space") {
      if (v.as_string() != "world" && v.as_string() != "local") return false;
      space_ = v.as_string() == "world" ? Space::world : Space::local;
      return true;
    }
    if (k == "muted") {
      if (!v.is_bool()) return false;
      set_muted(v.as_bool());
      return true;
    }
    if (k.rfind("viewport", 0) == 0 && k.size() > 10 && k[8] >= '0' && k[8] <= '3' && k[9] == '.') {
      Viewport& view = views_[static_cast<u32>(k[8] - '0')];
      const std::string rest = k.substr(10);
      if (rest == "type") {
        for (u32 i = 0; i < std::size(kTypeIds); ++i)
          if (v.as_string() == kTypeIds[i]) { view.type = static_cast<ViewType>(i); view.grid_key = 0; return true; }
        return false;
      }
      if (rest == "mode") {
        for (u32 i = 0; i < std::size(kModeIds); ++i)
          if (v.as_string() == kModeIds[i]) { view.mode = static_cast<ViewMode>(i); return true; }
        return false;
      }
      if (rest == "fov") return number(10, 150, view.camera.fov);
      if (rest.rfind("show.", 0) == 0) {
        for (const FlagName& f : kFlags)
          if (rest.compare(5, std::string::npos, f.id) == 0) return boolean(view.flags.*f.member);
      }
    }
    return false;
  };
  for (const json::Value::Member& m : values.members())
    if (apply(m.first, m.second)) {
      ++applied;
      keys.push_back(m.first);
    }
  for (const std::string& k : keys)
    if (!std::binary_search(settings_chosen_.begin(), settings_chosen_.end(), k))
      settings_chosen_.insert(std::upper_bound(settings_chosen_.begin(), settings_chosen_.end(), k), k);
  for (Viewport& v : views_) v.grid_key = 0;
  ++scene_revision_;
  settings_seen_ = settings_json();
  touch();
  return applied;
}

void Studio::reset_settings() {
  const f32 autosave = prefs_.autosave_seconds;
  prefs_ = Preferences{};
  prefs_.autosave_seconds = autosave;
  snap_ = Snap{};
  space_ = Space::world;
  set_muted(false);
  for (u32 i = 0; i < 4; ++i) internal::default_view_settings(views_[i], i);
  settings_chosen_.clear();
  settings_seen_ = settings_json();
  settings_due_ = true;  // the host drops what it stored
  ++scene_revision_;
  touch();
}

void Studio::note_settings() {
  json::Value now = settings_json();
  if (!settings_seen_.is_object()) {
    settings_seen_ = std::move(now);
    return;
  }
  for (const json::Value::Member& m : now.members()) {
    if (settings_seen_[m.first] == m.second) continue;
    const bool chosen = std::binary_search(settings_chosen_.begin(), settings_chosen_.end(), m.first);
    // Multi-player Play switches to four viewports by itself: not the user's choice.
    if (!chosen && m.first == "layout" && play_ != PlayState::editing) continue;
    if (!chosen) settings_chosen_.insert(std::upper_bound(settings_chosen_.begin(), settings_chosen_.end(), m.first), m.first);
    settings_due_ = true;
  }
  settings_seen_ = std::move(now);
}

json::Value Studio::take_settings() {
  note_settings();
  if (!settings_due_) return json::Value();
  settings_due_ = false;
  json::Value o = json::Value::object();
  for (const std::string& k : settings_chosen_) o.set(k, settings_seen_[k]);
  return o;
}

}  // namespace kern::studio
