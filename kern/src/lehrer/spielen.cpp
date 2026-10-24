// Spielen (spielen.h): the play network's environment, its demonstration, evaluation and imitation.
#include "kern/lehrer/spielen.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#if !defined(__wasi__)
#include <mutex>
#endif

#include "kern/core/dmath.h"
#include "kern/game/servo.h"
#include "kern/game/walker.h"
#include "kern/gfx/renderer.h"

namespace kern::lehrer {

namespace {

constexpr f32 kS = game::kWalkerScale;
constexpr f32 kPi = 3.14159265f;
constexpr f32 kDecisionDt = kControlDt * game::GameWorld::kPlayDecisionTicks;  // real seconds per decision
constexpr i32 kMaxDecisions = static_cast<i32>(60 / kDecisionDt);  // 60 s of the robot's time
// Where the moves want the object (the demonstration's aim; the network learns its own).
constexpr f32 kKickAhead = 0.09f, kKickLeft = 0.042f, kKickTolerance = 0.02f, kKickBall = 0.035f;  // microduck_ball_kick_env_cfg.py (BALL_POS_NOISE_XY +-1.5 cm; a little more is fine)
constexpr f32 kBallCoastM = -0.005f;  // to the ball it walks right up to the spot (it stops a little short)
// Where the sock should lie for the bite the Lehrer measures itself (bite_spot); how far off it may be
// (the beak is wide: sideways more).
constexpr f32 kSockTolerance = 0.008f, kSockSideTolerance = 0.012f;
constexpr f32 kCoastM = 0.007f;  // told to stop from 0.12 m/s, it still goes a little (kern_lehrer --halt-messen)
constexpr f32 kSettleS = 1.5f;  // told to stop, it has come to rest after this long
constexpr f32 kBackOffM = 0.1f;  // closer than this and crooked: back off and come in again
constexpr f32 kPickOpen = kPlayBeakOpen, kPickClose = kPlayBeakClose;  // the beak opens on the way down, bites at the bottom
constexpr f32 kCarryM = 0.3f;                         // carried this far, it spits the sock out
constexpr f32 kDeliveredM = 0.25f, kKickedM = 0.25f;  // carried this far from where it was picked up; kicked this far

/// -1 before `from`, +1 from `to` on, a straight line between.
f32 ramp(f32 x, f32 from, f32 to) { return clamp(2 * (x - from) / (to - from) - 1, -1.0f, 1.0f); }


}  // namespace

struct PlayEnv::Impl {
  Renderer renderer;
  std::unique_ptr<game::GameWorld> world;
  game::Level base;
  game::ActorId duck = 0, ball = 0, sock = 0;
  std::vector<game::ActorId> sock_parts, other_socks;  // other_socks: a second sock's parts, if any
  f32 ball_radius = 0.035f;
  Vec3 ball_start, sock_start;  // real metres
  bool kick_seen = false, was_holding = false, carrying = false;
  Vec3 carry_from;  // where the sock was picked up
  bool carry_other = false;  // it is the second sock it carries
  bool was_picking = false;  // the pick was running at the last decision
  f32 progress_distance = -1;
  // The demonstration's own memory.
  f32 foot_home[2] = {0, 0};
  bool forced = false;
  f32 forced_at[2] = {0, 0};
  Impl() {
    renderer.set_headless();
    world = std::make_unique<game::GameWorld>(renderer);
  }
  ~Impl() { world.reset(); }
  Vec3 centre(game::ActorId id, const std::vector<game::ActorId>& parts) const {
    Vec3 sum{};
    int n = 0;
    Mat4 m;
    if (parts.empty()) {
      if (world->actor_matrix(id, m)) return m.translation() * (1.0f / kS);
      return {};
    }
    for (game::ActorId p : parts)
      if (world->actor_matrix(p, m)) { sum = sum + m.translation(); ++n; }
    return n ? sum * (1.0f / (kS * static_cast<f32>(n))) : Vec3{};
  }
};

PlayEnv::PlayEnv(u64 seed, bool randomize) : impl_(std::make_unique<Impl>()), rng_(seed), randomize_(randomize) {
  game::Level& l = impl_->base;
  l.name = "Spielen";
  l.world.step_hz = game::kWalkerStepHz;
  l.world.ground = true;
  l.world.ground_friction = 1.0f;
  l.world.ground_surface = "floor";
  impl_->duck = game::add_micro_duck(l, {0, 0, 0});
  game::ControllerComponent& c = *l.find(impl_->duck)->controller;
  c.autonomous = true;   // the play layer works where the autonomy works
  c.kern_extras = true;  // Kern's play layer on
  l.world.player = 0;
  obs_.assign(kPlayObs, 0.0f);
  critic_.assign(static_cast<std::size_t>(kPlayObs + kPrivileged), 0.0f);
}

PlayEnv::~PlayEnv() = default;

void PlayEnv::reset() {
  Impl& m = *impl_;
  game::Level l = m.base;
  game::Actor* trunk = l.find(m.duck);
  const f32 yaw = m.forced ? 0.0f : rng_.uniform(-180, 180);
  trunk->transform.rotation = m.forced ? Vec3{0, 0, 0} : Vec3{rng_.uniform(-2, 2), rng_.uniform(-2, 2), yaw};
  // What lies on the floor: a ball (30 %), a sock (30 %) or both (40 %, as in the example level,
  // where they lie close together and the ball now and then on the way to the sock or behind it);
  // 0.35 .. 1.2 m away, now and then right by it (it starts standing in front of it), mostly in front
  // (the camera sees it at once), now and then beside or behind it (it has to look). Now and then a
  // second sock lies about (the example level has two).
  const f32 kind = rng_.uniform();
  outcome_ = Outcome{};
  outcome_.has_ball = !m.forced && (kind < 0.3f || kind >= 0.6f);
  outcome_.has_sock = m.forced || kind >= 0.3f;
  auto place = [&](f32 min_d) {
    const f32 d = rng_.uniform() < 0.2f ? rng_.uniform(0.12f, 0.35f) : rng_.uniform(min_d, 1.2f);
    const f32 spread = rng_.uniform() < 0.8f ? 0.9f : kPi;
    const f32 a = yaw * kPi / 180 + rng_.uniform(-spread, spread);
    return Vec3{d * kern::dm::cos(a), d * kern::dm::sin(a), 0};
  };
  m.ball = m.sock = 0;
  m.sock_parts.clear();
  m.other_socks.clear();
  auto add_sock = [&](Vec3 at, std::vector<game::ActorId>& ids) {
    std::vector<game::Actor> parts = game::micro_duck_sock_actors();
    const game::ActorId first = l.next_id;
    for (std::size_t i = 0; i < parts.size(); ++i) {
      game::Actor a = parts[i];
      a.id = first + static_cast<game::ActorId>(i);
      if (i == 0) a.transform.position = a.transform.position + at * kS;
      if (a.parent) a.parent = first + a.parent - 1;
      for (game::JointComponent& j : a.joints)
        if (j.target) j.target = first + j.target - 1;
      ids.push_back(a.id);
      l.add(std::move(a));
    }
    return first;
  };
  Vec3 sock_at{};
  if (outcome_.has_sock) {
    sock_at = place(0.35f);
    if (m.forced) sock_at = {m.forced_at[0], m.forced_at[1], 0};
    m.sock = add_sock(sock_at, m.sock_parts);
  }
  if (outcome_.has_ball) {
    m.ball_radius = rng_.uniform(0.035f, 0.06f);
    Vec3 at = place(0.35f);
    if (outcome_.has_sock) {
      const f32 u = rng_.uniform();
      if (u < 0.4f) {  // on the way to the sock
        const f32 f = rng_.uniform(0.3f, 0.8f), off = rng_.uniform(-0.12f, 0.12f);
        const f32 n = max_of(kern::dm::hypot(sock_at.x, sock_at.y), 1e-3f);
        at = Vec3{sock_at.x * f - sock_at.y / n * off, sock_at.y * f + sock_at.x / n * off, 0};
      } else if (u < 0.7f) {  // close by the sock, any side
        const f32 a = rng_.uniform(-kPi, kPi), d = rng_.uniform(0.15f, 0.4f);
        at = Vec3{sock_at.x + d * kern::dm::cos(a), sock_at.y + d * kern::dm::sin(a), 0};
      }
      if (kern::dm::hypot(at.x - sock_at.x, at.y - sock_at.y) < 0.12f) at = Vec3{-sock_at.x, -sock_at.y, 0};
    }
    if (kern::dm::hypot(at.x, at.y) < 0.12f) at = Vec3{-sock_at.x, -sock_at.y, 0};  // not under its feet
    game::Actor ball;
    ball.name = "Ball";
    ball.tags = {"ball"};
    ball.transform.position = Vec3{at.x, at.y, m.ball_radius} * kS;
    ball.mesh = game::MeshComponent{};
    ball.mesh->shape = game::Shape::sphere;
    ball.mesh->radius = m.ball_radius * kS;
    ball.body = game::BodyComponent{};
    ball.body->mass = rng_.uniform(0.015f, 0.05f) * kS * kS * kS;
    ball.mesh->rolling_resistance = rng_.uniform(0.01f, 0.05f);  // a toy ball on a floor rolls out (foam .. rubber)
    m.ball = l.add(ball).id;
    m.ball_start = {at.x, at.y, m.ball_radius};
  }
  if (outcome_.has_sock && !m.forced && rng_.uniform() < 0.3f) {  // a second sock somewhere else
    for (i32 tries = 0; tries < 8; ++tries) {
      const Vec3 at = place(0.35f);
      const bool clear = kern::dm::hypot(at.x - sock_at.x, at.y - sock_at.y) > 0.3f &&
                         (!m.ball || kern::dm::hypot(at.x - m.ball_start.x, at.y - m.ball_start.y) > 0.15f) && kern::dm::hypot(at.x, at.y) > 0.15f;
      if (clear) { add_sock(at, m.other_socks); break; }
    }
  }
  m.forced = false;
  if (randomize_) {
    for (game::Actor& a : l.actors)
      for (game::ColliderComponent& c : a.colliders)
        if (c.impact_sound == "builtin:plastic_step") c.friction *= rng_.uniform(0.7f, 1.3f);  // the soles
    game::DuckVariation v;
    v.trunk_mass = rng_.uniform(0.95f, 1.05f);
    v.trunk_com = {rng_.uniform(-0.005f, 0.005f), rng_.uniform(-0.005f, 0.005f), rng_.uniform(-0.005f, 0.005f)};
    v.head_com = {rng_.uniform(-0.003f, 0.003f), rng_.uniform(-0.003f, 0.003f), rng_.uniform(-0.003f, 0.003f)};
    game::vary_micro_duck(l, m.duck, v);
    const game::ServoProfile servo = game::xl330_profile();
    for (i32 link = 1; link <= kActions + 1; ++link) {
      game::ServoProfile p = servo;
      p.friction *= rng_.uniform(1 - servo.spread_friction, 1 + servo.spread_friction);
      p.armature *= rng_.uniform(1 - servo.spread_armature, 1 + servo.spread_armature);
      game::set_micro_duck_servo(l, m.duck, link, p);
    }
    l.find(m.duck)->controller->servo_delay = rng_.uniform(servo.delay_min, servo.delay_max);
  }
  game::GameWorld& w = *m.world;
  w.start(l, game::SimMode::simulate);
  w.set_walker_play_external(m.duck, true);
  game::GameWorld::DetectorModel det;
  if (randomize_) {
    det.sigma_abs = rng_.uniform(0.0f, 0.004f);
    det.sigma_rel = rng_.uniform(0.0f, 0.03f);
    det.dropout = rng_.uniform(0.0f, 0.2f);
  }
  det.seed = rng_.next();
  w.set_walker_detector(m.duck, det);
  if (m.sock) m.sock_start = m.centre(m.sock, m.sock_parts);
  i32 guard = 0;
  while (!w.walker_play_due(m.duck) && ++guard < 64) w.step_once();
  decisions_ = 0;
  m.kick_seen = m.was_holding = m.carrying = false;
  m.progress_distance = -1;
  m.was_picking = false;
  observe();
  m.foot_home[0] = obs_[8];
  m.foot_home[1] = obs_[9];
}

f32 PlayEnv::sock_moved() const {
  const Impl& m = *impl_;
  if (!m.sock) return 0;
  const Vec3 c = m.centre(m.sock, m.sock_parts);
  return kern::dm::hypot(c.x - m.sock_start.x, c.y - m.sock_start.y);
}

const game::GameWorld& PlayEnv::world() const { return *impl_->world; }
game::ActorId PlayEnv::duck() const { return impl_->duck; }

void PlayEnv::place_sock_next(f32 ahead, f32 left) {
  impl_->forced = true;
  impl_->forced_at[0] = ahead;
  impl_->forced_at[1] = left;
}

BiteSpot measure_bite(f32 open, f32 close, bool print, const ParallelFor& parallel) {
  // Where the beak gets a sock: the duck walks a little (the stance after walking, not the spawn's),
  // stands, the sock (lying as a heap) is put down in front of it, and the pick runs with the beak
  // open over [open, close) of its phase. A grid of places; per place: did it come up, and is it
  // still in the beak after 3 s of carrying it off?
  constexpr i32 kLeft = 11, kAhead = 9;
  std::vector<i32> held(kLeft * kAhead), kept(kLeft * kAhead);
  auto at_left = [](i32 i) { return -0.02f + 0.005f * static_cast<f32>(i); };
  auto at_ahead = [](i32 j) { return 0.045f + 0.004f * static_cast<f32>(j); };
  const ParallelFor run = parallel ? parallel : ParallelFor([](i32 n, const std::function<void(i32)>& fn) { for (i32 i = 0; i < n; ++i) fn(i); });
  run(kLeft * kAhead, [&](i32 cell) {
    const f32 left = at_left(cell / kAhead), ahead = at_ahead(cell % kAhead);
    PlayEnv env(1, false);
    env.place_sock_next(-1.0f, 1.0f);  // out of the way while it walks
    env.reset();
    PlayEnv::Impl& m = *env.impl_;
    f32 a[kPlayActions] = {0, 0, 0, 0, 0, -1, -1, -1, 0};
    a[0] = 1;
    for (i32 i = 0; i < 25; ++i) env.step(a);
    a[0] = 0.4f;
    for (i32 i = 0; i < 25; ++i) env.step(a);
    a[0] = 0;
    for (i32 i = 0; i < 40; ++i) env.step(a);
    game::GameWorld::WalkerProbe wp;
    m.world->walker_probe(m.duck, wp);
    const Vec3 f = wp.rotation.rotate({1, 0, 0});
    const f32 h = kern::dm::atan2(f.y, f.x), c = kern::dm::cos(h), sn = kern::dm::sin(h);
    const Vec3 centre = m.centre(m.sock, m.sock_parts) * kS;
    const Vec3 at = Vec3{wp.position.x + c * ahead - sn * left, wp.position.y + sn * ahead + c * left, 0} * kS;
    for (game::ActorId id : m.sock_parts) {  // the heap as it lies, its centre at `at`
      Mat4 mp;
      m.world->actor_matrix(id, mp);
      m.world->teleport(id, Vec3{at.x + mp.translation().x - centre.x, at.y + mp.translation().y - centre.y, mp.translation().z});
      m.world->set_actor_velocity(id, {});
    }
    for (i32 i = 0; i < 25; ++i) env.step(a);  // stand a second
    for (i32 i = 0; i < 200; ++i) {
      const f32* o = env.observation();
      a[7] = i == 0 ? 1.0f : -1.0f;
      a[5] = o[13] > 0.5f && o[15] >= open && o[15] < close ? 1.0f : -1.0f;
      env.step(a);
      if (i > 20 && env.observation()[13] < 0.5f) break;
    }
    const bool got = env.observation()[10] > 0.5f;
    bool still = false;
    if (got) {
      a[5] = a[7] = -1;
      a[4] = -0.1f;
      i32 k = 0;
      for (; k < 75 && env.observation()[10] > 0.5f; ++k) {
        a[0] = k < 12 ? 1.0f : 0.5f;
        env.step(a);
      }
      still = k == 75;
    }
    held[static_cast<std::size_t>(cell)] = got;
    kept[static_cast<std::size_t>(cell)] = still;
  });
  // The best place: kept, with the most kept places around it (the walk-up is never exact).
  BiteSpot best;
  i32 best_score = -1;
  for (i32 cell = 0; cell < kLeft * kAhead; ++cell) {
    if (!kept[static_cast<std::size_t>(cell)]) continue;
    const i32 li = cell / kAhead, aj = cell % kAhead;
    i32 score = 0;
    for (i32 dl = -2; dl <= 2; ++dl)
      for (i32 da = -1; da <= 1; ++da) {
        const i32 l2 = li + dl, a2 = aj + da;
        if (l2 >= 0 && l2 < kLeft && a2 >= 0 && a2 < kAhead) score += kept[static_cast<std::size_t>(l2 * kAhead + a2)];
      }
    if (score > best_score) { best_score = score; best = {at_ahead(aj), at_left(li), score}; }
  }
  if (print) {
    std::printf("sock centre (ahead, left, real m): held at the end of the pick / still held after carrying 3 s\n");
    for (i32 cell = 0; cell < kLeft * kAhead; ++cell)
      std::printf("%+.3f %+.3f  %d %d\n", at_ahead(cell % kAhead), at_left(cell / kAhead), held[static_cast<std::size_t>(cell)], kept[static_cast<std::size_t>(cell)]);
    std::printf("best: %+.3f ahead, %+.3f left (%d kept around it)\n", best.ahead, best.left, best.kept);
  }
  return best;
}

BiteSpot bite_spot(const ParallelFor& parallel) {
  static BiteSpot spot;
  auto measure = [&] {
    spot = measure_bite(kPlayBeakOpen, kPlayBeakClose, false, parallel);
    if (spot.kept == 0) spot = {0.065f, 0.0f, 0};  // nothing held anywhere: in front of the beak
  };
#if defined(__wasi__)  // one thread
  static bool measured = false;
  if (!measured) { measured = true; measure(); }
#else
  static std::once_flag once;
  std::call_once(once, measure);
#endif
  return spot;
}

void measure_stop() {
  // Walking steadily at v, then told to stop: how far it still goes, how far it ends up sideways.
  for (f32 v : {0.1f, 0.12f, 0.15f, 0.2f})
    for (i32 r = 0; r < 4; ++r) {
      PlayEnv env(17 + static_cast<u64>(r), false);
      env.place_sock_next(3.0f, 2.0f);
      env.reset();
      PlayEnv::Impl& m = *env.impl_;
      f32 a[kPlayActions] = {0, 0, 0, 0, 0, -1, -1, -1, 0};
      a[0] = 1;
      for (i32 i = 0; i < 25; ++i) env.step(a);  // get going
      a[0] = v / 0.3f;
      for (i32 i = 0; i < 50 + r * 3; ++i) env.step(a);
      game::GameWorld::WalkerProbe p0, p1;
      m.world->walker_probe(m.duck, p0);
      a[0] = 0;
      for (i32 i = 0; i < 50; ++i) env.step(a);
      m.world->walker_probe(m.duck, p1);
      const Vec3 f = p0.rotation.rotate({1, 0, 0});
      const f32 h = kern::dm::atan2(f.y, f.x);
      const f32 dx = p1.position.x - p0.position.x, dy = p1.position.y - p0.position.y;
      std::printf("v %.2f m/s (moving %.3f): after the stop %+.4f m on, %+.4f m sideways\n", v, kern::dm::hypot(p0.velocity.x, p0.velocity.y),
                  kern::dm::cos(h) * dx + kern::dm::sin(h) * dy, -kern::dm::sin(h) * dx + kern::dm::cos(h) * dy);
      std::fflush(stdout);
    }
}

void PlayEnv::observe() {
  Impl& m = *impl_;
  m.world->walker_play_observation(m.duck, obs_.data());
  std::memcpy(critic_.data(), obs_.data(), sizeof(f32) * kPlayObs);
  game::GameWorld::WalkerProbe p;
  m.world->walker_probe(m.duck, p);
  const Vec3 fwd = p.rotation.rotate({1, 0, 0});
  const f32 h = kern::dm::atan2(fwd.y, fwd.x), c = kern::dm::cos(h), s = kern::dm::sin(h);
  f32* q = critic_.data() + kPlayObs;
  std::fill(q, q + kPrivileged, 0.0f);
  if (m.ball) {
    const Vec3 b = m.centre(m.ball, {});
    q[0] = 1;
    q[1] = c * (b.x - p.position.x) + s * (b.y - p.position.y);
    q[2] = -s * (b.x - p.position.x) + c * (b.y - p.position.y);
  }
  if (m.sock) {
    const Vec3 b = m.centre(m.sock, m.sock_parts);
    q[3] = 1;
    q[4] = c * (b.x - p.position.x) + s * (b.y - p.position.y);
    q[5] = -s * (b.x - p.position.x) + c * (b.y - p.position.y);
  }
}

Env::Step PlayEnv::step(const f32* action) {
  Impl& m = *impl_;
  game::GameWorld& w = *m.world;
  f32 a[kPlayActions];
  for (i32 i = 0; i < kPlayActions; ++i) a[i] = std::isfinite(action[i]) ? action[i] : 0.0f;
  w.set_walker_play_action(m.duck, a);
  // Until the next decision.
  i32 guard = 0;
  do {
    w.step_once();
    if (w.walker_driving(m.duck) == "kick_left") m.kick_seen = true;
  } while (!w.walker_play_due(m.duck) && ++guard < 64);
  ++decisions_;
  observe();
  Step r;
  game::GameWorld::WalkerProbe p;
  w.walker_probe(m.duck, p);
  const bool holding = obs_[10] > 0.5f;
  r.reward = -0.01f;
  // Toward what it is after (the sock first, else the ball): shaping by the true distance.
  const f32* q = critic_.data() + kPlayObs;
  f32 dist = -1;
  if (holding) dist = -1;
  else if (q[3] > 0 && !outcome_.delivered) dist = kern::dm::hypot(q[4], q[5]);
  else if (q[0] > 0 && !outcome_.kicked) dist = kern::dm::hypot(q[1], q[2]);
  if (dist >= 0 && m.progress_distance >= 0) r.reward += 2 * (m.progress_distance - dist);
  m.progress_distance = dist;
  // Picked up: still in the beak once the pick is over (the beak shuts on a lump now and then and
  // loses it on the way up - that is no pick).
  const bool picking = obs_[13] > 0.5f;
  if (picking && !m.was_picking) ++outcome_.pick_attempts;
  if (!picking && m.was_picking && holding) ++outcome_.picks_held;
  m.was_picking = picking;
  if (m.sock && holding && !picking && !m.carrying) {
    m.carrying = true;
    // Which sock: the one by its beak.
    const Vec3 a = m.centre(m.sock, m.sock_parts), b = m.other_socks.empty() ? a : m.centre(m.other_socks[0], m.other_socks);
    const Vec3 at = p.position;
    m.carry_other = kern::dm::hypot(b.x - at.x, b.y - at.y) < kern::dm::hypot(a.x - at.x, a.y - at.y);
    m.carry_from = m.carry_other ? b : a;
    if (!outcome_.picked) { outcome_.picked = true; r.reward += 3; }
  }
  if (holding && m.carrying) r.reward += 0.5f * kDecisionDt * min_of(obs_[19], kCarryM);  // carrying it off
  m.was_holding = holding;
  if (m.ball && m.kick_seen && !outcome_.kicked) {
    const Vec3 b = m.centre(m.ball, {});
    if (kern::dm::hypot(b.x - m.ball_start.x, b.y - m.ball_start.y) >= kKickedM) { outcome_.kicked = true; r.reward += 10; }
  }
  if (m.carrying && !holding) {  // out of the beak: put down far enough from where it was picked up?
    const Vec3 c = m.carry_other ? m.centre(m.other_socks[0], m.other_socks) : m.centre(m.sock, m.sock_parts);
    if (kern::dm::hypot(c.x - m.carry_from.x, c.y - m.carry_from.y) >= kDeliveredM) { outcome_.delivered = true; r.reward += 10; }
    m.carrying = false;
  }
  if (p.gravity.z > -0.3f) { outcome_.fell = true; r.terminal = true; r.reward -= 5; }
  const bool done = outcome_.kicked || outcome_.delivered;
  r.timeout = !r.terminal && (done || decisions_ >= kMaxDecisions);
  outcome_.seconds = static_cast<f32>(decisions_) * kDecisionDt;
  return r;
}

void PlayEnv::teacher(f32 out[kPlayActions]) {
  Impl& m = *impl_;
  const f32* o = obs_.data();
  std::fill(out, out + kPlayActions, 0.0f);
  out[5] = -1;  // beak shut
  out[6] = out[7] = -1;
  const bool holding = o[10] > 0.5f, picking = o[13] > 0.5f, kicking = o[14] > 0.5f;
  if (kicking) return;
  if (picking) {  // the beak: open on the way down, bite at the bottom
    const f32 phase = o[15];
    out[5] = phase >= kPickOpen && phase < kPickClose ? 1.0f : -1.0f;
    return;
  }
  if (holding) {  // off with it, slowly (the beak's grip is light), head up; carried far enough, spit it out
    if (o[19] >= kCarryM) out[5] = 1;
    else {
      out[0] = o[22] < 0.5f ? 1.0f : 0.5f;  // a firm first step (from a stand a slow one wobbles), then slowly
      out[4] = -0.1f;
    }
    return;
  }
  // It goes by what its camera's detector and tracker report (the observation), as the network
  // will: then the network can learn exactly this. The nearer of the two (a ball in the way gets
  // kicked away first, it does not walk over it); nothing known: it turns on the spot and looks.
  const bool sock_known = o[4] > 0.5f, ball_known = o[0] > 0.5f;
  const bool ball_nearer = ball_known && (!sock_known || kern::dm::hypot(o[1], o[2]) < kern::dm::hypot(o[5], o[6]));
  const bool go_ball = ball_nearer, go_sock = sock_known && !go_ball;
  if (!go_sock && !go_ball) {  // it turns on the spot, the head at home (bowed or swung out while it turns, it tips over)
    out[2] = 0.8f;
    return;
  }
  const f32 tx = go_sock ? o[5] : o[1], ty = go_sock ? o[6] : o[2];  // where it is (heading frame)
  const BiteSpot bite = bite_spot();
  f32 want_ahead = go_sock ? bite.ahead : kKickAhead + (m.ball_radius - kKickBall);
  f32 want_left = go_sock ? bite.left : kKickLeft;
  if (go_ball) {  // the kick is trained from the home stance: measure from the foot
    want_ahead += o[8] - m.foot_home[0];
    want_left += o[9] - m.foot_home[1];
  }
  const f32 dist = kern::dm::hypot(tx, ty), bearing = kern::dm::atan2(ty, tx);
  const f32 ahead = tx - want_ahead, side = ty - want_left;
  // Keep it in the picture and look at it: the head pitches down (more, the closer it gets) and turns
  // to it - a little: bowed far while it walks fast and turns, it tips over.
  out[4] = clamp(kern::dm::atan2(0.12f, max_of(dist, 0.05f)) - 0.2f, 0.1f, 0.5f) / 1.1f;
  if (dist > 0.25f) {
    out[3] = clamp(0.5f * bearing, -0.3f, 0.3f) / 1.1f;  // not far: turning with the head swung out tips it over
    const f32 vx = std::fabs(bearing) > 0.6f ? 0.0f : clamp(dist * 1.2f, 0.15f, 0.3f);
    out[0] = vx / 0.3f;
    if (vx == 0) out[3] = out[4] = 0;  // turning on the spot: the head at home (else it tips over)
    // Not faster than 0.6 rad/s on the spot, 0.4 rad/s while it walks: faster tips it over.
    out[2] = clamp(bearing * 1.5f, vx > 0 ? -0.4f : -0.6f, vx > 0 ? 0.4f : 0.6f);
    return;
  }
  // Close by, the same for both: the gait can only be stopped, not inched (below about 0.12 m/s from
  // a stand it does not step at all), and a foot that swings into a sock shoves it away. So it walks
  // up on a straight line at a slow steady pace, steering (not side-stepping) so the target stays on
  // its line, says stop the coasting distance before the spot, stands, and starts the move once it
  // has stood long enough; came in too far or crooked, it backs off and comes in again. Every answer
  // follows from what it sees now and how long it has stood (no hidden memory: the network learns
  // exactly this).
  const f32 tol = go_sock ? kSockTolerance : kKickTolerance, side_tol = go_sock ? kSockSideTolerance : kKickTolerance;
  out[4] = dist < 0.15f ? 0.0f : min_of(out[4], 0.3f / 1.1f);  // close by the beak would brush it
  const f32 steer = clamp(2.0f * side / max_of(tx, 0.05f), -0.4f, 0.4f);
  const bool in_place = std::fabs(ahead) <= tol && std::fabs(side) <= side_tol;
  // Standing: both soles down longer than a step's double support lasts.
  const bool standing = o[21] > 0.25f && ahead <= kBackOffM, backing = o[22] < 0.0f;
  auto back_off = [&] {
    out[0] = -1.0f;  // back at full speed: slower, a stiff duck does not step at all
    out[2] = steer;
  };
  // Standing on the spot it goes on standing, then starts the move - whatever its walking command did
  // a moment ago (a twitch backwards is no reason to back off).
  if (!(standing && in_place) && backing && ahead <= kBackOffM) return back_off();  // until it has room again
  if (standing) {  // stopped by the spot: stand still, then pick - or, not right, back off and come in again
    out[4] = 0;
    if (!in_place) {
      if (o[21] >= kSettleS) back_off();
      return;
    }
  } else {
    if (ahead < -tol) return back_off();  // too far in
    if (ahead > (go_sock ? kCoastM : kBallCoastM)) {  // on its way: slowly (it coasts the rest when told to stop)
      f32 vx = ahead > 0.06f ? 0.15f : 0.12f;
      if (o[22] < 0.5f) vx = 0.3f;  // from a stand the gait needs a firm push
      out[0] = vx / 0.3f;
      out[2] = std::fabs(steer) < 0.04f ? 0.0f : steer;
      return;
    }
    out[4] = 0;
    return;  // stop here
  }
  // There: head up (the moves were trained with the head at home), stand, then go. The move's wish
  // rises over the last half second (a ramp, not one decision's pulse: the network learns it from
  // how long it has stood).
  out[go_sock ? 7 : 6] = ramp(o[21], kSettleS - 0.5f, kSettleS);
}

std::vector<std::pair<std::string, std::string>> play_metadata() {
  return {
      {"task", "play"},
      {"lehrer", "Kern (C++ DAgger + PPO on Box3D)"},
      {"rate_hz", "25"},
      {"observation_names",
       "ball_known,ball_ahead,ball_left,ball_age,sock_known,sock_ahead,sock_left,sock_age,left_foot_ahead,left_foot_left,holding,beak_open,"
       "walking,picking,kicking,pick_phase,gravity_x,gravity_y,gravity_z,carried_m,carried_s,still_s,walking_s"},
      {"action_names", "forward,strafe,turn,head_yaw,head_pitch_down,beak_open,kick_left,ground_pick,neck_bow"},
  };
}

std::unique_ptr<Ppo> make_play_learner(u64 seed) {
  PpoConfig c;
  c.actor_hidden = {256, 128};
  c.critic_hidden = {256, 128};
  c.init_std = 0.2f;
  c.learning_rate = 1e-4f;
  c.entropy_coef = 0.0f;
  c.steps_per_env = 64;
  c.seed = seed;
  return std::make_unique<Ppo>(c, kPlayObs, kPlayObs + PlayEnv::kPrivileged, kPlayActions);
}

PlayReport evaluate_play(PlayActFn act, void* user, i32 episodes, u64 seed, bool randomized, const ParallelFor& parallel) {
  bite_spot(parallel);  // measured before the episodes need it
  const ParallelFor run = parallel ? parallel : ParallelFor([](i32 n, const std::function<void(i32)>& fn) { for (i32 i = 0; i < n; ++i) fn(i); });
  std::vector<PlayEnv::Outcome> outs(static_cast<std::size_t>(episodes));
  run(episodes, [&](i32 e) {
    PlayEnv env(seed + static_cast<u64>(e) * 7919u, randomized);
    env.reset();
    for (;;) {
      f32 a[kPlayActions];
      if (act) act(user, env.observation(), a);
      else env.teacher(a);
      const Env::Step s = env.step(a);
      if (s.terminal || s.timeout) break;
    }
    outs[static_cast<std::size_t>(e)] = env.outcome();
  });
  PlayReport r;
  for (const PlayEnv::Outcome& o : outs) {
    ++r.episodes;
    r.falls += o.fell ? 1 : 0;
    r.pick_attempts += o.pick_attempts;
    r.picks_held += o.picks_held;
    if (o.has_sock) {
      ++r.sock_episodes;
      r.picked += o.picked ? 1 : 0;
      if (o.delivered) { ++r.delivered; r.sock_seconds += o.seconds; }
    } else if (o.has_ball) {
      ++r.ball_episodes;
      if (o.kicked) { ++r.kicked; r.ball_seconds += o.seconds; }
    }
  }
  if (r.kicked) r.ball_seconds /= static_cast<f32>(r.kicked);
  if (r.delivered) r.sock_seconds /= static_cast<f32>(r.delivered);
  return r;
}

PlayImitationReport imitate_play(Ppo& learner, i32 rounds, i32 episodes, u64 seed, const ParallelFor& parallel,
                                 const std::function<void(i32, const PlayImitationReport&)>& progress) {
  const ParallelFor run = parallel ? parallel : ParallelFor([](i32 n, const std::function<void(i32)>& fn) { for (i32 i = 0; i < n; ++i) fn(i); });
  bite_spot(parallel);  // measured before the episodes need it
  std::vector<f32> all_obs, all_act;
  PlayImitationReport rep;
  for (i32 round = 0; round < rounds; ++round) {
    std::vector<std::vector<f32>> obs(static_cast<std::size_t>(episodes)), act(static_cast<std::size_t>(episodes));
    const Policy student = learner.policy();
    run(episodes, [&](i32 e) {
      PlayEnv env(seed * 1000003ull + static_cast<u64>(round) * 7919u + static_cast<u64>(e), e % 2 == 1);  // half on a clean robot, half varied
      env.reset();
      Rng noise(seed * 31 + static_cast<u64>(round) * 1000003u + static_cast<u64>(e));
      for (;;) {
        f32 label[kPlayActions], a[kPlayActions];
        env.teacher(label);
        auto& o = obs[static_cast<std::size_t>(e)];
        o.insert(o.end(), env.observation(), env.observation() + kPlayObs);
        act[static_cast<std::size_t>(e)].insert(act[static_cast<std::size_t>(e)].end(), label, label + kPlayActions);
        if (round == 0) std::memcpy(a, label, sizeof a);
        else {
          student.act(env.observation(), a);
          for (f32& x : a) x += 0.05f * noise.normal();
        }
        const Env::Step s = env.step(a);
        if (s.terminal || s.timeout) break;
      }
    });
    for (i32 e = 0; e < episodes; ++e) {
      all_obs.insert(all_obs.end(), obs[static_cast<std::size_t>(e)].begin(), obs[static_cast<std::size_t>(e)].end());
      all_act.insert(all_act.end(), act[static_cast<std::size_t>(e)].begin(), act[static_cast<std::size_t>(e)].end());
    }
    learner.set_parallel(parallel);
    rep.rounds = round + 1;
    rep.samples = static_cast<i32>(all_act.size() / kPlayActions);
    rep.mse = learner.imitate(all_obs, all_act, 30, 256, 1e-3f, seed + static_cast<u64>(round));
    if (progress) {
      rep.student = evaluate_play([](void* u, const f32* o, f32* a) { static_cast<Ppo*>(u)->act(o, a); }, &learner, 16, 99991, false, parallel);
      progress(round, rep);
    }
  }
  return rep;
}

}  // namespace kern::lehrer
