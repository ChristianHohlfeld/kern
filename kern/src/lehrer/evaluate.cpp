// evaluate_jump (env.h): what a jump policy does in the Studio's physics.
#include <cmath>

#include "kern/lehrer/env.h"

namespace kern::lehrer {

void JumpMeter::tick(const game::GameWorld::WalkerProbe& p) {
  if (!started) { z0 = ground_z = p.position.z; started = true; }
  ++ticks;
  min_up = min_of(min_up, -p.gravity.z);
  const bool air = !p.foot_contact[0] && !p.foot_contact[1] && !p.body_contact;
  if (air) {
    if (!in_air) { liftoff_z = ground_z; flight = 0; }
    in_air = true;
    flight += kControlDt;
    longest = max_of(longest, flight);
    rise = max_of(rise, p.position.z - liftoff_z);
    air_apex = max_of(air_apex, p.position.z - z0);
  } else {
    in_air = false;
    ground_z = p.position.z;
  }
}

bool JumpMeter::finish(bool fell, const game::GameWorld::WalkerProbe& p, JumpReport& rep) const {
  const bool standing = !fell && p.foot_contact[0] && p.foot_contact[1] && !p.body_contact && p.gravity.z < -0.9f;
  const f32 tilt = kern::dm::acos(clamp(min_up, -1.0f, 1.0f)) * 57.2957795f;
  ++rep.episodes;
  rep.falls += fell ? 1 : 0;
  rep.height_mm += max_of(0.0f, air_apex) * 1000;
  rep.rise_mm += rise * 1000;
  rep.flight_s += longest;
  rep.tilt_deg += tilt;
  rep.best_height_mm = max_of(rep.best_height_mm, max_of(0.0f, air_apex) * 1000);
  rep.best_flight_s = max_of(rep.best_flight_s, longest);
  const bool real = standing && longest >= 0.06f - 1e-4f && rise >= 0.005f && tilt <= 30.0f;
  rep.jumps += real ? 1 : 0;
  return real;
}

void finish_report(JumpReport& rep) {
  if (!rep.episodes) return;
  const f32 n = static_cast<f32>(rep.episodes);
  rep.height_mm /= n;
  rep.rise_mm /= n;
  rep.flight_s /= n;
  rep.tilt_deg /= n;
}

JumpReport evaluate_jump(ActFn act, void* user, i32 episodes, u64 seed, i32 substeps, bool randomized) {
  JumpReport rep;
  for (i32 e = 0; e < episodes; ++e) {
    DuckEnv env(make_jump_task(), seed + static_cast<u64>(e) * 7919u, substeps > 0 ? substeps : game::kWalkerSubsteps);
    env.task().randomize = randomized;
    if (randomized) env.task().progress(100000);  // the curricula at their end: the full ranges
    env.reset();
    JumpMeter m;
    m.tick(env.probe());
    bool fell = false;
    for (;;) {
      f32 a[kActions];
      act(user, env.observation(), a);
      const Env::Step s = env.step(a);
      m.tick(env.probe());
      if (s.terminal) { fell = true; break; }
      if (s.timeout) break;
    }
    m.finish(fell, env.probe(), rep);
  }
  finish_report(rep);
  return rep;
}

}  // namespace kern::lehrer
