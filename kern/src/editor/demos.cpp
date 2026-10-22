// Example scenes (File → Examples): the four Editor3D demos, ported exactly,
// plus a small game that uses the game components.
#include "internal.h"

namespace kern::editor {

void Editor::load_demo(std::string_view kind, bool reuse) {
  const std::string name = kind == "hulls" ? "3D hulls & stacking" : kind == "joints" ? "Joint laboratory" : kind == "cradle" ? "Newton cradle"
                           : kind == "game" ? "Roll & Collect" : "Suspension test";
  if (!reuse) add_document(new_scene(name));
  Document* d = doc();
  world::Scene& s = d->scene;
  s.bodies.clear();
  s.joints.clear();
  s.images.clear();
  s.samplers.clear();
  auto mk = [&](const std::string& n, BodyKind type, const D3& p, FixtureShape shape, std::vector<f64> size = {}) -> Body& {
    Body b = new_body(type, p, n);
    Fixture f = new_fixture(shape, "fixture" + std::to_string(s.fixture_count()));
    if (shape == FixtureShape::box) f.size = size.size() == 3 ? D3{size[0], size[1], size[2]} : D3{1, 1, 1};
    if (shape == FixtureShape::sphere) f.radius = !size.empty() && size[0] != 0 ? size[0] : 0.5;
    if (shape == FixtureShape::capsule) {
      f.radius = !size.empty() && size[0] != 0 ? size[0] : 0.3;
      f.length = size.size() > 1 && size[1] != 0 ? size[1] : 1;
    }
    b.fixtures = {f};
    s.bodies.push_back(std::move(b));
    return s.bodies.back();
  };
  auto jn = [&](JointKind type, const std::string& a_id, const std::string& b_id, const D3& p) -> Joint& {
    const Body* a = s.body(a_id);
    const Body* b = s.body(b_id);
    Joint j;
    j.id = make_uid();
    j.name = std::string(joint_kind_name(type)) + "joint" + std::to_string(s.joints.size());
    j.type = type;
    j.body_a = a_id;
    j.body_b = b_id;
    j.anchor_a = world_to_local(a->pose(), p);
    j.anchor_b = world_to_local(b->pose(), p);
    j.axis_a = {1, 0, 0};
    j.axis_b = D3{1, 0, 0};
    j.lower_limit = -0.7;
    j.upper_limit = 0.7;
    j.motor_speed = 1;
    j.max_motor_torque = 100;
    j.max_motor_force = 200;
    j.length = std::max(0.2, length(sub(a->position, b->position)));
    j.spring_frequency = 4;
    j.spring_damping_ratio = 0.7;
    j.frequency = 5;
    j.damping_ratio = 1;
    s.joints.push_back(std::move(j));
    return s.joints.back();
  };
  std::string message;
  if (kind != "cradle" && kind != "game") mk("ground", BodyKind::fixed, {0, 0, -0.5}, FixtureShape::box, {24, 24, 1});
  if (kind == "joints") {
    s.world.gravity = {0, 0, -5};
    for (u32 k = 0; k < kJointKindCount; ++k) {
      const JointKind t = static_cast<JointKind>(k);
      const std::string tn = joint_kind_name(t);
      const f64 x = (k % 4) * 5.0 - 7.5, y = std::floor(k / 4.0) * 5 - 2.5;
      const std::string a = mk(tn + " anchor", BodyKind::fixed, {x, y, 4}, FixtureShape::box, {0.4, 0.4, 0.4}).id;
      const std::string b = mk(tn + " body", BodyKind::dynamic, {x, y, 2}, FixtureShape::box, {0.7, 0.7, 1.5}).id;
      Joint& j = jn(t, a, b, {x, y, 3.8});
      if (t == JointKind::revolute || t == JointKind::prismatic) { j.enable_limit = true; j.lower_limit = -0.8; j.upper_limit = 0.8; }
      if (t == JointKind::revolute || t == JointKind::wheel) j.enable_motor = true;
    }
  } else if (kind == "hulls") {
    for (int z = 0; z < 5; ++z)
      for (int x = 0; x < 5 - z; ++x)
        mk("crate " + std::to_string(z) + "-" + std::to_string(x), BodyKind::dynamic, {-2 + x * 1.05 + z * 0.5, 0, 0.55 + z * 1.05}, FixtureShape::box, {1, 1, 1});
    Body& h = mk("editable hull", BodyKind::dynamic, {4, 0, 2}, FixtureShape::hull);
    h.fixtures[0].vertices = {{-1, -1, -1}, {1, -1, -1}, {1, 1, -1}, {-1, 1, -1}, {0, 0, 1.6}};
  } else if (kind == "cradle") {
    s.world.gravity = {0, 0, -9.80665};
    s.world.allow_sleep = false;
    s.world.continuous_physics = true;
    s.world.steps_per_second = 120;
    s.world.sub_step_count = 12;
    s.world.restitution_threshold = 0.01;
    s.world.restitution_iterations = 12;
    s.world.restitution_propagation = true;
    const f64 top_z = 0.34, drop = 0.20, radius = 0.025, spacing = 2 * radius, wire = 0.018, steel_density = 7850;
    const i32 count = 5;
    const std::string steel = "#66727d", base = "#4e5964", ball_color = "#d9a0aa";
    auto part = [&](const std::string& n, const D3& p, std::vector<f64> size, const std::string& color) {
      Body& b = mk(n, BodyKind::fixed, p, FixtureShape::box, std::move(size));
      b.fixtures[0].color = color;
    };
    part("top beam", {0, 0, top_z + 0.018}, {0.38, 0.055, 0.025}, steel);
    part("left base rail", {0, -0.085, 0.012}, {0.42, 0.025, 0.024}, base);
    part("right base rail", {0, 0.085, 0.012}, {0.42, 0.025, 0.024}, base);
    for (f64 x : {-0.19, 0.19}) {
      std::string xs;
      json::append_number(xs, x);
      part("frame post L " + xs, {x, -0.085, 0.17}, {0.018, 0.018, 0.32}, steel);
      part("frame post R " + xs, {x, 0.085, 0.17}, {0.018, 0.018, 0.32}, steel);
      part("frame cross " + xs, {x, 0, 0.025}, {0.024, 0.19, 0.018}, base);
      part("frame shoulder " + xs, {x, 0, top_z}, {0.018, 0.19, 0.018}, steel);
    }
    const f64 wire_len = std::sqrt(drop * drop + wire * wire);
    for (i32 i = 0; i < count; ++i) {
      const f64 rest_x = (i - (count - 1) / 2.0) * spacing, z = top_z - drop;
      Body& ball = mk("cradle ball " + std::to_string(i + 1), BodyKind::dynamic, {rest_x, 0, z}, FixtureShape::sphere, {radius});
      Fixture& f = ball.fixtures[0];
      f.density = steel_density;
      f.friction = 0.005;
      f.restitution = 0.995;
      f.rolling_resistance = 0;
      f.color = ball_color;
      ball.linear_damping = 0;
      ball.angular_damping = 0;
      ball.allow_sleep = false;
      ball.awake = true;
      ball.bullet = false;
      const std::string ball_id = ball.id;
      for (int side : {-1, 1}) {
        Body a = new_body(BodyKind::fixed, {rest_x, side * wire, top_z}, "cradle anchor " + std::to_string(i + 1) + (side < 0 ? " L" : " R"));
        const std::string aid = a.id;
        s.bodies.push_back(std::move(a));
        Joint& j = jn(JointKind::distance, aid, ball_id, {rest_x, side * wire, top_z});
        j.anchor_a = {};
        j.anchor_b = {};
        j.length = wire_len;
        j.enable_limit = false;
        j.enable_spring = false;
        j.frequency = 0;
        j.damping_ratio = 0;
      }
    }
    s.cursor = {0, 0, top_z - drop};
    message = "Newton cradle · equilibrium start · 50 mm steel balls (~0.514 kg each) · restitution 0.995 · 120 Hz / 12 substeps · threshold 0.01 m/s. Run → Grab an end ball, pull it back and release.";
  } else if (kind == "game") {
    auto colored = [](Body& b, const char* color) -> Body& { b.fixtures[0].color = color; return b; };
    colored(mk("ground", BodyKind::fixed, {0, 0, -0.5}, FixtureShape::box, {32, 8, 1}), "#56664a");
    Body& ramp = colored(mk("ramp", BodyKind::fixed, {17.6, 0, 0.62}, FixtureShape::box, {8, 6, 0.4}), "#6f7f5f");
    ramp.rotation = quat_from_euler_xyz({0, deg2rad(-11), 0});
    colored(mk("upper deck", BodyKind::fixed, {26, 0, 1.5}, FixtureShape::box, {10, 8, 1}), "#56664a");
    Body& player = colored(mk("player ball", BodyKind::dynamic, {-13, 0, 0.6}, FixtureShape::sphere, {0.5}), "#e8b04a");
    player.game.role = GameRole::player;
    player.game.control = GameControl::roll;
    player.game.force = 14;
    player.game.jump = 6;
    player.game.camera_follow = true;
    player.fixtures[0].friction = 0.9;
    player.fixtures[0].rolling_resistance = 0.02;
    player.angular_damping = 0.3;
    const D3 coins[] = {{-8, -2, 0.6}, {-4, 2, 0.6}, {1, 0, 0.6}, {6, -2, 0.6}, {10, 2, 0.6}, {26, 0, 2.6}};
    int n = 0;
    for (const D3& c : coins) {
      Body& coin = colored(mk("coin " + std::to_string(++n), BodyKind::fixed, c, FixtureShape::sphere, {0.35}), "#f4d35e");
      coin.fixtures[0].sensor = true;
      coin.game.role = GameRole::pickup;
      coin.game.points = 1;
    }
    Body& spinner = colored(mk("spinning bar", BodyKind::kinematic, {3.5, 0, 0.35}, FixtureShape::box, {0.35, 6.5, 0.35}), "#d1495b");
    spinner.angular_velocity = {0, 0, 1.4};
    spinner.game.role = GameRole::hazard;
    Body& goal = colored(mk("goal gate", BodyKind::fixed, {29.5, 0, 3}, FixtureShape::box, {1.2, 3, 2}), "#5fd38d");
    goal.fixtures[0].sensor = true;
    goal.game.role = GameRole::goal;
    for (int k = 0; k < 4; ++k)
      colored(mk("crate " + std::to_string(k + 1), BodyKind::dynamic, {23 + (k % 2) * 1.1, -2.5 + (k / 2) * 1.1, 2.5}, FixtureShape::box, {1, 1, 1}), "#a47148");
    s.game.title = "Roll & Collect";
    s.game.goal = GameGoal::collect_all;
    s.game.time_limit = 120;
    s.game.camera_distance = 9;
    s.game.camera_height = 5;
    s.game.fall_limit = -10;
    message = "Roll & Collect · Run (Ctrl+R): WASD/arrows roll the ball, Space jumps. Collect all coins, avoid the red bar, then reach the green gate.";
  } else {
    Body& ramp = mk("ramp", BodyKind::fixed, {4, 0, 0.6}, FixtureShape::box, {5, 4, 0.3});
    ramp.rotation = quat_from_euler_xyz({0, deg2rad(-12), 0});
    const std::string chassis = mk("chassis", BodyKind::dynamic, {-4, 0, 2.2}, FixtureShape::box, {3.2, 1.5, 0.6}).id;
    for (f64 y : {-0.8, 0.8})
      for (f64 x : {-5.1, -2.9}) {
        Body& w = mk("wheel", BodyKind::dynamic, {x, y, 1.2}, FixtureShape::capsule, {0.45, 0.35});
        w.rotation = qaxis({0, 1, 0}, kPiD / 2);
        const std::string wid = w.id;
        Joint& j = jn(JointKind::wheel, chassis, wid, {x, y, 1.2});
        j.enable_motor = true;
        j.motor_speed = 3;
        j.max_motor_torque = 80;
      }
    const std::string a = mk("pendulum support", BodyKind::fixed, {4, 0, 7}, FixtureShape::box, {0.4, 0.4, 0.4}).id;
    const std::string bob = mk("pendulum bob", BodyKind::dynamic, {4, 0, 4}, FixtureShape::sphere, {0.7}).id;
    Joint& j = jn(JointKind::distance, a, bob, {4, 0, 7});
    j.anchor_a = {};
    j.anchor_b = {};
    j.length = 3;
    for (int z = 0; z < 3; ++z)
      for (int x = 0; x < 3 - z; ++x)
        mk("box " + std::to_string(z) + "-" + std::to_string(x), BodyKind::dynamic, {5 + x * 0.85 + z * 0.42, 3, 0.45 + z * 0.85}, FixtureShape::box, {0.8, 0.8, 0.8});
  }
  if (!message.empty()) report(message);
  s.cursor = kind == "game" ? D3{-13, 0, 1} : D3{0, 0, 2};
  d->dirty = false;
  for (auto& sel : d->selection.ids) sel.clear();
  if (s.bodies.size() > 1) d->selection[Mode::body] = {s.bodies[1].id};
  mode_ = Mode::body;
  d->history.clear();
  d->index = 0;
  ++d->rev;
  for (auto& v : views_)
    if (v->doc == d->id) v->needs_fit = true;
  touch();
  report("Example loaded: " + s.name);
  schedule_save();
}

}  // namespace kern::editor
