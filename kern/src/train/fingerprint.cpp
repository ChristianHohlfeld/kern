// determinism_fingerprint (env.h).
#include <cstring>

#include "kern/game/walker_policy.h"
#include "kern/train/env.h"

namespace kern::train {
namespace {

class WalkTask : public Task {
 public:
  std::string name() const override { return "fingerprint"; }
  f32 episode_seconds() const override { return 1e6f; }
  Start reset(Rng&) override { return {}; }
  void command(f32 o[kObs], i32 step) override {
    for (i32 i = 48; i < kObs; ++i) o[i] = 0;
    o[48] = step < 150 ? 0.15f : 0.0f;  // forward, then turn on the spot
    o[50] = step < 150 ? 0.0f : 0.8f;
  }
  f32 reward(const Tick&, bool& terminal) override { terminal = false; return 0; }
};

u64 mix(u64 h, const void* data, std::size_t n) {
  const u8* p = static_cast<const u8*>(data);
  for (std::size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 1099511628211ull;  // FNV-1a
  return h;
}

}  // namespace

std::vector<u64> determinism_fingerprint(i32 ticks) {
  auto task = std::make_unique<WalkTask>();
  task->randomize = false;
  DuckEnv env(std::move(task), 1, game::kWalkerSubsteps);
  env.reset();
  auto net = game::walker_policy("velstand.onnx");
  std::vector<u64> out;
  for (i32 t = 0; t < ticks; ++t) {
    f32 a[kActions];
    net->run(env.observation(), a);
    env.step(a);
    const auto& p = env.probe();
    u64 h = 1469598103934665603ull;
    h = mix(h, &p.position, sizeof p.position);
    h = mix(h, &p.rotation, sizeof p.rotation);
    h = mix(h, &p.velocity, sizeof p.velocity);
    h = mix(h, &p.spin, sizeof p.spin);
    h = mix(h, p.joint, sizeof p.joint);
    h = mix(h, p.joint_speed, sizeof p.joint_speed);
    out.push_back(h);
  }
  return out;
}

}  // namespace kern::train
