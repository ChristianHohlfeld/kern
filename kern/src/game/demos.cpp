#include "kern/game/demos.h"
#include "kern/core/dmath.h"

#include <cmath>
#include <cstdio>

#include "kern/core/json.h"
#include "kern/game/geometry.h"
#include "kern/game/walker.h"
#include "kern/logic/logic.h"

namespace kern::game {

namespace {

Actor make(const char* name, Vec3 position, Vec3 rotation = {}, Vec3 scale = {1, 1, 1}) {
  Actor a;
  a.name = name;
  a.transform.position = position;
  a.transform.rotation = rotation;
  a.transform.scale = scale;
  return a;
}

MeshComponent box_mesh(Vec3 size, Vec3 color, f32 roughness = 0.6f, f32 metalness = 0) {
  MeshComponent m;
  m.shape = Shape::box;
  m.size = size;
  m.material.color = color;
  m.material.roughness = roughness;
  m.material.metalness = metalness;
  return m;
}

MeshComponent round_mesh(Shape shape, f32 radius, f32 height, Vec3 color, f32 roughness = 0.5f) {
  MeshComponent m;
  m.shape = shape;
  m.radius = radius;
  m.height = height;
  m.material.color = color;
  m.material.roughness = roughness;
  return m;
}

BodyComponent body(Motion motion, f32 mass = 0) {
  BodyComponent b;
  b.motion = motion;
  b.mass = mass;
  return b;
}

/// sRGB colour whose linear value is `linear` divided by its largest component.
Vec3 emissive_color(Vec3 linear, f32* strength) {
  const f32 m = max_of(linear.x, max_of(linear.y, linear.z));
  *strength = m;
  auto enc = [](f32 v) { return v <= 0.0031308f ? v * 12.92f : 1.055f * kern::dm::pow(v, 1 / 2.4f) - 0.055f; };
  return {enc(linear.x / m), enc(linear.y / m), enc(linear.z / m)};
}

}  // namespace

Level empty_level() {
  Level l;
  l.name = "Empty";
  l.world.ground = false;
  return l;
}

Level basic_level() {
  Level l;
  l.name = "Untitled";
  l.world.time_of_day = 0.42f;
  Actor floor = make("Floor", {0, 0, -0.1f});
  floor.mesh = box_mesh({40, 40, 0.2f}, {0.55f, 0.57f, 0.6f}, 0.9f);
  l.add(floor);
  Actor cube = make("Cube", {0, 0, 0.5f});
  cube.mesh = box_mesh({1, 1, 1}, {0.85f, 0.55f, 0.3f});
  l.add(cube);
  l.camera.position = {-8, -10, 6};
  l.camera.yaw = 51;
  l.camera.pitch = -22;
  return l;
}

Level orb_run_level() {
  Level l;
  l.name = "Orb Run";
  WorldSettings& w = l.world;
  w.title = "Orb Run";
  w.goal = GameGoal::collect_all;
  w.time_of_day = 0.705f;  // late afternoon: the run ends in the night
  w.day_length = 480;
  w.fog_density = 0.0011f;
  w.ground_color = {0.19f, 0.27f, 0.15f};
  w.kill_z = -20;
  w.substeps = 4;
  w.music = "builtin:music_drive";
  w.music_volume = 0.45f;
  w.ambience = "builtin:city";
  w.ambience_volume = 0.5f;
  w.reverb = 0.3f;

  Actor city = make("City", {0, 0, 0});
  city.city = CityComponent{};
  const CityComponent cc = *city.city;
  l.add(city);

  // The hover car: one dynamic body, the parts are welded children.
  const f32 half = cc.blocks * cc.pitch * 0.5f;
  Actor car = make("Hover Car", {-half + cc.pitch * 3, -half + cc.pitch * 3 + 20, 1.0f}, {0, 0, 90});
  car.mesh = box_mesh({4.2f, 2.0f, 0.55f}, {0.9f, 0.9f, 0.92f}, 0.35f, 0.3f);
  car.mesh->collision = false;
  car.body = body(Motion::dynamic, 1200);
  car.body->angular_damping = 0.5f;
  car.body->allow_sleep = false;
  ColliderComponent hull;
  hull.shape = ColliderShape::box;
  hull.size = {4.2f, 2.0f, 0.9f};
  hull.center = {0, 0, 0.15f};
  hull.friction = 0.25f;
  car.colliders.push_back(hull);
  ControllerComponent ctl;
  ctl.mode = ControlMode::hovercar;
  ctl.max_speed = 36;
  ctl.acceleration = 24;
  ctl.turn_rate = 132;
  ctl.boost = true;
  ctl.boost_speed = 60;
  ctl.hover_height = 0.6f;
  ctl.camera = CameraMode::chase;
  ctl.camera_distance = 12;
  ctl.camera_height = 4;
  ctl.camera_fov = 62;
  car.controller = ctl;
  car.tags = {"Player"};
  const ActorId car_id = l.add(car).id;

  Actor cabin = make("Cabin", {-0.3f, 0, 0.5f});
  cabin.parent = car_id;
  cabin.mesh = box_mesh({2.0f, 1.7f, 0.55f}, {0.12f, 0.16f, 0.22f}, 0.25f, 0.5f);
  cabin.mesh->collision = false;
  l.add(cabin);
  Actor spoiler = make("Spoiler", {-2.0f, 0, 0.4f});
  spoiler.parent = car_id;
  spoiler.mesh = box_mesh({0.5f, 2.3f, 0.12f}, {0.95f, 0.35f, 0.15f}, 0.4f);
  spoiler.mesh->collision = false;
  l.add(spoiler);
  const Vec3 pods[4] = {{1.5f, 1.05f, -0.1f}, {1.5f, -1.05f, -0.1f}, {-1.6f, 1.05f, -0.1f}, {-1.6f, -1.05f, -0.1f}};
  const char* pod_names[4] = {"Thruster FL", "Thruster FR", "Thruster RL", "Thruster RR"};
  for (int i = 0; i < 4; ++i) {
    Actor pod = make(pod_names[i], pods[i]);
    pod.parent = car_id;
    pod.mesh = round_mesh(Shape::sphere, 0.32f, 0.64f, {0.3f, 0.9f, 1.0f});
    pod.mesh->segments = 12;
    pod.mesh->collision = false;
    pod.mesh->material.cast_shadow = false;
    pod.mesh->material.emissive = emissive_color({0.6f, 2.4f, 3.0f}, &pod.mesh->material.emissive_strength);
    l.add(pod);
  }
  Actor headlight = make("Headlight", {6, 0, 1.2f});
  headlight.parent = car_id;
  headlight.light = LightComponent{{1.0f, 0.95f, 0.85f}, 10, 18};
  l.add(headlight);

  // Energy orbs on the streets.
  Rng rng(cc.seed ^ 0x51ed270bu);
  for (int i = 0; i < 24; ++i) {
    char name[32];
    std::snprintf(name, sizeof name, "Orb %02d", i + 1);
    const Vec2 p = city_street_point(cc, rng);
    Actor orb = make(name, {p.x, p.y, 1.6f});
    orb.folder = "Orbs";
    orb.mesh = round_mesh(Shape::sphere, 0.9f, 1.8f, {0.3f, 1.0f, 0.9f});
    orb.mesh->segments = 18;
    orb.mesh->collision = false;
    orb.mesh->material.cast_shadow = false;
    orb.mesh->material.emissive = emissive_color({0.5f, 2.6f, 2.2f}, &orb.mesh->material.emissive_strength);
    orb.light = LightComponent{{0.3f, 1.0f, 0.9f}, 12, 16};
    orb.pickup = PickupComponent{1, {0.55f, 1.0f, 0.92f}, 0};
    orb.rotator = RotatorComponent{{0, 0, 57.3f}};
    orb.mover = MoverComponent{{0, 0, 0.7f}, 2.86f, rng.unit(), MoverMode::sine};
    ColliderComponent sensor;
    sensor.shape = ColliderShape::sphere;
    sensor.radius = 2.4f;
    sensor.sensor = true;
    orb.colliders.push_back(sensor);
    orb.tags = {"Orb"};
    l.add(orb);
  }
  // Down the avenue behind the car, the skyline ahead.
  l.camera.position = {car.transform.position.x, car.transform.position.y - 38, 13};
  l.camera.yaw = 90;
  l.camera.pitch = -15;
  return l;
}

Level micro_duck_level() {
  // A room of the real robot's size class: 2.5 x 2.5 m real (the duck is built kWalkerScale x, so
  // 10 x 10 m here), a few things in it to walk around, and a ball it quacks at when its head
  // camera sees it (the level blueprint).
  const f32 S = kWalkerScale;
  Level l;
  l.name = "Micro Duck";
  l.world.title = "Micro Duck";
  l.world.time_of_day = 0.4f;
  l.world.step_hz = kWalkerStepHz;  // the policy's timing (docs/walker.md section 3)
  l.world.substeps = kWalkerSubsteps;
  l.world.reverb = 0.2f;
  l.world.ground = false;  // the floor is the ground (no z-fighting with the ground plane)
  l.world.ao_radius = 0.8f;  // the room's scale: contact shade under the duck's feet and in the corners
  l.world.ground_friction = 1.0f;
  Actor floor = make("Floor", {0, 0, -0.1f});
  floor.mesh = box_mesh({60, 60, 0.2f}, {0.62f, 0.55f, 0.46f}, 0.85f);  // a wooden floor
  floor.mesh->material.tiling = 0.5f;
  // Its friction is the reference's: Pollen's training floor and the duck's soles both 1.0, so the
  // soles grip at 1.0 here too (Box3D: sqrt(1.0 x 1.0)).
  floor.mesh->friction = 1.0f;
  l.add(floor);
  const f32 half = 1.25f * S, h = 0.25f * S;
  const struct { const char* name; Vec3 at, size; } walls[] = {
      {"Wall North", {0, half, h}, {2 * half + 0.4f, 0.4f, 2 * h}}, {"Wall South", {0, -half, h}, {2 * half + 0.4f, 0.4f, 2 * h}},
      {"Wall East", {half, 0, h}, {0.4f, 2 * half, 2 * h}},         {"Wall West", {-half, 0, h}, {0.4f, 2 * half, 2 * h}}};
  for (const auto& w : walls) {
    Actor a = make(w.name, w.at);
    a.folder = "Room";
    a.mesh = box_mesh(w.size, {0.86f, 0.84f, 0.8f}, 0.9f);
    l.add(a);
  }
  // Furniture at duck scale: shoeboxes, a stack of books, a flower pot (real sizes x S).
  const struct { const char* name; Vec3 at, size, color; } boxes[] = {
      {"Shoebox", {0.35f * S, 0.4f * S, 0.06f * S}, {0.33f * S, 0.2f * S, 0.12f * S}, {0.8f, 0.3f, 0.25f}},
      {"Books", {-0.45f * S, 0.55f * S, 0.05f * S}, {0.24f * S, 0.17f * S, 0.1f * S}, {0.25f, 0.4f, 0.7f}},
      {"Crate", {0.6f * S, -0.5f * S, 0.1f * S}, {0.2f * S, 0.2f * S, 0.2f * S}, {0.72f, 0.52f, 0.3f}}};
  for (const auto& b : boxes) {
    Actor a = make(b.name, b.at);
    a.folder = "Furniture";
    a.mesh = box_mesh(b.size, b.color, 0.7f);
    l.add(a);
  }
  Actor pot = make("Flower Pot", {-0.5f * S, -0.45f * S, 0.08f * S});
  pot.folder = "Furniture";
  pot.mesh = round_mesh(Shape::cylinder, 0.08f * S, 0.16f * S, {0.75f, 0.4f, 0.25f}, 0.8f);
  l.add(pot);
  // A light ball (a real 12 cm toy ball): Box3D pushes it when the duck walks into it.
  Actor ball = make("Ball", {0.1f * S, -0.2f * S, 0.06f * S});
  ball.mesh = round_mesh(Shape::sphere, 0.06f * S, 0.12f * S, {0.95f, 0.8f, 0.15f}, 0.4f);
  ball.body = body(Motion::dynamic, 0.05f * S * S * S);
  const ActorId ball_id = l.add(ball).id;

  // Two crumpled socks on the floor: the duck sees them with its head camera, walks up, picks one
  // up, carries it off and puts it down (Kern behaviour, docs/walker.md section 9).
  for (const Vec3 at : {Vec3{-0.2f * S, -0.55f * S, 0}, Vec3{0.45f * S, -0.1f * S, 0}}) {
    std::vector<Actor> sock = micro_duck_sock_actors();
    const ActorId first = l.next_id;
    for (std::size_t i = 0; i < sock.size(); ++i) {
      Actor a = sock[i];
      a.id = first + static_cast<ActorId>(i);
      a.folder = "Socks";
      if (i == 0) a.transform.position = a.transform.position + at;
      if (a.parent) a.parent = first + a.parent - 1;
      for (JointComponent& j : a.joints)
        if (j.target) j.target = first + j.target - 1;
      l.add(std::move(a));
    }
  }

  // The duck: autonomous (it roams with its ToF and looks around when it stands).
  std::vector<Actor> parts = micro_duck_actors();
  const ActorId base = l.next_id;
  for (std::size_t i = 0; i < parts.size(); ++i) {
    Actor a = parts[i];
    a.id = base + static_cast<ActorId>(i);
    if (i == 0) a.transform.position = a.transform.position + Vec3{-0.7f * S, -0.7f * S, 0};
    if (i == 0) a.transform.rotation = {0, 0, 45};
    if (a.parent) a.parent = base + a.parent - 1;
    for (JointComponent& j : a.joints)
      if (j.target) j.target = base + j.target - 1;
    l.add(std::move(a));
  }
  l.world.player = base;
  l.world.observer = true;  // watch it play: a free camera, and pick things up with the mouse / a finger

  // Level blueprint (a demo of the Robots nodes, not the robot's own behaviour): chirp once whenever
  // the ball comes into the head camera's picture.
  char text[2048];
  std::snprintf(text, sizeof text, R"({"variables":[{"name":"Ball Seen","type":"bool","value":false}],
    "nodes":[{"id":1,"type":"event.every","x":0,"y":0,"params":{"seconds":"0.25"}},{"id":2,"type":"flow.branch","x":608,"y":0},
      {"id":3,"type":"walker.sees","x":-32,"y":192,"values":{"other":%u}},{"id":4,"type":"var.get","x":32,"y":384,"params":{"variable":"Ball Seen"}},
      {"id":5,"type":"logic.not","x":288,"y":384},{"id":6,"type":"logic.and","x":416,"y":224},
      {"id":7,"type":"walker.quack","x":896,"y":-64},{"id":8,"type":"var.set","x":1152,"y":-64,"params":{"variable":"Ball Seen"}},
      {"id":9,"type":"var.set","x":896,"y":160,"params":{"variable":"Ball Seen"}}],
    "comments":[{"id":20,"x":-64,"y":-112,"w":1472,"h":592,"text":"Demo, not something the robot does by itself: chirp when the ball comes into the head camera's picture"}],
    "links":[[1,"then",2,"exec"],[3,"visible",6,"a"],[4,"value",5,"a"],[5,"result",6,"b"],[6,"result",2,"condition"],
      [2,"true",7,"exec"],[7,"then",8,"exec"],[3,"visible",8,"value"],[2,"false",9,"exec"],[3,"visible",9,"value"]]})",
                static_cast<unsigned>(ball_id));
  json::Value j;
  logic::Graph g;
  if (json::parse(text, j) && logic::graph_from_json(j, g)) l.logic = std::move(g);

  l.camera.position = {-8.5f, -9.5f, 7.5f};
  l.camera.yaw = 50;
  l.camera.pitch = -38;
  return l;
}

Level shooting_range_level() {
  Level l;
  l.name = "Shooting Range";
  l.world.title = "Shooting Range";
  l.world.time_of_day = 0.38f;
  l.world.goal = GameGoal::score;
  l.world.score_target = 150;
  l.world.kill_z = -20;
  l.world.ambience = "builtin:wind";
  l.world.ambience_volume = 0.3f;
  l.world.reverb = 0.25f;

  Actor floor = make("Floor", {0, 0, -0.25f});
  floor.mesh = box_mesh({120, 60, 0.5f}, {0.46f, 0.47f, 0.42f}, 0.95f);
  floor.mesh->material.tiling = 0.25f;
  l.add(floor);
  // Walls around the range.
  const struct { const char* name; Vec3 at, size; } walls[] = {
      {"Wall North", {0, 30, 2}, {120, 1, 4}}, {"Wall South", {0, -30, 2}, {120, 1, 4}},
      {"Wall West", {-60, 0, 2}, {1, 60, 4}},  {"Backstop", {60, 0, 4}, {1, 60, 8}}};
  for (const auto& w : walls) {
    Actor a = make(w.name, w.at);
    a.folder = "Walls";
    a.mesh = box_mesh(w.size, {0.62f, 0.6f, 0.55f}, 0.9f);
    l.add(a);
  }
  // Firing line: a low counter and lane markers.
  Actor counter = make("Firing Line", {-38, 0, 0.5f});
  counter.folder = "Firing Line";
  counter.mesh = box_mesh({0.6f, 24, 1}, {0.35f, 0.3f, 0.25f}, 0.7f);
  l.add(counter);
  for (int i = -2; i <= 2; ++i) {
    char name[32];
    std::snprintf(name, sizeof name, "Lane %d", i + 3);
    Actor m = make(name, {-10, i * 5.0f, 0.01f});
    m.folder = "Firing Line";
    m.mesh = box_mesh({56, 0.12f, 0.02f}, {0.9f, 0.85f, 0.3f}, 0.6f);
    m.mesh->collision = false;
    l.add(m);
  }
  // Target dummies at 10 .. 60 m (head tagged for headshots).
  const Vec3 dummies[] = {{-28, -5, 0}, {-22, 5, 0}, {-12, 0, 0}, {0, -10, 0}, {6, 8, 0}, {14, -3, 0}, {22, 12, 0}, {30, 0, 3}};
  int k = 0;
  for (const Vec3& d : dummies) {
    char name[32];
    std::snprintf(name, sizeof name, "Target Dummy %d", ++k);
    Actor t = make(name, d + Vec3{0, 0, 0.725f}, {0, 0, 180});
    t.folder = "Targets";
    t.mesh = round_mesh(Shape::capsule, 0.32f, 1.45f, {0.85f, 0.35f, 0.25f}, 0.7f);
    HealthComponent h;
    h.regen_delay = 3;
    h.regen_rate = 40;
    h.respawn_delay = 3;
    t.health = h;
    t.tags.push_back("target");
    const ActorId id = l.add(t).id;
    Actor head = make("Head", {0, 0, 0.88f});
    head.parent = id;
    head.folder = "Targets";
    head.mesh = round_mesh(Shape::sphere, 0.17f, 0.34f, {0.95f, 0.85f, 0.75f}, 0.6f);
    head.tags.push_back("head");
    l.add(head);
  }
  // A platform for the far target and a ramp up to it.
  Actor deck = make("Tower Deck", {30, 0, 1.5f});
  deck.folder = "Tower";
  deck.mesh = box_mesh({6, 6, 3}, {0.4f, 0.42f, 0.46f}, 0.7f, 0.3f);
  l.add(deck);
  Actor ramp = make("Tower Ramp", {22.5f, -1.75f, 1.5f}, {0, -20, 0});
  ramp.folder = "Tower";
  ramp.mesh = box_mesh({9, 2.5f, 0.3f}, {0.4f, 0.42f, 0.46f}, 0.7f, 0.3f);
  l.add(ramp);
  // Cover: concrete blocks and loose crates (Box3D bodies the shots push around).
  const Vec3 cover[] = {{-20, -12, 0.75f}, {-6, 12, 0.75f}, {4, -4, 0.75f}, {16, 6, 0.75f}};
  k = 0;
  for (const Vec3& c : cover) {
    char name[32];
    std::snprintf(name, sizeof name, "Cover %d", ++k);
    Actor a = make(name, c);
    a.folder = "Cover";
    a.mesh = box_mesh({1.2f, 3.5f, 1.5f}, {0.58f, 0.58f, 0.56f}, 0.95f);
    l.add(a);
  }
  k = 0;
  for (int i = 0; i < 6; ++i) {
    char name[32];
    std::snprintf(name, sizeof name, "Crate %d", ++k);
    Actor c = make(name, {-16.0f + (i % 3) * 0.9f, 16.0f, 0.4f + (i / 3) * 0.8f});
    c.folder = "Crates";
    c.mesh = box_mesh({0.8f, 0.8f, 0.8f}, {0.72f, 0.52f, 0.3f}, 0.75f);
    c.body = body(Motion::dynamic, 15);
    l.add(c);
  }
  Actor barrel = make("Barrel", {-8, -18, 0.5f});
  barrel.folder = "Crates";
  barrel.mesh = round_mesh(Shape::cylinder, 0.35f, 1.0f, {0.25f, 0.45f, 0.75f}, 0.4f);
  barrel.body = body(Motion::dynamic, 25);
  l.add(barrel);

  // The player: a Soldier with a rifle (first person; replicated in multiplayer automatically).
  Actor soldier = make("Soldier", {-44, 0, 0.725f});
  soldier.mesh = round_mesh(Shape::capsule, 0.35f, 1.45f, {0.32f, 0.42f, 0.3f}, 0.8f);
  soldier.body = body(Motion::dynamic, 80);
  soldier.body->allow_sleep = false;
  ControllerComponent c;
  c.mode = ControlMode::soldier;
  c.camera = CameraMode::first_person;
  c.max_speed = 5.5f;
  c.boost_speed = 8.1f;
  c.jump_speed = 3.6f;
  c.camera_fov = 72;
  soldier.controller = c;
  soldier.weapon = WeaponComponent{};
  soldier.health = HealthComponent{};
  const ActorId sid = l.add(soldier).id;
  Actor shead = make("Head", {0, 0, 0.83f});
  shead.parent = sid;
  shead.mesh = round_mesh(Shape::sphere, 0.17f, 0.34f, {0.85f, 0.7f, 0.55f}, 0.6f);
  shead.tags.push_back("head");
  l.add(shead);
  Actor gun = make("Rifle", {0.35f, -0.22f, 0.35f});
  gun.parent = sid;
  gun.mesh = box_mesh({0.75f, 0.07f, 0.12f}, {0.12f, 0.12f, 0.13f}, 0.5f, 0.4f);
  gun.mesh->collision = false;
  l.add(gun);

  l.camera.position = {-52, -14, 9};
  l.camera.yaw = 28;
  l.camera.pitch = -18;
  return l;
}

Level physics_lab_level() {
  Level l;
  l.name = "Physics Lab";
  l.world.title = "Physics Lab";
  l.world.time_of_day = 0.45f;
  l.world.goal = GameGoal::reach_goal;
  l.world.kill_z = -15;
  l.world.music = "builtin:music_calm";
  l.world.music_volume = 0.35f;
  l.world.ambience = "builtin:hum";
  l.world.ambience_volume = 0.25f;
  l.world.reverb = 0.45f;

  Actor floor = make("Floor", {0, 0, -0.25f});
  floor.mesh = box_mesh({70, 50, 0.5f}, {0.5f, 0.53f, 0.56f}, 0.85f);
  floor.mesh->material.tiling = 0.5f;
  l.add(floor);

  // 1. A pyramid of crates.
  int n = 0;
  for (int row = 0; row < 5; ++row)
    for (int k = 0; k < 5 - row; ++k) {
      char name[32];
      std::snprintf(name, sizeof name, "Crate %02d", ++n);
      Actor c = make(name, {-20 + (k - (4 - row) * 0.5f) * 1.05f, -12, 0.5f + row * 1.0f});
      c.folder = "Crate Stack";
      c.mesh = box_mesh({1, 1, 1}, {0.75f, 0.55f, 0.32f}, 0.7f);
      c.body = body(Motion::dynamic);
      c.body->start_awake = false;
      ColliderComponent col;
      col.density = 300;
      col.friction = 0.7f;
      c.colliders.push_back(col);
      l.add(c);
    }

  // 2. Pendulum: a heavy ball on a distance joint below a static beam.
  Actor post = make("Pendulum Beam", {-8, -12, 6});
  post.folder = "Pendulum";
  post.mesh = box_mesh({0.4f, 4, 0.4f}, {0.3f, 0.32f, 0.36f}, 0.4f, 0.7f);
  const ActorId post_id = l.add(post).id;
  Actor pole_l = make("Pendulum Pole L", {-8, -14, 3});
  pole_l.folder = "Pendulum";
  pole_l.mesh = box_mesh({0.3f, 0.3f, 6}, {0.3f, 0.32f, 0.36f}, 0.4f, 0.7f);
  l.add(pole_l);
  Actor pole_r = make("Pendulum Pole R", {-8, -10, 3});
  pole_r.folder = "Pendulum";
  pole_r.mesh = box_mesh({0.3f, 0.3f, 6}, {0.3f, 0.32f, 0.36f}, 0.4f, 0.7f);
  l.add(pole_r);
  Actor ball = make("Pendulum Ball", {-4, -12, 3});
  ball.folder = "Pendulum";
  ball.mesh = round_mesh(Shape::sphere, 0.7f, 1.4f, {0.85f, 0.2f, 0.2f}, 0.3f);
  ball.mesh->material.metalness = 0.6f;
  ball.body = body(Motion::dynamic, 80);
  ball.body->allow_sleep = false;
  JointComponent rope;
  rope.kind = JointKind::distance;
  rope.target = post_id;
  rope.anchor = {0, 0, 0};
  rope.length = 0;
  ball.joints.push_back(rope);
  l.add(ball);

  // 3. Hinged door with limits and a closing spring.
  Actor frame = make("Door Frame", {4, -12, 1.5f});
  frame.folder = "Door";
  frame.mesh = box_mesh({0.3f, 0.3f, 3}, {0.35f, 0.25f, 0.18f}, 0.8f);
  const ActorId frame_id = l.add(frame).id;
  Actor door = make("Door", {5.2f, -12, 1.5f});
  door.folder = "Door";
  door.mesh = box_mesh({2.1f, 0.12f, 2.8f}, {0.6f, 0.42f, 0.28f}, 0.7f);
  door.body = body(Motion::dynamic, 40);
  JointComponent hinge;
  hinge.kind = JointKind::hinge;
  hinge.target = frame_id;
  hinge.anchor = {-1.15f, 0, 0};
  hinge.axis = {0, 0, 1};
  hinge.enable_limit = true;
  hinge.lower = -100;
  hinge.upper = 100;
  hinge.enable_spring = true;
  hinge.spring_hertz = 0.6f;
  hinge.spring_damping = 0.3f;
  door.joints.push_back(hinge);
  l.add(door);

  // 4. Rope bridge between two towers; the middle joints break under load.
  const int planks = 12;
  Actor tower_a = make("Bridge Tower A", {-8.8f, 6, 1.5f});
  tower_a.folder = "Bridge";
  tower_a.mesh = box_mesh({1.6f, 3, 3}, {0.42f, 0.44f, 0.47f}, 0.9f);
  const ActorId ta = l.add(tower_a).id;
  Actor tower_b = make("Bridge Tower B", {8.8f, 6, 1.5f});
  tower_b.folder = "Bridge";
  tower_b.mesh = box_mesh({1.6f, 3, 3}, {0.42f, 0.44f, 0.47f}, 0.9f);
  const ActorId tb = l.add(tower_b).id;
  ActorId previous = ta;
  for (int i = 0; i < planks; ++i) {
    char name[32];
    std::snprintf(name, sizeof name, "Plank %02d", i + 1);
    const f32 x = -7.15f + i * 1.3f;
    Actor p = make(name, {x, 6, 2.9f});
    p.folder = "Bridge";
    p.mesh = box_mesh({1.2f, 2.4f, 0.16f}, {0.55f, 0.38f, 0.22f}, 0.8f);
    p.body = body(Motion::dynamic, 15);
    JointComponent link;
    link.kind = JointKind::hinge;
    link.target = previous;
    link.anchor = {-0.65f, 0, 0};
    link.axis = {0, 1, 0};
    if (i >= 5 && i <= 7) link.break_force = 60000;
    p.joints.push_back(link);
    previous = l.add(p).id;
  }
  // Last plank to the far tower.
  Actor* last = l.find(previous);
  JointComponent end;
  end.kind = JointKind::hinge;
  end.target = tb;
  end.anchor = {0.65f, 0, 0};
  end.axis = {0, 1, 0};
  last->joints.push_back(end);

  // 5. Motorised turntable (hinge to the world) with boxes riding on it.
  Actor table = make("Turntable", {16, -4, 0.25f});
  table.folder = "Turntable";
  table.mesh = round_mesh(Shape::cylinder, 3.5f, 0.5f, {0.2f, 0.45f, 0.75f}, 0.4f);
  table.mesh->segments = 40;
  table.body = body(Motion::dynamic, 400);
  table.body->allow_sleep = false;
  JointComponent spin;
  spin.kind = JointKind::hinge;
  spin.target = 0;
  spin.axis = {0, 0, 1};
  spin.enable_motor = true;
  spin.motor_speed = 45;
  spin.max_motor_force = 50000;
  table.joints.push_back(spin);
  l.add(table);
  for (int i = 0; i < 3; ++i) {
    char name[32];
    std::snprintf(name, sizeof name, "Rider %d", i + 1);
    Actor r = make(name, {16 + 2.2f * kern::dm::cos(i * 2.1f), -4 + 2.2f * kern::dm::sin(i * 2.1f), 1.0f});
    r.folder = "Turntable";
    r.mesh = box_mesh({0.8f, 0.8f, 0.8f}, {0.95f, 0.8f, 0.25f}, 0.5f);
    r.body = body(Motion::dynamic);
    l.add(r);
  }

  // 6. Slider lift: a platform on a prismatic joint pushed up by its motor, held by a limit.
  Actor lift = make("Lift Platform", {24, 6, 0.3f});
  lift.folder = "Lift";
  lift.mesh = box_mesh({3, 3, 0.3f}, {0.3f, 0.7f, 0.4f}, 0.5f);
  lift.body = body(Motion::dynamic, 200);
  lift.body->allow_sleep = false;
  JointComponent slide;
  slide.kind = JointKind::slider;
  slide.target = 0;
  slide.axis = {0, 0, 1};
  slide.enable_limit = true;
  slide.lower = 0;
  slide.upper = 4;
  slide.enable_motor = true;
  slide.motor_speed = 1.2f;
  slide.max_motor_force = 9000;
  lift.joints.push_back(slide);
  l.add(lift);

  // 7. A self-driving car: chassis and four wheels on wheel joints with spin motors.
  Actor chassis = make("Rover Chassis", {0, 16, 1.2f});
  chassis.folder = "Rover";
  chassis.mesh = box_mesh({3.2f, 1.6f, 0.5f}, {0.85f, 0.3f, 0.2f}, 0.4f, 0.3f);
  chassis.body = body(Motion::dynamic, 300);
  chassis.body->allow_sleep = false;
  const ActorId chassis_id = l.add(chassis).id;
  const Vec2 wheel_pos[4] = {{1.2f, 1.05f}, {1.2f, -1.05f}, {-1.2f, 1.05f}, {-1.2f, -1.05f}};
  for (int i = 0; i < 4; ++i) {
    char name[32];
    std::snprintf(name, sizeof name, "Rover Wheel %d", i + 1);
    Actor wheel = make(name, {wheel_pos[i].x, 16 + wheel_pos[i].y, 0.55f}, {90, 0, 0});
    wheel.folder = "Rover";
    wheel.mesh = round_mesh(Shape::cylinder, 0.5f, 0.35f, {0.12f, 0.12f, 0.13f}, 0.9f);
    wheel.mesh->segments = 24;
    wheel.body = body(Motion::dynamic, 20);
    wheel.body->allow_sleep = false;
    ColliderComponent tyre;
    tyre.shape = ColliderShape::sphere;
    tyre.radius = 0.5f;
    tyre.friction = 1.2f;
    tyre.rolling_resistance = 0.02f;
    wheel.colliders.push_back(tyre);
    const ActorId wheel_id = l.add(wheel).id;
    JointComponent axle;
    axle.kind = JointKind::wheel;
    axle.target = wheel_id;
    axle.anchor = {wheel_pos[i].x, wheel_pos[i].y, -0.65f};
    axle.axis = {0, 1, 0};
    axle.enable_spring = true;
    axle.spring_hertz = 4;
    axle.spring_damping = 0.7f;
    axle.enable_limit = true;
    axle.lower = -0.25f;
    axle.upper = 0.25f;
    axle.enable_motor = true;
    axle.motor_speed = -220;
    axle.max_motor_force = 400;
    l.find(chassis_id)->joints.push_back(axle);
  }

  // 8. The player: a rolling ball, stars to collect, a hazard sweeper and the goal.
  Actor player = make("Player Ball", {-24, 4, 1});
  player.mesh = round_mesh(Shape::sphere, 0.6f, 1.2f, {0.95f, 0.75f, 0.3f}, 0.35f);
  player.body = body(Motion::dynamic, 30);
  player.body->allow_sleep = false;
  player.body->angular_damping = 0.3f;
  ColliderComponent skin;
  skin.shape = ColliderShape::sphere;
  skin.radius = 0.6f;
  skin.friction = 0.9f;
  skin.rolling_resistance = 0.03f;
  player.colliders.push_back(skin);
  ControllerComponent roll;
  roll.mode = ControlMode::rolling;
  roll.max_speed = 14;
  roll.acceleration = 26;
  roll.jump_speed = 6.5f;
  roll.boost = false;
  roll.camera = CameraMode::chase;
  roll.camera_distance = 9;
  roll.camera_height = 3;
  player.controller = roll;
  l.add(player);
  const Vec3 stars[5] = {{-14, 4, 1.2f}, {-4, -6, 1.2f}, {10, 0, 1.2f}, {0, 6, 4.2f}, {24, 6, 5.5f}};
  for (int i = 0; i < 5; ++i) {
    char name[32];
    std::snprintf(name, sizeof name, "Star %d", i + 1);
    Actor s = make(name, stars[i]);
    s.folder = "Stars";
    s.mesh = round_mesh(Shape::torus, 0.45f, 0.14f, {1.0f, 0.85f, 0.3f}, 0.3f);
    s.mesh->collision = false;
    s.mesh->material.emissive = {1.0f, 0.75f, 0.25f};
    s.mesh->material.emissive_strength = 1.5f;
    s.mesh->material.cast_shadow = false;
    s.pickup = PickupComponent{10, {1.0f, 0.85f, 0.4f}, 0};
    s.rotator = RotatorComponent{{0, 0, 120}};
    s.mover = MoverComponent{{0, 0, 0.4f}, 1.6f, i * 0.2f, MoverMode::sine};
    s.light = LightComponent{{1.0f, 0.8f, 0.4f}, 6, 6};
    l.add(s);
  }
  Actor sweeper = make("Hazard Sweeper", {-8, 0, 0.6f});
  sweeper.mesh = box_mesh({9, 0.4f, 0.4f}, {0.85f, 0.15f, 0.15f}, 0.4f);
  sweeper.mesh->material.emissive = {1, 0.1f, 0.1f};
  sweeper.mesh->material.emissive_strength = 0.6f;
  sweeper.rotator = RotatorComponent{{0, 0, 70}};
  sweeper.trigger = TriggerComponent{TriggerAction::hazard, "Swept away!", true};
  ColliderComponent blade;
  blade.shape = ColliderShape::box;
  blade.size = {9, 0.5f, 0.6f};
  blade.sensor = true;
  sweeper.colliders.push_back(blade);
  l.add(sweeper);
  Actor goal = make("Goal", {30, -8, 1.5f});
  goal.mesh = round_mesh(Shape::torus, 1.5f, 0.25f, {0.3f, 0.95f, 0.5f}, 0.3f);
  goal.transform.rotation = {90, 0, 0};
  goal.mesh->collision = false;
  goal.mesh->material.emissive = {0.3f, 1, 0.5f};
  goal.mesh->material.emissive_strength = 1.2f;
  goal.trigger = TriggerComponent{TriggerAction::goal, "Lab complete!", false};
  ColliderComponent ring;
  ring.shape = ColliderShape::sphere;
  ring.radius = 1.4f;
  ring.sensor = true;
  goal.colliders.push_back(ring);
  l.add(goal);
  Actor lamp = make("Lab Lamp", {0, 0, 8});
  lamp.light = LightComponent{{1.0f, 0.95f, 0.85f}, 30, 30};
  l.add(lamp);

  l.camera.position = {-30, -30, 18};
  l.camera.yaw = 42;
  l.camera.pitch = -26;
  return l;
}

}  // namespace kern::game
