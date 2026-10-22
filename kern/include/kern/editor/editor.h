// The Kern editor: a R.U.B.E-style 3D scene editor for Box3D scenes (the
// Editor3D workflow) running entirely in C++. The browser page only renders
// DOM widgets (menus, panels, dialogs) from the JSON this class produces and
// forwards input; documents, selection, undo, transforms, validation, the
// physics player, picking, image tracing and drawing all happen here.
//
// Link with `kern/build.sh --editor`. The C ABI for the page is in
// kern/src/editor/editor_api.cpp, the page itself in kern/web/editor/.
#pragma once

#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "kern/core/json.h"
#include "kern/gfx/renderer.h"
#include "kern/world/runtime.h"
#include "kern/world/scene.h"

namespace kern::editor {

using world::D3;
using world::DQ;
using world::DPose;

enum class Mode : u8 { body, fixture, vertex, joint, image, sampler, world };
constexpr u32 kModeCount = 7;
const char* mode_name(Mode m);   // "body", "fixture", ...
const char* mode_label(Mode m);  // "Body", "Fixture", ...
bool parse_mode(std::string_view s, Mode& out);

struct Selection {
  std::vector<std::string> ids[kModeCount];
  std::vector<std::string>& operator[](Mode m) { return ids[static_cast<u32>(m)]; }
  const std::vector<std::string>& operator[](Mode m) const { return ids[static_cast<u32>(m)]; }
  bool operator==(const Selection& o) const;
};

struct Snapshot {
  world::Scene scene;
  Selection selection;
};

struct HistoryEntry {
  std::string label;
  Snapshot before, after;
};

struct Document {
  u32 id = 0;
  world::Scene scene;
  Selection selection;
  std::vector<HistoryEntry> history;
  std::size_t index = 0;
  bool dirty = false;
  u64 rev = 1;
};

/// Orbit camera with damping (the behaviour of three.js OrbitControls, Z up).
struct OrbitCamera {
  Vec3 target{0, 0, 1};
  f32 theta = 0, phi = 1, radius = 15;  // azimuth around Z, polar angle from +Z, distance
  f32 fov_degrees = 48, near = 0.03f, far = 500;
  f32 pending_theta = 0, pending_phi = 0, pending_scale = 1;
  Vec3 pending_pan;
  bool damping = true;
  Vec3 eye() const;
  void set_eye(Vec3 eye);
  void rotate(f32 dx_pixels, f32 dy_pixels, f32 height_pixels);
  void pan(f32 dx_pixels, f32 dy_pixels, f32 height_pixels);
  void zoom(f32 scale);  // > 1 moves away
  /// Applies pending motion (damped); returns true while the camera still moves.
  bool update();
  Camera camera() const;
};

enum class ViewType : u8 { editor, player };

struct ViewMesh {
  u32 mesh = 0;
  Mode mode = Mode::body;
  std::string id, body, fixture;  // item id, owning body, owning fixture (vertex handles)
  // Player views re-pose these each frame from the runtime:
  bool follows_body = false;
  D3 center{};
  DQ rotation = world::kIdentityQ;
  D3 scale{1, 1, 1};
  bool visible = true;
};

struct SelectBox {
  bool active = false;
  f32 x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  i32 pointer = -1;
  bool shift = false, toggle = false;
};

struct View {
  u32 id = 0;
  u32 doc = 0;
  ViewType type = ViewType::editor;
  u32 layer = 0;          // renderer layer bit index (0..30)
  bool playing = false;
  f64 speed = 1;
  f64 last_time = -1;
  OrbitCamera camera;
  Viewport rect;          // drawing-buffer pixels (origin bottom-left, GL convention)
  f32 css_width = 1, css_height = 1, dpr = 1;
  bool visible = true;
  bool needs_fit = false;
  bool touch_orbit = true;  // touch: orbit/pinch (true) or select/grab (false)
  // drawing
  u64 synced_rev = ~0ull;
  Mode synced_mode = Mode::world;
  bool needs_sync = true;
  std::vector<ViewMesh> meshes;
  u32 joint_lines = 0;    // player: re-built every frame
  // player
  std::unique_ptr<world::SceneRuntime> runtime;
  i32 grab_pointer = -1;
  f32 grab_distance = 0;
  world::GameInput game_input;
  bool follow_camera = true;
  // pointer
  SelectBox box;
  i32 drag_button = -1;   // 1 = middle (pan), 2 = right (orbit)
  i32 drag_pointer = -1;
  f32 last_x = 0, last_y = 0;
  struct Touch { i32 id; f32 x, y; };
  std::vector<Touch> touches;
  f32 pinch_distance = 0, pinch_cx = 0, pinch_cy = 0;
};

enum class OperationKind : u8 { translate, rotate, scale, cursor, limit };
const char* operation_name(OperationKind k);

struct Operation {
  OperationKind kind = OperationKind::translate;
  Snapshot before;
  char axis = 0;          // 0, 'x', 'y', 'z'
  std::string typed;
  D3 pivot{};
  bool use_cursor = false;
  bool has_start = false;
  D3 start{};
  f32 start_x = 0, start_y = 0;
  i32 stage = 0;
};

struct Options {
  bool grid = true, labels = false, shapes = true, joints = true, images = true, centers = true, wireframe = false;
  bool fixture_colors = false;  // editor views: fixture colours instead of the body-type palette
  bool degrees = true;
  f64 snap = 0.25, angle_snap = 15, scale_snap = 0.1;
  bool pivot_cursor = false;
  bool reverse_zoom = false;
};

struct LogLine {
  std::string message, type;  // type: info, warn, error
};

/// Decoded RGBA image supplied by the page (data URLs cannot be decoded in C++ without codecs).
struct ImageData {
  i32 width = 0, height = 0;
  std::vector<u8> rgba;
  u32 texture = 0, texture_nearest = 0;
  bool requested = false;
};

/// A UI request returned to the page: open a file picker, download, dialog, popup, clipboard...
struct UiAction {
  json::Value data;
};

class Editor {
 public:
  /// Views at most open at once (one renderer layer bit each).
  static constexpr u32 kMaxViews = 31;
  Editor();
  ~Editor();

  /// Starts the editor on `renderer` (owned by the caller, already initialised).
  /// `player_only` starts the game runtime shell used by exported games.
  bool init(Renderer* renderer, bool player_only = false);
  void shutdown();

  // ---- the page protocol (JSON in, JSON out) ----
  /// Runs one command, e.g. {"cmd":"addBody","type":2,"shape":"box"}; returns
  /// {"ok":true} or {"ok":false,"error":...} plus optional "ui" actions.
  json::Value command(const json::Value& cmd);
  /// Everything the panels show (tabs, tree, properties, history, help); changes bump ui_revision().
  json::Value state();
  /// Cheap per-frame information: status bar, view hints, selection boxes, labels, game HUD.
  json::Value status();
  u64 ui_revision() const { return ui_revision_; }
  /// Workspace JSON when an autosave is due (empty string otherwise).
  std::string take_autosave(f64 now_ms);
  bool any_dirty() const;

  /// One display frame: steps players, moves cameras, syncs meshes, renders visible views.
  void frame(f64 now_ms);
  /// Input from the page; coordinates are CSS pixels relative to the view.
  json::Value pointer(u32 view, std::string_view type, f32 x, f32 y, i32 button, i32 buttons, i32 pointer_id, std::string_view pointer_type,
                      bool shift, bool ctrl, bool alt, f32 wheel);
  json::Value key(std::string_view key, std::string_view code, bool down, bool shift, bool ctrl, bool alt, bool repeat);
  void blur();
  /// Places a view on the shared canvas (drawing-buffer pixels, GL origin bottom-left).
  void set_view_rect(u32 view, i32 x, i32 y, i32 w, i32 h, f32 css_w, f32 css_h, f32 dpr, bool visible);
  void set_mobile(bool mobile) { mobile_ = mobile; }

  // images supplied by the page (decoded RGBA, at most 2048 px per side)
  json::Value pending_images();
  const std::string* image_source(std::string_view key) const;
  bool set_image_pixels(std::string_view key, i32 width, i32 height, const u8* rgba);

  // ---- direct C++ API (used by the commands and by tests) ----
  Document* doc();
  world::Scene* scene();
  Document* document(u32 id);
  View* view(u32 id);
  View* active_view();
  Mode mode() const { return mode_; }
  const Operation* operation() const { return operation_ ? &*operation_ : nullptr; }
  Options& options() { return options_; }
  const std::vector<LogLine>& log() const { return log_; }
  const std::vector<std::unique_ptr<Document>>& documents() const { return documents_; }
  const std::vector<std::unique_ptr<View>>& views() const { return views_; }

  Document& add_document(world::Scene scene);
  bool load_document(std::string_view text, std::string_view name);
  void set_mode(Mode m);
  void set_selection(const std::vector<std::string>& ids, bool shift = false, bool toggle = false, bool record = true);
  void set_selection_in(Mode m, const std::vector<std::string>& ids);
  /// Runs `fn` as one undoable step. `fn` returns false to cancel (after reporting why);
  /// edits that introduce new validation errors are rolled back.
  bool edit(const std::string& label, const std::function<bool()>& fn);
  void undo();
  void redo();
  void jump_history(i64 index);  // -1 = initial state
  void add_body(world::BodyKind type, const std::string& shape = {});
  void add_fixture(world::FixtureShape shape);
  void add_joint(world::JointKind type);
  void add_sampler();
  bool add_image(std::string_view file_name, std::string_view data_url, i32 width, i32 height);
  void delete_selected();
  void duplicate(bool offset = false);
  void copy_selection();
  void paste();
  void select_all(bool clear = false);
  void begin_operation(OperationKind kind);
  void type_operation(std::string_view text);  // replaces the typed number
  void set_operation_axis(char axis);
  void pointer_operation(u32 view, f32 x, f32 y, bool shift, bool ctrl);
  void finish_operation(bool accept);
  void add_hull_vertex();
  void weld_vertices();
  void make_chain(i32 count, const D3& offset, world::JointKind type);
  void set_cursor(const D3& p);
  void cursor_to_selection();
  void load_demo(std::string_view kind, bool reuse = false);
  bool open_player();
  bool reload_player(View* v = nullptr);
  void step_player(View* v = nullptr);
  void fit_view(View* v = nullptr, bool selection_only = false);
  bool trace_image_to_boxes();
  /// Sampler actions: "png", "sobel", "maxima" (fill rgba/width/height) or "trace" (adds fixtures).
  bool sampler_action(std::string_view action, std::vector<u8>* rgba = nullptr, i32* width = nullptr, i32* height = nullptr);
  void sampler_to_fixture();
  std::string scene_text(bool exported = false);
  std::vector<std::string> validate_scene() const;
  void report(const std::string& message, const std::string& type = "info");
  /// Properties panel: set a field (path as in the panel, e.g. "position.0") on all selected items.
  bool set_property(const std::string& path, const json::Value& value);
  void rename(const std::string& id, const std::string& name);
  void close_view(View* v, bool force = false);
  void clone_view();
  void set_tiled(bool tiled) { tiled_ = tiled; touch(); }
  /// RGBA pixels of the last sampler image action (for the page's PNG download).
  const std::vector<u8>& last_pixels() const { return last_pixels_; }
  bool tiled() const { return tiled_; }

 public:
  struct ItemRef;

 private:
  friend struct EditorAccess;
  void touch() { ++ui_revision_; }
  void commit(const std::string& label, const Snapshot& before, Document* d = nullptr);
  Snapshot snapshot(const Document* d = nullptr) const;
  void restore(const Snapshot& s, Document* d = nullptr);
  View& create_view(Document& d, ViewType type);
  void remove_view(View* v);
  void activate(View* v);
  void schedule_save();
  std::vector<std::string>& selection_ref(Mode m);
  const std::vector<std::string>& selection(Mode m) const;
  std::vector<ItemRef> all_items(Mode m, const world::Scene* s = nullptr) const;
  std::vector<ItemRef> chosen(Mode m) const;
  D3 item_point(const ItemRef& item, Mode m, const world::Scene& s) const;
  D3 center_of(const world::Scene& s, Mode m) const;
  void transform_selection(const world::Scene& source, const Operation& op, const D3& delta, f64 angle, const D3& scale);
  void apply_typed_operation();
  void apply_joint_limit_typed(f64 n);
  void apply_joint_limit_pointer(View& v, f32 x, f32 y);
  D3 project_on_plane(const View& v, f32 x, f32 y, const D3& pivot) const;
  bool ray_from(const View& v, f32 x, f32 y, Vec3& origin, Vec3& direction) const;
  struct Hit { std::string id; D3 point; bool ok = false; };
  Hit hit_test(View& v, f32 x, f32 y, Mode m);
  void select_in_box(View& v, const SelectBox& b);
  bool begin_grab(View& v, f32 x, f32 y, i32 pointer);
  void move_grab(View& v, f32 x, f32 y);
  void end_grab(View& v);
  void destroy_runtime(View& v);
  void sync_view(View& v);
  void clear_view_meshes(View& v);
  void update_player_meshes(View& v);
  void sync_shared();
  void render_view(View& v);
  bool screen_point(const View& v, const D3& p, f32& x, f32& y) const;
  ImageData* image_for(const world::SharedText& src);
  u32 image_texture(const world::SharedText& src, bool nearest);
  bool compose_sampler(const world::Sampler& sample, i32 limit, std::vector<u8>& rgba, i32& w, i32& h, f64& res);
  void add_run_fixtures(const std::string& body, const D3& center, const DQ& rotation, f64 width, f64 height,
                        const std::vector<std::array<i32, 4>>& runs, f64 res, f64 thickness, const std::string& label);
  json::Value fields_json();
  json::Value tree_json();
  json::Value menu_json(std::string_view name);
  json::Value dialog_json(std::string_view name);
  json::Value submit_dialog(std::string_view name, const json::Value& values);
  std::string context_html() const;
  json::Value game_hud(const View& v) const;
  void game_keys(View& v, std::string_view code, bool down);

  Renderer* renderer_ = nullptr;
  bool player_only_ = false;
  bool mobile_ = false;
  std::vector<std::unique_ptr<Document>> documents_;
  std::vector<std::unique_ptr<View>> views_;
  u32 next_doc_ = 1, next_view_ = 1;
  View* active_ = nullptr;
  Mode mode_ = Mode::body;
  std::optional<Operation> operation_;
  Options options_;
  std::vector<LogLine> log_;
  std::string status_message_ = "Ready";
  std::string coord_status_ = "World location: 0, 0, 0";
  D3 mouse_world_{};
  bool tiled_ = false;
  u64 ui_revision_ = 1;
  f64 save_due_ = -1;
  f64 now_ = 0;
  struct Clipboard {
    Mode mode = Mode::body;
    world::Scene scene;                        // bodies (+ their joints, images, samplers)
    std::vector<world::Fixture> fixtures;
    std::vector<world::Joint> joints;
    std::vector<world::ImagePlane> images;
    std::vector<world::Sampler> samplers;
    bool valid = false;
  } clipboard_;
  std::unordered_map<std::string, ImageData> images_;
  std::unordered_map<std::string, world::SharedText> image_sources_;
  std::vector<u32> shared_meshes_;  // grid + floor (all views)
  bool shared_synced_ = false;
  f32 shadow_extent_ = 0;
  std::string tree_filter_, tree_kind_ = "all";
  std::vector<std::string> collapsed_;
  std::vector<UiAction> pending_ui_;
  std::string last_engine_error_;
  f32 last_pointer_y_ = 0;
  std::vector<u8> last_pixels_;
};

}  // namespace kern::editor
