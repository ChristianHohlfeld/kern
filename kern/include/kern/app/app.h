// Kern application framework: write a game in C++ against Engine/App and the
// platform layer (kern/web/app.mjs) runs it - frame loop, drawing-buffer
// size, input, context loss and a status-text channel are handled for you.
//
//   class MyGame final : public kern::App {
//     bool start(kern::Engine& engine) override { ...create meshes... }
//     void update(kern::Engine& engine, const kern::Frame& frame) override { ...move things... }
//   };
//   kern::App* kern_create_app() { static MyGame game; return &game; }
//
// update() runs once per display frame; the engine renders right after it.
#pragma once

#include <string>
#include <utility>
#include <vector>

#include "kern/gfx/renderer.h"

namespace kern {

/// Keyboard keys (KeyboardEvent.code order in kern/web/app.mjs KEY_CODES).
enum class Key : u8 {
  unknown = 0,
  a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p, q, r, s, t, u, v, w, x, y, z,
  digit0, digit1, digit2, digit3, digit4, digit5, digit6, digit7, digit8, digit9,
  left, right, up, down, space, enter, escape, tab, backspace, shift_left, shift_right, control_left, control_right, alt_left, alt_right,
  count
};

constexpr u32 kMaxTouches = 10;

struct Touch {
  i32 id = -1;  // < 0: slot unused
  Vec2 start, position, previous;
};

class Input {
 public:
  bool down(Key k) const { return keys_[static_cast<u32>(k)] & kDown; }
  bool pressed(Key k) const { return keys_[static_cast<u32>(k)] & kPressed; }
  bool released(Key k) const { return keys_[static_cast<u32>(k)] & kReleased; }
  /// Pointer position in drawing-buffer pixels, movement since the last frame (pixels, includes locked movement).
  Vec2 pointer() const { return pointer_; }
  Vec2 pointer_delta() const { return pointer_delta_; }
  bool button(u32 b) const { return buttons_ & (1u << b); }
  bool button_pressed(u32 b) const { return pressed_buttons_ & (1u << b); }
  f32 wheel() const { return wheel_; }
  const Touch* touches() const { return touches_; }
  u32 touch_count() const;
  bool focused() const { return focused_; }

  // ---- platform side ----
  void key(Key k, bool down);
  void pointer_move(Vec2 position, Vec2 delta);
  void pointer_buttons(u32 buttons);
  void add_wheel(f32 dy) { wheel_ += dy; }
  void touch(i32 id, i32 phase, Vec2 position);  // phase 0 start, 1 move, 2 end/cancel
  void set_focus(bool focused);
  void end_frame();  // clears per-frame edges and deltas

 private:
  enum : u8 { kDown = 1, kPressed = 2, kReleased = 4 };
  u8 keys_[static_cast<u32>(Key::count)] = {};
  Vec2 pointer_, pointer_delta_;
  u32 buttons_ = 0, pressed_buttons_ = 0;
  f32 wheel_ = 0;
  Touch touches_[kMaxTouches];
  bool focused_ = true;
};

struct Frame {
  f64 time = 0;   // seconds since start
  f32 dt = 0;     // seconds since the previous frame (clamped to 0.1)
  u64 index = 0;
};

class Engine {
 public:
  explicit Engine(Renderer& renderer) : renderer(renderer) {}
  Renderer& renderer;
  Input input;
  /// Text shown by the host page in its status overlay (empty hides it).
  void set_status(const char* text);
  /// Asks the host to capture (or release) the mouse for mouse-look controls.
  void request_pointer_lock(bool lock);
  /// Launch parameter `key` from the host (the page's query string by default), or `fallback`.
  const char* arg(const char* key, const char* fallback = nullptr) const;
  f64 arg_number(const char* key, f64 fallback) const;
  /// Platform side: "k=v&k2=v2" (URL-encoded) launch parameters.
  void set_args(const char* text, u32 length);
  i32 width() const { return renderer.width(); }
  i32 height() const { return renderer.height(); }
  f32 aspect() const { return static_cast<f32>(renderer.width()) / static_cast<f32>(max_of(1, renderer.height())); }
  const RenderStats& stats() const { return renderer.last_stats(); }
  /// Clear colour for the frame (behind the sky only where no sky is drawn).
  Vec4 clear_color{0.045f, 0.07f, 0.12f, 1};

 private:
  std::vector<std::pair<std::string, std::string>> args_;
};

class App {
 public:
  virtual ~App() = default;
  /// Renderer configuration, asked once before start().
  virtual RendererOptions configure() { return {}; }
  /// Shader extensions the game uses (must outlive the app).
  virtual Span<ShaderExtension* const> extensions() { return {}; }
  virtual bool start(Engine& engine) = 0;
  virtual void update(Engine& engine, const Frame& frame) = 0;
  virtual void resized(Engine& engine) { (void)engine; }
  virtual void stop(Engine& engine) { (void)engine; }
};

}  // namespace kern

/// Implemented by the game: returns its App instance (called once).
kern::App* kern_create_app();
