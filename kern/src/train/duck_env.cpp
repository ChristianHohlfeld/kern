// The trainer's environment: one Micro Duck in its own headless GameWorld (see env.h).
#include "kern/train/env.h"

#include <cmath>
#include <cstring>

#include "kern/game/walker.h"
#include "kern/gfx/renderer.h"

namespace kern::train {

struct DuckEnv::Impl {
  Renderer renderer;
  std::unique_ptr<game::GameWorld> world;
  game::Level base;
  Impl() {
    renderer.set_headless();
    world = std::make_unique<game::GameWorld>(renderer);
  }
  ~Impl() { world.reset(); }
};

DuckEnv::DuckEnv(std::unique_ptr<Task> task, u64 seed) : impl_(std::make_unique<Impl>()), task_(std::move(task)), rng_(seed) {
  game::Level& l = impl_->base;
  l.name = "Training";
  l.world.step_hz = game::kWalkerStepHz;
  l.world.substeps = 16;
  l.world.ground = true;
  l.world.ground_friction = 1.0f;  // as the example level's floor
  duck_ = game::add_micro_duck(l, {0, 0, 0});
  l.find(duck_)->controller->autonomous = false;
  l.world.player = 0;
  max_steps_ = max_of(1, static_cast<i32>(std::lround(task_->episode_seconds() / kControlDt)));
  obs_.assign(kObs, 0.0f);
  critic_.assign(static_cast<std::size_t>(critic_size()), 0.0f);
}

DuckEnv::~DuckEnv() = default;

game::GameWorld& DuckEnv::world() { return *impl_->world; }

i32 DuckEnv::push_interval() {
  const Task::Noise& n = task_->noise;
  return static_cast<i32>(std::lround(rng_.uniform(n.push_every_min, n.push_every_max) / kControlDt));
}

void DuckEnv::reset() {
  const Task::Start s = task_->reset(rng_);
  game::Level l = impl_->base;
  game::Actor* trunk = l.find(duck_);
  trunk->transform.rotation = {s.roll, s.pitch, s.yaw};
  trunk->transform.position.z += s.dz * game::kWalkerScale;
  for (game::Actor& a : l.actors)
    for (game::ColliderComponent& c : a.colliders)
      if (c.impact_sound == "builtin:plastic_step") c.friction *= s.foot_friction;  // the soles
  game::GameWorld& w = *impl_->world;
  w.start(l, game::SimMode::simulate);
  w.set_walker_external(duck_, true);
  step_ = 0;
  return_ = 0;
  std::memset(previous_action_, 0, sizeof previous_action_);
  std::memset(last_action_, 0, sizeof last_action_);
  const Task::Noise& n = task_->noise;
  for (f32& b : bias_) b = task_->randomize ? rng_.uniform(-n.encoder_bias, n.encoder_bias) : 0.0f;
  next_push_ = push_interval();
  w.walker_probe(duck_, probe_);
  observe();
}

void DuckEnv::observe() {
  game::GameWorld& w = *impl_->world;
  w.walker_observation(duck_, obs_.data());
  task_->command(obs_.data());
  // The critic sees the clean state; the actor what the robot's sensors give (noise, bias).
  std::memcpy(critic_.data(), obs_.data(), sizeof(f32) * kObs);
  if (task_->privileged()) task_->privileged(probe_, critic_.data() + kObs);
  if (task_->randomize) {
    const Task::Noise& n = task_->noise;
    for (i32 i = 0; i < 3; ++i) obs_[static_cast<std::size_t>(i)] += rng_.uniform(-n.gyro, n.gyro);
    for (i32 i = 3; i < 6; ++i) obs_[static_cast<std::size_t>(i)] += rng_.uniform(-n.gravity, n.gravity);
    for (i32 j = 0; j < kActions; ++j) {
      obs_[static_cast<std::size_t>(6 + j)] += bias_[j] + rng_.uniform(-n.joint_pos, n.joint_pos);
      obs_[static_cast<std::size_t>(20 + j)] += rng_.uniform(-n.joint_vel, n.joint_vel);
    }
  }
}

Env::Step DuckEnv::step(const f32* action) {
  game::GameWorld& w = *impl_->world;
  std::memcpy(previous_action_, last_action_, sizeof previous_action_);
  for (i32 j = 0; j < kActions; ++j) last_action_[j] = std::isfinite(action[j]) ? action[j] : 0.0f;
  w.set_walker_action(duck_, last_action_);
  // One control tick: the fixed steps until the next one is due (4 at the walker rate).
  i32 guard = 0;
  do {
    w.step_once();
  } while (!w.walker_tick_due(duck_) && ++guard < 64);
  ++step_;
  if (task_->randomize && step_ >= next_push_) {
    const f32 p = task_->noise.push * push_scale;
    w.walker_push(duck_, {rng_.uniform(-p, p), rng_.uniform(-p, p), 0});
    next_push_ = step_ + push_interval();
  }
  w.walker_probe(duck_, probe_);
  Step r;
  r.reward = task_->reward(Tick{probe_, last_action_, previous_action_, step_}, r.terminal);
  if (!std::isfinite(r.reward)) { r.reward = 0; r.terminal = true; }
  r.timeout = !r.terminal && step_ >= max_steps_;
  return_ += r.reward;
  observe();
  return r;
}

}  // namespace kern::train
