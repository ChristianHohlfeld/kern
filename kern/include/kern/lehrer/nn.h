// The Lehrer's neural networks, in plain C++: a multilayer perceptron (ELU between layers, linear
// output, as rsl_rl's actor and critic), its gradient, Adam, and the running observation normaliser.
// Batched and cache-friendly; no dependency beyond the standard library.
#pragma once

#include <initializer_list>
#include <utility>
#include <vector>

#include "kern/core/base.h"
#include "kern/lehrer/env.h"

namespace kern::lehrer {

class Mlp {
 public:
  Mlp() = default;
  /// dims: inputs, hidden..., outputs. Weights as PyTorch's nn.Linear initialises them (uniform
  /// +-1/sqrt(fan_in)), the last layer scaled by `output_gain`.
  void init(const std::vector<i32>& dims, Rng& rng, f32 output_gain = 1.0f);
  const std::vector<i32>& dims() const { return dims_; }
  i32 inputs() const { return dims_.front(); }
  i32 outputs() const { return dims_.back(); }
  i32 layers() const { return static_cast<i32>(dims_.size()) - 1; }
  std::vector<f32>& params() { return params_; }
  const std::vector<f32>& params() const { return params_; }
  /// Layer l: weight w[i * out + o] (input-major) and bias b[o].
  const f32* weight(i32 l) const { return params_.data() + w_[static_cast<std::size_t>(l)]; }
  const f32* bias(i32 l) const { return params_.data() + b_[static_cast<std::size_t>(l)]; }
  f32* weight(i32 l) { return params_.data() + w_[static_cast<std::size_t>(l)]; }
  f32* bias(i32 l) { return params_.data() + b_[static_cast<std::size_t>(l)]; }

  /// What a forward pass keeps for the backward one.
  struct Cache {
    i32 batch = 0;
    std::vector<std::vector<f32>> act;  // act[0] = input, act[l + 1] = output of layer l (after ELU)
  };
  /// out[b * outputs + o] for `batch` inputs x[b * inputs + i]. With a cache, it can be differentiated.
  void forward(const f32* x, i32 batch, f32* out, Cache* cache = nullptr) const;
  /// Adds d(loss)/d(params) to grad (same layout as params) given d(loss)/d(out); the cache of the
  /// forward pass. dx (optional) receives d(loss)/d(input).
  void backward(const Cache& cache, const f32* dout, f32* grad, f32* dx = nullptr) const;

 private:
  std::vector<i32> dims_;
  std::vector<f32> params_;
  std::vector<std::size_t> w_, b_;
};

/// Adam (PyTorch's defaults) with the gradient clipped to a global norm first (rsl_rl: 1.0).
class Adam {
 public:
  f32 lr = 1e-3f, beta1 = 0.9f, beta2 = 0.999f, eps = 1e-8f, max_norm = 1.0f;
  void step(std::vector<f32>& params, std::vector<f32>& grad);
  /// One step over several parameter blocks clipped together (a network and its std, say).
  struct Block { f32* params; f32* grad; std::size_t size; };
  void step(std::initializer_list<Block> blocks);
  void reset() { m_.clear(); v_.clear(); t_ = 0; }
  // The moments and the step count (checkpoints).
  std::vector<f32>& m() { return m_; }
  std::vector<f32>& v() { return v_; }
  i64& t() { return t_; }

 private:
  std::vector<f32> m_, v_;
  i64 t_ = 0;
};

/// rsl_rl's EmpiricalNormalization: running mean and variance; y = (x - mean) / (std + 0.01). The
/// export bakes it into the ONNX as Sub and Div, as Pollen's networks have it.
class Normalizer {
 public:
  void init(i32 size);
  void update(const f32* x, i32 batch);
  void apply(const f32* x, i32 batch, f32* y) const;
  const std::vector<f32>& mean() const { return mean_; }
  std::vector<f32> divisor() const;  // std + eps
  i32 size() const { return static_cast<i32>(mean_.size()); }
  const std::vector<f32>& variance() const { return var_; }
  f64 count() const { return count_; }
  void set(std::vector<f32> mean, std::vector<f32> variance, f64 count) { mean_ = std::move(mean); var_ = std::move(variance); count_ = count; }

 private:
  std::vector<f32> mean_, var_;
  f64 count_ = 0;
};

}  // namespace kern::lehrer
