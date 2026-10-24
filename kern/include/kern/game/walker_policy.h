// Walker control policies: the trained networks a robot runs (rsl_rl actors exported as ONNX:
// observation normaliser, then Gemm/ELU layers). Kern reads the .onnx files the robot itself
// loads (pollen-robotics/microduck-policies) and runs them on the CPU, deterministically.
#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "kern/core/base.h"

namespace kern::game {

struct WalkerPolicy {
  std::string file;                        // "velstand.onnx"
  std::vector<i32> dims;                   // input, hidden..., output (61, 512, 256, 128, 14)
  std::vector<f32> mean, div;              // (obs - mean) / div
  std::vector<std::vector<f32>> weights;   // per layer, [out][in] (Gemm, transB)
  std::vector<std::vector<f32>> biases;
  std::vector<std::pair<std::string, std::string>> metadata;  // the ONNX metadata_props
  /// action = net(observation): ELU between layers, linear output.
  void run(const f32* observation, f32* action) const;
  std::string meta(std::string_view key) const;
};

/// Reads an exported rsl_rl actor (Sub, Div, then Gemm/Elu ... Gemm). False with a reason when the
/// file is not one.
bool parse_onnx_policy(Span<const u8> bytes, WalkerPolicy& out, std::string* error = nullptr);

/// robotd's pick-up detector (duck-control pickup.rs, docs/design/robotd-design.md section 2.4.2):
/// a temporal CNN that reads the last second of the control loop (kPickupWindow rows of
/// kPickupFeatures) and says how likely it is that a hand holds the robot. The graph Pollen ships:
/// normalise and mask, drop the current columns (Gather), three Conv1d(k, stride) + ELU, then
/// Gemm, ELU, Gemm, Sigmoid.
constexpr i32 kPickupWindow = 50, kPickupFeatures = 63;
struct PickupDetector {
  std::vector<f32> mean, stdev, mask;     // per feature
  std::vector<i32> columns;               // the features the convolutions read (Gather)
  struct Conv { i32 out = 0, in = 0, kernel = 0, stride = 1; std::vector<f32> w, b; };
  std::vector<Conv> convs;
  std::vector<i32> dims;                  // head: flattened, hidden..., 1
  std::vector<std::vector<f32>> weights, biases;
  f32 alpha = 1;                          // ELU
  /// p(held) of a window, oldest row first (kPickupWindow x kPickupFeatures).
  f32 run(const f32* window) const;
};
bool parse_onnx_pickup(Span<const u8> bytes, PickupDetector& out, std::string* error = nullptr);
/// The detector robotd ships (duck-control/models/pickup_detector.onnx, built in); null if it does not parse.
const PickupDetector* pickup_detector();

/// The policies this program has: velstand.onnx is built in; the page (or a test) loads the
/// others of the robot's set (kern_studio_load_policy / register_walker_policy).
void register_walker_policy(std::shared_ptr<const WalkerPolicy> policy);
std::shared_ptr<const WalkerPolicy> walker_policy(std::string_view file);
std::vector<std::string> walker_policy_files();

/// The Micro Duck's policy set manifest (manifest.json of microduck-policies, built in).
const char* micro_duck_manifest();

}  // namespace kern::game
