// Kern Logic: gameplay nodes. Every node is a C++ function on the GameWorld;
// they register themselves into the logic library at start-up.
#include <cmath>
#include <cstdlib>

#include "kern/game/runtime.h"
#include "kern/logic/logic.h"

namespace kern::game {

namespace {

using logic::Context;
using logic::NodeDef;
using logic::NodeKind;
using logic::ParamDef;
using logic::PinDef;
using logic::PinType;
using logic::Value;
using logic::kStop;

constexpr u32 kEventColor = 0xb4232f;

GameWorld* world(Context& c) { return static_cast<GameWorld*>(c.host().game()); }

PinDef exec_in(std::string id = "exec", std::string label = "") { return {std::move(id), std::move(label), PinType::exec, {}}; }
PinDef exec_out(std::string id = "then", std::string label = "") { return {std::move(id), std::move(label), PinType::exec, {}}; }
PinDef data(std::string id, std::string label, PinType t, json::Value def = {}) { return {std::move(id), std::move(label), t, std::move(def)}; }
json::Value vec3(f32 x, f32 y, f32 z) {
  json::Value a = json::Value::array();
  a.push(static_cast<f64>(x));
  a.push(static_cast<f64>(y));
  a.push(static_cast<f64>(z));
  return a;
}
ParamDef param(std::string key, std::string label, ParamDef::Kind kind, std::string def = {}, std::vector<std::string> choices = {}) {
  ParamDef p;
  p.key = std::move(key);
  p.label = std::move(label);
  p.kind = kind;
  p.def = std::move(def);
  p.choices = std::move(choices);
  return p;
}
NodeDef make(std::string type, std::string label, std::string category, NodeKind kind, std::string help, std::string keywords = {}) {
  NodeDef d;
  d.type = std::move(type);
  d.label = std::move(label);
  d.category = std::move(category);
  d.kind = kind;
  d.help = std::move(help);
  d.keywords = std::move(keywords);
  return d;
}
/// An action node: exec in -> run -> exec out.
NodeDef action(std::string type, std::string label, std::string category, std::vector<PinDef> in, std::vector<PinDef> out, std::function<void(Context&, GameWorld&)> fn,
               std::string help, std::string keywords = {}) {
  NodeDef d = make(std::move(type), std::move(label), std::move(category), NodeKind::action, std::move(help), std::move(keywords));
  d.inputs = {exec_in()};
  for (PinDef& p : in) d.inputs.push_back(std::move(p));
  d.outputs = {exec_out()};
  for (PinDef& p : out) d.outputs.push_back(std::move(p));
  d.run = [fn = std::move(fn)](Context& c) {
    if (GameWorld* w = world(c)) fn(c, *w);
    return 0;
  };
  return d;
}
/// A pure node reading the world.
NodeDef getter(std::string type, std::string label, std::string category, std::vector<PinDef> in, std::vector<PinDef> out, std::function<void(Context&, GameWorld&)> fn,
               std::string help, std::string keywords = {}) {
  NodeDef d = make(std::move(type), std::move(label), std::move(category), NodeKind::pure, std::move(help), std::move(keywords));
  d.inputs = std::move(in);
  d.outputs = std::move(out);
  d.run = [fn = std::move(fn)](Context& c) {
    if (GameWorld* w = world(c)) fn(c, *w);
    return kStop;
  };
  return d;
}
/// An event node: `event` is the GameWorld event type; outputs are filled from its arguments.
NodeDef event(std::string type, std::string label, std::string channel, std::vector<PinDef> out, std::string help, std::string keywords = {}) {
  NodeDef d = make(std::move(type), std::move(label), "Events", NodeKind::event, std::move(help), std::move(keywords));
  d.event = std::move(channel);
  d.outputs = {exec_out()};
  for (PinDef& p : out) d.outputs.push_back(std::move(p));
  d.color = kEventColor;
  d.accepts = [](const logic::Node&, const logic::Event&) { return true; };
  return d;
}

/// The actor input, defaulting to Self when unlinked and 0.
ActorId target(Context& c, std::string_view pin = "target") {
  const logic::ActorRef a = c.in_actor(pin);
  return a ? a : c.self();
}

Vec3 actor_position(GameWorld& w, ActorId id) {
  Mat4 m;
  return w.actor_matrix(id, m) ? m.translation() : Vec3{};
}

std::string key_label(const std::string& code) {
  if (code.rfind("Key", 0) == 0 && code.size() == 4) return code.substr(3);
  if (code.rfind("Digit", 0) == 0 && code.size() == 6) return code.substr(5);
  return code;
}

/// "F" -> "KeyF", "1" -> "Digit1", "Space" stays.
std::string key_code(std::string k) {
  if (k.size() == 1 && std::isalpha(static_cast<unsigned char>(k[0]))) return std::string("Key") + static_cast<char>(std::toupper(static_cast<unsigned char>(k[0])));
  if (k.size() == 1 && std::isdigit(static_cast<unsigned char>(k[0]))) return "Digit" + k;
  return k;
}

const std::vector<std::string> kActions = {"jump", "Jump", "fire", "Fire", "aim", "Aim", "reload", "Reload", "crouch", "Crouch", "interact", "Interact", "boost", "Sprint / boost"};

void add_events(logic::Library& lib) {
  const PinDef actor_out = data("actor", "Actor", PinType::actor), other_out = data("other", "Other Actor", PinType::actor);
  lib.add(event("game.overlap_begin", "On Begin Overlap", "game.overlap_begin", {actor_out, other_out},
                "Something entered this actor's sensor collider (a trigger volume). Other Actor is who came in.", "trigger enter touch volume"));
  lib.add(event("game.overlap_end", "On End Overlap", "game.overlap_end", {actor_out, other_out}, "Something left this actor's sensor collider.", "trigger leave exit"));
  lib.add(event("game.touched_sensor", "On Entered Sensor", "game.overlapped_begin", {actor_out, data("other", "Sensor Actor", PinType::actor)},
                "This actor (e.g. a player) walked into someone else's sensor.", "overlap volume"));
  lib.add(event("game.hit", "On Hit", "game.hit",
                {actor_out, other_out, data("speed", "Impact Speed", PinType::number), data("point", "Point", PinType::vector), data("normal", "Normal", PinType::vector)},
                "This actor's body hit something hard enough (Box3D hit event).", "collide collision bump impact"));
  lib.add(event("game.pickup", "On Collected", "game.pickup", {actor_out, data("other", "Collector", PinType::actor), data("points", "Points", PinType::number)},
                "A pickup was collected.", "pickup orb"));
  lib.add(event("game.trigger", "On Trigger", "game.trigger", {actor_out, data("other", "Player", PinType::actor)}, "A player touched this trigger.", "goal checkpoint"));
  lib.add(event("game.damaged", "On Damaged", "game.damaged",
                {actor_out, data("other", "Attacker", PinType::actor), data("amount", "Damage", PinType::number), data("head", "Headshot", PinType::boolean)},
                "This actor took damage (needs a Health component).", "hurt shot"));
  lib.add(event("game.death", "On Death", "game.death", {actor_out, data("other", "Killer", PinType::actor), data("head", "Headshot", PinType::boolean)},
                "This actor's health reached 0.", "die killed destroyed"));
  lib.add(event("game.respawn", "On Respawn", "game.respawn", {actor_out}, "This actor came back after dying.", "revive"));
  lib.add(event("game.interact", "On Interact", "game.interact", {actor_out, data("other", "Player", PinType::actor), data("point", "Point", PinType::vector)},
                "A player pressed Interact (F / Y on the gamepad) looking at this actor or standing next to it.", "use button press open"));
  lib.add(event("game.pawn_added", "On Player Joined", "game.pawn_added", {data("actor", "Player", PinType::actor)}, "A player joined (multiplayer).", "connect spawn"));
  {
    NodeDef d = event("game.over", "On Game Over", "game.over", {data("won", "Won", PinType::boolean), data("message", "Message", PinType::text)}, "The game was won or lost.", "end win lose");
    lib.add(std::move(d));
  }
  {
    NodeDef d = event("game.input", "On Input Action", "game.input", {data("actor", "Player", PinType::actor), data("pressed", "Pressed", PinType::boolean)},
                      "A player pressed or released an action (works for every player, on every device: keyboard, gamepad, touch).", "button control press");
    d.params = {param("action", "Action", ParamDef::Kind::choice, "interact", kActions),
                param("edge", "When", ParamDef::Kind::choice, "pressed", {"pressed", "Pressed", "released", "Released", "both", "Both"})};
    d.title = [](const NodeDef&, const logic::Node& n) {
      std::string a = n.param("action", "interact");
      if (!a.empty()) a[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(a[0])));
      const std::string e = n.param("edge", "pressed");
      return "On " + a + (e == "released" ? " Released" : e == "both" ? "" : " Pressed");
    };
    d.accepts = [](const logic::Node& n, const logic::Event& e) {
      if (e.key != n.param("action", "interact")) return false;
      const std::string edge = n.param("edge", "pressed");
      const logic::Value* p = e.arg("pressed");
      const bool pressed = p && p->as_bool();
      return edge == "both" || (edge == "pressed") == pressed;
    };
    lib.add(std::move(d));
  }
  {
    NodeDef d = event("game.key", "On Key", "game.key", {data("pressed", "Pressed", PinType::boolean), data("actor", "Player", PinType::actor)},
                      "A key of this computer's keyboard (only the local player; use On Input Action for gamepads and multiplayer).", "keyboard");
    d.params = {param("key", "Key", ParamDef::Kind::key, "F"), param("edge", "When", ParamDef::Kind::choice, "pressed", {"pressed", "Pressed", "released", "Released", "both", "Both"})};
    d.title = [](const NodeDef&, const logic::Node& n) { return "On Key " + key_label(key_code(n.param("key", "F"))); };
    d.accepts = [](const logic::Node& n, const logic::Event& e) {
      if (e.key != key_code(n.param("key", "F"))) return false;
      const std::string edge = n.param("edge", "pressed");
      const logic::Value* p = e.arg("pressed");
      const bool pressed = p && p->as_bool();
      return edge == "both" || (edge == "pressed") == pressed;
    };
    lib.add(std::move(d));
  }
}

void add_actor_nodes(logic::Library& lib) {
  const json::Value zero3 = vec3(0, 0, 0);
  lib.add(getter("actor.self", "Self", "Actors", {}, {data("actor", "Self", PinType::actor)}, [](Context& c, GameWorld&) { c.out("actor", Value::actor(c.self())); },
                 "The actor this graph belongs to (none in the level graph).", "this me owner"));
  {
    NodeDef d = getter("actor.player", "Get Player", "Actors", {data("index", "Player Index", PinType::integer, 0)}, {data("actor", "Player", PinType::actor)},
                       [](Context& c, GameWorld& w) {
                         const i64 i = c.in_int("index");
                         const std::vector<ActorId> p = w.pawns();
                         ActorId a = i == 0 ? w.player() : 0;
                         if (!a && i >= 0 && static_cast<std::size_t>(i) < p.size()) a = p[static_cast<std::size_t>(i)];
                         c.out("actor", Value::actor(a));
                       },
                       "The player's actor (0 = this machine's player; 1, 2... other players).", "pawn character me");
    lib.add(std::move(d));
  }
  lib.add(getter("actor.player_count", "Player Count", "Actors", {}, {data("count", "Count", PinType::integer)},
                 [](Context& c, GameWorld& w) { c.out("count", Value::integer(static_cast<i64>(w.pawns().size()))); }, "How many players are in the game.", "players number"));
  lib.add(getter("actor.find", "Find Actor by Name", "Actors", {data("name", "Name", PinType::text, "Cube")}, {data("actor", "Actor", PinType::actor)},
                 [](Context& c, GameWorld& w) { c.out("actor", Value::actor(w.find_actor(c.in_text("name")))); }, "The actor with this name (as in the Outliner).", "get lookup named"));
  lib.add(getter("actor.find_tag", "Find Actor with Tag", "Actors", {data("tag", "Tag", PinType::text, "target")}, {data("actor", "Actor", PinType::actor), data("count", "Count", PinType::integer)},
                 [](Context& c, GameWorld& w) {
                   std::vector<ActorId> v;
                   w.actors_with_tag(c.in_text("tag"), v);
                   c.out("actor", Value::actor(v.empty() ? 0 : v.front()));
                   c.out("count", Value::integer(static_cast<i64>(v.size())));
                 },
                 "The first living actor with the tag, and how many have it.", "get tagged"));
  lib.add(getter("actor.random_tag", "Random Actor with Tag", "Actors", {data("tag", "Tag", PinType::text, "spawn")}, {data("actor", "Actor", PinType::actor)},
                 [](Context& c, GameWorld& w) {
                   std::vector<ActorId> v;
                   w.actors_with_tag(c.in_text("tag"), v);
                   c.out("actor", Value::actor(v.empty() ? 0 : v[c.host().random() % v.size()]));
                 },
                 "One living actor with the tag, chosen at random.", "pick"));
  lib.add(getter("actor.has_tag", "Has Tag", "Actors", {data("target", "Actor", PinType::actor, 0), data("tag", "Tag", PinType::text, "enemy")}, {data("result", "Result", PinType::boolean)},
                 [](Context& c, GameWorld& w) { c.out("result", Value::boolean(w.has_tag(target(c), c.in_text("tag")))); }, "Does the actor carry the tag?", "is"));
  lib.add(getter("actor.name", "Get Name", "Actors", {data("target", "Actor", PinType::actor, 0)}, {data("name", "Name", PinType::text)},
                 [](Context& c, GameWorld& w) {
                   const Actor* a = w.level().find(target(c));
                   c.out("name", Value::text(a ? a->name : std::string()));
                 },
                 "The actor's name.", "display"));
  lib.add(getter("actor.valid", "Is Valid", "Actors", {data("target", "Actor", PinType::actor, 0)}, {data("result", "Valid", PinType::boolean)},
                 [](Context& c, GameWorld& w) {
                   const ActorId id = c.in_actor("target");
                   c.out("result", Value::boolean(id && w.level().find(id) && w.actor_alive(id)));
                 },
                 "True for an existing, living actor.", "exists alive none null"));
  lib.add(getter("actor.location", "Get Location", "Actors", {data("target", "Actor", PinType::actor, 0)}, {data("location", "Location", PinType::vector)},
                 [](Context& c, GameWorld& w) { c.out("location", Value::vector(actor_position(w, target(c)))); }, "World position of the actor.", "position where"));
  lib.add(getter("actor.rotation", "Get Rotation", "Actors", {data("target", "Actor", PinType::actor, 0)}, {data("rotation", "Rotation", PinType::vector)},
                 [](Context& c, GameWorld& w) {
                   Mat4 m;
                   c.out("rotation", Value::vector(w.actor_matrix(target(c), m) ? matrix_transform(m).rotation : Vec3{}));
                 },
                 "World rotation in degrees (X roll, Y pitch, Z yaw).", "orientation angle"));
  lib.add(getter("actor.forward", "Get Forward Vector", "Actors", {data("target", "Actor", PinType::actor, 0)}, {data("forward", "Forward", PinType::vector)},
                 [](Context& c, GameWorld& w) {
                   Mat4 m;
                   c.out("forward", Value::vector(w.actor_matrix(target(c), m) ? normalize(m.transform_vector({1, 0, 0}), {1, 0, 0}) : Vec3{1, 0, 0}));
                 },
                 "The direction the actor faces (its X axis).", "direction facing"));
  lib.add(getter("actor.velocity", "Get Velocity", "Actors", {data("target", "Actor", PinType::actor, 0)}, {data("velocity", "Velocity", PinType::vector), data("speed", "Speed", PinType::number)},
                 [](Context& c, GameWorld& w) {
                   const Vec3 v = w.actor_velocity(target(c));
                   c.out("velocity", Value::vector(v));
                   c.out("speed", Value::number(length(v)));
                 },
                 "Linear velocity (m/s) and speed.", "speed motion"));
  lib.add(getter("actor.distance", "Distance Between", "Actors", {data("a", "A", PinType::actor, 0), data("b", "B", PinType::actor, 0)}, {data("distance", "Distance", PinType::number)},
                 [](Context& c, GameWorld& w) { c.out("distance", Value::number(distance(actor_position(w, target(c, "a")), actor_position(w, c.in_actor("b"))))); },
                 "Metres between two actors (A defaults to Self).", "near far range"));
  lib.add(getter("actor.health", "Get Health", "Combat", {data("target", "Actor", PinType::actor, 0)}, {data("health", "Health", PinType::number), data("dead", "Is Dead", PinType::boolean)},
                 [](Context& c, GameWorld& w) {
                   const f32 h = w.health_of(target(c));
                   c.out("health", Value::number(h));
                   c.out("dead", Value::boolean(h == 0));
                 },
                 "Hit points (-1 without a Health component).", "hp life"));
  lib.add(getter("game.score", "Get Score", "Game", {}, {data("score", "Score", PinType::number), data("pickups", "Collected", PinType::integer)},
                 [](Context& c, GameWorld& w) {
                   c.out("score", Value::number(w.hud().score));
                   c.out("pickups", Value::integer(w.hud().pickups));
                 },
                 "Score and collected pickups.", "points"));

  // ---- actions ----
  lib.add(action("actor.set_location", "Set Location", "Actors", {data("target", "Actor", PinType::actor, 0), data("location", "Location", PinType::vector, zero3)}, {},
                 [](Context& c, GameWorld& w) { w.teleport(target(c), c.in_vector("location")); }, "Teleports the actor (bodies included).", "move teleport position"));
  lib.add(action("actor.set_rotation", "Set Rotation", "Actors", {data("target", "Actor", PinType::actor, 0), data("rotation", "Rotation", PinType::vector, zero3)}, {},
                 [](Context& c, GameWorld& w) {
                   const Vec3 r = c.in_vector("rotation");
                   w.teleport(target(c), actor_position(w, target(c)), &r);
                 },
                 "Turns the actor (degrees).", "rotate turn orient"));
  lib.add(action("actor.move_by", "Move By", "Actors", {data("target", "Actor", PinType::actor, 0), data("offset", "Offset", PinType::vector, vec3(0, 0, 1))}, {},
                 [](Context& c, GameWorld& w) { w.teleport(target(c), actor_position(w, target(c)) + c.in_vector("offset")); }, "Moves the actor by an offset.", "translate add offset"));
  lib.add(action("actor.set_velocity", "Set Velocity", "Physics", {data("target", "Actor", PinType::actor, 0), data("velocity", "Velocity", PinType::vector, zero3)}, {},
                 [](Context& c, GameWorld& w) { w.set_actor_velocity(target(c), c.in_vector("velocity")); }, "Sets the linear velocity (players too).", "speed launch"));
  lib.add(action("actor.impulse", "Add Impulse", "Physics", {data("target", "Actor", PinType::actor, 0), data("impulse", "Impulse", PinType::vector, vec3(0, 0, 500))}, {},
                 [](Context& c, GameWorld& w) { w.push_actor(target(c), c.in_vector("impulse"), true); }, "A kick (N·s) to a dynamic body or a player.", "push kick throw"));
  lib.add(action("actor.force", "Add Force", "Physics", {data("target", "Actor", PinType::actor, 0), data("force", "Force", PinType::vector, vec3(0, 0, 1000))}, {},
                 [](Context& c, GameWorld& w) { w.push_actor(target(c), c.in_vector("force"), false); }, "A force (N) for this step; call it every Tick for a steady push.", "thrust wind"));
  lib.add(action("actor.launch", "Launch", "Physics", {data("target", "Actor", PinType::actor, 0), data("velocity", "Velocity", PinType::vector, vec3(0, 0, 10))}, {},
                 [](Context& c, GameWorld& w) {
                   const ActorId t = target(c);
                   w.set_actor_velocity(t, w.actor_velocity(t) * Vec3{1, 1, 0} + c.in_vector("velocity"));
                 },
                 "Throws a player or body (keeps its horizontal speed, adds this).", "jump bounce"));
  lib.add(action("actor.simulate", "Set Simulate Physics", "Physics", {data("target", "Actor", PinType::actor, 0), data("simulate", "Simulate", PinType::boolean, true)}, {},
                 [](Context& c, GameWorld& w) { w.set_simulate_physics(target(c), c.in_bool("simulate")); }, "Lets the body fall (on) or holds it (off).", "dynamic kinematic freeze"));
  lib.add(action("actor.joint_motor", "Set Joint Motor", "Physics",
                 {data("target", "Actor", PinType::actor, 0), data("joint", "Joint Index", PinType::integer, 0), data("enable", "Enable", PinType::boolean, true),
                  data("speed", "Speed (°/s or m/s)", PinType::number, 90.0), data("force", "Max Force", PinType::number, 1000.0)},
                 {}, [](Context& c, GameWorld& w) {
                   w.set_joint_motor(target(c), static_cast<i32>(c.in_int("joint")), c.in_bool("enable"), static_cast<f32>(c.in_number("speed")), static_cast<f32>(c.in_number("force")));
                 },
                 "Drives a hinge, slider or rope of the actor (doors, lifts, winches).", "constraint hinge slider drive"));
  lib.add(action("actor.explode", "Explosion", "Physics",
                 {data("location", "Location", PinType::vector, zero3), data("radius", "Radius", PinType::number, 6.0), data("impulse", "Strength", PinType::number, 6.0),
                  data("damage", "Damage", PinType::number, 50.0)},
                 {}, [](Context& c, GameWorld& w) {
                   const Vec3 at = c.in_vector("location");
                   const f32 r = static_cast<f32>(max_of(0.1, c.in_number("radius")));
                   w.physics().explode(at, r, r * 0.5f, static_cast<f32>(c.in_number("impulse")));
                   w.particle_burst(at, {3.0f, 1.2f, 0.3f});
                   w.play_sound("builtin:explosion", at, 1, 1, true);
                   w.shake_camera(0.6f);
                   const f32 dmg = static_cast<f32>(c.in_number("damage"));
                   if (dmg > 0)
                     for (const Actor& a : w.level().actors) {
                       if (w.health_of(a.id) <= 0) continue;
                       const f32 d = distance(actor_position(w, a.id), at);
                       if (d < r) w.damage(a.id, dmg * (1 - d / r), c.self(), false, at);
                     }
                 },
                 "Box3D radial impulse with flash, sound and damage falling off with distance.", "blast bomb radial"));
  {
    NodeDef d = action("actor.spawn", "Spawn Actor", "Actors",
                       {data("template", "Template", PinType::actor, 0), data("location", "Location", PinType::vector, vec3(0, 0, 1)), data("rotation", "Rotation", PinType::vector, zero3)},
                       {data("spawned", "Spawned", PinType::actor)},
                       [](Context& c, GameWorld& w) {
                         ActorId tpl = c.in_actor("template");
                         if (!tpl) tpl = w.find_actor(c.param("template"));
                         const ActorId id = tpl ? w.spawn_copy(tpl, c.in_vector("location"), c.in_vector("rotation")) : 0;
                         if (!id) c.host().error("Spawn Actor: no template \"" + c.param("template") + "\"");
                         c.out("spawned", Value::actor(id));
                       },
                       "A copy of a template actor (with its children and logic) - replicated to every player.", "create instantiate clone copy");
    d.params = {param("template", "Template name", ParamDef::Kind::text, "")};
    lib.add(std::move(d));
  }
  lib.add(action("actor.destroy", "Destroy Actor", "Actors", {data("target", "Actor", PinType::actor, 0)}, {}, [](Context& c, GameWorld& w) { w.destroy_actor(target(c)); },
                 "Removes the actor for the rest of the game.", "delete remove kill"));
  lib.add(action("actor.enable", "Set Enabled", "Actors", {data("target", "Actor", PinType::actor, 0), data("enabled", "Enabled", PinType::boolean, false)}, {},
                 [](Context& c, GameWorld& w) { w.set_alive(target(c), c.in_bool("enabled")); }, "Hides and disables (or brings back) the actor and its body.", "visible hide show active"));
  lib.add(action("actor.set_property", "Set Property", "Actors",
                 {data("target", "Actor", PinType::actor, 0), data("path", "Property", PinType::text, "components.light.intensity"), data("value", "Value", PinType::text, "20")}, {},
                 [](Context& c, GameWorld& w) {
                   const std::string text = c.in_text("value");
                   json::Value v;
                   if (!json::parse(text, v)) v = json::Value(text);
                   if (!w.set_property(target(c), c.in_text("path"), v)) c.host().error(std::string("Set Property: ") + last_error());
                 },
                 "Changes any component value (the path as in the level file, e.g. components.mesh.material.color) - replicated.", "component field details"));
  lib.add(action("actor.set_light", "Set Light", "Effects",
                 {data("target", "Actor", PinType::actor, 0), data("color", "Color", PinType::vector, vec3(1, 0.9f, 0.7f)), data("intensity", "Intensity", PinType::number, 10.0),
                  data("range", "Range", PinType::number, 10.0)},
                 {}, [](Context& c, GameWorld& w) {
                   const Vec3 col = c.in_vector("color");
                   const ActorId t = target(c);
                   w.set_property(t, "components.light.color", vec3(col.x, col.y, col.z));
                   w.set_property(t, "components.light.intensity", json::Value(c.in_number("intensity")));
                   w.set_property(t, "components.light.range", json::Value(c.in_number("range")));
                 },
                 "Colour (0..1 each), brightness and reach of the actor's point light.", "lamp brightness"));
  lib.add(action("actor.set_color", "Set Color", "Effects", {data("target", "Actor", PinType::actor, 0), data("color", "Color", PinType::vector, vec3(1, 0.2f, 0.2f))}, {},
                 [](Context& c, GameWorld& w) {
                   const Vec3 col = c.in_vector("color");
                   char hex[8];
                   auto b = [](f32 v) { return static_cast<int>(std::lround(clamp(v, 0.0f, 1.0f) * 255)); };
                   std::snprintf(hex, sizeof hex, "#%02x%02x%02x", b(col.x), b(col.y), b(col.z));
                   w.set_property(target(c), "components.mesh.material.color", json::Value(hex));
                 },
                 "Mesh colour (0..1 each).", "material paint tint"));
}

/// Robots (walkers, the Micro Duck): drive them, look, read their ToF and camera. Commands are in
/// the robot's own units (the real duck's m/s and degrees); distances are world metres.
void add_walker_nodes(logic::Library& lib) {
  const json::Value none = json::Value(0.0);
  lib.add(action("walker.move", "Walk", "Robots",
                 {data("target", "Robot", PinType::actor, 0), data("forward", "Forward (m/s)", PinType::number, 0.3), data("strafe", "Sideways (m/s)", PinType::number, 0.0),
                  data("turn", "Turn (°/s)", PinType::number, 0.0), data("duration", "Duration (s, 0 = keep)", PinType::number, 0.0)},
                 {}, [](Context& c, GameWorld& w) {
                   w.walker_command(target(c), static_cast<f32>(c.in_number("forward")), static_cast<f32>(c.in_number("strafe")),
                                    static_cast<f32>(c.in_number("turn") * 3.14159265358979 / 180), static_cast<f32>(c.in_number("duration")));
                 },
                 "Walks the robot like its gamepad: forward up to 0.4 m/s, sideways 0.3, turning 57 °/s (its trained ranges). Overrides its own behaviour "
                 "for the duration (0: until the next Walk; Set Autonomous gives it back).",
                 "drive move go duck robot walker twist"));
  lib.add(action("walker.look", "Look", "Robots",
                 {data("target", "Robot", PinType::actor, 0), data("yaw", "Yaw (°, + left)", PinType::number, 0.0), data("pitch", "Pitch (°, + down)", PinType::number, 0.0),
                  data("duration", "Duration (s, 0 = keep)", PinType::number, 0.0)},
                 {}, [](Context& c, GameWorld& w) {
                   constexpr f64 k = 3.14159265358979 / 180;
                   w.walker_look(target(c), static_cast<f32>(c.in_number("yaw") * k), static_cast<f32>(c.in_number("pitch") * k), static_cast<f32>(c.in_number("duration")));
                 },
                 "Turns the robot's head (its camera and ToF look there): yaw up to 80°, pitch up to 63°.", "head camera gaze duck robot"));
  lib.add(action("walker.autonomous", "Set Autonomous", "Robots", {data("target", "Robot", PinType::actor, 0), data("enable", "Autonomous", PinType::boolean, true)}, {},
                 [](Context& c, GameWorld& w) { w.walker_set_autonomous(target(c), c.in_bool("enable")); },
                 "On: the robot roams on its own (camera, ToF, exploration memory) whenever nobody drives it. Off: it stands until driven.",
                 "brain behaviour wander ai duck robot"));
  lib.add(action("walker.skill", "Robot Move", "Robots", {data("target", "Robot", PinType::actor, 0), data("move", "Move", PinType::text, "pick")}, {},
                 [](Context& c, GameWorld& w) { w.walker_skill(target(c), c.in_text("move")); },
                 "One of the robot's moves, as its daemon runs them: sit (toggles), pick (ground pick; the crouch on rollers), kick_left, kick_right, roulade.",
                 "skill sit kick roll pick duck robot"));
  lib.add(action("walker.beak", "Robot Beak", "Robots", {data("target", "Robot", PinType::actor, 0), data("open", "Open (0-1)", PinType::number, 1.0)}, {},
                 [](Context& c, GameWorld& w) { w.walker_mouth(target(c), static_cast<f32>(c.in_number("open"))); },
                 "Opens (1) or closes (0) the beak.", "mouth jaw duck robot"));
  lib.add(action("walker.pet", "Pet Robot", "Robots", {data("target", "Robot", PinType::actor, 0), data("seconds", "Seconds", PinType::number, 3.0)}, {},
                 [](Context& c, GameWorld& w) { w.walker_pet(target(c), static_cast<f32>(c.in_number("seconds"))); },
                 "A hand on the duck for a while: it melts, wiggles or falls asleep, as Pollen's brain does it.", "petting stroke touch duck robot"));
  lib.add(action("walker.hear", "Robot Hears", "Robots", {data("target", "Robot", PinType::actor, 0), data("voice", "Voice (else a noise)", PinType::boolean, true)}, {},
                 [](Context& c, GameWorld& w) { w.walker_hear(target(c), c.in_bool("voice")); },
                 "The duck hears someone talk (it perks up and answers) or a noise (it stirs from a doze).", "sound voice listen duck robot"));
  lib.add(getter("walker.holding", "Robot Holds Something", "Robots", {data("target", "Robot", PinType::actor, 0)}, {data("holding", "Holding", PinType::boolean)},
                 [](Context& c, GameWorld& w) { c.out("holding", Value::boolean(w.walker_holding(target(c)))); },
                 "The beak is told to shut and its servo reads it open: something is in it (a sock).", "sock beak carry duck robot"));
  lib.add(action("walker.quack", "Chirp", "Robots", {data("target", "Robot", PinType::actor, 0)}, {},
                 [](Context& c, GameWorld& w) { w.walker_quack(target(c)); }, "The duck chirps (its own voice, the robot's synth) and opens its beak.", "sound duck robot"));
  lib.add(getter("walker.tof", "Robot ToF", "Robots", {data("target", "Robot", PinType::actor, 0)},
                 {data("blocked", "Obstacle Seen", PinType::boolean), data("distance", "Nearest (m)", PinType::number), data("bearing", "Bearing (°, + left)", PinType::number),
                  data("left", "Free Left (0-1)", PinType::number), data("right", "Free Right (0-1)", PinType::number)},
                 [](Context& c, GameWorld& w) {
                   GameWorld::WalkerSensors s;
                   const bool ok = w.walker_sensors(target(c), s);
                   c.out("blocked", Value::boolean(ok && s.nearest >= 0));
                   c.out("distance", Value::number(ok ? s.nearest : -1.0));
                   c.out("bearing", Value::number(ok ? s.nearest_bearing * 180 / 3.14159265358979 : 0.0));
                   c.out("left", Value::number(ok ? s.free_left : 0.0));
                   c.out("right", Value::number(ok ? s.free_right : 0.0));
                 },
                 "The head time-of-flight sensor (8 x 8 zones, 45°): the nearest obstacle with the robot's own floor filter. -1 = nothing in range (4 m real).",
                 "distance sensor depth obstacle duck robot"));
  lib.add(getter("walker.sees", "Robot Camera Sees", "Robots", {data("target", "Robot", PinType::actor, 0), data("other", "Actor", PinType::actor, 0)},
                 {data("visible", "Visible", PinType::boolean), data("bearing", "Bearing (°, + left)", PinType::number), data("distance", "Distance (m)", PinType::number)},
                 [](Context& c, GameWorld& w) {
                   bool visible = false;
                   f32 bearing = 0, distance = -1;
                   w.walker_sees(target(c), c.in_actor("other"), visible, bearing, distance);
                   c.out("visible", Value::boolean(visible));
                   c.out("bearing", Value::number(bearing * 180 / 3.14159265358979));
                   c.out("distance", Value::number(distance));
                 },
                 "Whether the actor is in the head camera's picture (62° x 37°) with nothing in between.", "vision see detect camera duck robot"));
  lib.add(getter("walker.state", "Robot State", "Robots", {data("target", "Robot", PinType::actor, 0)},
                 {data("state", "Behaviour", PinType::text), data("fallen", "Fallen", PinType::boolean), data("speed", "Speed (m/s)", PinType::number)},
                 [](Context& c, GameWorld& w) {
                   GameWorld::WalkerSensors s;
                   const bool ok = w.walker_sensors(target(c), s);
                   c.out("state", Value::text(w.walker_state(target(c))));
                   c.out("fallen", Value::boolean(ok && s.fallen));
                   c.out("speed", Value::number(ok ? s.speed : 0.0));
                 },
                 "What the robot is doing (Chill, LookAround, Wander, TurnInPlace, BackOff, Zoomies, GetUp; Driven / Logic when someone drives it).",
                 "behaviour status fallen duck robot"));
  (void)none;
}

void add_game_nodes(logic::Library& lib) {
  const json::Value zero3 = vec3(0, 0, 0);
  lib.add(action("game.message", "Show Message", "Game", {data("text", "Text", PinType::text, "Hello!")}, {}, [](Context& c, GameWorld& w) { w.show_message(c.in_text("text")); },
                 "A message in the middle of the screen (every player sees it).", "say hud print notify"));
  lib.add(action("game.add_score", "Add Score", "Game", {data("points", "Points", PinType::number, 10.0)}, {}, [](Context& c, GameWorld& w) { w.add_score(static_cast<f32>(c.in_number("points"))); },
                 "Adds points (wins at the target score when the goal is Score).", "points reward"));
  lib.add(action("game.win", "Win Game", "Game", {data("message", "Message", PinType::text, "You win!")}, {}, [](Context& c, GameWorld& w) { w.end_game(true, c.in_text("message")); },
                 "Ends the game as won.", "victory finish complete"));
  lib.add(action("game.lose", "Lose Game", "Game", {data("message", "Message", PinType::text, "Game over")}, {}, [](Context& c, GameWorld& w) { w.end_game(false, c.in_text("message")); },
                 "Ends the game as lost.", "fail defeat"));
  {
    NodeDef d = action("game.sound", "Play Sound", "Audio",
                       {data("location", "Location", PinType::vector, zero3), data("volume", "Volume", PinType::number, 1.0), data("pitch", "Pitch", PinType::number, 1.0),
                        data("spatial", "3D", PinType::boolean, true)},
                       {}, [](Context& c, GameWorld& w) {
                         Vec3 at = c.in_vector("location");
                         if (length_sq(at) == 0 && c.self()) at = actor_position(w, c.self());
                         w.play_sound(c.param("sound", "builtin:confirm"), at, static_cast<f32>(c.in_number("volume")), static_cast<f32>(c.in_number("pitch")), c.in_bool("spatial"),
                                      c.self());
                       },
                       "Plays a sound once (3D at the location, or Self's position; or flat when 3D is off).", "audio sfx noise");
    d.params = {param("sound", "Sound", ParamDef::Kind::sound, "builtin:confirm")};
    d.title = [](const NodeDef&, const logic::Node& n) {
      std::string s = n.param("sound", "builtin:confirm");
      if (s.rfind("builtin:", 0) == 0) s = s.substr(8);
      return "Play " + s;
    };
    lib.add(std::move(d));
  }
  lib.add(action("game.particles", "Spawn Sparks", "Effects", {data("location", "Location", PinType::vector, zero3), data("color", "Color", PinType::vector, vec3(1, 0.8f, 0.3f))}, {},
                 [](Context& c, GameWorld& w) { w.particle_burst(c.in_vector("location"), c.in_vector("color") * 1.6f); }, "A burst of glowing particles.", "fx effect burst confetti"));
  lib.add(action("game.shake", "Camera Shake", "Effects", {data("amount", "Amount", PinType::number, 0.5)}, {}, [](Context& c, GameWorld& w) { w.shake_camera(static_cast<f32>(c.in_number("amount"))); },
                 "Shakes this machine's camera (0..1).", "rumble"));
  lib.add(action("combat.damage", "Apply Damage", "Combat",
                 {data("target", "Actor", PinType::actor, 0), data("amount", "Damage", PinType::number, 25.0), data("instigator", "Instigator", PinType::actor, 0)}, {},
                 [](Context& c, GameWorld& w) {
                   const ActorId t = target(c);
                   w.damage(t, static_cast<f32>(c.in_number("amount")), c.in_actor("instigator"), false, actor_position(w, c.self() ? c.self() : t));
                 },
                 "Hurts an actor with a Health component (armor first, death, score).", "hurt hit attack"));
  lib.add(action("combat.heal", "Heal", "Combat", {data("target", "Actor", PinType::actor, 0), data("amount", "Amount", PinType::number, 25.0)}, {},
                 [](Context& c, GameWorld& w) { w.heal(target(c), static_cast<f32>(c.in_number("amount"))); }, "Gives health back (up to the maximum).", "health restore medkit"));
  lib.add(action("combat.kill", "Kill", "Combat", {data("target", "Actor", PinType::actor, 0)}, {}, [](Context& c, GameWorld& w) { w.damage(target(c), 1e9f, 0, false, {}); },
                 "Takes all health (respawns like any death).", "die"));
  lib.add(action("combat.ammo", "Give Ammo", "Combat", {data("target", "Player", PinType::actor, 0), data("rounds", "Rounds", PinType::integer, 30)}, {},
                 [](Context& c, GameWorld& w) { w.give_ammo(target(c), static_cast<i32>(c.in_int("rounds"))); }, "Adds rounds to a soldier's reserve.", "bullets reload"));
  // Line trace (exec node like Unreal's, so the hit is taken once).
  {
    NodeDef d = action("physics.trace", "Line Trace", "Physics",
                       {data("start", "Start", PinType::vector, zero3), data("end", "End", PinType::vector, vec3(10, 0, 0)), data("ignore", "Ignore", PinType::actor, 0)},
                       {data("hit", "Hit", PinType::boolean), data("actor", "Hit Actor", PinType::actor), data("location", "Location", PinType::vector), data("normal", "Normal", PinType::vector)},
                       [](Context& c, GameWorld& w) {
                         ActorId a = 0;
                         Vec3 p, n;
                         const bool hit = w.line_trace(c.in_vector("start"), c.in_vector("end"), c.in_actor("ignore"), a, p, n);
                         c.out("hit", Value::boolean(hit));
                         c.out("actor", Value::actor(a));
                         c.out("location", Value::vector(p));
                         c.out("normal", Value::vector(n));
                       },
                       "First thing between Start and End (a Box3D ray).", "raycast ray line of sight");
    lib.add(std::move(d));
  }
  // For each actor with a tag.
  {
    NodeDef d = make("flow.for_each_tag", "For Each Actor with Tag", "Flow", NodeKind::flow, "Runs Loop Body for every living actor with the tag, then Completed.", "loop all every");
    d.inputs = {exec_in(), data("tag", "Tag", PinType::text, "target")};
    d.outputs = {exec_out("body", "Loop Body"), data("actor", "Actor", PinType::actor), data("index", "Index", PinType::integer), exec_out("completed", "Completed")};
    d.run = [](Context& c) {
      GameWorld* w = world(c);
      if (!w) return c.exec("completed");
      logic::NodeState& s = c.state();
      if (c.entry() != Context::kResume) {
        std::vector<ActorId> v;
        w->actors_with_tag(c.in_text("tag"), v);
        // Remember the list in the node state's text (ids joined), so the loop sees a stable set.
        s.value = Value::text("");
        for (ActorId id : v) s.value.s += std::to_string(id) + ",";
        s.index = 0;
      } else {
        ++s.index;
      }
      // Find entry `index` in the list.
      i64 k = 0;
      std::size_t pos = 0;
      while (k < s.index && pos < s.value.s.size()) { pos = s.value.s.find(',', pos) + 1; ++k; }
      if (pos >= s.value.s.size()) return c.exec("completed");
      const ActorId id = static_cast<ActorId>(std::strtoul(s.value.s.c_str() + pos, nullptr, 10));
      c.out("actor", Value::actor(id));
      c.out("index", Value::integer(s.index));
      c.loop_back(Context::kResume);
      return c.exec("body");
    };
    lib.add(std::move(d));
  }
}

const bool kRegistered = [] {
  logic::Library& lib = logic::library();
  add_events(lib);
  add_actor_nodes(lib);
  add_game_nodes(lib);
  add_walker_nodes(lib);
  return true;
}();

}  // namespace

/// Referenced from GameWorld so the registration above is always linked.
bool logic_nodes_registered() { return kRegistered; }

}  // namespace kern::game
