// The digital twin's calibration (docs/kalibrierung.md): device profiles from measurements, and how
// far the simulated robot is from the real one.
//
//   1. Bench (Prüfstand): one servo with a known load (an arm with a weight) follows a test signal;
//      its log (time, commanded angle, measured angle) is replayed in Box3D through the same servo
//      law the robot runs on (game::servo_step) - the rig, built at the walker scale. CMA-ES fits
//      the profile until the simulated angles meet the measured ones; logs held out of the fit
//      show whether it generalises.
//   2. Whole robot (Abgleich): a log of the real duck (what its policy observed and commanded each
//      tick) is replayed in the simulation with the same commands; the difference per sensor is the
//      remaining distance to the real world. It shrinks as the profiles get better.
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "kern/game/servo.h"
#include "kern/lehrer/env.h"

namespace kern::lehrer {

/// What hangs on the servo on the bench (real units): an arm whose centre of mass is `arm` metres
/// from the axis, `mass` kg, `inertia` kg*m^2 about its centre of mass (about the axis direction).
/// With `horizontal_axis` gravity loads it (a pendulum); otherwise it only has inertia.
struct ServoLoad {
  f32 mass = 0.05f, arm = 0.05f, inertia = 1e-5f;
  bool horizontal_axis = true;
};

/// One bench log: samples in time order. Angles in radians, 0 = arm hanging straight down (with a
/// horizontal axis) or wherever the servo's zero is.
struct ServoLog {
  std::string name;
  ServoLoad load;
  std::vector<f32> time, target, angle;
};

/// CSV: a header line, then "time,target,angle" per line (seconds, radians). The load comes from
/// comment lines "# mass 0.05", "# arm 0.05", "# inertia 1e-5", "# horizontal 1".
bool parse_servo_log(std::string_view csv, ServoLog& out, std::string* error = nullptr);
std::string servo_log_csv(const ServoLog& log);

/// Replays `log`'s commands on the rig with `profile`: the simulated angle at each sample time.
std::vector<f32> simulate_servo(const game::ServoProfile& profile, const ServoLog& log);
/// Root mean square of simulated - measured angle over the log (radians).
f32 servo_error(const game::ServoProfile& profile, const ServoLog& log);

struct CalibrationReport {
  game::ServoProfile profile;
  f32 fit_rms = 0, fit_rms_before = 0;          // over the logs it was fitted to (radians)
  f32 holdout_rms = 0, holdout_rms_before = 0;  // over the held-out logs (0 if none)
  i32 evaluations = 0;
};

/// Fits stiffness, damping, friction, torque, armature and delay of `start` to `fit` (CMA-ES in
/// log space around `start`; deterministic for a given seed) and checks the result on `holdout`.
/// `threads` simulations at once.
CalibrationReport calibrate_servo(const game::ServoProfile& start, const std::vector<ServoLog>& fit, const std::vector<ServoLog>& holdout,
                                  i32 generations = 80, u64 seed = 1, i32 threads = 1);

/// A log of the whole robot: per control tick (50 Hz) the 61 observations its policy saw and the 14
/// actions it sent. The robot stands still at home when the log begins.
struct RobotLog {
  std::vector<std::vector<f32>> observation, action;
};
/// CSV: a header, then 61 observation values and 14 actions per line.
bool parse_robot_log(std::string_view csv, RobotLog& out, std::string* error = nullptr);
std::string robot_log_csv(const RobotLog& log);

/// How far the simulation is from a log: the log's actions replayed on the clean simulated robot
/// with `servo`, its observations compared tick by tick (root mean square per sensor group).
struct LogDelta {
  i32 ticks = 0;
  f32 gyro = 0, gravity = 0, joint_pos = 0, joint_vel = 0;  // rad/s, -, rad, rad/s
  f32 joint_pos_each[kActions] = {};
};
LogDelta compare_robot_log(const RobotLog& log, const game::ServoProfile& servo);

/// A robot log made in the simulation: Pollen's velstand network drives the clean duck with `servo`
/// (standing, then walking forward and turning) for `ticks` ticks. For tests and as the format's
/// example.
RobotLog simulated_robot_log(const game::ServoProfile& servo, i32 ticks);

}  // namespace kern::lehrer
