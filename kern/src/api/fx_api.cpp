// C ABI of the particle system (one system per module, drawing into a
// renderer mesh chosen by the host).
#include "kern/api/c_api.h"
#include "kern/gfx/particles.h"

using namespace kern;

namespace {
Particles g_particles;
bool g_ready = false;
}  // namespace

/// (Re)creates the particle system drawing into mesh `mesh_id` (allocated by the host).
KERN_EXPORT(kern_fx_init) i32 kern_fx_init(u32 mesh_id, u32 capacity) {
  g_ready = g_particles.init(c_api_renderer(), capacity, mesh_id);
  return g_ready ? 1 : 0;
}

/// spec (f64 x 16): position xyz, velocity xyz, rgba, start_ms, life_ms, size, gravity, grow, fade_extra.
/// Returns the particle id, 0 when dropped (budget) or invalid (see kern_last_error).
KERN_EXPORT(kern_fx_emit) u32 kern_fx_emit(const f64* s) {
  if (!g_ready) return 0;
  ParticleSpec p;
  p.position = {static_cast<f32>(s[0]), static_cast<f32>(s[1]), static_cast<f32>(s[2])};
  p.velocity = {static_cast<f32>(s[3]), static_cast<f32>(s[4]), static_cast<f32>(s[5])};
  p.color = {static_cast<f32>(s[6]), static_cast<f32>(s[7]), static_cast<f32>(s[8]), static_cast<f32>(s[9])};
  p.start_ms = s[10];
  p.life_ms = static_cast<f32>(s[11]);
  p.size = static_cast<f32>(s[12]);
  p.gravity = static_cast<f32>(s[13]);
  p.grow = static_cast<f32>(s[14]);
  p.fade_extra = static_cast<f32>(s[15]);
  return g_particles.emit(p);
}

/// out (u32 x 5): active, dropped, uploaded, expired, mesh visible.
KERN_EXPORT(kern_fx_tick) void kern_fx_tick(f64 now_ms, f32 eye_x, f32 eye_y, f32 eye_z, u32* out) {
  ParticleStats s = g_ready ? g_particles.tick(now_ms, {eye_x, eye_y, eye_z}) : ParticleStats{};
  out[0] = s.active; out[1] = s.dropped; out[2] = s.uploaded; out[3] = s.expired;
  const Mesh* m = g_ready ? c_api_renderer().mesh(g_particles.mesh_id()) : nullptr;
  out[4] = m && m->visible ? 1 : 0;
}

KERN_EXPORT(kern_fx_active) u32 kern_fx_active() { return g_ready ? g_particles.active() : 0; }
KERN_EXPORT(kern_fx_dropped) u32 kern_fx_dropped() { return g_ready ? g_particles.stats().dropped : 0; }
KERN_EXPORT(kern_fx_clear) void kern_fx_clear() { if (g_ready) g_particles.clear(); }
