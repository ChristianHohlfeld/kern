// evaluate_jump (env.h): what a jump policy does on the clean robot.
#include <cmath>

#include "kern/lehrer/env.h"

namespace kern::lehrer {

JumpReport evaluate_jump(ActFn act, void* user, i32 episodes, u64 seed, i32 substeps) {
  JumpReport rep;
  for (i32 e = 0; e < episodes; ++e) {
    DuckEnv env(make_jump_task(), seed + static_cast<u64>(e) * 7919u, substeps > 0 ? substeps : game::kWalkerSubsteps);
    env.task().randomize = false;
    env.reset();
    const f32 z0 = env.probe().position.z;
    f32 apex = 0, flight = 0, longest = 0;
    bool fell = false;
    for (;;) {
      f32 a[kActions];
      act(user, env.observation(), a);
      const Env::Step s = env.step(a);
      const auto& p = env.probe();
      apex = max_of(apex, p.position.z - z0);
      const bool airborne = !p.foot_contact[0] && !p.foot_contact[1] && !p.body_contact;
      flight = airborne ? flight + kControlDt : 0.0f;
      longest = max_of(longest, flight);
      if (s.terminal) { fell = true; break; }
      if (s.timeout) break;
    }
    const auto& p = env.probe();
    const bool standing = !fell && p.foot_contact[0] && p.foot_contact[1] && !p.body_contact && p.gravity.z < -0.9f;
    ++rep.episodes;
    rep.falls += fell ? 1 : 0;
    rep.height_mm += apex * 1000;
    rep.flight_s += longest;
    rep.best_height_mm = max_of(rep.best_height_mm, apex * 1000);
    rep.best_flight_s = max_of(rep.best_flight_s, longest);
    if (standing && longest >= 0.06f - 1e-4f && apex >= 0.015f) ++rep.jumps;
  }
  if (rep.episodes) { rep.height_mm /= static_cast<f32>(rep.episodes); rep.flight_s /= static_cast<f32>(rep.episodes); }
  return rep;
}

}  // namespace kern::lehrer
