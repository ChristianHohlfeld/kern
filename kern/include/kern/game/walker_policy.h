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

/// The policies this program has: velstand.onnx is built in; the page (or a test) loads the
/// others of the robot's set (kern_studio_load_policy / register_walker_policy).
void register_walker_policy(std::shared_ptr<const WalkerPolicy> policy);
std::shared_ptr<const WalkerPolicy> walker_policy(std::string_view file);
std::vector<std::string> walker_policy_files();

/// The Micro Duck's policy set manifest (manifest.json of microduck-policies, built in).
const char* micro_duck_manifest();

}  // namespace kern::game
