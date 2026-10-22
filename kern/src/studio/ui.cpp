// Kern Studio protocol: keyboard shortcuts, the command dispatcher, and the
// JSON the page renders panels from (outliner, menus, dialogs, viewports,
// per-frame status).
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <sstream>

#include "internal.h"
#include "kern/game/demos.h"
#include "kern/studio/studio.h"

namespace kern::studio {

namespace {

enum Mods : u32 { shift = 1, ctrl = 2, alt = 4 };

json::Value ok(bool v = true) {
  json::Value r = json::Value::object();
  r.set("ok", v);
  return r;
}

json::Value error(const std::string& text) {
  json::Value r = ok(false);
  r.set("error", text);
  return r;
}

std::vector<ActorId> ids_of(const json::Value& v) {
  std::vector<ActorId> out;
  if (v.is_number()) out.push_back(static_cast<ActorId>(v.as_number()));
  for (const json::Value& x : v.items())
    if (x.is_number() && x.as_number() > 0) out.push_back(static_cast<ActorId>(x.as_number()));
  return out;
}

json::Value cmd(const char* name) {
  json::Value c = json::Value::object();
  c.set("cmd", name);
  return c;
}

template <typename T>
json::Value cmd(const char* name, const char* key, T value) {
  json::Value c = cmd(name);
  c.set(key, json::Value(value));
  return c;
}

/// A menu entry: label, the command it runs, shortcut text, enabled, checked.
json::Value entry(const std::string& label, json::Value command, const char* shortcut = "", bool enabled = true, int checked = -1) {
  json::Value e = json::Value::object();
  e.set("label", label);
  e.set("cmd", std::move(command));
  if (*shortcut) e.set("shortcut", shortcut);
  if (!enabled) e.set("disabled", true);
  if (checked >= 0) e.set("checked", checked == 1);
  return e;
}

json::Value separator() {
  json::Value e = json::Value::object();
  e.set("separator", true);
  return e;
}

json::Value submenu(const std::string& label, json::Value entries) {
  json::Value e = json::Value::object();
  e.set("label", label);
  e.set("submenu", std::move(entries));
  return e;
}

/// What the outliner calls an actor (Unreal's "Type" column) and its icon.
void actor_kind(const game::Actor& a, const char*& type, const char*& icon) {
  if (a.city) { type = "Procedural City"; icon = "city"; return; }
  if (a.controller) { type = "Player"; icon = "player"; return; }
  if (a.trigger) { type = "Trigger"; icon = "trigger"; return; }
  if (a.pickup) { type = "Pickup"; icon = "pickup"; return; }
  if (a.audio && !a.mesh) { type = "Audio Source"; icon = "speaker"; return; }
  if (!a.joints.empty() && a.mesh) { type = "Constrained Mesh"; icon = "joint"; return; }
  if (a.mesh) {
    if (a.body && a.body->motion == game::Motion::dynamic) { type = "Physics Mesh"; icon = "physics"; return; }
    type = "Static Mesh";
    icon = game::shape_name(a.mesh->shape);
    return;
  }
  if (a.light) { type = "Point Light"; icon = "light"; return; }
  if (!a.colliders.empty() || a.body) { type = "Physics Body"; icon = "physics"; return; }
  type = "Empty";
  icon = "empty";
}

std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

game::Level template_level(const std::string& name) {
  if (name == "orbRun") return game::orb_run_level();
  if (name == "physicsLab") return game::physics_lab_level();
  if (name == "shootingRange") return game::shooting_range_level();
  if (name == "empty") return game::empty_level();
  return game::basic_level();
}

}  // namespace

// ---------------------------------------------------------------------------
// keyboard

json::Value Studio::key(std::string_view key, std::string_view code, bool down, u32 mods, bool repeat) {
  json::Value result = json::Value::object();
  modifiers_ = mods;
  // Held keys use the physical position (WASD stays WASD on AZERTY / QWERTZ), shortcuts the
  // printed letter: Ctrl+Z is the key labelled Z on a German keyboard, as in every editor.
  if (down && !repeat && code != "Unidentified" && !code.empty()) {
    const bool was_touch = input_.touch();
    input_.key_down();
    if (was_touch) { touch_ctl_.release_all(); touch(); }
  }
  if (down) keys_.insert(std::string(code));
  else keys_.erase(std::string(code));
  std::string c(code);
  if (key.size() == 1 && std::isalpha(static_cast<unsigned char>(key[0])) && c.rfind("Key", 0) == 0) {
    const bool driving = (play_ == PlayState::playing && !ejected_) ||
                         std::any_of(views_.begin(), views_.end(), [](const Viewport& v) { return v.drag == Drag::fly || v.drag == Drag::pedestal; });
    if ((mods & ctrl) || !driving) c = std::string("Key") + static_cast<char>(std::toupper(static_cast<unsigned char>(key[0])));
  }
  bool handled = false;
  auto done = [&]() {
    result.set("handled", handled);
    return result;
  };
  if (!down) {
    // Movement keys while flying or playing are ours.
    handled = play_ == PlayState::playing || std::any_of(views_.begin(), views_.end(), [](const Viewport& v) { return v.drag == Drag::fly; });
    return done();
  }
  const bool m_shift = mods & shift, m_ctrl = mods & ctrl, m_alt = mods & alt;
  const bool flying = std::any_of(views_.begin(), views_.end(), [](const Viewport& v) {
    return v.drag == Drag::fly || v.drag == Drag::pedestal;
  });
  const bool playing_pawn = play_ == PlayState::playing && !ejected_;

  // ---- exported game (no editor) ----
  if (player_only_) {
    handled = true;
    if (c == "Escape") {
      mouse_captured_ = false;
      json::Value a = json::Value::object();
      a.set("action", "releasePointer");
      push_ui(std::move(a));
    } else if ((c == "KeyR" || c == "Enter") && !repeat && world_->running() &&
               (world_->hud().status == game::GameStatus::won || world_->hud().status == game::GameStatus::lost || c == "KeyR")) {
      if (c == "KeyR" && !m_ctrl && world_->hud().status == game::GameStatus::playing) return done();  // R is a game key while playing
      world_->restart();
    } else if (c == "KeyP" && !repeat) {
      toggle_pause();
    }
    return done();
  }

  // ---- playing / simulating ----
  if (play_ != PlayState::editing) {
    if (c == "Escape" && !repeat) {
      // Escape cancels a gizmo drag first, then stops.
      if (gizmo_.active) { end_gizmo(false); for (Viewport& v : views_) if (v.drag == Drag::gizmo) v.drag = Drag::none; }
      else stop();
      handled = true;
      return done();
    }
    if (c == "F8" && !repeat) { eject(); handled = true; return done(); }
    if (c == "F1" && m_shift) {
      mouse_captured_ = false;
      json::Value a = json::Value::object();
      a.set("action", "releasePointer");
      push_ui(std::move(a));
      report("Mouse released - click the viewport to control the game again");
      handled = true;
      return done();
    }
    if (c == "Pause" || (c == "KeyP" && m_alt && m_shift)) { toggle_pause(); handled = true; return done(); }
    if (c == "KeyP" && m_alt && !repeat) { stop(); handled = true; return done(); }  // Alt+P toggles play
    if (c == "KeyS" && m_alt && !repeat) { play(game::SimMode::simulate); handled = true; return done(); }
    if (playing_pawn) {
      // Ctrl+R restarts, except for soldiers (Ctrl crouches, R reloads).
      if (c == "KeyR" && m_ctrl && !soldier_pawn()) { world_->restart(); handled = true; return done(); }
      // Everything else drives the pawn.
      handled = !m_ctrl || c == "ControlLeft";
      return done();
    }
    // Ejected / simulating: the free camera and a subset of the editor keys.
    if (c == "KeyK" && !repeat && !flying) { keep_simulation_changes(); handled = true; return done(); }
    if (c == "KeyF" && !flying) { focus_selection(); handled = true; return done(); }
    if (c == "Period" && paused_) { world_->step_once(play_input()); handled = true; return done(); }
    if (flying) { handled = true; return done(); }
    if (c == "KeyG" && !m_ctrl) { Viewport& v = views_[active_view_]; v.game_view = !v.game_view; touch(); handled = true; return done(); }
    return done();
  }

  // ---- editing ----
  if (flying) {
    // WASD QE etc. move the camera; nothing else fires.
    handled = true;
    return done();
  }
  handled = true;
  if (m_ctrl && !m_alt) {
    if (c == "KeyZ") { if (m_shift) redo(); else undo(); }
    else if (c == "KeyY") redo();
    else if (c == "KeyC") copy_selection();
    else if (c == "KeyX") cut_selection();
    else if (c == "KeyV") { if (!paste()) { json::Value a = json::Value::object(); a.set("action", "readClipboard"); push_ui(std::move(a)); } }
    else if (c == "KeyD") duplicate_selection();
    else if (c == "KeyA") select_all();
    else if (c == "KeyG") group_selection();
    else if (c == "KeyH") {
      std::vector<ActorId> all;
      for (const game::Actor& a : level_.actors) if (a.hidden) all.push_back(a.id);
      if (!all.empty()) set_hidden(all, false);
    } else if (c == "KeyS") { json::Value a = json::Value::object(); a.set("action", m_shift ? "saveAs" : "save"); push_ui(std::move(a)); }
    else if (c == "KeyO") { json::Value a = json::Value::object(); a.set("action", "open"); push_ui(std::move(a)); }
    else if (c == "KeyN") { json::Value a = json::Value::object(); a.set("action", "dialog"); a.set("dialog", dialog_json("new", json::Value())); a.set("name", "new"); push_ui(std::move(a)); }
    else if (c == "Space") { json::Value a = json::Value::object(); a.set("action", "toggleDrawer"); push_ui(std::move(a)); }
    else if (c == "Backquote") { space_ = space_ == Space::world ? Space::local : Space::world; touch(); }
    else if (code.size() == 6 && code.rfind("Digit", 0) == 0 && code[5] >= '0' && code[5] <= '9') {
      bookmarks_[static_cast<u32>(code[5] - '0')] = views_[active_view_].camera;
      report(std::string("Bookmark ") + code[5] + " set");
    } else handled = false;
    return done();
  }
  if (m_alt && !m_ctrl) {
    if (c == "KeyP") play(game::SimMode::play);
    else if (c == "KeyS") play(game::SimMode::simulate);
    else if (c == "KeyG") set_view_type(active_view_, ViewType::perspective);
    else if (c == "KeyJ") set_view_type(active_view_, ViewType::top);
    else if (c == "KeyH") set_view_type(active_view_, ViewType::front);
    else if (c == "KeyK") set_view_type(active_view_, ViewType::right);
    else if (c == "Digit2") { views_[active_view_].mode = ViewMode::wireframe; touch(); }
    else if (c == "Digit3") { views_[active_view_].mode = ViewMode::unlit; touch(); }
    else if (c == "Digit4") { views_[active_view_].mode = ViewMode::lit; touch(); }
    else if (c == "Digit5") { views_[active_view_].mode = ViewMode::collision; touch(); }
    else handled = false;
    return done();
  }
  if (c == "KeyQ") set_tool(Tool::select);
  else if (c == "KeyW") set_tool(Tool::translate);
  else if (c == "KeyE") set_tool(Tool::rotate);
  else if (c == "KeyR") set_tool(Tool::scale);
  else if (c == "Space" && !repeat) set_tool(tool_ == Tool::translate ? Tool::rotate : tool_ == Tool::rotate ? Tool::scale : Tool::translate);
  else if (c == "Delete" || c == "Backspace") delete_selection();
  else if (c == "KeyF") focus_selection();
  else if (c == "End") snap_to_floor();
  else if (c == "KeyH" && m_shift) {
    std::vector<ActorId> others;
    for (const game::Actor& a : level_.actors) if (!selected(a.id) && !a.hidden && !a.city) others.push_back(a.id);
    if (!others.empty()) set_hidden(others, true);
  } else if (c == "KeyH") { if (!selection_.empty()) set_hidden(selection_, true); }
  else if (c == "KeyG") { Viewport& v = views_[active_view_]; v.game_view = !v.game_view; touch(); }
  else if (c == "Escape") {
    if (gizmo_.active) { end_gizmo(false); for (Viewport& v : views_) if (v.drag == Drag::gizmo) v.drag = Drag::none; }
    else deselect();
  } else if (c == "F2") {
    if (primary()) { json::Value a = json::Value::object(); a.set("action", "rename"); a.set("id", primary()); push_ui(std::move(a)); }
  } else if (c == "F11") { json::Value a = json::Value::object(); a.set("action", "fullscreen"); push_ui(std::move(a)); }
  else if (code.size() == 6 && code.rfind("Digit", 0) == 0 && code[5] >= '0' && code[5] <= '9' && !m_shift) {
    auto it = bookmarks_.find(static_cast<u32>(code[5] - '0'));
    if (it != bookmarks_.end()) { views_[active_view_].camera = it->second; touch(); report(std::string("Bookmark ") + code[5]); }
    else handled = false;
  } else if (c == "BracketLeft" || c == "BracketRight") {
    // Grid size down / up.
    static const f32 sizes[] = {0.01f, 0.05f, 0.1f, 0.25f, 0.5f, 1, 2, 5, 10, 50, 100};
    i32 i = 0;
    while (i < 10 && sizes[i] < snap_.grid_size - 1e-6f) ++i;
    i = clamp(i + (c == "BracketRight" ? 1 : -1), 0, 10);
    snap_.grid_size = sizes[i];
    report("Grid snap " + std::to_string(snap_.grid_size).substr(0, 5) + " m");
  } else handled = false;
  (void)key;
  return done();
}

void Studio::blur() {
  keys_.clear();
  touch_ = {};
  modifiers_ = 0;
  for (Viewport& v : views_) {
    if (v.drag == Drag::gizmo) end_gizmo(true);
    if (v.drag == Drag::grab) world_->end_grab();
    v.drag = Drag::none;
    v.buttons = 0;
    v.fov_boost = 0;
  }
  touch();
}

// ---------------------------------------------------------------------------
// panels

json::Value Studio::outliner_json() const {
  json::Value rows = json::Value::array();
  const std::string filter = lower(outliner_filter_);
  const ActorId the_player = game::default_player(level_);
  auto row = [&](const game::Actor& a, u32 depth) {
    json::Value r = json::Value::object();
    const char* type = "";
    const char* icon = "";
    actor_kind(a, type, icon);
    r.set("id", a.id);
    r.set("name", a.name);
    r.set("type", type);
    r.set("icon", icon);
    r.set("depth", depth);
    if (a.hidden) r.set("hidden", true);
    if (a.locked) r.set("locked", true);
    if (selected(a.id)) r.set("selected", true);
    if (a.id == primary()) r.set("primary", true);
    if (a.id == the_player) r.set("player", true);
    return r;
  };
  if (!filter.empty()) {
    for (const game::Actor& a : level_.actors) {
      const char* type = "";
      const char* icon = "";
      actor_kind(a, type, icon);
      if (lower(a.name).find(filter) == std::string::npos && lower(type).find(filter) == std::string::npos) {
        bool tag = false;
        for (const std::string& t : a.tags) tag |= lower(t).find(filter) != std::string::npos;
        if (!tag) continue;
      }
      rows.push(row(a, 0));
    }
  } else {
    // Children lists once.
    std::unordered_map<ActorId, std::vector<const game::Actor*>> kids;
    for (const game::Actor& a : level_.actors) kids[a.parent].push_back(&a);
    std::function<void(const game::Actor&, u32)> walk = [&](const game::Actor& a, u32 depth) {
      json::Value r = row(a, depth);
      auto it = kids.find(a.id);
      const bool has = it != kids.end() && !it->second.empty();
      const std::string key = "a:" + std::to_string(a.id);
      if (has) {
        r.set("children", static_cast<u32>(it->second.size()));
        r.set("collapsed", collapsed_.count(key) > 0);
      }
      rows.push(std::move(r));
      if (has && !collapsed_.count(key))
        for (const game::Actor* k : it->second) walk(*k, depth + 1);
    };
    // Folders (sorted) with their top-level actors, then unfiled actors.
    std::map<std::string, std::vector<const game::Actor*>> folders;
    for (const game::Actor* a : kids[0]) if (!a->folder.empty()) folders[a->folder].push_back(a);
    for (auto& [name, list] : folders) {
      json::Value f = json::Value::object();
      const std::string key = "f:" + name;
      f.set("folder", name);
      f.set("depth", 0);
      f.set("children", static_cast<u32>(list.size()));
      f.set("collapsed", collapsed_.count(key) > 0);
      bool all_hidden = true;
      for (const game::Actor* a : list) all_hidden = all_hidden && a->hidden;
      if (all_hidden) f.set("hidden", true);
      rows.push(std::move(f));
      if (!collapsed_.count(key))
        for (const game::Actor* a : list) walk(*a, 1);
    }
    for (const game::Actor* a : kids[0]) if (a->folder.empty()) walk(*a, 0);
  }
  json::Value o = json::Value::object();
  o.set("rows", std::move(rows));
  o.set("filter", outliner_filter_);
  o.set("total", static_cast<u32>(level_.actors.size()));
  o.set("selected", static_cast<u32>(selection_.size()));
  return o;
}

json::Value Studio::menu_json(std::string_view name) const {
  json::Value m = json::Value::array();
  const bool editing = play_ == PlayState::editing;
  const bool sel = !selection_.empty();
  if (name == "file") {
    m.push(entry("New Level...", cmd("dialog", "name", "new"), "Ctrl+N", editing));
    m.push(entry("Open Level...", cmd("ui", "action", "open"), "Ctrl+O", editing));
    m.push(separator());
    m.push(entry("Save", cmd("ui", "action", "save"), "Ctrl+S"));
    m.push(entry("Save As...", cmd("ui", "action", "saveAs"), "Ctrl+Shift+S"));
    m.push(separator());
    m.push(entry("Import Texture...", cmd("ui", "action", "importTexture"), "", editing));
    m.push(entry("Package Game (HTML)...", cmd("dialog", "name", "export"), "", editing));
    m.push(separator());
    json::Value recent = json::Value::array();
    recent.push(entry("Orb Run", cmd("new", "template", "orbRun"), "", editing));
    recent.push(entry("Physics Lab", cmd("new", "template", "physicsLab"), "", editing));
    recent.push(entry("Basic", cmd("new", "template", "basic"), "", editing));
    recent.push(entry("Empty", cmd("new", "template", "empty"), "", editing));
    m.push(submenu("Example Levels", std::move(recent)));
  } else if (name == "edit") {
    std::string u = "Undo", r = "Redo";
    if (can_undo()) u += " " + history_[history_index_].label;
    if (can_redo()) r += " " + history_[history_index_ + 1].label;
    m.push(entry(u, cmd("undo"), "Ctrl+Z", editing && can_undo()));
    m.push(entry(r, cmd("redo"), "Ctrl+Y", editing && can_redo()));
    m.push(entry("History...", cmd("dialog", "name", "history"), "", editing));
    m.push(separator());
    m.push(entry("Cut", cmd("cut"), "Ctrl+X", editing && sel));
    m.push(entry("Copy", cmd("copy"), "Ctrl+C", sel));
    m.push(entry("Paste", cmd("paste"), "Ctrl+V", editing));
    m.push(entry("Duplicate", cmd("duplicate"), "Ctrl+D", editing && sel));
    m.push(entry("Delete", cmd("delete"), "Delete", editing && sel));
    m.push(separator());
    m.push(entry("Select All", cmd("selectAll"), "Ctrl+A"));
    m.push(entry("Deselect", cmd("deselect"), "Esc", sel));
    m.push(entry("Group", cmd("group"), "Ctrl+G", editing && sel));
    m.push(entry("Snap to Floor", cmd("snapToFloor"), "End", editing && sel));
    m.push(separator());
    m.push(entry("Preferences...", cmd("dialog", "name", "preferences")));
  } else if (name == "window" || name == "view") {
    const Viewport& v = views_[active_view_];
    json::Value layouts = json::Value::array();
    layouts.push(entry("1 Viewport", cmd("layout", "layout", "single"), "", true, prefs_.layout == Layout::single));
    layouts.push(entry("2 Viewports", cmd("layout", "layout", "twoColumns"), "", true, prefs_.layout == Layout::two_columns));
    layouts.push(entry("4 Viewports", cmd("layout", "layout", "quad"), "", true, prefs_.layout == Layout::quad));
    m.push(submenu("Viewport Layout", std::move(layouts)));
    json::Value types = json::Value::array();
    const char* shortcuts[] = {"Alt+G", "", "", "", "Alt+K", "Alt+H", ""};
    const char* ids[] = {"perspective", "top", "bottom", "left", "right", "front", "back"};
    for (u32 i = 0; i < 7; ++i) {
      json::Value c = cmd("viewType");
      c.set("viewport", active_view_);
      c.set("type", ids[i]);
      types.push(entry(view_type_name(static_cast<ViewType>(i)), std::move(c), i == 1 ? "Alt+J" : shortcuts[i], true, v.type == static_cast<ViewType>(i)));
    }
    m.push(submenu("Viewport Type", std::move(types)));
    json::Value modes = json::Value::array();
    const char* mode_ids[] = {"lit", "unlit", "wireframe", "collision"};
    const char* mode_keys[] = {"Alt+4", "Alt+3", "Alt+2", "Alt+5"};
    for (u32 i = 0; i < 4; ++i) {
      json::Value c = cmd("viewMode");
      c.set("viewport", active_view_);
      c.set("mode", mode_ids[i]);
      modes.push(entry(view_mode_name(static_cast<ViewMode>(i)), std::move(c), mode_keys[i], true, v.mode == static_cast<ViewMode>(i)));
    }
    m.push(submenu("View Mode", std::move(modes)));
    json::Value show = json::Value::array();
    const std::pair<const char*, bool> flags[] = {{"grid", v.flags.grid},           {"icons", v.flags.icons},   {"selection", v.flags.selection},
                                                  {"colliders", v.flags.colliders}, {"joints", v.flags.joints}, {"contacts", v.flags.contacts},
                                                  {"bounds", v.flags.bounds},       {"fog", v.flags.fog},       {"sky", v.flags.sky},
                                                  {"stats", v.flags.stats}};
    const char* flag_labels[] = {"Grid", "Icons", "Selection Outline", "Collision Shapes", "Constraints", "Contact Points", "Bounds", "Fog", "Sky", "Stats"};
    for (std::size_t i = 0; i < std::size(flags); ++i) {
      json::Value c = cmd("showFlag");
      c.set("viewport", active_view_);
      c.set("flag", flags[i].first);
      c.set("value", !flags[i].second);
      show.push(entry(flag_labels[i], std::move(c), "", true, flags[i].second));
    }
    m.push(submenu("Show", std::move(show)));
    m.push(entry("Game View", cmd("gameView", "viewport", active_view_), "G", true, v.game_view));
    m.push(separator());
    m.push(entry("Focus Selection", cmd("focus"), "F"));
    m.push(entry("Content Drawer", cmd("ui", "action", "toggleDrawer"), "Ctrl+Space"));
    m.push(entry("Output Log", cmd("ui", "action", "toggleLog"), ""));
    m.push(entry("Fullscreen", cmd("ui", "action", "fullscreen"), "F11"));
  } else if (name == "play") {
    m.push(entry("Play", cmd("play", "mode", "play"), "Alt+P", play_ != PlayState::playing));
    m.push(entry("Simulate", cmd("play", "mode", "simulate"), "Alt+S", play_ == PlayState::editing));
    m.push(entry(paused_ ? "Resume" : "Pause", cmd("pause"), "Pause", !editing));
    m.push(entry("Advance One Frame", cmd("stepFrame"), ".", !editing && paused_));
    m.push(entry("Stop", cmd("stop"), "Esc", !editing));
    m.push(separator());
    m.push(entry(play_ == PlayState::playing ? "Eject from Player" : "Possess Player", cmd("eject"), "F8", !editing));
    m.push(entry("Restart Game", cmd("restart"), "Ctrl+R", !editing));
    m.push(entry("Keep Simulation Changes", cmd("keep"), "K", play_ == PlayState::simulating && sel));
    m.push(separator());
    {
      // Unreal's "Number of Players": a listen server and local clients in the other viewports.
      json::Value players = json::Value::array();
      for (i32 n = 1; n <= 4; ++n) {
        json::Value c = cmd("netPlayers");
        c.set("players", n);
        json::Value e = entry(n == 1 ? "1 Player" : std::to_string(n) + " Players (listen server)", std::move(c), "", editing);
        e.set("checked", net_play_.players == n);
        players.push(std::move(e));
      }
      m.push(submenu("Number of Players", std::move(players)));
      json::Value lag = json::Value::array();
      const struct { const char* label; f32 latency, jitter, loss; } presets[] = {
          {"Perfect", 0, 0, 0}, {"LAN (5 ms)", 5, 1, 0}, {"Average (50 ms, 1% loss)", 50, 10, 1}, {"Bad (150 ms, 5% loss)", 150, 40, 5}, {"Terrible (300 ms, 20% loss)", 300, 100, 20}};
      for (const auto& p : presets) {
        json::Value c = cmd("netSimulate");
        c.set("latency", p.latency);
        c.set("jitter", p.jitter);
        c.set("loss", p.loss);
        json::Value e = entry(p.label, std::move(c));
        e.set("checked", net_play_.network.latency_ms == p.latency && net_play_.network.loss_percent == p.loss);
        lag.push(std::move(e));
      }
      m.push(submenu("Network Emulation", std::move(lag)));
      m.push(entry("Host Online Game...", cmd("dialog", "name", "netHost"), "", editing));
      m.push(entry("Join Online Game...", cmd("dialog", "name", "netJoin"), "", editing));
      m.push(entry("Leave Online Game", cmd("netStop"), "", net_remote_));
    }
  } else if (name == "help") {
    m.push(entry("Controls...", cmd("dialog", "name", "controls"), "F1"));
    m.push(entry("About Kern Studio", cmd("dialog", "name", "about")));
  } else if (name == "context") {
    if (sel && editing) {
      m.push(entry("Focus", cmd("focus"), "F"));
      m.push(entry("Rename", cmd("ui", "action", "rename"), "F2", selection_.size() == 1));
      m.push(separator());
      m.push(entry("Cut", cmd("cut"), "Ctrl+X"));
      m.push(entry("Copy", cmd("copy"), "Ctrl+C"));
      m.push(entry("Paste", cmd("paste"), "Ctrl+V", !clipboard_.empty()));
      m.push(entry("Duplicate", cmd("duplicate"), "Ctrl+D"));
      m.push(entry("Delete", cmd("delete"), "Delete"));
      const game::Actor* p0 = level_.find(primary());
      if (p0 && selection_.size() == 1) m.push(entry(p0->logic ? "Edit Logic" : "Add Logic (Blueprint)", cmd("logicOpen", "owner", p0->id)));
      if (p0 && p0->controller) {
        const bool is = game::default_player(level_) == p0->id;
        m.push(entry(is ? "Is the Player" : "Make Player (control it in Play)", cmd("setPlayer", "id", p0->id), "", !is));
      }
      m.push(separator());
      m.push(entry("Snap to Floor", cmd("snapToFloor"), "End"));
      m.push(entry("Group", cmd("group"), "Ctrl+G"));
      if (selection_.size() >= 2) {
        json::Value c = cmd("attach");
        json::Value kids = json::Value::array();
        for (std::size_t i = 0; i + 1 < selection_.size(); ++i) kids.push(selection_[i]);
        c.set("ids", std::move(kids));
        c.set("parent", primary());
        m.push(entry("Attach to " + level_.find(primary())->name, std::move(c)));
        json::Value j = json::Value::array();
        const char* kinds[] = {"hinge", "slider", "ball", "fixed", "distance", "wheel", "motor"};
        const char* labels[] = {"Hinge", "Slider", "Ball", "Fixed (weld)", "Distance / rope", "Wheel", "Motor"};
        for (int i = 0; i < 7; ++i) {
          json::Value jc = cmd("connect");
          jc.set("kind", kinds[i]);
          j.push(entry(labels[i], std::move(jc)));
        }
        m.push(submenu("Connect with Constraint", std::move(j)));
      }
      const game::Actor* p = level_.find(primary());
      if (p && p->parent) m.push(entry("Detach", [&] { json::Value c = cmd("attach"); json::Value ids = json::Value::array(); for (ActorId id : selection_) ids.push(id); c.set("ids", std::move(ids)); c.set("parent", 0); return c; }()));
      m.push(separator());
      m.push(entry("Hide", cmd("hide"), "H"));
      m.push(entry(p && p->locked ? "Unlock" : "Lock", cmd("lock", "value", !(p && p->locked))));
      m.push(entry("Select Children", cmd("selectChildren")));
    } else if (sel) {
      m.push(entry("Focus", cmd("focus"), "F"));
      m.push(entry("Keep Simulation Changes", cmd("keep"), "K", play_ == PlayState::simulating));
    } else {
      m.push(entry("Paste", cmd("paste"), "Ctrl+V", editing && !clipboard_.empty()));
      m.push(entry("Select All", cmd("selectAll"), "Ctrl+A"));
      m.push(entry("Show All", cmd("showAll"), "Ctrl+H", editing));
    }
    if (editing) {
      m.push(separator());
      json::Value place = json::Value::array();
      const char* quick[] = {"cube", "sphere", "physics_cube", "point_light", "orb", "empty"};
      const char* quick_labels[] = {"Cube", "Sphere", "Physics Cube", "Point Light", "Orb", "Empty Actor"};
      for (int i = 0; i < 6; ++i) {
        json::Value c = cmd("spawn", "item", quick[i]);
        c.set("atCursor", true);
        place.push(entry(quick_labels[i], std::move(c)));
      }
      m.push(submenu("Place Actor", std::move(place)));
    }
  }
  return m;
}

json::Value Studio::dialog_json(std::string_view name, const json::Value& args) const {
  json::Value d = json::Value::object();
  d.set("name", name);
  json::Value fields = json::Value::array();
  auto field = [&](const char* path, const char* label, const char* type, json::Value value) {
    json::Value f = json::Value::object();
    f.set("path", path);
    f.set("label", label);
    f.set("type", type);
    f.set("value", std::move(value));
    return f;
  };
  auto buttons = [&](const char* primary_label) {
    json::Value b = json::Value::array();
    json::Value p = json::Value::object();
    p.set("label", primary_label);
    p.set("primary", true);
    b.push(std::move(p));
    json::Value c = json::Value::object();
    c.set("label", "Cancel");
    b.push(std::move(c));
    d.set("buttons", std::move(b));
  };
  if (name == "new") {
    d.set("title", "New Level");
    json::Value cards = json::Value::array();
    const char* ids[] = {"orbRun", "physicsLab", "shootingRange", "basic", "empty"};
    const char* labels[] = {"Orb Run", "Physics Lab", "Shooting Range", "Basic", "Empty"};
    const char* help[] = {"Procedural night city, Box3D hover car, 24 orbs to collect",
                          "Every Box3D joint: hinge, slider, wheel, rope bridge, motor, stacks",
                          "First-person Soldier (desktop, gamepad, touch), targets, cover - multiplayer ready",
                          "Floor, sun and sky - a clean start",
                          "Nothing at all"};
    for (int i = 0; i < 5; ++i) {
      json::Value c = json::Value::object();
      c.set("id", ids[i]);
      c.set("label", labels[i]);
      c.set("help", help[i]);
      cards.push(std::move(c));
    }
    d.set("templates", std::move(cards));
    if (dirty()) d.set("text", "Unsaved changes of the current level are kept in the autosave until you start editing the new one.");
    buttons("Create");
  } else if (name == "preferences") {
    d.set("title", "Preferences");
    json::Value s = field("cameraSpeed", "Camera speed (1-8)", "integer", prefs_.camera_speed);
    s.set("min", 1);
    s.set("max", 8);
    fields.push(std::move(s));
    json::Value sens = field("mouseSensitivity", "Mouse sensitivity", "number", static_cast<f64>(prefs_.mouse_sensitivity));
    sens.set("min", 0.1);
    sens.set("max", 5);
    sens.set("step", 0.05);
    fields.push(std::move(sens));
    fields.push(field("invertY", "Invert mouse Y", "bool", prefs_.invert_y));
    fields.push(field("orbitAroundSelection", "Orbit around selection (Alt+LMB)", "bool", prefs_.orbit_around_selection));
    fields.push(field("snapGrid", "Grid snapping", "bool", snap_.grid));
    json::Value gs = field("gridSize", "Grid size", "number", static_cast<f64>(snap_.grid_size));
    gs.set("min", 0.001);
    gs.set("max", 1000);
    gs.set("unit", "m");
    fields.push(std::move(gs));
    fields.push(field("snapRotation", "Rotation snapping", "bool", snap_.rotation));
    json::Value rs = field("rotationStep", "Rotation step", "number", static_cast<f64>(snap_.rotation_step));
    rs.set("min", 0.1);
    rs.set("max", 180);
    rs.set("unit", "°");
    fields.push(std::move(rs));
    fields.push(field("snapScale", "Scale snapping", "bool", snap_.scale));
    json::Value ss = field("scaleStep", "Scale step", "number", static_cast<f64>(snap_.scale_step));
    ss.set("min", 0.001);
    ss.set("max", 100);
    fields.push(std::move(ss));
    buttons("Apply");
  } else if (name == "export") {
    d.set("title", "Package Game");
    d.set("text", "Builds one self-contained HTML file with the Kern runtime and this level. It plays full screen in any browser - no editor, no server.");
    fields.push(field("title", "Game title", "text", level_.world.title.empty() ? level_.name : level_.world.title));
    buttons("Package");
  } else if (name == "history") {
    d.set("title", "Undo History");
    json::Value list = json::Value::array();
    for (std::size_t i = 0; i < history_.size(); ++i) {
      json::Value e = json::Value::object();
      e.set("index", static_cast<u32>(i));
      e.set("label", history_[i].label);
      if (i == history_index_) e.set("current", true);
      list.push(std::move(e));
    }
    d.set("history", std::move(list));
    json::Value b = json::Value::array();
    json::Value c = json::Value::object();
    c.set("label", "Close");
    c.set("primary", true);
    b.push(std::move(c));
    d.set("buttons", std::move(b));
  } else if (name == "netHost" || name == "netJoin") {
    const bool host = name == "netHost";
    d.set("title", host ? "Host Online Game" : "Join Online Game");
    d.set("text", host ? "Players join with the same room name. Leave the relay empty for players in other tabs of this browser; "
                         "for other machines run tools/net_relay.mjs and enter its address (ws://host:8790). Everyone needs this level."
                       : "Enter the room the host chose (and the relay address if the host uses one). You need the same level as the host.");
    fields.push(field("room", "Room", "text", "kern"));
    fields.push(field("relay", "Relay (optional)", "text", ""));
    buttons(host ? "Host" : "Join");
  } else if (name == "rename") {
    const ActorId id = args.is_object() && args["id"].is_number() ? static_cast<ActorId>(args["id"].as_number()) : primary();
    const game::Actor* a = level_.find(id);
    d.set("title", "Rename");
    d.set("id", id);
    fields.push(field("name", "Name", "text", a ? a->name : ""));
    buttons("Rename");
  } else if (name == "controls") {
    d.set("title", "Controls");
    const char* rows[][2] = {
        {"Right mouse + WASD / QE", "Fly (Unreal fly mode); wheel while flying changes speed, Shift = faster, Z / C zoom"},
        {"Left click", "Select (Shift adds, Ctrl toggles); click empty space to deselect"},
        {"Left drag", "Marquee select (Shift adds, Ctrl toggles); actors move with the gizmo (W / E / R)"},
        {"Right + left mouse", "Move up / down and sideways while flying"},
        {"Middle mouse drag", "Pan"},
        {"Alt + left / right / middle", "Orbit / dolly / pan around the selection"},
        {"Mouse wheel", "Move forward / back (zoom in orthographic views)"},
        {"F  /  End", "Focus selection  /  snap to floor"},
        {"Q  W  E  R  /  Space", "Select / move / rotate / scale  /  cycle"},
        {"Alt + drag gizmo", "Duplicate while moving"},
        {"Drag from Place Actors", "Drop a new actor exactly where you release it"},
        {"Ctrl+Z / Ctrl+Y", "Undo / redo"},
        {"Ctrl+C / V / X / D, Delete", "Copy, paste, cut, duplicate, delete"},
        {"H  /  Shift+H  /  Ctrl+H", "Hide selected / hide others / show all"},
        {"Ctrl+1..9  /  1..9", "Set / jump to camera bookmark"},
        {"Alt+G / J / H / K", "Perspective / top / front / side view"},
        {"Alt+2 / 3 / 4 / 5", "Wireframe / unlit / lit / collision view"},
        {"G", "Game view (hide editor helpers)"},
        {"[  /  ]", "Smaller / larger grid snap"},
        {"Alt+P  /  Alt+S", "Play  /  Simulate"},
        {"Esc  /  F8  /  Shift+F1", "Stop  /  eject or possess  /  free the mouse"},
        {"Ctrl + drag (Simulate)", "Grab and throw physics bodies"},
        {"K (Simulate)", "Keep the simulated poses of the selection"},
    };
    json::Value list = json::Value::array();
    for (auto& r : rows) {
      json::Value e = json::Value::array();
      e.push(r[0]);
      e.push(r[1]);
      list.push(std::move(e));
    }
    d.set("table", std::move(list));
    json::Value b = json::Value::array();
    json::Value c = json::Value::object();
    c.set("label", "Close");
    c.set("primary", true);
    b.push(std::move(c));
    d.set("buttons", std::move(b));
  } else if (name == "about") {
    d.set("title", "Kern Studio");
    d.set("text",
          "The editor of the Kern engine: a C++ renderer (WebGL2 through WebAssembly) and Box3D rigid-body physics, "
          "with levels made of actors and components that play right in the viewport. Everything in the viewport is "
          "rendered and simulated by the same native code that runs exported games.");
    json::Value b = json::Value::array();
    json::Value c = json::Value::object();
    c.set("label", "Close");
    c.set("primary", true);
    b.push(std::move(c));
    d.set("buttons", std::move(b));
  } else {
    return json::Value();
  }
  d.set("fields", std::move(fields));
  return d;
}

json::Value Studio::submit_dialog(std::string_view name, const json::Value& values) {
  if (name == "netHost" || name == "netJoin") {
    // The page opens the transport, tells us our address and then hosts / joins.
    json::Value a = json::Value::object();
    a.set("action", "netOpen");
    a.set("host", name == "netHost");
    a.set("room", values["room"].is_string() && !values["room"].as_string().empty() ? values["room"].as_string() : std::string("kern"));
    a.set("relay", values["relay"].is_string() ? values["relay"].as_string() : std::string());
    push_ui(std::move(a));
    return ok();
  }
  if (name == "new") {
    const std::string t = values["template"].is_string() ? values["template"].as_string() : "basic";
    load_level(template_level(t));
    report("New level: " + level_.name);
    return ok();
  }
  if (name == "preferences") {
    if (values["cameraSpeed"].is_number()) prefs_.camera_speed = clamp(static_cast<i32>(values["cameraSpeed"].as_number()), 1, 8);
    if (values["mouseSensitivity"].is_number()) prefs_.mouse_sensitivity = clamp(static_cast<f32>(values["mouseSensitivity"].as_number()), 0.1f, 5.0f);
    if (values["invertY"].is_bool()) prefs_.invert_y = values["invertY"].as_bool();
    if (values["orbitAroundSelection"].is_bool()) prefs_.orbit_around_selection = values["orbitAroundSelection"].as_bool();
    if (values["snapGrid"].is_bool()) snap_.grid = values["snapGrid"].as_bool();
    if (values["gridSize"].is_number()) snap_.grid_size = clamp(static_cast<f32>(values["gridSize"].as_number()), 0.001f, 1000.0f);
    if (values["snapRotation"].is_bool()) snap_.rotation = values["snapRotation"].as_bool();
    if (values["rotationStep"].is_number()) snap_.rotation_step = clamp(static_cast<f32>(values["rotationStep"].as_number()), 0.1f, 180.0f);
    if (values["snapScale"].is_bool()) snap_.scale = values["snapScale"].as_bool();
    if (values["scaleStep"].is_number()) snap_.scale_step = clamp(static_cast<f32>(values["scaleStep"].as_number()), 0.001f, 100.0f);
    for (Viewport& v : views_) v.grid_key = 0;
    touch();
    return ok();
  }
  if (name == "export") {
    json::Value r = ok();
    game::Level copy = level_;
    if (values["title"].is_string() && !values["title"].as_string().empty()) copy.world.title = values["title"].as_string();
    r.set("text", game::to_text(copy, -1));
    r.set("title", copy.world.title.empty() ? copy.name : copy.world.title);
    json::Value a = json::Value::object();
    a.set("action", "packageGame");
    a.set("text", r["text"]);
    a.set("title", r["title"]);
    push_ui(std::move(a));
    report("Packaging " + r["title"].as_string());
    return r;
  }
  if (name == "rename") {
    const ActorId id = values["id"].is_number() ? static_cast<ActorId>(values["id"].as_number()) : primary();
    if (!values["name"].is_string()) return error("name missing");
    return ok(rename(id, values["name"].as_string()));
  }
  return ok();
}

json::Value Studio::viewports_json() const {
  json::Value list = json::Value::array();
  static const char* type_ids[] = {"perspective", "top", "bottom", "left", "right", "front", "back"};
  static const char* mode_ids[] = {"lit", "unlit", "wireframe", "collision"};
  for (const Viewport& v : views_) {
    json::Value o = json::Value::object();
    o.set("index", v.index);
    o.set("type", type_ids[static_cast<u32>(v.type)]);
    o.set("typeName", view_type_name(v.type));
    o.set("mode", mode_ids[static_cast<u32>(v.mode)]);
    o.set("modeName", view_mode_name(v.mode));
    o.set("active", v.index == active_view_);
    o.set("gameView", v.game_view);
    json::Value f = json::Value::object();
    f.set("grid", v.flags.grid);
    f.set("icons", v.flags.icons);
    f.set("selection", v.flags.selection);
    f.set("colliders", v.flags.colliders);
    f.set("joints", v.flags.joints);
    f.set("contacts", v.flags.contacts);
    f.set("bounds", v.flags.bounds);
    f.set("fog", v.flags.fog);
    f.set("sky", v.flags.sky);
    f.set("stats", v.flags.stats);
    o.set("flags", std::move(f));
    o.set("fov", static_cast<f64>(v.camera.fov));
    list.push(std::move(o));
  }
  return list;
}

json::Value Studio::state() {
  json::Value s = json::Value::object();
  s.set("revision", static_cast<f64>(revision_));
  s.set("playerOnly", player_only_);
  json::Value lv = json::Value::object();
  lv.set("name", level_.name);
  lv.set("dirty", dirty());
  lv.set("title", level_.world.title);
  lv.set("actors", static_cast<u32>(level_.actors.size()));
  s.set("level", std::move(lv));
  static const char* play_names[] = {"editing", "simulating", "playing"};
  json::Value p = json::Value::object();
  p.set("state", play_names[static_cast<u32>(play_)]);
  p.set("paused", paused_);
  p.set("ejected", ejected_);
  p.set("captured", mouse_captured_);
  s.set("play", std::move(p));
  s.set("tool", tool_name(tool_));
  s.set("space", space_ == Space::world ? "world" : "local");
  json::Value sn = json::Value::object();
  sn.set("grid", snap_.grid);
  sn.set("gridSize", static_cast<f64>(snap_.grid_size));
  sn.set("rotation", snap_.rotation);
  sn.set("rotationStep", static_cast<f64>(snap_.rotation_step));
  sn.set("scale", snap_.scale);
  sn.set("scaleStep", static_cast<f64>(snap_.scale_step));
  s.set("snap", std::move(sn));
  json::Value pr = json::Value::object();
  pr.set("cameraSpeed", prefs_.camera_speed);
  pr.set("cameraSpeedMs", static_cast<f64>(camera_speed()));
  pr.set("layout", prefs_.layout == Layout::single ? "single" : prefs_.layout == Layout::quad ? "quad" : "twoColumns");
  s.set("prefs", std::move(pr));
  s.set("activeViewport", active_view_);
  s.set("viewports", viewports_json());
  s.set("outliner", outliner_json());
  s.set("details", details_json());
  s.set("world", world_json());
  if (logic_open_) s.set("logic", logic_json());
  json::Value sel = json::Value::array();
  for (ActorId id : selection_) sel.push(id);
  s.set("selection", std::move(sel));
  json::Value actors = json::Value::array();
  for (const game::Actor& a : level_.actors) {
    json::Value e = json::Value::array();
    e.push(a.id);
    e.push(a.name);
    actors.push(std::move(e));
  }
  s.set("actors", std::move(actors));
  json::Value tex = json::Value::array();
  for (const game::TextureAsset& t : level_.textures) {
    json::Value e = json::Value::object();
    e.set("id", t.id);
    e.set("name", t.name);
    e.set("width", t.width);
    e.set("height", t.height);
    tex.push(std::move(e));
  }
  s.set("textures", std::move(tex));
  {
    json::Value au = json::Value::object();
    au.set("ready", audio_ready());
    au.set("muted", muted());
    au.set("previewing", previewing());
    json::Value imported = json::Value::array();
    for (const game::SoundAsset& snd : level_.sounds) {
      json::Value e = json::Value::object();
      e.set("id", snd.id);
      e.set("name", snd.name);
      e.set("duration", internal::round_num(snd.duration, 100));
      imported.push(std::move(e));
    }
    au.set("imported", std::move(imported));
    static const json::Value builtin = [] {
      json::Value list = json::Value::array();
      for (const audio::BuiltinSound& b : audio::builtin_sounds()) {
        json::Value e = json::Value::object();
        e.set("id", std::string("builtin:") + b.name);
        e.set("name", b.label);
        e.set("category", b.category);
        e.set("loop", b.loop);
        list.push(std::move(e));
      }
      return list;
    }();
    au.set("builtin", builtin);
    s.set("sounds", std::move(au));
  }
  json::Value h = json::Value::object();
  h.set("canUndo", can_undo());
  h.set("index", static_cast<f64>(history_index_));
  h.set("size", static_cast<f64>(history_.size()));
  h.set("dirty", dirty());
  h.set("canRedo", can_redo());
  h.set("undo", can_undo() ? history_[history_index_].label : std::string());
  h.set("redo", can_redo() ? history_[history_index_ + 1].label : std::string());
  s.set("history", std::move(h));
  json::Value menus = json::Value::object();
  for (const char* m : {"file", "edit", "window", "play", "help"}) menus.set(m, menu_json(m));
  s.set("menus", std::move(menus));
  json::Value log = json::Value::array();
  const std::size_t from = log_.size() > 200 ? log_.size() - 200 : 0;
  for (std::size_t i = from; i < log_.size(); ++i) {
    json::Value e = json::Value::array();
    e.push(log_[i].level);
    e.push(log_[i].text);
    e.push(internal::round_num(log_[i].time / 1000.0, 100));
    log.push(std::move(e));
  }
  s.set("log", std::move(log));
  s.set("statusText", status_text_);
  s.set("bookmarks", [&] { json::Value b = json::Value::array(); for (auto& [k, v] : bookmarks_) { (void)v; b.push(k); } return b; }());
  return s;
}


static json::Value hud_json(const game::Hud& h, bool player) {
  json::Value hud = json::Value::object();
  hud.set("status", game::game_status_name(h.status));
  hud.set("title", h.title);
  hud.set("message", h.message);
  hud.set("messageAge", internal::round_num(h.message_age, 100));
  hud.set("score", internal::round_num(h.score, 10));
  hud.set("pickups", h.pickups);
  hud.set("pickupsTotal", h.pickups_total);
  hud.set("elapsed", internal::round_num(h.elapsed, 100));
  hud.set("remaining", internal::round_num(h.remaining, 100));
  hud.set("speed", internal::round_num(h.speed, 10));
  hud.set("boost", internal::round_num(h.boost, 100));
  hud.set("best", internal::round_num(h.best, 100));
  hud.set("player", player);
  if (h.soldier) {
    json::Value f = json::Value::object();
    f.set("health", internal::round_num(h.health, 10));
    f.set("maxHealth", internal::round_num(h.max_health, 10));
    f.set("armor", internal::round_num(h.armor, 10));
    f.set("ammo", h.ammo);
    f.set("magazine", h.magazine);
    f.set("reserve", h.reserve);
    f.set("reload", internal::round_num(h.reload, 100));
    f.set("spread", internal::round_num(h.spread, 100));
    f.set("hitAge", internal::round_num(h.hit_age, 100));
    f.set("hitHead", h.hit_head);
    f.set("hitKill", h.hit_kill);
    f.set("damageAge", internal::round_num(h.damage_age, 100));
    f.set("damageYaw", internal::round_num(h.damage_yaw, 10));
    f.set("respawnIn", internal::round_num(h.respawn_in, 10));
    f.set("aiming", h.aiming);
    f.set("fov", internal::round_num(h.fov, 10));
    hud.set("soldier", std::move(f));
  }
  return hud;
}

json::Value Studio::touch_json() const {
  const game::TouchView& t = touch_ctl_.view();
  auto r = [](f32 v) { return internal::round_num(v, 10); };
  auto stick = [&](const game::TouchStick& s) {
    json::Value j = json::Value::object();
    j.set("x", r(s.x));
    j.set("y", r(s.y));
    j.set("size", r(s.size));
    j.set("kx", internal::round_num(s.knob_x, 1000));
    j.set("ky", internal::round_num(s.knob_y, 1000));
    j.set("live", s.live);
    j.set("accent", s.accent);
    j.set("label", s.label);
    return j;
  };
  json::Value j = json::Value::object();
  j.set("move", stick(t.move));
  j.set("look", stick(t.look));
  json::Value buttons = json::Value::array();
  for (const game::TouchButton& b : t.buttons) {
    json::Value o = json::Value::object();
    o.set("id", b.id);
    o.set("label", b.label);
    o.set("x", r(b.x));
    o.set("y", r(b.y));
    o.set("w", r(b.w));
    o.set("h", r(b.h));
    o.set("round", b.round);
    o.set("pressed", b.pressed);
    o.set("on", b.on);
    char hex[8];
    std::snprintf(hex, sizeof hex, "#%06x", b.color & 0xffffff);
    o.set("color", std::string(hex));
    buttons.push(std::move(o));
  }
  j.set("buttons", std::move(buttons));
  j.set("fire", t.fire);
  if (t.fire) { j.set("fireX", r(t.fire_x)); j.set("fireY", r(t.fire_y)); }
  return j;
}

json::Value Studio::status() {
  json::Value s = json::Value::object();
  s.set("revision", static_cast<f64>(revision_));
  s.set("fps", internal::round_num(fps_, 10));
  static const char* play_names[] = {"editing", "simulating", "playing"};
  s.set("play", play_names[static_cast<u32>(play_)]);
  s.set("paused", paused_);
  s.set("captured", mouse_captured_);
  {
    json::Value in = json::Value::object();
    in.set("device", input_.touch() ? "touch" : "mouse");
    in.set("gamepad", input_.gamepad());
    s.set("input", std::move(in));
  }
  if (touch_ctl_.view().visible && play_ == PlayState::playing && input_.touch()) s.set("touch", touch_json());
  if (logic_open_ && world_->running()) s.set("logicTrace", logic_trace_json());
  if (logic::Runtime* rt = world_->running() ? world_->logic_runtime() : nullptr) {
    json::Value lg = json::Value::object();
    u32 compiled = 0;
    for (const auto& i : rt->instances()) compiled += i->compiled() ? 1 : 0;
    lg.set("graphs", static_cast<u32>(rt->instances().size()));
    lg.set("compiled", compiled);
    s.set("logic", std::move(lg));
  }
  if (play_ != PlayState::editing) {
    json::Value hud = hud_json(world_->hud(), world_->player() != 0 && play_ == PlayState::playing);
    s.set("hud", std::move(hud));
    const game::SimStats st = world_->stats();
    json::Value stats = json::Value::object();
    stats.set("bodies", st.bodies);
    stats.set("awake", st.awake);
    stats.set("shapes", st.shapes);
    stats.set("joints", st.joints);
    stats.set("broken", st.broken_joints);
    stats.set("contacts", st.contacts);
    stats.set("steps", static_cast<f64>(st.steps));
    s.set("sim", std::move(stats));
  }
  if (audio_ready()) {
    const audio::Stats& st = mixer_->stats();
    json::Value au = json::Value::object();
    json::Value peak = json::Value::array();
    peak.push(internal::round_num(st.master.peak[0], 1000));
    peak.push(internal::round_num(st.master.peak[1], 1000));
    au.set("peak", std::move(peak));
    au.set("voices", st.voices);
    au.set("audible", st.audible);
    au.set("muted", muted());
    s.set("audio", std::move(au));
  }
  if (net_) {
    const game::NetStats& ns = net_->stats();
    json::Value n = json::Value::object();
    n.set("role", net_->role() == game::NetRole::server ? "server" : "client");
    n.set("status", net_->status());
    n.set("players", ns.players);
    n.set("rtt", internal::round_num(ns.rtt_ms, 10));
    n.set("loss", internal::round_num(ns.packet_loss, 10));
    n.set("up", internal::round_num(ns.sent_kbps, 10));
    n.set("down", internal::round_num(ns.received_kbps, 10));
    n.set("local", static_cast<u32>(clients_.size()));
    json::Value cl = json::Value::array();
    for (const LocalClient& c : clients_) {
      json::Value o = json::Value::object();
      o.set("status", c.game->status());
      o.set("rtt", internal::round_num(c.game->stats().rtt_ms, 10));
      o.set("buffer", internal::round_num(c.game->stats().buffer_ms, 10));
      if (c.game->playing()) {
        json::Value hud = hud_json(c.world->hud(), true);
        o.set("hud", std::move(hud));
      }
      cl.push(std::move(o));
    }
    n.set("clients", std::move(cl));
    s.set("net", std::move(n));
  }
  if (player_only_) return s;
  const Viewport& v = views_[active_view_];
  json::Value cam = json::Value::object();
  cam.set("position", internal::vec_json(v.camera.position));
  cam.set("yaw", internal::round_num(v.camera.yaw, 10));
  cam.set("pitch", internal::round_num(v.camera.pitch, 10));
  cam.set("speed", prefs_.camera_speed);
  s.set("camera", std::move(cam));
  s.set("timeOfDay", internal::round_num(world_->time_of_day(), 1000));
  json::Value views = json::Value::array();
  for (const Viewport& w : views_) {
    json::Value o = json::Value::object();
    static const char* drags[] = {"none", "fly", "pedestal", "pan", "orbit", "dolly", "gizmo", "marquee", "pan", "grab", "look"};
    o.set("drag", drags[static_cast<u32>(w.drag)]);
    if (w.drag == Drag::marquee && w.moved) {
      json::Value r = json::Value::array();
      r.push(static_cast<f64>(min_of(w.press_x, w.x)));
      r.push(static_cast<f64>(min_of(w.press_y, w.y)));
      r.push(static_cast<f64>(std::fabs(w.x - w.press_x)));
      r.push(static_cast<f64>(std::fabs(w.y - w.press_y)));
      o.set("marquee", std::move(r));
    }
    if (w.type != ViewType::perspective) o.set("orthoWidth", internal::round_num(w.camera.ortho_height * w.css_width / max_of(1.0f, w.css_height), 100));
    views.push(std::move(o));
  }
  s.set("viewports", std::move(views));
  if (gizmo_.active) {
    json::Value g = json::Value::object();
    if (gizmo_.tool == Tool::translate) g.set("delta", internal::vec_json(gizmo_.delta));
    else if (gizmo_.tool == Tool::rotate) g.set("angle", internal::round_num(gizmo_.angle, 100));
    else g.set("factor", internal::vec_json(gizmo_.factor));
    s.set("gizmo", std::move(g));
  }
  // Primary actor transform for the details panel to follow gizmo drags live.
  if (const game::Actor* a = level_.find(primary())) {
    json::Value t = json::Value::object();
    t.set("id", a->id);
    t.set("position", internal::vec_json(a->transform.position));
    t.set("rotation", internal::vec_json(a->transform.rotation));
    t.set("scale", internal::vec_json(a->transform.scale));
    s.set("transform", std::move(t));
  }
  return s;
}

// ---------------------------------------------------------------------------
// commands

json::Value Studio::command(const json::Value& c) {
  if (!ready_) return error("not initialised");
  if (!c.is_object() || !c["cmd"].is_string()) return error("command needs a \"cmd\" string");
  return run(c);
}

json::Value Studio::run(const json::Value& c) {
  const std::string& name = c["cmd"].as_string();
  auto num = [&](const char* k, f64 fallback = 0) { return c[k].is_number() ? c[k].as_number() : fallback; };
  auto str = [&](const char* k) -> std::string { return c[k].is_string() ? c[k].as_string() : std::string(); };
  auto view_index = [&]() { return c["viewport"].is_number() ? clamp(static_cast<u32>(num("viewport")), 0u, 3u) : active_view_; };
  if (name.rfind("logic", 0) == 0) return logic_command(name, c);

  // ---- selection ----
  if (name == "select") {
    select(ids_of(c["ids"].is_null() ? c["id"] : c["ids"]), c["add"].as_bool(), c["toggle"].as_bool());
    if (c["focus"].as_bool()) focus_selection();
    return ok();
  }
  if (name == "selectRange") {
    // Shift-click in the outliner: everything between the primary and `id` in document order.
    const ActorId to = static_cast<ActorId>(num("id"));
    const ActorId from = primary();
    std::vector<ActorId> ids;
    bool inside = false;
    for (const game::Actor& a : level_.actors) {
      const bool edge = a.id == from || a.id == to;
      if (edge || inside) ids.push_back(a.id);
      if (edge && from != to) inside = !inside;
    }
    select(ids, true);
    if (level_.find(to)) select({to}, true);
    return ok();
  }
  if (name == "selectAll") { select_all(); return ok(); }
  if (name == "deselect") { deselect(); return ok(); }
  if (name == "selectChildren") {
    std::vector<ActorId> ids = selection_;
    for (const game::Actor& a : level_.actors)
      for (ActorId s : selection_) if (level_.is_ancestor(s, a.id)) { ids.push_back(a.id); break; }
    select(ids);
    return ok();
  }
  if (name == "selectFolder") {
    std::vector<ActorId> ids;
    for (const game::Actor& a : level_.actors) if (a.folder == str("folder") && !a.parent) ids.push_back(a.id);
    select(ids, c["add"].as_bool());
    return ok();
  }

  // ---- actors ----
  if (name == "spawn") {
    std::optional<Vec3> at;
    Vec3 p;
    if (internal::json_vec(c["at"], p)) at = p;
    else if (c["x"].is_number() || c["atCursor"].as_bool()) {
      Viewport& v = views_[view_index()];
      const f32 x = c["x"].is_number() ? static_cast<f32>(num("x")) : v.press_x;
      const f32 y = c["y"].is_number() ? static_cast<f32>(num("y")) : v.press_y;
      Vec3 hit;
      if (pick(v.index, x, y, &hit)) at = hit;
      else {
        Ray r;
        if (ray(v, x, y, r) && internal::ray_plane(r.origin, r.direction, {0, 0, 0}, {0, 0, 1}, hit) && distance(hit, r.origin) < 2000) at = hit;
      }
      if (at && v.type != ViewType::perspective && v.type != ViewType::top && v.type != ViewType::bottom) {}  // side views keep the hit
    }
    const ActorId id = spawn(str("item"), at, str("sound"));
    json::Value r = ok(id != 0);
    r.set("id", id);
    return r;
  }
  if (name == "delete") { delete_selection(); return ok(); }
  if (name == "duplicate") { duplicate_selection(); return ok(); }
  if (name == "copy") { copy_selection(); return ok(); }
  if (name == "cut") { cut_selection(); return ok(); }
  if (name == "paste") {
    if (paste()) return ok();
    json::Value a = json::Value::object();
    a.set("action", "readClipboard");
    push_ui(std::move(a));
    return ok(false);
  }
  if (name == "pasteText") {
    // Actors copied in another tab / session arrive as text.
    json::Value doc;
    if (!json::parse(str("text"), doc) || doc["format"].as_string() != "kern-actors" || !doc["actors"].is_array()) return error("clipboard holds no Kern actors");
    std::vector<game::Actor> list;
    for (const json::Value& v : doc["actors"].items()) {
      game::Actor a;
      std::string err;
      if (game::actor_from_json(v, a, &err)) list.push_back(std::move(a));
    }
    if (list.empty()) return error("clipboard holds no Kern actors");
    clipboard_ = std::move(list);
    return ok(paste());
  }
  if (name == "undo") { undo(); return ok(); }
  if (name == "redo") { redo(); return ok(); }
  if (name == "historyJump") {
    const f64 i = num("index", -1);
    if (!(i >= 0) || i != std::floor(i)) return ok(false);
    return ok(jump_history(static_cast<std::size_t>(i)));
  }
  if (name == "rename") return ok(rename(static_cast<ActorId>(num("id", primary())), str("name")));
  if (name == "attach") {
    std::vector<ActorId> ids = ids_of(c["ids"]);
    if (ids.empty()) ids = selection_;
    return ok(attach(ids, static_cast<ActorId>(num("parent"))));
  }
  if (name == "moveTo") {
    // Outliner drag and drop: onto an actor (attach), a folder or the root.
    std::vector<ActorId> ids = ids_of(c["ids"]);
    if (ids.empty()) return ok(false);
    if (c["parent"].is_number() && num("parent") > 0) return ok(attach(ids, static_cast<ActorId>(num("parent"))));
    const std::string folder = str("folder");
    // Detaching and moving into the folder are one step.
    return ok(edit(folder.empty() ? "Detach" : "Move to folder", [&](game::Level& l) {
      reparent(l, ids, 0);
      for (ActorId id : ids) if (game::Actor* a = l.find(id)) a->folder = folder;
      return true;
    }));
  }
  if (name == "setFolder") {
    std::vector<ActorId> ids = ids_of(c["ids"]);
    if (ids.empty()) ids = selection_roots();
    const std::string folder = str("folder");
    return ok(edit(folder.empty() ? "Remove from folder" : "Move to folder", [&](game::Level& l) {
      for (ActorId id : ids) if (game::Actor* a = l.find(id)) if (!a->parent) a->folder = folder;
      return true;
    }));
  }
  if (name == "renameFolder") {
    const std::string from = str("from"), to = str("to");
    if (from.empty() || to.empty() || from == to) return ok(false);
    return ok(edit("Rename folder", [&](game::Level& l) {
      for (game::Actor& a : l.actors) if (a.folder == from) a.folder = to;
      return true;
    }));
  }
  if (name == "group") { group_selection(); return ok(); }
  if (name == "snapToFloor") { snap_to_floor(); return ok(); }
  if (name == "hide" || name == "show") {
    std::vector<ActorId> ids = ids_of(c["ids"]);
    if (ids.empty()) ids = selection_;
    if (!str("folder").empty())
      for (const game::Actor& a : level_.actors) if (a.folder == str("folder")) ids.push_back(a.id);
    bool hidden = name == "hide";
    if (c["value"].is_bool()) hidden = c["value"].as_bool();
    set_hidden(ids, hidden);
    return ok();
  }
  if (name == "showAll") {
    std::vector<ActorId> ids;
    for (const game::Actor& a : level_.actors) if (a.hidden) ids.push_back(a.id);
    if (!ids.empty()) set_hidden(ids, false);
    return ok();
  }
  if (name == "lock") {
    std::vector<ActorId> ids = ids_of(c["ids"]);
    if (ids.empty()) ids = selection_;
    const bool locked = c["value"].is_bool() ? c["value"].as_bool() : true;
    const bool done = edit(locked ? "Lock" : "Unlock", [&](game::Level& l) {
      for (ActorId id : ids) if (game::Actor* a = l.find(id)) a->locked = locked;
      if (locked) {  // locked actors leave the selection (part of the step, so undo brings them back)
        std::vector<ActorId> keep;
        for (ActorId id : selection_) if (std::find(ids.begin(), ids.end(), id) == ids.end()) keep.push_back(id);
        select(keep);
      }
      return true;
    });
    return ok(done);
  }
  if (name == "focus") {
    if (c["ids"].is_array() || c["id"].is_number()) select(ids_of(c["ids"].is_null() ? c["id"] : c["ids"]));
    focus_selection(c["viewport"].is_number() ? static_cast<i32>(num("viewport")) : -1);
    return ok();
  }
  if (name == "set") return ok(set_property(str("path"), c["value"], str("transaction")));
  if (name == "setWorld") return ok(set_world_property(str("path"), c["value"], str("transaction")));
  if (name == "addComponent") return ok(add_component(str("type")));
  if (name == "removeComponent") return ok(remove_component(str("type"), static_cast<i32>(num("index"))));
  if (name == "connect") {
    // Joint from every other selected actor to the primary one.
    if (selection_.size() < 2) { report("Select two actors: the constrained one, then (last) the one it hangs on", "warn"); return ok(false); }
    game::JointKind kind = game::JointKind::hinge;
    const std::string k = str("kind");
    const char* names[] = {"hinge", "slider", "ball", "fixed", "distance", "wheel", "motor", "parallel", "noCollision"};
    for (u32 i = 0; i < 9; ++i) if (k == names[i]) kind = static_cast<game::JointKind>(i);
    const ActorId target = primary();
    std::vector<ActorId> movers(selection_.begin(), selection_.end() - 1);
    return ok(edit("Connect", [&](game::Level& l) {
      const Mat4 target_world = l.world_matrix(target);
      for (ActorId id : movers) {
        game::Actor* a = l.find(id);
        if (!a) continue;
        game::JointComponent j;
        j.kind = kind;
        j.target = target;
        // Anchor halfway between the two actors, in this actor's space.
        const Mat4 world = l.world_matrix(id);
        const Vec3 mid = (world.translation() + target_world.translation()) * 0.5f;
        if (kind != game::JointKind::distance && kind != game::JointKind::wheel) j.anchor = internal::inverse(world).transform_point(mid);
        if (!a->body && kind != game::JointKind::no_collision) {
          a->body = game::BodyComponent{};
        }
        a->joints.push_back(j);
      }
      return true;
    }));
  }
  if (name == "importTexture") {
    const std::string src = str("src");
    if (src.rfind("data:image/", 0) != 0) return error("textures are data URLs of images");
    std::string id;
    const i32 w = static_cast<i32>(num("width")), h = static_cast<i32>(num("height"));
    const bool assign = c["assign"].as_bool(true);
    const bool done = edit("Import texture", [&](game::Level& l) {
      u32 n = 1;
      auto taken = [&](const std::string& s) { return l.texture(s) != nullptr; };
      do id = "tex" + std::to_string(n++); while (taken(id));
      game::TextureAsset t;
      t.id = id;
      t.name = str("name").empty() ? id : str("name");
      t.src = std::make_shared<const std::string>(src);
      t.width = w;
      t.height = h;
      l.textures.push_back(std::move(t));
      if (assign)
        for (ActorId sid : selection_)
          if (game::Actor* a = l.find(sid)) if (a->mesh) {
            a->mesh->material.texture = id;
            a->mesh->material.color = {1, 1, 1};
          }
      return true;
    });
    json::Value r = ok(done);
    r.set("id", id);
    return r;
  }
  if (name == "removeTexture") {
    const std::string id = str("id");
    return ok(edit("Remove texture", [&](game::Level& l) {
      const std::size_t before = l.textures.size();
      l.textures.erase(std::remove_if(l.textures.begin(), l.textures.end(), [&](const game::TextureAsset& t) { return t.id == id; }), l.textures.end());
      for (game::Actor& a : l.actors) if (a.mesh && a.mesh->material.texture == id) a.mesh->material.texture.clear();
      return l.textures.size() != before;
    }));
  }

  // ---- play ----
  if (name == "play") {
    if (c["players"].is_number()) net_play_.players = clamp(static_cast<i32>(num("players")), 1, 4);
    return ok(play(str("mode") == "simulate" ? game::SimMode::simulate : game::SimMode::play));
  }
  if (name == "netPlayers") {
    net_play_.players = clamp(static_cast<i32>(num("players", 1)), 1, 4);
    report(net_play_.players == 1 ? "Play: one player" : "Play: " + std::to_string(net_play_.players) + " players (listen server + " + std::to_string(net_play_.players - 1) + " client" + (net_play_.players > 2 ? "s" : "") + ")");
    touch();
    return ok();
  }
  if (name == "netSimulate") {
    net_play_.network.latency_ms = clamp(static_cast<f32>(num("latency", net_play_.network.latency_ms)), 0.0f, 1000.0f);
    net_play_.network.jitter_ms = clamp(static_cast<f32>(num("jitter", net_play_.network.jitter_ms)), 0.0f, 500.0f);
    net_play_.network.loss_percent = clamp(static_cast<f32>(num("loss", net_play_.network.loss_percent)), 0.0f, 90.0f);
    net_play_.network.duplicate_percent = clamp(static_cast<f32>(num("duplicate", net_play_.network.duplicate_percent)), 0.0f, 50.0f);
    host_transport_.simulator().settings = net_play_.network;
    touch();
    return ok();
  }
  if (name == "netAddress") {
    host_transport_.set_address(net::Address{static_cast<u64>(num("self"))});
    return ok();
  }
  if (name == "netHost") return ok(net_host());
  if (name == "netJoin") return ok(net_join(static_cast<u64>(num("server"))));
  if (name == "netStop") { net_stop(); return ok(); }
  if (name == "stop") { stop(); return ok(); }
  if (name == "setPlayer") {
    const ActorId id = static_cast<ActorId>(num("id"));
    const game::Actor* a = level_.find(id);
    if (!a || !a->controller) { report("Only an actor with a Player Controller can be the player", "warn"); return ok(false); }
    edit("Make Player", [&](game::Level& l) { l.world.player = id; return true; });
    report(a->name + " is the player now", "info");
    return ok();
  }
  if (name == "pause") {
    if (c["value"].is_bool()) { if (c["value"].as_bool() != paused_) toggle_pause(); }
    else toggle_pause();
    return ok();
  }
  if (name == "stepFrame") { if (play_ != PlayState::editing) world_->step_once(play_input()); return ok(); }
  if (name == "eject") { eject(); return ok(); }
  if (name == "restart") { if (play_ != PlayState::editing) { world_->restart(); report("Restarted"); } return ok(); }
  if (name == "keep") { keep_simulation_changes(); return ok(); }
  if (name == "gameInput") {
    if (c["moveX"].is_number()) touch_.move_x = clamp(static_cast<f32>(num("moveX")), -1.0f, 1.0f);
    if (c["moveY"].is_number()) touch_.move_y = clamp(static_cast<f32>(num("moveY")), -1.0f, 1.0f);
    if (c["up"].is_number()) touch_.up = clamp(static_cast<f32>(num("up")), -1.0f, 1.0f);
    if (c["jump"].is_bool()) touch_.jump = c["jump"].as_bool();
    if (c["boost"].is_bool()) touch_.boost = c["boost"].as_bool();
    if (c["fire"].is_bool()) touch_.fire = c["fire"].as_bool();
    if (c["aim"].is_bool()) touch_.aim = c["aim"].as_bool();
    if (c["reload"].is_bool()) touch_.reload = c["reload"].as_bool();
    if (c["crouch"].is_bool()) touch_.crouch = c["crouch"].as_bool();
    if (c["interact"].is_bool()) touch_.interact = c["interact"].as_bool();
    // Touch look: finger travel in CSS pixels (SpawnCity: 0.0068 rad/px across, 0.0059 rad/px up/down).
    if (c["lookX"].is_number()) touch_look_x_ += static_cast<f32>(num("lookX")) * 0.39f;
    if (c["lookY"].is_number()) touch_look_y_ += static_cast<f32>(num("lookY")) * 0.34f;
    return ok();
  }
  if (name == "gamepad") {
    // Xbox / standard-mapping gamepad state, sent every frame by the page while one is connected.
    game::PlayInput& g = pad_;
    g = game::PlayInput{};
    pad_connected_ = c["connected"].as_bool(true);
    if (!pad_connected_) return ok();
    g.gamepad = true;
    if (c["active"].as_bool(false)) input_.gamepad_used();
    g.move_x = clamp(static_cast<f32>(num("lx")), -1.0f, 1.0f);
    g.move_y = clamp(static_cast<f32>(num("ly")), -1.0f, 1.0f);
    g.look_stick_x = clamp(static_cast<f32>(num("rx")), -1.0f, 1.0f);
    g.look_stick_y = clamp(static_cast<f32>(num("ry")), -1.0f, 1.0f);
    g.fire = num("rt") > 0.35;
    g.aim = num("lt") > 0.35;
    g.jump = c["a"].as_bool();
    g.crouch = c["b"].as_bool();
    g.reload = c["x"].as_bool();
    g.interact = c["y"].as_bool();
    g.boost = c["l3"].as_bool() || c["lb"].as_bool();
    g.up = (c["rb"].as_bool() ? 1.0f : 0.0f) - (c["lb"].as_bool() ? 1.0f : 0.0f);
    if (c["start"].as_bool() && !pad_start_ && play_ != PlayState::editing) toggle_pause();
    pad_start_ = c["start"].as_bool();
    return ok();
  }
  if (name == "touchRelease") { touch_ctl_.release_all(); return ok(); }
  if (name == "device") {
    // What the page knows about the device and which one was used last (SpawnCity's rules:
    // touch switches to touch, a mouse only on a fine-pointer device, any key to desktop).
    if (c["platform"].is_object()) {
      const json::Value& p = c["platform"];
      input_.set_platform(p["fine"].as_bool(true), p["coarse"].as_bool(false), static_cast<i32>(p["touchPoints"].as_number(0)));
    }
    if (c["safe"].is_array() && c["safe"].size() == 4)
      for (u32 i = 0; i < 4; ++i) safe_[i] = static_cast<f32>(c["safe"][i].as_number(0));
    const std::string ev = c["event"].as_string();
    const bool was_touch = input_.touch();
    if (ev == "mouse") input_.pointer_down(false);
    else if (ev == "mousemove") input_.mouse_moved();
    else if (ev == "touch") input_.pointer_down(true);
    if (was_touch != input_.touch()) {
      touch_ctl_.release_all();
      if (!input_.touch()) touch_.fire = false;
      touch();
    }
    return ok();
  }
  if (name == "releasePointer") { mouse_captured_ = false; touch(); return ok(); }
  if (name == "pointerCaptured") { mouse_captured_ = c["value"].as_bool(true); touch(); return ok(); }

  // ---- tools, views ----
  if (name == "tool") {
    const std::string t = str("tool");
    set_tool(t == "select" ? Tool::select : t == "rotate" ? Tool::rotate : t == "scale" ? Tool::scale : Tool::translate);
    return ok();
  }
  if (name == "space") {
    const std::string s = str("space");
    space_ = s == "local" ? Space::local : s == "world" ? Space::world : (space_ == Space::world ? Space::local : Space::world);
    touch();
    return ok();
  }
  if (name == "snap") {
    if (c["grid"].is_bool()) snap_.grid = c["grid"].as_bool();
    if (c["gridSize"].is_number()) snap_.grid_size = clamp(static_cast<f32>(num("gridSize")), 0.001f, 1000.0f);
    if (c["rotation"].is_bool()) snap_.rotation = c["rotation"].as_bool();
    if (c["rotationStep"].is_number()) snap_.rotation_step = clamp(static_cast<f32>(num("rotationStep")), 0.1f, 180.0f);
    if (c["scale"].is_bool()) snap_.scale = c["scale"].as_bool();
    if (c["scaleStep"].is_number()) snap_.scale_step = clamp(static_cast<f32>(num("scaleStep")), 0.001f, 100.0f);
    for (Viewport& v : views_) v.grid_key = 0;
    touch();
    return ok();
  }
  if (name == "cameraSpeed") { prefs_.camera_speed = clamp(static_cast<i32>(num("speed", 4)), 1, 8); touch(); return ok(); }
  if (name == "layout") {
    const std::string l = str("layout");
    set_layout(l == "quad" ? Layout::quad : l == "twoColumns" ? Layout::two_columns : Layout::single);
    return ok();
  }
  if (name == "maximize") {
    // Toggles between the quad layout and this viewport alone (Unreal: the maximise button).
    active_view_ = view_index();
    set_layout(prefs_.layout == Layout::single ? Layout::quad : Layout::single);
    return ok();
  }
  if (name == "activeViewport") { active_view_ = view_index(); touch(); return ok(); }
  if (name == "viewType") {
    const std::string t = str("type");
    const char* ids[] = {"perspective", "top", "bottom", "left", "right", "front", "back"};
    for (u32 i = 0; i < 7; ++i) if (t == ids[i]) { set_view_type(view_index(), static_cast<ViewType>(i)); return ok(); }
    return error("unknown view type " + t);
  }
  if (name == "viewMode") {
    const std::string m = str("mode");
    const char* ids[] = {"lit", "unlit", "wireframe", "collision"};
    for (u32 i = 0; i < 4; ++i) if (m == ids[i]) { views_[view_index()].mode = static_cast<ViewMode>(i); touch(); return ok(); }
    return error("unknown view mode " + m);
  }
  if (name == "showFlag") {
    ShowFlags& f = views_[view_index()].flags;
    const std::string k = str("flag");
    const bool v = c["value"].as_bool();
    bool* target = k == "grid" ? &f.grid : k == "icons" ? &f.icons : k == "selection" ? &f.selection : k == "colliders" ? &f.colliders
                 : k == "joints" ? &f.joints : k == "contacts" ? &f.contacts : k == "bounds" ? &f.bounds : k == "fog" ? &f.fog
                 : k == "sky" ? &f.sky : k == "stats" ? &f.stats : nullptr;
    if (!target) return error("unknown show flag " + k);
    *target = v;
    ++scene_revision_;
    touch();
    return ok();
  }
  if (name == "gameView") {
    Viewport& v = views_[view_index()];
    v.game_view = c["value"].is_bool() ? c["value"].as_bool() : !v.game_view;
    touch();
    return ok();
  }
  if (name == "fov") { views_[view_index()].camera.fov = clamp(static_cast<f32>(num("value", 70)), 10.0f, 150.0f); touch(); return ok(); }
  if (name == "bookmark") {
    const u32 slot = static_cast<u32>(num("slot"));
    if (c["set"].as_bool()) bookmarks_[slot] = views_[active_view_].camera;
    else if (bookmarks_.count(slot)) views_[active_view_].camera = bookmarks_[slot];
    touch();
    return ok();
  }

  // ---- panels ----
  if (name == "filter") { outliner_filter_ = str("text"); touch(); return ok(); }
  if (name == "collapse") {
    const std::string k = str("key");
    const bool v = c["value"].is_bool() ? c["value"].as_bool() : !collapsed_.count(k);
    if (v) collapsed_.insert(k);
    else collapsed_.erase(k);
    touch();
    return ok();
  }
  if (name == "section") {
    const std::string k = str("id");
    const bool closed = c["closed"].is_bool() ? c["closed"].as_bool() : !closed_sections_.count(k);
    if (closed) closed_sections_.insert(k);
    else closed_sections_.erase(k);
    touch();
    return ok();
  }
  if (name == "menu") {
    json::Value r = ok();
    r.set("entries", menu_json(str("name")));
    return r;
  }
  if (name == "dialog") {
    json::Value d = dialog_json(str("name"), c["args"]);
    if (d.is_null()) return error("unknown dialog " + str("name"));
    json::Value a = json::Value::object();
    a.set("action", "dialog");
    a.set("name", str("name"));
    a.set("dialog", d);
    push_ui(std::move(a));
    json::Value r = ok();
    r.set("dialog", std::move(d));
    return r;
  }
  if (name == "submitDialog") return submit_dialog(str("name"), c["values"]);
  if (name == "ui") {
    // Menu entries that the page carries out (file pickers, panels).
    json::Value a = json::Value::object();
    a.set("action", str("action"));
    if (str("action") == "rename") a.set("id", primary());
    push_ui(std::move(a));
    return ok();
  }
  if (name == "catalog") {
    json::Value r = ok();
    r.set("groups", catalog_json());
    return r;
  }
  if (name == "clearLog") { log_.clear(); touch(); return ok(); }

  // ---- documents ----
  if (name == "new") {
    load_level(template_level(str("template")));
    report("New level: " + level_.name);
    return ok();
  }
  if (name == "open") return ok(load_level_text(str("text"), str("name").empty() ? "level" : str("name")));
  if (name == "save") {
    json::Value r = ok();
    r.set("text", level_text());
    std::string file = level_.name.empty() ? "level" : level_.name;
    for (char& ch : file) if (!std::isalnum(static_cast<unsigned char>(ch)) && ch != '-' && ch != '_') ch = '_';
    r.set("file", file + ".klevel");
    return r;
  }
  if (name == "saved") { mark_saved(); report("Saved " + level_.name); return ok(); }
  // ---- audio ----
  if (name == "previewSound") {
    preview_sound(str("sound"), static_cast<f32>(num("volume", 1)), static_cast<f32>(num("pitch", 1)));
    return ok();
  }
  if (name == "stopSound") { stop_preview(); return ok(); }
  if (name == "importSound") {
    const std::string id = import_sound(str("name"), str("src"), static_cast<f32>(num("duration")));
    json::Value r = ok(!id.empty());
    r.set("id", id);
    return r;
  }
  if (name == "removeSound") return ok(remove_sound(str("id")));
  if (name == "renameSound") {
    const std::string id = str("id"), to = str("name");
    if (to.empty()) return error("a sound needs a name");
    return ok(edit("Rename Sound", [&](game::Level& l) {
      for (game::SoundAsset& s : l.sounds)
        if (s.id == id) { s.name = to; return true; }
      return false;
    }));
  }
  if (name == "mute") {
    set_muted(c["muted"].is_bool() ? c["muted"].as_bool() : !muted());
    report(muted() ? "Sound muted" : "Sound on");
    return ok();
  }
  if (name == "project") {
    // Screen position (CSS pixels in the viewport) of an actor's pivot, or of a world point.
    const u32 vi = view_index();
    Vec3 p;
    if (c["id"].is_number()) {
      const ActorId id = static_cast<ActorId>(num("id"));
      Mat4 m;
      if (world_->actor_matrix(id, m)) p = m.translation();
      else if (level_.find(id)) p = level_.world_matrix(id).translation();  // not presented yet (added this frame)
      else return error("unknown actor");
    } else if (c["point"].is_array() && c["point"].size() == 3) {
      p = {static_cast<f32>(c["point"][0].as_number()), static_cast<f32>(c["point"][1].as_number()), static_cast<f32>(c["point"][2].as_number())};
    } else return error("project needs id or point");
    f32 x = 0, y = 0;
    json::Value r = ok();
    r.set("visible", to_screen(views_[vi], p, x, y));
    r.set("x", x);
    r.set("y", y);
    return r;
  }
  if (name == "packageText") {
    json::Value r = ok();
    game::Level copy = level_;
    r.set("text", game::to_text(copy, -1));
    r.set("title", copy.world.title.empty() ? copy.name : copy.world.title);
    return r;
  }
  if (name == "validate") {
    json::Value r = ok();
    json::Value list = json::Value::array();
    for (const game::Issue& i : game::validate(level_)) {
      json::Value e = json::Value::object();
      e.set("error", i.error);
      e.set("actor", i.actor);
      e.set("text", i.text);
      list.push(std::move(e));
    }
    r.set("issues", std::move(list));
    return r;
  }
  if (name == "console") return run([&] {
    // A tiny console: "help" lists the commands.
    std::istringstream in(str("text"));
    std::string word;
    in >> word;
    json::Value out = cmd("consoleResult");
    out.set("word", word);
    std::string rest;
    std::getline(in, rest);
    if (!rest.empty() && rest[0] == ' ') rest.erase(0, 1);
    out.set("rest", rest);
    return out;
  }());
  if (name == "consoleResult") {
    const std::string w = lower(str("word")), rest = str("rest");
    std::istringstream in(rest);
    if (w == "help" || w.empty()) {
      report("Console: play | simulate | stop | spawn <item> | select <name> | tod <0..1> | gravity <x y z> | camspeed <1-8> | stat | clear");
    } else if (w == "play") play(game::SimMode::play);
    else if (w == "simulate") play(game::SimMode::simulate);
    else if (w == "stop") stop();
    else if (w == "spawn") spawn(rest);
    else if (w == "select") {
      std::vector<ActorId> ids;
      for (const game::Actor& a : level_.actors) if (lower(a.name).find(lower(rest)) != std::string::npos) ids.push_back(a.id);
      select(ids);
      report("Selected " + std::to_string(ids.size()) + " actors");
    } else if (w == "tod") {
      f64 t = 0;
      in >> t;
      json::Value v = t;
      set_world_property("world.timeOfDay", v);
    } else if (w == "gravity") {
      f64 x = 0, y = 0, z = -9.80665;
      in >> x >> y >> z;
      json::Value g = json::Value::array();
      g.push(x);
      g.push(y);
      g.push(z);
      set_world_property("world.gravity", g);
    } else if (w == "camspeed") {
      i32 s = 4;
      in >> s;
      prefs_.camera_speed = clamp(s, 1, 8);
      touch();
    } else if (w == "stat") {
      char line[200];
      if (play_ != PlayState::editing) {
        const game::SimStats st = world_->stats();
        std::snprintf(line, sizeof line, "%.0f fps - %u bodies (%u awake), %u shapes, %u joints (%u broken), %u contacts", fps_, st.bodies, st.awake, st.shapes, st.joints, st.broken_joints, st.contacts);
      } else {
        std::snprintf(line, sizeof line, "%.0f fps - %zu actors, %zu textures", fps_, level_.actors.size(), level_.textures.size());
      }
      report(line);
    } else if (w == "clear") { log_.clear(); touch(); }
    else { report("Unknown console command: " + w + " (try help)", "warn"); return ok(false); }
    return ok();
  }
  return error("unknown command " + name);
}

}  // namespace kern::studio
