// Kern Studio details panel: the property schema of every component, values
// of the selection (with "multiple values"), edits through the level's JSON
// form (so every edit is validated and clamped like a loaded file), adding and
// removing components, and the world settings.
#include <algorithm>
#include <cmath>
#include <cstring>

#include "internal.h"
#include "kern/game/geometry.h"
#include "kern/studio/studio.h"

namespace kern::studio {

namespace {

enum class F : u8 { number, integer, vec3, boolean, choice, color, text, actor, texture, flags3, uinteger, sound, sound_auto };

struct FieldDef {
  const char* key;     // JSON key below the section's root
  const char* label;
  F type;
  f64 min = -1e9, max = 1e9, step = 0;
  const char* unit = "";
  const char* const* choices = nullptr;  // value/label pairs, nullptr-terminated
  const char* group = nullptr;           // sub heading
};

constexpr const char* kShapes[] = {"box", "Box", "sphere", "Sphere", "cylinder", "Cylinder", "cone", "Cone", "capsule", "Capsule", "plane", "Plane", "torus", "Torus", "ramp", "Ramp", "mesh", "Mesh", nullptr};
constexpr const char* kMotion[] = {"static", "Static", "kinematic", "Kinematic", "dynamic", "Dynamic (simulate physics)", nullptr};
constexpr const char* kColliders[] = {"auto", "Fit to mesh", "box", "Box", "sphere", "Sphere", "capsule", "Capsule", "hull", "Convex hull", nullptr};
constexpr const char* kJoints[] = {"hinge", "Hinge (revolute)", "slider", "Slider (prismatic)", "ball", "Ball (spherical)", "fixed", "Fixed (weld)", "distance", "Distance / rope",
                                   "wheel", "Wheel (suspension)", "motor", "Motor", "parallel", "Parallel", "noCollision", "No collision (filter)", nullptr};
constexpr const char* kActionProfiles[] = {"training", "As trained (scale 1, no filter)", "robot", "As the robot's daemon (scale 0.9, low-pass 0.5 / 0.7)", nullptr};
constexpr const char* kGaits[] = {"velstand", "velstand (walks and stands, v8)", "alpha", "alpha_walking + alpha_stand (older pair)", nullptr};
constexpr const char* kModes[] = {"hovercar", "Hover car", "rolling", "Rolling ball", "character", "Character", "flyer", "Flyer", "vehicle", "Vehicle", "soldier", "Soldier (first person shooter)", "walker", "Walker (robot on its trained network)", nullptr};
constexpr const char* kWeaponKinds[] = {"rifle", "Rifle", "pistol", "Pistol", "shotgun", "Shotgun", "sniper", "Sniper rifle", nullptr};
constexpr const char* kCameras[] = {"chase", "Chase", "firstPerson", "First person", "topDown", "Top down", nullptr};
constexpr const char* kTriggers[] = {"goal", "Goal (win)", "hazard", "Hazard (lose)", "checkpoint", "Checkpoint (respawn here)", "message", "Message",
                                       "launch", "Jump pad (launch)", "teleport", "Teleport", "damage", "Damage zone", "event", "Logic event only", nullptr};
constexpr const char* kMovers[] = {"sine", "Sine", "pingPong", "Ping-pong", "loop", "Loop", nullptr};
constexpr const char* kGoals[] = {"none", "None (sandbox)", "collectAll", "Collect all pickups", "reachGoal", "Reach a goal trigger", "survive", "Survive the time limit",
                                  "score", "Reach a target score", nullptr};

constexpr FieldDef kMesh[] = {
    {"shape", "Shape", F::choice, 0, 0, 0, "", kShapes},
    {"size", "Size", F::vec3, 0.001, 1e5, 0.1, "m"},
    {"radius", "Radius", F::number, 0.001, 1e5, 0.05, "m"},
    {"height", "Height", F::number, 0.001, 1e5, 0.05, "m"},
    {"segments", "Segments", F::integer, 3, 128, 1},
    {"collision", "Generate collision", F::boolean, 0, 0, 0, "", nullptr, "Physics"},
    {"friction", "Friction", F::number, 0, 10, 0.05},
    {"restitution", "Restitution (bounce)", F::number, 0, 1, 0.05},
    {"rollingResistance", "Rolling resistance", F::number, 0, 10, 0.01},
    {"material.color", "Base color", F::color, 0, 0, 0, "", nullptr, "Material"},
    {"material.roughness", "Roughness", F::number, 0, 1, 0.01},
    {"material.metalness", "Metallic", F::number, 0, 1, 0.01},
    {"material.emissive", "Emissive color", F::color},
    {"material.emissiveStrength", "Emissive strength", F::number, 0, 1000, 0.1},
    {"material.opacity", "Opacity", F::number, 0, 1, 0.01},
    {"material.unlit", "Unlit", F::boolean},
    {"material.castShadow", "Cast shadow", F::boolean},
    {"material.doubleSided", "Two sided", F::boolean},
    {"material.texture", "Texture", F::texture},
    {"material.tiling", "Texture tiling", F::number, 0.001, 1000, 0.05, "/m"},
};
constexpr FieldDef kLight[] = {
    {"color", "Light color", F::color},
    {"intensity", "Intensity", F::number, 0, 1e5, 0.5},
    {"range", "Attenuation radius", F::number, 0.1, 1e4, 0.5, "m"},
};
constexpr FieldDef kBody[] = {
    {"motion", "Mobility", F::choice, 0, 0, 0, "", kMotion},
    {"mass", "Mass (0 = from density)", F::number, 0, 1e9, 1, "kg"},
    {"linearDamping", "Linear damping", F::number, 0, 1e4, 0.01},
    {"angularDamping", "Angular damping", F::number, 0, 1e4, 0.01},
    {"gravityScale", "Gravity scale", F::number, -100, 100, 0.05},
    {"bullet", "Continuous (bullet)", F::boolean},
    {"startAwake", "Start awake", F::boolean},
    {"allowSleep", "Allow sleep", F::boolean},
    {"lockLinear", "Lock position", F::flags3, 0, 0, 0, "", nullptr, "Constraints"},
    {"lockAngular", "Lock rotation", F::flags3},
    {"linearVelocity", "Initial velocity", F::vec3, -1e5, 1e5, 0.1, "m/s", nullptr, "Initial state"},
    {"angularVelocity", "Initial spin", F::vec3, -1e5, 1e5, 1, "°/s"},
    {"impactSound", "Impact sound", F::sound, 0, 0, 0, "", nullptr, "Sound"},
    {"impactThreshold", "Impact threshold", F::number, 0, 1e4, 0.1, "m/s"},
};
constexpr FieldDef kCollider[] = {
    {"shape", "Shape", F::choice, 0, 0, 0, "", kColliders},
    {"center", "Offset", F::vec3, -1e5, 1e5, 0.05, "m"},
    {"rotation", "Rotation", F::vec3, -360, 360, 1, "°"},
    {"size", "Box size", F::vec3, 0.001, 1e5, 0.05, "m"},
    {"radius", "Radius", F::number, 0.001, 1e5, 0.05, "m"},
    {"length", "Capsule length", F::number, 0, 1e5, 0.05, "m"},
    {"density", "Density", F::number, 0, 1e6, 10, "kg/m³", nullptr, "Physical material"},
    {"friction", "Friction", F::number, 0, 10, 0.05},
    {"restitution", "Restitution (bounce)", F::number, 0, 1, 0.05},
    {"rollingResistance", "Rolling resistance", F::number, 0, 10, 0.01},
    {"sensor", "Sensor (overlap only)", F::boolean, 0, 0, 0, "", nullptr, "Collision"},
    {"category", "Channel bits", F::uinteger, 0, 4294967295.0, 1},
    {"mask", "Collides with bits", F::uinteger, 0, 4294967295.0, 1},
    {"group", "Group index", F::integer, -32768, 32767, 1},
    {"impactSound", "Impact sound", F::sound_auto, 0, 0, 0, "", nullptr, "Sound"},
};
constexpr FieldDef kJoint[] = {
    {"kind", "Type", F::choice, 0, 0, 0, "", kJoints},
    {"target", "Connected actor", F::actor},
    {"anchor", "Anchor (local)", F::vec3, -1e5, 1e5, 0.05, "m"},
    {"axis", "Axis (local)", F::vec3, -1, 1, 0.05},
    {"collideConnected", "Collide connected", F::boolean},
    {"enableLimit", "Limit", F::boolean, 0, 0, 0, "", nullptr, "Limits"},
    {"lower", "Lower", F::number, -1e5, 1e5, 1},
    {"upper", "Upper", F::number, -1e5, 1e5, 1},
    {"enableMotor", "Motor", F::boolean, 0, 0, 0, "", nullptr, "Motor"},
    {"motorSpeed", "Speed", F::number, -1e5, 1e5, 1},
    {"maxMotorForce", "Max force / torque", F::number, 0, 1e9, 10},
    {"enableSpring", "Spring", F::boolean, 0, 0, 0, "", nullptr, "Spring"},
    {"springHertz", "Frequency", F::number, 0, 1000, 0.1, "Hz"},
    {"springDamping", "Damping ratio", F::number, 0, 100, 0.05},
    {"length", "Rest length (0 = at start)", F::number, 0, 1e5, 0.05, "m"},
    {"breakForce", "Break force (0 = never)", F::number, 0, 1e12, 100, "N", nullptr, "Breaking"},
    {"breakTorque", "Break torque (0 = never)", F::number, 0, 1e12, 100, "N·m"},
    {"breakSound", "Break sound", F::sound},
};
constexpr FieldDef kController[] = {
    {"mode", "Movement", F::choice, 0, 0, 0, "", kModes},
    {"maxSpeed", "Max speed", F::number, 0, 1000, 0.5, "m/s"},
    {"acceleration", "Acceleration", F::number, 0, 1000, 0.5, "m/s²"},
    {"turnRate", "Turn rate", F::number, 0, 3600, 1, "°/s"},
    {"jumpSpeed", "Jump speed (0 = none)", F::number, 0, 200, 0.1, "m/s"},
    {"boost", "Boost", F::boolean},
    {"boostSpeed", "Boost speed", F::number, 0, 1000, 0.5, "m/s"},
    {"hoverHeight", "Hover height", F::number, 0.05, 50, 0.05, "m"},
    {"camera", "Camera", F::choice, 0, 0, 0, "", kCameras, "Camera"},
    {"cameraDistance", "Distance", F::number, 0, 500, 0.5, "m"},
    {"cameraHeight", "Height", F::number, -100, 500, 0.25, "m"},
    {"cameraFov", "Field of view", F::number, 20, 120, 1, "°"},
    {"crouchSpeed", "Crouch speed", F::number, 0, 100, 0.1, "m/s", nullptr, "Soldier"},
    {"eyeHeight", "Eye height", F::number, 0.2, 10, 0.01, "m"},
    {"lookSensitivity", "Look sensitivity", F::number, 0.05, 10, 0.05},
    {"headBob", "Head bob", F::boolean},
    {"autonomous", "Autonomous (camera, ToF)", F::boolean, 0, 0, 0, "", nullptr, "Walker"},
    {"gait", "Walking network", F::choice, 0, 0, 0, "", kGaits},
    {"actions", "Actions to servo goals", F::choice, 0, 0, 0, "", kActionProfiles},
    {"kernExtras", "Kern extras (ball play, sock fetching, beak in the pick)", F::boolean},
    {"engineSound", "Engine loop", F::sound_auto, 0, 0, 0, "", nullptr, "Sound"},
    {"jumpSound", "Jump", F::sound},
    {"boostSound", "Boost", F::sound},
};
constexpr FieldDef kWeapon[] = {
    {"kind", "Type", F::choice, 0, 0, 0, "", kWeaponKinds},
    {"damage", "Damage", F::number, 0, 10000, 1},
    {"headshot", "Headshot multiplier", F::number, 1, 20, 0.1},
    {"fireRate", "Fire rate", F::number, 1, 6000, 10, "rpm"},
    {"automatic", "Automatic", F::boolean},
    {"pellets", "Pellets per shot", F::integer, 1, 32, 1},
    {"range", "Range", F::number, 1, 10000, 5, "m"},
    {"impulse", "Impulse on bodies", F::number, 0, 1e5, 0.5, "N·s"},
    {"magazine", "Magazine", F::integer, 1, 1000, 1, "", nullptr, "Ammo"},
    {"reserve", "Reserve", F::integer, 0, 100000, 10},
    {"reloadTime", "Reload time", F::number, 0, 60, 0.1, "s"},
    {"spread", "Hip spread", F::number, 0, 30, 0.05, "°", nullptr, "Handling"},
    {"aimSpread", "Aimed spread", F::number, 0, 30, 0.01, "°"},
    {"recoil", "Recoil", F::number, 0, 20, 0.05},
    {"aimFov", "Aim field of view", F::number, 5, 120, 1, "°"},
    {"fireSound", "Fire", F::sound, 0, 0, 0, "", nullptr, "Sound"},
    {"reloadSound", "Reload", F::sound},
};
constexpr FieldDef kHealth[] = {
    {"maxHealth", "Health", F::number, 1, 1e7, 5},
    {"armor", "Armor", F::number, 0, 1e7, 5},
    {"regenDelay", "Regenerate after (0 = never)", F::number, 0, 1e5, 0.5, "s"},
    {"regenRate", "Regeneration", F::number, 0, 1e5, 1, "/s"},
    {"respawn", "Respawn", F::boolean},
    {"respawnDelay", "Respawn delay", F::number, 0, 1e5, 0.5, "s"},
    {"points", "Points for a kill", F::number, -1e6, 1e6, 1},
    {"hitSound", "Hit", F::sound, 0, 0, 0, "", nullptr, "Sound"},
    {"deathSound", "Down", F::sound},
    {"explodeRadius", "Explosion radius (0 = none)", F::number, 0, 1000, 0.5, "m", nullptr, "Explosion"},
    {"explodeDamage", "Explosion damage", F::number, 0, 1e6, 5},
};
constexpr FieldDef kPickup[] = {
    {"points", "Points", F::number, -1e6, 1e6, 1},
    {"burstColor", "Burst color", F::color},
    {"respawn", "Respawn after (0 = never)", F::number, 0, 1e5, 0.5, "s"},
    {"sound", "Sound", F::sound},
    {"heal", "Heals", F::number, 0, 1e6, 5},
    {"ammo", "Ammo", F::integer, 0, 100000, 10},
};
constexpr FieldDef kTrigger[] = {
    {"action", "Action", F::choice, 0, 0, 0, "", kTriggers},
    {"message", "Message", F::text},
    {"once", "Only once", F::boolean},
    {"strength", "Strength (launch m/s, damage)", F::number, 0, 1e5, 0.5},
    {"target", "Teleport target", F::actor},
    {"sound", "Sound", F::sound_auto},
};
constexpr const char* kBuses[] = {"sfx", "Effects", "ambience", "Ambience", "music", "Music", "ui", "Interface", "voice", "Voice", nullptr};
constexpr const char* kRolloffs[] = {"inverse", "Inverse (natural)", "linear", "Linear", "exponential", "Exponential", nullptr};
constexpr FieldDef kAudio[] = {
    {"sound", "Sound", F::sound},
    {"volume", "Volume", F::number, 0, 4, 0.05},
    {"pitch", "Pitch", F::number, 0.05, 8, 0.05},
    {"loop", "Loop", F::boolean},
    {"autoplay", "Play on start", F::boolean},
    {"delay", "Start delay", F::number, 0, 1e5, 0.1, "s"},
    {"bus", "Bus", F::choice, 0, 0, 0, "", kBuses},
    {"spatial", "Spatial (3D)", F::boolean, 0, 0, 0, "", nullptr, "Attenuation"},
    {"minDistance", "Inner radius", F::number, 0.01, 1e5, 0.25, "m"},
    {"maxDistance", "Falloff distance", F::number, 0.02, 1e5, 1, "m"},
    {"rolloff", "Roll-off", F::choice, 0, 0, 0, "", kRolloffs},
    {"doppler", "Doppler", F::number, 0, 4, 0.05},
    {"pitchBySpeed", "Pitch by speed", F::number, -10, 10, 0.05},
};
constexpr FieldDef kRotator[] = {
    {"rate", "Rotation rate", F::vec3, -1e5, 1e5, 1, "°/s"},
};
constexpr FieldDef kMover[] = {
    {"offset", "Offset", F::vec3, -1e5, 1e5, 0.1, "m"},
    {"period", "Period", F::number, 0.05, 1e5, 0.1, "s"},
    {"phase", "Phase", F::number, 0, 1, 0.01},
    {"mode", "Motion", F::choice, 0, 0, 0, "", kMovers},
};
constexpr FieldDef kCity[] = {
    {"seed", "Seed", F::uinteger, 0, 4294967295.0, 1},
    {"blocks", "Blocks per side", F::integer, 1, 16, 1},
    {"pitch", "Block pitch", F::number, 30, 400, 1, "m"},
    {"street", "Street width", F::number, 6, 60, 0.5, "m"},
    {"maxHeight", "Tallest building", F::number, 6, 400, 1, "m"},
    {"trees", "Trees", F::boolean},
    {"traffic", "Traffic cars", F::integer, 0, 200, 1},
    {"nightWindows", "Lit windows at night", F::boolean},
};
constexpr FieldDef kWorld[] = {
    {"title", "Game title", F::text, 0, 0, 0, "", nullptr, "Game"},
    {"player", "Player (controlled in Play)", F::actor},
    {"observer", "Observer camera (Play: fly, watch, drag bodies)", F::boolean},
    {"goal", "Win condition", F::choice, 0, 0, 0, "", kGoals},
    {"timeLimit", "Time limit (0 = none)", F::number, 0, 1e6, 1, "s"},
    {"scoreTarget", "Target score", F::number, 0, 1e9, 1},
    {"killZ", "Kill Z (respawn below)", F::number, -1e5, 1e5, 1, "m"},
    {"timeOfDay", "Time of day", F::number, 0, 0.9999, 0.005, "", nullptr, "Sky and light"},
    {"dayLength", "Day length (0 = frozen)", F::number, 0, 1e6, 10, "s"},
    {"sunAzimuth", "Sun azimuth", F::number, -360, 720, 1, "°"},
    {"fogDensity", "Fog density", F::number, 0, 0.1, 0.0001},
    {"exposure", "Exposure", F::number, 0.05, 20, 0.05},
    {"aoRadius", "Ambient occlusion radius (0 = off)", F::number, 0, 50, 0.05, "m"},
    {"clouds", "Clouds", F::boolean},
    {"ground", "Endless ground", F::boolean},
    {"groundColor", "Ground color", F::color},
    {"gravity", "Gravity", F::vec3, -1e3, 1e3, 0.1, "m/s²", nullptr, "Physics (Box3D)"},
    {"substeps", "Sub-steps", F::integer, 1, 16, 1},
    {"groundFriction", "Ground friction", F::number, 0, 10, 0.05},
    {"groundRestitution", "Ground bounciness", F::number, 0, 1, 0.05},
    {"contactHertz", "Contact stiffness", F::number, 1, 1000, 1, "Hz"},
    {"contactDamping", "Contact damping ratio", F::number, 0, 1000, 0.5},
    {"contactSpeed", "Contact push-out speed", F::number, 0.01, 100, 0.1, "m/s"},
    {"stepHz", "Step rate", F::number, 15, 480, 1, "Hz"},
    {"masterVolume", "Master volume", F::number, 0, 4, 0.05, "", nullptr, "Audio"},
    {"music", "Music", F::sound},
    {"musicVolume", "Music volume", F::number, 0, 4, 0.05},
    {"ambience", "Ambience", F::sound},
    {"ambienceVolume", "Ambience volume", F::number, 0, 4, 0.05},
    {"reverb", "Reverb", F::number, 0, 1, 0.01},
    {"doppler", "Doppler", F::number, 0, 4, 0.05},
};

struct SectionDef {
  const char* type;         // component key ("mesh", "colliders", ...)
  const char* label;
  const FieldDef* fields;
  std::size_t count;
  bool list;                // colliders / joints
};

constexpr SectionDef kSections[] = {
    {"mesh", "Static Mesh", kMesh, std::size(kMesh), false},
    {"light", "Point Light", kLight, std::size(kLight), false},
    {"body", "Rigid Body (Box3D)", kBody, std::size(kBody), false},
    {"colliders", "Collider", kCollider, std::size(kCollider), true},
    {"joints", "Constraint", kJoint, std::size(kJoint), true},
    {"controller", "Player Controller", kController, std::size(kController), false},
    {"weapon", "Weapon", kWeapon, std::size(kWeapon), false},
    {"health", "Health", kHealth, std::size(kHealth), false},
    {"pickup", "Pickup", kPickup, std::size(kPickup), false},
    {"trigger", "Trigger", kTrigger, std::size(kTrigger), false},
    {"rotator", "Rotator", kRotator, std::size(kRotator), false},
    {"mover", "Mover", kMover, std::size(kMover), false},
    {"city", "Procedural City", kCity, std::size(kCity), false},
    {"audio", "Audio Source", kAudio, std::size(kAudio), false},
};

/// Value at a dotted path ("material.color", "0.radius").
const json::Value* at_path(const json::Value& root, std::string_view path) {
  const json::Value* v = &root;
  while (!path.empty() && v) {
    const std::size_t dot = path.find('.');
    const std::string_view token = path.substr(0, dot);
    if (v->is_array()) {
      const std::size_t i = static_cast<std::size_t>(std::atoi(std::string(token).c_str()));
      v = i < v->size() ? &(*v)[i] : nullptr;
    } else {
      v = v->find(token);
    }
    path = dot == std::string_view::npos ? std::string_view() : path.substr(dot + 1);
  }
  return v;
}

json::Value* at_path_mut(json::Value& root, std::string_view path) {
  json::Value* v = &root;
  while (!path.empty() && v) {
    const std::size_t dot = path.find('.');
    const std::string_view token = path.substr(0, dot);
    if (v->is_array()) {
      const std::size_t i = static_cast<std::size_t>(std::atoi(std::string(token).c_str()));
      v = i < v->size() ? &v->at(i) : nullptr;
    } else {
      v = v->find(token);
    }
    path = dot == std::string_view::npos ? std::string_view() : path.substr(dot + 1);
  }
  return v;
}

const char* type_name(F t) {
  switch (t) {
    case F::number: return "number";
    case F::integer: return "integer";
    case F::uinteger: return "integer";
    case F::vec3: return "vec3";
    case F::boolean: return "bool";
    case F::choice: return "enum";
    case F::color: return "color";
    case F::text: return "text";
    case F::actor: return "actor";
    case F::texture: return "texture";
    case F::flags3: return "flags3";
    case F::sound: return "sound";
    case F::sound_auto: return "sound";
  }
  return "text";
}

json::Value field_json(const FieldDef& f, const std::string& path, const std::vector<const json::Value*>& values) {
  json::Value o = json::Value::object();
  o.set("path", path);
  o.set("label", f.label);
  o.set("type", type_name(f.type));
  if (f.type == F::number || f.type == F::integer || f.type == F::uinteger || f.type == F::vec3) {
    o.set("min", f.min);
    o.set("max", f.max);
    if (f.step > 0) o.set("step", f.step);
  }
  if (*f.unit) o.set("unit", f.unit);
  if (f.choices) {
    json::Value c = json::Value::array();
    for (const char* const* p = f.choices; *p; p += 2) {
      json::Value pair = json::Value::array();
      pair.push(p[0]);
      pair.push(p[1]);
      c.push(std::move(pair));
    }
    o.set("choices", std::move(c));
  }
  if (f.group) o.set("group", f.group);
  if (f.type == F::sound_auto) o.set("auto", true);
  // Value of the first actor; per vector component "mixed" when they differ.
  const json::Value* first = values.empty() ? nullptr : values[0];
  o.set("value", first ? *first : json::Value());
  bool mixed = false;
  json::Value mixed_parts = json::Value::array();
  if (first && first->is_array()) for (std::size_t i = 0; i < first->size(); ++i) mixed_parts.push(false);
  for (std::size_t k = 1; k < values.size(); ++k) {
    if (!values[k] || !first) { mixed = true; continue; }
    if (*values[k] == *first) continue;
    mixed = true;
    if (first->is_array() && values[k]->is_array() && values[k]->size() == first->size())
      for (std::size_t i = 0; i < first->size(); ++i)
        if (!((*values[k])[i] == (*first)[i])) mixed_parts.at(i) = true;
  }
  if (mixed) {
    o.set("mixed", true);
    if (first && first->is_array()) o.set("mixedParts", std::move(mixed_parts));
  }
  return o;
}

}  // namespace

json::Value Studio::details_json() const {
  json::Value d = json::Value::object();
  json::Value sections = json::Value::array();
  std::vector<const game::Actor*> actors;
  for (ActorId id : selection_) if (const game::Actor* a = level_.find(id)) actors.push_back(a);
  if (actors.empty()) {
    d.set("empty", "Select an actor in the viewport or the outliner to edit its details.");
    return d;
  }
  // Primary actor first so its values show for single-value fields.
  std::rotate(actors.begin(), actors.end() - 1, actors.end());
  std::vector<json::Value> docs;
  for (const game::Actor* a : actors) docs.push_back(game::actor_to_json(*a));
  const game::Actor& p = *actors[0];
  d.set("title", actors.size() == 1 ? p.name : std::to_string(actors.size()) + " actors");
  d.set("count", static_cast<u32>(actors.size()));
  d.set("id", p.id);
  auto collect = [&](const std::string& path) {
    std::vector<const json::Value*> values;
    for (const json::Value& doc : docs) values.push_back(at_path(doc, path));
    return values;
  };
  auto section = [&](const char* id, const std::string& label, bool removable, const char* remove_type, i32 index) {
    json::Value s = json::Value::object();
    s.set("id", id);
    s.set("label", label);
    s.set("closed", closed_sections_.count(id) > 0);
    if (removable) {
      json::Value r = json::Value::object();
      r.set("cmd", "removeComponent");
      r.set("type", remove_type);
      r.set("index", index);
      s.set("remove", std::move(r));
    }
    s.set("fields", json::Value::array());
    return s;
  };
  auto simple = [&](const char* path, const char* label, F type, const char* group = nullptr) {
    FieldDef f{path, label, type};
    f.group = group;
    return field_json(f, path, collect(path));
  };
  {
    json::Value s = section("actor", "Actor", false, "", 0);
    json::Value& fields = *s.find("fields");
    if (actors.size() == 1) fields.push(simple("name", "Name", F::text));
    fields.push(simple("folder", "Outliner folder", F::text));
    {
      // Tags as a comma separated list.
      json::Value f = json::Value::object();
      f.set("path", "tags");
      f.set("label", "Tags");
      f.set("type", "tags");
      std::string tags;
      for (const std::string& t : p.tags) tags += (tags.empty() ? "" : ", ") + t;
      f.set("value", tags);
      fields.push(std::move(f));
    }
    fields.push(simple("hidden", "Hidden (editor and game)", F::boolean));
    fields.push(simple("locked", "Locked (not selectable)", F::boolean));
    sections.push(std::move(s));
  }
  {
    json::Value s = section("transform", "Transform", false, "", 0);
    json::Value& fields = *s.find("fields");
    FieldDef loc{"transform.position", "Location", F::vec3, -1e7, 1e7, 0.1, "m"};
    FieldDef rot{"transform.rotation", "Rotation", F::vec3, -1e5, 1e5, 1, "°"};
    FieldDef scl{"transform.scale", "Scale", F::vec3, -1e4, 1e4, 0.05};
    json::Value l = field_json(loc, loc.key, collect(loc.key));
    l.set("reset", internal::vec_json({}));
    json::Value r = field_json(rot, rot.key, collect(rot.key));
    r.set("reset", internal::vec_json({}));
    json::Value c = field_json(scl, scl.key, collect(scl.key));
    c.set("reset", internal::vec_json({1, 1, 1}));
    c.set("uniformLock", true);
    fields.push(std::move(l));
    fields.push(std::move(r));
    fields.push(std::move(c));
    if (p.parent) {
      if (const game::Actor* parent = level_.find(p.parent)) {
        json::Value f = json::Value::object();
        f.set("type", "info");
        f.set("label", "Attached to");
        f.set("value", parent->name);
        fields.push(std::move(f));
      }
    }
    sections.push(std::move(s));
  }
  // Components present on every selected actor.
  for (const SectionDef& sd : kSections) {
    if (sd.list) {
      // Lists: one section per entry of the primary actor (index must exist on all).
      const json::Value* list = at_path(docs[0], std::string("components.") + sd.type);
      const std::size_t n = list ? list->size() : 0;
      for (std::size_t i = 0; i < n; ++i) {
        bool all = true;
        for (const json::Value& doc : docs) {
          const json::Value* l = at_path(doc, std::string("components.") + sd.type);
          all = all && l && l->size() > i;
        }
        if (!all) continue;
        const std::string id = std::string(sd.type) + "." + std::to_string(i);
        std::string label = sd.label;
        if (n > 1) label += " " + std::to_string(i + 1);
        json::Value s = section(id.c_str(), label, true, sd.type, static_cast<i32>(i));
        json::Value& fields = *s.find("fields");
        for (std::size_t k = 0; k < sd.count; ++k) {
          const std::string path = std::string("components.") + sd.type + "." + std::to_string(i) + "." + sd.fields[k].key;
          fields.push(field_json(sd.fields[k], path, collect(path)));
        }
        if (std::strcmp(sd.type, "joints") == 0) {
          // Units of the limits follow the joint type.
          const std::string kind = (*list)[i]["kind"].as_string();
          json::Value note = json::Value::object();
          note.set("type", "info");
          note.set("label", "Units");
          note.set("value", kind == "slider" || kind == "wheel" || kind == "distance" ? "limits in m, speed in m/s (wheel: °/s)" : "limits in °, speed in °/s");
          fields.push(std::move(note));
        }
        sections.push(std::move(s));
      }
      continue;
    }
    bool all = true;
    for (const json::Value& doc : docs) all = all && at_path(doc, std::string("components.") + sd.type);
    if (!all) continue;
    json::Value s = section(sd.type, sd.label, true, sd.type, 0);
    json::Value& fields = *s.find("fields");
    for (std::size_t k = 0; k < sd.count; ++k) {
      const std::string path = std::string("components.") + sd.type + "." + sd.fields[k].key;
      fields.push(field_json(sd.fields[k], path, collect(path)));
    }
    if (std::strcmp(sd.type, "mesh") == 0 && actors.size() == 1 && p.mesh) {
      // Only the dimensions the shape uses.
      const game::Shape shape = p.mesh->shape;
      const bool uses_size = shape == game::Shape::box || shape == game::Shape::plane || shape == game::Shape::ramp || shape == game::Shape::mesh;
      json::Value kept = json::Value::array();
      for (const json::Value& f : fields.items()) {
        const std::string& path = f["path"].as_string();
        if (path == "components.mesh.size" && !uses_size) continue;
        if ((path == "components.mesh.radius" || path == "components.mesh.segments") && uses_size) continue;
        if (path == "components.mesh.height" && (uses_size || shape == game::Shape::sphere)) continue;
        kept.push(f);
      }
      fields = std::move(kept);
    }
    sections.push(std::move(s));
  }
  d.set("sections", std::move(sections));
  // Components that can be added.
  json::Value add = json::Value::array();
  auto offer = [&](const char* type, const char* label, bool ok) {
    if (!ok) return;
    json::Value e = json::Value::array();
    e.push(type);
    e.push(label);
    add.push(std::move(e));
  };
  bool any_mesh = false, any_light = false, any_body = false, any_ctl = false, any_pickup = false, any_trigger = false, any_rot = false, any_mover = false,
       any_city = false, any_parent = false, any_audio = false, any_weapon = false, any_health = false;
  for (const game::Actor* a : actors) {
    any_mesh |= a->mesh.has_value();
    any_light |= a->light.has_value();
    any_body |= a->body.has_value();
    any_ctl |= a->controller.has_value();
    any_pickup |= a->pickup.has_value();
    any_trigger |= a->trigger.has_value();
    any_rot |= a->rotator.has_value();
    any_mover |= a->mover.has_value();
    any_city |= a->city.has_value();
    any_audio |= a->audio.has_value();
    any_weapon |= a->weapon.has_value();
    any_health |= a->health.has_value();
    any_parent |= a->parent != 0;
  }
  offer("mesh", "Static Mesh", !any_mesh);
  offer("light", "Point Light", !any_light);
  offer("body", "Rigid Body", !any_body);
  offer("collider", "Collider", true);
  offer("joint", "Constraint (joint)", true);
  offer("controller", "Player Controller", !any_ctl);
  offer("weapon", "Weapon", !any_weapon);
  offer("health", "Health", !any_health);
  offer("pickup", "Pickup", !any_pickup);
  offer("trigger", "Trigger", !any_trigger);
  offer("rotator", "Rotator", !any_rot);
  offer("mover", "Mover", !any_mover);
  offer("audio", "Audio Source", !any_audio);
  offer("city", "Procedural City", !any_city && !any_parent && actors.size() == 1);
  d.set("add", std::move(add));
  return d;
}

json::Value Studio::world_json() const {
  json::Value d = json::Value::object();
  json::Value sections = json::Value::array();
  const json::Value w = game::world_to_json(level_.world);
  json::Value s = json::Value::object();
  s.set("id", "world");
  s.set("label", "World Settings");
  json::Value fields = json::Value::array();
  {
    json::Value f = json::Value::object();
    f.set("path", "level.name");
    f.set("label", "Level name");
    f.set("type", "text");
    f.set("value", level_.name);
    fields.push(std::move(f));
  }
  for (const FieldDef& f : kWorld) {
    const json::Value* v = w.find(f.key);
    fields.push(field_json(f, std::string("world.") + f.key, {v}));
  }
  s.set("fields", std::move(fields));
  sections.push(std::move(s));
  d.set("sections", std::move(sections));
  return d;
}

bool Studio::set_property(const std::string& path, const json::Value& value, const std::string& transaction) {
  if (path.rfind("world.", 0) == 0 || path == "level.name") return set_world_property(path, value, transaction);
  if (selection_.empty()) return false;
  std::string label = "Edit " + path.substr(path.find_last_of('.') == std::string::npos ? 0 : path.find_last_of('.') + 1);
  std::string error;
  coalesce_ = "p:" + path;
  for (ActorId id : selection_) coalesce_ += "," + std::to_string(id);
  transaction_ = transaction;
  const bool ok = edit(label.c_str(), [&](game::Level& l) {
    u32 changed_actors = 0;
    for (ActorId id : selection_) {
      game::Actor* a = l.find(id);
      if (!a) continue;
      json::Value doc = game::actor_to_json(*a);
      if (path == "tags") {
        json::Value tags = json::Value::array();
        std::string s = value.as_string(), cur;
        for (char ch : s + ",") {
          if (ch == ',') {
            while (!cur.empty() && cur.front() == ' ') cur.erase(cur.begin());
            while (!cur.empty() && cur.back() == ' ') cur.pop_back();
            if (!cur.empty()) tags.push(cur);
            cur.clear();
          } else cur += ch;
        }
        doc.set("tags", std::move(tags));
      } else {
        json::Value* target = at_path_mut(doc, path);
        // Optional root keys (folder, hidden, locked) are left out of the JSON when unset.
        if (!target && path.find('.') == std::string::npos) target = &doc.set(path, json::Value());
        if (!target) continue;  // this actor lacks the component
        json::Value v = value;
        // Numbers typed into text boxes arrive as strings.
        if (target->is_number() && v.is_string()) {
          char* end = nullptr;
          const f64 n = std::strtod(v.as_string().c_str(), &end);
          if (end == v.as_string().c_str() || !std::isfinite(n)) { error = "\"" + v.as_string() + "\" is not a number"; return false; }
          v = n;
        }
        if (target->is_number() && v.is_bool()) v = v.as_bool() ? 1.0 : 0.0;
        // Uniform scale lock: one component typed, all follow (handled by the page sending a full vector).
        *target = std::move(v);
      }
      game::Actor updated;
      if (!game::actor_from_json(doc, updated, &error)) return false;
      updated.id = a->id;
      updated.parent = a->parent;
      if (path == "name" && updated.name.empty()) { error = "a name cannot be empty"; return false; }
      // A new weapon type brings its handling (damage, rate, magazine, spread...); sounds stay.
      if (path == "weapon.kind" && updated.weapon) {
        game::WeaponComponent w = game::weapon_preset(updated.weapon->kind);
        w.fire_sound = updated.weapon->fire_sound;
        w.reload_sound = updated.weapon->reload_sound;
        updated.weapon = w;
      }
      *a = std::move(updated);
      ++changed_actors;
    }
    return changed_actors > 0;
  });
  coalesce_.clear();
  transaction_.clear();
  if (!ok && !error.empty()) report(error, "error");
  return ok;
}

bool Studio::set_world_property(const std::string& path, const json::Value& value, const std::string& transaction) {
  std::string error;
  coalesce_ = "w:" + path;
  transaction_ = transaction;
  const bool ok = edit("Edit world settings", [&](game::Level& l) {
    if (path == "level.name") {
      if (value.as_string().empty()) { error = "a level needs a name"; return false; }
      l.name = value.as_string();
      return true;
    }
    json::Value w = game::world_to_json(l.world);
    json::Value* target = at_path_mut(w, path.substr(6));
    if (!target) { error = "unknown world setting " + path; return false; }
    json::Value v = value;
    if (target->is_number() && v.is_string()) v = std::strtod(v.as_string().c_str(), nullptr);
    *target = std::move(v);
    game::WorldSettings out = l.world;
    if (!game::world_from_json(w, out, &error)) return false;
    l.world = out;
    return true;
  });
  coalesce_.clear();
  transaction_.clear();
  if (!ok && !error.empty()) report(error, "error");
  return ok;
}

bool Studio::add_component(const std::string& type) {
  if (selection_.empty()) return false;
  std::string error;
  const bool ok = edit(("Add " + type).c_str(), [&](game::Level& l) {
    for (ActorId id : selection_) {
      game::Actor* a = l.find(id);
      if (!a) continue;
      if (type == "mesh" && !a->mesh) a->mesh = game::MeshComponent{};
      else if (type == "light" && !a->light) a->light = game::LightComponent{};
      else if (type == "body" && !a->body) a->body = game::BodyComponent{};
      else if (type == "collider") {
        game::ColliderComponent c;
        if (a->mesh) c = game::fit_collider(*a->mesh);
        c.shape = a->mesh ? game::ColliderShape::automatic : game::ColliderShape::box;
        if (a->pickup || a->trigger) c.sensor = true;
        a->colliders.push_back(c);
      } else if (type == "joint") {
        game::JointComponent j;
        // Connect to another selected actor when two are selected.
        for (ActorId other : selection_) if (other != id) { j.target = other; break; }
        a->joints.push_back(j);
        if (!a->body) a->body = game::BodyComponent{};
      } else if (type == "controller" && !a->controller) {
        a->controller = game::ControllerComponent{};
        if (!a->body) a->body = game::BodyComponent{};
        a->body->motion = game::Motion::dynamic;
      } else if (type == "pickup" && !a->pickup) a->pickup = game::PickupComponent{};
      else if (type == "trigger" && !a->trigger) a->trigger = game::TriggerComponent{};
      else if (type == "rotator" && !a->rotator) a->rotator = game::RotatorComponent{};
      else if (type == "mover" && !a->mover) a->mover = game::MoverComponent{};
      else if (type == "audio" && !a->audio) a->audio = game::AudioComponent{};
      else if (type == "weapon" && !a->weapon) a->weapon = game::WeaponComponent{};
      else if (type == "health" && !a->health) a->health = game::HealthComponent{};
      else if (type == "city" && !a->city) {
        if (a->parent) { error = "a city must be a top-level actor"; return false; }
        a->city = game::CityComponent{};
      } else continue;
    }
    return true;
  });
  if (!ok && !error.empty()) report(error, "error");
  return ok;
}

bool Studio::remove_component(const std::string& type, i32 index) {
  if (selection_.empty()) return false;
  return edit(("Remove " + type).c_str(), [&](game::Level& l) {
    for (ActorId id : selection_) {
      game::Actor* a = l.find(id);
      if (!a) continue;
      if (type == "mesh") a->mesh.reset();
      else if (type == "light") a->light.reset();
      else if (type == "body") a->body.reset();
      else if (type == "colliders" && index >= 0 && static_cast<std::size_t>(index) < a->colliders.size()) a->colliders.erase(a->colliders.begin() + index);
      else if (type == "joints" && index >= 0 && static_cast<std::size_t>(index) < a->joints.size()) a->joints.erase(a->joints.begin() + index);
      else if (type == "controller") a->controller.reset();
      else if (type == "pickup") a->pickup.reset();
      else if (type == "trigger") a->trigger.reset();
      else if (type == "rotator") a->rotator.reset();
      else if (type == "mover") a->mover.reset();
      else if (type == "city") a->city.reset();
      else if (type == "audio") a->audio.reset();
      else if (type == "weapon") a->weapon.reset();
      else if (type == "health") a->health.reset();
    }
    return true;
  });
}

}  // namespace kern::studio
