// C ABI of the Kern editor for the browser page (kern/web/editor/).
//
// Strings go in as (pointer, length) pairs in linear memory; JSON results come
// back as a pointer to a NUL-terminated buffer that stays valid until the next
// call (kern_editor_result_length() gives its size).
#include <memory>

#include "kern/api/c_api.h"
#include "kern/editor/editor.h"

using namespace kern;

namespace {

struct EditorHost {
  Renderer renderer;
  editor::Editor editor;
  std::string result;
  bool ready = false;
};

EditorHost* g_host = nullptr;

const char* answer(std::string text) {
  g_host->result = std::move(text);
  return g_host->result.c_str();
}
const char* answer(const json::Value& v) { return answer(json::stringify(v)); }
std::string_view text(const char* p, u32 n) { return p && n ? std::string_view(p, n) : std::string_view(); }

const char* pointer_type(u32 code) {
  static const char* names[] = {"down", "move", "up", "cancel", "wheel", "leave"};
  return code < 6 ? names[code] : "move";
}

}  // namespace

/// Creates the renderer (drawing buffer width x height) and the editor.
/// player_only = 1 starts the runtime shell of exported games.
KERN_EXPORT(kern_editor_init) i32 kern_editor_init(i32 width, i32 height, i32 player_only) {
  if (!g_host) g_host = new EditorHost();
  if (g_host->ready) return 1;
  RendererOptions o;
  o.width = width > 0 ? width : 960;
  o.height = height > 0 ? height : 540;
  o.shadows = true;
  o.shadow.resolution = 2048;
  o.shadow.extent = 20;
  o.bloom.enabled = false;
  o.grade = false;
  if (!g_host->renderer.init(o)) return 0;
  if (!g_host->editor.init(&g_host->renderer, player_only != 0)) return 0;
  g_host->ready = true;
  return 1;
}

KERN_EXPORT(kern_editor_resize) i32 kern_editor_resize(i32 width, i32 height) {
  return g_host && g_host->ready && g_host->renderer.resize(width, height) ? 1 : 0;
}

KERN_EXPORT(kern_editor_result_length) u32 kern_editor_result_length() { return g_host ? static_cast<u32>(g_host->result.size()) : 0; }

KERN_EXPORT(kern_editor_command) const char* kern_editor_command(const char* json_text, u32 length) {
  if (!g_host || !g_host->ready) return nullptr;
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
  return answer(g_host->editor.command(cmd));
}

KERN_EXPORT(kern_editor_state) const char* kern_editor_state() { return g_host && g_host->ready ? answer(g_host->editor.state()) : nullptr; }
KERN_EXPORT(kern_editor_status) const char* kern_editor_status() { return g_host && g_host->ready ? answer(g_host->editor.status()) : nullptr; }
KERN_EXPORT(kern_editor_ui_revision) f64 kern_editor_ui_revision() { return g_host ? static_cast<f64>(g_host->editor.ui_revision()) : 0; }

KERN_EXPORT(kern_editor_frame) void kern_editor_frame(f64 now_ms) {
  if (g_host && g_host->ready && !g_host->renderer.lost()) g_host->editor.frame(now_ms);
}

/// type: 0 down, 1 move, 2 up, 3 cancel, 4 wheel, 5 leave; pointer_kind: 0 mouse, 1 touch, 2 pen;
/// modifiers: 1 shift, 2 ctrl/meta, 4 alt.
KERN_EXPORT(kern_editor_pointer)
const char* kern_editor_pointer(u32 view, u32 type, f32 x, f32 y, i32 button, i32 buttons, i32 pointer_id, u32 pointer_kind, u32 modifiers, f32 wheel) {
  if (!g_host || !g_host->ready) return nullptr;
  const char* kind = pointer_kind == 1 ? "touch" : pointer_kind == 2 ? "pen" : "mouse";
  return answer(g_host->editor.pointer(view, pointer_type(type), x, y, button, buttons, pointer_id, kind, modifiers & 1, modifiers & 2, modifiers & 4, wheel));
}

KERN_EXPORT(kern_editor_key)
const char* kern_editor_key(const char* key, u32 key_length, const char* code, u32 code_length, i32 down, u32 modifiers, i32 repeat) {
  if (!g_host || !g_host->ready) return nullptr;
  return answer(g_host->editor.key(text(key, key_length), text(code, code_length), down != 0, modifiers & 1, modifiers & 2, modifiers & 4, repeat != 0));
}

KERN_EXPORT(kern_editor_blur) void kern_editor_blur() { if (g_host && g_host->ready) g_host->editor.blur(); }

KERN_EXPORT(kern_editor_set_view_rect)
void kern_editor_set_view_rect(u32 view, i32 x, i32 y, i32 w, i32 h, f32 css_w, f32 css_h, f32 dpr, i32 visible) {
  if (g_host && g_host->ready) g_host->editor.set_view_rect(view, x, y, w, h, css_w, css_h, dpr, visible != 0);
}

KERN_EXPORT(kern_editor_set_mobile) void kern_editor_set_mobile(i32 mobile) { if (g_host) g_host->editor.set_mobile(mobile != 0); }

KERN_EXPORT(kern_editor_pending_images) const char* kern_editor_pending_images() {
  return g_host && g_host->ready ? answer(g_host->editor.pending_images()) : nullptr;
}

KERN_EXPORT(kern_editor_image_source) const char* kern_editor_image_source(const char* key, u32 length) {
  if (!g_host || !g_host->ready) return nullptr;
  const std::string* src = g_host->editor.image_source(text(key, length));
  return answer(src ? *src : std::string());
}

KERN_EXPORT(kern_editor_image_pixels) i32 kern_editor_image_pixels(const char* key, u32 length, i32 width, i32 height, const u8* rgba) {
  return g_host && g_host->ready && g_host->editor.set_image_pixels(text(key, length), width, height, rgba) ? 1 : 0;
}

KERN_EXPORT(kern_editor_take_autosave) const char* kern_editor_take_autosave(f64 now_ms) {
  if (!g_host || !g_host->ready) return nullptr;
  std::string s = g_host->editor.take_autosave(now_ms);
  if (s.empty()) return nullptr;
  return answer(std::move(s));
}

KERN_EXPORT(kern_editor_any_dirty) i32 kern_editor_any_dirty() { return g_host && g_host->editor.any_dirty() ? 1 : 0; }

/// Log lines from index `since` on: [{"message":..., "type":...}, ...].
KERN_EXPORT(kern_editor_log) const char* kern_editor_log(u32 since) {
  if (!g_host || !g_host->ready) return nullptr;
  json::Value out = json::Value::array();
  const auto& log = g_host->editor.log();
  for (std::size_t i = since; i < log.size(); ++i) {
    json::Value l = json::Value::object();
    l.set("message", log[i].message);
    l.set("type", log[i].type);
    out.push(std::move(l));
  }
  return answer(out);
}

KERN_EXPORT(kern_editor_pixels) const u8* kern_editor_pixels() { return g_host && !g_host->editor.last_pixels().empty() ? g_host->editor.last_pixels().data() : nullptr; }
KERN_EXPORT(kern_editor_pixels_length) u32 kern_editor_pixels_length() { return g_host ? static_cast<u32>(g_host->editor.last_pixels().size()) : 0; }

KERN_EXPORT(kern_editor_context_lost) void kern_editor_context_lost() { if (g_host && g_host->ready) g_host->renderer.context_lost(); }
KERN_EXPORT(kern_editor_context_restored) i32 kern_editor_context_restored() { return g_host && g_host->ready && g_host->renderer.context_restored() ? 1 : 0; }
