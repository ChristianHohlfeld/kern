// PPO as rsl_rl runs it (see ppo.h): the same losses, schedule and bookkeeping, in C++.
#include "kern/train/ppo.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <numeric>

#if !defined(__wasi__)
#include <thread>
#endif

namespace kern::train {
namespace {

constexpr f32 kLog2Pi = 1.83787706641f;
constexpr f32 kMinStd = 1e-3f;

f64 seconds_since(std::chrono::steady_clock::time_point t) {
  return std::chrono::duration<f64>(std::chrono::steady_clock::now() - t).count();
}

}  // namespace

ParallelFor thread_pool(i32 threads) {
#if defined(__wasi__)
  (void)threads;
  return [](i32 n, const std::function<void(i32)>& fn) { for (i32 i = 0; i < n; ++i) fn(i); };
#else
  const i32 count = threads > 0 ? threads : max_of(1, static_cast<i32>(std::thread::hardware_concurrency()));
  return [count](i32 n, const std::function<void(i32)>& fn) {
    const i32 workers = min_of(count, n);
    if (workers <= 1) { for (i32 i = 0; i < n; ++i) fn(i); return; }
    std::vector<std::thread> pool;
    pool.reserve(static_cast<std::size_t>(workers));
    for (i32 w = 0; w < workers; ++w)
      pool.emplace_back([&, w]() { for (i32 i = w; i < n; i += workers) fn(i); });
    for (std::thread& t : pool) t.join();
  };
#endif
}

void Policy::act(const f32* observation, f32* action) const {
  std::vector<f32> x(static_cast<std::size_t>(normalizer.size()));
  normalizer.apply(observation, 1, x.data());
  actor.forward(x.data(), 1, action);
}

std::vector<f32> Policy::serialize() const {
  // [layers + 1, dims..., params..., std..., mean..., variance..., count]
  std::vector<f32> out;
  out.push_back(static_cast<f32>(actor.dims().size()));
  for (i32 d : actor.dims()) out.push_back(static_cast<f32>(d));
  out.insert(out.end(), actor.params().begin(), actor.params().end());
  out.insert(out.end(), stddev.begin(), stddev.end());
  out.insert(out.end(), normalizer.mean().begin(), normalizer.mean().end());
  out.insert(out.end(), normalizer.variance().begin(), normalizer.variance().end());
  out.push_back(static_cast<f32>(normalizer.count()));
  return out;
}

bool Policy::deserialize(const f32* data, std::size_t size) {
  std::size_t at = 0;
  auto take = [&]() { return at < size ? data[at++] : 0.0f; };
  const i32 n = static_cast<i32>(take());
  if (n < 2 || n > 16) return false;
  std::vector<i32> dims;
  for (i32 k = 0; k < n; ++k) dims.push_back(static_cast<i32>(take()));
  if (dims != actor.dims()) {
    Rng r(1);
    actor.init(dims, r);
  }
  const std::size_t params = actor.params().size(), in = static_cast<std::size_t>(dims.front()), out = static_cast<std::size_t>(dims.back());
  if (size < at + params + out + 2 * in + 1) return false;
  std::copy(data + at, data + at + params, actor.params().begin());
  at += params;
  stddev.assign(data + at, data + at + out);
  at += out;
  std::vector<f32> mean(data + at, data + at + in);
  at += in;
  std::vector<f32> var(data + at, data + at + in);
  at += in;
  normalizer.set(std::move(mean), std::move(var), data[at]);
  return true;
}

namespace {
template <typename T>
void put(std::vector<u8>& out, const std::vector<T>& v) {
  const u64 n = v.size();
  const u8* p = reinterpret_cast<const u8*>(&n);
  out.insert(out.end(), p, p + 8);
  const u8* d = reinterpret_cast<const u8*>(v.data());
  out.insert(out.end(), d, d + n * sizeof(T));
}
template <typename T>
bool get(const u8* data, std::size_t size, std::size_t& at, std::vector<T>& v) {
  if (at + 8 > size) return false;
  u64 n = 0;
  std::memcpy(&n, data + at, 8);
  at += 8;
  if (n > (size - at) / sizeof(T)) return false;
  v.resize(static_cast<std::size_t>(n));
  if (n) std::memcpy(v.data(), data + at, static_cast<std::size_t>(n) * sizeof(T));
  at += static_cast<std::size_t>(n) * sizeof(T);
  return true;
}
}  // namespace

std::vector<u8> Rollout::serialize() const {
  std::vector<u8> out;
  put(out, std::vector<i32>{envs, steps, obs, critic, actions});
  put(out, std::vector<f64>{seconds});
  put(out, observation);
  put(out, critic_obs);
  put(out, action);
  put(out, reward);
  put(out, terminal);
  put(out, timeout);
  put(out, last_critic);
  put(out, episode_returns);
  put(out, episode_lengths);
  return out;
}

bool Rollout::deserialize(const u8* data, std::size_t size) {
  std::size_t at = 0;
  std::vector<i32> head;
  std::vector<f64> secs;
  if (!get(data, size, at, head) || head.size() != 5 || !get(data, size, at, secs) || secs.size() != 1) return false;
  envs = head[0]; steps = head[1]; obs = head[2]; critic = head[3]; actions = head[4];
  seconds = secs[0];
  const bool ok = get(data, size, at, observation) && get(data, size, at, critic_obs) && get(data, size, at, action) &&
                  get(data, size, at, reward) && get(data, size, at, terminal) && get(data, size, at, timeout) &&
                  get(data, size, at, last_critic) && get(data, size, at, episode_returns) && get(data, size, at, episode_lengths);
  const std::size_t rows = static_cast<std::size_t>(envs) * static_cast<std::size_t>(steps);
  return ok && observation.size() == rows * static_cast<std::size_t>(obs) && critic_obs.size() == rows * static_cast<std::size_t>(critic) &&
         action.size() == rows * static_cast<std::size_t>(actions) && reward.size() == rows && terminal.size() == rows && timeout.size() == rows &&
         last_critic.size() == static_cast<std::size_t>(envs) * static_cast<std::size_t>(critic);
}

void Collector::collect(std::vector<Env*>& envs, i32 steps, const Policy& policy, Rollout& out, const ParallelFor& parallel_in) {
  const ParallelFor parallel = parallel_in ? parallel_in : thread_pool(1);
  const auto t0 = std::chrono::steady_clock::now();
  const i32 N = static_cast<i32>(envs.size()), T = steps;
  const i32 O = envs[0]->obs_size(), C = envs[0]->critic_size(), A = envs[0]->action_size();
  const std::size_t rows = static_cast<std::size_t>(N) * static_cast<std::size_t>(T);
  out.envs = N; out.steps = T; out.obs = O; out.critic = C; out.actions = A;
  out.observation.resize(rows * static_cast<std::size_t>(O));
  out.critic_obs.resize(rows * static_cast<std::size_t>(C));
  out.action.resize(rows * static_cast<std::size_t>(A));
  out.reward.resize(rows);
  out.terminal.resize(rows);
  out.timeout.resize(rows);
  out.last_critic.resize(static_cast<std::size_t>(N) * static_cast<std::size_t>(C));
  out.episode_returns.clear();
  out.episode_lengths.clear();
  while (static_cast<i32>(env_rng_.size()) < N) env_rng_.emplace_back(rng_.next());
  episode_return_.resize(static_cast<std::size_t>(N), 0.0f);
  episode_length_.resize(static_cast<std::size_t>(N), 0);
  std::vector<std::vector<std::pair<f32, i32>>> finished(static_cast<std::size_t>(N));
  // Each environment's steps run on one thread, one after the other (an episode is sequential).
  parallel(N, [&](i32 i) {
    Env& env = *envs[static_cast<std::size_t>(i)];
    Rng& r = env_rng_[static_cast<std::size_t>(i)];
    std::vector<f32> x(static_cast<std::size_t>(O)), mu(static_cast<std::size_t>(A));
    for (i32 t = 0; t < T; ++t) {
      const std::size_t row = static_cast<std::size_t>(t) * static_cast<std::size_t>(N) + static_cast<std::size_t>(i);
      f32* o = out.observation.data() + row * static_cast<std::size_t>(O);
      std::memcpy(o, env.observation(), sizeof(f32) * static_cast<std::size_t>(O));
      std::memcpy(out.critic_obs.data() + row * static_cast<std::size_t>(C), env.critic(), sizeof(f32) * static_cast<std::size_t>(C));
      policy.normalizer.apply(o, 1, x.data());
      policy.actor.forward(x.data(), 1, mu.data());
      f32* a = out.action.data() + row * static_cast<std::size_t>(A);
      for (i32 j = 0; j < A; ++j) a[j] = mu[static_cast<std::size_t>(j)] + policy.stddev[static_cast<std::size_t>(j)] * r.normal();
      const Env::Step st = env.step(a);
      out.reward[row] = st.reward;
      out.terminal[row] = st.terminal ? 1 : 0;
      out.timeout[row] = st.timeout ? 1 : 0;
      episode_return_[static_cast<std::size_t>(i)] += st.reward;
      ++episode_length_[static_cast<std::size_t>(i)];
      if (st.terminal || st.timeout) {
        finished[static_cast<std::size_t>(i)].push_back({episode_return_[static_cast<std::size_t>(i)], episode_length_[static_cast<std::size_t>(i)]});
        episode_return_[static_cast<std::size_t>(i)] = 0;
        episode_length_[static_cast<std::size_t>(i)] = 0;
        env.reset();
      }
    }
    std::memcpy(out.last_critic.data() + static_cast<std::size_t>(i) * C, env.critic(), sizeof(f32) * static_cast<std::size_t>(C));
  });
  for (const auto& f : finished)
    for (const auto& [ret, len] : f) {
      out.episode_returns.push_back(ret);
      out.episode_lengths.push_back(len);
    }
  out.seconds = seconds_since(t0);
}

Ppo::Ppo(const PpoConfig& config, i32 obs_size, i32 critic_size, i32 action_size)
    : cfg_(config), obs_(obs_size), crit_(critic_size), act_(action_size), lr_(config.learning_rate), rng_(config.seed), collector_(config.seed ^ 0x5DEECE66Dull) {
  std::vector<i32> a{obs_size}, c{critic_size};
  for (i32 h : cfg_.actor_hidden) a.push_back(h);
  for (i32 h : cfg_.critic_hidden) c.push_back(h);
  a.push_back(action_size);
  c.push_back(1);
  policy_.actor.init(a, rng_);
  critic_.init(c, rng_);
  policy_.normalizer.init(obs_size);
  critic_norm_.init(critic_size);
  policy_.stddev.assign(static_cast<std::size_t>(action_size), cfg_.init_std);
  actor_adam_.max_norm = critic_adam_.max_norm = cfg_.max_grad_norm;
  actor_adam_.lr = critic_adam_.lr = lr_;
}

IterationStats Ppo::iterate(std::vector<Env*>& envs, const ParallelFor& parallel) {
  collector_.collect(envs, cfg_.steps_per_env, policy_, local_, parallel);
  return learn({&local_});
}

IterationStats Ppo::learn(const std::vector<const Rollout*>& rollouts) {
  IterationStats stats;
  stats.iteration = ++iteration_;
  std::size_t rows = 0;
  f64 reward_sum = 0;
  for (const Rollout* r : rollouts) {
    rows += r->reward.size();
    for (f32 v : r->reward) reward_sum += v;
    for (std::size_t k = 0; k < r->episode_returns.size(); ++k) {
      stats.mean_return += r->episode_returns[k];
      stats.mean_length += static_cast<f32>(r->episode_lengths[k]);
      ++stats.episodes;
    }
    stats.collect_seconds = std::max(stats.collect_seconds, r->seconds);
  }
  if (!rows) return stats;
  if (stats.episodes) { stats.mean_return /= static_cast<f32>(stats.episodes); stats.mean_length /= static_cast<f32>(stats.episodes); }
  stats.mean_reward = static_cast<f32>(reward_sum / static_cast<f64>(rows));
  total_steps_ += static_cast<i64>(rows);
  stats.steps = total_steps_;
  const auto t1 = std::chrono::steady_clock::now();
  // The normalisers learn from what was observed (rsl_rl updates them step by step while collecting;
  // collectors elsewhere keep the policy fixed for a rollout, so the learner does it here).
  for (const Rollout* r : rollouts) {
    policy_.normalizer.update(r->observation.data(), r->envs * r->steps);
    critic_norm_.update(r->critic_obs.data(), r->envs * r->steps);
  }
  // Gather every row; evaluate the old policy and the values under today's normalisers.
  obs_buf_.resize(rows * static_cast<std::size_t>(obs_));
  crit_buf_.resize(rows * static_cast<std::size_t>(crit_));
  act_buf_.resize(rows * static_cast<std::size_t>(act_));
  mu_buf_.resize(rows * static_cast<std::size_t>(act_));
  logp_buf_.resize(rows);
  value_buf_.resize(rows);
  return_buf_.resize(rows);
  adv_buf_.resize(rows);
  std::size_t at = 0;
  for (const Rollout* r : rollouts) {
    const std::size_t n = r->reward.size();
    std::memcpy(obs_buf_.data() + at * obs_, r->observation.data(), sizeof(f32) * n * static_cast<std::size_t>(obs_));
    std::memcpy(crit_buf_.data() + at * crit_, r->critic_obs.data(), sizeof(f32) * n * static_cast<std::size_t>(crit_));
    std::memcpy(act_buf_.data() + at * act_, r->action.data(), sizeof(f32) * n * static_cast<std::size_t>(act_));
    at += n;
  }
  {
    const std::size_t chunk = 256;
    const i32 A = act_;
    std::vector<f32> xo(chunk * static_cast<std::size_t>(obs_)), xcr(chunk * static_cast<std::size_t>(crit_));
    for (std::size_t s = 0; s < rows; s += chunk) {
      const i32 n = static_cast<i32>(std::min(chunk, rows - s));
      policy_.normalizer.apply(obs_buf_.data() + s * obs_, n, xo.data());
      critic_norm_.apply(crit_buf_.data() + s * crit_, n, xcr.data());
      policy_.actor.forward(xo.data(), n, mu_buf_.data() + s * A);
      critic_.forward(xcr.data(), n, value_buf_.data() + s);
      for (i32 k = 0; k < n; ++k) {
        f32 logp = 0;
        for (i32 j = 0; j < A; ++j) {
          const f32 sd = policy_.stddev[static_cast<std::size_t>(j)], d = act_buf_[(s + static_cast<std::size_t>(k)) * A + j] - mu_buf_[(s + static_cast<std::size_t>(k)) * A + j];
          logp += -0.5f * d * d / (sd * sd) - std::log(sd) - 0.5f * kLog2Pi;
        }
        logp_buf_[s + static_cast<std::size_t>(k)] = logp;
      }
    }
  }
  // GAE (lambda) per environment; a time-out is bootstrapped with the value where it stood (rsl_rl).
  at = 0;
  for (const Rollout* r : rollouts) {
    const i32 N = r->envs, T = r->steps;
    std::vector<f32> last(static_cast<std::size_t>(N)), xc(r->last_critic.size());
    critic_norm_.apply(r->last_critic.data(), N, xc.data());
    critic_.forward(xc.data(), N, last.data());
    for (i32 i = 0; i < N; ++i) {
      f32 adv = 0;
      for (i32 t = T - 1; t >= 0; --t) {
        const std::size_t local = static_cast<std::size_t>(t) * N + static_cast<std::size_t>(i), row = at + local;
        const f32 v = value_buf_[row];
        const f32 next = t == T - 1 ? last[static_cast<std::size_t>(i)] : value_buf_[row + static_cast<std::size_t>(N)];
        const f32 reward = r->reward[local] + (r->timeout[local] ? cfg_.gamma * v : 0.0f);
        const f32 live = r->terminal[local] || r->timeout[local] ? 0.0f : 1.0f;
        const f32 delta = reward + live * cfg_.gamma * next - v;
        adv = delta + live * cfg_.gamma * cfg_.lam * adv;
        return_buf_[row] = adv + v;
        adv_buf_[row] = adv;
      }
    }
    at += r->reward.size();
  }
  {
    f64 m = 0, v = 0;
    for (f32 a : adv_buf_) m += a;
    m /= static_cast<f64>(rows);
    for (f32 a : adv_buf_) v += (a - m) * (a - m);
    const f64 sd = std::sqrt(v / static_cast<f64>(rows > 1 ? rows - 1 : 1)) + 1e-8;
    for (f32& a : adv_buf_) a = static_cast<f32>((a - m) / sd);
  }
  update(stats);
  stats.learn_seconds = seconds_since(t1);
  stats.learning_rate = lr_;
  f32 s = 0;
  for (f32 v : policy_.stddev) s += v;
  stats.mean_std = s / static_cast<f32>(policy_.stddev.size());
  return stats;
}

void Ppo::update(IterationStats& stats) {
  const std::size_t rows = logp_buf_.size();
  const i32 mb = max_of(1, cfg_.minibatches);
  const std::size_t size = rows / static_cast<std::size_t>(mb);
  if (!size) return;
  // One permutation for all epochs, as rsl_rl's mini_batch_generator.
  std::vector<u32> perm(rows);
  std::iota(perm.begin(), perm.end(), 0u);
  for (std::size_t k = rows - 1; k > 0; --k) std::swap(perm[k], perm[rng_.next() % (k + 1)]);
  const std::vector<f32> old_std = policy_.stddev;
  const i32 A = act_;
  std::vector<f32> xa(size * static_cast<std::size_t>(obs_)), xc(size * static_cast<std::size_t>(crit_)), ro(xa.size()), rc(xc.size());
  std::vector<f32> mu(size * static_cast<std::size_t>(A)), value(size), dmu(mu.size()), dv(size), dstd(static_cast<std::size_t>(A));
  std::vector<f32> gactor(policy_.actor.params().size()), gcritic(critic_.params().size());
  Mlp::Cache ca, cc;
  f64 vloss_sum = 0, sloss_sum = 0, ent_sum = 0;
  i32 updates = 0;
  for (i32 epoch = 0; epoch < cfg_.epochs; ++epoch) {
    for (i32 b = 0; b < mb; ++b) {
      for (std::size_t k = 0; k < size; ++k) {
        const std::size_t row = perm[static_cast<std::size_t>(b) * size + k];
        std::memcpy(ro.data() + k * obs_, obs_buf_.data() + row * obs_, sizeof(f32) * static_cast<std::size_t>(obs_));
        std::memcpy(rc.data() + k * crit_, crit_buf_.data() + row * crit_, sizeof(f32) * static_cast<std::size_t>(crit_));
      }
      policy_.normalizer.apply(ro.data(), static_cast<i32>(size), xa.data());
      critic_norm_.apply(rc.data(), static_cast<i32>(size), xc.data());
      policy_.actor.forward(xa.data(), static_cast<i32>(size), mu.data(), &ca);
      critic_.forward(xc.data(), static_cast<i32>(size), value.data(), &cc);
      // Adaptive learning rate from the KL divergence between the old and the new policy.
      if (cfg_.adaptive_lr && cfg_.desired_kl > 0) {
        f64 kl = 0;
        for (std::size_t k = 0; k < size; ++k) {
          const std::size_t row = perm[static_cast<std::size_t>(b) * size + k];
          for (i32 j = 0; j < A; ++j) {
            const f32 s0 = old_std[static_cast<std::size_t>(j)], s1 = policy_.stddev[static_cast<std::size_t>(j)];
            const f32 d = mu_buf_[row * A + j] - mu[k * A + j];
            kl += std::log(s1 / s0) + (s0 * s0 + d * d) / (2 * s1 * s1) - 0.5f;
          }
        }
        kl /= static_cast<f64>(size);
#ifdef KERN_PPO_DEBUG
        std::fprintf(stderr, "    epoch %d mb %d kl %.5f lr %.2e\n", epoch, b, kl, lr_);
#endif
        if (kl > cfg_.desired_kl * 2.0) lr_ = max_of(1e-5f, lr_ / 1.5f);
        else if (kl < cfg_.desired_kl / 2.0 && kl > 0.0) lr_ = min_of(1e-2f, lr_ * 1.5f);
        actor_adam_.lr = critic_adam_.lr = lr_;
      }
      std::fill(dstd.begin(), dstd.end(), 0.0f);
      f64 sloss = 0, vloss = 0;
      const f32 inv = 1.0f / static_cast<f32>(size);
      for (std::size_t k = 0; k < size; ++k) {
        const std::size_t row = perm[static_cast<std::size_t>(b) * size + k];
        f32 logp = 0;
        for (i32 j = 0; j < A; ++j) {
          const f32 s = policy_.stddev[static_cast<std::size_t>(j)], d = act_buf_[row * A + j] - mu[k * A + j];
          logp += -0.5f * d * d / (s * s) - std::log(s) - 0.5f * kLog2Pi;
        }
        const f32 adv = adv_buf_[row];
        const f32 ratio = std::exp(clamp(logp - logp_buf_[row], -20.0f, 20.0f));
        const f32 s1 = -adv * ratio, s2 = -adv * clamp(ratio, 1 - cfg_.clip, 1 + cfg_.clip);
        sloss += max_of(s1, s2);
        const f32 g = s1 >= s2 ? -adv * ratio * inv : 0.0f;  // d(loss)/d(logp)
        for (i32 j = 0; j < A; ++j) {
          const f32 s = policy_.stddev[static_cast<std::size_t>(j)], d = act_buf_[row * A + j] - mu[k * A + j];
          dmu[k * A + j] = g * d / (s * s);
          dstd[static_cast<std::size_t>(j)] += g * (d * d / (s * s * s) - 1 / s);
        }
        // Value loss (clipped around the value at collection).
        const f32 v = value[k], v0 = value_buf_[row], R = return_buf_[row];
        const f32 vc = v0 + clamp(v - v0, -cfg_.clip, cfg_.clip);
        const f32 l1 = (v - R) * (v - R), l2 = (vc - R) * (vc - R);
        if (!cfg_.clipped_value_loss || l1 >= l2) { vloss += l1; dv[k] = cfg_.value_coef * 2 * (v - R) * inv; }
        else { vloss += l2; dv[k] = std::fabs(v - v0) < cfg_.clip ? cfg_.value_coef * 2 * (vc - R) * inv : 0.0f; }
      }
      // Entropy bonus: sum over actions of 0.5 + 0.5 log(2 pi) + log(std), the same for every sample.
      f32 entropy = 0;
      for (i32 j = 0; j < A; ++j) {
        entropy += 0.5f + 0.5f * kLog2Pi + std::log(policy_.stddev[static_cast<std::size_t>(j)]);
        dstd[static_cast<std::size_t>(j)] += -cfg_.entropy_coef / policy_.stddev[static_cast<std::size_t>(j)];
      }
      std::fill(gactor.begin(), gactor.end(), 0.0f);
      std::fill(gcritic.begin(), gcritic.end(), 0.0f);
      policy_.actor.backward(ca, dmu.data(), gactor.data());
      critic_.backward(cc, dv.data(), gcritic.data());
      actor_adam_.step({Adam::Block{policy_.actor.params().data(), gactor.data(), gactor.size()}, Adam::Block{policy_.stddev.data(), dstd.data(), dstd.size()}});
      critic_adam_.step({Adam::Block{critic_.params().data(), gcritic.data(), gcritic.size()}});
      for (f32& s : policy_.stddev) s = max_of(kMinStd, s);
      sloss_sum += sloss / static_cast<f64>(size);
      vloss_sum += vloss / static_cast<f64>(size);
      ent_sum += entropy;
      ++updates;
    }
  }
  if (updates) {
    stats.surrogate_loss = static_cast<f32>(sloss_sum / updates);
    stats.value_loss = static_cast<f32>(vloss_sum / updates);
    stats.entropy = static_cast<f32>(ent_sum / updates);
  }
}

}  // namespace kern::train
