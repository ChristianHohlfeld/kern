// The Lehrer's networks (see nn.h). Weights are stored input-major (w[i * out + o]) so that both
// passes run as contiguous axpy loops the compiler vectorises without reassociating sums.
#include "kern/lehrer/nn.h"
#include "kern/core/dmath.h"

#include <cmath>
#include <cstring>

namespace kern::lehrer {

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

namespace {

// y[r][o] = b[o] + sum_i x[r][i] w[i][o] for rows r: four rows share each pass over w.
void dense(const f32* x, i32 rows, i32 in, i32 out, const f32* w, const f32* b, f32* y) {
  i32 r = 0;
  for (; r + 4 <= rows; r += 4) {
    f32* y0 = y + static_cast<std::size_t>(r) * out;
    f32 *y1 = y0 + out, *y2 = y1 + out, *y3 = y2 + out;
    for (i32 o = 0; o < out; ++o) y0[o] = y1[o] = y2[o] = y3[o] = b[o];
    const f32* x0 = x + static_cast<std::size_t>(r) * in;
    const f32 *x1 = x0 + in, *x2 = x1 + in, *x3 = x2 + in;
    for (i32 i = 0; i < in; ++i) {
      const f32 a0 = x0[i], a1 = x1[i], a2 = x2[i], a3 = x3[i];
      const f32* wi = w + static_cast<std::size_t>(i) * out;
      for (i32 o = 0; o < out; ++o) {
        const f32 wv = wi[o];
        y0[o] += wv * a0;
        y1[o] += wv * a1;
        y2[o] += wv * a2;
        y3[o] += wv * a3;
      }
    }
  }
  for (; r < rows; ++r) {
    f32* yr = y + static_cast<std::size_t>(r) * out;
    std::memcpy(yr, b, sizeof(f32) * static_cast<std::size_t>(out));
    const f32* xr = x + static_cast<std::size_t>(r) * in;
    for (i32 i = 0; i < in; ++i) {
      const f32 a = xr[i];
      const f32* wi = w + static_cast<std::size_t>(i) * out;
      for (i32 o = 0; o < out; ++o) yr[o] += wi[o] * a;
    }
  }
}

}  // namespace

void Mlp::forward(const f32* x, i32 batch, f32* out, Cache* cache) const {
  std::vector<f32> buf[2];
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
    std::vector<f32>& target = cache ? cache->act[static_cast<std::size_t>(l) + 1] : buf[l & 1];
    if (last && !cache) {
      y = out;
    } else {
      target.resize(static_cast<std::size_t>(batch) * static_cast<std::size_t>(o_n));
      y = target.data();
    }
    dense(in_ptr, batch, in, o_n, weight(l), bias(l), y);
    if (!last) {
      const std::size_t n = static_cast<std::size_t>(batch) * static_cast<std::size_t>(o_n);
      for (std::size_t k = 0; k < n; ++k) y[k] = y[k] > 0 ? y[k] : kern::dm::expm1(y[k]);  // ELU
    } else if (cache) {
      std::memcpy(out, y, sizeof(f32) * static_cast<std::size_t>(batch) * static_cast<std::size_t>(o_n));
    }
    in_ptr = y;
  }
}

void Mlp::backward(const Cache& cache, const f32* dout, f32* grad, f32* dx) const {
  const i32 batch = cache.batch;
  std::vector<f32> delta(dout, dout + static_cast<std::size_t>(batch) * static_cast<std::size_t>(outputs())), prev, wt;
  for (i32 l = layers() - 1; l >= 0; --l) {
    const i32 in = dims_[static_cast<std::size_t>(l)], o_n = dims_[static_cast<std::size_t>(l) + 1];
    const f32* x = cache.act[static_cast<std::size_t>(l)].data();
    const f32* w = weight(l);
    f32* gw = grad + w_[static_cast<std::size_t>(l)];
    f32* gb = grad + b_[static_cast<std::size_t>(l)];
    const f32* d = delta.data();
    // Bias and weight gradients: gw[i][o] += sum_r x[r][i] d[r][o], four rows per pass.
    for (i32 r = 0; r < batch; ++r)
      for (i32 o = 0; o < o_n; ++o) gb[o] += d[static_cast<std::size_t>(r) * o_n + o];
    i32 r = 0;
    for (; r + 4 <= batch; r += 4) {
      const f32* x0 = x + static_cast<std::size_t>(r) * in;
      const f32 *x1 = x0 + in, *x2 = x1 + in, *x3 = x2 + in;
      const f32* d0 = d + static_cast<std::size_t>(r) * o_n;
      const f32 *d1 = d0 + o_n, *d2 = d1 + o_n, *d3 = d2 + o_n;
      for (i32 i = 0; i < in; ++i) {
        const f32 a0 = x0[i], a1 = x1[i], a2 = x2[i], a3 = x3[i];
        f32* gwi = gw + static_cast<std::size_t>(i) * o_n;
        for (i32 o = 0; o < o_n; ++o) gwi[o] += a0 * d0[o] + a1 * d1[o] + a2 * d2[o] + a3 * d3[o];
      }
    }
    for (; r < batch; ++r) {
      const f32* xr = x + static_cast<std::size_t>(r) * in;
      const f32* dr = d + static_cast<std::size_t>(r) * o_n;
      for (i32 i = 0; i < in; ++i) {
        const f32 a = xr[i];
        f32* gwi = gw + static_cast<std::size_t>(i) * o_n;
        for (i32 o = 0; o < o_n; ++o) gwi[o] += a * dr[o];
      }
    }
    if (l == 0 && !dx) break;
    // Input gradient: dx[r][i] = sum_o d[r][o] w[i][o] = d . w^T (with w transposed once: [o][i]).
    wt.resize(static_cast<std::size_t>(in) * static_cast<std::size_t>(o_n));
    for (i32 i = 0; i < in; ++i)
      for (i32 o = 0; o < o_n; ++o) wt[static_cast<std::size_t>(o) * in + i] = w[static_cast<std::size_t>(i) * o_n + o];
    std::vector<f32> zero(static_cast<std::size_t>(in), 0.0f);
    prev.resize(static_cast<std::size_t>(batch) * static_cast<std::size_t>(in));
    dense(d, batch, o_n, in, wt.data(), zero.data(), prev.data());
    if (l > 0) {
      // Through the ELU of the layer below: y > 0 passes, else dy/dx = y + 1.
      for (std::size_t k = 0; k < prev.size(); ++k) {
        const f32 y = x[k];
        if (y <= 0) prev[k] *= y + 1;
      }
      delta.swap(prev);
    } else {
      std::memcpy(dx, prev.data(), sizeof(f32) * prev.size());
    }
  }
}

void Adam::step(std::vector<f32>& params, std::vector<f32>& grad) { step({Block{params.data(), grad.data(), params.size()}}); }

void Adam::step(std::initializer_list<Block> blocks) {
  std::size_t total = 0;
  for (const Block& b : blocks) total += b.size;
  if (m_.size() != total) { m_.assign(total, 0.0f); v_.assign(total, 0.0f); t_ = 0; }
  if (max_norm > 0) {
    f64 sq = 0;
    for (const Block& b : blocks)
      for (std::size_t k = 0; k < b.size; ++k) sq += static_cast<f64>(b.grad[k]) * b.grad[k];
    const f32 norm = static_cast<f32>(std::sqrt(sq));
    if (norm > max_norm) {
      const f32 s = max_norm / (norm + 1e-6f);
      for (const Block& b : blocks)
        for (std::size_t k = 0; k < b.size; ++k) b.grad[k] *= s;
    }
  }
  ++t_;
  const f32 c1 = 1 - kern::dm::pow(beta1, static_cast<f32>(t_)), c2 = 1 - kern::dm::pow(beta2, static_cast<f32>(t_));
  const f32 step = lr / c1, root_c2 = std::sqrt(c2);
  std::size_t at = 0;
  for (const Block& b : blocks) {
    for (std::size_t k = 0; k < b.size; ++k, ++at) {
      const f32 g = b.grad[k];
      m_[at] = beta1 * m_[at] + (1 - beta1) * g;
      v_[at] = beta2 * v_[at] + (1 - beta2) * g * g;
      b.params[k] -= step * m_[at] / (std::sqrt(v_[at]) / root_c2 + eps);
    }
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

}  // namespace kern::lehrer
