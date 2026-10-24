// Sound from a physical simulation, after Ange Yaghi's engine-sim (github.com/ange-yaghi/engine-sim,
// MIT, Copyright 2022 AngeTheGreat): the simulation writes a physical quantity (in engine-sim the
// exhaust flow of each cylinder, here the forces a robot's servos put into its shell) at its own
// high rate, and the synthesizer turns it into sound the way engine-sim's Synthesizer does:
//
//   input (simulation rate) -> linear interpolation to the audio rate through a 4th-order
//   Butterworth low-pass (anti-aliasing) -> jitter (the input sampled at slightly random instants)
//   -> DC removed (10 Hz) -> mix of the signal's derivative (dF/dt: what a surface radiates) and the
//   signal under turbulent amplitude noise -> convolution with the impulse response of what carries
//   the sound -> anti-aliasing -> leveling -> volume.
//
// Ported in plain C++ for Kern: single-threaded (Kern's mixer pulls, and the simulation that feeds
// the input runs inside that pull, as engine-sim keeps its simulation just ahead of the audio
// buffer), float output in -1..1 instead of int16 (the leveler's target scaled to match), Kern's
// xorshift for rand() / std::default_random_engine. The convolution keeps engine-sim's direct form
// (ConvolutionFilter); an impulse response that is a sum of decaying modes can instead be given as
// those modes and is then computed as a resonator bank - the same convolution, exactly, at a few
// operations per sample.
#pragma once

#include <vector>

#include "kern/core/base.h"

namespace kern::audio::physical {

/// engine-sim ButterworthLowPassFilter: 4th-order Butterworth low-pass (bilinear transform).
template <typename T>
class ButterworthLowPass {
 public:
  void set_cutoff(T cutoff, T rate);
  T f(T x);

 private:
  T a_[5] = {1, 0, 0, 0, 0}, f4_ = 0;
  T x_[4] = {}, y_[4] = {};  // [0] most recent
};

/// engine-sim LowPassFilter: one-pole RC low-pass.
struct LowPass {
  f32 dt = 1 / 44100.0f, rc = 0, y = 0;
  void set_cutoff(f32 hz) { rc = 1.0f / (hz * 2.0f * 3.14159265358979f); }
  f32 f(f32 x) {
    const f32 alpha = dt / (rc + dt);
    y = alpha * x + (1 - alpha) * y;
    return y;
  }
};

/// engine-sim DerivativeFilter.
struct Derivative {
  f32 dt = 0, previous = 0;
  f32 f(f32 x) {
    const f32 d = (x - previous) / dt;
    previous = x;
    return d;
  }
};

/// engine-sim LevelingFilter: an automatic gain that follows the peak.
struct Leveler {
  f32 peak = 1, attenuation = 1, target = 1, min_level = 0, max_level = 1;
  f32 f(f32 x);
};

/// engine-sim ConvolutionFilter: direct-form FIR convolution with an impulse response.
class Convolution {
 public:
  void set_impulse_response(std::vector<f32> ir);
  bool empty() const { return ir_.empty(); }
  f32 f(f32 x);

 private:
  std::vector<f32> ir_, shift_;
  i32 offset_ = 0;
};

/// An impulse response made of decaying modes, h(t) = sum a_i e^(-t/tau_i) sin(2 pi f_i t), computed
/// as the equivalent resonator bank (one two-pole filter per mode, impulse-invariant).
class ModalResponse {
 public:
  struct Mode { f32 hz, tau, amp; };
  void set(const std::vector<Mode>& modes, f32 rate);
  f32 f(f32 x);

 private:
  struct R { f32 b1 = 0, a1 = 0, a2 = 0, y1 = 0, y2 = 0, x1 = 0; };
  std::vector<R> r_;
};

/// engine-sim JitterFilter: reads its input back from a short history at a random, low-passed delay.
class Jitter {
 public:
  void initialize(i32 max_jitter, f32 noise_cutoff, f32 rate);
  void set_scale(f32 s) { scale_ = s; }
  f32 f(f32 x);

 private:
  ButterworthLowPass<f32> noise_;
  std::vector<f32> history_;
  i32 offset_ = 0;
  f32 scale_ = 0;
  u32 rng_ = 0x9E3779B9u;
};

/// engine-sim Synthesizer::AudioParameters (the leveler's target for -1..1 output).
struct AudioParameters {
  f32 volume = 1;
  f32 convolution = 1;
  f32 dF_F_mix = 0.01f;
  f32 input_sample_noise = 0.5f;
  f32 input_sample_noise_cutoff = 10000;
  f32 air_noise = 1;
  f32 air_noise_cutoff = 2000;
  f32 leveler_target = 0.9f;
  f32 leveler_max_gain = 1.9f;
  f32 leveler_min_gain = 0.00001f;
};

/// engine-sim Synthesizer, one or more input channels, pulled by the audio side.
class Synthesizer {
 public:
  struct Parameters {
    i32 channels = 1;
    f32 input_rate = 10000;          // the simulation's rate
    f32 audio_rate = 44100;
    f32 input_antialias_hz = 1900;   // engine-sim: 1900 Hz on each input
    AudioParameters audio;
  };
  void initialize(const Parameters& p);
  /// The response a channel's signal is convolved with (engine-sim initializeImpulseResponse).
  void set_impulse_response(i32 channel, std::vector<f32> ir, f32 volume);
  void set_modal_response(i32 channel, const std::vector<ModalResponse::Mode>& modes);
  /// One simulation step: a value per channel (engine-sim writeInput).
  void write_input(const f64* values);
  /// Audio-rate samples interpolated from the input so far and not yet rendered.
  i32 pending() const { return static_cast<i32>(queue_[0].size()) - read_; }
  /// Renders up to `frames` samples into `out`; returns how many there were input for.
  i32 render(f32* out, i32 frames);
  AudioParameters& audio() { return p_.audio; }
  const Parameters& parameters() const { return p_; }

 private:
  struct Filters {
    Convolution convolution;
    ModalResponse modal;
    bool modal_set = false;
    Derivative derivative;
    Jitter jitter;
    ButterworthLowPass<f32> air_noise;
    LowPass dc;
    ButterworthLowPass<f64> antialias;
    f64 last_input = 0;
  };
  f32 render_one(i32 index);
  f32 noise();

  Parameters p_;
  std::vector<Filters> filters_;
  std::vector<std::vector<f32>> queue_;  // per channel, audio rate
  i32 read_ = 0;
  f64 write_offset_ = 0;                 // audio samples written, fractional
  ButterworthLowPass<f32> antialias_;
  Leveler leveler_;
  u32 rng_ = 0x2545F491u;
};

}  // namespace kern::audio::physical
