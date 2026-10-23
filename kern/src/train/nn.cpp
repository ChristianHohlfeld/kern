// The trainer's networks (see nn.h). Weights are stored input-major (w[i * out + o]) so that both
// passes run as contiguous axpy loops the compiler vectorises without reassociating sums.
#include "kern/train/nn.h"

#include <cmath>
#include <cstring>

namespace kern::train {

void Mlp::init(const std::vector<i32>& dims, Rng& rng, f32 output_gain) {
  dims_ = dims;
  w_.clear();
  b_.clear();
  std::size_t n = 0;
  for (std::size_t l = 0; l + 1 < dims.size(); ++l) {
    w_.push_back(n);
    n += static_cast<std::size_t>(dims[l]) * static_cast<std::size_t>(dims[l + 1]);
    b_.push_back(n);
    n += static_cast<std::size_t>(dims[l + 1]);
  }
  params_.assign(n, 0.0f);
  for (i32 l = 0; l < layers(); ++l) {
    const i32 in = dims_[static_cast<std::size_t>(l)], out = dims_[static_cast<std::size_t>(l) + 1];
    const f32 bound = 1.0f / std::sqrt(static_cast<f32>(in)) * (l == layers() - 1 ? output_gain : 1.0f);
    f32* w = weight(l);
    for (i32 k = 0; k < in * out; ++k) w[k] = rng.uniform(-bound, bound);
    f32* b = bias(l);
    for (i32 o = 0; o < out; ++o) b[o] = rng.uniform(-bound, bound);
  }
}

void Mlp::forward(const f32* x, i32 batch, f32* out, Cache* cache) const {
  std::vector<f32> local_in, local_out;
  const f32* in_ptr = x;
  if (cache) {
    cache->batch = batch;
    cache->act.resize(dims_.size());
    cache->act[0].assign(x, x + static_cast<std::size_t>(batch) * static_cast<std::size_t>(inputs()));
    in_ptr = cache->act[0].data();
  }
  for (i32 l = 0; l < layers(); ++l) {
    const i32 in = dims_[static_cast<std::size_t>(l)], o_n = dims_[static_cast<std::size_t>(l) + 1];
    const bool last = l == layers() - 1;
    f32* y;
    if (last && !cache) y = out;
    else if (cache) {
      cache->act[static_cast<std::size_t>(l) + 1].resize(static_cast<std::size_t>(batch) * static_cast<std::size_t>(o_n));
      y = cache->act[static_cast<std::size_t>(l) + 1].data();
    } else {
      local_out.resize(static_cast<std::size_t>(batch) * static_cast<std::size_t>(o_n));
      y = local_out.data();
    }
    const f32* w = weight(l);
    const f32* b = bias(l);
    for (i32 r = 0; r < batch; ++r) {
      f32* yr = y + static_cast<std::size_t>(r) * o_n;
      std::memcpy(yr, b, sizeof(f32) * static_cast<std::size_t>(o_n));
      const f32* xr = in_ptr + static_cast<std::size_t>(r) * in;
      for (i32 i = 0; i < in; ++i) {
        const f32 xi = xr[i];
        if (xi == 0) continue;
        const f32* wi = w + static_cast<std::size_t>(i) * o_n;
        for (i32 o = 0; o < o_n; ++o) yr[o] += wi[o] * xi;
      }
      if (!last)
        for (i32 o = 0; o < o_n; ++o) yr[o] = yr[o] > 0 ? yr[o] : std::expm1(yr[o]);  // ELU
    }
    if (last && cache) std::memcpy(out, y, sizeof(f32) * static_cast<std::size_t>(batch) * static_cast<std::size_t>(o_n));
    if (!cache && !last) {
      local_in.swap(local_out);
      in_ptr = local_in.data();
    } else if (cache) {
      in_ptr = y;
    }
  }
}

void Mlp::backward(const Cache& cache, const f32* dout, f32* grad, f32* dx) const {
  const i32 batch = cache.batch;
  std::vector<f32> delta(dout, dout + static_cast<std::size_t>(batch) * static_cast<std::size_t>(outputs())), prev;
  for (i32 l = layers() - 1; l >= 0; --l) {
    const i32 in = dims_[static_cast<std::size_t>(l)], o_n = dims_[static_cast<std::size_t>(l) + 1];
    const std::vector<f32>& x = cache.act[static_cast<std::size_t>(l)];
    const f32* w = weight(l);
    f32* gw = grad + w_[static_cast<std::size_t>(l)];
    f32* gb = grad + b_[static_cast<std::size_t>(l)];
    const bool need_dx = l > 0 || dx;
    if (need_dx) prev.assign(static_cast<std::size_t>(batch) * static_cast<std::size_t>(in), 0.0f);
    for (i32 r = 0; r < batch; ++r) {
      const f32* d = delta.data() + static_cast<std::size_t>(r) * o_n;
      const f32* xr = x.data() + static_cast<std::size_t>(r) * in;
      for (i32 o = 0; o < o_n; ++o) gb[o] += d[o];
      f32* pr = need_dx ? prev.data() + static_cast<std::size_t>(r) * in : nullptr;
      for (i32 i = 0; i < in; ++i) {
        f32* gwi = gw + static_cast<std::size_t>(i) * o_n;
        const f32 xi = xr[i];
        if (xi != 0)
          for (i32 o = 0; o < o_n; ++o) gwi[o] += xi * d[o];
        if (pr) {
          const f32* wi = w + static_cast<std::size_t>(i) * o_n;
          f32 s0 = 0, s1 = 0, s2 = 0, s3 = 0;
          i32 o = 0;
          for (; o + 4 <= o_n; o += 4) { s0 += wi[o] * d[o]; s1 += wi[o + 1] * d[o + 1]; s2 += wi[o + 2] * d[o + 2]; s3 += wi[o + 3] * d[o + 3]; }
          for (; o < o_n; ++o) s0 += wi[o] * d[o];
          pr[i] = (s0 + s1) + (s2 + s3);
        }
      }
    }
    if (l > 0) {
      // Through the ELU of the layer below: y > 0 passes, else dy/dx = y + 1.
      for (std::size_t k = 0; k < prev.size(); ++k) {
        const f32 y = x[k];
        if (y <= 0) prev[k] *= y + 1;
      }
      delta.swap(prev);
    } else if (dx) {
      std::memcpy(dx, prev.data(), sizeof(f32) * prev.size());
    }
  }
}

void Adam::step(std::vector<f32>& params, std::vector<f32>& grad) {
  if (m_.size() != params.size()) { m_.assign(params.size(), 0.0f); v_.assign(params.size(), 0.0f); t_ = 0; }
  if (max_norm > 0) {
    f64 sq = 0;
    for (f32 g : grad) sq += static_cast<f64>(g) * g;
    const f32 norm = static_cast<f32>(std::sqrt(sq));
    if (norm > max_norm) {
      const f32 s = max_norm / (norm + 1e-6f);
      for (f32& g : grad) g *= s;
    }
  }
  ++t_;
  const f32 c1 = 1 - std::pow(beta1, static_cast<f32>(t_)), c2 = 1 - std::pow(beta2, static_cast<f32>(t_));
  const f32 step = lr / c1, root_c2 = std::sqrt(c2);
  for (std::size_t k = 0; k < params.size(); ++k) {
    const f32 g = grad[k];
    m_[k] = beta1 * m_[k] + (1 - beta1) * g;
    v_[k] = beta2 * v_[k] + (1 - beta2) * g * g;
    params[k] -= step * m_[k] / (std::sqrt(v_[k]) / root_c2 + eps);
  }
}

void Normalizer::init(i32 size) {
  mean_.assign(static_cast<std::size_t>(size), 0.0f);
  var_.assign(static_cast<std::size_t>(size), 1.0f);
  count_ = 0;
}

void Normalizer::update(const f32* x, i32 batch) {
  if (batch <= 0) return;
  // Chan's parallel update of mean and variance (as rsl_rl's EmpiricalNormalization).
  const i32 n = size();
  const f64 total = count_ + batch;
  for (i32 i = 0; i < n; ++i) {
    f64 m = 0;
    for (i32 r = 0; r < batch; ++r) m += x[static_cast<std::size_t>(r) * n + i];
    m /= batch;
    f64 v = 0;
    for (i32 r = 0; r < batch; ++r) {
      const f64 d = x[static_cast<std::size_t>(r) * n + i] - m;
      v += d * d;
    }
    v /= batch;
    const f64 delta = m - mean_[static_cast<std::size_t>(i)];
    const f64 new_mean = mean_[static_cast<std::size_t>(i)] + delta * batch / total;
    const f64 m2 = var_[static_cast<std::size_t>(i)] * count_ + v * batch + delta * delta * count_ * batch / total;
    mean_[static_cast<std::size_t>(i)] = static_cast<f32>(new_mean);
    var_[static_cast<std::size_t>(i)] = static_cast<f32>(m2 / total);
  }
  count_ = total;
}

std::vector<f32> Normalizer::divisor() const {
  std::vector<f32> d(var_.size());
  for (std::size_t i = 0; i < d.size(); ++i) d[i] = std::sqrt(var_[i]) + 0.01f;
  return d;
}

void Normalizer::apply(const f32* x, i32 batch, f32* y) const {
  const i32 n = size();
  const std::vector<f32> div = divisor();
  for (i32 r = 0; r < batch; ++r)
    for (i32 i = 0; i < n; ++i) {
      const std::size_t k = static_cast<std::size_t>(r) * n + i;
      y[k] = (x[k] - mean_[static_cast<std::size_t>(i)]) / div[static_cast<std::size_t>(i)];
    }
}

}  // namespace kern::train
