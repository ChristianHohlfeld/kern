// The Lehrer's environment: one Micro Duck in its own headless GameWorld (see env.h).
#include "kern/lehrer/env.h"

#include <cmath>
#include <cstring>

#include "kern/game/servo.h"
#include "kern/game/walker.h"
#include "kern/gfx/renderer.h"

namespace kern::lehrer {

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

DuckEnv::DuckEnv(std::unique_ptr<Task> task, u64 seed, i32 substeps) : impl_(std::make_unique<Impl>()), task_(std::move(task)), rng_(seed) {
  game::Level& l = impl_->base;
  l.name = "Training";
  l.world.step_hz = game::kWalkerStepHz;
  l.world.ground = true;
  l.world.ground_friction = 1.0f;  // as the example level's floor
  duck_ = game::add_micro_duck(l, {0, 0, 0});
  if (substeps > 0) l.world.substeps = substeps;
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
  const Task::Noise& n = task_->noise;
  if (task_->randomize) {
    game::DuckVariation v;
    v.trunk_mass = rng_.uniform(n.mass_min, n.mass_max);
    v.trunk_com = {rng_.uniform(-n.com, n.com), rng_.uniform(-n.com, n.com), rng_.uniform(-n.com, n.com)};
    v.head_com = {rng_.uniform(-n.head_com, n.head_com), rng_.uniform(-n.head_com, n.head_com), rng_.uniform(-n.head_com, n.head_com)};
    game::vary_micro_duck(l, duck_, v);
    // Each servo its own (they differ from unit to unit), the bus one delay.
    auto spread = [&](f32 value, f32 rel) { return rel > 0 ? value * rng_.uniform(1 - rel, 1 + rel) : value; };
    for (i32 link = 1; link <= kActions + 1; ++link) {
      game::ServoProfile p = servo;
      p.stiffness = spread(servo.stiffness, servo.spread_stiffness);
      p.damping = spread(servo.damping, servo.spread_damping);
      p.friction = spread(servo.friction, servo.spread_friction);
      p.torque = spread(servo.torque, servo.spread_torque);
      p.armature = spread(servo.armature, servo.spread_armature);
      game::set_micro_duck_servo(l, duck_, link, p);
    }
    l.find(duck_)->controller->servo_delay = servo.delay_max > servo.delay_min ? rng_.uniform(servo.delay_min, servo.delay_max) : servo.delay;
  } else {
    game::set_micro_duck_servos(l, duck_, servo);
  }
  game::GameWorld& w = *impl_->world;
  w.start(l, game::SimMode::simulate);
  w.set_walker_external(duck_, true);
  step_ = 0;
  return_ = 0;
  std::memset(previous_action_, 0, sizeof previous_action_);
  std::memset(last_action_, 0, sizeof last_action_);
  imu_lag_ = imu_lag_left_ = 0;
  for (f32& b : bias_) b = task_->randomize ? rng_.uniform(-n.encoder_bias, n.encoder_bias) : 0.0f;
  next_push_ = push_interval();
  w.walker_probe(duck_, probe_);
  observe();
}

void DuckEnv::observe() {
  game::GameWorld& w = *impl_->world;
  w.walker_observation(duck_, obs_.data());
  task_->command(obs_.data(), step_);
  // The critic sees the clean state; the actor what the robot's sensors give (noise, bias).
  std::memcpy(critic_.data(), obs_.data(), sizeof(f32) * kObs);
  if (task_->privileged()) task_->privileged(probe_, critic_.data() + kObs);
  if (task_->randomize) {
    const Task::Noise& n = task_->noise;
    // The IMU's latency: this tick's reading or the last one (re-drawn every imu_lag_period ticks).
    if (--imu_lag_left_ <= 0) {
      imu_lag_ = n.imu_lag > 0 ? static_cast<i32>(rng_.next() % static_cast<u64>(n.imu_lag + 1)) : 0;
      imu_lag_left_ = max_of(1, n.imu_lag_period);
    }
    f32 now[6];
    std::memcpy(now, obs_.data(), sizeof now);
    if (imu_lag_ > 0 && step_ > 0) std::memcpy(obs_.data(), imu_, sizeof imu_);
    std::memcpy(imu_, now, sizeof imu_);
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

}  // namespace kern::lehrer
