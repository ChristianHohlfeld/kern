// Camera-facing particle quads (sparks, smoke, muzzle flashes, blasts) drawn
// as ONE instanced transparent mesh, sorted back to front every tick.
// Purely visual: no collision, no physics world.
#pragma once

#include <vector>

#include "kern/gfx/renderer.h"

namespace kern {

struct ParticleSpec {
  Vec3 position;
  Vec3 velocity;
  Vec4 color{1, 1, 1, 1};  // rgba, alpha fades over the life time
  f64 start_ms = 0;        // emission time (same clock as tick())
  f32 life_ms = 350;       // 16 .. 10000
  f32 size = 0.35f;        // quad edge length in metres at birth
  f32 gravity = 0;         // m/s^2 pulling down (negative lifts)
  f32 grow = 0;            // size multiplier added over the life (1 + grow * t)
  f32 fade_extra = 0;      // extra alpha fade (alpha *= 1 - fade_extra * t)
};

struct ParticleStats {
  u32 active = 0, dropped = 0, uploaded = 0, expired = 0;
};

class Particles {
 public:
  /// Creates the system drawing into mesh `mesh_id` of `renderer` (0 = allocate one).
  bool init(Renderer& renderer, u32 capacity, u32 mesh_id = 0);
  /// Adds a particle; returns its serial id or 0 when the budget is full / input invalid.
  u32 emit(const ParticleSpec& spec);
  /// Advances to `now_ms` (never backwards), drops expired particles and uploads the
  /// sorted billboards facing `eye`. The mesh is hidden while no particle is alive.
  ParticleStats tick(f64 now_ms, Vec3 eye);
  void clear();
  u32 mesh_id() const { return mesh_; }
  u32 active() const { return static_cast<u32>(alive_.size()); }
  const ParticleStats& stats() const { return stats_; }

 private:
  struct Particle {
    ParticleSpec spec;
    u32 serial;
  };
  struct Sample {
    Vec3 position;
    f32 t, distance;
    u32 index;
  };
  Renderer* renderer_ = nullptr;
  u32 mesh_ = 0, capacity_ = 0, serial_ = 0;
  bool created_ = false, visible_ = false;
  f64 last_tick_ = -1e300;
  std::vector<Particle> alive_;
  std::vector<Sample> samples_;
  std::vector<f32> transforms_, tints_;
  ParticleStats stats_;
};

/// Picks the most significant dynamic lights for the renderer's 6 slots:
/// score = intensity / (1 + distance_to_eye / max(4, range)), ties by request order.
class LightPool {
 public:
  explicit LightPool(u32 max_lights = kMaxPointLights, u32 max_requests = 64)
      : max_lights_(min_of(max_lights, kMaxPointLights)), max_requests_(max_requests) {}
  /// Queues a light for the next resolve(); false when invalid or the queue is full.
  bool request(Vec3 position, Vec3 color, f32 intensity, f32 range);
  /// Chooses the lights for this frame and clears the queue.
  const PointLights& resolve(Vec3 eye);
  void clear() { requests_.clear(); }

 private:
  struct Request { Vec3 position, color; f32 intensity, range, score; u32 order; };
  u32 max_lights_, max_requests_;
  std::vector<Request> requests_;
  PointLights frame_;
};

}  // namespace kern
