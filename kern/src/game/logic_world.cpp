// Kern Logic in the game world: running the level's and the actors' graphs on
// the authority, the events the simulation raises for them, and the actor
// operations their nodes use (with automatic replication of what they change).
#include <algorithm>
#include "kern/core/dmath.h"
#include <cmath>

#include "kern/game/geometry.h"
#include "kern/game/runtime.h"
#include "runtime_internal.h"

namespace kern::game {

using rt::kDegree;


namespace {
logic::Value act(ActorId id) { return logic::Value::actor(static_cast<logic::ActorRef>(id)); }
}  // namespace

void GameWorld::start_logic() {
  logic_.reset();
  logic_host_.reset();
  prop_changes_.clear();
  prop_log_.clear();
  spawn_changes_.clear();
  spawn_log_.clear();
  logic_seed_ = 0x9e3779b9u ^ static_cast<u32>(level_.actors.size() * 2654435761u);
  if ((mode_ != SimMode::play && mode_ != SimMode::simulate) || starting_replica_) return;
  bool any = !level_.logic.empty();
  for (const Actor& a : level_.actors) any |= a.logic.has_value() && !a.logic->empty();
  if (!any) return;
  logic_host_ = std::make_unique<LogicHost>(*this);
  logic_ = std::make_unique<logic::Runtime>(*logic_host_);
  if (!level_.logic.empty()) logic_->add(level_.logic, 0, "Level");
  for (const Actor& a : level_.actors)
    if (a.logic && !a.logic->empty()) logic_->add(*a.logic, static_cast<logic::ActorRef>(a.id), a.name);
}

void GameWorld::logic_fire(const char* type, ActorId actor, ActorId other, std::vector<std::pair<std::string, logic::Value>> extra, std::string key) {
  if (!logic_) return;
  // The actor's graph, or its nearest ancestor's (a welded collider of a door belongs to the door).
  ActorId target = actor;
  for (const Actor* a = level_.find(actor); a; a = level_.find(a->parent))
    if (logic_->find(static_cast<logic::ActorRef>(a->id))) { target = a->id; break; }
  logic::Event e;
  e.type = type;
  e.target = static_cast<logic::ActorRef>(target);
  e.level = true;
  e.key = std::move(key);
  e.args.push_back({"actor", act(actor)});
  e.args.push_back({"other", act(other)});
  for (auto& x : extra) e.args.push_back(std::move(x));
  if (!target) {  // events about nobody in particular (game over...) reach every graph
    e.target = 0;
    e.level = false;
  }
  logic_->fire(e);
}

void GameWorld::logic_inputs(Pawn& pawn, const PlayInput& in) {
  if (!logic_) { pawn.prev_input = in; return; }
  const PlayInput& was = pawn.prev_input;
  const struct { const char* name; bool now, before; } actions[] = {
      {"jump", in.jump, was.jump},     {"fire", in.fire, was.fire},       {"aim", in.aim, was.aim},
      {"reload", in.reload, was.reload}, {"crouch", in.crouch, was.crouch}, {"interact", in.interact, was.interact},
      {"boost", in.boost, was.boost}};
  for (const auto& a : actions)
    if (a.now != a.before) logic_fire("game.input", pawn.actor, 0, {{"pressed", logic::Value::boolean(a.now)}}, a.name);
  if (in.interact && !was.interact) interact(pawn);
  pawn.prev_input = in;
}

void GameWorld::interact(Pawn& pawn) {
  // What the player looks at within 3 m (first person: the aim ray; others: the facing ray at chest height).
  Mat4 m;
  if (!actor_matrix(pawn.actor, m)) return;
  const Actor* a = level_.find(pawn.actor);
  const bool soldier = a && a->controller && a->controller->mode == ControlMode::soldier;
  Vec3 from = m.translation(), dir;
  if (soldier) {
    from = pawn.mover.position() + Vec3{0, 0, a_eye_height(pawn.actor) * (1 - 0.4f * pawn.crouch)};
    const f32 y = pawn.yaw * kDegree, p = pawn.pitch * kDegree;
    dir = {kern::dm::cos(p) * kern::dm::cos(y), kern::dm::cos(p) * kern::dm::sin(y), -kern::dm::sin(p)};
  } else {
    dir = {kern::dm::cos(pawn.yaw * kDegree), kern::dm::sin(pawn.yaw * kDegree), 0};
  }
  ActorId hit = 0;
  Vec3 point, normal;
  if (line_trace(from, from + dir * 3.0f, pawn.actor, hit, point, normal) && hit) {
    logic_fire("game.interact", hit, pawn.actor, {{"point", logic::Value::vector(point)}});
    return;
  }
  // Nothing in the line: the nearest actor with a graph within 2 m.
  ActorId best = 0;
  f32 best_d = 2.0f;
  for (const auto& inst : logic_->instances()) {
    if (!inst->self() || inst->self() == pawn.actor) continue;
    Mat4 w;
    if (!actor_matrix(inst->self(), w)) continue;
    const f32 d = distance(w.translation(), m.translation());
    if (d < best_d) { best_d = d; best = inst->self(); }
  }
  if (best) logic_fire("game.interact", best, pawn.actor, {{"point", logic::Value::vector(m.translation())}});
}

void GameWorld::key_event(const std::string& code, bool down) {
  if (!logic_ || !running() || paused_) return;
  logic::Event e;
  e.type = "game.key";
  e.key = code;
  e.args.push_back({"pressed", logic::Value::boolean(down)});
  e.args.push_back({"actor", act(player_)});
  logic_->fire(e);
}

void GameWorld::call_logic_event(const std::string& name, ActorId target) {
  if (logic_) logic_->call(name, static_cast<logic::ActorRef>(target));
}

// ---------------------------------------------------------------------------
// replication

void GameWorld::take_replicated_changes(std::vector<PropertyChange>& props, std::vector<SpawnChange>& spawns) {
  props.insert(props.end(), prop_changes_.begin(), prop_changes_.end());
  spawns.insert(spawns.end(), spawn_changes_.begin(), spawn_changes_.end());
  prop_changes_.clear();
  spawn_changes_.clear();
}

void GameWorld::replicated_state(std::vector<PropertyChange>& props, std::vector<SpawnChange>& spawns) const {
  spawns = spawn_log_;
  props = prop_log_;
}

namespace {
json::Value* path_mut(json::Value& root, std::string_view path) {
  json::Value* v = &root;
  while (!path.empty() && v) {
    const std::size_t dot = path.find('.');
    const std::string_view token = path.substr(0, dot);
    if (v->is_array()) {
      const std::size_t i = static_cast<std::size_t>(std::atoi(std::string(token).c_str()));
      v = i < v->size() ? &v->at(i) : nullptr;
    } else if (v->is_object()) {
      v = v->find(token);
    } else {
      v = nullptr;
    }
    path = dot == std::string_view::npos ? std::string_view() : path.substr(dot + 1);
  }
  return v;
}
}  // namespace

bool GameWorld::set_property(ActorId id, const std::string& path, const json::Value& value) {
  Actor* a = level_.find(id);
  if (!a || path.empty() || path == "id" || path == "parent" || path.rfind("components.logic", 0) == 0) return false;
  json::Value doc = actor_to_json(*a);
  json::Value* target = path_mut(doc, path);
  if (!target) return fail("actor \"%s\" has no property \"%s\"", a->name.c_str(), path.c_str());
  json::Value v = value;
  if (target->is_number() && v.is_string()) v = std::strtod(v.as_string().c_str(), nullptr);
  if (target->is_number() && v.is_bool()) v = v.as_bool() ? 1.0 : 0.0;
  if (target->is_bool() && !v.is_bool()) v = v.truthy();
  if (target->is_array() && v.is_number() && target->size() == 3) {  // a number for a vector: all three
    json::Value arr = json::Value::array();
    for (int i = 0; i < 3; ++i) arr.push(v.as_number());
    v = std::move(arr);
  }
  *target = std::move(v);
  Actor updated;
  std::string error;
  if (!actor_from_json(doc, updated, &error)) return fail("%s", error.c_str());
  updated.id = a->id;
  updated.parent = a->parent;
  updated.logic = std::move(a->logic);
  *a = std::move(updated);
  // Presentation follows (meshes, lights read the level every frame; bodies keep their shapes).
  if (path.rfind("transform", 0) == 0) {
    auto b = body_of_actor_.find(id);
    if (b == body_of_actor_.end() || !b->second->body.valid()) world_[id] = level_.world_matrix(id);
    else {  // a body (static colliders included) follows its actor's new transform
      const Mat4 wm = level_.world_matrix(id);
      const Transform t = matrix_transform(wm);
      physics_.set_pose(b->second->body, physics::Pose{t.position, euler_to_quat(t.rotation)});
      b->second->prev = physics_.pose(b->second->body);
    }
  }
  if (a->mesh) sync_actor_mesh(*a, world_[id]);
  if (mode_ != SimMode::replica) {
    PropertyChange c{id, path, *target};
    prop_changes_.push_back(c);
    bool replaced = false;
    for (PropertyChange& p : prop_log_)
      if (p.actor == id && p.path == path) { p.value = c.value; replaced = true; }
    if (!replaced && prop_log_.size() < 4096) prop_log_.push_back(std::move(c));
  }
  return true;
}

ActorId GameWorld::spawn_copy(ActorId source, Vec3 position, Vec3 rotation_deg, ActorId id) {
  if (!running()) return 0;
  const Actor* src = level_.find(source);
  if (!src) { fail("spawn: no template actor %u", source); return 0; }
  std::vector<const Actor*> subtree{src};
  for (std::size_t i = 0; i < subtree.size(); ++i)
    for (ActorId k : level_.children(subtree[i]->id))
      if (const Actor* c = level_.find(k)) subtree.push_back(c);
  ActorId root = id ? id : level_.next_id;
  if (!id) for (const Actor& a : level_.actors) root = max_of(root, a.id + 1);
  for (std::size_t i = 0; i < subtree.size(); ++i)
    if (level_.find(root + static_cast<ActorId>(i))) { fail("spawn: actor ids are taken"); return 0; }
  std::unordered_map<ActorId, ActorId> remap;
  for (std::size_t i = 0; i < subtree.size(); ++i) remap[subtree[i]->id] = root + static_cast<ActorId>(i);
  // Copies are made from the level (they may be modified copies); collect them first, the vector grows.
  std::vector<Actor> copies;
  for (const Actor* a : subtree) copies.push_back(*a);
  std::vector<ActorId> created;
  for (Actor& copy : copies) {
    const ActorId old = copy.id;
    copy.id = remap[old];
    if (old == source) {
      copy.parent = 0;
      copy.transform.position = position;
      copy.transform.rotation = rotation_deg;
      const Mat4 sw = level_.world_matrix(source);
      copy.transform.scale = matrix_transform(sw).scale;
      copy.hidden = false;
    } else {
      copy.parent = remap.count(copy.parent) ? remap[copy.parent] : 0;
    }
    for (JointComponent& j : copy.joints)
      if (remap.count(j.target)) j.target = remap[j.target];
    level_.add(copy);
    created.push_back(copy.id);
    alive_[copy.id] = true;
    order_.push_back(copy.id);
  }
  level_.next_id = max_of(level_.next_id, root + static_cast<ActorId>(subtree.size()));
  for (ActorId aid : created) {
    world_[aid] = level_.world_matrix(aid);
    if (const Actor* a = level_.find(aid); a && a->mesh) sync_actor_mesh(*a, world_[aid]);
  }
  Level part;
  part.world = level_.world;
  for (ActorId aid : created) part.actors.push_back(*level_.find(aid));
  for (BodyPlan& plan : plan_bodies(part)) {
    if (!plan.owner) continue;
    auto sb = std::make_unique<SimBody>();
    sb->body = create_body(plan);
    if (!sb->body.valid()) continue;
    if (const Actor* owner = level_.find(plan.owner)) sb->scale = matrix_transform(level_.world_matrix(owner->id)).scale;
    if (mode_ == SimMode::replica) physics_.set_body_type(sb->body, physics::BodyType::kinematic);
    actor_of_body_[sb->body.id] = plan.owner;
    sb->plan = std::move(plan);
    body_of_actor_[sb->plan.owner] = sb.get();
    bodies_.push_back(std::move(sb));
  }
  if (mode_ != SimMode::replica) {
    for (ActorId aid : created) if (const Actor* a = level_.find(aid)) create_joints(*a);
    for (ActorId aid : created) init_combat(aid);
    SpawnChange c{source, root, position, rotation_deg};
    spawn_changes_.push_back(c);
    if (spawn_log_.size() < 4096) spawn_log_.push_back(c);
    if (logic_)
      for (ActorId aid : created)
        if (const Actor* a = level_.find(aid); a && a->logic && !a->logic->empty()) logic_->add(*a->logic, static_cast<logic::ActorRef>(aid), a->name);
  }
  return root;
}

void GameWorld::destroy_actor(ActorId id) {
  if (!level_.find(id)) return;
  set_actor_alive(id, false);
  for (std::size_t i = 0; i < respawns_.size();)
    if (respawns_[i].id == id) respawns_.erase(respawns_.begin() + static_cast<std::ptrdiff_t>(i));
    else ++i;
  if (auto h = health_.find(id); h != health_.end()) { h->second.dead = true; h->second.respawn_at = -1; }
  if (logic_) logic_->remove(static_cast<logic::ActorRef>(id));
}

// ---------------------------------------------------------------------------
// actor operations

bool GameWorld::teleport(ActorId id, Vec3 position, const Vec3* rotation_deg) {
  Actor* a = level_.find(id);
  if (!a || !finite(position)) return false;
  auto it = body_of_actor_.find(id);
  if (it != body_of_actor_.end() && it->second->body.valid()) {
    SimBody& b = *it->second;
    physics::Pose p = physics_.pose(b.body);
    p.position = position;
    if (rotation_deg) p.rotation = euler_to_quat(*rotation_deg);
    if (Pawn* pawn = pawn_of(id); pawn && pawn->use_mover) {
      pawn->mover.reset(position - Vec3{0, 0, pawn->mover_lift});
      if (rotation_deg) pawn->yaw = rotation_deg->z;
    }
    physics_.set_pose(b.body, p);
    b.prev = p;
    physics_.set_awake(b.body, true);
    // Static bodies are not in the snapshots (they never move by themselves): replicate the move.
    if (b.plan.motion == Motion::fixed && b.plan.owner == id && mode_ != SimMode::replica) {
      Mat4 parent;
      if (a->parent) parent = level_.world_matrix(a->parent);
      f64 inv[16];
      Vec3 local = position;
      if (a->parent && invert(parent, inv)) {
        Mat4 pi;
        for (int i = 0; i < 16; ++i) pi.m[i] = static_cast<f32>(inv[i]);
        local = pi.transform_point(position);
      }
      a->transform.position = local;
      if (rotation_deg) a->transform.rotation = *rotation_deg;
      auto record = [&](const char* path, Vec3 v) {
        json::Value arr = json::Value::array();
        arr.push(static_cast<f64>(v.x));
        arr.push(static_cast<f64>(v.y));
        arr.push(static_cast<f64>(v.z));
        PropertyChange c{id, path, arr};
        prop_changes_.push_back(c);
        for (PropertyChange& q : prop_log_) if (q.actor == id && q.path == path) { q.value = c.value; return; }
        if (prop_log_.size() < 4096) prop_log_.push_back(std::move(c));
      };
      record("transform.position", local);
      if (rotation_deg) record("transform.rotation", *rotation_deg);
    }
    return true;
  }
  // No body: the actor's transform (relative to its parent).
  Mat4 parent;
  if (a->parent) parent = world_.count(a->parent) ? world_[a->parent] : level_.world_matrix(a->parent);
  Vec3 local = position;
  f64 inv[16];
  if (a->parent && invert(parent, inv)) {
    Mat4 pi;
    for (int i = 0; i < 16; ++i) pi.m[i] = static_cast<f32>(inv[i]);
    local = pi.transform_point(position);
  }
  json::Value pos = json::Value::array();
  pos.push(static_cast<f64>(local.x));
  pos.push(static_cast<f64>(local.y));
  pos.push(static_cast<f64>(local.z));
  bool ok = set_property(id, "transform.position", pos);
  if (rotation_deg) {
    json::Value r = json::Value::array();
    r.push(static_cast<f64>(rotation_deg->x));
    r.push(static_cast<f64>(rotation_deg->y));
    r.push(static_cast<f64>(rotation_deg->z));
    ok = set_property(id, "transform.rotation", r) && ok;
  }
  world_[id] = level_.world_matrix(id);
  return ok;
}

Vec3 GameWorld::actor_velocity(ActorId id) const {
  if (const Pawn* p = const_cast<GameWorld*>(this)->pawn_of(id); p && p->use_mover) return p->mover.velocity();
  auto it = body_of_actor_.find(id);
  if (it == body_of_actor_.end() || !it->second->body.valid()) return {};
  return physics_.linear_velocity(it->second->body);
}

bool GameWorld::set_actor_velocity(ActorId id, Vec3 v) {
  if (Pawn* p = pawn_of(id); p && p->use_mover) { p->mover.set_velocity(v); return true; }
  auto it = body_of_actor_.find(id);
  if (it == body_of_actor_.end() || !it->second->body.valid()) return false;
  physics_.set_linear_velocity(it->second->body, v);
  physics_.set_awake(it->second->body, true);
  return true;
}

bool GameWorld::push_actor(ActorId id, Vec3 amount, bool impulse) {
  if (Pawn* p = pawn_of(id); p && p->use_mover) {
    // Characters: an impulse changes the velocity by impulse / mass (75 kg); a force for one step.
    const f32 mass = 75;
    const f32 h = 1.0f / clamp(level_.world.step_hz, 15.0f, 480.0f);
    p->mover.set_velocity(p->mover.velocity() + amount * ((impulse ? 1.0f : h) / mass));
    if (amount.z > 0) p->mover.jump(p->mover.velocity().z);
    return true;
  }
  const physics::Body b = actor_body(id);
  if (!b.valid() || physics_.body_type(b) != physics::BodyType::dynamic) return false;
  if (impulse) physics_.apply_linear_impulse_to_center(b, amount);
  else physics_.apply_force_to_center(b, amount);
  physics_.set_awake(b, true);
  return true;
}

bool GameWorld::set_simulate_physics(ActorId id, bool simulate) {
  auto it = body_of_actor_.find(id);
  if (it == body_of_actor_.end() || !it->second->body.valid()) return false;
  physics_.set_body_type(it->second->body, simulate ? physics::BodyType::dynamic : physics::BodyType::kinematic);
  physics_.set_awake(it->second->body, true);
  return true;
}

bool GameWorld::set_joint_motor(ActorId id, i32 joint, bool enable, f32 speed, f32 max_force) {
  for (auto& sj : joints_) {
    if (sj->actor != id || sj->broken || static_cast<i32>(sj->index) != joint) continue;
    const bool angular = sj->kind == JointKind::hinge || sj->kind == JointKind::wheel || sj->kind == JointKind::ball;
    physics_.set_joint_motor(sj->joint, enable, angular ? speed * kDegree : speed, max_force);
    return true;
  }
  return false;
}

ActorId GameWorld::find_actor(std::string_view name) const {
  for (const Actor& a : level_.actors) if (a.name == name && actor_alive(a.id)) return a.id;
  for (const Actor& a : level_.actors) if (a.name == name) return a.id;
  return 0;
}

void GameWorld::actors_with_tag(std::string_view tag, std::vector<ActorId>& out) const {
  out.clear();
  for (const Actor& a : level_.actors)
    if (actor_alive(a.id) && std::find(a.tags.begin(), a.tags.end(), tag) != a.tags.end()) out.push_back(a.id);
}

bool GameWorld::has_tag(ActorId id, std::string_view tag) const {
  const Actor* a = level_.find(id);
  return a && std::find(a->tags.begin(), a->tags.end(), tag) != a->tags.end();
}

bool GameWorld::line_trace(Vec3 from, Vec3 to, ActorId ignore, ActorId& actor, Vec3& point, Vec3& normal) {
  actor = 0;
  point = to;
  normal = {};
  if (!physics_.valid() || !finite(from) || !finite(to)) return false;
  const physics::RayHit hit = physics_.cast_ray(from, to - from, ~0ull, actor_body(ignore));
  if (!hit.hit) return false;
  actor = static_cast<ActorId>(physics_.shape_user(hit.shape));
  if (!actor) actor = actor_for_body(hit.body);
  point = hit.point;
  normal = hit.normal;
  return true;
}

void GameWorld::add_score(f32 points) {
  if (hud_.status == GameStatus::playing || hud_.status == GameStatus::inactive) hud_.score += points;
  const WorldSettings& w = level_.world;
  if (w.goal == GameGoal::score && w.score_target > 0 && hud_.score >= w.score_target) finish(GameStatus::won, "Target score reached!");
}

void GameWorld::end_game(bool won, const std::string& message) {
  finish(won ? GameStatus::won : GameStatus::lost, message.empty() ? (won ? "You win!" : "Game over") : message);
}

void GameWorld::heal(ActorId id, f32 amount) {
  auto h = health_.find(id);
  if (h == health_.end() || h->second.dead) return;
  const Actor* a = level_.find(id);
  const f32 max = a && a->health ? a->health->max_health : 100;
  h->second.hp = min_of(max, h->second.hp + max_of(0.0f, amount));
}

void GameWorld::give_ammo(ActorId pawn, i32 rounds) {
  if (Pawn* p = pawn_of(pawn)) p->reserve = max_of(0, p->reserve + rounds);
}

}  // namespace kern::game
