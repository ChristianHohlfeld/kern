// ServoSound (servo_sound.h): the drive trains simulated at audio rate, heard through the shell.
#include "kern/audio/servo_sound.h"

#include <algorithm>
#include <cmath>

namespace kern::audio {

namespace {
constexpr f64 kTwoPi = 6.283185307179586;
// Mesh stiffness over one tooth pass: the double-contact zones stiffen it twice per pitch around the
// single-contact middle - a fundamental with falling harmonics (spur gears, contact ratio ~1.5).
// Tabulated (14 servos x 4 stages at 24 kHz: a table, not std::cos).
constexpr i32 kTable = 2048;
const f32* mesh_table() {
  static f32 t[kTable + 1];
  static const bool built = [] {
    for (i32 i = 0; i <= kTable; ++i) {
      const f64 w = kTwoPi * i / kTable;
      t[i] = static_cast<f32>(std::cos(w) + 0.45 * std::cos(2 * w) + 0.2 * std::cos(3 * w));
    }
    return true;
  }();
  (void)built;
  return t;
}
const f32* cos_table() {
  static f32 t[kTable + 1];
  static const bool built = [] {
    for (i32 i = 0; i <= kTable; ++i) t[i] = static_cast<f32>(std::cos(kTwoPi * i / kTable));
    return true;
  }();
  (void)built;
  return t;
}
inline f32 lookup(const f32* t, f64 turns) {
  const f64 x = (turns - std::floor(turns)) * kTable;
  const i32 i = static_cast<i32>(x);
  const f32 f = static_cast<f32>(x - i);
  return t[i] + (t[i + 1] - t[i]) * f;
}
inline f32 mesh_variation(f64 phase) { return lookup(mesh_table(), phase); }
}  // namespace

ServoSound::ServoSound(i32 servos, ServoAcoustics acoustics, u32 seed)
    : a_(std::move(acoustics)), servos_(static_cast<std::size_t>(std::max(0, servos))), rng_(seed * 2654435761u | 1u) {
  stage_ratio_ = std::pow(static_cast<f64>(a_.gear_ratio), 1.0 / std::max(1, a_.stages));
}

f32 ServoSound::noise() {
  rng_ ^= rng_ << 13;
  rng_ ^= rng_ >> 17;
  rng_ ^= rng_ << 5;
  return static_cast<f32>(rng_ >> 8) * (2.0f / 16777216.0f) - 1.0f;
}

void ServoSound::set(i32 servo, f32 joint_speed, f32 current) {
  if (servo < 0 || servo >= static_cast<i32>(servos_.size())) return;
  Servo& s = servos_[static_cast<std::size_t>(servo)];
  s.speed = std::isfinite(joint_speed) ? joint_speed : 0.0f;
  s.current = std::isfinite(current) ? std::fabs(current) : 0.0f;
}

void ServoSound::footstep(f32 speed, f32 mass) {
  if (!(speed > 0) || !(mass > 0)) return;
  // Hertzian contact of the sole on the floor: the force pulse's peak grows with speed^1.2 and the
  // contact lasts longer the softer and heavier it is (t ~ (m^2 / v)^(1/5)); a soft printed sole on
  // wood: a few milliseconds.
  Step st;
  st.amp = 0.6f * std::pow(std::min(speed, 1.0f) / 0.3f, 1.2f) * std::sqrt(std::min(mass, 2.0f) / 0.2f);
  st.duration = std::clamp(0.006f * std::pow(mass * mass / std::max(0.02f, speed) / 0.13f, 0.2f), 0.003f, 0.012f);
  st.t = 0;
  if (steps_.size() < 8) steps_.push_back(st);
}

void ServoSound::step() {
  const f32 dt = 1.0f / a_.input_rate;
  f64 shell = 0;
  for (Servo& s : servos_) {
    s.s += glide_ * (s.speed - s.s);
    s.c += glide_ * (s.current - s.c);
    const f32 load = std::min(1.5f, s.c / std::max(1e-3f, a_.stall_current));
    const f64 motor = static_cast<f64>(s.s) * a_.gear_ratio / kTwoPi;  // turns per second, signed
    const f64 speed = std::fabs(motor);
    s.motor += motor * dt;
    s.motor -= std::floor(s.motor);
    f32 force = 0;
    if (speed > 0.2) {
      // Gear mesh, stage by stage: the force each stage carries (the load, a little even unloaded:
      // the gears' own inertia) pulses with its mesh stiffness at the tooth-pass phase.
      f64 shaft = motor;
      const f32 carried = 0.1f + load;
      static constexpr f32 kStage[4] = {1.0f, 0.55f, 0.32f, 0.2f};
      const f32 reach = std::min(1.0f, static_cast<f32>(std::sqrt(speed * a_.pinion_teeth / 800.0)));
      for (i32 k = 0; k < std::min(4, a_.stages); ++k) {
        s.stage[k] += shaft * dt;
        s.stage[k] -= std::floor(s.stage[k]);
        force += kStage[k] * carried * reach * mesh_variation(s.stage[k] * a_.pinion_teeth);
        shaft /= stage_ratio_;
      }
      // The motor: commutation ripple with the current, brush friction noise with the speed.
      force += 0.3f * load * std::min(1.0f, static_cast<f32>(speed / 50.0)) * lookup(cos_table(), s.motor * a_.commutator_slots * 2);
      s.brush += k_brush_ * (noise() - s.brush);
      force += 0.25f * std::min(1.0f, static_cast<f32>(speed / 60.0)) * s.brush;
    }
    // Backlash: the drive reversing strikes the teeth across their gap.
    const f32 sign = s.s > 0.02f ? 1.0f : s.s < -0.02f ? -1.0f : s.last_sign;
    if (sign != 0 && s.last_sign != 0 && sign != s.last_sign) force += a_.backlash * (0.3f + load) * noise();
    s.last_sign = sign;
    // Holding a load: the controller's corrections pulse the motor (band-limited force noise).
    if (load > 0.02f) {
      const f32 still = 1.0f - std::min(1.0f, static_cast<f32>(speed / 15.0));
      s.hold += k_hold_ * (noise() - s.hold);
      force += 0.2f * load * still * s.hold;
    }
    shell += force;
  }
  shell /= std::sqrt(static_cast<f64>(std::max<std::size_t>(1, servos_.size())));
  // Footsteps: half-sine contact force pulses.
  f64 floor = 0;
  for (Step& st : steps_) {
    if (st.t < 0) continue;
    floor += st.amp * std::sin(3.14159265358979 * st.t / st.duration);
    st.t += dt;
    if (st.t >= st.duration) st.t = -1;
  }
  steps_.erase(std::remove_if(steps_.begin(), steps_.end(), [](const Step& st) { return st.t < 0; }), steps_.end());
  const f64 in[2] = {shell, floor};
  synth_.write_input(in);
}

void ServoSound::render(f32* mono, u32 frames, u32 rate) {
  if (!mono || !rate) return;
  if (rate != rate_) {
    rate_ = rate;
    physical::Synthesizer::Parameters p;
    p.channels = 2;  // 0 the servos into the shell, 1 the soles onto the floor
    p.input_rate = a_.input_rate;
    p.audio_rate = static_cast<f32>(rate);
    p.input_antialias_hz = std::min(9000.0f, 0.45f * static_cast<f32>(rate));
    p.audio.dF_F_mix = 0.0002f;        // engine-sim 0.01 for an exhaust under 2 kHz; servos reach kHz: the same balance at ~800 Hz
    p.audio.input_sample_noise = 0.3f;
    p.audio.input_sample_noise_cutoff = 8000;
    p.audio.air_noise = 0.25f;           // gears and brushes: a little rough, not an exhaust's turbulence
    p.audio.air_noise_cutoff = 3000;
    p.audio.convolution = 0.75f;         // mostly the shell, a little straight from the servos
    p.audio.leveler_target = 0.9f;       // the leveler only limits (max gain 1): louder with more load
    p.audio.leveler_max_gain = 1.0f;
    p.audio.volume = a_.level;
    synth_.initialize(p);
    synth_.set_modal_response(0, a_.shell);
    synth_.set_modal_response(1, a_.floor);
    glide_ = 1.0f - std::exp(-1.0f / (0.008f * a_.input_rate));
    k_brush_ = 1.0f - std::exp(-static_cast<f32>(kTwoPi) * 3000.0f / a_.input_rate);
    k_hold_ = 1.0f - std::exp(-static_cast<f32>(kTwoPi) * 2000.0f / a_.input_rate);
  }
  // The simulation stays just ahead of the audio (engine-sim: simulate until the buffer is full).
  u32 done = 0;
  while (done < frames) {
    while (synth_.pending() < static_cast<i32>(frames - done)) step();
    done += static_cast<u32>(synth_.render(mono + done, static_cast<i32>(frames - done)));
  }
}

}  // namespace kern::audio
