// CMA-ES (Hansen's covariance matrix adaptation evolution strategy): a derivative-free optimiser for
// a few dozen parameters. The Lehrer uses it where gradients do not exist: fitting a device profile
// to measurements (calibration) - the simulation is the function, its parameters the input.
// Deterministic (its own Rng), so a calibration is reproducible like a training run.
#pragma once

#include <functional>
#include <vector>

#include "kern/lehrer/env.h"

namespace kern::lehrer {

struct CmaesResult {
  std::vector<f64> best;     // the best point evaluated
  f64 best_cost = 0;
  i32 evaluations = 0, generations = 0;
};

/// Minimises cost(x) from `start` with step size `sigma` (per coordinate, the same scale for all:
/// normalise the parameters first). `population` <= 0 picks 4 + 3 ln n. The candidates of a
/// generation go to `evaluate_all` (so they can run in parallel); it fills costs[i] for xs[i].
/// Stops after `generations` or when sigma falls below `tolerance`. `start` is evaluated first, so
/// the result is never worse than it.
CmaesResult cmaes_minimise(const std::vector<f64>& start, f64 sigma, i32 generations, u64 seed,
                           const std::function<void(const std::vector<std::vector<f64>>& xs, std::vector<f64>& costs)>& evaluate_all,
                           i32 population = 0, f64 tolerance = 1e-8,
                           const std::function<void(i32 generation, const CmaesResult& so_far, f64 sigma)>& progress = {});

}  // namespace kern::lehrer
