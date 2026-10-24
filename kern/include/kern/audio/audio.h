// Kern audio: a software mixer in C++ that runs inside the engine module.
//
// The host only moves finished PCM to the speakers (KernAudio in kern/web/studio/game.mjs: an
// AudioWorklet where available, a ScriptProcessor elsewhere). Everything else
// happens here, sample by sample and deterministically:
//
//   clips      PCM at the mixer rate (imported audio is resampled once with a
//              windowed-sinc filter; built-in sounds are synthesised, see synth.h)
//   voices     one playing clip each: volume / pitch / pan with click-free
//              ramps, loops, fades, start offset, pause, priority and stealing
//   3D         listener-relative equal-power panning, distance roll-off
//              (inverse, linear, exponential), air absorption and "behind the
//              listener" low-pass, Doppler pitch from relative velocity,
//              virtual voices beyond the audible range
//   buses      music, sfx, ambience, ui, voice -> master; volume, mute,
//              low-pass, reverb send; peak / RMS meters
//   master     Freeverb-style reverb return, look-ahead-free peak limiter,
//              final soft clip: the output never exceeds [-1, 1]
//
// Conventions: metres, seconds, Z up (the renderer's world). Gains are linear;
// helpers convert decibels.
#pragma once

#include <string>
#include <vector>

#include "kern/math/math.h"

namespace kern::audio {

constexpr u32 kOutputChannels = 2;
constexpr u32 kMaxVoices = 128;

enum class Bus : u8 { master = 0, music, sfx, ambience, ui, voice, count };
const char* bus_name(Bus b);
bool parse_bus(std::string_view s, Bus& out);

enum class Rolloff : u8 { inverse = 0, linear, exponential };
const char* rolloff_name(Rolloff r);
bool parse_rolloff(std::string_view s, Rolloff& out);

using ClipId = u32;   // 0 = none
using VoiceId = u64;  // generation << 32 | slot; 0 = none

inline f32 db_to_gain(f32 db) { return std::pow(10.0f, db / 20.0f); }
inline f32 gain_to_db(f32 g) { return g > 1e-9f ? 20.0f * std::log10(g) : -180.0f; }

struct ClipInfo {
  std::string name;
  u32 frames = 0;       // at the mixer rate
  u32 channels = 1;     // 1 or 2
  f32 duration = 0;     // seconds
  f32 peak = 0;         // largest absolute sample
  bool loopable = false;  // generated seamless loop
};

struct PlayParams {
  f32 volume = 1;       // linear gain
  f32 pitch = 1;        // playback-rate factor (0.05 .. 8)
  f32 pan = 0;          // -1 left .. 1 right (2D voices)
  bool loop = false;
  Bus bus = Bus::sfx;
  f32 fade_in = 0;      // seconds
  f32 start = 0;        // start offset in seconds
  i32 priority = 0;     // higher keeps playing when voices run out
  // 3D
  bool spatial = false;
  Vec3 position, velocity;
  f32 min_distance = 1;   // full volume inside
  f32 max_distance = 50;  // silent (linear) / clamp distance (inverse, exponential)
  Rolloff rolloff = Rolloff::inverse;
  f32 spread = 0;         // 0 point source .. 1 fully enveloping (no panning)
  f32 doppler = 1;        // 0 disables the Doppler shift for this voice
};

struct BusSettings {
  f32 volume = 1;
  bool muted = false;
  f32 lowpass_hz = 0;     // 0 = off
  f32 reverb_send = 0;    // 0..1 to the master reverb
};

struct Meter {
  f32 peak[kOutputChannels] = {};
  f32 rms[kOutputChannels] = {};
};

struct Stats {
  u32 voices = 0, audible = 0, virtual_voices = 0, stolen = 0, clips = 0;
  u64 frames_rendered = 0;
  f32 limiter_reduction_db = 0;
  Meter master;
  Meter buses[static_cast<u32>(Bus::count)];
  /// Master peak with hold: falls back at 20 dB per second (a meter's needle, so a short sound
  /// stays visible between two reads of the stats).
  f32 peak_hold[kOutputChannels] = {};
};

struct Listener {
  Vec3 position;
  Vec3 forward{1, 0, 0}, up{0, 0, 1};
  Vec3 velocity;
};

/// The mixer. One instance per module is typical (Kern Studio owns one).
class Mixer {
 public:
  Mixer();
  ~Mixer();
  Mixer(const Mixer&) = delete;
  Mixer& operator=(const Mixer&) = delete;

  /// Sample rate of the output device (8000 .. 192000). Clears everything.
  bool init(u32 sample_rate, u32 max_voices = 64);
  u32 sample_rate() const { return rate_; }
  bool ready() const { return rate_ != 0; }

  // ---- clips ----
  /// Copies `frames` frames of `channels` interleaved samples at `rate` and resamples
  /// them to the mixer rate. Returns 0 (and sets last_error) for invalid input.
  ClipId create_clip(const f32* samples, u32 frames, u32 channels, u32 rate, std::string name);
  /// Takes samples already at the mixer rate (synthesised sounds).
  ClipId adopt_clip(std::vector<f32> samples, u32 channels, std::string name, bool loopable = false);
  bool destroy_clip(ClipId clip);  // stops its voices
  const ClipInfo* clip_info(ClipId clip) const;
  const std::vector<f32>* clip_samples(ClipId clip) const;
  u32 clip_count() const;

  // ---- voices ----
  VoiceId play(ClipId clip, const PlayParams& params);
  /// Stops with a short fade (seconds; 0 = 5 ms click guard).
  void stop(VoiceId voice, f32 fade = 0);
  void stop_all(Bus bus = Bus::master, f32 fade = 0);  // master = every bus
  bool playing(VoiceId voice) const;
  void set_volume(VoiceId voice, f32 volume, f32 ramp = 0.03f);
  void set_pitch(VoiceId voice, f32 pitch);
  void set_pan(VoiceId voice, f32 pan);
  void set_position(VoiceId voice, Vec3 position, Vec3 velocity = {});
  void set_paused(VoiceId voice, bool paused);
  /// Pauses / resumes every voice (game pause) without losing their positions.
  void set_all_paused(bool paused);
  /// Playback position in seconds.
  f32 position_of(VoiceId voice) const;

  // ---- world ----
  void set_listener(const Listener& listener);
  const Listener& listener() const { return listener_; }
  f32 speed_of_sound = 343;
  f32 doppler_factor = 1;   // global scale of the Doppler effect
  BusSettings& bus(Bus b) { return buses_[static_cast<u32>(b)]; }
  const BusSettings& bus(Bus b) const { return buses_[static_cast<u32>(b)]; }
  /// Reverb character of the master return.
  void set_reverb(f32 room_size, f32 damping, f32 wet);

  // ---- output ----
  /// Mixes `frames` frames of interleaved stereo into `out`.
  void render(f32* out, u32 frames);
  const Stats& stats() const { return stats_; }

 private:
  struct Clip;
  struct Voice;
  struct Biquad;
  struct Reverb;
  struct Limiter;

  Voice* voice(VoiceId id);
  const Voice* voice(VoiceId id) const;
  void spatialize(Voice& v, f32& left, f32& right, f32& pitch_factor, f32& cutoff) const;
  void mix_voice(Voice& v, u32 frames);

  u32 rate_ = 0;
  u32 max_voices_ = 64;
  std::vector<Clip*> clips_;     // index = id - 1 (nullptr = free)
  std::vector<Voice*> voices_;   // fixed pool
  Listener listener_;
  BusSettings buses_[static_cast<u32>(Bus::count)];
  Biquad* bus_filters_ = nullptr;  // per bus, stereo
  Reverb* reverb_ = nullptr;
  Limiter* limiter_ = nullptr;
  std::vector<f32> bus_mix_;     // count * frames * 2
  std::vector<f32> reverb_in_, scratch_;
  Stats stats_;
  u32 generation_ = 1;
  u64 play_serial_ = 0;
  f32 reverb_wet_ = 0.22f;
  f32 gain_now_[static_cast<u32>(Bus::count)] = {};  // smoothed bus gains (index 0 = master)
  bool gains_valid_ = false;
};

}  // namespace kern::audio
