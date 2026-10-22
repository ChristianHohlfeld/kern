// Touch controls and input-device detection for Kern games, after SpawnCity's
// on-foot / vehicle touch system (thumb layout, sticks, look drag with edge turn,
// screen-touch fire, context buttons), in C++ so the editor's Play mode, packaged
// games and every controller use one implementation.
//
//   InputArbiter   which device the player uses right now (touch, mouse+keyboard,
//                  gamepad): the last one used wins, with SpawnCity's rules (a
//                  mouse never switches a coarse-pointer device to desktop; any
//                  real key press does). Pointer lock only for mouse+keyboard.
//   TouchControls  layout (168 px corner sticks flush with the safe insets,
//                  buttons packed around the thumbs), pointer ownership
//                  (buttons > sticks > screen), stick maths and the PlayInput
//                  they produce. The page draws view() as an overlay.
#pragma once

#include <string>
#include <vector>

#include "kern/game/level.h"
#include "kern/game/runtime.h"

namespace kern::game {

enum class InputDevice : u8 { touch, mouse_keyboard };

class InputArbiter {
 public:
  /// What the page knows about the device: a fine hovering primary pointer, touch points.
  void set_platform(bool fine_pointer, bool coarse_pointer, i32 max_touch_points);
  /// A pointer went down ("mouse", "touch", "pen").
  void pointer_down(bool touch);
  /// The mouse moved by itself (not locked, not over the UI).
  void mouse_moved();
  /// A real key went down (keyboards switch to the desktop layout, even on phones).
  void key_down();
  void gamepad_used();
  InputDevice device() const { return device_; }
  bool touch() const { return device_ == InputDevice::touch; }
  bool gamepad() const { return pad_; }
  bool coarse() const { return coarse_; }
  /// Counts device switches (listeners refresh layouts / pointer lock).
  u32 changes() const { return changes_; }

 private:
  void set(InputDevice d);
  InputDevice device_ = InputDevice::mouse_keyboard;
  bool fine_ = true, coarse_ = false, pad_ = false;
  i32 touch_points_ = 0;
  u32 changes_ = 0;
};

/// One drawn control of the overlay (CSS px in the viewport, top-left origin).
struct TouchButton {
  std::string id, label;
  f32 x = 0, y = 0, w = 0, h = 0;
  bool round = true, pressed = false, on = false;   // on: toggles (aim, crouch)
  u32 color = 0x0d1118;                              // fill (rgb); drawn translucent
};

struct TouchStick {
  f32 x = 0, y = 0, size = 168;   // ring rectangle
  f32 knob_x = 0, knob_y = 0;     // -1..1 deflection drawn
  bool live = false, accent = false;
  std::string label;
};

struct TouchView {
  bool visible = false;
  TouchStick move, look;
  std::vector<TouchButton> buttons;
  bool fire = false;              // a finger is firing (soldier)
  f32 fire_x = 0, fire_y = 0;     // where (px)
};

class TouchControls {
 public:
  /// Viewport in CSS px and the safe-area insets (notches, rounded corners).
  void set_frame(f32 width, f32 height, f32 safe_left, f32 safe_top, f32 safe_right, f32 safe_bottom);
  /// The possessed controller (layout and meaning of sticks / buttons follow it).
  void set_controller(const ControllerComponent* c, bool weapon, bool alive);
  /// Pointer events of touch / pen fingers (type: 0 down, 1 move, 2 up, 3 cancel). True when used.
  bool pointer(u32 type, i32 id, f32 x, f32 y, f64 time_ms);
  void release_all();
  /// Per frame: the input the fingers give (look in degrees for this frame, edge turn included).
  /// vfov_deg: the camera's current field of view (zoomed views turn slower).
  void update(f32 dt, PlayInput& in, f32 vfov_deg);
  const TouchView& view() const { return view_; }
  bool active() const { return !fingers_.empty(); }

 private:
  struct Finger {
    i32 id = 0;
    enum Kind : u8 { move, look, fire, look_drag, button } kind = move;
    f32 x0 = 0, y0 = 0, x = 0, y = 0, last_x = 0, last_y = 0;
    f64 last_t = 0;
    std::string button_id;
    f32 base_x = 0, base_y = 0;   // knob deflection when the finger landed (no jump)
  };
  void layout();
  TouchButton* button_at(f32 x, f32 y);
  bool in_stick(const TouchStick& s, f32 x, f32 y) const;
  void set_pressed(const std::string& id, bool pressed);

  f32 w_ = 0, h_ = 0, sl_ = 0, st_ = 0, sr_ = 0, sb_ = 0;
  ControlMode mode_ = ControlMode::character;
  bool has_controller_ = false, weapon_ = false, alive_ = true, boost_ = false, jump_ = false;
  std::vector<Finger> fingers_;
  TouchView view_;
  f32 move_x_ = 0, move_y_ = 0;   // stick deflection, y down
  f32 look_x_ = 0, look_y_ = 0;   // look stick deflection (edge turn)
  f32 look_edge_ = 0;
  f32 drag_yaw_ = 0, drag_pitch_ = 0;  // degrees of look dragged since the last update
  bool aim_on_ = false, crouch_on_ = false;
  bool sprint_ = false;
  f32 base_fov_ = 62;
};

}  // namespace kern::game
