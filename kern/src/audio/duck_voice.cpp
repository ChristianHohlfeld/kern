// The Micro Duck's voice synthesiser, ported from the `sounds` crate of
// pollen-robotics/microduck (Rust, Apache-2.0, Copyright Pollen Robotics):
// sounds/src/rng.rs, personality.rs, synth.rs, voices.rs and seed_from_id /
// variant_count / render from lib.rs.
//
// The port is meant to be sample-exact at 48 kHz: the same xoshiro256++ /
// splitmix64 stream (including the Box-Muller spare that carries across
// calls), the same order of random draws, the same constants, and f32 vs f64
// arithmetic exactly where the Rust uses each. Expressions that draw from the
// RNG more than once are split into statements, because C++ (unlike Rust) does
// not fix the evaluation order of operands. Floating-point contraction (FMA) is
// disabled (clang and GCC pragmas) so a*b+c rounds twice like Rust's.
//
// What changed against the original:
//  - Personality::register is `register_shift` (`register` is a C++ keyword).
//  - lib.rs's file output (to_wav / render_all / hardware_seed) is left out;
//    seed_from_id is kept, with a small built-in SHA-256 instead of `sha2`.
//  - Rendering at a rate other than 48 kHz resamples the 48 kHz result
//    linearly (each wheee segment on its own).
//  - Not ported: stream.rs (the robot's live audio stream), chorale/ and the
//    CLI (main.rs) - the recipes do not depend on them.
#include "kern/audio/duck_voice.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>

// No FMA contraction: Rust rounds every a * b + c twice, so must this file.
#if defined(__clang__)
#pragma clang fp contract(off)
#elif defined(__GNUC__)
#pragma GCC optimize("fp-contract=off")
#endif

namespace kern::audio {

namespace {

constexpr f64 kTau = 6.283185307179586;  // std::f64::consts::TAU
constexpr u32 kSr = kDuckVoiceRate;
constexpr f64 kTunedSr = 22050.0;

// ---------------------------------------------------------------------------
// rng.rs

u64 rotl(u64 x, int k) { return (x << k) | (x >> (64 - k)); }

struct Rng {
  u64 s[4];
  bool has_spare = false;
  f32 spare = 0;

  explicit Rng(u32 seed) {
    u64 x = seed;
    for (u64& w : s) {
      x += 0x9E3779B97F4A7C15ull;
      u64 z = x;
      z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
      z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
      w = z ^ (z >> 31);
    }
  }

  u64 next_u64() {
    const u64 result = rotl(s[0] + s[3], 23) + s[0];
    const u64 t = s[1] << 17;
    s[2] ^= s[0];
    s[3] ^= s[1];
    s[1] ^= s[2];
    s[0] ^= s[3];
    s[2] ^= t;
    s[3] = rotl(s[3], 45);
    return result;
  }

  f64 random() { return static_cast<f64>(next_u64() >> 11) * (1.0 / static_cast<f64>(u64{1} << 53)); }
  f64 uniform(f64 lo, f64 hi) {
    const f64 r = random();
    return lo + (hi - lo) * r;
  }
  i64 integers(i64 lo, i64 hi) {
    const f64 r = random();
    return lo + static_cast<i64>(std::floor(r * static_cast<f64>(hi - lo)));
  }
  f64 choice(const f64* choices, size_t n) { return choices[static_cast<size_t>(integers(0, static_cast<i64>(n)))]; }

  f32 standard_normal() {
    if (has_spare) {
      has_spare = false;
      return spare;
    }
    const f64 r = 1.0 - random();
    const f64 theta = kTau * random();
    const f64 mag = std::sqrt(-2.0 * std::log(r));
    spare = static_cast<f32>(mag * std::sin(theta));
    has_spare = true;
    return static_cast<f32>(mag * std::cos(theta));
  }
};

u32 crc32(std::string_view data) {
  u32 crc = 0xFFFFFFFFu;
  for (char c : data) {
    crc ^= static_cast<u8>(c);
    for (int k = 0; k < 8; ++k) {
      const u32 mask = 0u - (crc & 1u);
      crc = (crc >> 1) ^ (0xEDB88320u & mask);
    }
  }
  return ~crc;
}

// ---------------------------------------------------------------------------
// personality.rs

Rng variant_rng(const DuckPersonality& p, std::string_view tag, u32 variant) {
  const u64 h = (static_cast<u64>(p.seed) * 1000003ull) ^ static_cast<u64>(crc32(tag)) ^
                (static_cast<u64>(variant) * 2654435761ull);
  return Rng(static_cast<u32>(h));
}

constexpr size_t kHarmonics = 7;

void harmonics(const DuckPersonality& p, f64 (&weights)[kHarmonics]) {
  for (size_t n = 1; n <= kHarmonics; ++n) {
    const f64 nf = static_cast<f64>(n);
    const f64 base = 1.0 / std::pow(nf, p.tilt);
    const f64 high_lift = p.brightness * std::pow(nf / static_cast<f64>(kHarmonics), 1.5);
    const f64 nasal = p.nasal * ((n == 2 || n == 3) ? 0.6 : 0.0);
    const f64 skew = p.harmonic_skew >= 0.0 ? p.harmonic_skew * (n % 2 == 0 ? 0.4 : -0.2)
                                            : -p.harmonic_skew * (n % 2 == 0 ? -0.3 : 0.4);
    const f64 formant = n == p.formant_n ? p.formant_gain : 0.0;
    weights[n - 1] = std::max(base + high_lift + nasal + skew + formant * base * 1.5, 0.0);
  }
  weights[0] = std::max(weights[0], 0.7);
}

// ---------------------------------------------------------------------------
// synth.rs

using Buf = std::vector<f32>;
struct Point {
  f64 t, v;
};

Buf t_axis(f64 duration_s) {
  const size_t n = std::max<size_t>(static_cast<size_t>(std::round(duration_s * static_cast<f64>(kSr))), 1);
  Buf t(n);
  for (size_t i = 0; i < n; ++i) t[i] = static_cast<f32>(i) / static_cast<f32>(kSr);
  return t;
}

Buf lerp(const Buf& t, const Point* pts, size_t count) {
  Buf out(t.size());
  for (size_t i = 0; i < t.size(); ++i) {
    const f64 x = t[i];
    f32 v = static_cast<f32>(pts[count - 1].v);
    if (x <= pts[0].t) {
      v = static_cast<f32>(pts[0].v);
    } else {
      for (size_t k = 0; k + 1 < count; ++k) {
        const f64 x0 = pts[k].t, y0 = pts[k].v, x1 = pts[k + 1].t, y1 = pts[k + 1].v;
        if (x <= x1) {
          v = x1 <= x0 ? static_cast<f32>(y1) : static_cast<f32>(y0 + (y1 - y0) * (x - x0) / (x1 - x0));
          break;
        }
      }
    }
    out[i] = v;
  }
  return out;
}
template <size_t N>
Buf lerp(const Buf& t, const Point (&pts)[N]) {
  return lerp(t, pts, N);
}

Buf expdecay(const Buf& t, f64 attack_s, f64 decay_s) {
  const f32 attack = static_cast<f32>(std::max(attack_s, 1e-4));
  const f32 decay = static_cast<f32>(std::max(decay_s, 1e-4));
  Buf out(t.size());
  for (size_t i = 0; i < t.size(); ++i) {
    const f32 x = t[i];
    const f32 a = std::clamp(x / attack, 0.0f, 1.0f);
    const f32 d = std::exp(-std::max(x - attack, 0.0f) / decay);
    out[i] = a * d;
  }
  return out;
}

Buf bell(const Buf& t, f64 attack_s, f64 release_s) {
  const f64 total = t.empty() ? 0.0 : static_cast<f64>(t.back());
  const f64 rel_start = std::max(total - release_s, 0.0);
  const f64 attack = std::max(attack_s, 1e-4);
  const f64 release = std::max(release_s, 1e-4);
  Buf out(t.size());
  for (size_t i = 0; i < t.size(); ++i) {
    const f64 x = t[i];
    f64 v = 1.0;
    if (x < attack_s) v = x / attack;
    if (x > rel_start) v = std::max(1.0 - (x - rel_start) / release, 0.0);
    out[i] = static_cast<f32>(std::clamp(v, 0.0, 1.0));
  }
  return out;
}

Buf phase_from_freq(const Buf& freq) {
  Buf out(freq.size());
  f64 acc = 0.0;
  for (size_t i = 0; i < freq.size(); ++i) {
    acc += static_cast<f64>(freq[i]);
    out[i] = static_cast<f32>(kTau * acc / static_cast<f64>(kSr));
  }
  return out;
}

Buf harmonic_osc(const Buf& phase, const f64 (&weights)[kHarmonics]) {
  Buf out(phase.size(), 0.0f);
  for (size_t k = 0; k < kHarmonics; ++k) {
    if (weights[k] == 0.0) continue;
    const f32 n = static_cast<f32>(k + 1);
    const f32 w = static_cast<f32>(weights[k]);
    for (size_t i = 0; i < phase.size(); ++i) out[i] += w * std::sin(n * phase[i]);
  }
  return out;
}

Buf vibrato(const Buf& t, f64 rate_hz, f64 depth_semitones, f64 phase) {
  if (rate_hz <= 0.0 || depth_semitones <= 0.0) return Buf(t.size(), 1.0f);
  Buf out(t.size());
  for (size_t i = 0; i < t.size(); ++i) {
    const f64 lfo = std::sin(kTau * rate_hz * static_cast<f64>(t[i]) + phase);
    out[i] = static_cast<f32>(std::pow(2.0, depth_semitones * lfo / 12.0));
  }
  return out;
}

// numpy convolve(x, ones(k)/k, mode="same"), with the Rust's running sum.
Buf moving_average_same(const Buf& x, size_t k) {
  const size_t n = x.size();
  Buf out(n, 0.0f);
  const size_t offset = (k - 1) / 2;
  const f32 inv = 1.0f / static_cast<f32>(k);
  f32 acc = 0.0f;
  size_t hi = 0;
  auto sat_sub = [](size_t a, size_t b) { return a > b ? a - b : size_t{0}; };
  for (size_t i = 0; i < n; ++i) {
    const size_t want_hi = std::min(i + offset + 1, n);
    while (hi < want_hi) acc += x[hi++];
    const size_t lo = sat_sub(i + offset + 1, k);
    if (i > 0)
      for (size_t j = sat_sub(i + offset, k); j < lo; ++j) acc -= x[j];
    out[i] = acc * inv;
  }
  return out;
}

Buf jitter(const Buf& t, f64 depth_semitones, Rng& rng) {
  if (depth_semitones <= 0.0) return Buf(t.size(), 1.0f);
  Buf raw(t.size());
  for (f32& v : raw) v = rng.standard_normal();
  const size_t window = std::max<size_t>(static_cast<size_t>(std::round(64.0 * static_cast<f64>(kSr) / kTunedSr)), 1);
  const Buf smoothed = moving_average_same(raw, window);
  Buf out(t.size());
  for (size_t i = 0; i < smoothed.size(); ++i)
    out[i] = static_cast<f32>(std::pow(2.0, depth_semitones * static_cast<f64>(smoothed[i]) / 12.0));
  return out;
}

f32 peak_abs(const Buf& x) {
  f32 m = 0.0f;
  for (f32 v : x) m = std::max(m, std::fabs(v));
  return m;
}

Buf pink_noise(size_t n, Rng& rng) {
  const f32 a = static_cast<f32>(std::pow(0.985, kTunedSr / static_cast<f64>(kSr)));
  f32 leak = 0.0f;
  Buf pink(n);
  for (f32& v : pink) {
    leak = a * leak + rng.standard_normal();
    v = leak;
  }
  const f32 peak = peak_abs(pink) + 1e-9f;
  for (f32& v : pink) v /= peak;
  return pink;
}

Buf click(size_t n, Rng& rng, size_t length) {
  Buf out(n, 0.0f);
  const size_t l = std::min(length, n);
  for (size_t i = 0; i < l; ++i) {
    const f32 fade = 1.0f - static_cast<f32>(i) / static_cast<f32>(l);
    out[i] = static_cast<f32>(rng.uniform(-1.0, 1.0)) * fade * fade;
  }
  return out;
}

void normalise(Buf& x, f64 peak_dbfs) {
  const f32 peak = peak_abs(x) + 1e-9f;
  const f32 target = static_cast<f32>(std::pow(10.0, peak_dbfs / 20.0));
  const f32 gain = target / peak;
  for (f32& v : x) v *= gain;
}

void multiply(Buf& a, const Buf& b) {
  for (size_t i = 0; i < a.size() && i < b.size(); ++i) a[i] = a[i] * b[i];
}

// ---------------------------------------------------------------------------
// voices.rs

f64 attack(const DuckPersonality& p, f64 dur, f64 snappy) {
  const f64 soft = 0.04 * dur;
  const f64 sharp = 0.003 * dur;
  return std::max(soft + (sharp - soft) * p.attack_sharpness * snappy, 0.001);
}

Buf voice(const DuckPersonality& p, const Buf& t, const Buf& freq, Rng& rng, f64 am_scale, f64 breath_scale) {
  const f64 vib_phase = rng.uniform(0.0, kTau);
  const Buf vib = vibrato(t, p.vibrato_rate_hz, p.vibrato_depth, vib_phase);
  const Buf jit = jitter(t, p.jitter_depth, rng);
  Buf f(std::min({freq.size(), vib.size(), jit.size()}));
  for (size_t i = 0; i < f.size(); ++i) f[i] = freq[i] * vib[i] * jit[i];
  const Buf phase = phase_from_freq(f);
  f64 weights[kHarmonics];
  harmonics(p, weights);
  Buf body = harmonic_osc(phase, weights);

  const f64 am_d = p.am_depth * am_scale * p.quackiness;
  if (am_d > 0.01) {
    for (size_t i = 0; i < body.size() && i < t.size(); ++i) {
      const f64 am = 1.0 - am_d * (0.5 + 0.5 * std::sin(kTau * p.am_rate_hz * static_cast<f64>(t[i])));
      body[i] *= static_cast<f32>(am);
    }
  }

  const f64 breath = p.breath * breath_scale;
  if (breath > 0.0) {
    const Buf noise = pink_noise(t.size(), rng);
    for (size_t i = 0; i < body.size() && i < noise.size(); ++i) body[i] += static_cast<f32>(breath) * noise[i];
  }
  return body;
}

Buf alarm(const DuckPersonality& p, u32 variant) {
  Rng rng = variant_rng(p, "alarm", variant);
  const f64 dur = (0.20 + 0.12 * rng.random()) / p.speed;
  const Buf t = t_axis(dur);
  const f64 f0 = p.pitch_center_hz * (1.25 + 0.35 * p.pitch_spread) * (0.94 + 0.12 * rng.random());
  const f64 peak_mul = 1.15 + 0.25 * p.pitch_spread + 0.10 * rng.random();
  const f64 fall_mul = 0.75 + 0.20 * (1.0 - p.pitch_spread);
  const Buf freq = lerp(t, {{0.0, f0}, {0.05 * dur, f0 * peak_mul}, {dur, f0 * fall_mul}});
  const f64 decay = dur * (0.40 + 0.20 * rng.random());
  const Buf env = expdecay(t, attack(p, dur, 1.0), decay);
  Buf sig = voice(p, t, freq, rng, 0.5, 1.0);
  multiply(sig, env);
  const f32 crackle = static_cast<f32>(0.04 + 0.10 * p.brightness);
  for (size_t i = 0; i < sig.size() && i < env.size(); ++i) sig[i] += crackle * rng.standard_normal() * env[i];
  normalise(sig, -3.0);
  return sig;
}

Buf greet_syllable(const DuckPersonality& p, Rng& rng, f64 dur_scale, f64 f0_scale) {
  const f64 dur = (0.32 + 0.25 * rng.random()) * dur_scale / p.speed;
  const Buf t = t_axis(dur);
  const f64 f0 = p.pitch_center_hz * (0.9 + 0.15 * rng.random()) * f0_scale;
  const f64 bias = p.glide_bias;
  const f64 bend = 0.10 + 0.15 * p.pitch_spread;
  const f64 start = f0 * (1.0 - bias * bend * 0.5);
  const f64 mid = f0 * (1.0 + bias * bend);
  const f64 end = f0 * (1.0 - bias * bend * 0.3) * (0.92 + 0.08 * rng.random());
  const Buf freq = lerp(t, {{0.0, start}, {0.18 * dur, mid}, {dur, end}});
  const Buf env = expdecay(t, attack(p, dur, 0.5), dur * 0.7);
  Buf sig = voice(p, t, freq, rng, 1.0, 1.0);
  multiply(sig, env);
  return sig;
}

Buf greet(const DuckPersonality& p, u32 variant) {
  Rng rng = variant_rng(p, "greet", variant);
  Buf sig = greet_syllable(p, rng, 1.0, 1.0);
  if (rng.random() < 0.4) {
    const size_t gap_n = static_cast<size_t>((0.05 + 0.06 * rng.random()) / p.speed * static_cast<f64>(kSr));
    sig.insert(sig.end(), gap_n, 0.0f);
    const f64 f0_scale = 0.95 + 0.06 * rng.random();
    const Buf second = greet_syllable(p, rng, 0.8, f0_scale);
    sig.insert(sig.end(), second.begin(), second.end());
  }
  normalise(sig, -3.0);
  return sig;
}

Buf inquire(const DuckPersonality& p, u32 variant) {
  Rng rng = variant_rng(p, "inquire", variant);
  const f64 dur = (0.42 + 0.25 * rng.random()) / p.speed;
  const Buf t = t_axis(dur);
  const f64 f0 = p.pitch_center_hz * (0.88 + 0.10 * rng.random());
  const f64 rise = 1.15 + 0.50 * p.pitch_spread + 0.20 * std::max(p.glide_bias, 0.0) + 0.10 * rng.random();
  const Buf freq = lerp(t, {{0.0, f0 * 0.92}, {0.30 * dur, f0 * 0.95}, {dur, f0 * rise}});
  const Buf env = bell(t, dur * (0.06 + 0.10 * (1.0 - p.attack_sharpness)), dur * 0.32);
  Buf sig = voice(p, t, freq, rng, 0.6, 1.0);
  multiply(sig, env);
  normalise(sig, -3.0);
  return sig;
}

Buf peck(const DuckPersonality& p, u32 variant) {
  Rng rng = variant_rng(p, "peck", variant);
  const f64 dur = (0.16 + 0.12 * rng.random()) / p.speed;
  const Buf t = t_axis(dur);
  const f64 f0 = p.pitch_center_hz * (0.45 + 0.20 * rng.random());
  const Buf freq = lerp(t, {{0.0, f0 * 1.5}, {0.04 * dur, f0}, {dur, f0 * 0.80}});
  const Buf env = expdecay(t, attack(p, dur, 1.0), dur * 0.35);
  Buf body = voice(p, t, freq, rng, 0.3, 0.5);
  multiply(body, env);
  const size_t click_len = static_cast<size_t>((0.003 + 0.006 * p.attack_sharpness) * static_cast<f64>(kSr));
  const f32 click_gain = static_cast<f32>(0.4 + 0.4 * p.attack_sharpness);
  const Buf c = click(t.size(), rng, click_len);
  for (size_t i = 0; i < body.size() && i < c.size(); ++i) body[i] += click_gain * c[i];
  normalise(body, -3.0);
  return body;
}

Buf chirp_syllable(const DuckPersonality& p, Rng& rng, f64 f0, f64 dur, const Point (&contour)[3]) {
  const Buf t = t_axis(dur);
  const f64 warble_phase = rng.uniform(0.0, kTau);
  const Buf warble = vibrato(t, p.warble_hz, p.warble_depth, warble_phase);
  const Buf shape = lerp(t, contour);
  Buf freq(std::min(shape.size(), warble.size()));
  for (size_t i = 0; i < freq.size(); ++i) freq[i] = static_cast<f32>(f0) * shape[i] * warble[i];
  const Buf env = expdecay(t, attack(p, dur, 0.7), dur * 0.55);
  DuckPersonality p_soft = p;
  p_soft.quackiness *= 0.2;
  p_soft.brightness *= 0.5;
  p_soft.formant_gain *= 0.5;
  Buf sig = voice(p_soft, t, freq, rng, 1.0, 0.4);
  multiply(sig, env);
  return sig;
}

Buf chirp(const DuckPersonality& p, u32 variant) {
  Rng rng = variant_rng(p, "chirp", variant);
  const u32 shape = variant % 4;
  const f64 f0 = p.pitch_center_hz * (0.95 + 0.75 * rng.random());
  Buf sig;
  if (shape == 0) {  // rising blip
    const f64 dur = (0.10 + 0.10 * rng.random()) / p.speed;
    const f64 peak = 1.12 + 0.10 * rng.random();
    sig = chirp_syllable(p, rng, f0, dur, {{0.0, 0.88}, {0.5 * dur, peak}, {dur, 1.05}});
  } else if (shape == 1) {  // falling blip
    const f64 dur = (0.10 + 0.10 * rng.random()) / p.speed;
    const f64 start = 1.12 + 0.10 * rng.random();
    sig = chirp_syllable(p, rng, f0, dur, {{0.0, start}, {0.3 * dur, 1.0}, {dur, 0.78}});
  } else if (shape == 2) {  // trill: longer, warble cranked up
    const f64 dur = (0.22 + 0.14 * rng.random()) / p.speed;
    DuckPersonality p_trill = p;
    p_trill.warble_depth = std::max(p.warble_depth, 0.7) * 1.6;
    sig = chirp_syllable(p_trill, rng, f0, dur, {{0.0, 1.0}, {0.5 * dur, 1.06}, {dur, 0.94}});
  } else {  // double "wek-wek"
    const f64 dur = (0.08 + 0.05 * rng.random()) / p.speed;
    sig = chirp_syllable(p, rng, f0, dur, {{0.0, 0.95}, {0.4 * dur, 1.10}, {dur, 0.88}});
    const size_t gap_n = static_cast<size_t>((0.03 + 0.03 * rng.random()) / p.speed * static_cast<f64>(kSr));
    sig.insert(sig.end(), gap_n, 0.0f);
    const f64 dur_b = dur * 0.9;
    const f64 f0_b = f0 * (0.92 + 0.10 * rng.random());
    const Buf b = chirp_syllable(p, rng, f0_b, dur_b, {{0.0, 0.95}, {0.4 * dur_b, 1.10}, {dur_b, 0.88}});
    sig.insert(sig.end(), b.begin(), b.end());
  }
  normalise(sig, -6.0);
  return sig;
}

Buf coo(const DuckPersonality& p, u32 variant) {
  Rng rng = variant_rng(p, "coo", variant);
  const f64 dur = (0.85 + 0.55 * rng.random()) / p.speed;
  const Buf t = t_axis(dur);
  const f64 f0 = p.pitch_center_hz * (0.42 + 0.15 * (1.0 - p.attack_sharpness)) * (0.94 + 0.12 * rng.random());
  const f64 drift_a = 1.0 + 0.05 * rng.random() + 0.04 * p.glide_bias;
  const Buf freq = lerp(t, {{0.0, f0 * 0.94}, {dur * 0.5, f0 * drift_a}, {dur, f0 * 0.90}});
  const Buf env = bell(t, dur * (0.18 + 0.10 * (1.0 - p.attack_sharpness)), dur * 0.30);
  DuckPersonality p_soft = p;
  p_soft.breath = std::max(p.breath, 0.12) + 0.10;
  p_soft.quackiness = p.quackiness * 0.25;
  p_soft.am_rate_hz = p.am_rate_hz * 0.30;
  p_soft.vibrato_rate_hz = p.vibrato_rate_hz * 0.45;
  p_soft.vibrato_depth = p.vibrato_depth * 0.7;
  Buf sig = voice(p_soft, t, freq, rng, 1.0, 1.0);
  multiply(sig, env);
  normalise(sig, -5.0);
  return sig;
}

void wheee_segments(const DuckPersonality& p, u32 variant, Buf& start, Buf& loop, Buf& end) {
  Rng rng = variant_rng(p, "wheee", variant);
  const f64 d_start = (0.80 + 0.30 * rng.random()) / p.speed;
  const f64 d_loop = (1.60 + 0.60 * rng.random()) / p.speed;
  const f64 d_end = (0.55 + 0.25 * rng.random()) / p.speed;
  const f64 total = d_start + d_loop + d_end;
  const Buf t = t_axis(total);
  const f64 t1 = d_start, t2 = d_start + d_loop;

  const f64 f0 = p.pitch_center_hz * (0.95 + 0.10 * rng.random());
  const f64 top = 1.6 + 0.5 * p.pitch_spread + 0.25 * rng.random();
  const Buf base = lerp(t, {{0.0, f0 * 0.85},
                            {0.15 * d_start, f0},
                            {t1, f0 * top},
                            {t2, f0 * top},
                            {t2 + 0.25 * d_end, f0 * top * 1.04},
                            {total, f0 * 0.60}});
  const f64 wob_hz = 4.5 + 3.0 * rng.random();
  const Buf swell = lerp(t, {{0.0, 0.15}, {t1, 1.0}, {total, 1.0}});
  Buf freq(t.size());
  for (size_t i = 0; i < t.size(); ++i) {
    const f64 wob = std::sin(kTau * wob_hz * static_cast<f64>(t[i]));
    freq[i] = base[i] * static_cast<f32>(std::pow(2.0, 0.5 * static_cast<f64>(swell[i]) * wob / 12.0));
  }

  const Buf env = lerp(t, {{0.0, 0.0}, {std::min(0.06, 0.5 * d_start), 1.0}, {t2 + 0.40 * d_end, 1.0}, {total, 0.0}});
  DuckPersonality p_joy = p;
  p_joy.vibrato_depth = p.vibrato_depth * 0.5;
  p_joy.quackiness = p.quackiness * 0.5;
  Buf sig = voice(p_joy, t, freq, rng, 0.5, 0.5);
  multiply(sig, env);
  normalise(sig, -4.0);

  const size_t n = sig.size();
  const size_t n2 = std::min(static_cast<size_t>(t2 * static_cast<f64>(kSr)), n);  // Rust would panic past n
  const size_t n1 = std::min(static_cast<size_t>(t1 * static_cast<f64>(kSr)), n2);
  start.assign(sig.begin(), sig.begin() + static_cast<std::ptrdiff_t>(n1));
  loop.assign(sig.begin() + static_cast<std::ptrdiff_t>(n1), sig.begin() + static_cast<std::ptrdiff_t>(n2));
  end.assign(sig.begin() + static_cast<std::ptrdiff_t>(n2), sig.end());
  const size_t nx = std::min({static_cast<size_t>(0.08 * static_cast<f64>(kSr)), loop.size() / 2, n1});
  const size_t len = loop.size();
  for (size_t i = 0; i < nx; ++i) {
    const f32 w = static_cast<f32>(i) / std::max(static_cast<f32>(std::max<size_t>(nx, 1)) - 1.0f, 1.0f);
    const size_t tail = len - nx + i;
    loop[tail] = (1.0f - w) * loop[tail] + w * sig[n1 - nx + i];
  }
}

// ---------------------------------------------------------------------------
// lib.rs and the kern-side glue

bool render_native(std::string_view tag, const DuckPersonality& p, u32 variant, Buf& out) {
  if (tag == "alarm") out = alarm(p, variant);
  else if (tag == "greet") out = greet(p, variant);
  else if (tag == "inquire") out = inquire(p, variant);
  else if (tag == "peck") out = peck(p, variant);
  else if (tag == "chirp") out = chirp(p, variant);
  else if (tag == "coo") out = coo(p, variant);
  else if (tag == "wheee") {
    Buf start, loop, end;
    wheee_segments(p, variant, start, loop, end);
    out = std::move(start);
    out.insert(out.end(), loop.begin(), loop.end());
    out.insert(out.end(), loop.begin(), loop.end());
    out.insert(out.end(), end.begin(), end.end());
  } else {
    return false;
  }
  return true;
}

// Linear resample from 48 kHz to `rate` (identity at 48 kHz).
void resample(Buf& x, u32 rate) {
  if (rate == kSr || rate == 0 || x.empty()) return;
  const size_t n = x.size();
  const size_t m = std::max<size_t>(static_cast<size_t>((static_cast<u64>(n) * rate + kSr - 1) / kSr), 1);
  const f64 step = static_cast<f64>(kSr) / static_cast<f64>(rate);
  Buf y(m);
  for (size_t i = 0; i < m; ++i) {
    const f64 pos = static_cast<f64>(i) * step;
    const size_t k = static_cast<size_t>(pos);
    if (k + 1 >= n) {
      y[i] = x[n - 1];
      continue;
    }
    const f32 frac = static_cast<f32>(pos - static_cast<f64>(k));
    y[i] = x[k] + (x[k + 1] - x[k]) * frac;
  }
  x = std::move(y);
}

// SHA-256 (FIPS 180-4), just enough for seed_from_id.
void sha256(const u8* data, size_t len, u8 (&digest)[32]) {
  static constexpr u32 k[64] = {
      0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
      0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
      0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
      0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
      0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
      0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
      0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
      0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
  u32 h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  auto rotr = [](u32 x, int r) { return (x >> r) | (x << (32 - r)); };
  const u64 bits = static_cast<u64>(len) * 8;
  const size_t total = ((len + 9 + 63) / 64) * 64;
  u8 block[64];
  for (size_t off = 0; off < total; off += 64) {
    for (size_t i = 0; i < 64; ++i) {
      const size_t at = off + i;
      if (at < len) block[i] = data[at];
      else if (at == len) block[i] = 0x80;
      else if (at >= total - 8) block[i] = static_cast<u8>(bits >> (8 * (total - 1 - at)));
      else block[i] = 0;
    }
    u32 w[64];
    for (int i = 0; i < 16; ++i)
      w[i] = (u32{block[4 * i]} << 24) | (u32{block[4 * i + 1]} << 16) | (u32{block[4 * i + 2]} << 8) | u32{block[4 * i + 3]};
    for (int i = 16; i < 64; ++i) {
      const u32 s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
      const u32 s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    u32 a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; ++i) {
      const u32 t1 = hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
      const u32 t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
      hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
  }
  for (int i = 0; i < 8; ++i)
    for (int j = 0; j < 4; ++j) digest[4 * i + j] = static_cast<u8>(h[i] >> (24 - 8 * j));
}

}  // namespace

DuckPersonality duck_personality(u32 seed) {
  Rng rng(seed);
  DuckPersonality p;
  p.seed = seed;
  // Bimodal-ish register: some ducks smaller & higher, some big & low.
  static constexpr f64 kRegisters[4] = {-1.0, 0.0, 0.0, 1.0};
  const f64 reg = rng.choice(kRegisters, 4);
  p.register_shift = reg + rng.uniform(-0.4, 0.4);
  const f64 base = rng.uniform(160.0, 380.0);
  p.pitch_center_hz = std::clamp(base * std::pow(2.0, p.register_shift * 0.45), 110.0, 620.0);
  p.pitch_spread = rng.uniform(0.4, 1.2);
  p.glide_bias = rng.uniform(-1.0, 1.0);

  p.brightness = rng.uniform(0.05, 0.55);
  p.tilt = rng.uniform(1.4, 2.8);
  p.nasal = rng.uniform(0.1, 1.0);
  p.harmonic_skew = rng.uniform(-1.0, 1.0);
  p.formant_n = static_cast<u32>(rng.integers(1, 6));
  p.formant_gain = rng.uniform(0.0, 1.4);

  p.vibrato_rate_hz = rng.uniform(3.5, 9.5);
  p.vibrato_depth = rng.uniform(0.0, 0.7);
  p.jitter_depth = rng.uniform(0.03, 0.35);
  p.breath = rng.uniform(0.0, 0.30);
  p.quackiness = rng.uniform(0.2, 1.0);
  p.am_rate_hz = rng.uniform(18.0, 55.0);
  p.am_depth = rng.uniform(0.15, 0.70);
  p.warble_hz = rng.uniform(7.0, 18.0);
  p.warble_depth = rng.uniform(0.0, 1.4);

  p.attack_sharpness = rng.uniform(0.0, 1.0);
  p.speed = rng.uniform(0.82, 1.22);
  return p;
}

u32 duck_seed_from_id(std::string_view id) {
  u8 digest[32];
  sha256(reinterpret_cast<const u8*>(id.data()), id.size(), digest);
  return (u32{digest[0]} << 24) | (u32{digest[1]} << 16) | (u32{digest[2]} << 8) | u32{digest[3]};
}

u32 duck_variant_count(std::string_view tag) {
  if (tag == "greet" || tag == "chirp") return 12;
  if (tag == "wheee") return 6;
  return 10;
}

bool render_duck_voice(std::string_view tag, u32 seed, u32 variant, u32 rate, std::vector<f32>& out) {
  const DuckPersonality p = duck_personality(seed);
  if (!render_native(tag, p, variant, out)) {
    out.clear();
    return fail("duck voice: unknown tag '%.*s'", static_cast<int>(tag.size()), tag.data());
  }
  resample(out, rate);
  return true;
}

bool render_duck_wheee(u32 seed, u32 variant, u32 rate, std::vector<f32>& start, std::vector<f32>& loop,
                       std::vector<f32>& end) {
  wheee_segments(duck_personality(seed), variant, start, loop, end);
  resample(start, rate);
  resample(loop, rate);
  resample(end, rate);
  return true;
}

}  // namespace kern::audio
