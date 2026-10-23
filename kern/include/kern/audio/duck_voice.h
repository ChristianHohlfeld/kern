// The Micro Duck's voice: a tiny seedable synth for pet vocalisations, ported
// from the real robot's `sounds` crate (pollen-robotics/microduck, Apache-2.0).
//
// A single u32 seed deterministically derives a DuckPersonality - pitch
// register, harmonic tilt, nasality, vibrato, quackiness, tempo. Two seeds sound
// like two different creatures; the same seed always sounds the same. Each tag
// has several variants (small re-rolls within the same voice) so the duck does
// not sound like a stuck recording.
//
// Tags: alarm, greet, inquire, peck, chirp, coo, wheee. `wheee` is segmented
// (start / loop / end) so a player can hold the loop for as long as a ride
// lasts; render_duck_voice("wheee") returns start + loop + loop + end like the
// robot's `render`.
//
// Rendering happens at 48 kHz exactly as on the robot (same RNG, same recipes,
// same float precision), then is resampled linearly when another rate is asked.
#pragma once

#include <string_view>
#include <vector>

#include "kern/core/base.h"

namespace kern::audio {

/// The rate the duck voice is synthesised at (the robot's SR).
inline constexpr u32 kDuckVoiceRate = 48000;

/// Stable per-robot vocal traits, every field a function of the seed.
struct DuckPersonality {
  u32 seed = 0;

  // pitch
  f64 pitch_center_hz = 0;
  f64 register_shift = 0;   // -1..+1 extra octave-ish shift (Rust: `register`)
  f64 pitch_spread = 0;     // 0..1, how dramatic glides are
  f64 glide_bias = 0;       // -1..+1, negative falls, positive rises

  // timbre
  f64 brightness = 0;       // 0..1 harmonic rolloff (1 = bright / buzzy)
  f64 tilt = 0;             // 1.4..2.8 exponent on harmonic decay
  f64 nasal = 0;            // 0..1 emphasis on 2nd/3rd harmonic
  f64 harmonic_skew = 0;    // -1..+1, negative odd-only, positive even-leaning
  u32 formant_n = 1;        // 1..5, which harmonic the formant boosts
  f64 formant_gain = 0;     // 0..1.4 formant strength

  // modulation
  f64 vibrato_rate_hz = 0;
  f64 vibrato_depth = 0;    // semitones
  f64 jitter_depth = 0;     // semitones, random pitch wobble
  f64 breath = 0;           // noise mix
  f64 quackiness = 0;       // 0.2..1, pure tone vs AM buzz
  f64 am_rate_hz = 0;       // 18..55
  f64 am_depth = 0;         // 0.15..0.7
  f64 warble_hz = 0;        // 7..18, trill on chirp
  f64 warble_depth = 0;     // semitones

  // timing
  f64 attack_sharpness = 0; // 0 soft pad, 1 snappy
  f64 speed = 1;            // 0.82..1.22 global tempo multiplier
};

/// The personality a seed derives (Personality::from_seed).
DuckPersonality duck_personality(u32 seed);

/// sha256(id), first four bytes big-endian - the robot's seed from its SoC
/// serial / machine id (seed_from_id).
u32 duck_seed_from_id(std::string_view id);

/// How many variants a tag has: greet / chirp 12, wheee 6, every other 10.
u32 duck_variant_count(std::string_view tag);

/// Renders one tag/variant (mono) at `rate`. False for an unknown tag.
/// `variant` is not wrapped: any value renders (the robot uses 0..count-1).
bool render_duck_voice(std::string_view tag, u32 seed, u32 variant, u32 rate, std::vector<f32>& out);

/// The wheee ride as its three segments (mono, at `rate`): play start, repeat
/// loop while the ride lasts, then end. Always true.
bool render_duck_wheee(u32 seed, u32 variant, u32 rate, std::vector<f32>& start, std::vector<f32>& loop,
                       std::vector<f32>& end);

}  // namespace kern::audio
