// Application runtime: Input bookkeeping and the C ABI the platform layer
// (kern/web/app.mjs) drives. Only linked into modules that define
// kern_create_app() (the C ABI below references it).
#include "kern/app/app.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "kern/api/c_api.h"

namespace kern {

void Input::key(Key k, bool is_down) {
  u8& s = keys_[static_cast<u32>(k)];
  if (is_down && !(s & kDown)) s |= kDown | kPressed;
  if (!is_down && (s & kDown)) { s &= ~kDown; s |= kReleased; }
}

void Input::pointer_move(Vec2 position, Vec2 delta) {
  pointer_ = position;
  pointer_delta_ += delta;
}

void Input::pointer_buttons(u32 buttons) {
  pressed_buttons_ |= buttons & ~buttons_;
  buttons_ = buttons;
}

void Input::touch(i32 id, i32 phase, Vec2 position) {
  Touch* slot = nullptr;
  for (Touch& t : touches_)
    if (t.id == id) { slot = &t; break; }
  if (phase == 0) {
    if (!slot)
      for (Touch& t : touches_)
        if (t.id < 0) { slot = &t; break; }
    if (!slot) return;
    slot->id = id;
    slot->start = slot->position = slot->previous = position;
  } else if (slot) {
    slot->position = position;
    if (phase == 2) slot->id = -1;
  }
}

u32 Input::touch_count() const {
  u32 n = 0;
  for (const Touch& t : touches_) n += t.id >= 0;
  return n;
}

void Input::set_focus(bool focused) {
  focused_ = focused;
  if (!focused) {
    // Releasing everything on blur prevents stuck keys after alt-tab.
    for (u8& s : keys_) s = (s & kDown) ? kReleased : 0;
    buttons_ = 0;
  }
}

void Input::end_frame() {
  for (u8& s : keys_) s &= kDown;
  pressed_buttons_ = 0;
  pointer_delta_ = {};
  wheel_ = 0;
  for (Touch& t : touches_) t.previous = t.position;
}

}  // namespace kern

#if defined(__wasm__)
#define KERN_ENV(name) __attribute__((import_module("kern_env"), import_name(#name)))
extern "C" {
KERN_ENV(status) void kern_env_status(const char* text, kern::u32 length);
KERN_ENV(pointer_lock) void kern_env_pointer_lock(kern::i32 lock);
}
#else
static void kern_env_status(const char* text, kern::u32 length) { std::printf("[status] %.*s\n", static_cast<int>(length), text); }
static void kern_env_pointer_lock(kern::i32) {}
#endif

namespace kern {

void Engine::set_status(const char* text) {
  static char last[512] = {};
  if (std::strncmp(last, text, sizeof last - 1) == 0) return;
  std::snprintf(last, sizeof last, "%s", text);
  kern_env_status(text, static_cast<u32>(std::strlen(text)));
}

void Engine::request_pointer_lock(bool lock) { kern_env_pointer_lock(lock ? 1 : 0); }

namespace {
std::string url_decode(const char* s, std::size_t n) {
  std::string out;
  out.reserve(n);
  auto hex = [](char c) { return c >= '0' && c <= '9' ? c - '0' : (c | 32) >= 'a' && (c | 32) <= 'f' ? (c | 32) - 'a' + 10 : -1; };
  for (std::size_t i = 0; i < n; ++i) {
    if (s[i] == '+') out += ' ';
    else if (s[i] == '%' && i + 2 < n && hex(s[i + 1]) >= 0 && hex(s[i + 2]) >= 0) { out += static_cast<char>(hex(s[i + 1]) * 16 + hex(s[i + 2])); i += 2; }
    else out += s[i];
  }
  return out;
}
}  // namespace

void Engine::set_args(const char* text, u32 length) {
  args_.clear();
  std::size_t start = 0;
  if (length && text[0] == '?') start = 1;
  while (start < length) {
    std::size_t end = start;
    while (end < length && text[end] != '&') ++end;
    std::size_t eq = start;
    while (eq < end && text[eq] != '=') ++eq;
    if (eq > start) args_.emplace_back(url_decode(text + start, eq - start), eq < end ? url_decode(text + eq + 1, end - eq - 1) : std::string());
    start = end + 1;
  }
}

const char* Engine::arg(const char* key, const char* fallback) const {
  for (const auto& kv : args_)
    if (kv.first == key) return kv.second.c_str();
  return fallback;
}

f64 Engine::arg_number(const char* key, f64 fallback) const {
  const char* v = arg(key);
  if (!v || !*v) return fallback;
  char* end = nullptr;
  const f64 x = std::strtod(v, &end);
  return end && *end == 0 && std::isfinite(x) ? x : fallback;
}

}  // namespace kern

using namespace kern;

namespace {
App* g_app = nullptr;
Engine* g_engine = nullptr;
f64 g_start_ms = -1, g_last_ms = -1;
u64 g_frame = 0;
bool g_running = false;
}  // namespace

Engine& engine_instance() {
  static Engine engine(c_api_renderer());
  return engine;
}

/// Launch parameters, before kern_app_start ("k=v&k2=v2", URL-encoded).
KERN_EXPORT(kern_app_set_args) void kern_app_set_args(const char* text, u32 length) { engine_instance().set_args(text, length); }

KERN_EXPORT(kern_app_start) i32 kern_app_start(i32 width, i32 height) {
  if (g_running) return 1;
  g_app = kern_create_app();
  if (!g_app) { fail("no application"); return 0; }
  Engine& engine = engine_instance();
  g_engine = &engine;
  RendererOptions options = g_app->configure();
  options.width = width;
  options.height = height;
  if (!engine.renderer.init(options, g_app->extensions())) return 0;
  if (!g_app->start(engine)) return 0;
  g_running = true;
  g_start_ms = g_last_ms = -1;
  g_frame = 0;
  return 1;
}

/// One display frame: update + render. Returns 0 when rendering failed.
KERN_EXPORT(kern_app_frame) i32 kern_app_frame(f64 now_ms, i32 page_hidden) {
  if (!g_running) return 0;
  if (g_start_ms < 0) g_start_ms = g_last_ms = now_ms;
  Frame frame;
  frame.time = (now_ms - g_start_ms) / 1000.0;
  frame.dt = static_cast<f32>(clamp((now_ms - g_last_ms) / 1000.0, 0.0, 0.1));
  frame.index = g_frame++;
  g_last_ms = now_ms;
  g_app->update(*g_engine, frame);
  g_engine->input.end_frame();
  if (g_engine->renderer.lost()) return 1;
  RenderParams params;
  params.clear_color = g_engine->clear_color;
  params.page_hidden = page_hidden != 0;
  RenderStats stats;
  return g_engine->renderer.render(params, stats) ? 1 : 0;
}

KERN_EXPORT(kern_app_resize) i32 kern_app_resize(i32 width, i32 height) {
  if (!g_running) return 0;
  if (!g_engine->renderer.resize(width, height)) return 0;
  g_app->resized(*g_engine);
  return 1;
}

KERN_EXPORT(kern_app_key) void kern_app_key(u32 key, i32 down) {
  if (g_engine && key < static_cast<u32>(Key::count)) g_engine->input.key(static_cast<Key>(key), down != 0);
}
KERN_EXPORT(kern_app_pointer) void kern_app_pointer(f32 x, f32 y, f32 dx, f32 dy, u32 buttons) {
  if (!g_engine) return;
  g_engine->input.pointer_move({x, y}, {dx, dy});
  g_engine->input.pointer_buttons(buttons);
}
KERN_EXPORT(kern_app_wheel) void kern_app_wheel(f32 dy) { if (g_engine) g_engine->input.add_wheel(dy); }
KERN_EXPORT(kern_app_touch) void kern_app_touch(i32 id, i32 phase, f32 x, f32 y) { if (g_engine) g_engine->input.touch(id, phase, {x, y}); }
KERN_EXPORT(kern_app_focus) void kern_app_focus(i32 focused) { if (g_engine) g_engine->input.set_focus(focused != 0); }
KERN_EXPORT(kern_app_stop) void kern_app_stop() {
  if (!g_running) return;
  g_app->stop(*g_engine);
  g_engine->renderer.shutdown();
  g_running = false;
}
