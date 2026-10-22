// Kern Studio: the engine editor. One level open at a time, edited through
// viewports with Unreal-style navigation and transform gizmos, panels for the
// outliner, details, world settings and content, and Play / Simulate on the
// game runtime (kern/game/runtime.h). Everything here is C++; the browser page
// (kern/web/studio) draws panels from the JSON this class returns and forwards
// input (see kern/src/studio/api.cpp for the C ABI).
#pragma once

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "kern/core/json.h"
#include "kern/game/runtime.h"
#include "kern/game/net_game.h"
#include "kern/game/touch.h"
#include "kern/game/sound.h"
#include "kern/net/transport.h"
#include "kern/gfx/renderer.h"

namespace kern::studio {

using game::ActorId;

enum class Tool : u8 { select, translate, rotate, scale };
enum class Space : u8 { world, local };
enum class ViewType : u8 { perspective, top, bottom, left, right, front, back };
enum class ViewMode : u8 { lit, unlit, wireframe, collision };
enum class Layout : u8 { single, quad, two_columns };
enum class PlayState : u8 { editing, simulating, playing };

const char* tool_name(Tool t);
const char* view_type_name(ViewType t);
const char* view_mode_name(ViewMode m);

struct ShowFlags {
  bool grid = true, icons = true, colliders = false, joints = true, contacts = false;
  bool bounds = false, stats = false, fog = true, sky = true, selection = true;
};

/// Renderer layer bits used by Studio.
namespace layer {
enum : u32 {
  scene = 1u << 0,
  grid = 1u << 1,         // perspective ground grid
  icons = 1u << 2,        // lights, empties, player starts, cameras
  selection = 1u << 3,    // selection outline
  colliders = 1u << 4,    // physics debug lines
  wireframe = 1u << 5,    // wireframe view mode
  bounds = 1u << 6,
  joints = 1u << 7,       // joint and contact debug lines
  gizmo0 = 1u << 8,       // + viewport index (4 viewports)
  ortho_grid0 = 1u << 12, // + viewport index
  client0 = 1u << 16,     // + client index: worlds of network clients in multi-player Play
};
}

struct Rect {
  i32 x = 0, y = 0, w = 0, h = 0;  // drawing-buffer pixels, origin bottom-left
};

struct ViewCamera {
  Vec3 position{-24, -30, 18};
  f32 yaw = 51, pitch = -24;    // degrees: yaw 0 looks along +X, pitch < 0 looks down
  f32 fov = 70;
  f32 ortho_height = 40;        // orthographic views
  Vec3 ortho_center;            // orthographic views: the point at the centre
  Vec3 forward() const;
  Vec3 right() const;
};

enum class Drag : u8 { none, fly, pedestal, pan, orbit, dolly, gizmo, marquee, ortho_pan, grab, look };

struct Viewport {
  u32 index = 0;                // 0..3
  ViewType type = ViewType::perspective;
  ViewMode mode = ViewMode::lit;
  ShowFlags flags;
  ViewCamera camera;
  bool game_view = false;       // hides every editor helper (G)
  Rect rect;
  f32 css_width = 1, css_height = 1, dpr = 1;
  bool visible = false;
  // input
  Drag drag = Drag::none;
  u32 buttons = 0;              // bit 0 left, 1 right, 2 middle
  f32 x = 0, y = 0;             // pointer, css pixels
  f32 press_x = 0, press_y = 0;
  bool moved = false;           // the pointer moved since the press (a click otherwise)
  Vec3 pivot;                   // orbit / dolly pivot
  f32 fov_boost = 0;            // Z / C zoom while flying
  std::vector<u32> grid_meshes; // ortho grid of this viewport
  u32 gizmo_mesh = 0, gizmo_solid = 0;
  u64 gizmo_key = 0;
  u64 grid_key = 0;
};

struct GizmoDrag {
  bool active = false;
  Tool tool = Tool::translate;
  u32 axes = 0;                 // bits x, y, z; 7 = screen plane / uniform
  Vec3 pivot;
  Quat frame;                   // gizmo orientation (world or the primary actor's)
  Vec3 start_point;             // translate: hit point on the axis/plane
  f32 start_angle = 0, start_t = 0;
  f32 start_x = 0, start_y = 0;
  std::vector<std::pair<ActorId, Mat4>> start;  // selected roots with their start world matrices
  Vec3 delta;                   // translate (world)
  f32 angle = 0;                // rotate (degrees)
  Vec3 factor{1, 1, 1};         // scale
  bool duplicated = false;
  bool moved = false;
  bool surface = false;         // dragged by the actor itself: slides over surfaces under the cursor
  f32 lift = 0;                 // surface drag: grab point height above the selection's bottom
  u32 viewport = 0;
  game::Level before;           // the level when the drag started (undo / cancel)
  game::Level base;             // the level the live preview starts from (before + Alt-duplicates)
  std::vector<ActorId> selection_before;  // the selection when the drag started (before an Alt-duplicate)
};

/// One step of the undo history. Every step stores the complete document after
/// the change, so undo and redo are exact (no inverse operations to drift) and
/// depend only on the sequence of edits, never on timing.
struct HistoryEntry {
  std::string label;
  game::Level level;                       // document after this step
  std::vector<ActorId> selection_before;   // selection when the step began (restored by undo)
  std::vector<ActorId> selection_after;    // selection after the step (restored by redo)
  std::string merge_key;                   // property edits: path + targets
  std::string transaction;                 // page token of one continuous interaction (slider, scrub, colour drag)
  u64 serial = 0;                          // identity of this state (dirty tracking)
};

struct Snap {
  bool grid = true, rotation = true, scale = false;
  f32 grid_size = 0.5f, rotation_step = 15, scale_step = 0.25f;
};

struct Preferences {
  i32 camera_speed = 4;         // 1..8
  f32 mouse_sensitivity = 1;
  bool invert_y = false;
  Layout layout = Layout::single;
  bool orbit_around_selection = true;
  f32 autosave_seconds = 2;
};

struct LogLine {
  std::string level;            // info, warn, error
  std::string text;
  f64 time = 0;
};

struct UiAction {
  json::Value data;
};

class Studio {
 public:
  Studio();
  ~Studio();
  Studio(const Studio&) = delete;
  Studio& operator=(const Studio&) = delete;

  /// `player_only`: no editor, the level plays full screen (exported games).
  bool init(Renderer* renderer, game::NightWindows* windows, bool player_only = false);

  // ---- per frame ----
  void frame(f64 now_ms);
  void set_viewport_rect(u32 index, Rect rect, f32 css_width, f32 css_height, f32 dpr, bool visible);
  /// Pointer: type 0 down, 1 move, 2 up, 3 cancel, 4 wheel, 5 leave. buttons: DOM bits.
  json::Value pointer(u32 viewport, u32 type, f32 x, f32 y, i32 button, i32 buttons, u32 modifiers, f32 wheel);
  /// Touch / pen fingers on the active viewport while playing (engine touch controls). True when used.
  bool touch_pointer(u32 type, i32 id, f32 x, f32 y, f64 time_ms);
  const game::InputArbiter& input_device() const { return input_; }
  /// The camera of the possessed player this frame (Play).
  const Camera& play_camera() const { return play_camera_; }
  const game::TouchControls& touch_controls() const { return touch_ctl_; }
  /// Keyboard (key = KeyboardEvent.key, code = KeyboardEvent.code). modifiers: 1 shift, 2 ctrl/meta, 4 alt.
  json::Value key(std::string_view key, std::string_view code, bool down, u32 modifiers, bool repeat);
  void blur();
  /// Pointer-lock look deltas while playing (css pixels).
  void look(f32 dx, f32 dy);

  // ---- protocol ----
  json::Value command(const json::Value& cmd);
  json::Value state();          // full panel state (outliner, details, menus, ...)
  json::Value status();         // per-frame state (hud, stats, camera, overlays)
  u64 revision() const { return revision_; }
  json::Value take_ui();
  std::string take_autosave(f64 now_ms);
  /// Unsaved changes: the current history state differs from the saved one.
  bool dirty() const { return history_.empty() || history_[history_index_].serial != saved_serial_; }
  const std::vector<LogLine>& log() const { return log_; }
  std::vector<std::string> pending_textures();
  bool texture_pixels(const std::string& id, i32 w, i32 h, const u8* rgba);
  std::string texture_source(const std::string& id) const;

  // ---- audio (the host pulls rendered PCM; see kern/web/audio.mjs) ----
  /// Starts the mixer at the output device's rate. Without it the studio is silent.
  bool audio_init(u32 sample_rate);
  bool audio_ready() const { return mixer_->ready(); }
  /// Interleaved stereo of the next `frames` frames.
  void audio_render(f32* out, u32 frames);
  audio::Mixer& mixer() { return *mixer_; }
  game::SoundBank& sound_bank() { return *bank_; }
  game::GameAudio& game_audio() { return *game_audio_; }
  /// Imported sounds that need decoded PCM from the host.
  std::vector<std::string> pending_sounds() const;
  std::string sound_source(const std::string& id) const;
  bool sound_pcm(const std::string& id, const f32* samples, u32 frames, u32 channels, u32 rate);
  void sound_failed(const std::string& id, const std::string& reason);
  /// Plays a sound reference on the UI bus (the details panel's preview button). One at a time.
  void preview_sound(const std::string& ref, f32 volume = 1, f32 pitch = 1);
  void stop_preview();
  bool previewing() const;
  /// Adds an imported sound (a data: URL) as one undo step; returns its id.
  std::string import_sound(const std::string& name, const std::string& src, f32 duration);
  bool remove_sound(const std::string& id);
  void set_muted(bool muted);
  bool muted() const { return game_audio_->muted(); }

  // ---- multiplayer ----
  /// Play with several players in this editor (Unreal's "Number of Players"): the
  /// level's world is a listen server, every further player a network client with
  /// its own world and viewport, connected through an in-process network.
  struct NetPlaySettings {
    i32 players = 1;                      // 1..4
    net::SimulatorSettings network;       // latency, jitter, loss for the local players
  };
  NetPlaySettings& net_play() { return net_play_; }
  /// Real network play (browser host transport, see kern/web/studio/game.mjs KernNet).
  bool net_host();
  bool net_join(u64 server);
  void net_stop();
  net::HostTransport& host_transport() { return host_transport_; }
  game::NetGame* net_game() { return net_.get(); }
  /// World a viewport shows (the main world or a client's).
  game::GameWorld& world_of(u32 viewport);

  // ---- direct API (commands call these; tests use them) ----
  const game::Level& level() const { return level_; }
  game::Level& edit_level() { return level_; }
  bool load_level(game::Level level, bool reset_history = true);
  bool load_level_text(std::string_view text, std::string_view name);
  std::string level_text() const;
  /// Applies a modification as one undo step (label shows in the history). `fn`
  /// edits the level in place and may change the selection; returning false
  /// rolls both back. A change that leaves the document identical adds no step.
  template <typename F> bool edit(const char* label, F&& fn) {
    if (play_ != PlayState::editing) { report("Stop playing to edit the level", "warn"); return false; }
    game::Level before = level_;
    std::vector<ActorId> selection_before = selection_;
    if (!fn(level_)) {
      level_ = std::move(before);
      selection_ = std::move(selection_before);
      coalesce_.clear();
      transaction_.clear();
      changed();
      return false;
    }
    commit(label, std::move(before), std::move(selection_before));
    return true;
  }
  void undo();
  void redo();
  /// Moves to history step `index` through the intermediate steps (same result as repeated undo/redo).
  bool jump_history(std::size_t index);
  bool can_undo() const { return history_index_ > 0; }
  bool can_redo() const { return history_index_ + 1 < history_.size(); }
  const std::vector<HistoryEntry>& history() const { return history_; }
  std::size_t history_index() const { return history_index_; }
  /// Marks the current state as saved (dirty() becomes false until the next change).
  void mark_saved();
  static constexpr std::size_t kMaxHistory = 500;

  // selection
  const std::vector<ActorId>& selection() const { return selection_; }
  ActorId primary() const { return selection_.empty() ? 0 : selection_.back(); }
  void select(const std::vector<ActorId>& ids, bool add = false, bool toggle = false);
  void select_all();
  void deselect();
  bool selected(ActorId id) const;
  /// Selected actors without selected ancestors (the ones to move).
  std::vector<ActorId> selection_roots() const;

  // actors
  ActorId spawn(const std::string& item, std::optional<Vec3> at = {}, const std::string& sound = {});
  ActorId add_actor(game::Actor actor, bool select_it = true);
  void delete_selection();
  void delete_selection(const char* label);
  void duplicate_selection(Vec3 offset = {});
  /// Alt-drag: copies the selection into the level without an undo step (the drag commits it).
  void duplicate_in_place();
  void copy_selection();
  bool paste();
  void cut_selection();
  bool attach(const std::vector<ActorId>& children, ActorId parent);
  /// Re-parents inside an edit step, keeping world placements (0 = top level).
  static void reparent(game::Level& level, const std::vector<ActorId>& children, ActorId parent);
  bool rename(ActorId id, const std::string& name);
  void group_selection();
  void snap_to_floor();
  void set_hidden(const std::vector<ActorId>& ids, bool hidden);
  void focus_selection(i32 viewport = -1);
  /// Consecutive edits of the same property with the same non-empty `transaction`
  /// token (one slider drag, one scrub) form a single undo step.
  bool set_property(const std::string& path, const json::Value& value, const std::string& transaction = {});
  bool set_world_property(const std::string& path, const json::Value& value, const std::string& transaction = {});
  bool add_component(const std::string& type);
  bool remove_component(const std::string& type, i32 index);
  void keep_simulation_changes();

  // play
  bool play(game::SimMode mode);
  void stop();
  void toggle_pause();
  void eject();
  PlayState play_state() const { return play_; }
  game::GameWorld& world() { return *world_; }
  const Preferences& prefs() const { return prefs_; }

  // tools
  void set_tool(Tool t);
  Tool tool() const { return tool_; }
  Space space() const { return space_; }
  Snap& snap() { return snap_; }
  Preferences& preferences() { return prefs_; }
  Viewport& viewport(u32 i) { return views_[i < 4 ? i : 0]; }
  u32 active_viewport() const { return active_view_; }
  void set_layout(Layout l);
  void set_view_type(u32 viewport, ViewType t);
  GizmoDrag& gizmo() { return gizmo_; }

  // queries used by input handling and tests
  ActorId pick(u32 viewport, f32 x, f32 y, Vec3* hit_point = nullptr);
  bool ray(const Viewport& v, f32 x, f32 y, Ray& out) const;
  bool to_screen(const Viewport& v, Vec3 world, f32& x, f32& y) const;
  Camera camera_of(const Viewport& v) const;
  /// Gizmo handle under the pointer (0 none; axis bits as GizmoDrag::axes).
  u32 gizmo_hit(const Viewport& v, f32 x, f32 y) const;
  void report(const std::string& text, const char* level = "info");

 private:
  /// Records the step; false (and no step) when the document did not change.
  bool commit(const char* label, game::Level before, std::vector<ActorId> selection_before);
  void touch() { ++revision_; }
  void changed();
  void step_history(bool forward);
  // viewports
  void update_cameras(f32 dt);
  void render_viewport(Viewport& v, f64 now_ms);
  void begin_drag(Viewport& v, Drag d);
  void apply_drag(Viewport& v, f32 dx, f32 dy, u32 modifiers);
  void end_drag(Viewport& v);
  void click(Viewport& v, f32 x, f32 y, u32 modifiers);
  void marquee_select(Viewport& v, u32 modifiers);
  Vec3 orbit_pivot(Viewport& v);
  f32 camera_speed() const;
  // gizmo
  bool gizmo_visible(const Viewport& v) const;
  bool gizmo_frame(Vec3& pivot, Quat& frame) const;
  f32 gizmo_scale(const Viewport& v, Vec3 pivot) const;
  void begin_gizmo(Viewport& v, u32 axes, u32 modifiers);
  void update_gizmo(Viewport& v);
  /// Drag the selection by grabbing an actor: it follows the cursor over whatever
  /// surface is underneath (other actors, the ground), resting on it.
  void begin_surface_drag(Viewport& v, Vec3 grab_point);
  void update_surface_drag(Viewport& v);
  /// Surface under the cursor ignoring the given actors (and their children).
  bool surface_under(const Viewport& v, f32 x, f32 y, const std::vector<ActorId>& ignore, Vec3& point);
  void end_gizmo(bool accept);
  void build_gizmo_meshes(Viewport& v);
  // overlays
  void sync_overlays();
  void sync_grid(Viewport& v);
  void sync_perspective_grid(const Viewport& v);
  void sync_selection_outline();
  void sync_icons();
  void sync_debug_lines();
  void sync_bounds();
  void sync_wireframe();
  u32 upload_lines(u32& id, const std::vector<f32>& positions, const std::vector<f32>& colors, u32 layers, bool on_top = false);
  // panels
  json::Value outliner_json() const;
  // logic editor (logic_edit.cpp)
  json::Value logic_command(const std::string& name, const json::Value& c);
  json::Value logic_json() const;
  json::Value logic_trace_json() const;
  json::Value logic_palette(const json::Value& c) const;
  u32 logic_owner_ = 0;
  bool logic_open_ = false;
  std::string logic_view_ = "graph";
  json::Value details_json() const;
  json::Value world_json() const;
  json::Value catalog_json() const;
  json::Value menu_json(std::string_view name) const;
  json::Value dialog_json(std::string_view name, const json::Value& args) const;
  json::Value submit_dialog(std::string_view name, const json::Value& values);
  json::Value viewports_json() const;
  void push_ui(json::Value action);
  json::Value run(const json::Value& cmd);
  game::PlayInput play_input() const;

  Renderer* r_ = nullptr;
  game::NightWindows* windows_ = nullptr;
  std::unique_ptr<game::GameWorld> world_;
  bool ready_ = false, player_only_ = false;
  game::Level level_;
  std::vector<HistoryEntry> history_;
  std::size_t history_index_ = 0;
  std::string coalesce_;                  // merge key of the edit being committed
  std::string transaction_;               // transaction token of the edit being committed
  u64 next_serial_ = 1, saved_serial_ = 0;
  f64 save_due_ = -1;
  std::vector<ActorId> selection_;
  std::vector<game::Actor> clipboard_;
  std::array<Viewport, 4> views_;
  u32 active_view_ = 0;
  Tool tool_ = Tool::translate;
  Space space_ = Space::world;
  Snap snap_;
  Preferences prefs_;
  GizmoDrag gizmo_;
  u32 gizmo_hover_ = 0;
  PlayState play_ = PlayState::editing;
  bool paused_ = false;
  bool ejected_ = false;
  bool mouse_captured_ = false;
  std::unordered_set<std::string> keys_;  // KeyboardEvent.code held
  u32 modifiers_ = 0;
  f32 look_dx_ = 0, look_dy_ = 0, play_zoom_ = 0;
  game::PlayInput pad_;                   // gamepad (standard mapping), from the page
  bool pad_connected_ = false, pad_start_ = false;
  u32 play_buttons_ = 0;                  // mouse buttons held while the game has the mouse
  f32 touch_look_x_ = 0, touch_look_y_ = 0;  // degrees this frame from touch look
  bool soldier_pawn() const;
  game::PlayInput touch_;                 // on-screen input sent by the page (gameInput command)
  game::InputArbiter input_;              // touch or mouse+keyboard (+ gamepad), the last used wins
  game::TouchControls touch_ctl_;         // engine-drawn touch sticks and buttons (Play on touch devices)
  game::PlayInput touch_ctl_in_;          // what the touch controls give this frame
  f32 safe_[4] = {0, 0, 0, 0};            // safe-area insets left, top, right, bottom (css px)
  /// Takes the mouse for Play (pointer lock), never on touch devices.
  void capture_play_pointer();
  /// Keeps the touch controls in step with the viewport, the possessed player and the device.
  void update_touch_controls(f32 dt);
  json::Value touch_json() const;
  f64 now_ = 0, last_frame_ = -1;
  f32 fps_ = 0;
  u64 revision_ = 1;
  u64 scene_revision_ = 0, synced_revision_ = ~0ull;
  u64 world_synced_ = ~0ull;              // scene revision the presentation (meshes for picking) shows
  /// Brings the presentation up to date now (commands that pick or measure right after an edit).
  void ensure_world_synced() {
    if (play_ == PlayState::editing && world_synced_ != scene_revision_) { world_->sync(level_); world_synced_ = scene_revision_; }
  }
  std::vector<LogLine> log_;
  std::vector<UiAction> ui_;
  std::unordered_map<u32, ViewCamera> bookmarks_;
  std::string status_text_;
  std::string outliner_filter_;
  std::unordered_set<std::string> collapsed_;  // outliner folders / actors ("f:Name" / "a:12")
  std::unordered_set<std::string> closed_sections_;  // details categories
  // overlay meshes
  u32 grid_mesh_ = 0;
  i64 grid_cell_x_ = 1 << 30, grid_cell_y_ = 1 << 30;
  f32 grid_level_ = -1;
  u32 selection_mesh_ = 0, icons_mesh_ = 0, debug_mesh_ = 0, joints_mesh_ = 0, bounds_mesh_ = 0;
  Camera play_camera_;
  bool play_camera_valid_ = false;
  std::vector<u32> wireframe_meshes_;
  u64 overlay_key_ = 0, wireframe_key_ = 0;
  bool any_wireframe_ = false;
  u32 next_texture_ = 1;
  // audio
  std::unique_ptr<audio::Mixer> mixer_;
  std::unique_ptr<game::SoundBank> bank_;
  std::unique_ptr<game::GameAudio> game_audio_;
  audio::VoiceId preview_ = 0;
  std::string preview_ref_;               // waiting for its clip
  f32 preview_volume_ = 1, preview_pitch_ = 1;
  u64 audio_scene_ = ~0ull;
  void update_audio(const Camera& listener, f32 dt);
  // multiplayer
  struct LocalClient {
    std::unique_ptr<game::GameWorld> world;
    std::unique_ptr<net::Transport> transport;
    std::unique_ptr<game::NetGame> game;
    Camera camera;
    bool camera_valid = false;
  };
  NetPlaySettings net_play_;
  std::unique_ptr<net::LoopbackNetwork> hub_;
  std::unique_ptr<net::Transport> hub_server_;
  std::vector<LocalClient> clients_;      // client i shows in viewport i + 1
  std::unique_ptr<game::NetGame> net_;    // the main world's role (listen server or remote client)
  net::HostTransport host_transport_;
  bool net_remote_ = false;               // net_ talks over host_transport_
  f64 net_time_ = 0;
  void start_local_clients();
  void stop_net();
  void update_net(f32 dt, const game::PlayInput& in);
};

}  // namespace kern::studio
