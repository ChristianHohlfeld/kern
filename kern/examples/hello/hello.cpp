// The smallest Kern game: a field of instanced, shadow-casting boxes that
// spin, an orbiting camera (drag / arrow keys) and click-to-pick.
#include <cstdio>
#include <vector>

#include "kern/kern.h"

using namespace kern;

class Hello final : public App {
 public:
  RendererOptions configure() override {
    RendererOptions o;
    o.shadows = true;
    o.shadow.resolution = 1024;
    o.shadow.extent = 40;
    return o;
  }

  bool start(Engine& engine) override {
    Renderer& r = engine.renderer;
    ground_ = r.allocate_id();
    MeshInput g = ground_mesh_.as_input();
    g.material.color = {0.42f, 0.47f, 0.40f, 1};
    if (!r.upsert(ground_, g)) return false;
    boxes_ = r.allocate_id();
    layout(0);
    MeshInput b = box_mesh_.as_input();
    b.material.roughness = 0.45f;
    b.transforms = Span<const f32>(transforms_.data(), transforms_.size());
    b.tints = Span<const f32>(tints_.data(), tints_.size());
    return r.upsert(boxes_, b);
  }

  void update(Engine& engine, const Frame& frame) override {
    Input& in = engine.input;
    if (in.button(0)) yaw_ -= in.pointer_delta().x * 0.005f;
    if (in.down(Key::left)) yaw_ += frame.dt * 1.5f;
    if (in.down(Key::right)) yaw_ -= frame.dt * 1.5f;
    distance_ = clamp(distance_ * (1 + in.wheel() * 0.001f), 8.0f, 80.0f);
    Camera& c = engine.renderer.camera;
    c.target = {0, 0, 1};
    c.eye = {std::cos(yaw_) * distance_, std::sin(yaw_) * distance_, distance_ * 0.6f};

    // Spin every box; one instance upload per frame for all of them.
    layout(static_cast<f32>(frame.time));
    engine.renderer.update_instances(boxes_, Span<const f32>(transforms_.data(), transforms_.size()),
                                     Span<const f32>(tints_.data(), tints_.size()));

    if (in.button_pressed(0)) {
      PickHit hit;
      if (engine.renderer.pick(in.pointer().x, in.pointer().y, {}, hit) && hit.mesh == boxes_) {
        tints_[hit.instance * 4 + 0] = 1.0f; tints_[hit.instance * 4 + 1] = 0.25f; tints_[hit.instance * 4 + 2] = 0.2f;
      }
    }
    char text[96];
    std::snprintf(text, sizeof text, "Kern hello - %u draws, %.0f triangles", engine.stats().draw_calls, engine.stats().triangles);
    engine.set_status(text);
  }

 private:
  void layout(f32 time) {
    for (u32 i = 0; i < kCount; ++i) {
      const f32 angle = time * (0.5f + (i % 7) * 0.15f);
      const Vec3 p{(static_cast<f32>(i % kSide) - kSide * 0.5f) * 3, (static_cast<f32>(i / kSide) - kSide * 0.5f) * 3, 0};
      const Mat4 m = Mat4::trs(p, Quat::axis_angle({0, 0, 1}, angle), {1, 1, 1 + (i % 3) * 0.6f});
      std::memcpy(&transforms_[i * 16], m.m, sizeof m.m);
    }
  }

  static constexpr u32 kSide = 8, kCount = kSide * kSide;
  MeshData ground_mesh_ = primitives::plane(60, 60);
  MeshData box_mesh_ = primitives::box_on_ground({1.4f, 1.4f, 1.4f}, {0.95f, 0.62f, 0.32f});
  std::vector<f32> transforms_ = std::vector<f32>(kCount * 16, 0.0f);
  std::vector<f32> tints_ = std::vector<f32>(kCount * 4, 1.0f);
  u32 ground_ = 0, boxes_ = 0;
  f32 yaw_ = 0.6f, distance_ = 30;
};

App* kern_create_app() {
  static Hello app;
  return &app;
}
