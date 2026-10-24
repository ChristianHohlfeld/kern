// physical_synth.h: engine-sim's synthesizer and filters (MIT, Copyright 2022 AngeTheGreat), ported.
#include "kern/audio/physical_synth.h"

#include <algorithm>
#include <cmath>

namespace kern::audio::physical {

namespace {
constexpr f64 kPi = 3.14159265358979323846;
inline u32 xorshift(u32& s) {
  s ^= s << 13;
  s ^= s >> 17;
  s ^= s << 5;
  return s;
}
}  // namespace

// ---- ButterworthLowPassFilter (butterworth_low_pass_filter.h) -----------------------------------

template <typename T>
void ButterworthLowPass<T>::set_cutoff(T cutoff, T rate) {
  const T f = static_cast<T>(std::tan(kPi * cutoff / rate));
  const T f2 = f * f, f3 = f2 * f, f4 = f2 * f2;
  const T m = static_cast<T>(-2.0 * std::cos(5.0 * kPi / 8.0));
  const T n = static_cast<T>(-2.0 * std::cos(7.0 * kPi / 8.0));
  a_[0] = 1 + (m + n) * f + (2 + n * m) * f2 + (m + n) * f3 + f4;
  a_[1] = (-4 - 2 * (n + m) * f + 2 * (m + n) * f3 + 4 * f4) / a_[0];
  a_[2] = (6 - 2 * (2 + m * n) * f2 + 6 * f4) / a_[0];
  a_[3] = (-4 + 2 * (m + n) * f - 2 * (m + n) * f3 + 4 * f4) / a_[0];
  a_[4] = (1 - (n + m) * f + (2 + m * n) * f2 - (m + n) * f3 + f4) / a_[0];
  f4_ = f4;
}

template <typename T>
T ButterworthLowPass<T>::f(T x) {
  const T n = f4_ / a_[0] * (x + 4 * x_[0] + 6 * x_[1] + 4 * x_[2] + x_[3]);
  const T d = -a_[1] * y_[0] - a_[2] * y_[1] - a_[3] * y_[2] - a_[4] * y_[3];
  const T y = n + d;
  x_[3] = x_[2]; x_[2] = x_[1]; x_[1] = x_[0]; x_[0] = x;
  y_[3] = y_[2]; y_[2] = y_[1]; y_[1] = y_[0]; y_[0] = y;
  return y;
}

template class ButterworthLowPass<f32>;
template class ButterworthLowPass<f64>;

// ---- LevelingFilter (leveling_filter.cpp) --------------------------------------------------------

f32 Leveler::f(f32 x) {
  peak = 0.999f * peak;
  if (std::fabs(x) > peak) peak = std::fabs(x);
  if (peak == 0) return 0;
  const f32 a = std::clamp(target / peak, min_level, max_level);
  attenuation = 0.9f * attenuation + 0.1f * a;
  return x * attenuation;
}

// ---- ConvolutionFilter (convolution_filter.cpp) -------------------------------------------------

void Convolution::set_impulse_response(std::vector<f32> ir) {
  ir_ = std::move(ir);
  shift_.assign(ir_.size(), 0.0f);
  offset_ = 0;
}

f32 Convolution::f(f32 x) {
  const i32 n = static_cast<i32>(ir_.size());
  if (n == 0) return x;
  shift_[static_cast<std::size_t>(offset_)] = x;
  f32 r = 0;
  for (i32 i = 0; i < n - offset_; ++i) r += ir_[static_cast<std::size_t>(i)] * shift_[static_cast<std::size_t>(i + offset_)];
  for (i32 i = n - offset_; i < n; ++i) r += ir_[static_cast<std::size_t>(i)] * shift_[static_cast<std::size_t>(i - (n - offset_))];
  offset_ = (offset_ - 1 + n) % n;
  return r;
}

// ---- A modal impulse response as its resonator bank ---------------------------------------------

void ModalResponse::set(const std::vector<Mode>& modes, f32 rate) {
  // h[k] = a e^(-k/(tau fs)) sin(w k): its z-transform is a b1 z^-1 / (1 + a1 z^-1 + a2 z^-2) with
  // r = e^(-1/(tau fs)), b1 = r sin w, a1 = -2 r cos w, a2 = r^2 - the convolution, exactly.
  r_.clear();
  for (const Mode& m : modes) {
    if (!(m.hz > 0) || m.hz >= 0.5f * rate || !(m.tau > 0)) continue;
    const f64 w = 2 * kPi * m.hz / rate, r = std::exp(-1.0 / (m.tau * rate));
    R x;
    x.b1 = static_cast<f32>(m.amp * r * std::sin(w));
    x.a1 = static_cast<f32>(-2 * r * std::cos(w));
    x.a2 = static_cast<f32>(r * r);
    r_.push_back(x);
  }
}

f32 ModalResponse::f(f32 x) {
  f32 sum = 0;
  for (R& r : r_) {
    const f32 y = r.b1 * r.x1 - r.a1 * r.y1 - r.a2 * r.y2;
    r.x1 = x;
    r.y2 = r.y1;
    r.y1 = y;
    sum += y;
  }
  return sum;
}

// ---- JitterFilter (jitter_filter.h) -------------------------------------------------------------

void Jitter::initialize(i32 max_jitter, f32 noise_cutoff, f32 rate) {
  history_.assign(static_cast<std::size_t>(std::max(1, max_jitter)), 0.0f);
  offset_ = 0;
  noise_.set_cutoff(noise_cutoff, rate);
}

f32 Jitter::f(f32 x) {
  const i32 n = static_cast<i32>(history_.size());
  history_[static_cast<std::size_t>(offset_)] = x;
  if (++offset_ >= n) offset_ = 0;
  const f32 uniform = static_cast<f32>(xorshift(rng_) >> 8) * (1.0f / 16777216.0f) * static_cast<f32>(n - 1);
  const f32 s = noise_.f(uniform * scale_);
  const f32 s0 = std::clamp(std::floor(s), 0.0f, static_cast<f32>(n - 1));
  const f32 s1 = std::clamp(std::ceil(s), 0.0f, static_cast<f32>(n - 1));
  const f32 frac = s - s0;
  const i32 i0 = static_cast<i32>(s0) + offset_, i1 = static_cast<i32>(s1) + offset_;
  const f32 v0 = history_[static_cast<std::size_t>(i0 >= n ? i0 - n : i0)];
  const f32 v1 = history_[static_cast<std::size_t>(i1 >= n ? i1 - n : i1)];
  return v1 * frac + v0 * (1 - frac);
}

// ---- Synthesizer (synthesizer.cpp) --------------------------------------------------------------

void Synthesizer::initialize(const Parameters& p) {
  p_ = p;
  p_.channels = std::max(1, p_.channels);
  filters_.assign(static_cast<std::size_t>(p_.channels), Filters{});
  queue_.assign(static_cast<std::size_t>(p_.channels), {});
  read_ = 0;
  write_offset_ = 0;
  for (Filters& f : filters_) {
    f.air_noise.set_cutoff(p_.audio.air_noise_cutoff, p_.audio_rate);
    f.derivative.dt = 1 / p_.audio_rate;
    f.dc.set_cutoff(10.0f);
    f.dc.dt = 1 / p_.audio_rate;
    f.jitter.initialize(10, p_.audio.input_sample_noise_cutoff, p_.audio_rate);
    f.antialias.set_cutoff(p_.input_antialias_hz, p_.audio_rate);
  }
  leveler_ = Leveler{};
  leveler_.target = p_.audio.leveler_target;
  leveler_.peak = p_.audio.leveler_target;
  leveler_.max_level = p_.audio.leveler_max_gain;
  leveler_.min_level = p_.audio.leveler_min_gain;
  antialias_.set_cutoff(p_.audio_rate * 0.45f, p_.audio_rate);
}

void Synthesizer::set_impulse_response(i32 channel, std::vector<f32> ir, f32 volume) {
  if (channel < 0 || channel >= p_.channels) return;
  // engine-sim: the response is clipped where it falls under 100 / 32767, at most 10000 samples.
  std::size_t length = 0;
  for (std::size_t i = 0; i < ir.size(); ++i)
    if (std::fabs(ir[i]) > 100.0f / 32767.0f) length = i + 1;
  ir.resize(std::min<std::size_t>(10000, length));
  for (f32& v : ir) v *= volume;
  filters_[static_cast<std::size_t>(channel)].convolution.set_impulse_response(std::move(ir));
}

void Synthesizer::set_modal_response(i32 channel, const std::vector<ModalResponse::Mode>& modes) {
  if (channel < 0 || channel >= p_.channels) return;
  Filters& f = filters_[static_cast<std::size_t>(channel)];
  f.modal.set(modes, p_.audio_rate);
  f.modal_set = true;
}

void Synthesizer::write_input(const f64* values) {
  // engine-sim writeInput: the input advances the write offset by audio / input rate; the audio
  // samples up to it are interpolated linearly from the last input value, through the anti-aliasing.
  const f64 before = write_offset_;
  write_offset_ += static_cast<f64>(p_.audio_rate) / p_.input_rate;
  const f64 distance = write_offset_ - before;
  for (i32 c = 0; c < p_.channels; ++c) {
    Filters& f = filters_[static_cast<std::size_t>(c)];
    std::vector<f32>& q = queue_[static_cast<std::size_t>(c)];
    for (f64 s = std::ceil(before); s < write_offset_; s += 1.0) {
      const f64 t = (s - before) / distance;
      const f64 sample = f.last_input * (1 - t) + values[c] * t;
      q.push_back(static_cast<f32>(f.antialias.f(sample)));
    }
    f.last_input = values[c];
  }
}

f32 Synthesizer::noise() { return static_cast<f32>(xorshift(rng_) >> 8) * (2.0f / 16777216.0f) - 1.0f; }

f32 Synthesizer::render_one(i32 index) {
  const AudioParameters& a = p_.audio;
  f32 signal = 0;
  for (i32 c = 0; c < p_.channels; ++c) {
    Filters& fl = filters_[static_cast<std::size_t>(c)];
    fl.jitter.set_scale(a.input_sample_noise);
    const f32 f_in = fl.jitter.f(queue_[static_cast<std::size_t>(c)][static_cast<std::size_t>(index)]);
    const f32 f_dc = fl.dc.f(f_in);
    const f32 f = f_in - f_dc;
    const f32 f_p = fl.derivative.f(f_in);
    const f32 r = fl.air_noise.f(noise());
    const f32 r_mixed = a.air_noise * r + (1 - a.air_noise);
    f32 v_in = f_p * a.dF_F_mix + f * r_mixed * (1 - a.dF_F_mix);
    if (std::fpclassify(v_in) == FP_SUBNORMAL) v_in = 0;
    const f32 convolved = fl.modal_set ? fl.modal.f(v_in) : fl.convolution.empty() ? v_in : fl.convolution.f(v_in);
    signal += a.convolution * convolved + (1 - a.convolution) * v_in;
  }
  signal = antialias_.f(signal);
  leveler_.target = a.leveler_target;
  return leveler_.f(signal) * a.volume;
}

i32 Synthesizer::render(f32* out, i32 frames) {
  const i32 n = std::min(frames, pending());
  for (i32 i = 0; i < n; ++i) out[i] = render_one(read_ + i);
  read_ += n;
  // Drop what has been rendered (engine-sim removeBeginning), keeping the vectors short.
  if (read_ > 4096) {
    for (std::vector<f32>& q : queue_) q.erase(q.begin(), q.begin() + read_);
    write_offset_ -= read_;
    read_ = 0;
  }
  return n;
}

}  // namespace kern::audio::physical
