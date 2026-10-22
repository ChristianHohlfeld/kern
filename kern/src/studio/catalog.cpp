// Kern Studio "Place Actors" catalog: basic shapes, lights, physics props,
// gameplay actors and procedural content, spawned where the user drops them.
#include <cmath>
#include <cstdio>
#include <cstring>

#include "internal.h"
#include "kern/game/geometry.h"
#include "kern/studio/studio.h"

namespace kern::studio {

namespace {

struct Item {
  const char* id;
  const char* label;
  const char* group;
  const char* icon;  // glyph name the page maps to an icon
  const char* help;
};

constexpr Item kItems[] = {
    {"cube", "Cube", "Basic", "cube", "1 m box with collision"},
    {"sphere", "Sphere", "Basic", "sphere", "1 m sphere"},
    {"cylinder", "Cylinder", "Basic", "cylinder", "Upright cylinder"},
    {"cone", "Cone", "Basic", "cone", "Upright cone"},
    {"capsule", "Capsule", "Basic", "capsule", "Capsule along Z"},
    {"plane", "Plane", "Basic", "plane", "10 x 10 m floor"},
    {"torus", "Torus", "Basic", "torus", "Ring"},
    {"ramp", "Ramp", "Basic", "ramp", "Wedge to drive or roll up"},
    {"empty", "Empty Actor", "Basic", "empty", "Transform only: a parent or pivot"},
    {"point_light", "Point Light", "Lights", "light", "Coloured light with a radius"},
    {"physics_cube", "Physics Cube", "Physics", "cube", "Dynamic Box3D box"},
    {"physics_sphere", "Physics Sphere", "Physics", "sphere", "Dynamic ball"},
    {"physics_barrel", "Physics Barrel", "Physics", "cylinder", "Dynamic cylinder (convex hull)"},
    {"crate_stack", "Crate Stack", "Physics", "stack", "Pyramid of 10 crates"},
    {"pendulum", "Pendulum", "Physics", "pendulum", "Ball on a distance joint"},
    {"hinged_door", "Hinged Door", "Physics", "door", "Door on a sprung hinge with limits"},
    {"chain", "Chain", "Physics", "chain", "8 links on ball joints"},
    {"see_saw", "See-Saw", "Physics", "seesaw", "Plank on a hinge"},
    {"hover_car", "Hover Car", "Gameplay", "car", "Playable hover car (Box3D)"},
    {"rolling_ball", "Rolling Ball", "Gameplay", "ball", "Playable ball with jump"},
    {"character", "Character", "Gameplay", "person", "Playable capsule character"},
    {"drone", "Drone", "Gameplay", "drone", "Playable flyer"},
    {"orb", "Orb", "Gameplay", "orb", "Glowing pickup worth 1 point"},
    {"star", "Star", "Gameplay", "star", "Spinning pickup worth 10 points"},
    {"goal", "Goal", "Gameplay", "goal", "Trigger that wins the game"},
    {"hazard", "Hazard", "Gameplay", "hazard", "Trigger that loses the game"},
    {"checkpoint", "Checkpoint", "Gameplay", "flag", "Respawn point"},
    {"message", "Message Trigger", "Gameplay", "message", "Shows a text when touched"},
    {"moving_platform", "Moving Platform", "Gameplay", "platform", "Kinematic platform on a mover"},
    {"spinner", "Spinner", "Gameplay", "spinner", "Rotating kinematic bar"},
    {"city", "Procedural City", "Procedural", "city", "Blocks, streets, trees and traffic"},
};

game::Actor shape_actor(const char* name, game::Shape shape, Vec3 color) {
  game::Actor a;
  a.name = name;
  a.mesh = game::MeshComponent{};
  a.mesh->shape = shape;
  a.mesh->material.color = color;
  return a;
}

game::Actor dynamic(game::Actor a, f32 mass = 0) {
  a.body = game::BodyComponent{};
  a.body->mass = mass;
  return a;
}

}  // namespace

json::Value Studio::catalog_json() const {
  json::Value groups = json::Value::array();
  const char* order[] = {"Basic", "Lights", "Physics", "Gameplay", "Procedural"};
  for (const char* g : order) {
    json::Value group = json::Value::object();
    group.set("label", g);
    json::Value items = json::Value::array();
    for (const Item& it : kItems) {
      if (std::strcmp(it.group, g) != 0) continue;
      json::Value x = json::Value::object();
      x.set("id", it.id);
      x.set("label", it.label);
      x.set("icon", it.icon);
      x.set("help", it.help);
      items.push(std::move(x));
    }
    group.set("items", std::move(items));
    groups.push(std::move(group));
  }
  return groups;
}

ActorId Studio::spawn(const std::string& item, std::optional<Vec3> at) {
  if (play_ != PlayState::editing) { report("Stop playing to add actors", "warn"); return 0; }
  // Where: the drop point, else what the active camera looks at, else 10 m ahead.
  Vec3 pos;
  if (at) pos = *at;
  else {
    Viewport& v = views_[active_view_];
    Vec3 hit;
    if (pick(v.index, v.css_width * 0.5f, v.css_height * 0.5f, &hit) && distance(hit, camera_of(v).eye) < 200) pos = hit;
    else if (v.type == ViewType::perspective) {
      const Vec3 f = v.camera.forward();
      pos = v.camera.position + f * 10;
      if (f.z < -0.05f) {
        const f32 t = -v.camera.position.z / f.z;
        if (t > 0 && t < 60) pos = v.camera.position + f * t;
      }
    } else pos = v.camera.ortho_center;
  }
  if (snap_.grid) { pos.x = internal::snap(pos.x, snap_.grid_size); pos.y = internal::snap(pos.y, snap_.grid_size); }
  if (pos.z < 0 && level_.world.ground) pos.z = 0;
  std::vector<game::Actor> actors;  // [0] is the root; others reference it through parent = 1 (index + 1)
  auto add = [&](game::Actor a, Vec3 offset, i32 parent_index = -1) {
    a.transform.position = offset;
    a.parent = parent_index >= 0 ? static_cast<ActorId>(parent_index + 1) : 0;
    actors.push_back(std::move(a));
    return static_cast<i32>(actors.size() - 1);
  };
  const Vec3 grey{0.72f, 0.72f, 0.74f};
  if (item == "cube") add(shape_actor("Cube", game::Shape::box, grey), {0, 0, 0.5f});
  else if (item == "sphere") add(shape_actor("Sphere", game::Shape::sphere, grey), {0, 0, 0.5f});
  else if (item == "cylinder") { game::Actor a = shape_actor("Cylinder", game::Shape::cylinder, grey); add(a, {0, 0, 0.5f}); }
  else if (item == "cone") add(shape_actor("Cone", game::Shape::cone, grey), {0, 0, 0.5f});
  else if (item == "capsule") { game::Actor a = shape_actor("Capsule", game::Shape::capsule, grey); a.mesh->height = 2; add(a, {0, 0, 1}); }
  else if (item == "plane") { game::Actor a = shape_actor("Plane", game::Shape::plane, {0.6f, 0.62f, 0.64f}); a.mesh->size = {10, 10, 1}; add(a, {0, 0, 0.01f}); }
  else if (item == "torus") { game::Actor a = shape_actor("Torus", game::Shape::torus, grey); a.mesh->height = 0.2f; add(a, {0, 0, 0.25f}); }
  else if (item == "ramp") { game::Actor a = shape_actor("Ramp", game::Shape::ramp, {0.62f, 0.6f, 0.55f}); a.mesh->size = {6, 3, 1.5f}; add(a, {0, 0, 0.75f}); }
  else if (item == "empty") { game::Actor a; a.name = "Empty"; add(a, {0, 0, 1}); }
  else if (item == "point_light") { game::Actor a; a.name = "Point Light"; a.light = game::LightComponent{}; add(a, {0, 0, 3}); }
  else if (item == "physics_cube") add(dynamic(shape_actor("Physics Cube", game::Shape::box, {0.85f, 0.55f, 0.3f})), {0, 0, 0.5f});
  else if (item == "physics_sphere") add(dynamic(shape_actor("Physics Sphere", game::Shape::sphere, {0.35f, 0.6f, 0.9f})), {0, 0, 0.5f});
  else if (item == "physics_barrel") {
    game::Actor a = dynamic(shape_actor("Physics Barrel", game::Shape::cylinder, {0.7f, 0.3f, 0.2f}));
    a.mesh->radius = 0.4f;
    a.mesh->height = 1.1f;
    add(a, {0, 0, 0.55f});
  } else if (item == "crate_stack") {
    game::Actor root;
    root.name = "Crate Stack";
    const i32 r = add(root, {0, 0, 0});
    int n = 0;
    for (int row = 0; row < 4; ++row)
      for (int k = 0; k < 4 - row; ++k) {
        char name[32];
        std::snprintf(name, sizeof name, "Crate %d", ++n);
        game::Actor c = dynamic(shape_actor(name, game::Shape::box, {0.75f, 0.55f, 0.32f}));
        c.body->start_awake = false;
        add(c, {(k - (3 - row) * 0.5f) * 1.05f, 0, 0.5f + row * 1.0f}, r);
      }
  } else if (item == "pendulum") {
    game::Actor beam = shape_actor("Pendulum Beam", game::Shape::box, {0.3f, 0.32f, 0.36f});
    beam.mesh->size = {0.3f, 0.3f, 0.3f};
    beam.mesh->material.metalness = 0.7f;
    const i32 b = add(beam, {0, 0, 5});
    game::Actor ball = dynamic(shape_actor("Pendulum Ball", game::Shape::sphere, {0.85f, 0.2f, 0.2f}), 50);
    ball.mesh->radius = 0.5f;
    game::JointComponent rope;
    rope.kind = game::JointKind::distance;
    rope.target = static_cast<ActorId>(b + 1);
    ball.joints.push_back(rope);
    add(ball, {3, 0, 2}, b);
  } else if (item == "hinged_door") {
    game::Actor frame = shape_actor("Door Frame", game::Shape::box, {0.35f, 0.25f, 0.18f});
    frame.mesh->size = {0.25f, 0.25f, 2.6f};
    const i32 f = add(frame, {0, 0, 1.3f});
    game::Actor door = dynamic(shape_actor("Door", game::Shape::box, {0.6f, 0.42f, 0.28f}), 30);
    door.mesh->size = {1.8f, 0.1f, 2.4f};
    game::JointComponent h;
    h.kind = game::JointKind::hinge;
    h.target = static_cast<ActorId>(f + 1);
    h.anchor = {-0.95f, 0, 0};
    h.enable_limit = true;
    h.lower = -100;
    h.upper = 100;
    h.enable_spring = true;
    h.spring_hertz = 0.6f;
    h.spring_damping = 0.3f;
    door.joints.push_back(h);
    add(door, {1.05f, 0, 0}, f);
  } else if (item == "chain") {
    game::Actor anchor = shape_actor("Chain Anchor", game::Shape::sphere, {0.3f, 0.3f, 0.32f});
    anchor.mesh->radius = 0.2f;
    i32 prev = add(anchor, {0, 0, 6});
    const i32 root = prev;
    for (int i = 0; i < 8; ++i) {
      char name[32];
      std::snprintf(name, sizeof name, "Link %d", i + 1);
      game::Actor link = dynamic(shape_actor(name, game::Shape::capsule, {0.6f, 0.6f, 0.65f}), 4);
      link.mesh->radius = 0.12f;
      link.mesh->height = 0.6f;
      link.mesh->material.metalness = 0.8f;
      game::JointComponent j;
      j.kind = game::JointKind::ball;
      j.target = static_cast<ActorId>(prev + 1);
      j.anchor = {0, 0, 0.3f};
      link.joints.push_back(j);
      prev = add(link, {0, 0, -0.3f - 0.6f * i}, root);
    }
  } else if (item == "see_saw") {
    game::Actor pivot = shape_actor("See-Saw Pivot", game::Shape::ramp, {0.4f, 0.42f, 0.45f});
    pivot.mesh->size = {0.8f, 0.8f, 0.6f};
    pivot.transform.rotation = {0, 0, 0};
    const i32 p = add(pivot, {0, 0, 0.3f});
    game::Actor plank = dynamic(shape_actor("See-Saw Plank", game::Shape::box, {0.6f, 0.45f, 0.3f}), 20);
    plank.mesh->size = {6, 0.8f, 0.12f};
    game::JointComponent h;
    h.kind = game::JointKind::hinge;
    h.target = static_cast<ActorId>(p + 1);
    h.axis = {0, 1, 0};
    h.enable_limit = true;
    h.lower = -20;
    h.upper = 20;
    plank.joints.push_back(h);
    add(plank, {0, 0, 0.4f}, p);
  } else if (item == "hover_car") {
    game::Actor car = shape_actor("Hover Car", game::Shape::box, {0.9f, 0.9f, 0.92f});
    car.mesh->size = {4.2f, 2.0f, 0.55f};
    car.mesh->collision = false;
    car.mesh->material.roughness = 0.35f;
    car.mesh->material.metalness = 0.3f;
    car = dynamic(car, 1200);
    car.body->allow_sleep = false;
    car.body->angular_damping = 0.5f;
    game::ColliderComponent hull;
    hull.shape = game::ColliderShape::box;
    hull.size = {4.2f, 2.0f, 0.9f};
    hull.center = {0, 0, 0.15f};
    hull.friction = 0.25f;
    car.colliders.push_back(hull);
    car.controller = game::ControllerComponent{};
    const i32 c = add(car, {0, 0, 1});
    game::Actor cabin = shape_actor("Cabin", game::Shape::box, {0.12f, 0.16f, 0.22f});
    cabin.mesh->size = {2.0f, 1.7f, 0.55f};
    cabin.mesh->collision = false;
    add(cabin, {-0.3f, 0, 0.5f}, c);
    game::Actor light;
    light.name = "Headlight";
    light.light = game::LightComponent{{1, 0.95f, 0.85f}, 10, 18};
    add(light, {6, 0, 1.2f}, c);
  } else if (item == "rolling_ball") {
    game::Actor ball = dynamic(shape_actor("Player Ball", game::Shape::sphere, {0.95f, 0.75f, 0.3f}), 30);
    ball.mesh->radius = 0.6f;
    ball.body->allow_sleep = false;
    game::ControllerComponent c;
    c.mode = game::ControlMode::rolling;
    c.max_speed = 14;
    c.acceleration = 26;
    c.boost = false;
    c.camera_distance = 9;
    c.camera_height = 3;
    ball.controller = c;
    add(ball, {0, 0, 0.6f});
  } else if (item == "character") {
    game::Actor body = dynamic(shape_actor("Character", game::Shape::capsule, {0.3f, 0.55f, 0.85f}), 75);
    body.mesh->radius = 0.4f;
    body.mesh->height = 1.8f;
    body.body->allow_sleep = false;
    game::ControllerComponent c;
    c.mode = game::ControlMode::character;
    c.max_speed = 6;
    c.boost_speed = 11;
    c.jump_speed = 5.5f;
    c.camera_distance = 6;
    c.camera_height = 2.5f;
    body.controller = c;
    add(body, {0, 0, 0.9f});
  } else if (item == "drone") {
    game::Actor drone = dynamic(shape_actor("Drone", game::Shape::box, {0.25f, 0.27f, 0.3f}), 3);
    drone.mesh->size = {0.6f, 0.6f, 0.18f};
    drone.body->allow_sleep = false;
    game::ControllerComponent c;
    c.mode = game::ControlMode::flyer;
    c.max_speed = 14;
    c.boost_speed = 28;
    c.acceleration = 30;
    c.camera_distance = 5;
    c.camera_height = 1.5f;
    drone.controller = c;
    const i32 d = add(drone, {0, 0, 2});
    for (int i = 0; i < 4; ++i) {
      game::Actor rotor = shape_actor(i == 0 ? "Rotor 1" : i == 1 ? "Rotor 2" : i == 2 ? "Rotor 3" : "Rotor 4", game::Shape::cylinder, {0.15f, 0.8f, 1.0f});
      rotor.mesh->radius = 0.2f;
      rotor.mesh->height = 0.03f;
      rotor.mesh->collision = false;
      rotor.mesh->material.emissive = {0.2f, 0.8f, 1.0f};
      rotor.mesh->material.emissive_strength = 1;
      add(rotor, {i & 1 ? 0.38f : -0.38f, i & 2 ? 0.38f : -0.38f, 0.12f}, d);
    }
  } else if (item == "orb") {
    game::Actor orb = shape_actor("Orb", game::Shape::sphere, {0.3f, 1.0f, 0.9f});
    orb.mesh->radius = 0.6f;
    orb.mesh->collision = false;
    orb.mesh->material.emissive = {0.25f, 1.0f, 0.9f};
    orb.mesh->material.emissive_strength = 2.4f;
    orb.mesh->material.cast_shadow = false;
    orb.light = game::LightComponent{{0.3f, 1.0f, 0.9f}, 10, 12};
    orb.pickup = game::PickupComponent{};
    orb.rotator = game::RotatorComponent{{0, 0, 57}};
    orb.mover = game::MoverComponent{{0, 0, 0.5f}, 2.8f, 0, game::MoverMode::sine};
    game::ColliderComponent s;
    s.shape = game::ColliderShape::sphere;
    s.radius = 1.6f;
    s.sensor = true;
    orb.colliders.push_back(s);
    add(orb, {0, 0, 1.4f});
  } else if (item == "star") {
    game::Actor s = shape_actor("Star", game::Shape::torus, {1.0f, 0.85f, 0.3f});
    s.mesh->radius = 0.45f;
    s.mesh->height = 0.14f;
    s.mesh->collision = false;
    s.mesh->material.emissive = {1.0f, 0.75f, 0.25f};
    s.mesh->material.emissive_strength = 1.5f;
    s.pickup = game::PickupComponent{10, {1.0f, 0.85f, 0.4f}, 0};
    s.rotator = game::RotatorComponent{{0, 0, 120}};
    s.mover = game::MoverComponent{{0, 0, 0.4f}, 1.6f, 0, game::MoverMode::sine};
    add(s, {0, 0, 1.2f});
  } else if (item == "goal") {
    game::Actor g = shape_actor("Goal", game::Shape::torus, {0.3f, 0.95f, 0.5f});
    g.mesh->radius = 1.5f;
    g.mesh->height = 0.25f;
    g.mesh->collision = false;
    g.mesh->material.emissive = {0.3f, 1, 0.5f};
    g.mesh->material.emissive_strength = 1.2f;
    g.transform.rotation = {90, 0, 0};
    g.trigger = game::TriggerComponent{};
    game::ColliderComponent s;
    s.shape = game::ColliderShape::sphere;
    s.radius = 1.4f;
    s.sensor = true;
    g.colliders.push_back(s);
    add(g, {0, 0, 1.6f});
  } else if (item == "hazard") {
    game::Actor h = shape_actor("Hazard", game::Shape::box, {0.9f, 0.15f, 0.12f});
    h.mesh->size = {3, 3, 0.2f};
    h.mesh->material.emissive = {1, 0.15f, 0.1f};
    h.mesh->material.emissive_strength = 0.8f;
    h.trigger = game::TriggerComponent{game::TriggerAction::hazard, "", true};
    game::ColliderComponent s;
    s.shape = game::ColliderShape::box;
    s.size = {3, 3, 0.6f};
    s.sensor = true;
    h.colliders.push_back(s);
    h.mesh->collision = false;
    add(h, {0, 0, 0.1f});
  } else if (item == "checkpoint") {
    game::Actor c = shape_actor("Checkpoint", game::Shape::cylinder, {0.3f, 0.6f, 1.0f});
    c.mesh->radius = 1.2f;
    c.mesh->height = 0.1f;
    c.mesh->collision = false;
    c.mesh->material.emissive = {0.3f, 0.6f, 1.0f};
    c.mesh->material.emissive_strength = 0.8f;
    c.trigger = game::TriggerComponent{game::TriggerAction::checkpoint, "", true};
    game::ColliderComponent s;
    s.shape = game::ColliderShape::box;
    s.size = {2.4f, 2.4f, 3};
    s.center = {0, 0, 1.5f};
    s.sensor = true;
    c.colliders.push_back(s);
    add(c, {0, 0, 0.05f});
  } else if (item == "message") {
    game::Actor m;
    m.name = "Message Trigger";
    m.trigger = game::TriggerComponent{game::TriggerAction::message, "Hello from Kern Studio!", true};
    game::ColliderComponent s;
    s.shape = game::ColliderShape::box;
    s.size = {3, 3, 3};
    s.sensor = true;
    m.colliders.push_back(s);
    add(m, {0, 0, 1.5f});
  } else if (item == "moving_platform") {
    game::Actor p = shape_actor("Moving Platform", game::Shape::box, {0.35f, 0.5f, 0.7f});
    p.mesh->size = {3, 3, 0.3f};
    p.body = game::BodyComponent{};
    p.body->motion = game::Motion::kinematic;
    p.mover = game::MoverComponent{{0, 0, 3}, 4, 0, game::MoverMode::sine};
    add(p, {0, 0, 0.5f});
  } else if (item == "spinner") {
    game::Actor s = shape_actor("Spinner", game::Shape::box, {0.85f, 0.4f, 0.15f});
    s.mesh->size = {6, 0.4f, 0.4f};
    s.body = game::BodyComponent{};
    s.body->motion = game::Motion::kinematic;
    s.rotator = game::RotatorComponent{{0, 0, 90}};
    add(s, {0, 0, 0.6f});
  } else if (item == "city") {
    game::Actor c;
    c.name = "City";
    c.city = game::CityComponent{};
    add(c, {0, 0, 0});
  } else {
    report("Unknown actor type " + item, "error");
    return 0;
  }
  // Insert with fresh ids; references inside the prefab (parent, joint targets) use index + 1.
  ActorId root = 0;
  std::vector<ActorId> fresh;
  edit(("Add " + actors[0].name).c_str(), [&](game::Level& l) {
    std::vector<ActorId> ids;
    for (std::size_t i = 0; i < actors.size(); ++i) ids.push_back(l.next_id + static_cast<ActorId>(i));
    for (std::size_t i = 0; i < actors.size(); ++i) {
      game::Actor a = actors[i];
      a.id = ids[i];
      if (i == 0) {
        a.transform.position = a.transform.position + pos;
        // Spawned names are unique.
        std::string base = a.name;
        int n = 2;
        auto taken = [&](const std::string& s) { for (const game::Actor& x : l.actors) if (x.name == s) return true; return false; };
        while (taken(a.name)) a.name = base + " " + std::to_string(n++);
      }
      if (a.parent) a.parent = ids[a.parent - 1];
      for (game::JointComponent& j : a.joints) if (j.target) j.target = ids[j.target - 1];
      l.add(std::move(a));
    }
    root = ids[0];
    select({root});
    return true;
  });
  return root;
}

}  // namespace kern::studio
