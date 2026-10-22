#include "kern/audio/audio.h"

#include <algorithm>
#include <cstring>

namespace kern::audio {

namespace {

constexpr u32 kBlock = 128;            // frames per parameter update
constexpr u32 kBusCount = static_cast<u32>(Bus::count);
constexpr f32 kStopGuard = 0.005f;     // seconds: shortest fade that never clicks
constexpr f32 kSilence = 1e-5f;        // voices below this gain are virtual

const char* const kBusNames[] = {"master", "music", "sfx", "ambience", "ui", "voice"};
const char* const kRolloffNames[] = {"inverse", "linear", "exponential"};

f32 finite_or(f32 v, f32 fallback) { return std::isfinite(v) ? v : fallback; }

/// Hermite (Catmull-Rom) interpolation of 4 samples at fraction t.
inline f32 hermite(f32 x0, f32 x1, f32 x2, f32 x3, f32 t) {
  const f32 c1 = 0.5f * (x2 - x0);
  const f32 c2 = x0 - 2.5f * x1 + 2.0f * x2 - 0.5f * x3;
  const f32 c3 = 0.5f * (x3 - x0) + 1.5f * (x1 - x2);
  return ((c3 * t + c2) * t + c1) * t + x1;
}

f32 one_pole_coefficient(f32 cutoff, u32 rate) {
  if (cutoff <= 0 || cutoff >= 0.49f * static_cast<f32>(rate)) return 1.0f;
  return 1.0f - std::exp(-2.0f * kPi * cutoff / static_cast<f32>(rate));
}

/// Windowed-sinc resampler (Blackman window, 16 zero crossings), used once per imported clip.
std::vector<f32> resample(const f32* in, u32 frames, u32 channels, u32 from, u32 to) {
  if (from == to) return std::vector<f32>(in, in + static_cast<std::size_t>(frames) * channels);
  const f64 ratio = static_cast<f64>(to) / from;
  const u32 out_frames = static_cast<u32>(std::ceil(frames * ratio));
  std::vector<f32> out(static_cast<std::size_t>(out_frames) * channels, 0.0f);
  const f64 cutoff = std::min(1.0, ratio);  // low-pass at the lower Nyquist
  constexpr i32 kZeros = 16;
  const f64 half = kZeros / cutoff;
  for (u32 o = 0; o < out_frames; ++o) {
    const f64 center = o / ratio;
    const i32 first = static_cast<i32>(std::floor(center - half)), last = static_cast<i32>(std::ceil(center + half));
    for (u32 c = 0; c < channels; ++c) {
      f64 acc = 0, wsum = 0;
      for (i32 i = first; i <= last; ++i) {
        if (i < 0 || i >= static_cast<i32>(frames)) continue;
        const f64 x = (i - center) * cutoff;
        const f64 sinc = std::fabs(x) < 1e-9 ? 1.0 : std::sin(3.141592653589793 * x) / (3.141592653589793 * x);
        const f64 w = (i - center + half) / (2 * half);
        if (w < 0 || w > 1) continue;
        const f64 window = 0.42 - 0.5 * std::cos(2 * 3.141592653589793 * w) + 0.08 * std::cos(4 * 3.141592653589793 * w);
        const f64 k = sinc * window;
        acc += in[static_cast<std::size_t>(i) * channels + c] * k;
        wsum += k;
      }
      out[static_cast<std::size_t>(o) * channels + c] = static_cast<f32>(wsum > 1e-9 ? acc / wsum : 0.0);
    }
  }
  return out;
}

}  // namespace

const char* bus_name(Bus b) { return static_cast<u32>(b) < kBusCount ? kBusNames[static_cast<u32>(b)] : "sfx"; }
bool parse_bus(std::string_view s, Bus& out) {
  for (u32 i = 0; i < kBusCount; ++i)
    if (s == kBusNames[i]) { out = static_cast<Bus>(i); return true; }
  return false;
}
const char* rolloff_name(Rolloff r) { return static_cast<u32>(r) < 3 ? kRolloffNames[static_cast<u32>(r)] : "inverse"; }
bool parse_rolloff(std::string_view s, Rolloff& out) {
  for (u32 i = 0; i < 3; ++i)
    if (s == kRolloffNames[i]) { out = static_cast<Rolloff>(i); return true; }
  return false;
}

// ---------------------------------------------------------------------------
// internals

struct Mixer::Clip {
  std::vector<f32> data;
  ClipInfo info;
};

struct Mixer::Voice {
  bool active = false;
  u32 generation = 0;
  ClipId clip = 0;
  PlayParams p;
  f64 position = 0;          // frames (fractional)
  f32 gain_l = 0, gain_r = 0;  // gains applied at the end of the last block
  f32 volume = 1, volume_target = 1, volume_step = 0;
  f32 envelope = 0, envelope_step = 0;  // fade in / out
  bool stopping = false, paused = false, paused_global = false, started = false;
  f32 doppler = 1;           // smoothed Doppler factor
  f32 lp_state[2] = {0, 0};  // air absorption / occlusion low-pass
  f32 lp_coefficient = 1;
  f32 audibility = 0;        // last block's gain (stealing decisions)
  u64 order = 0;
};

struct Mixer::Biquad {
  // RBJ low-pass, stereo, direct form I.
  f32 b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
  f32 x1[2] = {}, x2[2] = {}, y1[2] = {}, y2[2] = {};
  f32 cutoff = -1;
  void configure(f32 hz, u32 rate) {
    if (hz == cutoff) return;
    cutoff = hz;
    const f32 w = 2 * kPi * clamp(hz, 10.0f, 0.45f * static_cast<f32>(rate)) / static_cast<f32>(rate);
    const f32 cw = std::cos(w), alpha = std::sin(w) / (2 * 0.7071f);
    const f32 a0 = 1 + alpha;
    b0 = (1 - cw) * 0.5f / a0;
    b1 = (1 - cw) / a0;
    b2 = b0;
    a1 = -2 * cw / a0;
    a2 = (1 - alpha) / a0;
  }
  f32 process(f32 x, u32 c) {
    const f32 y = b0 * x + b1 * x1[c] + b2 * x2[c] - a1 * y1[c] - a2 * y2[c];
    x2[c] = x1[c]; x1[c] = x;
    y2[c] = y1[c]; y1[c] = y;
    return y;
  }
  void reset() { std::memset(x1, 0, sizeof x1); std::memset(x2, 0, sizeof x2); std::memset(y1, 0, sizeof y1); std::memset(y2, 0, sizeof y2); }
};

/// Freeverb (Jezar at Dreampoint): 8 parallel damped combs into 4 series allpasses per channel.
struct Mixer::Reverb {
  struct Comb {
    std::vector<f32> buffer;
    u32 index = 0;
    f32 store = 0;
    f32 process(f32 x, f32 feedback, f32 damp) {
      const f32 out = buffer[index];
      store = out * (1 - damp) + store * damp;
      buffer[index] = x + store * feedback;
      if (++index >= buffer.size()) index = 0;
      return out;
    }
  };
  struct Allpass {
    std::vector<f32> buffer;
    u32 index = 0;
    f32 process(f32 x) {
      const f32 b = buffer[index];
      buffer[index] = x + b * 0.5f;
      if (++index >= buffer.size()) index = 0;
      return b - x;
    }
  };
  Comb combs[2][8];
  Allpass allpasses[2][4];
  f32 feedback = 0.84f, damp = 0.2f;
  void init(u32 rate) {
    static constexpr u32 kComb[8] = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
    static constexpr u32 kAllpass[4] = {556, 441, 341, 225};
    const f64 scale = rate / 44100.0;
    for (u32 c = 0; c < 2; ++c) {
      const u32 spread = c ? 23 : 0;
      for (u32 i = 0; i < 8; ++i) { combs[c][i].buffer.assign(std::max<u32>(1, static_cast<u32>((kComb[i] + spread) * scale)), 0.0f); combs[c][i].index = 0; combs[c][i].store = 0; }
      for (u32 i = 0; i < 4; ++i) { allpasses[c][i].buffer.assign(std::max<u32>(1, static_cast<u32>((kAllpass[i] + spread) * scale)), 0.0f); allpasses[c][i].index = 0; }
    }
  }
  void configure(f32 room_size, f32 damping) {
    feedback = 0.7f + 0.28f * clamp(room_size, 0.0f, 1.0f);
    damp = 0.4f * clamp(damping, 0.0f, 1.0f);
  }
  void process(const f32* in, f32* out, u32 frames) {
    for (u32 f = 0; f < frames; ++f) {
      const f32 x = (in[f * 2] + in[f * 2 + 1]) * 0.015f;  // Freeverb's fixed input gain
      for (u32 c = 0; c < 2; ++c) {
        f32 acc = 0;
        for (Comb& comb : combs[c]) acc += comb.process(x, feedback, damp);
        for (Allpass& ap : allpasses[c]) acc = ap.process(acc);
        out[f * 2 + c] = acc;
      }
    }
  }
};

/// Peak limiter: instant attack (the gain drops before a peak passes the
/// threshold), smooth release; a final soft clip keeps |out| <= 1.
struct Mixer::Limiter {
  f32 gain = 1, release = 0.9995f, threshold = 0.92f;
  f32 min_gain = 1;
  void init(u32 rate) { release = std::exp(-1.0f / (0.12f * static_cast<f32>(rate))); }
  void process(f32* io, u32 frames) {
    min_gain = 1;
    for (u32 f = 0; f < frames; ++f) {
      const f32 peak = std::max(std::fabs(io[f * 2]), std::fabs(io[f * 2 + 1]));
      const f32 needed = peak * gain > threshold ? threshold / peak : 1.0f;
      if (needed < gain) gain = needed;
      else gain = 1 - (1 - gain) * release;
      min_gain = std::min(min_gain, gain);
      for (u32 c = 0; c < 2; ++c) {
        f32 s = io[f * 2 + c] * gain;
        // Soft clip above the threshold (never reached with the gain above, but NaN-safe and exact at 1).
        if (!std::isfinite(s)) s = 0;
        if (s > threshold) s = threshold + (1 - threshold) * std::tanh((s - threshold) / (1 - threshold));
        else if (s < -threshold) s = -threshold - (1 - threshold) * std::tanh((-s - threshold) / (1 - threshold));
        io[f * 2 + c] = s;
      }
    }
  }
};

// ---------------------------------------------------------------------------
// setup

Mixer::Mixer() = default;

Mixer::~Mixer() {
  for (Clip* c : clips_) delete c;
  for (Voice* v : voices_) delete v;
  delete[] bus_filters_;
  delete reverb_;
  delete limiter_;
}

bool Mixer::init(u32 sample_rate, u32 max_voices) {
  if (sample_rate < 8000 || sample_rate > 192000) return fail("audio: sample rate %u outside 8000..192000", sample_rate);
  if (max_voices < 1 || max_voices > kMaxVoices) return fail("audio: 1..%u voices", kMaxVoices);
  for (Clip* c : clips_) delete c;
  clips_.clear();
  for (Voice* v : voices_) delete v;
  voices_.clear();
  rate_ = sample_rate;
  max_voices_ = max_voices;
  for (u32 i = 0; i < max_voices; ++i) voices_.push_back(new Voice());
  delete[] bus_filters_;
  bus_filters_ = new Biquad[kBusCount];
  delete reverb_;
  reverb_ = new Reverb();
  reverb_->init(rate_);
  reverb_->configure(0.6f, 0.4f);
  delete limiter_;
  limiter_ = new Limiter();
  limiter_->init(rate_);
  bus_mix_.assign(static_cast<std::size_t>(kBusCount) * kBlock * 2, 0.0f);
  reverb_in_.assign(kBlock * 2, 0.0f);
  scratch_.assign(kBlock * 2, 0.0f);
  for (BusSettings& b : buses_) b = BusSettings{};
  buses_[static_cast<u32>(Bus::music)].volume = 0.7f;
  buses_[static_cast<u32>(Bus::ambience)].volume = 0.8f;
  buses_[static_cast<u32>(Bus::sfx)].reverb_send = 0.12f;
  buses_[static_cast<u32>(Bus::voice)].reverb_send = 0.08f;
  stats_ = Stats{};
  listener_ = Listener{};
  gains_valid_ = false;
  return true;
}

void Mixer::set_reverb(f32 room_size, f32 damping, f32 wet) {
  if (reverb_) reverb_->configure(finite_or(room_size, 0.6f), finite_or(damping, 0.4f));
  reverb_wet_ = clamp(finite_or(wet, 0.22f), 0.0f, 1.0f);
}

// ---------------------------------------------------------------------------
// clips

ClipId Mixer::create_clip(const f32* samples, u32 frames, u32 channels, u32 rate, std::string name) {
  if (!rate_) { fail("audio: mixer not initialised"); return 0; }
  if (!samples || !frames || (channels != 1 && channels != 2) || rate < 3000 || rate > 384000) {
    fail("audio: invalid clip (frames %u, channels %u, rate %u)", frames, channels, rate);
    return 0;
  }
  if (static_cast<u64>(frames) * channels > 60ull * 384000 * 2) { fail("audio: clip longer than 60 s"); return 0; }
  std::vector<f32> data = resample(samples, frames, channels, rate, rate_);
  for (f32& s : data) if (!std::isfinite(s)) s = 0;
  return adopt_clip(std::move(data), channels, std::move(name));
}

ClipId Mixer::adopt_clip(std::vector<f32> samples, u32 channels, std::string name, bool loopable) {
  if (!rate_) { fail("audio: mixer not initialised"); return 0; }
  if (samples.empty() || (channels != 1 && channels != 2) || samples.size() % channels) { fail("audio: invalid clip data"); return 0; }
  Clip* c = new Clip();
  c->data = std::move(samples);
  c->info.name = std::move(name);
  c->info.channels = channels;
  c->info.frames = static_cast<u32>(c->data.size() / channels);
  c->info.duration = static_cast<f32>(c->info.frames) / static_cast<f32>(rate_);
  c->info.loopable = loopable;
  for (f32 s : c->data) c->info.peak = std::max(c->info.peak, std::fabs(s));
  for (std::size_t i = 0; i < clips_.size(); ++i)
    if (!clips_[i]) { clips_[i] = c; return static_cast<ClipId>(i + 1); }
  clips_.push_back(c);
  return static_cast<ClipId>(clips_.size());
}

bool Mixer::destroy_clip(ClipId id) {
  if (!id || id > clips_.size() || !clips_[id - 1]) return false;
  for (Voice* v : voices_)
    if (v->active && v->clip == id) v->active = false;
  delete clips_[id - 1];
  clips_[id - 1] = nullptr;
  return true;
}

const ClipInfo* Mixer::clip_info(ClipId id) const {
  return id && id <= clips_.size() && clips_[id - 1] ? &clips_[id - 1]->info : nullptr;
}

const std::vector<f32>* Mixer::clip_samples(ClipId id) const {
  return id && id <= clips_.size() && clips_[id - 1] ? &clips_[id - 1]->data : nullptr;
}

u32 Mixer::clip_count() const {
  u32 n = 0;
  for (const Clip* c : clips_) n += c != nullptr;
  return n;
}

// ---------------------------------------------------------------------------
// voices

Mixer::Voice* Mixer::voice(VoiceId id) {
  const u32 slot = static_cast<u32>(id & 0xffffffffu), gen = static_cast<u32>(id >> 32);
  if (!id || slot >= voices_.size()) return nullptr;
  Voice* v = voices_[slot];
  return v->active && v->generation == gen ? v : nullptr;
}
const Mixer::Voice* Mixer::voice(VoiceId id) const { return const_cast<Mixer*>(this)->voice(id); }

VoiceId Mixer::play(ClipId clip, const PlayParams& params) {
  if (!rate_) { fail("audio: mixer not initialised"); return 0; }
  const ClipInfo* info = clip_info(clip);
  if (!info) { fail("audio: unknown clip %u", clip); return 0; }
  PlayParams p = params;
  p.volume = clamp(finite_or(p.volume, 1), 0.0f, 16.0f);
  p.pitch = clamp(finite_or(p.pitch, 1), 0.05f, 8.0f);
  p.pan = clamp(finite_or(p.pan, 0), -1.0f, 1.0f);
  p.min_distance = std::max(0.01f, finite_or(p.min_distance, 1));
  p.max_distance = std::max(p.min_distance + 0.01f, finite_or(p.max_distance, 50));
  p.spread = clamp(finite_or(p.spread, 0), 0.0f, 1.0f);
  p.doppler = clamp(finite_or(p.doppler, 1), 0.0f, 4.0f);
  if (!finite(p.position)) p.position = {};
  if (!finite(p.velocity)) p.velocity = {};
  if (static_cast<u32>(p.bus) >= kBusCount || p.bus == Bus::master) p.bus = Bus::sfx;
  // A free slot, or steal the least important voice (lowest priority, then quietest, then oldest).
  u32 slot = ~0u;
  for (u32 i = 0; i < voices_.size(); ++i)
    if (!voices_[i]->active) { slot = i; break; }
  if (slot == ~0u) {
    for (u32 i = 0; i < voices_.size(); ++i) {
      const Voice& v = *voices_[i];
      if (v.p.priority > p.priority) continue;
      if (slot == ~0u) { slot = i; continue; }
      const Voice& b = *voices_[slot];
      // Lowest priority first, then the quietest, then the oldest.
      if (v.p.priority != b.p.priority ? v.p.priority < b.p.priority
          : v.audibility != b.audibility ? v.audibility < b.audibility : v.order < b.order)
        slot = i;
    }
    if (slot == ~0u) { fail("audio: all voices busy with higher priority"); return 0; }
    stats_.stolen++;
  }
  Voice& v = *voices_[slot];
  v = Voice{};
  v.active = true;
  v.generation = generation_++;
  if (!generation_) generation_ = 1;
  v.clip = clip;
  v.p = p;
  v.position = std::clamp(static_cast<f64>(finite_or(p.start, 0)) * rate_, 0.0, static_cast<f64>(info->frames - 1));
  v.volume = v.volume_target = p.volume;
  v.envelope = p.fade_in > 0 ? 0.0f : 1.0f;
  v.envelope_step = p.fade_in > 0 ? 1.0f / (p.fade_in * static_cast<f32>(rate_)) : 0.0f;
  v.order = ++play_serial_;
  return (static_cast<u64>(v.generation) << 32) | slot;
}

void Mixer::stop(VoiceId id, f32 fade) {
  Voice* v = voice(id);
  if (!v) return;
  const f32 t = std::max(kStopGuard, finite_or(fade, 0));
  v->stopping = true;
  v->envelope_step = -std::max(v->envelope, 1e-3f) / (t * static_cast<f32>(rate_));
}

void Mixer::stop_all(Bus bus, f32 fade) {
  for (u32 i = 0; i < voices_.size(); ++i) {
    Voice& v = *voices_[i];
    if (v.active && (bus == Bus::master || v.p.bus == bus)) stop((static_cast<u64>(v.generation) << 32) | i, fade);
  }
}

bool Mixer::playing(VoiceId id) const { return voice(id) != nullptr; }

void Mixer::set_volume(VoiceId id, f32 volume, f32 ramp) {
  Voice* v = voice(id);
  if (!v) return;
  v->volume_target = clamp(finite_or(volume, v->volume_target), 0.0f, 16.0f);
  const f32 frames = std::max(1.0f, finite_or(ramp, 0.03f) * static_cast<f32>(rate_));
  v->volume_step = (v->volume_target - v->volume) / frames;
}

void Mixer::set_pitch(VoiceId id, f32 pitch) { if (Voice* v = voice(id)) v->p.pitch = clamp(finite_or(pitch, v->p.pitch), 0.05f, 8.0f); }
void Mixer::set_pan(VoiceId id, f32 pan) { if (Voice* v = voice(id)) v->p.pan = clamp(finite_or(pan, v->p.pan), -1.0f, 1.0f); }
void Mixer::set_position(VoiceId id, Vec3 position, Vec3 velocity) {
  Voice* v = voice(id);
  if (!v) return;
  if (finite(position)) v->p.position = position;
  if (finite(velocity)) v->p.velocity = velocity;
}
void Mixer::set_paused(VoiceId id, bool paused) { if (Voice* v = voice(id)) v->paused = paused; }
void Mixer::set_all_paused(bool paused) { for (Voice* v : voices_) v->paused_global = paused; }

f32 Mixer::position_of(VoiceId id) const {
  const Voice* v = voice(id);
  return v ? static_cast<f32>(v->position / rate_) : 0.0f;
}

void Mixer::set_listener(const Listener& l) {
  if (!finite(l.position) || !finite(l.forward) || !finite(l.up)) return;
  listener_ = l;
  listener_.forward = normalize(l.forward, {1, 0, 0});
  listener_.up = normalize(l.up, {0, 0, 1});
  if (!finite(listener_.velocity)) listener_.velocity = {};
}

// ---------------------------------------------------------------------------
// mixing

void Mixer::spatialize(Voice& v, f32& left, f32& right, f32& pitch_factor, f32& cutoff) const {
  const PlayParams& p = v.p;
  pitch_factor = 1;
  cutoff = 0;
  if (!p.spatial) {
    // Equal-power pan, normalised so that a centred voice plays at unity gain on both channels.
    const f32 a = (p.pan + 1) * 0.25f * kPi;
    left = std::cos(a) * 1.41421356f;
    right = std::sin(a) * 1.41421356f;
    return;
  }
  const Vec3 rel = p.position - listener_.position;
  const f32 dist = length(rel);
  const Vec3 dir = dist > 1e-4f ? rel / dist : listener_.forward;
  // Distance roll-off.
  const f32 d = clamp(dist, p.min_distance, p.max_distance);
  f32 g = 1;
  switch (p.rolloff) {
    case Rolloff::inverse: g = p.min_distance / d; break;
    case Rolloff::linear: g = 1 - (d - p.min_distance) / (p.max_distance - p.min_distance); break;
    case Rolloff::exponential: g = std::pow(d / p.min_distance, -1.5f); break;
  }
  // Inverse and exponential curves fade to silence over the last 20 % before max_distance.
  if (p.rolloff != Rolloff::linear && dist > p.max_distance * 0.8f) g *= clamp((p.max_distance - dist) / (p.max_distance * 0.2f), 0.0f, 1.0f);
  // Panning in listener space.
  const Vec3 right_axis = normalize(cross(listener_.forward, listener_.up), {0, -1, 0});
  f32 pan = dot(dir, right_axis);
  const f32 front = dot(dir, listener_.forward);
  // Close sources and wide spreads pan less.
  const f32 proximity = clamp(dist / std::max(0.25f, p.min_distance * 0.5f), 0.0f, 1.0f);
  pan *= (1 - p.spread) * proximity;
  const f32 a = (pan + 1) * 0.25f * kPi;
  left = std::cos(a) * 1.41421356f * g;
  right = std::sin(a) * 1.41421356f * g;
  // Air absorption with distance, a softer top end behind the listener.
  f32 fc = 20000.0f / (1.0f + dist / 120.0f);
  if (front < 0) fc *= 1.0f + 0.6f * front;  // down to 40 % straight behind
  cutoff = fc;
  // Doppler: f' = f (c - vl) / (c - vs), velocities along the source->listener line.
  const f32 scale = doppler_factor * p.doppler;
  if (scale > 0 && dist > 1e-3f) {
    const Vec3 to_listener = -dir;
    const f32 c = std::max(1.0f, speed_of_sound);
    const f32 vl = clamp(dot(listener_.velocity, to_listener) * scale, -c * 0.9f, c * 0.9f);
    const f32 vs = clamp(dot(p.velocity, to_listener) * scale, -c * 0.9f, c * 0.9f);
    pitch_factor = clamp((c - vl) / (c - vs), 0.5f, 2.0f);
  }
}

void Mixer::mix_voice(Voice& v, u32 frames) {
  const Clip* clip = clips_[v.clip - 1];
  if (!clip) { v.active = false; return; }
  const u32 total = clip->info.frames, ch = clip->info.channels;
  const f32* data = clip->data.data();
  const BusSettings& bus = buses_[static_cast<u32>(v.p.bus)];
  f32 target_l = 0, target_r = 0, doppler = 1, cutoff = 0;
  spatialize(v, target_l, target_r, doppler, cutoff);
  if (!v.started) { v.doppler = doppler; v.started = true; }
  v.lp_coefficient = cutoff > 0 ? one_pole_coefficient(cutoff, rate_) : 1.0f;
  const f32 paused = v.paused || v.paused_global ? 0.0f : 1.0f;
  // Virtual voice: inaudible spatial voices keep their place without being mixed.
  const f32 loudness = std::max(target_l, target_r) * v.volume * (bus.muted ? 0.0f : 1.0f);
  v.audibility = loudness;
  if (v.p.spatial && loudness < kSilence && !v.stopping) {
    stats_.virtual_voices++;
    if (paused > 0) {
      v.position += static_cast<f64>(v.p.pitch) * frames;
      if (v.p.loop) v.position = std::fmod(v.position, static_cast<f64>(total));
      else if (v.position >= total) v.active = false;
    }
    v.gain_l = v.gain_r = 0;
    return;
  }
  stats_.audible++;
  f32* out = bus_mix_.data() + static_cast<std::size_t>(static_cast<u32>(v.p.bus)) * kBlock * 2;
  // Smooth the Doppler factor (no zipper noise on fast passes).
  v.doppler += (doppler - v.doppler) * 0.25f;
  const f64 step = static_cast<f64>(v.p.pitch) * v.doppler * paused;
  const f32 inv = 1.0f / static_cast<f32>(frames);
  const f32 dl = (target_l - v.gain_l) * inv, dr = (target_r - v.gain_r) * inv;
  f32 gl = v.gain_l, gr = v.gain_r;
  const f32 k = v.lp_coefficient;
  for (u32 f = 0; f < frames; ++f) {
    gl += dl;
    gr += dr;
    if (paused == 0) continue;
    // Envelope (fade in / stop) and volume ramp.
    v.envelope += v.envelope_step;
    if (v.envelope >= 1) { v.envelope = 1; if (!v.stopping) v.envelope_step = 0; }
    if (v.envelope <= 0 && v.stopping) { v.active = false; break; }
    if (v.volume_step != 0) {
      v.volume += v.volume_step;
      if ((v.volume_step > 0 && v.volume >= v.volume_target) || (v.volume_step < 0 && v.volume <= v.volume_target)) { v.volume = v.volume_target; v.volume_step = 0; }
    }
    // Read with 4-point interpolation (loops wrap, one-shots clamp to silence).
    const f64 pos = v.position;
    const i64 i1 = static_cast<i64>(pos);
    const f32 t = static_cast<f32>(pos - static_cast<f64>(i1));
    auto at = [&](i64 i, u32 c) -> f32 {
      if (v.p.loop) { i %= static_cast<i64>(total); if (i < 0) i += total; }
      else if (i < 0 || i >= static_cast<i64>(total)) return 0.0f;
      return data[static_cast<std::size_t>(i) * ch + c];
    };
    f32 s[2];
    for (u32 c = 0; c < ch; ++c) s[c] = hermite(at(i1 - 1, c), at(i1, c), at(i1 + 1, c), at(i1 + 2, c), t);
    if (ch == 1) s[1] = s[0];
    const f32 g = v.envelope * v.volume;
    for (u32 c = 0; c < 2; ++c) {
      v.lp_state[c] += k * (s[c] - v.lp_state[c]);
      s[c] = k >= 1 ? s[c] : v.lp_state[c];
    }
    out[f * 2] += s[0] * g * gl;
    out[f * 2 + 1] += s[1] * g * gr;
    v.position += step;
    if (v.position >= total) {
      if (v.p.loop) v.position = std::fmod(v.position, static_cast<f64>(total));
      else { v.active = false; break; }
    }
  }
  v.gain_l = target_l;
  v.gain_r = target_r;
}

void Mixer::render(f32* out, u32 frames) {
  if (!out) return;
  if (!rate_) { std::memset(out, 0, static_cast<std::size_t>(frames) * 2 * sizeof(f32)); return; }
  for (Meter& m : stats_.buses) m = Meter{};
  stats_.master = Meter{};
  f32 sum_sq[kBusCount + 1][2] = {};
  u32 measured = 0;
  stats_.limiter_reduction_db = 0;
  u32 done = 0;
  while (done < frames) {
    const u32 n = std::min(kBlock, frames - done);
    std::fill(bus_mix_.begin(), bus_mix_.end(), 0.0f);
    stats_.voices = stats_.audible = stats_.virtual_voices = 0;
    for (Voice* v : voices_) {
      if (!v->active) continue;
      stats_.voices++;
      mix_voice(*v, n);
    }
    // Buses -> master (+ reverb send).
    f32* master = out + static_cast<std::size_t>(done) * 2;
    std::fill(master, master + n * 2, 0.0f);
    std::fill(reverb_in_.begin(), reverb_in_.end(), 0.0f);
    for (u32 b = 1; b < kBusCount; ++b) {
      const BusSettings& s = buses_[b];
      f32* buf = bus_mix_.data() + static_cast<std::size_t>(b) * kBlock * 2;
      // Volume and mute changes ramp over the block: no zipper noise or clicks.
      const f32 target = s.muted ? 0.0f : clamp(finite_or(s.volume, 1), 0.0f, 4.0f);
      const f32 g0 = gains_valid_ ? gain_now_[b] : target, dg = (target - g0) / static_cast<f32>(n);
      gain_now_[b] = target;
      const bool filtered = s.lowpass_hz > 0;
      if (filtered) bus_filters_[b].configure(s.lowpass_hz, rate_);
      const f32 send = clamp(finite_or(s.reverb_send, 0), 0.0f, 1.0f);
      Meter& m = stats_.buses[b];
      for (u32 f = 0; f < n; ++f)
        for (u32 c = 0; c < 2; ++c) {
          f32 x = buf[f * 2 + c] * (g0 + dg * static_cast<f32>(f + 1));
          if (filtered) x = bus_filters_[b].process(x, c);
          master[f * 2 + c] += x;
          reverb_in_[f * 2 + c] += x * send;
          m.peak[c] = std::max(m.peak[c], std::fabs(x));
          sum_sq[b][c] += x * x;
        }
    }
    reverb_->process(reverb_in_.data(), scratch_.data(), n);
    const BusSettings& ms = buses_[0];
    const f32 master_target = ms.muted ? 0.0f : clamp(finite_or(ms.volume, 1), 0.0f, 4.0f);
    const f32 m0 = gains_valid_ ? gain_now_[0] : master_target, dm = (master_target - m0) / static_cast<f32>(n);
    gain_now_[0] = master_target;
    gains_valid_ = true;
    for (u32 f = 0; f < n * 2; ++f) master[f] = (master[f] + scratch_[f] * reverb_wet_ * 3.0f) * (m0 + dm * static_cast<f32>(f / 2 + 1));
    limiter_->process(master, n);
    stats_.limiter_reduction_db = std::min(stats_.limiter_reduction_db, gain_to_db(limiter_->min_gain));
    for (u32 f = 0; f < n; ++f)
      for (u32 c = 0; c < 2; ++c) {
        const f32 x = master[f * 2 + c];
        stats_.master.peak[c] = std::max(stats_.master.peak[c], std::fabs(x));
        sum_sq[kBusCount][c] += x * x;
      }
    measured += n;
    done += n;
  }
  const f32 inv = measured ? 1.0f / static_cast<f32>(measured) : 0.0f;
  for (u32 b = 0; b < kBusCount; ++b)
    for (u32 c = 0; c < 2; ++c) stats_.buses[b].rms[c] = std::sqrt(sum_sq[b][c] * inv);
  for (u32 c = 0; c < 2; ++c) stats_.master.rms[c] = std::sqrt(sum_sq[kBusCount][c] * inv);
  const f32 fall = std::pow(10.0f, -20.0f / 20.0f * static_cast<f32>(frames) / static_cast<f32>(rate_));  // -20 dB/s
  for (u32 c = 0; c < 2; ++c) stats_.peak_hold[c] = std::max(stats_.peak_hold[c] * fall, stats_.master.peak[c]);
  stats_.buses[0] = stats_.master;
  stats_.frames_rendered += frames;
  stats_.clips = clip_count();
}

}  // namespace kern::audio
