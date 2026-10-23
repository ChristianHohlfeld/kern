// What the trainer teaches the duck (see env.h, docs/training.md). The reward terms follow the
// shape of Pollen's mjlab tasks (microduck_rl, Apache-2.0): a task term, upright and home-pose
// terms, and the sim2real regularisers (action rate, body rate, self collision); weight x term x dt.
#include <cmath>

#include "kern/train/env.h"

namespace kern::train {
namespace {

constexpr f32 kStandZ = 0.115f;   // trunk height standing at home, real metres (Pollen: STAND_Z)
constexpr f32 kFallUp = -0.5f;    // projected gravity z above this: tipped past 60 degrees (robotd)
constexpr i32 kLegs[10] = {0, 1, 2, 3, 4, 9, 10, 11, 12, 13};
constexpr i32 kNeck[4] = {5, 6, 7, 8};

f32 pose_match(const f32* q, const i32* idx, i32 n, f32 std) {  // exp(-mean squared error / std^2)
  f32 e = 0;
  for (i32 i = 0; i < n; ++i) e += q[idx[i]] * q[idx[i]];
  return std::exp(-e / static_cast<f32>(n) / (std * std));
}
f32 upright(const game::GameWorld::WalkerProbe& p) {  // velocity's recipe: std^2 = 0.05
  return std::exp(-(p.gravity.x * p.gravity.x + p.gravity.y * p.gravity.y) / 0.05f);
}
f32 action_rate(const Tick& t) {
  f32 s = 0;
  for (i32 j = 0; j < kActions; ++j) s += (t.action[j] - t.previous_action[j]) * (t.action[j] - t.previous_action[j]);
  return s;
}
bool fell(const game::GameWorld::WalkerProbe& p) { return p.gravity.z > kFallUp || p.position.z < 0.05f; }

class Terms {
 public:
  explicit Terms(Task& t) : task_(t) { task_.terms.clear(); }
  f32 add(const char* name, f32 weight, f32 term, bool per_step = true) {
    const f32 v = weight * term * (per_step ? kControlDt : 1.0f);
    task_.terms.push_back({name, v});
    sum_ += v;
    return v;
  }
  f32 sum() const { return sum_; }

 private:
  Task& task_;
  f32 sum_ = 0;
};

class StandTask : public Task {
 public:
  std::string name() const override { return "stand"; }
  f32 episode_seconds() const override { return 4; }
  Start reset(Rng& rng) override {
    Start s;
    s.yaw = rng.uniform(-180, 180);
    s.roll = rng.uniform(-3, 3);
    s.pitch = rng.uniform(-3, 3);
    s.dz = rng.uniform(0, 0.005f);
    s.foot_friction = rng.uniform(0.7f, 1.3f);
    return s;
  }
  f32 reward(const Tick& t, bool& terminal) override {
    Terms r(*this);
    const auto& p = t.probe;
    r.add("upright", 2, upright(p));
    r.add("pose_legs", 2, pose_match(p.joint, kLegs, 10, 0.5f));
    r.add("pose_neck", 1, pose_match(p.joint, kNeck, 4, 0.3f));
    r.add("height", 1, std::exp(-(p.position.z - kStandZ) * (p.position.z - kStandZ) / (0.04f * 0.04f)));
    r.add("action_rate", -0.1f, action_rate(t));
    terminal = fell(p);
    if (terminal) r.add("fall", -4, 1, false);
    return r.sum();
  }
};

class JumpTask : public Task {
 public:
  std::string name() const override { return "jump"; }
  f32 episode_seconds() const override { return 3; }
  i32 privileged() const override { return 8; }
  void progress(i32 iteration) override {
    // Pollen's action-rate stages (velocity / kick): -0.1 -> -1.0 by iteration 1500.
    static const std::pair<i32, f32> stages[] = {{0, -0.1f}, {500, -0.2f}, {750, -0.4f}, {1000, -0.6f}, {1250, -0.8f}, {1500, -1.0f}};
    for (const auto& [at, w] : stages) if (iteration >= at) action_rate_weight_ = w;
  }
  Start reset(Rng& rng) override {
    Start s;
    s.yaw = rng.uniform(-180, 180);
    s.roll = rng.uniform(-3, 3);
    s.pitch = rng.uniform(-3, 3);
    s.dz = rng.uniform(0, 0.005f);
    s.foot_friction = rng.uniform(0.7f, 1.3f);
    return s;
  }
  void privileged(const game::GameWorld::WalkerProbe& p, f32* out) override {
    out[0] = p.position.z - kStandZ;
    out[1] = p.velocity.x;
    out[2] = p.velocity.y;
    out[3] = p.velocity.z;
    out[4] = p.foot_contact[0] ? 1.0f : 0.0f;
    out[5] = p.foot_contact[1] ? 1.0f : 0.0f;
    out[6] = p.foot_height[0];
    out[7] = p.foot_height[1];
  }
  f32 reward(const Tick& t, bool& terminal) override {
    Terms r(*this);
    const auto& p = t.probe;
    // In the air: nothing of it touches the floor (a duck on its tail with its feet up is not).
    const bool airborne = !p.foot_contact[0] && !p.foot_contact[1] && !p.body_contact;
    // The jump: both soles off the ground, the trunk high, a push upwards to get there.
    r.add("airborne", 5, airborne ? 1.0f : 0.0f);
    r.add("height", 10, airborne ? clamp((p.position.z - kStandZ) / 0.05f, 0.0f, 2.0f) : 0.0f);
    r.add("lift", 4, clamp(p.velocity.z, 0.0f, 1.0f));
    // Upright through it, and back at home to stand.
    r.add("upright", 2, upright(p));
    r.add("pose_legs", 1, pose_match(p.joint, kLegs, 10, 0.5f));
    r.add("pose_neck", 1, pose_match(p.joint, kNeck, 4, 0.3f));
    // Sim2real regularisers (velocity's weights; the action rate ramps in, below).
    r.add("action_rate", action_rate_weight_, action_rate(t));
    r.add("body_ang_vel", -0.05f, p.spin.x * p.spin.x + p.spin.y * p.spin.y);
    r.add("self_collisions", -1, p.self_contact ? 1.0f : 0.0f);
    r.add("body_contact", -2, p.body_contact ? 1.0f : 0.0f);  // only the soles meet the floor
    terminal = fell(p);
    if (terminal) r.add("fall", -4, 1, false);
    return r.sum();
  }

 private:
  f32 action_rate_weight_ = -0.1f;
};

}  // namespace

std::unique_ptr<Task> make_jump_task() { return std::make_unique<JumpTask>(); }
std::unique_ptr<Task> make_stand_task() { return std::make_unique<StandTask>(); }

}  // namespace kern::train
