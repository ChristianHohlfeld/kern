// C ABI of Kern Studio for the browser page (kern/web/studio/) and exported
// games (kern/web/studio/game.mjs).
//
// Strings go in as (pointer, length) pairs in linear memory; JSON results come
// back as a pointer to a NUL-terminated buffer that stays valid until the next
// call (kern_studio_result_length() gives its size).
#include <memory>

#include "kern/api/c_api.h"
#include "kern/studio/studio.h"

using namespace kern;

namespace {

struct StudioHost {
  Renderer renderer;
  game::NightWindows windows;
  std::unique_ptr<studio::Studio> studio;
  std::string result;
  bool ready = false;
};

StudioHost* g_host = nullptr;

bool live() { return g_host && g_host->ready; }

const char* answer(std::string text) {
  g_host->result = std::move(text);
  return g_host->result.c_str();
}
const char* answer(const json::Value& v) { return answer(json::stringify(v)); }
std::string_view text(const char* p, u32 n) { return p && n ? std::string_view(p, n) : std::string_view(); }

}  // namespace

/// Creates the renderer (drawing buffer width x height) and the studio.
/// player_only = 1 starts the runtime of exported games (no editor, no level).
KERN_EXPORT(kern_studio_init) i32 kern_studio_init(i32 width, i32 height, i32 player_only) {
  if (!g_host) g_host = new StudioHost();
  if (g_host->ready) return 1;
  RendererOptions o;
  o.width = width > 0 ? width : 960;
  o.height = height > 0 ? height : 540;
  o.shadows = true;
  o.shadow.resolution = 2048;
  o.shadow.extent = 110;
  o.shadow.bias = 0.0014f;
  o.bloom.enabled = true;
  ShaderExtension* ext[1] = {&g_host->windows};
  if (!g_host->renderer.init(o, Span<ShaderExtension* const>(ext, 1))) return 0;
  g_host->studio = std::make_unique<studio::Studio>();
  if (!g_host->studio->init(&g_host->renderer, &g_host->windows, player_only != 0)) return 0;
  g_host->ready = true;
  return 1;
}

KERN_EXPORT(kern_studio_resize) i32 kern_studio_resize(i32 width, i32 height) {
  return live() && g_host->renderer.resize(width, height) ? 1 : 0;
}

KERN_EXPORT(kern_studio_result_length) u32 kern_studio_result_length() { return g_host ? static_cast<u32>(g_host->result.size()) : 0; }

KERN_EXPORT(kern_studio_command) const char* kern_studio_command(const char* json_text, u32 length) {
  if (!live()) return nullptr;
  json::Value cmd;
  std::string err;
  json::ParseOptions po;
  po.max_depth = 120;
  if (!json::parse(text(json_text, length), cmd, &err, po)) {
    json::Value r = json::Value::object();
    r.set("ok", false);
    r.set("error", "invalid command: " + err);
    return answer(r);
  }
  return answer(g_host->studio->command(cmd));
}

KERN_EXPORT(kern_studio_state) const char* kern_studio_state() { return live() ? answer(g_host->studio->state()) : nullptr; }
KERN_EXPORT(kern_studio_status) const char* kern_studio_status() { return live() ? answer(g_host->studio->status()) : nullptr; }
KERN_EXPORT(kern_studio_revision) f64 kern_studio_revision() { return live() ? static_cast<f64>(g_host->studio->revision()) : 0; }
KERN_EXPORT(kern_studio_take_ui) const char* kern_studio_take_ui() { return live() ? answer(g_host->studio->take_ui()) : nullptr; }

KERN_EXPORT(kern_studio_frame) void kern_studio_frame(f64 now_ms) {
  if (live() && !g_host->renderer.lost()) g_host->studio->frame(now_ms);
}

/// type: 0 down, 1 move, 2 up, 3 cancel, 4 wheel, 5 leave; modifiers: 1 shift, 2 ctrl/meta, 4 alt.
KERN_EXPORT(kern_studio_pointer)
const char* kern_studio_pointer(u32 viewport, u32 type, f32 x, f32 y, i32 button, i32 buttons, u32 modifiers, f32 wheel) {
  if (!live()) return nullptr;
  return answer(g_host->studio->pointer(viewport, type, x, y, button, buttons, modifiers, wheel));
}

KERN_EXPORT(kern_studio_look) void kern_studio_look(f32 dx, f32 dy) { if (live()) g_host->studio->look(dx, dy); }

KERN_EXPORT(kern_studio_key)
const char* kern_studio_key(const char* key, u32 key_length, const char* code, u32 code_length, i32 down, u32 modifiers, i32 repeat) {
  if (!live()) return nullptr;
  return answer(g_host->studio->key(text(key, key_length), text(code, code_length), down != 0, modifiers, repeat != 0));
}

KERN_EXPORT(kern_studio_blur) void kern_studio_blur() { if (live()) g_host->studio->blur(); }

KERN_EXPORT(kern_studio_set_viewport)
void kern_studio_set_viewport(u32 index, i32 x, i32 y, i32 w, i32 h, f32 css_w, f32 css_h, f32 dpr, i32 visible) {
  if (live()) g_host->studio->set_viewport_rect(index, studio::Rect{x, y, w, h}, css_w, css_h, dpr, visible != 0);
}

/// Texture ids that need decoded pixels: ["tex1", ...].
KERN_EXPORT(kern_studio_pending_textures) const char* kern_studio_pending_textures() {
  if (!live()) return nullptr;
  json::Value list = json::Value::array();
  for (const std::string& id : g_host->studio->pending_textures()) list.push(id);
  return answer(list);
}

KERN_EXPORT(kern_studio_texture_source) const char* kern_studio_texture_source(const char* id, u32 length) {
  if (!live()) return nullptr;
  return answer(g_host->studio->texture_source(std::string(text(id, length))));
}

KERN_EXPORT(kern_studio_texture_pixels) i32 kern_studio_texture_pixels(const char* id, u32 length, i32 width, i32 height, const u8* rgba) {
  return live() && g_host->studio->texture_pixels(std::string(text(id, length)), width, height, rgba) ? 1 : 0;
}

KERN_EXPORT(kern_studio_take_autosave) const char* kern_studio_take_autosave(f64 now_ms) {
  if (!live()) return nullptr;
  std::string s = g_host->studio->take_autosave(now_ms);
  if (s.empty()) return nullptr;
  return answer(std::move(s));
}

KERN_EXPORT(kern_studio_dirty) i32 kern_studio_dirty() { return live() && g_host->studio->dirty() ? 1 : 0; }

KERN_EXPORT(kern_studio_context_lost) void kern_studio_context_lost() { if (live()) g_host->renderer.context_lost(); }
KERN_EXPORT(kern_studio_context_restored) i32 kern_studio_context_restored() { return live() && g_host->renderer.context_restored() ? 1 : 0; }
