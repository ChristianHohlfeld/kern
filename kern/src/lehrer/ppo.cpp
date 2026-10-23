// PPO as rsl_rl runs it (see ppo.h): the same losses, schedule and bookkeeping, in C++.
#include "kern/lehrer/ppo.h"
#include "kern/core/dmath.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <numeric>

#if !defined(__wasi__)
#include <thread>
#endif

namespace kern::lehrer {
namespace {

constexpr f32 kLog2Pi = 1.83787706641f;
constexpr f32 kMinStd = 1e-3f;
constexpr std::size_t kLearnChunks = 8;

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
  while (static_cast<i32>(env_rng_.size()) < N) {
    Rng r(rng_.state ^ (0xA24BAED4963EE407ull * static_cast<u64>(first_env_ + static_cast<i32>(env_rng_.size()) + 1)));
    r.next();
    env_rng_.push_back(r);
  }
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
  if (parallel) parallel_ = parallel;
  collector_.collect(envs, cfg_.steps_per_env, policy_, local_, parallel);
  return learn({&local_});
}

IterationStats Ppo::learn(const std::vector<const Rollout*>& rollouts) {
  IterationStats stats;
  stats.iteration = ++iteration_;
  std::size_t rows = 0;
  for (const Rollout* r : rollouts) {
    rows += r->reward.size();
    for (std::size_t k = 0; k < r->episode_returns.size(); ++k) {
      stats.mean_return += r->episode_returns[k];
      stats.mean_length += static_cast<f32>(r->episode_lengths[k]);
      ++stats.episodes;
    }
    stats.collect_seconds = std::max(stats.collect_seconds, r->seconds);
  }
  if (!rows) return stats;
  if (stats.episodes) { stats.mean_return /= static_cast<f32>(stats.episodes); stats.mean_length /= static_cast<f32>(stats.episodes); }
  total_steps_ += static_cast<i64>(rows);
  stats.steps = total_steps_;
  const auto t1 = std::chrono::steady_clock::now();
  // Gather every row; evaluate the old policy and the values under today's normalisers.
  obs_buf_.resize(rows * static_cast<std::size_t>(obs_));
  crit_buf_.resize(rows * static_cast<std::size_t>(crit_));
  act_buf_.resize(rows * static_cast<std::size_t>(act_));
  mu_buf_.resize(rows * static_cast<std::size_t>(act_));
  logp_buf_.resize(rows);
  value_buf_.resize(rows);
  return_buf_.resize(rows);
  adv_buf_.resize(rows);
  // One canonical order, whatever the split into rollouts: step-major over all environments in the
  // rollouts' order (the order of their global indices). The learner's result is then the same for
  // any number of workers (docs/lehrer.md, determinism).
  const i32 T = rollouts.front()->steps;
  i32 N = 0;
  for (const Rollout* r : rollouts) {
    if (r->steps != T) return stats;
    N += r->envs;
  }
  reward_.resize(rows);
  live_.resize(rows);
  timeout_.resize(rows);
  last_crit_.resize(static_cast<std::size_t>(N) * static_cast<std::size_t>(crit_));
  {
    std::size_t dst = 0;
    for (i32 t = 0; t < T; ++t)
      for (const Rollout* r : rollouts)
        for (i32 i = 0; i < r->envs; ++i, ++dst) {
          const std::size_t src = static_cast<std::size_t>(t) * static_cast<std::size_t>(r->envs) + static_cast<std::size_t>(i);
          std::memcpy(obs_buf_.data() + dst * obs_, r->observation.data() + src * obs_, sizeof(f32) * static_cast<std::size_t>(obs_));
          std::memcpy(crit_buf_.data() + dst * crit_, r->critic_obs.data() + src * crit_, sizeof(f32) * static_cast<std::size_t>(crit_));
          std::memcpy(act_buf_.data() + dst * act_, r->action.data() + src * act_, sizeof(f32) * static_cast<std::size_t>(act_));
          reward_[dst] = r->reward[src];
          timeout_[dst] = r->timeout[src];
          live_[dst] = r->terminal[src] || r->timeout[src] ? 0 : 1;
        }
    std::size_t e = 0;
    for (const Rollout* r : rollouts)
      for (i32 i = 0; i < r->envs; ++i, ++e)
        std::memcpy(last_crit_.data() + e * crit_, r->last_critic.data() + static_cast<std::size_t>(i) * crit_, sizeof(f32) * static_cast<std::size_t>(crit_));
  }
  {
    f64 sum = 0;
    for (f32 v : reward_) sum += v;
    stats.mean_reward = static_cast<f32>(sum / static_cast<f64>(rows));
  }
  // The normalisers learn from what was observed (rsl_rl updates them step by step while collecting;
  // collectors elsewhere keep the policy fixed for a rollout, so the learner does it here, at once).
  if (!cfg_.freeze_actor_normalizer) policy_.normalizer.update(obs_buf_.data(), static_cast<i32>(rows));
  critic_norm_.update(crit_buf_.data(), static_cast<i32>(rows));
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
          logp += -0.5f * d * d / (sd * sd) - kern::dm::log(sd) - 0.5f * kLog2Pi;
        }
        logp_buf_[s + static_cast<std::size_t>(k)] = logp;
      }
    }
  }
  // GAE (lambda) per environment; a time-out is bootstrapped with the value where it stood (rsl_rl).
  {
    std::vector<f32> last(static_cast<std::size_t>(N)), xc(last_crit_.size());
    critic_norm_.apply(last_crit_.data(), N, xc.data());
    critic_.forward(xc.data(), N, last.data());
    for (i32 i = 0; i < N; ++i) {
      f32 adv = 0;
      for (i32 t = T - 1; t >= 0; --t) {
        const std::size_t row = static_cast<std::size_t>(t) * static_cast<std::size_t>(N) + static_cast<std::size_t>(i);
        const f32 v = value_buf_[row];
        const f32 next = t == T - 1 ? last[static_cast<std::size_t>(i)] : value_buf_[row + static_cast<std::size_t>(N)];
        const f32 reward = reward_[row] + (timeout_[row] ? cfg_.gamma * v : 0.0f);
        const f32 live = static_cast<f32>(live_[row]);
        const f32 delta = reward + live * cfg_.gamma * next - v;
        adv = delta + live * cfg_.gamma * cfg_.lam * adv;
        return_buf_[row] = adv + v;
        adv_buf_[row] = adv;
      }
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
  // A minibatch is cut into chunks that run on the learner's threads; their gradients add up.
  // A fixed number of chunks whatever the thread count: the gradient's sum is the same on 1 thread,
  // 16 threads or in the browser.
  const i32 chunks = static_cast<i32>(std::min<std::size_t>(kLearnChunks, size));
  struct Chunk {
    std::size_t lo = 0, n = 0;
    std::vector<f32> ro, rc, xa, xc, mu, value, dmu, dv, dstd, gactor, gcritic;
    Mlp::Cache ca, cc;
    f64 sloss = 0, vloss = 0;
  };
  std::vector<Chunk> part(static_cast<std::size_t>(chunks));
  for (i32 c = 0; c < chunks; ++c) {
    Chunk& k = part[static_cast<std::size_t>(c)];
    k.lo = size * static_cast<std::size_t>(c) / static_cast<std::size_t>(chunks);
    k.n = size * static_cast<std::size_t>(c + 1) / static_cast<std::size_t>(chunks) - k.lo;
    k.ro.resize(k.n * obs_); k.xa.resize(k.ro.size());
    k.rc.resize(k.n * crit_); k.xc.resize(k.rc.size());
    k.mu.resize(k.n * A); k.dmu.resize(k.mu.size());
    k.value.resize(k.n); k.dv.resize(k.n);
    k.dstd.resize(static_cast<std::size_t>(A));
    k.gactor.resize(policy_.actor.params().size());
    k.gcritic.resize(critic_.params().size());
  }
  const ParallelFor run = parallel_ ? parallel_ : thread_pool(1);
  const f32 inv = 1.0f / static_cast<f32>(size);
  f64 vloss_sum = 0, sloss_sum = 0, ent_sum = 0;
  i32 updates = 0;
  for (i32 epoch = 0; epoch < cfg_.epochs; ++epoch) {
    for (i32 b = 0; b < mb; ++b) {
      const u32* rows_of = perm.data() + static_cast<std::size_t>(b) * size;
      run(chunks, [&](i32 c) {
        Chunk& k = part[static_cast<std::size_t>(c)];
        for (std::size_t r = 0; r < k.n; ++r) {
          const std::size_t row = rows_of[k.lo + r];
          std::memcpy(k.ro.data() + r * obs_, obs_buf_.data() + row * obs_, sizeof(f32) * static_cast<std::size_t>(obs_));
          std::memcpy(k.rc.data() + r * crit_, crit_buf_.data() + row * crit_, sizeof(f32) * static_cast<std::size_t>(crit_));
        }
        policy_.normalizer.apply(k.ro.data(), static_cast<i32>(k.n), k.xa.data());
        critic_norm_.apply(k.rc.data(), static_cast<i32>(k.n), k.xc.data());
        policy_.actor.forward(k.xa.data(), static_cast<i32>(k.n), k.mu.data(), &k.ca);
        critic_.forward(k.xc.data(), static_cast<i32>(k.n), k.value.data(), &k.cc);
      });
      // Adaptive learning rate from the KL divergence between the old and the new policy.
      if (cfg_.adaptive_lr && cfg_.desired_kl > 0) {
        f64 kl = 0;
        for (const Chunk& k : part)
          for (std::size_t r = 0; r < k.n; ++r) {
            const std::size_t row = rows_of[k.lo + r];
            for (i32 j = 0; j < A; ++j) {
              const f32 s0 = old_std[static_cast<std::size_t>(j)], s1 = policy_.stddev[static_cast<std::size_t>(j)];
              const f32 d = mu_buf_[row * A + j] - k.mu[r * A + j];
              kl += kern::dm::log(s1 / s0) + (s0 * s0 + d * d) / (2 * s1 * s1) - 0.5f;
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
      run(chunks, [&](i32 c) {
        Chunk& k = part[static_cast<std::size_t>(c)];
        std::fill(k.dstd.begin(), k.dstd.end(), 0.0f);
        k.sloss = k.vloss = 0;
        for (std::size_t r = 0; r < k.n; ++r) {
          const std::size_t row = rows_of[k.lo + r];
          f32 logp = 0;
          for (i32 j = 0; j < A; ++j) {
            const f32 sd = policy_.stddev[static_cast<std::size_t>(j)], d = act_buf_[row * A + j] - k.mu[r * A + j];
            logp += -0.5f * d * d / (sd * sd) - kern::dm::log(sd) - 0.5f * kLog2Pi;
          }
          const f32 adv = adv_buf_[row];
          const f32 ratio = kern::dm::exp(clamp(logp - logp_buf_[row], -20.0f, 20.0f));
          const f32 s1 = -adv * ratio, s2 = -adv * clamp(ratio, 1 - cfg_.clip, 1 + cfg_.clip);
          k.sloss += max_of(s1, s2);
          const f32 g = s1 >= s2 ? -adv * ratio * inv : 0.0f;  // d(loss)/d(logp)
          for (i32 j = 0; j < A; ++j) {
            const f32 sd = policy_.stddev[static_cast<std::size_t>(j)], d = act_buf_[row * A + j] - k.mu[r * A + j];
            k.dmu[r * A + j] = g * d / (sd * sd);
            k.dstd[static_cast<std::size_t>(j)] += g * (d * d / (sd * sd * sd) - 1 / sd);
          }
          // Value loss (clipped around the value at collection).
          const f32 v = k.value[r], v0 = value_buf_[row], R = return_buf_[row];
          const f32 vc = v0 + clamp(v - v0, -cfg_.clip, cfg_.clip);
          const f32 l1 = (v - R) * (v - R), l2 = (vc - R) * (vc - R);
          if (!cfg_.clipped_value_loss || l1 >= l2) { k.vloss += l1; k.dv[r] = cfg_.value_coef * 2 * (v - R) * inv; }
          else { k.vloss += l2; k.dv[r] = std::fabs(v - v0) < cfg_.clip ? cfg_.value_coef * 2 * (vc - R) * inv : 0.0f; }
        }
        std::fill(k.gactor.begin(), k.gactor.end(), 0.0f);
        std::fill(k.gcritic.begin(), k.gcritic.end(), 0.0f);
        policy_.actor.backward(k.ca, k.dmu.data(), k.gactor.data());
        critic_.backward(k.cc, k.dv.data(), k.gcritic.data());
      });
      Chunk& first = part[0];
      f64 sloss = first.sloss, vloss = first.vloss;
      for (std::size_t c = 1; c < part.size(); ++c) {
        const Chunk& k = part[c];
        for (std::size_t i = 0; i < first.gactor.size(); ++i) first.gactor[i] += k.gactor[i];
        for (std::size_t i = 0; i < first.gcritic.size(); ++i) first.gcritic[i] += k.gcritic[i];
        for (i32 j = 0; j < A; ++j) first.dstd[static_cast<std::size_t>(j)] += k.dstd[static_cast<std::size_t>(j)];
        sloss += k.sloss;
        vloss += k.vloss;
      }
      // Entropy bonus: sum over actions of 0.5 + 0.5 log(2 pi) + log(std), the same for every sample.
      f32 entropy = 0;
      for (i32 j = 0; j < A; ++j) {
        entropy += 0.5f + 0.5f * kLog2Pi + kern::dm::log(policy_.stddev[static_cast<std::size_t>(j)]);
        first.dstd[static_cast<std::size_t>(j)] += -cfg_.entropy_coef / policy_.stddev[static_cast<std::size_t>(j)];
      }
      actor_adam_.step({Adam::Block{policy_.actor.params().data(), first.gactor.data(), first.gactor.size()}, Adam::Block{policy_.stddev.data(), first.dstd.data(), first.dstd.size()}});
      critic_adam_.step({Adam::Block{critic_.params().data(), first.gcritic.data(), first.gcritic.size()}});
      for (f32& sd : policy_.stddev) sd = max_of(kMinStd, sd);
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

bool Ppo::warm_start(const std::vector<i32>& dims, const std::vector<std::vector<f32>>& weights, const std::vector<std::vector<f32>>& biases,
                     const std::vector<f32>& mean, const std::vector<f32>& div, const std::vector<i32>& new_inputs) {
  Mlp& a = policy_.actor;
  if (dims != a.dims() || weights.size() != static_cast<std::size_t>(a.layers()) || mean.size() != div.size() ||
      mean.size() != static_cast<std::size_t>(a.inputs()))
    return false;
  for (i32 l = 0; l < a.layers(); ++l) {
    const i32 in = dims[static_cast<std::size_t>(l)], out = dims[static_cast<std::size_t>(l) + 1];
    const std::vector<f32>& w = weights[static_cast<std::size_t>(l)];  // [out][in] (ONNX Gemm transB)
    if (w.size() != static_cast<std::size_t>(in) * static_cast<std::size_t>(out) || biases[static_cast<std::size_t>(l)].size() != static_cast<std::size_t>(out)) return false;
    f32* dst = a.weight(l);
    for (i32 o = 0; o < out; ++o)
      for (i32 i = 0; i < in; ++i) dst[static_cast<std::size_t>(i) * out + o] = w[static_cast<std::size_t>(o) * in + i];
    std::copy(biases[static_cast<std::size_t>(l)].begin(), biases[static_cast<std::size_t>(l)].end(), a.bias(l));
  }
  // The normaliser it was trained with: y = (x - mean) / div, div = std + 0.01.
  std::vector<f32> m = mean, var(div.size());
  for (std::size_t i = 0; i < div.size(); ++i) var[i] = (div[i] - 0.01f) * (div[i] - 0.01f);
  for (i32 i : new_inputs) {
    if (i < 0 || i >= a.inputs()) return false;
    m[static_cast<std::size_t>(i)] = 0;
    var[static_cast<std::size_t>(i)] = 0.99f * 0.99f;  // div 1
    f32* row = a.weight(0) + static_cast<std::size_t>(i) * static_cast<std::size_t>(dims[1]);
    std::fill(row, row + dims[1], 0.0f);
  }
  policy_.normalizer.set(std::move(m), std::move(var), 1e9);
  cfg_.freeze_actor_normalizer = true;
  return true;
}

namespace {
template <typename T>
void put_all(std::vector<u8>& out, const std::vector<T>& v) {
  const u64 n = v.size();
  out.insert(out.end(), reinterpret_cast<const u8*>(&n), reinterpret_cast<const u8*>(&n) + 8);
  out.insert(out.end(), reinterpret_cast<const u8*>(v.data()), reinterpret_cast<const u8*>(v.data()) + n * sizeof(T));
}
template <typename T>
bool get_all(const u8* d, std::size_t size, std::size_t& at, std::vector<T>& v, std::size_t expect = ~std::size_t{0}) {
  if (at + 8 > size) return false;
  u64 n;
  std::memcpy(&n, d + at, 8);
  at += 8;
  if (n > (size - at) / sizeof(T) || (expect != ~std::size_t{0} && n != expect)) return false;
  v.resize(static_cast<std::size_t>(n));
  if (n) std::memcpy(v.data(), d + at, static_cast<std::size_t>(n) * sizeof(T));
  at += static_cast<std::size_t>(n) * sizeof(T);
  return true;
}
}  // namespace

std::vector<u8> Ppo::save() const {
  std::vector<u8> out;
  const char magic[8] = {'K', 'E', 'R', 'N', 'P', 'P', 'O', '1'};
  out.insert(out.end(), magic, magic + 8);
  put_all(out, std::vector<f64>{static_cast<f64>(iteration_), static_cast<f64>(total_steps_), lr_, cfg_.freeze_actor_normalizer ? 1.0 : 0.0});
  put_all(out, policy_.serialize());
  put_all(out, critic_.params());
  put_all(out, critic_norm_.mean());
  put_all(out, critic_norm_.variance());
  put_all(out, std::vector<f64>{critic_norm_.count()});
  Ppo& self = const_cast<Ppo&>(*this);
  for (Adam* a : {&self.actor_adam_, &self.critic_adam_}) {
    put_all(out, a->m());
    put_all(out, a->v());
    put_all(out, std::vector<f64>{static_cast<f64>(a->t())});
  }
  return out;
}

bool Ppo::load(const u8* d, std::size_t size) {
  if (size < 8 || std::memcmp(d, "KERNPPO1", 8) != 0) return false;
  std::size_t at = 8;
  std::vector<f64> head, count;
  std::vector<f32> pol, cp, cm, cv;
  if (!get_all(d, size, at, head) || head.size() != 4 || !get_all(d, size, at, pol) || !get_all(d, size, at, cp, critic_.params().size()) ||
      !get_all(d, size, at, cm) || !get_all(d, size, at, cv) || !get_all(d, size, at, count) || count.size() != 1)
    return false;
  Policy p = policy_;
  if (!p.deserialize(pol.data(), pol.size()) || p.actor.dims() != policy_.actor.dims()) return false;
  for (Adam* a : {&actor_adam_, &critic_adam_}) {
    std::vector<f64> t;
    if (!get_all(d, size, at, a->m()) || !get_all(d, size, at, a->v()) || !get_all(d, size, at, t) || t.size() != 1) return false;
    a->t() = static_cast<i64>(t[0]);
  }
  policy_ = std::move(p);
  critic_.params() = std::move(cp);
  critic_norm_.set(std::move(cm), std::move(cv), count[0]);
  iteration_ = static_cast<i32>(head[0]);
  total_steps_ = static_cast<i64>(head[1]);
  lr_ = static_cast<f32>(head[2]);
  cfg_.freeze_actor_normalizer = head[3] != 0;
  actor_adam_.lr = critic_adam_.lr = lr_;
  return true;
}

}  // namespace kern::lehrer
