// CMA-ES (cmaes.h), after Hansen's "The CMA Evolution Strategy: A Tutorial" (2016): weighted
// recombination, cumulative step-size adaptation, rank-one and rank-mu covariance updates; the
// covariance's eigen decomposition by Jacobi rotations (n is small).
#include "kern/lehrer/cmaes.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace kern::lehrer {

namespace {

using Matrix = std::vector<std::vector<f64>>;

/// Symmetric A = V diag(d) V^T (Jacobi); columns of V are the eigenvectors.
void eigen(Matrix a, Matrix& v, std::vector<f64>& d) {
  const std::size_t n = a.size();
  v.assign(n, std::vector<f64>(n, 0));
  for (std::size_t i = 0; i < n; ++i) v[i][i] = 1;
  for (int sweep = 0; sweep < 64; ++sweep) {
    f64 off = 0;
    for (std::size_t p = 0; p < n; ++p)
      for (std::size_t q = p + 1; q < n; ++q) off += a[p][q] * a[p][q];
    if (off < 1e-30) break;
    for (std::size_t p = 0; p < n; ++p)
      for (std::size_t q = p + 1; q < n; ++q) {
        if (std::fabs(a[p][q]) < 1e-300) continue;
        const f64 th = (a[q][q] - a[p][p]) / (2 * a[p][q]);
        const f64 t = (th >= 0 ? 1.0 : -1.0) / (std::fabs(th) + std::sqrt(th * th + 1));
        const f64 c = 1 / std::sqrt(t * t + 1), s = t * c;
        for (std::size_t k = 0; k < n; ++k) {
          const f64 akp = a[k][p], akq = a[k][q];
          a[k][p] = c * akp - s * akq;
          a[k][q] = s * akp + c * akq;
        }
        for (std::size_t k = 0; k < n; ++k) {
          const f64 apk = a[p][k], aqk = a[q][k];
          a[p][k] = c * apk - s * aqk;
          a[q][k] = s * apk + c * aqk;
        }
        for (std::size_t k = 0; k < n; ++k) {
          const f64 vkp = v[k][p], vkq = v[k][q];
          v[k][p] = c * vkp - s * vkq;
          v[k][q] = s * vkp + c * vkq;
        }
      }
  }
  d.resize(n);
  for (std::size_t i = 0; i < n; ++i) d[i] = std::sqrt(std::max(1e-30, a[i][i]));
}

}  // namespace

CmaesResult cmaes_minimise(const std::vector<f64>& start, f64 sigma, i32 generations, u64 seed,
                           const std::function<void(const std::vector<std::vector<f64>>&, std::vector<f64>&)>& evaluate_all, i32 population,
                           f64 tolerance, const std::function<void(i32, const CmaesResult&, f64)>& progress) {
  const std::size_t n = start.size();
  const f64 N = static_cast<f64>(n);
  const i32 lambda = population > 0 ? population : 4 + static_cast<i32>(3 * kern::dm::log(N));
  const i32 mu = lambda / 2;
  std::vector<f64> w(static_cast<std::size_t>(mu));
  for (i32 i = 0; i < mu; ++i) w[static_cast<std::size_t>(i)] = kern::dm::log(mu + 0.5) - kern::dm::log(i + 1.0);
  const f64 wsum = std::accumulate(w.begin(), w.end(), 0.0);
  for (f64& x : w) x /= wsum;
  f64 mueff = 0;
  for (f64 x : w) mueff += x * x;
  mueff = 1 / mueff;
  const f64 cs = (mueff + 2) / (N + mueff + 5);
  const f64 ds = 1 + 2 * std::max(0.0, std::sqrt((mueff - 1) / (N + 1)) - 1) + cs;
  const f64 cc = (4 + mueff / N) / (N + 4 + 2 * mueff / N);
  const f64 c1 = 2 / ((N + 1.3) * (N + 1.3) + mueff);
  const f64 cmu = std::min(1 - c1, 2 * (mueff - 2 + 1 / mueff) / ((N + 2) * (N + 2) + mueff));
  const f64 chi = std::sqrt(N) * (1 - 1 / (4 * N) + 1 / (21 * N * N));

  Rng rng(seed);
  auto normal = [&] {  // in double precision, deterministic (kern::dm)
    const f64 u1 = std::max(1e-300, static_cast<f64>(rng.next() >> 11) * 0x1.0p-53), u2 = static_cast<f64>(rng.next() >> 11) * 0x1.0p-53;
    return std::sqrt(-2 * kern::dm::log(u1)) * kern::dm::cos(6.283185307179586 * u2);
  };
  std::vector<f64> m = start, ps(n, 0), pc(n, 0), d(n, 1);
  Matrix c(n, std::vector<f64>(n, 0)), b;
  for (std::size_t i = 0; i < n; ++i) c[i][i] = 1;
  CmaesResult res;
  res.best = start;
  {  // the start itself is a candidate: the result is never worse than where it began
    std::vector<f64> c0(1);
    evaluate_all({start}, c0);
    res.best_cost = c0[0];
    res.evaluations = 1;
  }
  std::vector<std::vector<f64>> z(static_cast<std::size_t>(lambda), std::vector<f64>(n)), y = z, x = z;
  std::vector<f64> cost(static_cast<std::size_t>(lambda));
  for (i32 gen = 0; gen < generations && sigma > tolerance; ++gen) {
    eigen(c, b, d);
    for (std::size_t k = 0; k < static_cast<std::size_t>(lambda); ++k) {
      for (std::size_t i = 0; i < n; ++i) z[k][i] = normal();
      for (std::size_t i = 0; i < n; ++i) {
        f64 s = 0;
        for (std::size_t j = 0; j < n; ++j) s += b[i][j] * d[j] * z[k][j];
        y[k][i] = s;
        x[k][i] = m[i] + sigma * s;
      }
    }
    evaluate_all(x, cost);
    res.evaluations += lambda;
    res.generations = gen + 1;
    std::vector<i32> idx(static_cast<std::size_t>(lambda));
    std::iota(idx.begin(), idx.end(), 0);
    std::stable_sort(idx.begin(), idx.end(), [&](i32 a, i32 bb) { return cost[static_cast<std::size_t>(a)] < cost[static_cast<std::size_t>(bb)]; });
    if (cost[static_cast<std::size_t>(idx[0])] < res.best_cost) {
      res.best_cost = cost[static_cast<std::size_t>(idx[0])];
      res.best = x[static_cast<std::size_t>(idx[0])];
    }
    std::vector<f64> yw(n, 0), zw(n, 0);
    for (i32 r = 0; r < mu; ++r)
      for (std::size_t j = 0; j < n; ++j) {
        yw[j] += w[static_cast<std::size_t>(r)] * y[static_cast<std::size_t>(idx[static_cast<std::size_t>(r)])][j];
        zw[j] += w[static_cast<std::size_t>(r)] * z[static_cast<std::size_t>(idx[static_cast<std::size_t>(r)])][j];
      }
    for (std::size_t j = 0; j < n; ++j) m[j] += sigma * yw[j];
    f64 psn = 0;
    for (std::size_t i = 0; i < n; ++i) {
      f64 bz = 0;
      for (std::size_t j = 0; j < n; ++j) bz += b[i][j] * zw[j];
      ps[i] = (1 - cs) * ps[i] + std::sqrt(cs * (2 - cs) * mueff) * bz;
      psn += ps[i] * ps[i];
    }
    psn = std::sqrt(psn);
    const bool hsig = psn / std::sqrt(1 - kern::dm::pow(1 - cs, 2.0 * (gen + 1))) < (1.4 + 2 / (N + 1)) * chi;
    for (std::size_t i = 0; i < n; ++i) pc[i] = (1 - cc) * pc[i] + (hsig ? std::sqrt(cc * (2 - cc) * mueff) * yw[i] : 0.0);
    for (std::size_t i = 0; i < n; ++i)
      for (std::size_t j = 0; j < n; ++j) {
        f64 rank_mu = 0;
        for (i32 r = 0; r < mu; ++r) {
          const auto& yr = y[static_cast<std::size_t>(idx[static_cast<std::size_t>(r)])];
          rank_mu += w[static_cast<std::size_t>(r)] * yr[i] * yr[j];
        }
        c[i][j] = (1 - c1 - cmu) * c[i][j] + c1 * (pc[i] * pc[j] + (hsig ? 0.0 : cc * (2 - cc) * c[i][j])) + cmu * rank_mu;
      }
    sigma *= kern::dm::exp((cs / ds) * (psn / chi - 1));
    res.mean = m;
    if (progress) progress(gen, res, sigma);
  }
  res.mean = m;
  return res;
}

}  // namespace kern::lehrer
