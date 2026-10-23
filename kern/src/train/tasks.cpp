// What the trainer teaches the duck (see env.h, docs/training.md). The reward terms follow the
// shape of Pollen's mjlab tasks (microduck_rl, Apache-2.0): a task term, upright and home-pose
// terms, and the sim2real regularisers (action rate, body rate, self collision); weight x term x dt.
#include <cmath>

#include "kern/train/env.h"
#include "kern/game/walker.h"

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
  f32 episode_seconds() const override { return game::kJumpSeconds; }
  i32 privileged() const override { return 9; }
  void progress(i32 iteration) override {
    // Curricula: the fall costs little while the move is found and its full price once it forms;
    // the action rate's weight rises as in Pollen's recipes (here over fewer iterations: the policy
    // starts from velstand's balance, not from nothing).
    fall_penalty_ = -2.0f - 18.0f * clamp(static_cast<f32>(iteration) / 300.0f, 0.0f, 1.0f);
    action_rate_weight_ = -0.1f - 0.4f * clamp(static_cast<f32>(iteration - 200) / 600.0f, 0.0f, 1.0f);
    noise.com = 0.003f + 0.012f * clamp(static_cast<f32>(iteration - 200) / 600.0f, 0.0f, 1.0f);
    noise.head_com = 0.003f + 0.007f * clamp(static_cast<f32>(iteration - 200) / 600.0f, 0.0f, 1.0f);
  }
  Start reset(Rng& rng) override {
    Start s;
    s.yaw = rng.uniform(-180, 180);
    s.roll = rng.uniform(-3, 3);
    s.pitch = rng.uniform(-3, 3);
    s.foot_friction = rng.uniform(0.7f, 1.3f);
    flown_ = false;
    have_z0_ = false;
    return s;
  }
  void command(f32 o[kObs], i32 step) override {
    for (i32 i = 48; i < kObs; ++i) o[i] = 0;
    const f32 a = 6.28318531f * phase(step);
    o[game::kJumpPhaseSlot] = std::cos(a);
    o[game::kJumpPhaseSlot + 1] = std::sin(a);
  }
  void privileged(const game::GameWorld::WalkerProbe& p, f32* out) override {
    if (!have_z0_) { z0_ = p.position.z; have_z0_ = true; }
    out[0] = p.position.z - z0_;
    out[1] = p.velocity.x;
    out[2] = p.velocity.y;
    out[3] = p.velocity.z;
    out[4] = p.foot_contact[0] ? 1.0f : 0.0f;
    out[5] = p.foot_contact[1] ? 1.0f : 0.0f;
    out[6] = p.foot_height[0];
    out[7] = p.foot_height[1];
    out[8] = flown_ ? 1.0f : 0.0f;
  }
  /// The trunk's height the jump follows, relative to standing (real metres), over the phase: stand,
  /// crouch, push off, the flight's parabola up to kApex, land, absorb, stand.
  static f32 reference(f32 p) {
    constexpr f32 crouch = -0.015f, absorb = -0.008f;
    if (p < 0.15f) return 0;
    if (p < 0.25f) return crouch * (p - 0.15f) / 0.10f;
    if (p < 0.32f) return crouch + (kApex - crouch) * (p - 0.25f) / 0.07f;
    if (p < 0.40f) return kApex * (1 - (p - 0.32f) / 0.08f);
    if (p < 0.45f) return absorb * (p - 0.40f) / 0.05f;
    if (p < 0.55f) return absorb * (1 - (p - 0.45f) / 0.10f);
    return 0;
  }
  f32 reward(const Tick& t, bool& terminal) override {
    Terms r(*this);
    const auto& p = t.probe;
    if (!have_z0_) { z0_ = p.position.z; have_z0_ = true; }
    const f32 ph = phase(t.step), dz = p.position.z - z0_, up = upright(p);
    // In the air: nothing of it touches the floor (a duck on its tail with its feet up is not).
    const bool airborne = !p.foot_contact[0] && !p.foot_contact[1] && !p.body_contact;
    const bool both = p.foot_contact[0] && p.foot_contact[1] && !p.body_contact;
    if (airborne && dz > 0.01f) flown_ = true;
    const bool takeoff = ph >= 0.22f && ph < 0.33f, flight = ph >= 0.27f && ph < 0.42f;
    // The move: follow the reference height (an imitation of the jump's trunk motion), be in the
    // air when the phase says so and push up into it, land on both soles; every term only as far as
    // the duck stays upright (a dive or a tumble is no jump).
    const f32 e = dz - reference(ph);
    r.add("track_height", 8, up * std::exp(-e * e / (0.012f * 0.012f)));
    r.add("airborne", 6, flight && airborne ? up : 0.0f);
    r.add("lift", 3, takeoff ? up * clamp(p.velocity.z, 0.0f, 1.0f) : 0.0f);
    r.add("landed", 3, ph >= 0.42f && flown_ && both ? up : 0.0f);
    r.add("extra_hop", -4, ph >= 0.5f && airborne ? 1.0f : 0.0f);  // one jump, then it stands
    // Upright, at home before and after, straight up (no drift, no turn).
    r.add("upright", 2, up);
    r.add("pose_legs", 1, ph < 0.15f || ph > 0.55f ? pose_match(p.joint, kLegs, 10, 0.5f) : 0.0f);
    r.add("pose_neck", 1, pose_match(p.joint, kNeck, 4, 0.3f));
    r.add("drift", -2, p.velocity.x * p.velocity.x + p.velocity.y * p.velocity.y);
    // Sim2real regularisers (Pollen's velocity / kick weights).
    r.add("action_rate", action_rate_weight_, action_rate(t));
    r.add("body_ang_vel", -0.05f, p.spin.x * p.spin.x + p.spin.y * p.spin.y);
    r.add("self_collisions", -1, p.self_contact ? 1.0f : 0.0f);
    // Only the soles meet the floor: anything else touching it ends the episode like a fall (as
    // locomotion tasks end on a base contact) - else leaning on the beak or tail is the safe bet.
    terminal = fell(p) || p.body_contact;
    if (terminal) r.add("fall", fall_penalty_, 1, false);
    return r.sum();
  }
  static constexpr f32 kApex = 0.03f;  // real metres above standing at the top of the flight

 private:
  static f32 phase(i32 step) { return static_cast<f32>(step) * kControlDt / game::kJumpSeconds; }
  f32 action_rate_weight_ = -0.1f, fall_penalty_ = -2.0f;
  f32 z0_ = 0;
  bool have_z0_ = false, flown_ = false;
};

}  // namespace

std::unique_ptr<Task> make_jump_task() { return std::make_unique<JumpTask>(); }
std::unique_ptr<Task> make_stand_task() { return std::make_unique<StandTask>(); }

}  // namespace kern::train
