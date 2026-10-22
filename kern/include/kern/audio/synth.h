// Procedural sound: a small subtractive synthesiser (sfxr lineage, extended
// with resonant filters, noise colours, FM and layering) and a step sequencer
// for music loops. Everything is rendered offline into clips at the mixer
// rate, deterministically (fixed seeds): the same patch always produces the
// same samples on every platform.
//
// The built-in library ("builtin:<name>") covers what a game needs out of the
// box - pickups, jumps, impacts, engines, wind, UI, fanfares, a duck's quack,
// servo whine and two music loops - so levels sound good without imported
// files. Loops are rendered seamless (periodic content or crossfaded noise).
#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "kern/core/base.h"

namespace kern::audio {

enum class Wave : u8 { square, saw, sine, triangle, noise, pink, pulse };

struct SynthPatch {
  Wave wave = Wave::square;
  f32 volume = 0.5f;
  // envelope (seconds): attack, sustain (with optional punch), release
  f32 attack = 0.005f, sustain = 0.1f, punch = 0, release = 0.2f;
  // pitch
  f32 frequency = 440;          // Hz at the start
  f32 slide = 0;                // octaves per second
  f32 slide_accel = 0;          // octaves per second^2
  f32 min_frequency = 20;       // the slide stops here
  f32 vibrato_depth = 0;        // semitones
  f32 vibrato_rate = 6;         // Hz
  f32 arp_ratio = 1;            // frequency multiplier after arp_time
  f32 arp_time = 0;             // seconds, 0 = off
  f32 duty = 0.5f, duty_sweep = 0;  // square / pulse
  f32 fm_ratio = 0, fm_depth = 0;   // sine FM: modulator ratio and index
  // filters
  f32 lowpass = 0;              // Hz, 0 = off
  f32 lowpass_sweep = 0;        // octaves per second
  f32 resonance = 0;            // 0..0.95
  f32 highpass = 0;             // Hz, 0 = off
  // modulation
  f32 tremolo_depth = 0, tremolo_rate = 0;
  f32 repeat = 0;               // seconds: restart the pitch envelope
  f32 bitcrush = 0;             // 0..1 (sample-and-hold amount)
  u32 seed = 1;                 // noise
};

/// Renders a patch (mono) at `rate`.
std::vector<f32> synthesize(const SynthPatch& patch, u32 rate);
/// Mixes `src` into `dst` starting at `offset_seconds` (dst grows as needed).
void mix_into(std::vector<f32>& dst, const std::vector<f32>& src, f32 offset_seconds, u32 rate, f32 gain = 1);
/// Makes a mono buffer loop seamlessly by crossfading its tail into its head (seconds).
void make_seamless(std::vector<f32>& samples, u32 rate, f32 crossfade);
/// Normalises to `peak` (no-op for silence).
void normalize(std::vector<f32>& samples, f32 peak);

struct BuiltinSound {
  const char* name;      // "pickup" -> asset id "builtin:pickup"
  const char* label;
  const char* category;  // Gameplay, Physics, Vehicle, Ambience, UI, Music, Robot
  bool loop;
};
/// The built-in library in display order.
const std::vector<BuiltinSound>& builtin_sounds();
const BuiltinSound* find_builtin(std::string_view name);
/// Renders a built-in sound (mono, or stereo interleaved for music). False for unknown names.
bool render_builtin(std::string_view name, u32 rate, std::vector<f32>& out, u32& channels);

/// Renders a built-in sound in slices: music takes a few hundred milliseconds,
/// so it is produced one bar per step() and never stalls a frame.
class BuiltinJob {
 public:
  BuiltinJob();
  ~BuiltinJob();
  BuiltinJob(BuiltinJob&&) noexcept;
  BuiltinJob& operator=(BuiltinJob&&) noexcept;
  bool start(std::string_view name, u32 rate);  // false for unknown names
  /// Renders for about `budget_ms` (0 = until finished); true when finished.
  bool step(f64 budget_ms = 4);
  bool done() const;
  const std::string& name() const;
  std::vector<f32> take(u32& channels);         // the finished samples (once)

 private:
  struct State;
  std::unique_ptr<State> state_;
};

}  // namespace kern::audio
