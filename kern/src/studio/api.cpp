// C ABI of Kern Studio for the browser page (kern/web/studio/) and exported
// games (kern/web/studio/game.mjs).
//
// Strings go in as (pointer, length) pairs in linear memory; JSON results come
// back as a pointer to a NUL-terminated buffer that stays valid until the next
// call (kern_studio_result_length() gives its size).
#include <memory>
#include <vector>

#include "kern/api/c_api.h"
#include "kern/studio/studio.h"
#include "kern/game/walker_policy.h"
#include "kern/core/json.h"

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

/// A touch or pen finger (type: 0 down, 1 move, 2 up, 3 cancel) in css px of the active viewport.
/// Returns 1 when the engine's touch controls used it.
KERN_EXPORT(kern_studio_touch) i32 kern_studio_touch(u32 type, i32 id, f32 x, f32 y, f64 time_ms) {
  return live() && g_host->studio->touch_pointer(type, id, x, y, time_ms) ? 1 : 0;
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

// ---- robot policies ----

/// The policy files of the Micro Duck's set (manifest.json) not loaded yet: ["roller.onnx", ...].
/// The page fetches them (microduck/<file> next to the editor) and hands them over.
KERN_EXPORT(kern_studio_pending_policies) const char* kern_studio_pending_policies() {
  if (!live()) return nullptr;
  json::Value m, list = json::Value::array();
  bool robot = false;  // only a level with a walker needs them
  for (const game::Actor& a : g_host->studio->level().actors) robot = robot || (a.controller && a.controller->mode == game::ControlMode::walker);
  if (robot && json::parse(game::micro_duck_manifest(), m))
    for (const json::Value& p : m["policies"].items())
      if (!game::walker_policy(p["file"].as_string())) list.push(p["file"].as_string());
  return answer(list);
}

/// One .onnx policy (bytes); 1 when it is an rsl_rl actor Kern can run.
KERN_EXPORT(kern_studio_load_policy) i32 kern_studio_load_policy(const char* file, u32 length, const u8* data, u32 bytes) {
  auto p = std::make_shared<game::WalkerPolicy>();
  std::string error;
  if (!game::parse_onnx_policy(Span<const u8>(data, bytes), *p, &error)) return 0;
  p->file = std::string(text(file, length));
  game::register_walker_policy(std::move(p));
  return 1;
}

// ---- audio ----

/// Starts the mixer at the output device rate; 1 on success.
KERN_EXPORT(kern_studio_audio_init) i32 kern_studio_audio_init(u32 sample_rate) {
  return live() && g_host->studio->audio_init(sample_rate) ? 1 : 0;
}

/// Renders `frames` frames of interleaved stereo; the pointer stays valid until the next call.
KERN_EXPORT(kern_studio_audio_render) const f32* kern_studio_audio_render(u32 frames) {
  if (!live()) return nullptr;
  static std::vector<f32> buffer;
  frames = frames > 16384 ? 16384 : frames;
  buffer.resize(static_cast<std::size_t>(frames) * audio::kOutputChannels);
  g_host->studio->audio_render(buffer.data(), frames);
  return buffer.data();
}

/// Renders queued built-in sounds for about `budget_ms` (the page calls it from a timer, so
/// music is ready quickly even when frames are slow).
KERN_EXPORT(kern_studio_audio_prepare) i32 kern_studio_audio_prepare(f64 budget_ms) {
  if (!live() || !g_host->studio->audio_ready()) return 1;
  return g_host->studio->sound_bank().prepare(budget_ms > 0 ? budget_ms : 2) ? 1 : 0;
}

/// Imported sound ids that need decoded PCM: ["snd1", ...].
KERN_EXPORT(kern_studio_pending_sounds) const char* kern_studio_pending_sounds() {
  if (!live()) return nullptr;
  json::Value list = json::Value::array();
  for (const std::string& id : g_host->studio->pending_sounds()) list.push(id);
  return answer(list);
}

KERN_EXPORT(kern_studio_sound_source) const char* kern_studio_sound_source(const char* id, u32 length) {
  if (!live()) return nullptr;
  return answer(g_host->studio->sound_source(std::string(text(id, length))));
}

/// Decoded PCM (interleaved f32, 1 or 2 channels, any rate).
KERN_EXPORT(kern_studio_sound_pcm)
i32 kern_studio_sound_pcm(const char* id, u32 length, const f32* samples, u32 frames, u32 channels, u32 rate) {
  return live() && g_host->studio->sound_pcm(std::string(text(id, length)), samples, frames, channels, rate) ? 1 : 0;
}

KERN_EXPORT(kern_studio_sound_failed) void kern_studio_sound_failed(const char* id, u32 length, const char* reason, u32 reason_length) {
  if (live()) g_host->studio->sound_failed(std::string(text(id, length)), std::string(text(reason, reason_length)));
}

// ---- network (the page moves datagrams; kern/web/studio/game.mjs KernNet) ----

/// A datagram from peer `from` (a 53-bit id the page assigns).
KERN_EXPORT(kern_studio_net_push) void kern_studio_net_push(f64 from, const u8* data, u32 bytes) {
  if (live() && from > 0) g_host->studio->host_transport().push_incoming(net::Address{static_cast<u64>(from)}, data, static_cast<int>(bytes));
}

namespace {
std::vector<u8> g_out;
f64 g_out_to = 0;
}  // namespace

/// Next datagram to send: its size (0 = none); kern_studio_net_out() / kern_studio_net_out_to() give bytes and target.
KERN_EXPORT(kern_studio_net_take) u32 kern_studio_net_take() {
  if (!live()) return 0;
  net::Address to;
  if (!g_host->studio->host_transport().take_outgoing(to, g_out)) return 0;
  g_out_to = static_cast<f64>(to.id);
  return static_cast<u32>(g_out.size());
}
KERN_EXPORT(kern_studio_net_out) const u8* kern_studio_net_out() { return g_out.data(); }
KERN_EXPORT(kern_studio_net_out_to) f64 kern_studio_net_out_to() { return g_out_to; }

KERN_EXPORT(kern_studio_take_autosave) const char* kern_studio_take_autosave(f64 now_ms) {
  if (!live()) return nullptr;
  std::string s = g_host->studio->take_autosave(now_ms);
  if (s.empty()) return nullptr;
  return answer(std::move(s));
}

KERN_EXPORT(kern_studio_take_autosave_discard) i32 kern_studio_take_autosave_discard() { return live() && g_host->studio->take_autosave_discard() ? 1 : 0; }

KERN_EXPORT(kern_studio_dirty) i32 kern_studio_dirty() { return live() && g_host->studio->dirty() ? 1 : 0; }

KERN_EXPORT(kern_studio_context_lost) void kern_studio_context_lost() { if (live()) g_host->renderer.context_lost(); }
KERN_EXPORT(kern_studio_context_restored) i32 kern_studio_context_restored() { return live() && g_host->renderer.context_restored() ? 1 : 0; }
