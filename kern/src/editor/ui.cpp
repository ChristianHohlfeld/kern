// The page protocol: panel state, status, menus, dialogs, keyboard handling
// and the command dispatcher. Everything the DOM shows is produced here.
#include "internal.h"

namespace kern::editor {

namespace {

json::Value cmd(const char* name) {
  json::Value c = json::Value::object();
  c.set("cmd", name);
  return c;
}
json::Value cmd(const char* name, const char* key, json::Value value) {
  json::Value c = cmd(name);
  c.set(key, std::move(value));
  return c;
}
json::Value entry(const std::string& label, json::Value command, const std::string& key = {}, bool disabled = false) {
  json::Value e = json::Value::object();
  e.set("label", label);
  if (!key.empty()) e.set("key", key);
  e.set("cmd", std::move(command));
  if (disabled) e.set("disabled", true);
  return e;
}
json::Value submenu(const std::string& label, json::Value entries, const std::string& key = {}) {
  json::Value e = json::Value::object();
  e.set("label", label);
  if (!key.empty()) e.set("key", key);
  e.set("sub", std::move(entries));
  return e;
}
json::Value separator() { return json::Value("-"); }
json::Value ui(const char* action) {
  json::Value a = json::Value::object();
  a.set("action", action);
  return a;
}

const char* const kShortcutRows[][2] = {
    {"B / F / V / J / I / M / W", "Bodies / fixtures / vertices / joints / images / samplers / world"},
    {"T or G", "Translate selected items"},
    {"R / S", "Rotate / scale"},
    {"X / Y / Z", "Restrict transform axis"},
    {"Ctrl / Shift", "Snap / fine control"},
    {"C → S / C → C", "Cursor to selection / exact cursor"},
    {"E (vertex mode)", "Add hull vertex at cursor"},
    {"T / R (joint mode, repeated)", "Cycle anchor A / anchor B / both or axis"},
    {"L (joint mode)", "Edit lower / upper joint limit"},
    {"Shift+D", "Duplicate, then translate"},
    {"Delete", "Delete selected items"},
    {"Ctrl+A / Ctrl+Shift+A", "Select all / deselect all"},
    {"Ctrl+Z / Ctrl+Shift+Z", "Undo / redo"},
    {"Ctrl+C / Ctrl+V", "Copy / paste"},
    {"Ctrl+N / Ctrl+O / Ctrl+S", "New / open / save scene"},
    {"Ctrl+R", "Open player / reload player"},
    {"Ctrl+Shift+C / Ctrl+T", "Clone view / tile views"},
    {"Space (editor)", "Action menu"},
    {"Space (player)", "Pause / single step (jump while a game runs)"},
    {"WASD / arrows (player)", "Game controls: move / steer"},
    {"Space · Shift (player)", "Jump / fly up · fly down"},
    {"R / P (player game)", "Restart / pause the game"},
    {"Home", "Fit scene"},
    {"RMB / MMB / wheel", "Orbit / pan / zoom"},
    {"F11", "Hide / show panels"},
    {"Esc", "Cancel active operation"},
    {"Enter / typed numbers", "Confirm / precise transform"},
    {"F1", "Keyboard reference"},
};

json::Value block(const char* type) {
  json::Value b = json::Value::object();
  b.set("type", type);
  return b;
}
json::Value para(const std::string& html) {
  json::Value b = block("p");
  b.set("html", html);
  return b;
}
json::Value input_field(const std::string& id, const std::string& label, const char* input, json::Value value) {
  json::Value b = block("field");
  b.set("id", id);
  b.set("label", label);
  b.set("input", input);
  b.set("value", std::move(value));
  return b;
}
json::Value button(const std::string& label, const char* action) {
  json::Value b = json::Value::object();
  b.set("label", label);
  b.set("action", action);
  return b;
}

}  // namespace

// ---------------------------------------------------------------------------
// panels

json::Value Editor::tree_json() {
  json::Value out = json::Value::object();
  json::Value rows = json::Value::array();
  Document* d = doc();
  if (!d) return out;
  std::string query = tree_filter_;
  for (char& c : query) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  auto row = [&](const std::string& id, const std::string& name, Mode m, int indent, const char* disclose) {
    if (tree_kind_ != "all" && tree_kind_ != mode_name(m)) return;
    std::string lower = name;
    for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (!query.empty() && lower.find(query) == std::string::npos) return;
    json::Value r = json::Value::object();
    r.set("id", id);
    r.set("name", name.empty() ? "(unnamed)" : name);
    r.set("mode", mode_name(m));
    r.set("selected", contains(d->selection[m], id));
    r.set("indent", indent);
    r.set("disclose", disclose);
    rows.push(std::move(r));
  };
  for (const Body& b : d->scene.bodies) {
    const bool closed = contains(collapsed_, b.id);
    row(b.id, b.name, Mode::body, 0, closed ? "closed" : "open");
    if (!closed || !query.empty() || tree_kind_ == "fixture")
      for (const Fixture& f : b.fixtures) row(f.id, f.name, Mode::fixture, 1, "none");
  }
  for (const Joint& j : d->scene.joints) row(j.id, j.name, Mode::joint, 0, "none");
  for (const ImagePlane& i : d->scene.images) row(i.id, i.name, Mode::image, 0, "none");
  for (const Sampler& i : d->scene.samplers) row(i.id, i.name, Mode::sampler, 0, "none");
  out.set("rows", std::move(rows));
  out.set("empty", "No items. Space → Add → Body.");
  return out;
}

std::string Editor::context_html() const {
  if (active_ && active_->type == ViewType::player) {
    std::string t = "<h3>Player view</h3><p>Independent Box3D copy of the scene.</p><ul>"
                    "<li><span class=\"key\">Left mouse:</span> grab dynamic bodies with Box3D MotorJoint</li>"
                    "<li><span class=\"key\">Right mouse:</span> orbit</li><li><span class=\"key\">Middle mouse:</span> pan</li>"
                    "<li><span class=\"key\">Wheel:</span> zoom</li><li><span class=\"key\">Ctrl+R:</span> reload latest scene</li>";
    if (active_->runtime && active_->runtime->game().status != world::GameStatus::inactive)
      t += "<li><span class=\"key\">WASD / arrows:</span> move</li><li><span class=\"key\">Space:</span> jump · <span class=\"key\">Shift:</span> fly down</li>"
           "<li><span class=\"key\">R:</span> restart · <span class=\"key\">P:</span> pause</li>";
    else
      t += "<li><span class=\"key\">Space:</span> pause / single-step</li>";
    return t + "</ul>";
  }
  const std::string mode_l = mode_label(mode_);
  if (operation_) {
    std::string lower = mode_l;
    for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    std::string t = "<h3>" + std::string(operation_name(operation_->kind)) + " " + lower +
                    "</h3><p>Move the mouse or type a number.</p><ul><li>Enter / click confirms</li><li>Esc cancels</li>"
                    "<li>X / Y / Z constrains axis</li><li>Ctrl snaps · Shift fine control</li></ul>";
    if (mode_ == Mode::joint)
      t += "<p>Repeated <span class=\"key\">T/R</span> cycles anchor A → anchor B → both/axis. <span class=\"key\">L</span> cycles lower → upper limit. Stage " +
           std::to_string(operation_->stage + 1) + ".</p>";
    return t;
  }
  return "<h3>" + mode_l + " edit mode</h3><ul><li>Left click: select</li><li>Drag left: box selection</li><li>Shift: add · Ctrl: toggle</li>"
         "<li>T / G: translate</li><li>R: rotate</li><li>S: scale</li>" +
         std::string(mode_ == Mode::joint ? "<li>L: joint lower / upper limit</li>" : "") +
         "<li>Shift+D: duplicate + translate</li><li>Delete: delete</li></ul><p>C: 3D cursor · Space: action menu · Home: fit scene</p>"
         "<p>RMB orbit · MMB pan · wheel zoom</p>";
}

json::Value Editor::state() {
  json::Value s = json::Value::object();
  s.set("revision", static_cast<f64>(ui_revision_));
  s.set("mode", mode_name(mode_));
  s.set("tiled", tiled_);
  s.set("playerOnly", player_only_);
  json::Value tabs = json::Value::array(), views = json::Value::array();
  for (const auto& vp : views_) {
    const View& v = *vp;
    const Document* d = document(v.doc);
    if (!d) continue;
    const std::string name = std::string(v.type == ViewType::player ? "(Run) " : "") + d->scene.name + (d->dirty ? " *" : "");
    json::Value t = json::Value::object();
    t.set("view", v.id);
    t.set("title", name);
    t.set("active", &v == active_);
    t.set("type", v.type == ViewType::player ? "player" : "editor");
    tabs.push(t);
    json::Value o = json::Value::object();
    o.set("id", v.id);
    o.set("type", v.type == ViewType::player ? "player" : "editor");
    o.set("title", name);
    o.set("visible", tiled_ || &v == active_);
    o.set("focused", &v == active_);
    o.set("playing", v.playing);
    o.set("speed", v.speed);
    o.set("touchOrbit", v.touch_orbit);
    o.set("game", v.runtime && v.runtime->game().status != world::GameStatus::inactive);
    views.push(std::move(o));
  }
  s.set("tabs", std::move(tabs));
  s.set("views", std::move(views));
  if (doc()) {
    s.set("tree", tree_json());
    s.set("properties", fields_json());
    Document* d = doc();
    json::Value h = json::Value::object();
    h.set("index", static_cast<f64>(d->index));
    json::Value labels = json::Value::array();
    for (const HistoryEntry& e : d->history) labels.push(e.label);
    h.set("entries", std::move(labels));
    s.set("history", std::move(h));
    s.set("counts", std::to_string(d->scene.bodies.size()) + " bodies · " + std::to_string(d->scene.fixture_count()) + " fixtures · " +
                        std::to_string(d->scene.joints.size()) + " joints");
    s.set("sceneName", d->scene.name);
  }
  s.set("context", context_html());
  json::Value opts = json::Value::object();
  opts.set("grid", options_.grid);
  opts.set("labels", options_.labels);
  opts.set("shapes", options_.shapes);
  opts.set("joints", options_.joints);
  opts.set("images", options_.images);
  opts.set("centers", options_.centers);
  opts.set("wireframe", options_.wireframe);
  opts.set("fixtureColors", options_.fixture_colors);
  s.set("options", std::move(opts));
  if (operation_) {
    json::Value op = json::Value::object();
    op.set("kind", operation_name(operation_->kind));
    op.set("axis", operation_->axis ? std::string(1, operation_->axis) : std::string());
    op.set("typed", operation_->typed);
    op.set("stage", operation_->stage);
    s.set("operation", std::move(op));
  }
  json::Value uiq = json::Value::array();
  for (UiAction& a : pending_ui_) uiq.push(std::move(a.data));
  pending_ui_.clear();
  s.set("ui", std::move(uiq));
  s.set("logCount", static_cast<f64>(log_.size()));
  return s;
}

json::Value Editor::game_hud(const View& v) const {
  if (!v.runtime) return {};
  const world::GameState& g = v.runtime->game();
  if (g.status == world::GameStatus::inactive) return {};
  json::Value h = json::Value::object();
  h.set("status", g.status == world::GameStatus::playing ? "playing" : g.status == world::GameStatus::won ? "won" : "lost");
  h.set("score", g.score);
  h.set("pickups", static_cast<f64>(g.pickups_collected));
  h.set("pickupsTotal", static_cast<f64>(g.pickups_total));
  h.set("elapsed", std::round(g.elapsed * 10) / 10);
  const f64 limit = v.runtime->source().game.time_limit;
  if (limit > 0) h.set("remaining", std::max(0.0, std::round((limit - g.elapsed) * 10) / 10));
  h.set("message", g.message);
  h.set("title", v.runtime->source().game.title);
  DPose pose;
  if (!g.player.empty() && v.runtime->body_pose(g.player, pose)) {
    json::Value p = json::Value::array();
    for (f64 c : pose.position) p.push(std::round(c * 1000) / 1000);
    h.set("player", std::move(p));
  }
  return h;
}

json::Value Editor::status() {
  json::Value s = json::Value::object();
  std::string m = std::string(mode_label(mode_)) + " edit";
  if (operation_) m += std::string(" · ") + operation_name(operation_->kind);
  s.set("mode", m);
  s.set("coords", coord_status_);
  s.set("message", status_message_);
  s.set("revision", static_cast<f64>(ui_revision_));
  s.set("logCount", static_cast<f64>(log_.size()));
  if (active_) s.set("zoom", "Perspective · " + std::to_string(static_cast<int>(std::lround(active_->camera.fov_degrees))) + "°");
  json::Value views = json::Value::array();
  for (const auto& vp : views_) {
    const View& v = *vp;
    json::Value o = json::Value::object();
    o.set("id", v.id);
    std::string hint;
    if (v.type == ViewType::player) {
      hint = std::string(v.playing ? "Running" : "Paused") + " · " + std::to_string(v.runtime ? v.runtime->steps() : 0) + " steps · LMB grabs dynamic bodies";
    } else if (operation_) {
      hint = std::string(operation_name(operation_->kind)) + " " + (operation_->axis ? std::string(1, static_cast<char>(std::toupper(operation_->axis))) : "") + " " +
             operation_->typed + " · move mouse or type · Enter confirms · Esc cancels";
    } else {
      hint = "RMB orbit · MMB pan · wheel zoom";
    }
    o.set("hint", hint);
    o.set("playing", v.playing);
    if (v.runtime) o.set("grabbing", v.runtime->grabbing());
    if (v.box.active) {
      json::Value b = json::Value::object();
      b.set("x", min_of(v.box.x0, v.box.x1));
      b.set("y", min_of(v.box.y0, v.box.y1));
      b.set("w", std::fabs(v.box.x1 - v.box.x0));
      b.set("h", std::fabs(v.box.y1 - v.box.y0));
      o.set("box", std::move(b));
    }
    if (options_.labels && (tiled_ || &v == active_)) {
      const Document* d = document(v.doc);
      const world::Scene& sc = v.type == ViewType::player && v.runtime ? v.runtime->source() : d->scene;
      json::Value labels = json::Value::array();
      u32 n = 0;
      for (const Body& b : sc.bodies) {
        if (++n > 300) break;
        f32 x, y;
        if (!screen_point(v, display_pose(v, b).position, x, y) || x < 0 || y < 0 || x > v.css_width || y > v.css_height) continue;
        json::Value l = json::Value::object();
        l.set("x", x);
        l.set("y", y);
        l.set("text", b.name);
        labels.push(std::move(l));
      }
      o.set("labels", std::move(labels));
    }
    json::Value hud = game_hud(v);
    if (!hud.is_null()) o.set("hud", std::move(hud));
    views.push(std::move(o));
  }
  s.set("views", std::move(views));
  return s;
}

// ---------------------------------------------------------------------------
// menus

json::Value Editor::menu_json(std::string_view name) {
  json::Value e = json::Value::array();
  if (name == "actions") {
    json::Value body = json::Value::array();
    body.push(entry("Static body", cmd("addBody", "type", 0)));
    body.push(entry("Kinematic body", cmd("addBody", "type", 1)));
    body.push(entry("Dynamic body", cmd("addBody", "type", 2)));
    body.push(separator());
    for (const char* sh : {"box", "sphere", "capsule"}) {
      json::Value c = cmd("addBody", "type", 2);
      c.set("shape", sh);
      body.push(entry(std::string("Dynamic ") + sh, std::move(c)));
    }
    json::Value sb = cmd("addBody", "type", 0);
    sb.set("shape", "box");
    body.push(entry("Static box", std::move(sb)));
    json::Value fixture = json::Value::array();
    for (const char* sh : {"box", "sphere", "capsule", "hull"}) fixture.push(entry(capitalize(sh), cmd("addFixture", "shape", sh)));
    json::Value joint = json::Value::array();
    for (u32 k = 0; k < kJointKindCount; ++k) {
      const char* jn = joint_kind_name(static_cast<JointKind>(k));
      joint.push(entry(capitalize(jn), cmd("addJoint", "type", jn)));
    }
    json::Value add = json::Value::array();
    add.push(submenu("Body", std::move(body)));
    add.push(submenu("Fixture", std::move(fixture)));
    add.push(submenu("Joint", std::move(joint)));
    add.push(entry("Image…", cmd("openImage")));
    add.push(entry("Sampler volume", cmd("addSampler")));
    e.push(submenu("Add", std::move(add)));
    e.push(separator());
    json::Value b = json::Value::array();
    b.push(entry("Make static", cmd("makeStatic")));
    b.push(entry("Make dynamic", cmd("makeDynamic")));
    b.push(entry("Make chain…", cmd("dialog", "name", "chain")));
    e.push(submenu("Body", std::move(b)));
    json::Value vtx = json::Value::array();
    vtx.push(entry("Add hull vertex at cursor", cmd("addHullVertex"), "E"));
    vtx.push(entry("Weld together", cmd("weld")));
    e.push(submenu("Vertex", std::move(vtx)));
    json::Value img = json::Value::array();
    img.push(entry("Add image…", cmd("openImage")));
    img.push(entry("Create fixtures from alpha…", cmd("traceImage")));
    e.push(submenu("Image", std::move(img)));
    json::Value smp = json::Value::array();
    smp.push(entry("Save sampled PNG", cmd("samplerAction", "action", "png")));
    smp.push(entry("Sobel edge output", cmd("samplerAction", "action", "sobel")));
    smp.push(entry("Local maxima output", cmd("samplerAction", "action", "maxima")));
    smp.push(entry("Create fixtures from alpha", cmd("samplerAction", "action", "trace")));
    smp.push(entry("Box fixture from volume", cmd("samplerToFixture")));
    e.push(submenu("Sampler", std::move(smp)));
    e.push(separator());
    e.push(entry("Duplicate", cmd("duplicate"), "Shift+D"));
    e.push(entry("Delete", cmd("delete"), "Delete"));
    e.push(entry("Cursor to selection", cmd("cursorToSelection"), "C, S"));
    e.push(entry("Validate scene", cmd("dialog", "name", "validation")));
    return e;
  }
  if (name == "File") {
    e.push(entry("New scene", cmd("newScene"), "Ctrl+N"));
    e.push(entry("Open scene…", cmd("openScene"), "Ctrl+O"));
    e.push(entry("Save scene", cmd("saveScene"), "Ctrl+S"));
    e.push(entry("Save scene as…", cmd("dialog", "name", "saveAs"), "Ctrl+Shift+S"));
    e.push(separator());
    e.push(entry("Export JSON…", cmd("exportJson")));
    e.push(entry("Export view as PNG", cmd("exportView")));
    e.push(entry("Export game (HTML)…", cmd("exportGame")));
    e.push(separator());
    e.push(entry("Add image…", cmd("openImage")));
    json::Value ex = json::Value::array();
    ex.push(entry("Suspension & vehicle", cmd("loadDemo", "kind", "mechanism")));
    ex.push(entry("Newton cradle / pendulum", cmd("loadDemo", "kind", "cradle")));
    ex.push(entry("3D hulls & stacking", cmd("loadDemo", "kind", "hulls")));
    ex.push(entry("Joint laboratory", cmd("loadDemo", "kind", "joints")));
    ex.push(entry("Game: Roll & Collect", cmd("loadDemo", "kind", "game")));
    e.push(submenu("Examples", std::move(ex)));
    e.push(entry("Close view", cmd("closeView")));
    return e;
  }
  if (name == "Edit") {
    e.push(entry("Undo", cmd("undo"), "Ctrl+Z"));
    e.push(entry("Redo", cmd("redo"), "Ctrl+Shift+Z"));
    e.push(separator());
    e.push(entry("Copy", cmd("copy"), "Ctrl+C"));
    e.push(entry("Paste", cmd("paste"), "Ctrl+V"));
    e.push(entry("Duplicate", cmd("duplicate"), "Shift+D"));
    e.push(entry("Delete", cmd("delete"), "Delete"));
    e.push(separator());
    e.push(entry("Select all", cmd("selectAll"), "Ctrl+A"));
    e.push(entry("Deselect all", cmd("deselectAll"), "Ctrl+Shift+A"));
    return e;
  }
  if (name == "View") {
    e.push(entry("Fit scene", cmd("fitScene"), "Home"));
    e.push(entry("Fit selection", cmd("fitSelection")));
    e.push(separator());
    const std::pair<const char*, bool> toggles[] = {{"grid", options_.grid},   {"labels", options_.labels},       {"shapes", options_.shapes},
                                                    {"joints", options_.joints}, {"images", options_.images},       {"centers", options_.centers},
                                                    {"wireframe", options_.wireframe}, {"fixtureColors", options_.fixture_colors}};
    for (const auto& [key, on] : toggles) {
      const std::string label = std::string(key) == "fixtureColors" ? "Fixture colours" : capitalize(key);
      e.push(entry(std::string(on ? "✓ " : "   ") + label, cmd("toggleOption", "name", key)));
    }
    return e;
  }
  if (name == "Scene") {
    e.push(submenu("Actions", menu_json("actions"), "Space"));
    e.push(separator());
    const char* keys[] = {"B", "F", "V", "J", "I", "M", "W"};
    for (u32 i = 0; i < kModeCount; ++i)
      e.push(entry(std::string(mode_label(static_cast<Mode>(i))) + " edit", cmd("setMode", "mode", mode_name(static_cast<Mode>(i))), keys[i]));
    e.push(separator());
    e.push(entry("Translate", cmd("beginOperation", "kind", "translate"), "T / G"));
    e.push(entry("Rotate", cmd("beginOperation", "kind", "rotate"), "R"));
    e.push(entry("Scale", cmd("beginOperation", "kind", "scale"), "S"));
    e.push(entry("Move cursor", cmd("beginOperation", "kind", "cursor"), "C"));
    e.push(entry("Set cursor…", cmd("dialog", "name", "cursor")));
    e.push(entry("Edit joint limits", cmd("beginOperation", "kind", "limit"), "L"));
    return e;
  }
  if (name == "Window") {
    e.push(entry("Open player view", cmd("openPlayer"), "Ctrl+R"));
    e.push(entry("Clone view", cmd("cloneView"), "Ctrl+Shift+C"));
    e.push(entry("Tile views", cmd("toggleTile"), "Ctrl+T"));
    e.push(separator());
    json::Value panels = json::Value::array();
    const char* ids[][2] = {{"itemsPanel", "Items"}, {"propertiesPanel", "Properties"}, {"contextPanel", "Editor context help"},
                            {"helpPanel", "Help"},   {"undoPanel", "Undo stack"},        {"logPanel", "Message log"}};
    for (const auto& p : ids) {
      json::Value c = cmd("ui", "action", "togglePanel");
      c.set("id", p[0]);
      json::Value en = entry(p[1], std::move(c));
      en.set("panel", p[0]);
      panels.push(std::move(en));
    }
    e.push(submenu("Dockbars", std::move(panels)));
    e.push(entry("Hide / show all panels", cmd("ui", "action", "togglePanels"), "F11"));
    e.push(entry("Reset panel layout", cmd("ui", "action", "resetPanels")));
    return e;
  }
  if (name == "Tools") {
    e.push(entry("Validate scene", cmd("dialog", "name", "validation")));
    e.push(entry("Collision settings…", cmd("dialog", "name", "collision")));
    e.push(entry("Custom properties…", cmd("dialog", "name", "custom")));
    e.push(entry("Options…", cmd("dialog", "name", "options")));
    return e;
  }
  if (name == "Help") {
    e.push(entry("Keyboard reference", cmd("dialog", "name", "keyboard"), "F1"));
    e.push(entry("About / format support", cmd("dialog", "name", "about")));
    json::Value u = cmd("ui", "action", "openUrl");
    u.set("url", "https://github.com/erincatto/box3d");
    e.push(entry("Box3D repository ↗", std::move(u)));
    return e;
  }
  return e;
}

// ---------------------------------------------------------------------------
// dialogs

json::Value Editor::dialog_json(std::string_view name) {
  json::Value d = json::Value::object();
  d.set("name", name);
  json::Value body = json::Value::array(), buttons = json::Value::array();
  world::Scene* s = scene();
  if (name == "chain") {
    d.set("title", "Make chain");
    json::Value count = input_field("count", "Copies", "number", 8);
    count.set("min", 2);
    count.set("max", 100);
    body.push(std::move(count));
    body.push(input_field("x", "Offset X", "number", 1));
    body.push(input_field("y", "Offset Y", "number", 0));
    body.push(input_field("z", "Offset Z", "number", 0));
    json::Value joint = input_field("joint", "Joint", "select", "revolute");
    json::Value opts = json::Value::array();
    for (const char* j : {"revolute", "distance", "spherical"}) {
      json::Value o = json::Value::array();
      o.push(j);
      o.push(j);
      opts.push(std::move(o));
    }
    joint.set("options", std::move(opts));
    body.push(std::move(joint));
    buttons.push(button("Cancel", "close"));
    buttons.push(button("Create", "submit"));
  } else if (name == "cursor") {
    d.set("title", "Set 3D cursor");
    const D3 c = s ? s->cursor : D3{};
    body.push(input_field("x", "X", "number", c[0]));
    body.push(input_field("y", "Y", "number", c[1]));
    body.push(input_field("z", "Z", "number", c[2]));
    buttons.push(button("Cancel", "close"));
    buttons.push(button("OK", "submit"));
  } else if (name == "saveAs") {
    d.set("title", "Save scene as");
    body.push(input_field("name", "Name", "text", s ? s->name : std::string("Untitled")));
    buttons.push(button("Cancel", "close"));
    buttons.push(button("Save", "submit"));
  } else if (name == "options") {
    d.set("title", "Options — controls");
    body.push(input_field("snap", "Translation step (Ctrl)", "number", options_.snap));
    body.push(input_field("angle", "Rotation step (degrees)", "number", options_.angle_snap));
    body.push(input_field("scale", "Scale step", "number", options_.scale_snap));
    body.push(input_field("reverse", "Reverse zoom", "checkbox", options_.reverse_zoom));
    body.push(input_field("pivotCursor", "Pivot at 3D cursor", "checkbox", options_.pivot_cursor));
    body.push(input_field("degrees", "Angles in degrees", "checkbox", options_.degrees));
    buttons.push(button("Cancel", "close"));
    buttons.push(button("Apply", "submit"));
  } else if (name == "custom") {
    d.set("title", "Custom properties");
    json::Value values = json::Value::array();
    std::size_t count = 0;
    if (mode_ == Mode::world && s) {
      count = 1;
      if (s->world.custom) values = *s->world.custom;
    } else if (s) {
      const std::vector<ItemRef> items = chosen(mode_);
      count = items.size();
      if (items.empty()) return {};
      const std::string& id = items[0].id;
      if (const Body* b = s->body(id)) values = b->custom;
      else if (const Fixture* f = s->fixture(id)) values = f->custom;
      else if (const Joint* j = s->joint(id)) values = j->custom;
      else if (const ImagePlane* i = s->image(id)) values = i->custom;
      else if (const Sampler* i = s->sampler(id)) values = i->custom;
    }
    body.push(para("Applied to " + std::to_string(count) + " selected item(s). Types are preserved in .e3d."));
    json::Value props = block("properties");
    props.set("id", "props");
    json::Value rows = json::Value::array();
    if (values.is_array())
      for (const json::Value& p : values.items()) {
        if (!p.is_object()) continue;
        json::Value r = json::Value::object();
        r.set("name", p["name"].is_string() ? p["name"].as_string() : std::string("property"));
        std::string type = "string";
        for (const auto& [k, v] : p.members()) if (k != "name") { type = k; break; }
        r.set("type", type);
        const json::Value& v = p[type];
        r.set("value", v.is_string() ? v.as_string() : json::stringify(v));
        rows.push(std::move(r));
      }
    props.set("value", std::move(rows));
    body.push(std::move(props));
    body.push(para("<span class=\"muted\">vec3: [1,2,3] · color: [255,128,0,255] · bool: true / false</span>"));
    buttons.push(button("Cancel", "close"));
    buttons.push(button("Apply", "submit"));
  } else if (name == "collision") {
    const std::vector<ItemRef> fs = chosen(Mode::fixture);
    if (fs.empty() || !s) return {};
    const Fixture* f = s->fixture(fs[0].id);
    d.set("title", "Collision settings");
    body.push(para("Equal positive groups force collision; equal negative groups suppress it. Otherwise category/mask bits are checked."));
    json::Value g = input_field("group", "Group index", "number", f->group_index);
    g.set("min", -32768);
    g.set("max", 32767);
    body.push(std::move(g));
    json::Value bits = block("bits");
    bits.set("id", "bits");
    bits.set("category", f->category_bits);
    bits.set("mask", f->mask_bits);
    body.push(std::move(bits));
    buttons.push(button("Cancel", "close"));
    buttons.push(button("Apply", "submit"));
  } else if (name == "validation") {
    d.set("title", "Scene validation");
    const std::vector<std::string> errors = validate_scene();
    if (!errors.empty()) {
      body.push(para("<b>Fix these before running:</b>"));
      json::Value list = block("list");
      json::Value items = json::Value::array();
      for (const std::string& e : errors) items.push(e);
      list.set("items", std::move(items));
      body.push(std::move(list));
    } else if (s) {
      body.push(para("✓ IDs, shapes, bodies and joint references are valid."));
      body.push(para(std::to_string(s->bodies.size()) + " bodies, " + std::to_string(s->fixture_count()) + " fixtures, " + std::to_string(s->joints.size()) + " joints."));
    }
    buttons.push(button("Close", "close"));
  } else if (name == "keyboard") {
    d.set("title", "Keyboard reference");
    body.push(para("The Editor3D desktop workflow, extended with Z and game controls."));
    json::Value table = block("table");
    json::Value rows = json::Value::array();
    for (const auto& r : kShortcutRows) {
      json::Value row = json::Value::array();
      row.push(r[0]);
      row.push(r[1]);
      rows.push(std::move(row));
    }
    table.set("rows", std::move(rows));
    table.set("keyColumn", true);
    body.push(std::move(table));
    buttons.push(button("Close", "close"));
  } else if (name == "about") {
    d.set("title", "About Kern Editor");
    const physics::Version v = physics::version();
    body.push(para("<h3>Kern Editor · Box3D " + std::to_string(v.major) + "." + std::to_string(v.minor) + "." + std::to_string(v.revision) + " · C++/WebAssembly</h3>"));
    body.push(para("The Editor3D R.U.B.E-style workflow running on the Kern engine: scene model, undo, transforms, validation, "
                   "the Box3D player, picking, image tracing and drawing are C++ compiled to WebAssembly. The page only draws panels."));
    body.push(para("<b>Editable format:</b> Editor3D .e3d v2 (reads v1 and the legacy object list; game components are optional extra keys).<br>"
                   "<b>Physics:</b> Erin Catto Box3D, compiled into the engine module (no JavaScript bindings).<br>"
                   "<b>Rendering:</b> Kern renderer on WebGL2 (one canvas, one viewport per view)."));
    body.push(para("The running player is independent from the editor state. Dynamic bodies can be grabbed with a real Box3D MotorJoint. "
                   "Scenes with a player body run as games; File → Export game writes a standalone HTML game."));
    buttons.push(button("Close", "close"));
  } else if (name == "closeScene") {
    d.set("title", "Close scene?");
    body.push(para("Save <b>" + html_escape(s ? s->name : std::string()) + "</b> before closing?"));
    buttons.push(button("Cancel", "close"));
    buttons.push(button("Save and close", "submit"));
    json::Value discard = button("Discard", "submit");
    discard.set("value", "discard");
    buttons.push(std::move(discard));
  } else if (name == "rename") {
    d.set("title", "Rename item");
    body.push(input_field("name", "Name", "text", ""));
    buttons.push(button("Cancel", "close"));
    buttons.push(button("OK", "submit"));
  } else {
    return {};
  }
  d.set("body", std::move(body));
  d.set("buttons", std::move(buttons));
  return d;
}

json::Value Editor::submit_dialog(std::string_view name, const json::Value& values) {
  auto num = [&](const char* key, f64& out) {
    const json::Value& v = values[key];
    out = v.is_number() ? v.as_number() : js_number(v.as_string());
    return std::isfinite(out);
  };
  if (name == "chain") {
    f64 n, x, y, z;
    if (!num("count", n) || n != std::floor(n) || n < 2 || n > 100 || !num("x", x) || !num("y", y) || !num("z", z)) return error_result("Invalid chain settings");
    JointKind type;
    if (!parse_joint_kind(values["joint"].as_string(), type)) return error_result("Invalid chain settings");
    make_chain(static_cast<i32>(n), {x, y, z}, type);
    return ok_result();
  }
  if (name == "cursor") {
    f64 x, y, z;
    if (!num("x", x) || !num("y", y) || !num("z", z)) return error_result("Invalid position");
    set_cursor({x, y, z});
    return ok_result();
  }
  if (name == "saveAs") {
    std::string n = values["name"].as_string();
    if (n.empty()) n = "Untitled";
    scene()->name = n;
    touch();
    return command(cmd("saveScene"));
  }
  if (name == "options") {
    f64 snap, angle, scale;
    if (!num("snap", snap) || !num("angle", angle) || !num("scale", scale) || !(snap > 0) || !(angle > 0) || !(scale > 0))
      return error_result("Step sizes must be positive");
    options_.snap = snap;
    options_.angle_snap = angle;
    options_.scale_snap = scale;
    options_.reverse_zoom = values["reverse"].truthy();
    options_.pivot_cursor = values["pivotCursor"].truthy();
    options_.degrees = values["degrees"].truthy();
    touch();
    return ok_result();
  }
  if (name == "custom") {
    json::Value props = json::Value::array();
    for (const json::Value& row : values["props"].items()) {
      const std::string type = row["type"].as_string(), str = row["value"].as_string();
      const std::string pname = row["name"].as_string().empty() ? "property" : row["name"].as_string();
      json::Value v;
      if (type == "int" || type == "float") {
        const f64 n = js_number(str);
        if (!std::isfinite(n) || (type == "int" && n != std::floor(n))) return error_result("Invalid " + type);
        v = n;
      } else if (type == "bool") {
        if (str != "true" && str != "false") return error_result("Boolean must be true or false");
        v = str == "true";
      } else if (type == "vec3" || type == "color") {
        if (!json::parse(str, v)) return error_result(type == "vec3" ? "vec3 requires [x,y,z]" : "color requires [r,g,b,a], each 0–255");
        bool ok = v.is_array() && v.size() == (type == "vec3" ? 3u : 4u);
        for (std::size_t k = 0; ok && k < v.size(); ++k) {
          ok = v[k].is_number();
          if (ok && type == "color") ok = v[k].as_number() == std::floor(v[k].as_number()) && v[k].as_number() >= 0 && v[k].as_number() <= 255;
        }
        if (!ok) return error_result(type == "vec3" ? "vec3 requires [x,y,z]" : "color requires [r,g,b,a], each 0–255");
      } else if (type == "string") {
        v = str;
      } else {
        return error_result("Unknown property type " + type);
      }
      json::Value p = json::Value::object();
      p.set("name", pname);
      p.set(type, std::move(v));
      props.push(std::move(p));
    }
    edit("Set custom properties", [&] {
      world::Scene& s = *scene();
      if (mode_ == Mode::world) { s.world.custom = props; return true; }
      for (const ItemRef& item : chosen(mode_)) {
        if (Body* b = s.body(item.id)) b->custom = props;
        else if (Fixture* f = s.fixture(item.id)) f->custom = props;
        else if (Joint* j = s.joint(item.id)) j->custom = props;
        else if (ImagePlane* i = s.image(item.id)) i->custom = props;
        else if (Sampler* i = s.sampler(item.id)) i->custom = props;
      }
      return true;
    });
    return ok_result();
  }
  if (name == "collision") {
    f64 group, category, mask;
    if (!num("group", group) || group != std::floor(group) || group < -32768 || group > 32767) return error_result("Invalid group index");
    const json::Value& bits = values["bits"];
    category = bits["category"].as_number(1);
    mask = bits["mask"].as_number(4294967295.0);
    edit("Set collision filter", [&] {
      for (const ItemRef& item : chosen(Mode::fixture))
        if (Fixture* f = scene()->fixture(item.id)) {
          f->category_bits = category;
          f->mask_bits = mask;
          f->group_index = group;
        }
      return true;
    });
    return ok_result();
  }
  if (name == "closeScene") {
    View* v = view(static_cast<u32>(values["view"].as_number(0)));
    if (!v) v = active_;
    if (!v) return ok_result();
    if (values["button"].as_string() == "discard") { remove_view(v); return ok_result(); }
    activate(v);
    json::Value r = command(cmd("saveScene"));
    remove_view(v);
    return r;
  }
  if (name == "rename") {
    rename(values["id"].as_string(), values["name"].as_string());
    return ok_result();
  }
  return error_result("unknown dialog");
}

// ---------------------------------------------------------------------------
// keyboard

void Editor::game_keys(View& v, std::string_view code, bool down) {
  world::GameInput& in = v.game_input;
  const f32 k = down ? 1.0f : 0.0f;
  if (code == "KeyW" || code == "ArrowUp") in.move_y = k ? 1 : (in.move_y > 0 ? 0 : in.move_y);
  else if (code == "KeyS" || code == "ArrowDown") in.move_y = k ? -1 : (in.move_y < 0 ? 0 : in.move_y);
  else if (code == "KeyA" || code == "ArrowLeft") in.move_x = k ? -1 : (in.move_x < 0 ? 0 : in.move_x);
  else if (code == "KeyD" || code == "ArrowRight") in.move_x = k ? 1 : (in.move_x > 0 ? 0 : in.move_x);
  else if (code == "Space") { in.jump = down; in.up = k ? 1 : (in.up > 0 ? 0 : in.up); }
  else if (code == "ShiftLeft" || code == "ShiftRight" || code == "KeyQ") in.up = k ? -1 : (in.up < 0 ? 0 : in.up);
  else if (code == "KeyE") in.up = k ? 1 : (in.up > 0 ? 0 : in.up);
}

json::Value Editor::key(std::string_view key_in, std::string_view code, bool down, bool shift, bool ctrl, bool alt, bool repeat) {
  json::Value r = json::Value::object();
  r.set("handled", false);
  (void)alt;
  std::string key(key_in);
  for (char& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  auto handled = [&]() { r.set("handled", true); return r; };
  auto run = [&](json::Value c) {
    json::Value res = command(c);
    if (res.has("ui")) r.set("ui", res["ui"]);
    return handled();
  };
  View* v = active_;
  const bool game = v && v->type == ViewType::player && v->runtime && v->runtime->game().status != world::GameStatus::inactive;
  if (!down) {
    if (game) game_keys(*v, code, false);
    return r;
  }
  if (ctrl) {
    if (key == "z") return run(cmd(shift ? "redo" : "undo"));
    if (key == "y") return run(cmd("redo"));
    if (key == "s") return run(shift ? cmd("dialog", "name", "saveAs") : cmd("saveScene"));
    if (key == "o") return run(cmd("openScene"));
    if (key == "n") return run(cmd("newScene"));
    if (key == "r") return run(cmd(v && v->type == ViewType::player ? "reloadPlayer" : "openPlayer"));
    if (key == "a") return run(cmd(shift ? "deselectAll" : "selectAll"));
    if (key == "c") return run(cmd(shift ? "cloneView" : "copy"));
    if (key == "v") return run(cmd("paste"));
    if (key == "t") return run(cmd("toggleTile"));
  }
  if (key == "f11") return run(cmd("ui", "action", "togglePanels"));
  if (key == "f1") return run(cmd("dialog", "name", "keyboard"));
  if (key == "home") return run(cmd("fitScene"));
  if (key == "escape") {
    if (operation_) finish_operation(false);
    for (auto& w : views_) end_grab(*w);
    json::Value a = ui("closeMenu");
    json::Value list = json::Value::array();
    list.push(std::move(a));
    r.set("ui", std::move(list));
    touch();
    return handled();
  }
  if (v && v->type == ViewType::player) {
    if (game) {
      if (code == "KeyR" && !repeat) { v->runtime->restart(); v->needs_sync = true; touch(); return handled(); }
      if (code == "KeyP" && !repeat) { v->playing = !v->playing; v->last_time = -1; touch(); return handled(); }
      game_keys(*v, code, true);
      return handled();
    }
    if (key == " ") {
      if (v->playing) { v->playing = false; end_grab(*v); touch(); }
      else step_player(v);
      return handled();
    }
    return r;
  }
  if (operation_) {
    if (key == "enter") { finish_operation(true); return handled(); }
    if (key == "x" || key == "y" || key == "z") { set_operation_axis(key[0]); return handled(); }
    if ((key.size() == 1 && (std::isdigit(static_cast<unsigned char>(key[0])) || key == "." || key == "-")) || key == "backspace") {
      std::string t = operation_->typed;
      if (key == "backspace") { if (!t.empty()) t.pop_back(); }
      else t += key;
      type_operation(t);
      return handled();
    }
    if (operation_->kind == OperationKind::cursor && key == "s") {
      const Snapshot before = operation_->before;
      restore(before);
      scene()->cursor = center_of(*scene(), mode_);
      finish_operation(true);
      return handled();
    }
    if (operation_->kind == OperationKind::cursor && key == "c") {
      finish_operation(false);
      return run(cmd("dialog", "name", "cursor"));
    }
  }
  if (shift && key == "d") { duplicate(); return handled(); }
  if (key == "delete" || key == "backspace") { delete_selected(); return handled(); }
  if (key == " ") {
    json::Value a = ui("popup");
    a.set("menu", "actions");
    a.set("entries", menu_json("actions"));
    a.set("at", "pointer");
    json::Value list = json::Value::array();
    list.push(std::move(a));
    r.set("ui", std::move(list));
    return handled();
  }
  const std::pair<const char*, Mode> modes[] = {{"b", Mode::body}, {"f", Mode::fixture}, {"v", Mode::vertex}, {"j", Mode::joint},
                                                {"i", Mode::image}, {"m", Mode::sampler}, {"w", Mode::world}};
  if (!ctrl && !operation_)
    for (const auto& [k, m] : modes)
      if (key == k) { set_mode(m); return handled(); }
  if (key == "e" && mode_ == Mode::vertex) { add_hull_vertex(); return handled(); }
  if (!ctrl) {
    OperationKind kind;
    bool ok = true;
    if (key == "t" || key == "g") kind = OperationKind::translate;
    else if (key == "r") kind = OperationKind::rotate;
    else if (key == "s") kind = OperationKind::scale;
    else if (key == "c") kind = OperationKind::cursor;
    else if (key == "l" && mode_ == Mode::joint) kind = OperationKind::limit;
    else ok = false;
    if (ok) { begin_operation(kind); return handled(); }
  }
  return r;
}

void Editor::blur() {
  if (operation_) finish_operation(false);
  for (auto& v : views_) {
    end_grab(*v);
    v->game_input = {};
    v->drag_button = v->drag_pointer = -1;
    v->box.active = false;
    v->touches.clear();
  }
}

// ---------------------------------------------------------------------------
// commands

json::Value Editor::command(const json::Value& c) {
  const std::string name = c["cmd"].as_string();
  json::Value result = ok_result();
  auto view_arg = [&]() -> View* {
    if (c.has("view")) return view(static_cast<u32>(c["view"].as_number(0)));
    return active_;
  };
  auto push_ui = [&](json::Value a) {
    json::Value* list = result.find("ui");
    if (!list) list = &result.set("ui", json::Value::array());
    list->push(std::move(a));
  };
  auto download = [&](const std::string& file, const std::string& text, const char* type = "application/json") {
    json::Value a = ui("download");
    a.set("name", file);
    a.set("text", text);
    a.set("type", type);
    push_ui(std::move(a));
  };
  auto base_name = [&]() {
    std::string n = scene() ? scene()->name : std::string("scene");
    for (const char* ext : {".json", ".e3d"})
      if (n.size() > std::strlen(ext) && n.compare(n.size() - std::strlen(ext), std::strlen(ext), ext) == 0) n.resize(n.size() - std::strlen(ext));
    return n;
  };

  if (name == "newScene") add_document(new_scene());
  else if (name == "openScene") { json::Value a = ui("openFile"); a.set("accept", "scene"); push_ui(std::move(a)); }
  else if (name == "openImage") { json::Value a = ui("openFile"); a.set("accept", "image"); push_ui(std::move(a)); }
  else if (name == "saveScene") {
    if (!doc()) return error_result("no scene");
    download(base_name() + ".e3d", scene_text());
    doc()->dirty = false;
    report("Saved editable 3D scene: " + scene()->name);
  } else if (name == "exportJson") {
    const std::vector<std::string> e = validate_scene();
    if (!e.empty()) {
      std::string msg;
      for (const std::string& x : e) msg += (msg.empty() ? "" : "; ") + x;
      report(msg, "error");
      json::Value a = ui("dialog");
      a.set("dialog", dialog_json("validation"));
      push_ui(std::move(a));
    } else {
      download(base_name() + ".json", scene_text(true));
      report("Exported Editor3D Box3D JSON");
    }
  } else if (name == "exportView") {
    json::Value a = ui("exportView");
    a.set("view", active_ ? active_->id : 0);
    push_ui(std::move(a));
  } else if (name == "exportGame") {
    const std::vector<std::string> e = validate_scene();
    if (!e.empty()) {
      report(e.front(), "error");
      json::Value a = ui("dialog");
      a.set("dialog", dialog_json("validation"));
      push_ui(std::move(a));
    } else {
      json::Value a = ui("exportGame");
      a.set("name", base_name() + ".html");
      a.set("title", scene()->game.title.empty() ? scene()->name : scene()->game.title);
      a.set("scene", json::stringify(to_json(*scene())));
      push_ui(std::move(a));
      report("Exported standalone game: " + base_name() + ".html");
    }
  } else if (name == "loadDemo") load_demo(c["kind"].as_string().empty() ? "mechanism" : c["kind"].as_string(), c["reuse"].truthy());
  else if (name == "closeView") close_view(view_arg(), c["force"].truthy());
  else if (name == "undo") undo();
  else if (name == "redo") redo();
  else if (name == "copy") copy_selection();
  else if (name == "paste") paste();
  else if (name == "duplicate") duplicate(c["offset"].truthy());
  else if (name == "delete") delete_selected();
  else if (name == "selectAll") select_all(false);
  else if (name == "deselectAll") select_all(true);
  else if (name == "fitScene") fit_view();
  else if (name == "fitSelection") fit_view(nullptr, true);
  else if (name == "toggleOption") {
    const std::string o = c["name"].as_string();
    bool* flag = o == "grid" ? &options_.grid : o == "labels" ? &options_.labels : o == "shapes" ? &options_.shapes : o == "joints" ? &options_.joints
                 : o == "images" ? &options_.images : o == "centers" ? &options_.centers : o == "wireframe" ? &options_.wireframe
                 : o == "fixtureColors" ? &options_.fixture_colors : nullptr;
    if (!flag) return error_result("unknown option");
    *flag = !*flag;
    for (auto& v : views_) v->needs_sync = true;
    touch();
  } else if (name == "setMode") {
    Mode m;
    if (!parse_mode(c["mode"].as_string(), m)) return error_result("unknown mode");
    set_mode(m);
  } else if (name == "beginOperation") {
    const std::string k = c["kind"].as_string();
    OperationKind kind = k == "rotate" ? OperationKind::rotate : k == "scale" ? OperationKind::scale : k == "cursor" ? OperationKind::cursor
                         : k == "limit" ? OperationKind::limit : OperationKind::translate;
    if (kind == OperationKind::limit && mode_ != Mode::joint) set_mode(Mode::joint);
    begin_operation(kind);
  } else if (name == "finishOperation") finish_operation(c["accept"].truthy());
  else if (name == "typeOperation") type_operation(c["text"].as_string());
  else if (name == "setAxis") { const std::string a = c["axis"].as_string(); if (!a.empty()) set_operation_axis(a[0]); }
  else if (name == "addBody") add_body(static_cast<world::BodyKind>(static_cast<i32>(c["type"].as_number(2))), c["shape"].as_string());
  else if (name == "addFixture") {
    FixtureShape s;
    if (!parse_shape(c["shape"].as_string(), s)) return error_result("unknown shape");
    add_fixture(s);
  } else if (name == "addJoint") {
    JointKind t;
    if (!parse_joint_kind(c["type"].as_string(), t)) return error_result("unknown joint");
    add_joint(t);
  } else if (name == "addSampler") add_sampler();
  else if (name == "makeStatic" || name == "makeDynamic") {
    const BodyKind k = name == "makeStatic" ? BodyKind::fixed : BodyKind::dynamic;
    const std::vector<ItemRef> bodies = chosen(Mode::body);
    edit(name == "makeStatic" ? "Make static" : "Make dynamic", [&] {
      for (const ItemRef& b : bodies) if (Body* body = scene()->body(b.id)) body->type = k;
      return true;
    });
  } else if (name == "addHullVertex") add_hull_vertex();
  else if (name == "weld") weld_vertices();
  else if (name == "traceImage") trace_image_to_boxes();
  else if (name == "samplerAction") {
    const std::string action = c["action"].as_string();
    if (action == "trace") sampler_action("trace");
    else {
      const std::vector<ItemRef> smp = chosen(Mode::sampler);
      std::vector<u8> px;
      i32 w = 0, h = 0;
      if (sampler_action(action, &px, &w, &h)) {
        last_pixels_ = std::move(px);
        json::Value a = ui("downloadPixels");
        std::string file = action + ".png";
        if (action == "png" && !smp.empty())
          if (const Sampler* s = scene()->sampler(smp[0].id)) file = s->file.empty() ? "sample.png" : s->file;
        a.set("name", file);
        a.set("width", w);
        a.set("height", h);
        push_ui(std::move(a));
      }
    }
  } else if (name == "samplerToFixture") sampler_to_fixture();
  else if (name == "cursorToSelection") cursor_to_selection();
  else if (name == "setCursor") set_cursor({c["x"].as_number(NAN), c["y"].as_number(NAN), c["z"].as_number(NAN)});
  else if (name == "openPlayer") open_player();
  else if (name == "reloadPlayer") reload_player(view_arg());
  else if (name == "stepPlayer") step_player(view_arg());
  else if (name == "togglePlay") {
    View* v = view_arg();
    if (v && v->type == ViewType::player) {
      v->playing = !v->playing;
      if (!v->playing) end_grab(*v);
      v->last_time = -1;
      touch();
    }
  } else if (name == "setSpeed") {
    View* v = view_arg();
    const f64 sp = c["speed"].as_number(1);
    if (v && sp > 0 && sp <= 16) { v->speed = sp; touch(); }
  } else if (name == "cloneView") clone_view();
  else if (name == "toggleTile") { tiled_ = !tiled_; touch(); }
  else if (name == "activateView") { if (View* v = view_arg()) activate(v); }
  else if (name == "select") {
    std::vector<std::string> ids;
    for (const json::Value& id : c["ids"].items()) ids.push_back(id.as_string());
    Mode m = mode_;
    if (c.has("mode") && !parse_mode(c["mode"].as_string(), m)) return error_result("unknown mode");
    if (m != mode_) set_mode(m);
    set_selection(ids, c["shift"].truthy(), c["toggle"].truthy());
  } else if (name == "treeToggle") {
    const std::string id = c["id"].as_string();
    auto it = std::find(collapsed_.begin(), collapsed_.end(), id);
    if (it != collapsed_.end()) collapsed_.erase(it);
    else collapsed_.push_back(id);
    touch();
  } else if (name == "setTreeFilter") {
    tree_filter_ = c["query"].as_string();
    tree_kind_ = c["kind"].as_string().empty() ? "all" : c["kind"].as_string();
    touch();
  } else if (name == "rename") rename(c["id"].as_string(), c["name"].as_string());
  else if (name == "setProperty") set_property(c["path"].as_string(), c["value"]);
  else if (name == "historyJump") jump_history(static_cast<i64>(c["index"].as_number(-1)));
  else if (name == "setTouchOrbit") { if (View* v = view_arg()) { v->touch_orbit = c["on"].truthy(); v->touches.clear(); touch(); } }
  else if (name == "loadDocument") {
    if (!load_document(c["text"].as_string(), c["name"].as_string())) return error_result("could not open " + c["name"].as_string());
  }
  else if (name == "addImageData") add_image(c["name"].as_string(), c["src"].as_string(), static_cast<i32>(c["width"].as_number(0)), static_cast<i32>(c["height"].as_number(0)));
  else if (name == "restoreWorkspace") {
    json::Value docs;
    std::string err;
    if (!json::parse(c["text"].as_string(), docs, &err) || !docs.is_array() || docs.size() == 0 || docs.size() >= 30) {
      report("Could not restore autosave: " + (err.empty() ? std::string("unexpected content") : err), "warn");
      return error_result("restore failed");
    }
    u32 restored = 0;
    for (const json::Value& d : docs.items()) {
      world::Scene s;
      if (world::from_json(d, s, &err, d["name"].as_string())) { add_document(std::move(s)); ++restored; }
    }
    if (!restored) return error_result("restore failed");
    for (auto& d : documents_) d->dirty = false;
    save_due_ = -1;
    report("Restored locally saved workspace");
  } else if (name == "report") report(c["message"].as_string(), c["type"].as_string().empty() ? "info" : c["type"].as_string());
  else if (name == "clearLog") { log_.clear(); touch(); }
  else if (name == "menu") result.set("entries", menu_json(c["name"].as_string()));
  else if (name == "dialog") {
    json::Value d = dialog_json(c["name"].as_string());
    if (d.is_null()) return ok_result();
    if (c["name"].as_string() == "rename") {
      const std::string id = c["id"].as_string();
      d.set("id", id);
      std::string current;
      if (world::Scene* s = scene()) {
        if (const Body* b = s->body(id)) current = b->name;
        else if (const Fixture* f = s->fixture(id)) current = f->name;
        else if (const Joint* j = s->joint(id)) current = j->name;
        else if (const ImagePlane* i = s->image(id)) current = i->name;
        else if (const Sampler* i = s->sampler(id)) current = i->name;
      }
      if (json::Value* b = d.find("body"); b && b->size()) b->items()[0].set("value", current);
    }
    json::Value a = ui("dialog");
    a.set("dialog", std::move(d));
    push_ui(std::move(a));
  } else if (name == "dialogSubmit") {
    json::Value values = c["values"];
    if (c.has("button")) values.set("button", c["button"]);
    if (c.has("dialogView")) values.set("view", c["dialogView"]);
    if (c.has("dialogId")) values.set("id", c["dialogId"]);
    json::Value r = submit_dialog(c["name"].as_string(), values);
    if (!r["ok"].truthy()) return r;
    if (r.has("ui")) for (const json::Value& a : r["ui"].items()) push_ui(a);
  } else if (name == "ui") {
    json::Value a = ui(c["action"].as_string().c_str());
    for (const auto& [k, v] : c.members()) if (k != "cmd" && k != "action") a.set(k, v);
    push_ui(std::move(a));
  } else if (name == "gameInput") {
    // Touch joystick / on-screen buttons of the player host. Fields that are
    // absent keep their current value; values are clamped to -1..1.
    View* v = view_arg();
    if (!v || v->type != ViewType::player) return error_result("no player view");
    auto axis = [&](const char* k, f32& out) {
      if (c.has(k)) out = static_cast<f32>(std::clamp(c[k].as_number(0), -1.0, 1.0));
    };
    axis("moveX", v->game_input.move_x);
    axis("moveY", v->game_input.move_y);
    axis("up", v->game_input.up);
    if (c.has("jump")) v->game_input.jump = c["jump"].truthy();
  } else if (name == "restartGame") {
    View* v = view_arg();
    if (!v || v->type != ViewType::player || !v->runtime) return error_result("no player view");
    v->runtime->restart();
    v->needs_sync = true;
    v->playing = true;
    v->last_time = -1;
    touch();
  } else if (name == "fitNewViews") {
    for (auto& v : views_) v->needs_fit = true;
  } else {
    return error_result("unknown command " + name);
  }
  // Queued UI requests (dialogs from deeper calls, clipboard text) go out with this result.
  for (UiAction& a : pending_ui_) push_ui(std::move(a.data));
  pending_ui_.clear();
  return result;
}

}  // namespace kern::editor
