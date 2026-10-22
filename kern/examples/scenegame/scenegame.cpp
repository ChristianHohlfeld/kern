// A game written entirely in C++ that plays a scene made in the Kern editor.
//
// The level is an .e3d document (kern::world::Scene): either passed in by the
// host page (launch parameter "scene", the JSON text) or the small level built
// in code below. kern::world::SceneRuntime runs it on Box3D with the game
// components (player control, pickups, hazards, goal), kern::world::SceneMeshes
// draws it, and this file adds the camera, keyboard mapping and status line.
//
//   kern/build.sh --app --world kern/examples/scenegame/scenegame.wasm kern/examples/scenegame/scenegame.cpp
#include <cmath>
#include <cstdio>
#include <string>

#include "kern/kern.h"
#include "kern/world/runtime.h"
#include "kern/world/scene.h"
#include "kern/world/scene_meshes.h"

using namespace kern;
using world::D3;

namespace {

world::Fixture box_fixture(const D3& size, const char* color, const char* name) {
  world::Fixture f = world::new_fixture(world::FixtureShape::box, name);
  f.size = size;
  f.color = color;
  return f;
}

/// The built-in level: a deck, a ramp, three coins, a hazard bar and a goal.
world::Scene built_in_level() {
  world::Scene s = world::new_scene("Scene game");
  s.game.title = "Kern scene game";
  s.game.goal = world::GameGoal::collect_all;
  s.game.time_limit = 90;
  s.game.camera_distance = 9;
  s.game.camera_height = 5;

  world::Body ground = world::new_body(world::BodyKind::fixed, {0, 0, -0.25}, "ground");
  ground.fixtures.push_back(box_fixture({24, 24, 0.5}, "#6f8f6a", "deck"));
  s.bodies.push_back(ground);

  world::Body ramp = world::new_body(world::BodyKind::fixed, {6, 0, 0.6}, "ramp");
  ramp.rotation = world::qaxis({0, 1, 0}, -0.25);
  ramp.fixtures.push_back(box_fixture({6, 3, 0.3}, "#8a8f9c", "ramp"));
  s.bodies.push_back(ramp);

  world::Body player = world::new_body(world::BodyKind::dynamic, {-6, 0, 1}, "player");
  world::Fixture ball = world::new_fixture(world::FixtureShape::sphere, "ball");
  ball.radius = 0.5;
  ball.friction = 0.9;
  ball.rolling_resistance = 0.02;
  ball.color = "#e8b85a";
  player.fixtures.push_back(ball);
  player.game.role = world::GameRole::player;
  player.game.control = world::GameControl::roll;
  player.game.force = 14;
  player.game.jump = 5;
  player.game.camera_follow = true;
  s.bodies.push_back(player);

  const D3 coins[] = {{-2, 4, 0.6}, {2, -4, 0.6}, {8.5, 0, 2.2}};
  int n = 0;
  for (const D3& p : coins) {
    world::Body coin = world::new_body(world::BodyKind::fixed, p, "coin " + std::to_string(++n));
    world::Fixture f = world::new_fixture(world::FixtureShape::sphere, "coin");
    f.radius = 0.35;
    f.sensor = true;
    f.color = "#f2d14b";
    coin.fixtures.push_back(f);
    coin.game.role = world::GameRole::pickup;
    coin.game.points = 10;
    s.bodies.push_back(coin);
  }

  world::Body bar = world::new_body(world::BodyKind::kinematic, {0, 0, 0.4}, "hazard bar");
  bar.angular_velocity = {0, 0, 1.2};
  bar.fixtures.push_back(box_fixture({7, 0.3, 0.3}, "#d9534f", "bar"));
  bar.game.role = world::GameRole::hazard;
  s.bodies.push_back(bar);
  return s;
}

}  // namespace

class SceneGame final : public App {
 public:
  RendererOptions configure() override {
    RendererOptions o;
    o.shadows = true;
    o.shadow.resolution = 2048;
    o.shadow.extent = 30;
    return o;
  }

  bool start(Engine& engine) override {
    std::string error;
    const char* text = engine.arg("scene");
    if (text && *text) {
      if (!world::from_text(text, scene_, &error, "scene.e3d")) return fail(("scene: " + error).c_str());
    } else {
      scene_ = built_in_level();
    }
    if (!runtime_.build(scene_)) return fail(("scene: " + runtime_.error()).c_str());
    world::SceneMeshes::Options options;
    if (!meshes_.build(engine.renderer, scene_, options)) return false;
    ground_ = engine.renderer.allocate_id();
    // A dark floor far below marks the fall limit.
    floor_ = primitives::plane(400, 400);
    for (std::size_t i = 2; i < floor_.positions.size(); i += 3) floor_.positions[i] += static_cast<f32>(scene_.game.fall_limit) - 0.5f;
    MeshInput g = floor_.as_input();
    g.material.color = {0.16f, 0.18f, 0.22f, 1};
    g.material.roughness = 0.95f;
    return engine.renderer.upsert(ground_, g);
  }

  void update(Engine& engine, const Frame& frame) override {
    Input& in = engine.input;
    if (in.pressed(Key::r)) runtime_.restart();
    if (in.pressed(Key::p)) paused_ = !paused_;

    // Orbit with the mouse / one finger, zoom with the wheel.
    if (in.button(0) || in.touch_count() == 1) {
      yaw_ -= in.pointer_delta().x * 0.006f;
      pitch_ = clamp(pitch_ + in.pointer_delta().y * 0.004f, 0.15f, 1.35f);
    }
    zoom_ = clamp(zoom_ * (1 + in.wheel() * 0.001f), 0.4f, 3.0f);

    // Keyboard to game input, relative to the camera.
    world::GameInput gi;
    gi.move_y = (in.down(Key::w) || in.down(Key::up) ? 1.0f : 0.0f) - (in.down(Key::s) || in.down(Key::down) ? 1.0f : 0.0f);
    gi.move_x = (in.down(Key::d) || in.down(Key::right) ? 1.0f : 0.0f) - (in.down(Key::a) || in.down(Key::left) ? 1.0f : 0.0f);
    gi.up = (in.down(Key::space) || in.down(Key::e) ? 1.0f : 0.0f) - (in.down(Key::shift_left) || in.down(Key::q) ? 1.0f : 0.0f);
    gi.jump = in.down(Key::space);
    gi.camera_forward = {-std::cos(yaw_), -std::sin(yaw_), 0};
    runtime_.set_input(gi);
    if (!paused_) runtime_.advance(frame.dt);
    meshes_.update(engine.renderer, runtime_);

    // Camera follows the player (or the body marked camera-follow).
    const world::GameState& g = runtime_.game();
    world::DPose focus;
    const std::string& follow = !g.camera_body.empty() ? g.camera_body : g.player;
    if (follow.empty() || !runtime_.body_pose(follow, focus)) focus.position = {0, 0, 0};
    const Vec3 target{static_cast<f32>(focus.position[0]), static_cast<f32>(focus.position[1]), static_cast<f32>(focus.position[2])};
    const f32 dist = static_cast<f32>(std::hypot(scene_.game.camera_distance, scene_.game.camera_height)) * zoom_;
    Camera& c = engine.renderer.camera;
    c.target = target;
    c.eye = target + Vec3{std::cos(yaw_) * std::cos(pitch_), std::sin(yaw_) * std::cos(pitch_), std::sin(pitch_)} * dist;
    c.far = 2000;

    char status[200];
    if (g.status == world::GameStatus::inactive) {
      std::snprintf(status, sizeof status, "%s - no player in this scene - drag to orbit", scene_.name.c_str());
    } else {
      const char* state = g.status == world::GameStatus::won ? "  YOU WIN - R restarts" : g.status == world::GameStatus::lost ? "  GAME OVER - R restarts" : "";
      std::snprintf(status, sizeof status, "%s - score %.0f - %u/%u collected - %.1f s%s%s", scene_.game.title.empty() ? scene_.name.c_str() : scene_.game.title.c_str(),
                    g.score, g.pickups_collected, g.pickups_total, g.elapsed, paused_ ? "  (paused)" : "", state);
    }
    engine.set_status(status);
  }

 private:
  static bool fail(const char* message) { return kern::fail("%s", message); }

  world::Scene scene_;
  world::SceneRuntime runtime_;
  world::SceneMeshes meshes_;
  MeshData floor_;
  u32 ground_ = 0;
  f32 yaw_ = 3.6f, pitch_ = 0.5f, zoom_ = 1;
  bool paused_ = false;
};

App* kern_create_app() {
  static SceneGame app;
  return &app;
}
