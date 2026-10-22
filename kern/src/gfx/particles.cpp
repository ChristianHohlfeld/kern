#include "kern/gfx/particles.h"

#include <algorithm>

namespace kern {

namespace {

// Unit quad in the XZ plane facing -Y; rotated about Z towards the camera.
constexpr f32 kQuad[18] = {-0.5f, 0, -0.5f, 0.5f, 0, -0.5f, 0.5f, 0, 0.5f, -0.5f, 0, -0.5f, 0.5f, 0, 0.5f, -0.5f, 0, 0.5f};
constexpr f32 kNormals[18] = {0, -1, 0, 0, -1, 0, 0, -1, 0, 0, -1, 0, 0, -1, 0, 0, -1, 0};

bool finite4(Vec4 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) && std::isfinite(v.w); }

}  // namespace

bool Particles::init(Renderer& renderer, u32 capacity, u32 mesh_id) {
  if (capacity < 1 || capacity > 4096) return fail("invalid particle budget");
  renderer_ = &renderer;
  capacity_ = capacity;
  mesh_ = mesh_id ? mesh_id : renderer.allocate_id();
  created_ = visible_ = false;
  alive_.clear();
  alive_.reserve(capacity);
  stats_ = {};
  last_tick_ = -1e300;
  return true;
}

u32 Particles::emit(const ParticleSpec& s) {
  if (!renderer_) return 0;
  if (!finite(s.position) || !finite(s.velocity) || !finite4(s.color) || !std::isfinite(s.start_ms) || !std::isfinite(s.gravity) ||
      !std::isfinite(s.grow) || !std::isfinite(s.fade_extra)) {
    fail("invalid particle");
    return 0;
  }
  if (!(s.life_ms >= 16 && s.life_ms <= 10000) || !(s.size > 0 && s.size < 200) || s.color.x < 0 || s.color.x > 1 || s.color.y < 0 ||
      s.color.y > 1 || s.color.z < 0 || s.color.z > 1 || s.color.w < 0 || s.color.w > 1) {
    fail("invalid particle parameters");
    return 0;
  }
  if (alive_.size() >= capacity_) { stats_.dropped++; return 0; }
  alive_.push_back({s, ++serial_});
  return serial_;
}

ParticleStats Particles::tick(f64 now, Vec3 eye) {
  if (!renderer_) return stats_;
  // Never run time backwards: that would resurrect faded particles after a pause.
  now = max_of(now, last_tick_);
  last_tick_ = now;
  const std::size_t before = alive_.size();
  alive_.erase(std::remove_if(alive_.begin(), alive_.end(), [&](const Particle& p) { return !(now - p.spec.start_ms < p.spec.life_ms); }),
               alive_.end());
  const u32 expired = static_cast<u32>(before - alive_.size());
  stats_.expired += expired;
  if (alive_.empty()) {
    if (visible_) { renderer_->set_visible(mesh_, false); visible_ = false; }
    stats_.active = 0;
    stats_.uploaded = 0;
    return stats_;
  }
  samples_.resize(alive_.size());
  for (u32 i = 0; i < alive_.size(); ++i) {
    const ParticleSpec& s = alive_[i].spec;
    const f32 age = static_cast<f32>(max_of(0.0, now - s.start_ms) / 1000.0);
    const Vec3 p = s.position + s.velocity * age - Vec3{0, 0, 0.5f * s.gravity * age * age};
    samples_[i] = {p, clamp(age * 1000.0f / s.life_ms, 0.0f, 1.0f), distance(p, eye), i};
  }
  // Transparent billboards need back-to-front order inside the batch.
  std::sort(samples_.begin(), samples_.end(), [&](const Sample& a, const Sample& b) {
    if (a.distance != b.distance) return a.distance > b.distance;
    return alive_[a.index].serial < alive_[b.index].serial;
  });
  transforms_.resize(samples_.size() * 16);
  tints_.resize(samples_.size() * 4);
  for (u32 i = 0; i < samples_.size(); ++i) {
    const Sample& smp = samples_[i];
    const ParticleSpec& s = alive_[smp.index].spec;
    const f32 dx = eye.x - smp.position.x, dy = eye.y - smp.position.y;
    const f32 yaw = std::atan2(dx, -dy);
    const f32 radius = s.size * (1 + s.grow * smp.t);
    const Mat4 m = Mat4::trs(smp.position, Quat{0, 0, std::sin(yaw * 0.5f), std::cos(yaw * 0.5f)}, {radius, radius, radius});
    std::memcpy(&transforms_[i * 16], m.m, sizeof m.m);
    const f32 fade = (1 - smp.t) * (1 - s.fade_extra * smp.t);
    tints_[i * 4] = s.color.x; tints_[i * 4 + 1] = s.color.y; tints_[i * 4 + 2] = s.color.z; tints_[i * 4 + 3] = s.color.w * fade;
  }
  const Span<const f32> t(transforms_.data(), transforms_.size()), c(tints_.data(), tints_.size());
  if (!created_ || !renderer_->mesh(mesh_)) {
    MeshInput in;
    in.positions = Span<const f32>(kQuad, 18);
    in.normals = Span<const f32>(kNormals, 18);
    in.transforms = t;
    in.tints = c;
    in.material.alpha = AlphaMode::blend;
    in.material.double_sided = true;
    in.material.unlit = true;
    in.material.cast_shadow = false;
    in.material.frustum_culled = false;
    created_ = renderer_->upsert(mesh_, in);
    visible_ = created_;
  } else {
    if (!visible_) { renderer_->set_visible(mesh_, true); visible_ = true; }
    renderer_->update_instances(mesh_, t, c);
  }
  stats_.active = stats_.uploaded = static_cast<u32>(samples_.size());
  return stats_;
}

void Particles::clear() {
  alive_.clear();
  if (renderer_ && visible_) renderer_->set_visible(mesh_, false);
  visible_ = false;
  last_tick_ = -1e300;
}

bool LightPool::request(Vec3 position, Vec3 color, f32 intensity, f32 range) {
  if (!finite(position) || !finite(color) || !(intensity > 0) || !std::isfinite(intensity) || !(range > 0) || !std::isfinite(range) ||
      requests_.size() >= max_requests_)
    return false;
  requests_.push_back({position, color, min_of(intensity, 100.0f), min_of(range, 1000.0f), 0, static_cast<u32>(requests_.size())});
  return true;
}

const PointLights& LightPool::resolve(Vec3 eye) {
  for (Request& r : requests_) r.score = r.intensity / (1 + distance(r.position, eye) / max_of(4.0f, r.range));
  std::sort(requests_.begin(), requests_.end(), [](const Request& a, const Request& b) {
    return a.score != b.score ? a.score > b.score : a.order < b.order;
  });
  frame_ = PointLights{};
  for (u32 i = 0; i < requests_.size() && i < max_lights_; ++i) {
    const Request& r = requests_[i];
    f32* pr = frame_.pos_range + i * 4;
    f32* ci = frame_.color_intensity + i * 4;
    pr[0] = r.position.x; pr[1] = r.position.y; pr[2] = r.position.z; pr[3] = r.range;
    ci[0] = r.color.x; ci[1] = r.color.y; ci[2] = r.color.z; ci[3] = r.intensity;
    frame_.count = i + 1;
  }
  requests_.clear();
  return frame_;
}

}  // namespace kern
