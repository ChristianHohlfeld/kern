#include "kern/audio/synth.h"
#include "kern/audio/duck_voice.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <initializer_list>
#include <string>

namespace kern::audio {

namespace {

constexpr f64 kTau = 6.283185307179586;

struct Rng {
  u32 s;
  explicit Rng(u32 seed) : s(seed ? seed : 0x9e3779b9u) {}
  u32 next() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
  f32 bipolar() { return static_cast<f32>(next() >> 8) * (2.0f / 16777216.0f) - 1.0f; }
};

f32 midi_hz(f32 note) { return 440.0f * std::pow(2.0f, (note - 69.0f) / 12.0f); }

}  // namespace

std::vector<f32> synthesize(const SynthPatch& p, u32 rate) {
  const f64 sr = rate;
  const f64 length = std::max(0.0f, p.attack) + std::max(0.0f, p.sustain) + std::max(0.0f, p.release);
  const u32 n = static_cast<u32>(std::ceil(std::max(0.001, length) * sr));
  std::vector<f32> out(n, 0.0f);
  Rng rng(p.seed);
  f64 phase = 0, mod_phase = 0;
  f64 freq = std::max(1.0f, p.frequency);
  f64 slide = p.slide;
  f32 duty = p.duty;
  // Chamberlin state-variable low-pass (resonant) and a one-pole high-pass.
  f32 lp_low = 0, lp_band = 0;
  f32 hp_prev_in = 0, hp_prev_out = 0;
  f64 lp_cutoff = p.lowpass;
  f32 lp_fc = 0;
  // Pink noise (Paul Kellet's economy filter).
  f32 b0 = 0, b1 = 0, b2 = 0;
  f32 noise_value = 0;
  f64 noise_phase = 0;
  f32 crush_hold = 0;
  f64 crush_phase = 0;
  f64 repeat_clock = 0;
  bool arp_done = false;
  // Modulation runs at control rate (every 16 samples): exp2/sin per sample would
  // dominate the cost of long notes; 16 samples is far below audible stepping.
  f64 slide_mul = 1, vib_mul = 1;
  f32 trem = 1;
  for (u32 i = 0; i < n; ++i) {
    const f64 t = i / sr;
    if ((i & 15) == 0) {
      slide_mul = slide != 0 ? std::exp2(slide / sr) : 1.0;
      vib_mul = p.vibrato_depth > 0 ? std::exp2(p.vibrato_depth / 12.0 * std::sin(kTau * p.vibrato_rate * t)) : 1.0;
      trem = p.tremolo_depth > 0 ? 1.0f - p.tremolo_depth * 0.5f * (1.0f + static_cast<f32>(std::sin(kTau * p.tremolo_rate * t))) : 1.0f;
      if (p.lowpass > 0 && (p.lowpass_sweep != 0 || i == 0)) {
        lp_cutoff = p.lowpass * std::exp2(p.lowpass_sweep * t);
        lp_fc = static_cast<f32>(std::clamp(2.0 * std::sin(3.141592653589793 * std::min(lp_cutoff, sr * 0.2) / sr), 0.0, 1.0));
      }
    }
    // Repeat restarts the pitch envelope.
    if (p.repeat > 0 && (repeat_clock += 1 / sr) >= p.repeat) {
      repeat_clock = 0;
      freq = std::max(1.0f, p.frequency);
      slide = p.slide;
      arp_done = false;
    }
    if (p.arp_time > 0 && !arp_done && t >= p.arp_time) { freq *= p.arp_ratio; arp_done = true; }
    if (p.slide_accel != 0) slide += p.slide_accel / sr;
    freq *= slide_mul;
    if (freq < p.min_frequency) freq = p.min_frequency;
    f64 f = freq * vib_mul;
    f = std::min(f, sr * 0.45);
    duty = std::clamp(duty + p.duty_sweep / static_cast<f32>(sr), 0.02f, 0.98f);
    // Oscillator.
    f32 s = 0;
    switch (p.wave) {
      case Wave::square: s = phase < 0.5 ? 0.6f : -0.6f; break;
      case Wave::pulse: s = phase < duty ? 0.6f : -0.6f; break;
      case Wave::saw: s = static_cast<f32>(1.0 - 2.0 * phase) * 0.6f; break;
      case Wave::triangle: s = static_cast<f32>(phase < 0.5 ? 4.0 * phase - 1.0 : 3.0 - 4.0 * phase) * 0.8f; break;
      case Wave::sine: {
        f64 ph = phase;
        if (p.fm_ratio > 0) ph += p.fm_depth * std::sin(kTau * mod_phase) / kTau;
        s = static_cast<f32>(std::sin(kTau * ph));
        break;
      }
      case Wave::noise:
      case Wave::pink: {
        // Noise is resampled at the oscillator frequency (sfxr): lower notes sound darker.
        noise_phase += f / sr * 8;
        if (noise_phase >= 1 || i == 0) { noise_phase -= std::floor(noise_phase); noise_value = rng.bipolar(); }
        f32 w = p.frequency >= 10000 ? rng.bipolar() : noise_value;
        if (p.wave == Wave::pink) {
          b0 = 0.99765f * b0 + w * 0.0990460f;
          b1 = 0.96300f * b1 + w * 0.2965164f;
          b2 = 0.57000f * b2 + w * 1.0526913f;
          w = (b0 + b1 + b2 + w * 0.1848f) * 0.25f;
        }
        s = w * 0.8f;
        break;
      }
    }
    phase += f / sr;
    phase -= std::floor(phase);
    if (p.fm_ratio > 0) { mod_phase += f * p.fm_ratio / sr; mod_phase -= std::floor(mod_phase); }
    // Filters.
    if (p.lowpass > 0) {
      const f32 fc = lp_fc;
      const f32 q = 1.0f - std::clamp(p.resonance, 0.0f, 0.95f);
      lp_low += fc * lp_band;
      const f32 high = s - lp_low - q * lp_band;
      lp_band += fc * high;
      s = lp_low;
    }
    if (p.highpass > 0) {
      const f32 a = static_cast<f32>(1.0 / (1.0 + kTau * p.highpass / sr));
      const f32 y = a * (hp_prev_out + s - hp_prev_in);
      hp_prev_in = s;
      hp_prev_out = y;
      s = y;
    }
    if (p.bitcrush > 0) {
      crush_phase += 1.0 - std::clamp(p.bitcrush, 0.0f, 0.98f);
      if (crush_phase >= 1) { crush_phase -= 1; crush_hold = s; }
      s = crush_hold;
    }
    // Envelope.
    f32 env;
    if (t < p.attack) env = static_cast<f32>(t / std::max(1e-6f, p.attack));
    else if (t < p.attack + p.sustain) {
      const f32 k = static_cast<f32>((t - p.attack) / std::max(1e-6f, p.sustain));
      env = 1.0f + p.punch * (1.0f - k) * 2.0f;
    } else env = static_cast<f32>(1.0 - (t - p.attack - p.sustain) / std::max(1e-6f, p.release));
    env = std::max(0.0f, env);
    env *= trem;
    out[i] = s * env * p.volume;
  }
  return out;
}

void mix_into(std::vector<f32>& dst, const std::vector<f32>& src, f32 offset_seconds, u32 rate, f32 gain) {
  const std::size_t start = static_cast<std::size_t>(std::max(0.0f, offset_seconds) * rate);
  if (dst.size() < start + src.size()) dst.resize(start + src.size(), 0.0f);
  for (std::size_t i = 0; i < src.size(); ++i) dst[start + i] += src[i] * gain;
}

void make_seamless(std::vector<f32>& s, u32 rate, f32 crossfade) {
  const std::size_t x = std::min(s.size() / 2, static_cast<std::size_t>(crossfade * rate));
  if (x < 2) return;
  // The last x samples fade into the first x (equal power), then the tail is dropped.
  const std::size_t keep = s.size() - x;
  for (std::size_t i = 0; i < x; ++i) {
    const f32 t = static_cast<f32>(i) / static_cast<f32>(x);
    const f32 a = std::sin(t * 1.5707963f), b = std::cos(t * 1.5707963f);
    s[i] = s[i] * a + s[keep + i] * b;
  }
  s.resize(keep);
}

void normalize(std::vector<f32>& s, f32 peak) {
  f32 m = 0;
  for (f32 v : s) m = std::max(m, std::fabs(v));
  if (m < 1e-6f) return;
  const f32 k = peak / m;
  for (f32& v : s) v *= k;
}

// ---------------------------------------------------------------------------
// built-in library

namespace {

using Buffer = std::vector<f32>;

SynthPatch patch(Wave w, f32 freq, f32 attack, f32 sustain, f32 release, f32 volume = 0.5f) {
  SynthPatch p;
  p.wave = w;
  p.frequency = freq;
  p.attack = attack;
  p.sustain = sustain;
  p.release = release;
  p.volume = volume;
  return p;
}

/// Steady loop: renders warm-up + length and keeps the last `length` seconds.
/// Periodic patches (frequencies that complete whole cycles in `length`) loop exactly.
Buffer steady_loop(SynthPatch p, f32 length, u32 rate, f32 warm = 0.5f) {
  p.attack = 0;
  p.sustain = length + warm + 0.01f;
  p.release = 0;
  Buffer b = synthesize(p, rate);
  const std::size_t n = static_cast<std::size_t>(length * rate), skip = static_cast<std::size_t>(warm * rate);
  return Buffer(b.begin() + static_cast<std::ptrdiff_t>(skip), b.begin() + static_cast<std::ptrdiff_t>(skip + n));
}

void add(Buffer& dst, const Buffer& src, f32 gain = 1) {
  if (dst.size() < src.size()) dst.resize(src.size(), 0.0f);
  for (std::size_t i = 0; i < src.size(); ++i) dst[i] += src[i] * gain;
}

// ---- music ----

/// Memoised note renders: music repeats the same notes many times.
struct NoteCache {
  std::vector<std::pair<std::string, Buffer>> entries;
  template <typename F> const Buffer& get(const std::string& key, F&& make) {
    for (auto& e : entries) if (e.first == key) return e.second;
    entries.push_back({key, make()});
    return entries.back().second;
  }
};

struct Stereo {
  Buffer l, r;
  void put(const Buffer& note, f32 at, u32 rate, f32 gain, f32 pan) {
    const f32 a = (std::clamp(pan, -1.0f, 1.0f) + 1) * 0.25f * 3.14159265f;
    mix_into(l, note, at, rate, gain * std::cos(a) * 1.4142f);
    mix_into(r, note, at, rate, gain * std::sin(a) * 1.4142f);
  }
  /// Folds everything past `length` back onto the start: notes ringing over the loop point continue at its beginning.
  Buffer loop(f32 length, u32 rate) {
    const std::size_t n = static_cast<std::size_t>(length * rate);
    l.resize(std::max(l.size(), n), 0.0f);
    r.resize(std::max(r.size(), n), 0.0f);
    for (std::size_t i = n; i < l.size(); ++i) l[i % n] += l[i];
    for (std::size_t i = n; i < r.size(); ++i) r[i % n] += r[i];
    Buffer out(n * 2);
    for (std::size_t i = 0; i < n; ++i) { out[i * 2] = l[i]; out[i * 2 + 1] = r[i]; }
    return out;
  }
};

Buffer kick(u32 rate) {
  SynthPatch p = patch(Wave::sine, 150, 0.001f, 0.02f, 0.3f, 0.95f);
  p.slide = -7;
  p.min_frequency = 42;
  Buffer b = synthesize(p, rate);
  SynthPatch c = patch(Wave::noise, 12000, 0.0005f, 0.002f, 0.01f, 0.25f);
  c.seed = 11;
  add(b, synthesize(c, rate));
  return b;
}

Buffer snare(u32 rate) {
  SynthPatch n = patch(Wave::noise, 12000, 0.001f, 0.01f, 0.16f, 0.5f);
  n.highpass = 900;
  n.seed = 7;
  Buffer b = synthesize(n, rate);
  SynthPatch t = patch(Wave::triangle, 196, 0.001f, 0.01f, 0.09f, 0.45f);
  t.slide = -1;
  add(b, synthesize(t, rate));
  return b;
}

Buffer hat(u32 rate, bool open) {
  SynthPatch n = patch(Wave::noise, 12000, 0.0005f, 0.004f, open ? 0.22f : 0.045f, 0.22f);
  n.highpass = 7000;
  n.seed = open ? 31 : 29;
  return synthesize(n, rate);
}

Buffer bass(u32 rate, f32 note, f32 dur) {
  SynthPatch p = patch(Wave::saw, midi_hz(note), 0.004f, dur * 0.85f, 0.06f, 0.55f);
  p.lowpass = 520;
  p.resonance = 0.35f;
  Buffer b = synthesize(p, rate);
  SynthPatch sub = patch(Wave::sine, midi_hz(note - 12), 0.004f, dur * 0.85f, 0.06f, 0.4f);
  add(b, synthesize(sub, rate));
  return b;
}

Buffer pad(u32 rate, f32 note, f32 dur, f32 detune_cents) {
  SynthPatch p = patch(Wave::saw, midi_hz(note + detune_cents / 100.0f), 0.45f, dur, 1.2f, 0.16f);
  p.lowpass = 1300;
  p.resonance = 0.15f;
  p.vibrato_depth = 0.05f;
  p.vibrato_rate = 4.7f;
  return synthesize(p, rate);
}

Buffer pluck(u32 rate, f32 note, Wave w = Wave::pulse) {
  SynthPatch p = patch(w, midi_hz(note), 0.002f, 0.02f, 0.32f, 0.35f);
  p.duty = 0.28f;
  p.lowpass = 4200;
  p.lowpass_sweep = -2.5f;
  return synthesize(p, rate);
}

Buffer bell(u32 rate, f32 note, f32 release) {
  SynthPatch p = patch(Wave::sine, midi_hz(note), 0.004f, 0.02f, release, 0.42f);
  p.fm_ratio = 3.5f;
  p.fm_depth = 1.2f;
  return synthesize(p, rate);
}

// A small rigid object struck: a few exponentially decaying partials (modal synthesis) and a short
// noise transient for the contact itself, scaled to `peak`. Each mode: frequency (Hz), decay time
// constant (s), amplitude. `click_lowpass` shapes the transient (soft materials sound duller).
struct Partial { f32 hz, tau, amp; };
Buffer struck(u32 rate, std::initializer_list<Partial> modes, f32 click, f32 click_ms, f32 click_lowpass, f32 seconds, f32 peak, u32 seed) {
  const std::size_t n = static_cast<std::size_t>(seconds * rate);
  Buffer out(n, 0.0f);
  const f64 two_pi = 6.283185307179586;
  for (const Partial& m : modes) {
    const f64 w = two_pi * m.hz / rate, k = std::exp(-1.0 / (m.tau * rate));
    f64 env = m.amp;
    for (std::size_t i = 0; i < n && env > 1e-5; ++i, env *= k) out[i] += static_cast<f32>(env * std::sin(w * static_cast<f64>(i)));
  }
  // The contact: white noise under a fast decay, one-pole low-passed.
  u32 x = seed * 2654435761u + 1;
  const f64 kc = std::exp(-1.0 / (click_ms * 0.001 * rate)), lp = 1 - std::exp(-two_pi * click_lowpass / rate);
  f64 env = click, y = 0;
  for (std::size_t i = 0; i < n && env > 1e-5; ++i, env *= kc) {
    x = x * 1664525u + 1013904223u;
    const f64 white = static_cast<f64>(x >> 8) / 8388608.0 - 1.0;
    y += lp * (white - y);
    out[i] += static_cast<f32>(env * y);
  }
  // A soft onset (2 ms): nothing in a touch of plastic snaps on in one sample.
  const std::size_t ramp = std::min<std::size_t>(n, rate / 500);
  for (std::size_t i = 0; i < ramp; ++i) out[i] *= static_cast<f32>(i) / static_cast<f32>(ramp);
  f32 top = 0;
  for (f32 v : out) top = std::max(top, std::abs(v));
  if (top > 0) for (f32& v : out) v *= peak / top;
  return out;
}

// Songs are scores: each bar enqueues its notes, BuiltinJob renders the queue a
// few notes at a time (time budget per step) and memoises repeated notes.
struct Score {
  struct Event {
    std::string key;
    std::function<Buffer()> make;
    f32 at, gain, pan;
  };
  std::vector<Event> events;
  void put(std::string key, std::function<Buffer()> make, f32 at, f32 gain, f32 pan) {
    events.push_back({std::move(key), std::move(make), at, gain, pan});
  }
};

struct Song {
  f32 bar_seconds;
  int bars;
  f32 peak;
  void (*bar)(int b, f32 t0, f32 beat, u32 rate, Score& s);
};

std::string note_key(const char* kind, f32 a, f32 b = 0) { return std::string(kind) + std::to_string(a) + "/" + std::to_string(b); }

void drive_bar(int b, f32 t0, f32 beat, u32 rate, Score& s) {
  // 120 BPM: Am - F - C - G, four on the floor, 8th bass, 16th arpeggio.
  static const i32 roots[4] = {57, 53, 48, 55};
  static const i32 chords[4][3] = {{57, 60, 64}, {53, 57, 60}, {48, 52, 55}, {55, 59, 62}};
  const int c = b % 4;
  const f32 bar = 4 * beat;
  for (int q = 0; q < 4; ++q) s.put("kick", [rate] { return kick(rate); }, t0 + q * beat, 0.9f, 0);
  s.put("snare", [rate] { return snare(rate); }, t0 + beat, 0.55f, 0.05f);
  s.put("snare", [rate] { return snare(rate); }, t0 + 3 * beat, 0.55f, 0.05f);
  for (int e = 0; e < 8; ++e)
    s.put(e % 2 ? "hato" : "hatc", [rate, e] { return hat(rate, e % 2 != 0); }, t0 + e * beat * 0.5f, e % 2 ? 0.28f : 0.22f, e % 2 ? 0.3f : -0.3f);
  for (int e = 0; e < 8; ++e) {
    const f32 note = static_cast<f32>(roots[c] - 12 + (e == 7 ? 7 : 0));
    s.put(note_key("b", note), [rate, note, beat] { return bass(rate, note, beat * 0.45f); }, t0 + e * beat * 0.5f, 0.75f, 0);
  }
  for (int v = 0; v < 3; ++v) {
    const f32 note = static_cast<f32>(chords[c][v]);
    s.put(note_key("p", note, -7), [rate, note, bar] { return pad(rate, note, bar - 0.2f, -7); }, t0, 0.55f, -0.6f);
    s.put(note_key("p", note, 7), [rate, note, bar] { return pad(rate, note, bar - 0.2f, 7); }, t0, 0.55f, 0.6f);
  }
  for (int x = 0; x < 16; ++x) {
    const f32 note = static_cast<f32>(chords[c][x % 3] + 12 * (1 + (x / 6) % 2));
    s.put(note_key("k", note), [rate, note] { return pluck(rate, note); }, t0 + x * beat * 0.25f, 0.32f, x % 2 ? 0.45f : -0.45f);
  }
}

void calm_bar(int b, f32 t0, f32 beat, u32 rate, Score& s) {
  // 80 BPM: Fmaj7 - Cmaj7 - Am7 - G6, pads, bass and a pentatonic bell melody.
  static const i32 chords[4][4] = {{53, 57, 60, 64}, {48, 52, 55, 59}, {57, 60, 64, 67}, {55, 59, 62, 64}};
  static const i32 melody[32] = {76, 0, 74, 72, 0, 69, 72, 0, 74, 0, 72, 67, 0, 69, 0, 0,
                                 72, 0, 74, 76, 0, 79, 76, 0, 74, 0, 72, 69, 0, 67, 69, 0};
  const int c = b % 4;
  const f32 bar = 4 * beat;
  for (int v = 0; v < 4; ++v) {
    const f32 note = static_cast<f32>(chords[c][v]);
    s.put(note_key("p", note, -5), [rate, note, bar] { return pad(rate, note, bar - 0.3f, -5); }, t0, 0.5f, -0.7f);
    s.put(note_key("p", note, 5), [rate, note, bar] { return pad(rate, note, bar - 0.3f, 5); }, t0, 0.5f, 0.7f);
  }
  const f32 root = static_cast<f32>(chords[c][0] - 12);
  s.put(note_key("b", root), [rate, root, bar] { return bass(rate, root, bar * 0.9f); }, t0, 0.4f, 0);
  for (int q = 0; q < 4; ++q) {
    const f32 m = static_cast<f32>(melody[(b * 4 + q) % 32]);
    if (m > 0) s.put(note_key("m", m), [rate, m] { return bell(rate, m, 1.6f); }, t0 + q * beat, 0.38f, q % 2 ? 0.35f : -0.35f);
  }
}

const Song* find_song(std::string_view name) {
  static const Song drive{2.0f, 8, 0.8f, drive_bar};
  static const Song calm{3.0f, 8, 0.7f, calm_bar};
  if (name == "music_drive") return &drive;
  if (name == "music_calm") return &calm;
  return nullptr;
}

}  // namespace

const std::vector<BuiltinSound>& builtin_sounds() {
  static const std::vector<BuiltinSound> list = {
      {"pickup", "Pickup", "Gameplay", false},
      {"powerup", "Power-up", "Gameplay", false},
      {"jump", "Jump", "Gameplay", false},
      {"land", "Land", "Gameplay", false},
      {"goal", "Goal fanfare", "Gameplay", false},
      {"hazard", "Hazard", "Gameplay", false},
      {"checkpoint", "Checkpoint", "Gameplay", false},
      {"lose", "Game over", "Gameplay", false},
      {"laser", "Laser", "Gameplay", false},
      {"gunshot", "Gunshot", "Combat", false},
      {"smg", "SMG shot", "Combat", false},
      {"pistol", "Pistol shot", "Combat", false},
      {"sniper", "Sniper shot", "Combat", false},
      {"launcher", "Launcher shot", "Combat", false},
      {"reload", "Reload", "Combat", false},
      {"hit", "Hit marker", "Combat", false},
      {"down", "Down", "Combat", false},
      {"explosion", "Explosion", "Physics", false},
      {"impact", "Impact", "Physics", false},
      {"impact_light", "Light impact", "Physics", false},
      {"break", "Joint break", "Physics", false},
      {"step", "Footstep", "Physics", false},
      {"plastic", "Plastic tap", "Physics", false},
      {"plastic_step", "Small footstep", "Physics", false},
      {"engine", "Engine", "Vehicle", true},
      {"hover", "Hover drive", "Vehicle", true},
      {"boost", "Boost", "Vehicle", false},
      {"wind", "Wind", "Ambience", true},
      {"city", "City", "Ambience", true},
      {"hum", "Electric hum", "Ambience", true},
      {"servo", "Servo", "Robot", true},
      {"quack", "Quack", "Robot", false},
      {"beep", "Robot beep", "Robot", false},
      {"click", "Click", "UI", false},
      {"confirm", "Confirm", "UI", false},
      {"error", "Error", "UI", false},
      {"music_drive", "Drive (music)", "Music", true},
      {"music_calm", "Calm (music)", "Music", true},
  };
  return list;
}

namespace {

// The Micro Duck's voice bank, rendered on demand: "duck_<tag>_<seed>_<variant>", the wheee ride as
// "duck_wheee-start|loop|end_<seed>_<variant>" (kern/audio/duck_voice.h).
struct DuckRef { std::string tag, segment; u32 seed = 0, variant = 0; };
bool parse_duck(std::string_view name, DuckRef& out) {
  if (name.rfind("duck_", 0) != 0) return false;
  name.remove_prefix(5);
  const std::size_t a = name.find('_'), b = name.rfind('_');
  if (a == std::string_view::npos || b == a) return false;
  std::string tag(name.substr(0, a));
  if (const std::size_t dash = tag.find('-'); dash != std::string::npos) { out.segment = tag.substr(dash + 1); tag.resize(dash); }
  auto number = [](std::string_view s, u32& v) {
    if (s.empty() || s.size() > 10) return false;
    u64 x = 0;
    for (char c : s) { if (c < '0' || c > '9') return false; x = x * 10 + static_cast<u64>(c - '0'); }
    if (x > 0xffffffffull) return false;
    v = static_cast<u32>(x);
    return true;
  };
  if (!number(name.substr(a + 1, b - a - 1), out.seed) || !number(name.substr(b + 1), out.variant)) return false;
  out.tag = tag;
  static constexpr const char* kTags[] = {"alarm", "greet", "inquire", "peck", "chirp", "coo", "wheee"};
  if (std::find(std::begin(kTags), std::end(kTags), tag) == std::end(kTags)) return false;
  if (!out.segment.empty() && (tag != "wheee" || (out.segment != "start" && out.segment != "loop" && out.segment != "end"))) return false;
  return true;
}

}  // namespace

const BuiltinSound* find_builtin(std::string_view name) {
  if (name.rfind("builtin:", 0) == 0) name.remove_prefix(8);
  for (const BuiltinSound& b : builtin_sounds())
    if (name == b.name) return &b;
  DuckRef d;
  if (parse_duck(name, d)) {
    static const BuiltinSound once{"duck", "Micro Duck voice", "Robot", false}, loop{"duck", "Micro Duck voice", "Robot", true};
    return d.segment == "loop" ? &loop : &once;
  }
  return nullptr;
}


// ---------------------------------------------------------------------------
// SpawnCity's combat bank (sim/combat_audio_bank.mjs): a 9 mm SMG round and a
// hard handgun report built from what recordings contain (N-wave crack, blast
// noise, pressure thump, mechanism clacks, slap-back reflections, street tail).

namespace {

constexpr f32 kTwoPi = 6.28318530718f;

struct XorShift {
  u32 s;
  f32 next() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return static_cast<f32>(s) / 4294967296.0f; }
};

f32 n_wave(f32 t, f32 rise, f32 fall) {
  if (t < rise) return t / rise;
  if (t < rise + fall) return 1 - 1.6f * (t - rise) / fall;
  if (t < rise + fall * 1.9f) return -.6f * (1 - (t - rise - fall) / (fall * .9f));
  return 0;
}

void finish_bank(std::vector<f32>& d, f32 target) {
  f32 peak = 1e-6f;
  for (f32 v : d) peak = std::max(peak, std::fabs(v));
  const f32 g = target / peak;
  for (f32& v : d) v = std::tanh(v * g * 1.35f);
}

std::vector<f32> render_smg(u32 rate, int variant) {
  const f32 duration = .2f + variant * .01f;
  const std::size_t len = static_cast<std::size_t>(std::ceil(duration * rate));
  std::vector<f32> data(len, 0.0f), dry(len, 0.0f);
  XorShift rnd{0x5a1c0du + static_cast<u32>(variant) * 4049u};
  f32 lp_a = 0, lp_b = 0, phase = 0;
  const f32 rise = .00024f, fall = .0009f + variant * .00005f;
  for (std::size_t n = 0; n < len; ++n) {
    const f32 t = static_cast<f32>(n) / rate, white = rnd.next() * 2 - 1;
    lp_a += .6f * (white - lp_a);
    lp_b += .25f * (lp_a - lp_b);
    const f32 blast = ((lp_a - lp_b) * .9f + (white - lp_a) * .5f) * std::exp(-t / .008f);
    phase += kTwoPi * (185 * std::exp(-t * 30) + 70) / rate;
    const f32 thump = std::sin(phase) * std::exp(-t / .02f) * (1 - std::exp(-t / .0008f));
    const f32 u = t - .017f - variant * .001f;
    const f32 bolt = u > 0 && u < .012f ? (std::sin(kTwoPi * 2700 * u) + .5f * (rnd.next() * 2 - 1)) * std::exp(-u / .003f) * .25f : 0;
    dry[n] = n_wave(t, rise, fall) * 1.2f + blast * .9f + thump * .7f + bolt;
  }
  const f32 taps[3][2] = {{.011f, .32f}, {.019f, .22f}, {.031f, .14f}};
  for (const auto& tap : taps) {
    const std::size_t k = static_cast<std::size_t>((tap[0] + variant * .0006f) * rate);
    for (std::size_t n = len; n-- > k;) data[n] += dry[n - k] * tap[1];
  }
  f32 lp = 0;
  for (std::size_t n = 0; n < len; ++n) {
    lp += .5f * (data[n] - lp);
    data[n] = std::tanh((dry[n] + lp * .8f) * 2.1f);
  }
  finish_bank(data, .95f);
  return data;
}

std::vector<f32> render_pistol(u32 rate, int variant) {
  const f32 duration = .58f + variant * .012f;
  const std::size_t len = static_cast<std::size_t>(std::ceil(duration * rate));
  std::vector<f32> data(len, 0.0f), dry(len, 0.0f);
  XorShift rnd{0x61c0a7u + static_cast<u32>(variant) * 7919u};
  const f32 rise = .00028f + variant * .00002f, fall = .00105f + variant * .00006f;
  f32 lp_a = 0, lp_b = 0, tail = 0, phase = 0, ring2 = 0;
  const f32 mech[3][4] = {{.0215f + variant * .0011f, 2450, 3810, 1}, {.0452f + variant * .0017f, 2080, 3260, .8f}, {.0705f + variant * .0013f, 2900, 4400, .35f}};
  for (std::size_t n = 0; n < len; ++n) {
    const f32 t = static_cast<f32>(n) / rate, white = rnd.next() * 2 - 1;
    lp_a += .55f * (white - lp_a);
    const f32 hp = white - lp_a;
    lp_b += .22f * (lp_a - lp_b);
    const f32 blast = ((lp_a - lp_b) * .9f + hp * .55f) * std::exp(-t / .0105f);
    phase += kTwoPi * (185 * std::exp(-t * 22) + 50) / rate;
    const f32 thump = std::sin(phase) * std::exp(-t / .042f) * (1 - std::exp(-t / .0009f));
    const f32 boom = std::sin(kTwoPi * 44 * t) * std::exp(-t / .11f) * (1 - std::exp(-t / .004f));
    const f32 snap = t < .0018f ? (rnd.next() * 2 - 1) * (1 - t / .0018f) : 0;
    f32 m = 0;
    for (const auto& c : mech) {
      const f32 u = t - c[0];
      if (u >= 0 && u < .02f) m += c[3] * (std::sin(kTwoPi * c[1] * u) + .6f * std::sin(kTwoPi * c[2] * u) + .4f * (rnd.next() * 2 - 1)) * std::exp(-u / .0042f);
    }
    dry[n] = n_wave(t, rise, fall) * 1.3f + snap * .7f + blast + thump * 1.15f + boom * .38f + m * .36f;
  }
  const f32 taps[5][2] = {{.0142f, .42f}, {.0235f, .30f}, {.0371f, .24f}, {.0516f, .17f}, {.0784f, .11f}};
  for (std::size_t n = 0; n < len; ++n) {
    const f32 t = static_cast<f32>(n) / rate;
    f32 v = dry[n];
    for (const auto& tap : taps) {
      const std::size_t d = static_cast<std::size_t>((tap[0] + variant * .0007f) * rate);
      if (n >= d) v += tap[1] * dry[n - d] * .8f;
    }
    ring2 += .08f * ((rnd.next() * 2 - 1) - ring2);
    tail += .15f * (ring2 - tail);
    data[n] = v + (t > .012f ? tail * .42f * std::exp(-(t - .012f) / .16f) : 0);
  }
  f32 lp = 0;
  for (std::size_t n = 0; n < len; ++n) {
    lp += .45f * (data[n] - lp);
    data[n] = dry[n] + (lp - dry[n] * .45f);
  }
  for (f32& v : data) v = std::tanh(v * 2.6f);
  finish_bank(data, .99f);
  return data;
}

/// Plays `in` at `speed` (playbackRate: < 1 is lower and longer).
std::vector<f32> at_speed(const std::vector<f32>& in, f32 speed) {
  const std::size_t len = static_cast<std::size_t>(in.size() / speed);
  std::vector<f32> out(len);
  for (std::size_t i = 0; i < len; ++i) {
    const f32 x = i * speed;
    const std::size_t k = static_cast<std::size_t>(x);
    const f32 f = x - k;
    out[i] = k + 1 < in.size() ? in[k] * (1 - f) + in[k + 1] * f : (k < in.size() ? in[k] : 0);
  }
  return out;
}

}  // namespace

bool render_builtin(std::string_view name, u32 rate, std::vector<f32>& out, u32& channels) {
  if (name.rfind("builtin:", 0) == 0) name.remove_prefix(8);
  channels = 1;
  out.clear();
  if (DuckRef d; parse_duck(name, d)) {
    // As the robot plays it: rendered at the bank's 48 kHz and stored as 16-bit PCM (sounds
    // to_wav: x 32767, clamped, truncated), then brought to the mixer's rate.
    constexpr u32 kBankRate = 48000;
    std::vector<f32> bank;
    if (d.segment.empty()) {
      if (!render_duck_voice(d.tag, d.seed, d.variant, kBankRate, bank)) return false;
    } else {
      std::vector<f32> start, loop, end;
      render_duck_wheee(d.seed, d.variant, kBankRate, start, loop, end);
      bank = d.segment == "start" ? start : d.segment == "loop" ? loop : end;
    }
    for (f32& v : bank) v = static_cast<f32>(static_cast<i16>(std::clamp(v * 32767.0f, -32768.0f, 32767.0f))) / 32768.0f;
    if (rate == kBankRate) {
      out = std::move(bank);
    } else {  // cubic (Catmull-Rom) resampling
      const f64 step = static_cast<f64>(kBankRate) / rate;
      const std::size_t n = static_cast<std::size_t>(bank.size() / step);
      out.resize(n);
      auto at = [&](i64 i) { return i < 0 || i >= static_cast<i64>(bank.size()) ? 0.0f : bank[static_cast<std::size_t>(i)]; };
      for (std::size_t k = 0; k < n; ++k) {
        const f64 x = k * step;
        const i64 i = static_cast<i64>(x);
        const f32 t = static_cast<f32>(x - i), p0 = at(i - 1), p1 = at(i), p2 = at(i + 1), p3 = at(i + 2);
        out[k] = p1 + 0.5f * t * (p2 - p0 + t * (2 * p0 - 5 * p1 + 4 * p2 - p3 + t * (3 * (p1 - p2) + p3 - p0)));
      }
    }
    // Heard through the robot's small speaker in its head: it rolls off below about 300 Hz
    // (robotd SPEAKER_ROLLOFF_HZ) - a second-order high-pass there, as the cone does - and a driver
    // of that size behind the head's plastic gives up above a few kHz (a second-order low-pass at
    // 6.5 kHz): the synth's buzz is rounded, not sharp.
    auto biquad = [&](bool high, f64 hz) {
      const f64 w = 2 * 3.14159265358979 * hz / rate, q = 0.7071;
      const f64 alpha = std::sin(w) / (2 * q), cw = std::cos(w), a0 = 1 + alpha;
      const f64 b0 = (high ? (1 + cw) : (1 - cw)) / 2 / a0, b1 = (high ? -(1 + cw) : (1 - cw)) / a0, b2 = b0, a1 = -2 * cw / a0, a2 = (1 - alpha) / a0;
      f64 x1 = 0, x2 = 0, y1 = 0, y2 = 0;
      for (f32& v : out) {
        const f64 x = v, y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1; x1 = x; y2 = y1; y1 = y;
        v = static_cast<f32>(y);
      }
    };
    biquad(true, 300.0);
    if (rate > 14000) biquad(false, 6500.0);
    return !out.empty();
  }
  if (name == "pickup") {
    SynthPatch a = patch(Wave::pulse, 988, 0.002f, 0.05f, 0.28f, 0.42f);
    a.duty = 0.35f;
    a.arp_ratio = 1.5f;
    a.arp_time = 0.065f;
    out = synthesize(a, rate);
    add(out, bell(rate, 100, 0.35f), 0.35f);
  } else if (name == "powerup") {
    SynthPatch a = patch(Wave::square, 330, 0.002f, 0.32f, 0.3f, 0.35f);
    a.slide = 2.2f;
    a.repeat = 0.12f;
    a.vibrato_depth = 0.3f;
    a.vibrato_rate = 14;
    out = synthesize(a, rate);
  } else if (name == "jump") {
    SynthPatch a = patch(Wave::pulse, 230, 0.002f, 0.05f, 0.2f, 0.4f);
    a.duty = 0.3f;
    a.slide = 2.4f;
    a.highpass = 120;
    out = synthesize(a, rate);
  } else if (name == "land") {
    SynthPatch n = patch(Wave::noise, 3000, 0.001f, 0.01f, 0.09f, 0.5f);
    n.lowpass = 900;
    out = synthesize(n, rate);
    SynthPatch t = patch(Wave::sine, 110, 0.001f, 0.01f, 0.12f, 0.6f);
    t.slide = -2;
    add(out, synthesize(t, rate));
  } else if (name == "goal") {
    static const f32 notes[5] = {72, 76, 79, 84, 88};
    for (int i = 0; i < 4; ++i) mix_into(out, pluck(rate, notes[i], Wave::square), i * 0.11f, rate, 0.6f);
    for (int i = 0; i < 4; ++i) mix_into(out, bell(rate, notes[i + 1] - 12, 1.4f), 0.44f, rate, 0.35f);
    SynthPatch shimmer = patch(Wave::triangle, midi_hz(96), 0.01f, 0.4f, 0.8f, 0.12f);
    shimmer.vibrato_depth = 0.4f;
    shimmer.vibrato_rate = 9;
    mix_into(out, synthesize(shimmer, rate), 0.44f, rate);
  } else if (name == "hazard") {
    SynthPatch a = patch(Wave::saw, 180, 0.002f, 0.18f, 0.35f, 0.45f);
    a.slide = -1.2f;
    a.tremolo_depth = 0.6f;
    a.tremolo_rate = 22;
    a.lowpass = 2600;
    out = synthesize(a, rate);
  } else if (name == "checkpoint") {
    out = bell(rate, 88, 0.9f);
    mix_into(out, bell(rate, 95, 0.8f), 0.07f, rate, 0.7f);
  } else if (name == "lose") {
    static const f32 notes[4] = {67, 63, 60, 55};
    for (int i = 0; i < 4; ++i) {
      SynthPatch p = patch(Wave::triangle, midi_hz(notes[i]), 0.005f, 0.14f, i == 3 ? 0.8f : 0.1f, 0.45f);
      p.vibrato_depth = i == 3 ? 0.5f : 0.0f;
      p.vibrato_rate = 5;
      mix_into(out, synthesize(p, rate), i * 0.2f, rate);
    }
  } else if (name == "laser") {
    SynthPatch a = patch(Wave::saw, 1500, 0.001f, 0.03f, 0.18f, 0.35f);
    a.slide = -4.5f;
    a.lowpass = 5000;
    a.resonance = 0.4f;
    out = synthesize(a, rate);
  } else if (name == "smg") {
    out = render_smg(rate, 0);
    for (f32& v : out) v *= .55f;  // SpawnCity plays it at a lower gain than the pistol
  } else if (name == "pistol") {
    out = render_pistol(rate, 0);
  } else if (name == "sniper") {
    // The pistol report at .52 plus a .5 layer under it (gain .55): a deep, long boom.
    const std::vector<f32> p = render_pistol(rate, 1);
    out = at_speed(p, .52f);
    const std::vector<f32> low = at_speed(p, .5f);
    if (low.size() > out.size()) out.resize(low.size(), 0.0f);
    for (std::size_t i = 0; i < low.size(); ++i) out[i] = std::tanh(out[i] + low[i] * .55f);
  } else if (name == "launcher") {
    out = at_speed(render_pistol(rate, 2), .62f);
  } else if (name == "gunshot") {
    // Crack (bright noise, fast decay) + body (low thump) + room tail.
    SynthPatch crack = patch(Wave::noise, 12000, 0.0003f, 0.004f, 0.07f, 0.95f);
    crack.highpass = 900;
    crack.punch = 0.6f;
    crack.seed = 29;
    out = synthesize(crack, rate);
    SynthPatch body = patch(Wave::sine, 95, 0.0005f, 0.01f, 0.13f, 0.85f);
    body.slide = -1.8f;
    add(out, synthesize(body, rate));
    SynthPatch tail = patch(Wave::pink, 12000, 0.004f, 0.05f, 0.45f, 0.32f);
    tail.lowpass = 1600;
    tail.lowpass_sweep = -1.6f;
    tail.seed = 31;
    add(out, synthesize(tail, rate));
  } else if (name == "reload") {
    // Magazine out, magazine in, bolt.
    SynthPatch c = patch(Wave::noise, 7000, 0.0004f, 0.003f, 0.035f, 0.55f);
    c.highpass = 1800;
    c.seed = 51;
    const Buffer click = synthesize(c, rate);
    SynthPatch m = patch(Wave::square, 420, 0.0005f, 0.005f, 0.05f, 0.18f);
    m.slide = -2;
    m.lowpass = 2400;
    const Buffer metal = synthesize(m, rate);
    mix_into(out, click, 0.0f, rate, 0.8f);
    mix_into(out, metal, 0.0f, rate, 0.6f);
    mix_into(out, click, 0.55f, rate, 1.0f);
    mix_into(out, metal, 0.56f, rate, 0.8f);
    SynthPatch slide = patch(Wave::noise, 5000, 0.01f, 0.05f, 0.08f, 0.35f);
    slide.lowpass = 3000;
    slide.lowpass_sweep = 1.5f;
    slide.seed = 53;
    mix_into(out, synthesize(slide, rate), 0.95f, rate, 0.8f);
    mix_into(out, click, 1.08f, rate, 1.0f);
    mix_into(out, metal, 1.09f, rate, 1.0f);
  } else if (name == "hit") {
    SynthPatch a = patch(Wave::square, 1760, 0.0005f, 0.004f, 0.045f, 0.3f);
    a.lowpass = 6000;
    out = synthesize(a, rate);
    SynthPatch t = patch(Wave::noise, 6000, 0.0004f, 0.003f, 0.02f, 0.35f);
    t.highpass = 2500;
    t.seed = 61;
    add(out, synthesize(t, rate));
  } else if (name == "down") {
    SynthPatch t = patch(Wave::sine, 160, 0.001f, 0.02f, 0.35f, 0.7f);
    t.slide = -1.4f;
    out = synthesize(t, rate);
    SynthPatch n = patch(Wave::noise, 3000, 0.002f, 0.02f, 0.22f, 0.5f);
    n.lowpass = 700;
    n.seed = 67;
    add(out, synthesize(n, rate));
    SynthPatch b = patch(Wave::triangle, midi_hz(64), 0.005f, 0.1f, 0.2f, 0.25f);
    b.slide = -0.5f;
    mix_into(out, synthesize(b, rate), 0.05f, rate);
  } else if (name == "explosion") {
    SynthPatch n = patch(Wave::pink, 12000, 0.002f, 0.12f, 1.4f, 0.9f);
    n.lowpass = 1800;
    n.lowpass_sweep = -1.4f;
    n.punch = 0.4f;
    n.seed = 3;
    out = synthesize(n, rate);
    SynthPatch b = patch(Wave::sine, 62, 0.002f, 0.05f, 0.9f, 0.8f);
    b.slide = -0.7f;
    add(out, synthesize(b, rate));
  } else if (name == "impact" || name == "impact_light") {
    const bool light = name == "impact_light";
    // A body landing on something: a thud, not a crash - the noise is kept below 2 kHz.
    SynthPatch n = patch(Wave::noise, 5000, 0.002f, 0.005f, light ? 0.07f : 0.16f, light ? 0.4f : 0.65f);
    n.lowpass = light ? 1900 : 1500;
    n.lowpass_sweep = -3;
    n.seed = light ? 17 : 13;
    out = synthesize(n, rate);
    SynthPatch t = patch(Wave::sine, light ? 220 : 85, 0.001f, 0.01f, light ? 0.08f : 0.22f, light ? 0.35f : 0.75f);
    t.slide = -1.5f;
    add(out, synthesize(t, rate));
  } else if (name == "break") {
    SynthPatch n = patch(Wave::noise, 9000, 0.0005f, 0.01f, 0.09f, 0.6f);
    n.highpass = 1400;
    n.seed = 23;
    out = synthesize(n, rate);
    SynthPatch s = patch(Wave::square, 1100, 0.0005f, 0.01f, 0.08f, 0.3f);
    s.slide = -6;
    add(out, synthesize(s, rate));
  } else if (name == "plastic") {
    // A printed plastic shell or servo housing touching the floor or another part: a short, solid
    // "tock". A solid part rings neither low nor long - a low partial with a long decay is what
    // reads as a hollow box - so the modes sit mid-range and die within a few milliseconds, and the
    // contact itself carries most of it; no glassy highs.
    out = struck(rate, {{540, 0.006f, 0.45f}, {1180, 0.004f, 0.25f}, {2300, 0.002f, 0.07f}}, 0.55f, 2.5f, 1600, 0.06f, 0.24f, 71);
  } else if (name == "plastic_step") {
    // A small printed sole set down on a floor: a dull, soft thud. The sole lands over several
    // milliseconds (heel to toe, its rubbery pad), so the contact is long and low-passed - nothing
    // clicks; the floor answers low (a short thump), the leg and shell a little above it, and
    // nothing rings: every mode dies within a few milliseconds. No partial over 1 kHz.
    out = struck(rate, {{110, 0.012f, 0.5f}, {240, 0.008f, 0.4f}, {520, 0.005f, 0.18f}, {900, 0.003f, 0.05f}}, 0.45f, 7, 450, 0.07f, 0.14f, 73);
  } else if (name == "step") {
    SynthPatch n = patch(Wave::noise, 2400, 0.001f, 0.004f, 0.055f, 0.45f);
    n.lowpass = 900;
    n.seed = 41;
    out = synthesize(n, rate);
    SynthPatch t = patch(Wave::sine, 120, 0.001f, 0.004f, 0.04f, 0.3f);
    add(out, synthesize(t, rate));
  } else if (name == "engine") {
    // 2 s loop: 55 Hz saw + 110 Hz pulse + 27.5 Hz sub, 8 Hz tremolo - whole cycles in 2 s.
    SynthPatch a = patch(Wave::saw, 55, 0, 0, 0, 0.42f);
    a.lowpass = 820;
    a.resonance = 0.3f;
    a.tremolo_depth = 0.12f;
    a.tremolo_rate = 8;
    out = steady_loop(a, 2, rate);
    SynthPatch b = patch(Wave::pulse, 110, 0, 0, 0, 0.18f);
    b.duty = 0.3f;
    b.lowpass = 1200;
    add(out, steady_loop(b, 2, rate));
    SynthPatch c = patch(Wave::sine, 27.5f, 0, 0, 0, 0.35f);
    add(out, steady_loop(c, 2, rate));
  } else if (name == "hover") {
    SynthPatch a = patch(Wave::sine, 120, 0, 0, 0, 0.35f);
    out = steady_loop(a, 2, rate);
    SynthPatch b = patch(Wave::sine, 240.5f, 0, 0, 0, 0.18f);  // 0.5 Hz beating: whole cycles in 2 s
    add(out, steady_loop(b, 2, rate));
    SynthPatch c = patch(Wave::triangle, 480, 0, 0, 0, 0.06f);
    c.tremolo_depth = 0.5f;
    c.tremolo_rate = 6;
    add(out, steady_loop(c, 2, rate));
    SynthPatch n = patch(Wave::pink, 12000, 0, 2.6f, 0, 0.25f);
    n.lowpass = 700;
    Buffer air = synthesize(n, rate);
    make_seamless(air, rate, 0.6f);
    air.resize(out.size(), 0.0f);
    add(out, air);
  } else if (name == "boost") {
    SynthPatch n = patch(Wave::pink, 12000, 0.03f, 0.25f, 0.45f, 0.75f);
    n.lowpass = 700;
    n.lowpass_sweep = 2.6f;
    n.resonance = 0.55f;
    n.highpass = 250;
    n.seed = 5;
    out = synthesize(n, rate);
  } else if (name == "wind" || name == "city") {
    const bool city = name == "city";
    SynthPatch n = patch(Wave::pink, 12000, 0, 7, 0, city ? 0.5f : 0.6f);
    n.lowpass = city ? 360 : 520;
    n.tremolo_depth = city ? 0.15f : 0.45f;
    n.tremolo_rate = city ? 0.13f : 0.21f;
    n.seed = city ? 61 : 59;
    out = synthesize(n, rate);
    if (city) {
      SynthPatch hum = patch(Wave::saw, 60, 0, 7, 0, 0.05f);
      hum.lowpass = 200;
      add(out, synthesize(hum, rate));
    }
    make_seamless(out, rate, 1.0f);
  } else if (name == "hum") {
    SynthPatch a = patch(Wave::sine, 50, 0, 0, 0, 0.35f);
    out = steady_loop(a, 2, rate);
    SynthPatch b = patch(Wave::sine, 100, 0, 0, 0, 0.2f);
    add(out, steady_loop(b, 2, rate));
    SynthPatch c = patch(Wave::saw, 150, 0, 0, 0, 0.05f);
    c.lowpass = 600;
    add(out, steady_loop(c, 2, rate));
  } else if (name == "servo") {
    SynthPatch a = patch(Wave::saw, 820, 0, 0, 0, 0.16f);
    a.lowpass = 3200;
    a.tremolo_depth = 0.3f;
    a.tremolo_rate = 30;
    out = steady_loop(a, 1, rate, 0.2f);
    SynthPatch b = patch(Wave::pulse, 1640, 0, 0, 0, 0.05f);
    add(out, steady_loop(b, 1, rate, 0.2f));
  } else if (name == "quack") {
    // A saw with a falling pitch through a resonant formant: "quack".
    SynthPatch a = patch(Wave::saw, 420, 0.004f, 0.09f, 0.11f, 0.7f);
    a.slide = -1.3f;
    a.lowpass = 1250;
    a.resonance = 0.82f;
    a.highpass = 280;
    a.vibrato_depth = 0.6f;
    a.vibrato_rate = 23;
    out = synthesize(a, rate);
    SynthPatch b = patch(Wave::pulse, 840, 0.004f, 0.09f, 0.11f, 0.18f);
    b.duty = 0.2f;
    b.slide = -1.3f;
    b.lowpass = 2600;
    add(out, synthesize(b, rate));
  } else if (name == "beep") {
    SynthPatch a = patch(Wave::sine, 1320, 0.003f, 0.06f, 0.05f, 0.35f);
    a.arp_ratio = 1.335f;
    a.arp_time = 0.06f;
    a.fm_ratio = 2;
    a.fm_depth = 0.3f;
    out = synthesize(a, rate);
  } else if (name == "click") {
    out = synthesize(patch(Wave::sine, 1500, 0.0005f, 0.004f, 0.03f, 0.35f), rate);
  } else if (name == "confirm") {
    out = synthesize(patch(Wave::sine, 880, 0.002f, 0.04f, 0.08f, 0.35f), rate);
    mix_into(out, synthesize(patch(Wave::sine, 1320, 0.002f, 0.05f, 0.12f, 0.35f), rate), 0.07f, rate);
  } else if (name == "error") {
    SynthPatch a = patch(Wave::square, 180, 0.002f, 0.12f, 0.06f, 0.3f);
    a.lowpass = 1800;
    out = synthesize(a, rate);
    mix_into(out, synthesize(a, rate), 0.16f, rate);
  } else if (find_song(name)) {
    BuiltinJob job;
    job.start(name, rate);
    job.step(0);  // no budget: all at once
    out = job.take(channels);
  } else {
    return false;
  }
  for (f32& v : out) if (!std::isfinite(v)) v = 0;
  // Headroom: no built-in one-shot peaks above 0.85.
  f32 peak = 0;
  for (f32 v : out) peak = std::max(peak, std::fabs(v));
  if (peak > 0.85f) normalize(out, 0.85f);
  return !out.empty();
}

// ---------------------------------------------------------------------------
// incremental rendering

struct BuiltinJob::State {
  std::string name;
  u32 rate = 0;
  const Song* song = nullptr;
  int bar = 0;
  Score score;
  std::size_t next_event = 0;
  Stereo stereo;
  NoteCache cache;
  std::vector<f32> out;
  u32 channels = 1;
  bool done = false;
};

BuiltinJob::BuiltinJob() = default;
BuiltinJob::~BuiltinJob() = default;
BuiltinJob::BuiltinJob(BuiltinJob&&) noexcept = default;
BuiltinJob& BuiltinJob::operator=(BuiltinJob&&) noexcept = default;

bool BuiltinJob::start(std::string_view name, u32 rate) {
  if (name.rfind("builtin:", 0) == 0) name.remove_prefix(8);
  if (!find_builtin(name)) return false;
  state_ = std::make_unique<State>();
  state_->name = std::string(name);
  state_->rate = rate;
  state_->song = find_song(name);
  return true;
}

bool BuiltinJob::step(f64 budget_ms) {
  if (!state_) return true;
  State& st = *state_;
  if (st.done) return true;
  if (!st.song) {
    render_builtin(st.name, st.rate, st.out, st.channels);
    st.done = true;
    return true;
  }
  const Song& song = *st.song;
  const f64 start = now_ms();
  do {
    if (st.next_event >= st.score.events.size()) {
      if (st.bar >= song.bars) {
        st.out = st.stereo.loop(song.bar_seconds * song.bars, st.rate);
        normalize(st.out, song.peak);
        st.channels = 2;
        st.cache.entries.clear();
        st.score.events.clear();
        st.done = true;
        return true;
      }
      st.score.events.clear();
      st.next_event = 0;
      song.bar(st.bar, st.bar * song.bar_seconds, song.bar_seconds / 4, st.rate, st.score);
      ++st.bar;
      continue;
    }
    Score::Event& e = st.score.events[st.next_event++];
    st.stereo.put(st.cache.get(e.key, e.make), e.at, st.rate, e.gain, e.pan);
  } while (budget_ms <= 0 || now_ms() - start < budget_ms);
  return false;
}

bool BuiltinJob::done() const { return !state_ || state_->done; }
const std::string& BuiltinJob::name() const { static const std::string none; return state_ ? state_->name : none; }

std::vector<f32> BuiltinJob::take(u32& channels) {
  if (!state_ || !state_->done) { channels = 1; return {}; }
  channels = state_->channels;
  std::vector<f32> out = std::move(state_->out);
  state_.reset();
  return out;
}

}  // namespace kern::audio
