// PPO as rsl_rl runs it (see ppo.h): the same losses, schedule and bookkeeping, in C++.
#include "kern/train/ppo.h"

#include <chrono>
#include <cmath>
#include <cstring>
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

Ppo::Ppo(const PpoConfig& config, i32 obs_size, i32 critic_size, i32 action_size)
    : cfg_(config), obs_(obs_size), crit_(critic_size), act_(action_size), lr_(config.learning_rate), rng_(config.seed) {
  std::vector<i32> a{obs_size}, c{critic_size};
  for (i32 h : cfg_.actor_hidden) a.push_back(h);
  for (i32 h : cfg_.critic_hidden) c.push_back(h);
  a.push_back(action_size);
  c.push_back(1);
  actor_.init(a, rng_);
  critic_.init(c, rng_);
  actor_norm_.init(obs_size);
  critic_norm_.init(critic_size);
  std_.assign(static_cast<std::size_t>(action_size), cfg_.init_std);
  actor_adam_.max_norm = critic_adam_.max_norm = cfg_.max_grad_norm;
  actor_adam_.lr = critic_adam_.lr = lr_;
}

void Ppo::act(const f32* observation, f32* action) const {
  std::vector<f32> x(static_cast<std::size_t>(obs_));
  actor_norm_.apply(observation, 1, x.data());
  actor_.forward(x.data(), 1, action);
}

IterationStats Ppo::iterate(std::vector<Env*>& envs, const ParallelFor& parallel_in) {
  const ParallelFor parallel = parallel_in ? parallel_in : thread_pool(1);
  const i32 N = static_cast<i32>(envs.size()), T = cfg_.steps_per_env;
  const std::size_t rows = static_cast<std::size_t>(N) * static_cast<std::size_t>(T);
  IterationStats stats;
  stats.iteration = ++iteration_;
  const auto t0 = std::chrono::steady_clock::now();
  obs_buf_.resize(rows * static_cast<std::size_t>(obs_));
  crit_buf_.resize(rows * static_cast<std::size_t>(crit_));
  act_buf_.resize(rows * static_cast<std::size_t>(act_));
  mu_buf_.resize(rows * static_cast<std::size_t>(act_));
  logp_buf_.resize(rows);
  value_buf_.resize(rows);
  reward_buf_.resize(rows);
  done_buf_.resize(rows);
  return_buf_.resize(rows);
  adv_buf_.resize(rows);
  while (static_cast<i32>(env_rng_.size()) < N) env_rng_.emplace_back(rng_.next());
  episode_return_.resize(static_cast<std::size_t>(N), 0.0f);
  episode_length_.resize(static_cast<std::size_t>(N), 0);
  std::vector<std::vector<std::pair<f32, i32>>> finished(static_cast<std::size_t>(N));
  const std::vector<f32> stddev = std_;
  f64 reward_sum = 0;

  for (i32 t = 0; t < T; ++t) {
    parallel(N, [&](i32 i) {
      Env& env = *envs[static_cast<std::size_t>(i)];
      const std::size_t row = static_cast<std::size_t>(t) * static_cast<std::size_t>(N) + static_cast<std::size_t>(i);
      f32* o = obs_buf_.data() + row * static_cast<std::size_t>(obs_);
      f32* c = crit_buf_.data() + row * static_cast<std::size_t>(crit_);
      std::memcpy(o, env.observation(), sizeof(f32) * static_cast<std::size_t>(obs_));
      std::memcpy(c, env.critic(), sizeof(f32) * static_cast<std::size_t>(crit_));
      std::vector<f32> xa(static_cast<std::size_t>(obs_)), xc(static_cast<std::size_t>(crit_));
      actor_norm_.apply(o, 1, xa.data());
      critic_norm_.apply(c, 1, xc.data());
      f32* mu = mu_buf_.data() + row * static_cast<std::size_t>(act_);
      f32* a = act_buf_.data() + row * static_cast<std::size_t>(act_);
      actor_.forward(xa.data(), 1, mu);
      f32 value = 0;
      critic_.forward(xc.data(), 1, &value);
      Rng& r = env_rng_[static_cast<std::size_t>(i)];
      f32 logp = 0;
      for (i32 j = 0; j < act_; ++j) {
        const f32 s = stddev[static_cast<std::size_t>(j)], e = r.normal();
        a[j] = mu[j] + s * e;
        logp += -0.5f * e * e - std::log(s) - 0.5f * kLog2Pi;
      }
      const Env::Step st = env.step(a);
      f32 reward = st.reward;
      episode_return_[static_cast<std::size_t>(i)] += st.reward;
      ++episode_length_[static_cast<std::size_t>(i)];
      if (st.timeout) reward += cfg_.gamma * value;  // rsl_rl: bootstrap a time-out with the value
      const bool done = st.terminal || st.timeout;
      logp_buf_[row] = logp;
      value_buf_[row] = value;
      reward_buf_[row] = reward;
      done_buf_[row] = done ? 1.0f : 0.0f;
      if (done) {
        finished[static_cast<std::size_t>(i)].push_back({episode_return_[static_cast<std::size_t>(i)], episode_length_[static_cast<std::size_t>(i)]});
        episode_return_[static_cast<std::size_t>(i)] = 0;
        episode_length_[static_cast<std::size_t>(i)] = 0;
        env.reset();
      }
    });
    // The normalisers learn from the observations the step produced (rsl_rl process_env_step).
    std::vector<f32> next_o(static_cast<std::size_t>(N) * static_cast<std::size_t>(obs_)), next_c(static_cast<std::size_t>(N) * static_cast<std::size_t>(crit_));
    for (i32 i = 0; i < N; ++i) {
      std::memcpy(next_o.data() + static_cast<std::size_t>(i) * obs_, envs[static_cast<std::size_t>(i)]->observation(), sizeof(f32) * static_cast<std::size_t>(obs_));
      std::memcpy(next_c.data() + static_cast<std::size_t>(i) * crit_, envs[static_cast<std::size_t>(i)]->critic(), sizeof(f32) * static_cast<std::size_t>(crit_));
      reward_sum += reward_buf_[static_cast<std::size_t>(t) * N + i];
    }
    actor_norm_.update(next_o.data(), N);
    critic_norm_.update(next_c.data(), N);
  }
  total_steps_ += static_cast<i64>(rows);
  stats.steps = total_steps_;
  stats.mean_reward = static_cast<f32>(reward_sum / static_cast<f64>(rows));
  for (const auto& f : finished)
    for (const auto& [ret, len] : f) {
      stats.mean_return += ret;
      stats.mean_length += static_cast<f32>(len);
      ++stats.episodes;
    }
  if (stats.episodes) { stats.mean_return /= static_cast<f32>(stats.episodes); stats.mean_length /= static_cast<f32>(stats.episodes); }

  // GAE (lambda) with the value of where each environment stands now.
  std::vector<f32> last(static_cast<std::size_t>(N));
  {
    std::vector<f32> xc(static_cast<std::size_t>(N) * static_cast<std::size_t>(crit_)), raw(xc.size());
    for (i32 i = 0; i < N; ++i) std::memcpy(raw.data() + static_cast<std::size_t>(i) * crit_, envs[static_cast<std::size_t>(i)]->critic(), sizeof(f32) * static_cast<std::size_t>(crit_));
    critic_norm_.apply(raw.data(), N, xc.data());
    critic_.forward(xc.data(), N, last.data());
  }
  for (i32 i = 0; i < N; ++i) {
    f32 adv = 0;
    for (i32 t = T - 1; t >= 0; --t) {
      const std::size_t row = static_cast<std::size_t>(t) * N + static_cast<std::size_t>(i);
      const f32 next = t == T - 1 ? last[static_cast<std::size_t>(i)] : value_buf_[row + static_cast<std::size_t>(N)];
      const f32 live = 1.0f - done_buf_[row];
      const f32 delta = reward_buf_[row] + live * cfg_.gamma * next - value_buf_[row];
      adv = delta + live * cfg_.gamma * cfg_.lam * adv;
      return_buf_[row] = adv + value_buf_[row];
      adv_buf_[row] = adv;
    }
  }
  {
    f64 m = 0, v = 0;
    for (f32 a : adv_buf_) m += a;
    m /= static_cast<f64>(rows);
    for (f32 a : adv_buf_) v += (a - m) * (a - m);
    const f64 sd = std::sqrt(v / static_cast<f64>(rows > 1 ? rows - 1 : 1)) + 1e-8;
    for (f32& a : adv_buf_) a = static_cast<f32>((a - m) / sd);
  }
  stats.collect_seconds = seconds_since(t0);
  const auto t1 = std::chrono::steady_clock::now();
  update(stats);
  stats.learn_seconds = seconds_since(t1);
  stats.learning_rate = lr_;
  f32 s = 0;
  for (f32 v : std_) s += v;
  stats.mean_std = s / static_cast<f32>(std_.size());
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
  const std::vector<f32> old_std = std_;
  const i32 A = act_;
  std::vector<f32> xa(size * static_cast<std::size_t>(obs_)), xc(size * static_cast<std::size_t>(crit_)), ro(xa.size()), rc(xc.size());
  std::vector<f32> mu(size * static_cast<std::size_t>(A)), value(size), dmu(mu.size()), dv(size), dstd(static_cast<std::size_t>(A));
  std::vector<f32> gactor(actor_.params().size()), gcritic(critic_.params().size());
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
      actor_norm_.apply(ro.data(), static_cast<i32>(size), xa.data());
      critic_norm_.apply(rc.data(), static_cast<i32>(size), xc.data());
      actor_.forward(xa.data(), static_cast<i32>(size), mu.data(), &ca);
      critic_.forward(xc.data(), static_cast<i32>(size), value.data(), &cc);
      // Adaptive learning rate from the KL divergence between the old and the new policy.
      if (cfg_.adaptive_lr && cfg_.desired_kl > 0) {
        f64 kl = 0;
        for (std::size_t k = 0; k < size; ++k) {
          const std::size_t row = perm[static_cast<std::size_t>(b) * size + k];
          for (i32 j = 0; j < A; ++j) {
            const f32 s0 = old_std[static_cast<std::size_t>(j)], s1 = std_[static_cast<std::size_t>(j)];
            const f32 d = mu_buf_[row * A + j] - mu[k * A + j];
            kl += std::log(s1 / s0) + (s0 * s0 + d * d) / (2 * s1 * s1) - 0.5f;
          }
        }
        kl /= static_cast<f64>(size);
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
          const f32 s = std_[static_cast<std::size_t>(j)], d = act_buf_[row * A + j] - mu[k * A + j];
          logp += -0.5f * d * d / (s * s) - std::log(s) - 0.5f * kLog2Pi;
        }
        const f32 adv = adv_buf_[row];
        const f32 ratio = std::exp(clamp(logp - logp_buf_[row], -20.0f, 20.0f));
        const f32 s1 = -adv * ratio, s2 = -adv * clamp(ratio, 1 - cfg_.clip, 1 + cfg_.clip);
        sloss += max_of(s1, s2);
        const f32 g = s1 >= s2 ? -adv * ratio * inv : 0.0f;  // d(loss)/d(logp)
        for (i32 j = 0; j < A; ++j) {
          const f32 s = std_[static_cast<std::size_t>(j)], d = act_buf_[row * A + j] - mu[k * A + j];
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
        entropy += 0.5f + 0.5f * kLog2Pi + std::log(std_[static_cast<std::size_t>(j)]);
        dstd[static_cast<std::size_t>(j)] += -cfg_.entropy_coef / std_[static_cast<std::size_t>(j)];
      }
      std::fill(gactor.begin(), gactor.end(), 0.0f);
      std::fill(gcritic.begin(), gcritic.end(), 0.0f);
      actor_.backward(ca, dmu.data(), gactor.data());
      critic_.backward(cc, dv.data(), gcritic.data());
      actor_adam_.step({Adam::Block{actor_.params().data(), gactor.data(), gactor.size()}, Adam::Block{std_.data(), dstd.data(), dstd.size()}});
      critic_adam_.step({Adam::Block{critic_.params().data(), gcritic.data(), gcritic.size()}});
      for (f32& s : std_) s = max_of(kMinStd, s);
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
