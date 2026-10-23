// Writes a trained actor as ONNX in the shape of Pollen's exported networks (rsl_rl: input "obs"
// [1, n], Sub(mean), Div(std + eps), (Gemm, Elu)*, Gemm, output "actions" [1, m]; opset 17), so
// Kern (parse_onnx_policy), the Studio and the robot's own runtime all load it.
#pragma once

#include <string>
#include <utility>
#include <vector>

#include "kern/core/base.h"
#include "kern/train/nn.h"

namespace kern::train {

std::vector<u8> export_onnx(const Mlp& actor, const Normalizer& normalizer,
                            const std::vector<std::pair<std::string, std::string>>& metadata);

/// The Micro Duck's metadata as Pollen's networks carry it (joint names, home pose, observation
/// layout, action scale), plus who trained it and on what.
std::vector<std::pair<std::string, std::string>> micro_duck_metadata(const std::string& task);

}  // namespace kern::train
