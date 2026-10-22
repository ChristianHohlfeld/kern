// Touch controls and input-device arbitration (see touch.h). Numbers follow
// SpawnCity's foot_touch_controller / thumb_layout / edge_hud_layout /
// fps_control_math so the game feels the same on a phone.
#include "kern/game/touch.h"

#include <algorithm>
#include <cmath>

namespace kern::game {

namespace {

constexpr f32 kStick = 168;            // ring size, px
constexpr f32 kRadiusK = 0.42f;        // deflection radius = .42 * ring
constexpr f32 kEdgeStart = 0.92f;      // look stick: edge turn from .92 R
constexpr f32 kEdgeRamp = 42;          // ... to full over 42 px
constexpr f32 kYawPerPx = 0.0068f * 57.29578f;    // 1:1 drag, degrees per px
constexpr f32 kPitchPerPx = 0.0059f * 57.29578f;
constexpr f32 kYawRate = 4.2f * 57.29578f;        // edge turn, degrees per second
constexpr f32 kPitchRate = 3.3f * 57.29578f;
constexpr f32 kEdge = 10, kGap = 8;    // edge HUD margins

f32 smoothstep01(f32 t) {
  t = std::clamp(t, 0.0f, 1.0f);
  return t * t * (3 - 2 * t);
}

/// Monotone cubic Hermite through SpawnCity's dynamic look curve.
f32 dyn_curve(f32 n) {
  static const f32 px[] = {0, 0.3f, 0.7f, 0.9f, 1}, py[] = {0, 0.24f, 0.5f, 0.89f, 1}, pm[] = {1.0f, 0.62f, 1.1f, 1.5f, 1.0f};
  n = std::clamp(n, 0.0f, 1.0f);
  int i = 0;
  while (i < 3 && n > px[i + 1]) ++i;
  const f32 h = px[i + 1] - px[i], t = (n - px[i]) / h, t2 = t * t, t3 = t2 * t;
  return (2 * t3 - 3 * t2 + 1) * py[i] + (t3 - 2 * t2 + t) * h * pm[i] + (-2 * t3 + 3 * t2) * py[i + 1] + (t3 - t2) * h * pm[i + 1];
}

/// shapeFpsStick with the touch profile (inner dead zone .025, look fineness 6).
void shape_stick(f32& x, f32& y) {
  const f32 raw = std::min(1.0f, std::hypot(x, y));
  constexpr f32 inner = 0.025f, outer = 0.97f, anti = 0.045f;
  if (raw <= inner) { x = y = 0; return; }
  const f32 n = std::clamp((raw - inner) / (outer - inner), 0.0f, 1.0f);
  const f32 k = 0.48f * (6 - 1) / 9.0f, w = 0.5f + k / 0.6f * 0.5f;
  const f32 curve = dyn_curve(n) * w + n * (1 - w);
  const f32 curved = n >= 1 ? 1.0f : std::clamp(anti + (1 - anti) * curve, 0.0f, 1.0f);
  x *= curved / raw;
  y *= curved / raw;
}

TouchButton make_button(const char* id, const char* label, f32 size, u32 color, bool round = true) {
  TouchButton b;
  b.id = id;
  b.label = label;
  b.w = round ? size : size * 1.7f;
  b.h = size;
  b.round = round;
  b.color = color;
  return b;
}

}  // namespace

// ---------------------------------------------------------------------------
// InputArbiter

void InputArbiter::set_platform(bool fine_pointer, bool coarse_pointer, i32 max_touch_points) {
  const bool first = touch_points_ == 0 && fine_ && !coarse_;
  fine_ = fine_pointer;
  coarse_ = coarse_pointer;
  touch_points_ = max_touch_points;
  // Initial decision: desktop when the primary pointer is fine or there is no touch.
  if (first || changes_ == 0) set(fine_ || touch_points_ <= 0 ? InputDevice::mouse_keyboard : InputDevice::touch);
}

void InputArbiter::pointer_down(bool touch) {
  if (touch) { set(InputDevice::touch); return; }
  pad_ = false;
  // A mouse on a phone or tablet (coarse primary pointer) keeps the touch layout.
  if (!coarse_) set(InputDevice::mouse_keyboard);
}

void InputArbiter::mouse_moved() {
  if (fine_ && !coarse_) set(InputDevice::mouse_keyboard);
}

void InputArbiter::key_down() {
  pad_ = false;
  set(InputDevice::mouse_keyboard);
}

void InputArbiter::gamepad_used() {
  if (!pad_) ++changes_;
  pad_ = true;
}

void InputArbiter::set(InputDevice d) {
  if (d == device_) return;
  device_ = d;
  ++changes_;
}

// ---------------------------------------------------------------------------
// TouchControls

void TouchControls::set_frame(f32 width, f32 height, f32 safe_left, f32 safe_top, f32 safe_right, f32 safe_bottom) {
  if (width == w_ && height == h_ && safe_left == sl_ && safe_top == st_ && safe_right == sr_ && safe_bottom == sb_) return;
  w_ = width;
  h_ = height;
  sl_ = std::max(0.0f, safe_left);
  st_ = std::max(0.0f, safe_top);
  sr_ = std::max(0.0f, safe_right);
  sb_ = std::max(0.0f, safe_bottom);
  layout();
}

void TouchControls::set_controller(const ControllerComponent* c, bool weapon, bool alive) {
  const bool has = c != nullptr;
  const ControlMode mode = c ? c->mode : ControlMode::character;
  const bool boost = c && c->boost, jump = c && c->jump_speed > 0;
  const bool changed = has != has_controller_ || mode != mode_ || weapon != weapon_ || boost != boost_ || jump != jump_;
  if (c) base_fov_ = std::max(1.0f, c->camera_fov);
  if (alive != alive_) {
    alive_ = alive;
    if (!alive) { aim_on_ = false; crouch_on_ = false; }
    if (!changed) layout();
  }
  if (!changed) return;
  has_controller_ = has;
  mode_ = mode;
  weapon_ = weapon;
  boost_ = boost;
  jump_ = jump;
  aim_on_ = crouch_on_ = false;
  release_all();
  layout();
}

void TouchControls::layout() {
  view_.buttons.clear();
  view_.visible = has_controller_ && w_ > 0 && h_ > 0;
  if (!view_.visible) return;
  // Sticks: 168 px, flush in the corners at the safe insets (smaller only when two do not fit).
  const f32 size = std::clamp(std::min((w_ - sl_ - sr_ - 24) / 2.15f, (h_ - st_ - sb_) * 0.5f), 72.0f, kStick);
  view_.move.size = view_.look.size = size;
  view_.move.x = sl_;
  view_.move.y = h_ - sb_ - size;
  view_.look.x = w_ - sr_ - size;
  view_.look.y = h_ - sb_ - size;
  const bool vehicle = mode_ == ControlMode::hovercar || mode_ == ControlMode::vehicle;
  view_.move.label = vehicle ? "STEER / GAS" : mode_ == ControlMode::flyer ? "FLY" : "MOVE";
  view_.look.label = "LOOK";
  view_.move.accent = false;

  // Buttons per controller: the low row sits left of the look stick (nearest the thumb
  // first), the upper rows above it (edge HUD packing).
  std::vector<TouchButton> low, upper, left;
  constexpr u32 dark = 0x0d1118, amber = 0x2a1d0c, green = 0x0c1a14, blue = 0x0e2a3e;
  switch (mode_) {
    case ControlMode::soldier:
      low.push_back(make_button("jump", "JUMP", 64, dark));
      if (weapon_) low.push_back(make_button("reload", "RELOAD", 60, amber));
      if (weapon_) upper.push_back(make_button("aim", "AIM", 60, green));
      upper.push_back(make_button("crouch", "CROUCH", 60, dark));
      upper.push_back(make_button("interact", "USE", 54, blue));
      break;
    case ControlMode::character:
      if (jump_) low.push_back(make_button("jump", "JUMP", 64, dark));
      upper.push_back(make_button("interact", "USE", 54, blue));
      break;
    case ControlMode::rolling:
      if (jump_) low.push_back(make_button("jump", "JUMP", 64, dark));
      if (boost_) low.push_back(make_button("boost", "BOOST", 60, amber));
      break;
    case ControlMode::flyer:
      low.push_back(make_button("up", "UP", 60, dark));
      low.push_back(make_button("down", "DOWN", 60, dark));
      if (boost_) upper.push_back(make_button("boost", "BOOST", 60, amber));
      break;
    case ControlMode::hovercar:
    case ControlMode::vehicle:
      if (boost_) low.push_back(make_button("boost", "BOOST", 64, amber));
      if (jump_) low.push_back(make_button("jump", "HOP", 56, dark));
      upper.push_back(make_button("interact", "USE", 54, blue));
      break;
  }
  const TouchStick& L = view_.move;
  const TouchStick& R = view_.look;
  // Low row: right to left from the look stick.
  f32 x = R.x - kGap;
  for (TouchButton& b : low) {
    b.x = x - b.w;
    b.y = h_ - std::max(kEdge, sb_) - b.h;
    x -= b.w + kGap;
    view_.buttons.push_back(b);
  }
  // Upper rows above the look stick, packed leftwards from the right edge.
  auto pack = [&](std::vector<TouchButton>& items, f32 x0, f32 bottom, f32 dir, f32 max_width) {
    f32 cx = x0, row_h = 0, y_bottom = bottom, used = 0;
    for (TouchButton& b : items) {
      if (used > 0 && used + b.w > max_width) { y_bottom -= row_h + kGap; cx = x0; used = 0; row_h = 0; }
      b.x = dir > 0 ? cx : cx - b.w;
      b.y = y_bottom - b.h;
      cx += dir * (b.w + kGap);
      used += b.w + kGap;
      row_h = std::max(row_h, b.h);
      view_.buttons.push_back(b);
    }
  };
  pack(upper, w_ - std::max(kEdge, sr_), R.y - kGap, -1, std::max(R.size, 190.0f));
  pack(left, std::max(kEdge, L.x + 10), L.y - kGap, 1, std::max(L.size, 190.0f));
  for (TouchButton& b : view_.buttons) {
    if (b.id == "aim") b.on = aim_on_;
    if (b.id == "crouch") b.on = crouch_on_;
  }
}

bool TouchControls::in_stick(const TouchStick& s, f32 x, f32 y) const {
  const f32 r = s.size * 0.5f;
  return std::hypot(x - (s.x + r), y - (s.y + r)) <= r;  // the drawn circle only (no catch zone)
}

TouchButton* TouchControls::button_at(f32 x, f32 y) {
  for (TouchButton& b : view_.buttons) {
    if (b.round) {
      const f32 r = b.w * 0.5f;
      if (std::hypot(x - (b.x + r), y - (b.y + b.h * 0.5f)) <= r + 4) return &b;
    } else if (x >= b.x - 4 && x <= b.x + b.w + 4 && y >= b.y - 4 && y <= b.y + b.h + 4) {
      return &b;
    }
  }
  return nullptr;
}

void TouchControls::set_pressed(const std::string& id, bool pressed) {
  for (TouchButton& b : view_.buttons)
    if (b.id == id) b.pressed = pressed;
}

bool TouchControls::pointer(u32 type, i32 id, f32 x, f32 y, f64 time_ms) {
  if (!view_.visible) return false;
  auto it = std::find_if(fingers_.begin(), fingers_.end(), [&](const Finger& f) { return f.id == id; });
  if (type == 0) {
    if (it != fingers_.end()) fingers_.erase(it);
    Finger f;
    f.id = id;
    f.x0 = f.x = f.last_x = x;
    f.y0 = f.y = f.last_y = y;
    f.last_t = time_ms;
    if (TouchButton* b = button_at(x, y)) {
      f.kind = Finger::button;
      f.button_id = b->id;
      b->pressed = true;
      if (b->id == "aim") { aim_on_ = !aim_on_; b->on = aim_on_; }
      if (b->id == "crouch") { crouch_on_ = !crouch_on_; b->on = crouch_on_; }
    } else if (alive_ && in_stick(view_.move, x, y)) {
      // The newest finger takes the stick over; deflection counts from where it lands.
      fingers_.erase(std::remove_if(fingers_.begin(), fingers_.end(), [](const Finger& g) { return g.kind == Finger::move; }), fingers_.end());
      f.kind = Finger::move;
      f.base_x = move_x_;
      f.base_y = move_y_;
      view_.move.live = true;
    } else if (alive_ && in_stick(view_.look, x, y)) {
      fingers_.erase(std::remove_if(fingers_.begin(), fingers_.end(), [](const Finger& g) { return g.kind == Finger::look; }), fingers_.end());
      f.kind = Finger::look;
      look_x_ = look_y_ = look_edge_ = 0;
      view_.look.live = true;
    } else {
      // The screen: soldiers fire (and the finger keeps aiming), everything else looks around.
      f.kind = mode_ == ControlMode::soldier && weapon_ && alive_ ? Finger::fire : Finger::look_drag;
      if (f.kind == Finger::fire) { view_.fire = true; view_.fire_x = x; view_.fire_y = y; }
    }
    fingers_.push_back(f);
    return true;
  }
  if (it == fingers_.end()) return false;
  Finger& f = *it;
  if (type == 1) {
    const f32 dx = x - f.last_x, dy = y - f.last_y;
    const f64 dt_ms = std::max(1.0, time_ms - f.last_t);
    f.x = x;
    f.y = y;
    switch (f.kind) {
      case Finger::move: {
        const f32 r = std::max(1.0f, view_.move.size * kRadiusK);
        f32 vx = f.base_x + (x - f.x0) / r, vy = f.base_y + (y - f.y0) / r;
        const f32 m = std::hypot(vx, vy);
        if (m > 1) { vx /= m; vy /= m; }
        move_x_ = vx;
        move_y_ = vy;
        break;
      }
      case Finger::look: {
        // (a) 1:1 drag with flick gain.
        if (dx != 0 || dy != 0) {
          const f32 speed = static_cast<f32>(std::hypot(dx, dy) / dt_ms * 1000.0);
          const f32 gain = 1 + 0.35f * smoothstep01((speed - 700) / 2200);
          drag_yaw_ += dx * kYawPerPx * gain;
          drag_pitch_ += dy * kPitchPerPx * gain;
        }
        // (b) edge turn past the ring; the origin is dragged along so pulling back stops at once.
        const f32 R = view_.look.size * kRadiusK;
        const f32 cap = R * kEdgeStart + kEdgeRamp;
        f32 ox = x - f.x0, oy = y - f.y0, dist = std::hypot(ox, oy);
        if (dist > cap) {
          const f32 pull = (dist - cap) / dist;
          f.x0 += ox * pull;
          f.y0 += oy * pull;
          ox *= 1 - pull;
          oy *= 1 - pull;
          dist = cap;
        }
        look_edge_ = std::clamp((dist - kEdgeStart * R) / kEdgeRamp, 0.0f, 1.0f);
        look_x_ = dist > 0 ? ox / dist : 0;
        look_y_ = dist > 0 ? oy / dist : 0;
        const f32 m = std::min(1.0f, dist / R);
        view_.look.knob_x = look_x_ * m;
        view_.look.knob_y = look_y_ * m;
        break;
      }
      case Finger::fire:
      case Finger::look_drag: {
        if (dx != 0 || dy != 0) {
          const f32 speed = static_cast<f32>(std::hypot(dx, dy) / dt_ms * 1000.0);
          const f32 gain = 1 + 0.35f * smoothstep01((speed - 700) / 2200);
          drag_yaw_ += dx * kYawPerPx * gain;
          drag_pitch_ += dy * kPitchPerPx * gain;
        }
        if (f.kind == Finger::fire) { view_.fire_x = x; view_.fire_y = y; }
        break;
      }
      case Finger::button: break;
    }
    f.last_x = x;
    f.last_y = y;
    f.last_t = time_ms;
    return true;
  }
  // up / cancel
  switch (f.kind) {
    case Finger::move: move_x_ = move_y_ = 0; view_.move.live = false; break;
    case Finger::look:
      look_x_ = look_y_ = look_edge_ = 0;
      view_.look.knob_x = view_.look.knob_y = 0;
      view_.look.live = false;
      break;
    case Finger::button: set_pressed(f.button_id, false); break;
    default: break;
  }
  fingers_.erase(it);
  view_.fire = std::any_of(fingers_.begin(), fingers_.end(), [](const Finger& g) { return g.kind == Finger::fire; });
  return true;
}

void TouchControls::release_all() {
  fingers_.clear();
  move_x_ = move_y_ = look_x_ = look_y_ = look_edge_ = 0;
  drag_yaw_ = drag_pitch_ = 0;
  view_.move.live = view_.look.live = false;
  view_.move.knob_x = view_.move.knob_y = view_.look.knob_x = view_.look.knob_y = 0;
  view_.fire = false;
  for (TouchButton& b : view_.buttons) b.pressed = false;
}

void TouchControls::update(f32 dt, PlayInput& in, f32 vfov_deg) {
  in = PlayInput{};
  if (!view_.visible) return;
  dt = std::clamp(dt, 0.0f, 0.05f);
  // Zoomed views (aiming, scopes) turn slower, like SpawnCity's / adsZoom.
  const f32 zoom = std::clamp(vfov_deg / base_fov_, 0.1f, 1.0f);
  // Move stick: analog, sprint when pushed to the rim forwards.
  const f32 m = std::min(1.0f, std::hypot(move_x_, move_y_));
  sprint_ = m >= 0.9f && move_y_ < -0.35f;
  in.move_x = move_x_;
  in.move_y = -move_y_;
  view_.move.knob_x = move_x_;
  view_.move.knob_y = move_y_;
  view_.move.accent = sprint_ && (mode_ == ControlMode::character || mode_ == ControlMode::soldier);
  // Look: the drag of this frame plus the edge turn.
  f32 yaw = drag_yaw_, pitch = drag_pitch_;
  drag_yaw_ = drag_pitch_ = 0;
  if (look_edge_ > 0) {
    f32 sx = look_x_ * look_edge_, sy = look_y_ * look_edge_;
    shape_stick(sx, sy);
    yaw += sx * kYawRate * dt;
    pitch += sy * kPitchRate * dt;
  }
  in.look_x = yaw * zoom;
  in.look_y = pitch * zoom;
  auto held = [&](const char* id) {
    for (const TouchButton& b : view_.buttons)
      if (b.id == id && b.pressed) return true;
    return false;
  };
  in.jump = held("jump");
  in.reload = held("reload");
  in.interact = held("interact");
  in.boost = held("boost") || (sprint_ && (mode_ == ControlMode::character || mode_ == ControlMode::soldier));
  in.up = (held("up") ? 1.0f : 0.0f) - (held("down") ? 1.0f : 0.0f);
  in.aim = aim_on_;
  in.crouch = crouch_on_;
  in.fire = view_.fire;
}

}  // namespace kern::game
