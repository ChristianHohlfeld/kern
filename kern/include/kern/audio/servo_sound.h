// The sound of a robot's servos, simulated the way engine-sim simulates an engine's
// (audio/physical_synth.h): a physical simulation at a high rate writes the force that reaches the
// shell, and engine-sim's synthesizer turns that into sound through the shell's response.
//
// The simulation is each servo's drive train, run at 24 kHz between two of Box3D's control ticks
// (Box3D gives it its boundary conditions - each servo's joint speed and its current, the load,
// 50 times a second; they glide in between). The XL330 (Dynamixel, the Micro Duck's joints) is a
// DC motor behind 288.35:1 of spur gears:
//  * gear mesh: each stage's teeth come into and out of contact, so the mesh stiffness varies with
//    the tooth-pass phase (teeth x shaft angle) and the transmitted force - the load the stage
//    carries - pulses with it (transmission error excitation). Integrated from the motor angle.
//  * motor: the commutator's torque ripple, kt x current x ripple(slots x 2 per turn), and the
//    brushes' friction noise with the speed.
//  * backlash: when the torque reverses, the teeth cross the gap and strike - an impulse with the
//    speed of the change.
//  * holding: a loaded servo that does not turn still pulses - its controller corrects around the
//    target at the PWM rate - a small force noise with the current.
// The sum is the force on the shell. The synthesizer radiates it through the shell's modes (its
// impulse response, as modes: printed PLA, a hand-sized box) - the acoustic coupling: the shell, not
// the servo, is what is heard. Footsteps go the same way: Box3D's contact (approach speed, mass) is
// a Hertzian contact pulse into a second channel whose response is the floor and the leg.
#pragma once

#include <vector>

#include "kern/audio/audio.h"
#include "kern/audio/physical_synth.h"
#include "kern/core/base.h"

namespace kern::audio {

/// The motor and gearbox of one servo type (defaults: XL330-M288-T) and the body around them.
struct ServoAcoustics {
  f32 gear_ratio = 288.35f;          // datasheet
  i32 stages = 4;                    // spur stages (the ratio split evenly)
  f32 pinion_teeth = 10;             // teeth on each driving gear (not published: assumed)
  f32 commutator_slots = 3;          // a cored 3-slot motor: torque ripple 6 x per turn (assumed)
  f32 stall_current = 2.63f;         // A, the current at full duty (BAM m6: V / R)
  f32 backlash = 0.6f;               // strength of the tooth strike when the torque reverses
  // The shell's modes (Hz, decay s, amplitude) and the floor / leg's for footsteps.
  std::vector<physical::ModalResponse::Mode> shell{{460, 0.018f, 0.55f}, {1040, 0.012f, 0.8f}, {2150, 0.008f, 0.55f}, {3700, 0.005f, 0.3f}};
  std::vector<physical::ModalResponse::Mode> floor{{95, 0.02f, 0.7f}, {210, 0.012f, 0.5f}, {460, 0.006f, 0.25f}, {880, 0.003f, 0.08f}};
  f32 input_rate = 24000;            // the drive-train simulation's rate
  f32 level = 1;
};

class ServoSound : public Generator {
 public:
  explicit ServoSound(i32 servos, ServoAcoustics acoustics = {}, u32 seed = 1);
  /// The servo's state as it reports it: joint speed (rad/s, real, signed) and |present current| (A).
  void set(i32 servo, f32 joint_speed, f32 current);
  /// A sole set down (Box3D contact): approach speed (m/s, real) and the mass behind it (kg, real).
  void footstep(f32 speed, f32 mass);
  void render(f32* mono, u32 frames, u32 rate) override;

 private:
  struct Servo {
    f32 speed = 0, current = 0;  // targets (Box3D, 50 Hz)
    f32 s = 0, c = 0;            // glided
    f64 motor = 0;               // motor angle (turns)
    f64 stage[4] = {};           // each stage's driving shaft angle (turns)
    f32 brush = 0, hold = 0;     // noise states
    f32 last_sign = 0;           // torque direction (backlash)
  };
  struct Step { f32 amp = 0, duration = 0, t = -1; };
  void step();                   // one drive-train simulation step (1 / input_rate)
  f32 noise();

  ServoAcoustics a_;
  std::vector<Servo> servos_;
  std::vector<Step> steps_;
  physical::Synthesizer synth_;
  u32 rate_ = 0, rng_;
  f64 stage_ratio_ = 1;
  f32 glide_ = 0, k_brush_ = 0, k_hold_ = 0;
};

}  // namespace kern::audio
