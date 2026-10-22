// Kern Logic: values, graph data, the node library, checking and the interpreter.
#include "kern/logic/logic.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace kern::logic {

// ---------------------------------------------------------------------------
// pin types and values

namespace {
constexpr const char* kPinNames[] = {"exec", "bool", "int", "float", "vector", "text", "actor"};

std::string number_text(f64 n) {
  if (std::isfinite(n) && std::fabs(n) < 1e15 && n == std::floor(n)) return std::to_string(static_cast<i64>(n));
  char b[48];
  std::snprintf(b, sizeof b, "%.4g", n);
  return b;
}
}  // namespace

const char* pin_type_name(PinType t) { return static_cast<u32>(t) < static_cast<u32>(PinType::count) ? kPinNames[static_cast<u32>(t)] : "?"; }

bool pin_type_from_name(std::string_view name, PinType& out) {
  for (u32 i = 0; i < static_cast<u32>(PinType::count); ++i)
    if (name == kPinNames[i]) { out = static_cast<PinType>(i); return true; }
  if (name == "number" || name == "double") { out = PinType::number; return true; }
  if (name == "boolean") { out = PinType::boolean; return true; }
  if (name == "integer") { out = PinType::integer; return true; }
  if (name == "string") { out = PinType::text; return true; }
  return false;
}

bool convertible(PinType from, PinType to) {
  if (from == to) return true;
  if (from == PinType::exec || to == PinType::exec) return false;
  if (to == PinType::text) return true;  // everything prints
  const bool from_num = from == PinType::integer || from == PinType::number || from == PinType::boolean;
  const bool to_num = to == PinType::integer || to == PinType::number || to == PinType::boolean;
  if (from_num && to_num) return true;
  if (to == PinType::boolean && from == PinType::actor) return true;  // is valid
  if (to == PinType::vector && (from == PinType::number || from == PinType::integer)) return true;  // splat
  return false;
}

Value Value::zero(PinType t) {
  Value v;
  v.type = t;
  return v;
}

bool Value::as_bool() const {
  switch (type) {
    case PinType::boolean: return b;
    case PinType::integer: return i != 0;
    case PinType::number: return n != 0;
    case PinType::vector: return length_sq(v) > 0;
    case PinType::text: return !s.empty() && s != "false" && s != "0";
    case PinType::actor: return a != 0;
    default: return false;
  }
}

i64 Value::as_int() const {
  switch (type) {
    case PinType::boolean: return b ? 1 : 0;
    case PinType::integer: return i;
    case PinType::number: return std::isfinite(n) ? static_cast<i64>(std::llround(std::trunc(n))) : 0;
    case PinType::text: return std::strtoll(s.c_str(), nullptr, 10);
    case PinType::actor: return a;
    default: return 0;
  }
}

f64 Value::as_number() const {
  switch (type) {
    case PinType::boolean: return b ? 1 : 0;
    case PinType::integer: return static_cast<f64>(i);
    case PinType::number: return n;
    case PinType::vector: return length(v);
    case PinType::text: return std::strtod(s.c_str(), nullptr);
    case PinType::actor: return a;
    default: return 0;
  }
}

Vec3 Value::as_vector() const {
  if (type == PinType::vector) return v;
  const f32 x = static_cast<f32>(as_number());
  return {x, x, x};
}

std::string Value::as_text() const {
  switch (type) {
    case PinType::boolean: return b ? "true" : "false";
    case PinType::integer: return std::to_string(i);
    case PinType::number: return number_text(n);
    case PinType::vector: return "(" + number_text(v.x) + ", " + number_text(v.y) + ", " + number_text(v.z) + ")";
    case PinType::text: return s;
    case PinType::actor: return a ? "actor " + std::to_string(a) : "none";
    default: return "";
  }
}

ActorRef Value::as_actor() const {
  if (type == PinType::actor) return a;
  if (type == PinType::integer) return static_cast<ActorRef>(max_of<i64>(0, i));
  if (type == PinType::number) return static_cast<ActorRef>(max_of(0.0, n));
  return 0;
}

Value Value::converted(PinType to) const {
  if (to == type) return *this;
  switch (to) {
    case PinType::boolean: return boolean(as_bool());
    case PinType::integer: return integer(as_int());
    case PinType::number: return number(as_number());
    case PinType::vector: return vector(as_vector());
    case PinType::text: return text(as_text());
    case PinType::actor: return actor(as_actor());
    default: return *this;
  }
}

json::Value Value::to_json() const {
  switch (type) {
    case PinType::boolean: return json::Value(b);
    case PinType::integer: return json::Value(static_cast<f64>(i));
    case PinType::number: return json::Value(std::isfinite(n) ? n : 0.0);
    case PinType::vector: {
      json::Value arr = json::Value::array();
      arr.push(static_cast<f64>(v.x));
      arr.push(static_cast<f64>(v.y));
      arr.push(static_cast<f64>(v.z));
      return arr;
    }
    case PinType::text: return json::Value(s);
    case PinType::actor: return json::Value(static_cast<f64>(a));
    default: return json::Value();
  }
}

Value Value::from_json(const json::Value& j, PinType t) {
  Value v = zero(t);
  switch (t) {
    case PinType::boolean: v.b = j.is_bool() ? j.as_bool() : j.truthy(); break;
    case PinType::integer: v.i = j.is_number() ? static_cast<i64>(std::llround(j.as_number())) : j.is_string() ? std::strtoll(j.as_string().c_str(), nullptr, 10) : 0; break;
    case PinType::number: v.n = j.is_number() ? j.as_number() : j.is_string() ? std::strtod(j.as_string().c_str(), nullptr) : 0; break;
    case PinType::vector:
      if (j.is_array() && j.size() >= 3) v.v = {static_cast<f32>(j[0].as_number()), static_cast<f32>(j[1].as_number()), static_cast<f32>(j[2].as_number())};
      else if (j.is_number()) v.v = Vec3{1, 1, 1} * static_cast<f32>(j.as_number());
      break;
    case PinType::text: v.s = j.is_string() ? j.as_string() : j.is_null() ? std::string() : json::stringify(j); break;
    case PinType::actor: v.a = j.is_number() ? static_cast<ActorRef>(max_of(0.0, j.as_number())) : 0; break;
    default: break;
  }
  return v;
}

bool Value::operator==(const Value& o) const {
  if (type != o.type) {
    if ((type == PinType::text) != (o.type == PinType::text)) return as_text() == o.as_text();
    return as_number() == o.as_number();
  }
  switch (type) {
    case PinType::boolean: return b == o.b;
    case PinType::integer: return i == o.i;
    case PinType::number: return n == o.n;
    case PinType::vector: return v.x == o.v.x && v.y == o.v.y && v.z == o.v.z;
    case PinType::text: return s == o.s;
    case PinType::actor: return a == o.a;
    default: return true;
  }
}

// ---------------------------------------------------------------------------
// graph data

const json::Value* Node::value(std::string_view pin) const {
  for (const auto& [k, v] : values) if (k == pin) return &v;
  return nullptr;
}

std::string Node::param(std::string_view key, std::string_view fallback) const {
  for (const auto& [k, v] : params) if (k == key) return v;
  return std::string(fallback);
}

void Node::set_value(std::string_view pin, json::Value v) {
  for (auto& [k, x] : values) if (k == pin) { x = std::move(v); return; }
  values.emplace_back(std::string(pin), std::move(v));
}

void Node::set_param(std::string_view key, std::string v) {
  for (auto& [k, x] : params) if (k == key) { x = std::move(v); return; }
  params.emplace_back(std::string(key), std::move(v));
}

const char* trigger_kind_name(TriggerKind k) {
  switch (k) {
    case TriggerKind::event: return "event";
    case TriggerKind::timer: return "timer";
    case TriggerKind::condition: return "condition";
    case TriggerKind::automatic: return "auto";
  }
  return "event";
}

const State* StateMachine::find(u32 state) const {
  for (const State& s : states) if (s.id == state) return &s;
  return nullptr;
}
const State* StateMachine::find(std::string_view name) const {
  for (const State& s : states) if (s.name == name) return &s;
  return nullptr;
}

Node* Graph::find(u32 id) {
  for (Node& n : nodes) if (n.id == id) return &n;
  return nullptr;
}
const Node* Graph::find(u32 id) const {
  for (const Node& n : nodes) if (n.id == id) return &n;
  return nullptr;
}
const Variable* Graph::variable(std::string_view name) const {
  for (const Variable& v : variables) if (v.name == name) return &v;
  return nullptr;
}
Variable* Graph::variable(std::string_view name) {
  for (Variable& v : variables) if (v.name == name) return &v;
  return nullptr;
}
StateMachine* Graph::machine(u32 id) {
  for (StateMachine& m : machines) if (m.id == id) return &m;
  return nullptr;
}
const StateMachine* Graph::machine(std::string_view name) const {
  for (const StateMachine& m : machines) if (m.name == name) return &m;
  return nullptr;
}
bool Graph::operator==(const Graph& o) const { return graph_to_json(*this) == graph_to_json(o); }

json::Value graph_to_json(const Graph& g) {
  json::Value root = json::Value::object();
  root.set("kind", "kern-logic");
  root.set("version", 1);
  root.set("nextId", g.next_id);
  json::Value vars = json::Value::array();
  for (const Variable& v : g.variables) {
    json::Value o = json::Value::object();
    o.set("name", v.name);
    o.set("type", pin_type_name(v.type));
    o.set("value", v.value);
    if (!v.help.empty()) o.set("help", v.help);
    vars.push(std::move(o));
  }
  root.set("variables", std::move(vars));
  json::Value nodes = json::Value::array();
  for (const Node& n : g.nodes) {
    json::Value o = json::Value::object();
    o.set("id", n.id);
    o.set("type", n.type);
    o.set("x", static_cast<f64>(std::round(n.x)));
    o.set("y", static_cast<f64>(std::round(n.y)));
    if (!n.params.empty()) {
      json::Value p = json::Value::object();
      for (const auto& [k, v] : n.params) p.set(k, v);
      o.set("params", std::move(p));
    }
    if (!n.values.empty()) {
      json::Value p = json::Value::object();
      for (const auto& [k, v] : n.values) p.set(k, v);
      o.set("values", std::move(p));
    }
    if (!n.comment.empty()) o.set("comment", n.comment);
    nodes.push(std::move(o));
  }
  root.set("nodes", std::move(nodes));
  json::Value links = json::Value::array();
  for (const Link& l : g.links) {
    json::Value o = json::Value::array();
    o.push(l.from);
    o.push(l.out);
    o.push(l.to);
    o.push(l.in);
    links.push(std::move(o));
  }
  root.set("links", std::move(links));
  json::Value machines = json::Value::array();
  for (const StateMachine& m : g.machines) {
    json::Value o = json::Value::object();
    o.set("id", m.id);
    o.set("name", m.name);
    json::Value states = json::Value::array();
    for (const State& s : m.states) {
      json::Value x = json::Value::object();
      x.set("id", s.id);
      x.set("name", s.name);
      x.set("x", static_cast<f64>(std::round(s.x)));
      x.set("y", static_cast<f64>(std::round(s.y)));
      if (s.initial) x.set("initial", true);
      states.push(std::move(x));
    }
    o.set("states", std::move(states));
    json::Value transitions = json::Value::array();
    for (const Transition& t : m.transitions) {
      json::Value x = json::Value::object();
      x.set("id", t.id);
      x.set("from", t.from);
      x.set("to", t.to);
      x.set("trigger", trigger_kind_name(t.trigger));
      if (t.trigger == TriggerKind::event) x.set("event", t.event);
      if (t.trigger == TriggerKind::timer) x.set("seconds", static_cast<f64>(t.seconds));
      if (t.trigger == TriggerKind::condition) x.set("variable", t.variable);
      if (!t.label.empty()) x.set("label", t.label);
      transitions.push(std::move(x));
    }
    o.set("transitions", std::move(transitions));
    machines.push(std::move(o));
  }
  root.set("machines", std::move(machines));
  return root;
}

bool graph_from_json(const json::Value& j, Graph& out, std::string* error) {
  auto bad = [&](const std::string& why) {
    if (error) *error = "logic graph: " + why;
    return false;
  };
  if (!j.is_object()) return bad("not an object");
  Graph g;
  u32 max_id = 0;
  auto id_of = [&](const json::Value& v) -> u32 {
    const f64 n = v.as_number(0);
    if (!(n >= 1 && n < 1e9)) return 0;
    const u32 id = static_cast<u32>(n);
    max_id = max_of(max_id, id);
    return id;
  };
  for (const json::Value& v : j["variables"].items()) {
    Variable var;
    var.name = v["name"].as_string();
    if (var.name.empty() || var.name.size() > 64) return bad("a variable needs a name (1-64 characters)");
    if (g.variable(var.name)) return bad("variable \"" + var.name + "\" is declared twice");
    if (!pin_type_from_name(v["type"].as_string(), var.type) || var.type == PinType::exec) return bad("variable \"" + var.name + "\" has an unknown type");
    var.value = Value::from_json(v["value"], var.type).to_json();
    var.help = v["help"].as_string();
    g.variables.push_back(std::move(var));
  }
  for (const json::Value& v : j["nodes"].items()) {
    Node n;
    n.id = id_of(v["id"]);
    if (!n.id) return bad("a node has no valid id");
    if (g.find(n.id)) return bad("node id " + std::to_string(n.id) + " is used twice");
    n.type = v["type"].as_string();
    if (n.type.empty()) return bad("node " + std::to_string(n.id) + " has no type");
    n.x = static_cast<f32>(v["x"].as_number());
    n.y = static_cast<f32>(v["y"].as_number());
    if (!std::isfinite(n.x) || !std::isfinite(n.y) || std::fabs(n.x) > 1e6 || std::fabs(n.y) > 1e6) n.x = n.y = 0;
    for (const auto& [k, p] : v["params"].members()) n.params.emplace_back(k, p.is_string() ? p.as_string() : json::stringify(p));
    for (const auto& [k, p] : v["values"].members()) n.values.emplace_back(k, p);
    n.comment = v["comment"].as_string();
    g.nodes.push_back(std::move(n));
  }
  for (const json::Value& v : j["links"].items()) {
    Link l;
    if (v.is_array() && v.size() == 4) {
      l.from = static_cast<u32>(v[0].as_number());
      l.out = v[1].as_string();
      l.to = static_cast<u32>(v[2].as_number());
      l.in = v[3].as_string();
    } else {
      l.from = static_cast<u32>(v["from"].as_number());
      l.out = v["out"].as_string();
      l.to = static_cast<u32>(v["to"].as_number());
      l.in = v["in"].as_string();
    }
    if (!g.find(l.from) || !g.find(l.to)) return bad("a link refers to a missing node");
    if (std::find(g.links.begin(), g.links.end(), l) == g.links.end()) g.links.push_back(std::move(l));
  }
  for (const json::Value& v : j["machines"].items()) {
    StateMachine m;
    m.id = id_of(v["id"]);
    if (!m.id) return bad("a state machine has no valid id");
    m.name = v["name"].as_string();
    if (m.name.empty()) m.name = "State Machine";
    for (const json::Value& s : v["states"].items()) {
      State st;
      st.id = id_of(s["id"]);
      st.name = s["name"].as_string();
      if (!st.id || st.name.empty()) return bad("a state of \"" + m.name + "\" needs an id and a name");
      if (m.find(st.name)) return bad("state \"" + st.name + "\" exists twice in \"" + m.name + "\"");
      st.x = static_cast<f32>(s["x"].as_number());
      st.y = static_cast<f32>(s["y"].as_number());
      st.initial = s["initial"].as_bool();
      m.states.push_back(std::move(st));
    }
    for (const json::Value& t : v["transitions"].items()) {
      Transition tr;
      tr.id = id_of(t["id"]);
      tr.from = static_cast<u32>(t["from"].as_number());
      tr.to = static_cast<u32>(t["to"].as_number());
      const std::string kind = t["trigger"].is_string() ? t["trigger"].as_string() : std::string("event");
      tr.trigger = kind == "timer" ? TriggerKind::timer : kind == "condition" ? TriggerKind::condition : kind == "auto" ? TriggerKind::automatic : TriggerKind::event;
      tr.event = t["event"].as_string();
      tr.seconds = static_cast<f32>(clamp(t["seconds"].as_number(1), 0.0, 1e6));
      tr.variable = t["variable"].as_string();
      tr.label = t["label"].as_string();
      if (!tr.id || !m.find(tr.to) || (tr.from && !m.find(tr.from))) return bad("a transition of \"" + m.name + "\" connects missing states");
      m.transitions.push_back(std::move(tr));
    }
    g.machines.push_back(std::move(m));
  }
  g.next_id = max_of(max_id + 1, static_cast<u32>(j["nextId"].as_number(0)));
  out = std::move(g);
  return true;
}

// ---------------------------------------------------------------------------
// library

void Library::add(NodeDef def) {
  auto it = index_.find(def.type);
  if (it != index_.end()) { *defs_[it->second] = std::move(def); return; }
  index_[def.type] = defs_.size();
  defs_.push_back(std::make_unique<NodeDef>(std::move(def)));
}

const NodeDef* Library::find(std::string_view type) const {
  auto it = index_.find(std::string(type));
  return it == index_.end() ? nullptr : defs_[it->second].get();
}

void Library::pins(const Node& node, const Graph& graph, std::vector<PinDef>& in, std::vector<PinDef>& out) const {
  in.clear();
  out.clear();
  const NodeDef* d = find(node.type);
  if (!d) return;
  in = d->inputs;
  out = d->outputs;
  if (d->shape) d->shape(*d, node, graph, in, out);
}

std::string Library::title(const Node& node) const {
  const NodeDef* d = find(node.type);
  if (!d) return node.type;
  return d->title ? d->title(*d, node) : d->label;
}

Library& library() { return Library::core(); }

// ---------------------------------------------------------------------------
// checking

std::vector<Issue> check(const Graph& g, const Library& lib) {
  std::vector<Issue> out;
  auto err = [&](u32 node, std::string text) { out.push_back({node, true, std::move(text)}); };
  auto warn = [&](u32 node, std::string text) { out.push_back({node, false, std::move(text)}); };
  std::unordered_map<u32, std::pair<std::vector<PinDef>, std::vector<PinDef>>> pins;
  for (const Node& n : g.nodes) {
    const NodeDef* d = lib.find(n.type);
    if (!d) { err(n.id, "unknown node type \"" + n.type + "\""); continue; }
    auto& p = pins[n.id];
    lib.pins(n, g, p.first, p.second);
    for (const ParamDef& pd : d->params) {
      const std::string v = n.param(pd.key, pd.def);
      if (pd.kind == ParamDef::Kind::variable && !g.variable(v)) err(n.id, lib.title(n) + ": no variable \"" + v + "\"");
      if (pd.kind == ParamDef::Kind::machine && !g.machine(v)) err(n.id, lib.title(n) + ": no state machine \"" + v + "\"");
      if (pd.kind == ParamDef::Kind::state) {
        const StateMachine* m = g.machine(n.param("machine"));
        if (m && !v.empty() && v != "any" && !m->find(v)) err(n.id, lib.title(n) + ": \"" + m->name + "\" has no state \"" + v + "\"");
      }
      if (pd.kind == ParamDef::Kind::event && v.empty()) err(n.id, lib.title(n) + ": the event needs a name");
    }
  }
  auto find_pin = [&](u32 node, const std::string& id, bool input) -> const PinDef* {
    auto it = pins.find(node);
    if (it == pins.end()) return nullptr;
    for (const PinDef& p : input ? it->second.first : it->second.second) if (p.id == id) return &p;
    return nullptr;
  };
  std::unordered_map<std::string, int> data_in_links;
  for (const Link& l : g.links) {
    const PinDef* o = find_pin(l.from, l.out, false);
    const PinDef* i = find_pin(l.to, l.in, true);
    if (!o || !i) {
      if (pins.count(l.from) && pins.count(l.to)) err(l.to, "a wire connects a pin that does not exist (" + l.out + " -> " + l.in + ")");
      continue;
    }
    if ((o->type == PinType::exec) != (i->type == PinType::exec)) { err(l.to, "an exec wire is connected to a data pin"); continue; }
    if (!convertible(o->type, i->type)) err(l.to, std::string("cannot convert ") + pin_type_name(o->type) + " to " + pin_type_name(i->type) + " (" + i->label + ")");
    if (i->type != PinType::exec && ++data_in_links[std::to_string(l.to) + ":" + l.in] > 1) err(l.to, "input \"" + i->label + "\" has more than one wire");
  }
  // Pure nodes must not depend on themselves.
  std::unordered_map<u32, int> mark;
  std::function<bool(u32)> cyclic = [&](u32 id) -> bool {
    int& m = mark[id];
    if (m == 1) return true;
    if (m == 2) return false;
    m = 1;
    const Node* n = g.find(id);
    const NodeDef* d = n ? lib.find(n->type) : nullptr;
    if (d && d->kind == NodeKind::pure)
      for (const Link& l : g.links)
        if (l.to == id) {
          const Node* src = g.find(l.from);
          const NodeDef* sd = src ? lib.find(src->type) : nullptr;
          if (sd && sd->kind == NodeKind::pure && cyclic(l.from)) return true;
        }
    m = 2;
    return false;
  };
  for (const Node& n : g.nodes) {
    const NodeDef* d = lib.find(n.type);
    if (d && d->kind == NodeKind::pure && cyclic(n.id)) { err(n.id, lib.title(n) + " depends on itself"); mark.clear(); }
  }
  // Events with nothing attached, actions nobody reaches.
  for (const Node& n : g.nodes) {
    const NodeDef* d = lib.find(n.type);
    if (!d) continue;
    bool exec_in = false, exec_linked = false;
    for (const PinDef& p : pins[n.id].first)
      if (p.type == PinType::exec) {
        exec_in = true;
        for (const Link& l : g.links) if (l.to == n.id && l.in == p.id) exec_linked = true;
      }
    if (exec_in && !exec_linked && d->kind != NodeKind::pure) warn(n.id, lib.title(n) + " never runs (nothing is wired to its exec input)");
  }
  for (const StateMachine& m : g.machines) {
    int initial = 0;
    for (const State& s : m.states) initial += s.initial ? 1 : 0;
    if (!m.states.empty() && initial != 1) err(0, "state machine \"" + m.name + "\" needs exactly one initial state");
    for (const Transition& t : m.transitions) {
      if (t.trigger == TriggerKind::event && t.event.empty()) err(0, "a transition of \"" + m.name + "\" needs an event name");
      if (t.trigger == TriggerKind::condition) {
        const Variable* v = g.variable(t.variable);
        if (!v) err(0, "a transition of \"" + m.name + "\" waits for the missing variable \"" + t.variable + "\"");
      }
    }
  }
  return out;
}

// ---------------------------------------------------------------------------
// instances

Instance::Instance(Runtime& rt, const Graph& graph, ActorRef self, std::string name) : rt_(rt), graph_(graph), self_(self), name_(std::move(name)) {
  const Library& lib = rt.lib();
  for (const Variable& v : graph_.variables) vars_[v.name] = Value::from_json(v.value, v.type);
  nodes_.resize(graph_.nodes.size());
  for (std::size_t i = 0; i < graph_.nodes.size(); ++i) {
    CNode& c = nodes_[i];
    c.node = &graph_.nodes[i];
    c.def = lib.find(c.node->type);
    index_[c.node->id] = static_cast<int>(i);
    if (!c.def) continue;
    lib.pins(*c.node, graph_, c.in, c.out);
    c.outputs.resize(c.out.size());
    for (std::size_t k = 0; k < c.out.size(); ++k) c.outputs[k] = Value::zero(c.out[k].type);
    c.data_in.resize(c.in.size());
    c.literal.resize(c.in.size());
    c.exec_in_index.assign(c.in.size(), -1);
    int ordinal = 0;
    for (std::size_t k = 0; k < c.in.size(); ++k) {
      const PinDef& p = c.in[k];
      if (p.type == PinType::exec) { c.exec_in_index[k] = ordinal++; continue; }
      const json::Value* lit = c.node->value(p.id);
      c.literal[k] = Value::from_json(lit ? *lit : p.def, p.type);
    }
  }
  // Wires.
  for (std::size_t i = 0; i < nodes_.size(); ++i) {
    CNode& c = nodes_[i];
    if (!c.def) continue;
    int exec_outs = 0;
    for (const PinDef& p : c.out) exec_outs += p.type == PinType::exec ? 1 : 0;
    c.exec_out.assign(static_cast<std::size_t>(exec_outs), -1);
    c.exec_out_entry.assign(static_cast<std::size_t>(exec_outs), 0);
  }
  for (const Link& l : graph_.links) {
    auto fi = index_.find(l.from), ti = index_.find(l.to);
    if (fi == index_.end() || ti == index_.end()) continue;
    CNode& from = nodes_[static_cast<std::size_t>(fi->second)];
    CNode& to = nodes_[static_cast<std::size_t>(ti->second)];
    if (!from.def || !to.def) continue;
    int out_index = -1, exec_ordinal = -1, ord = 0;
    for (std::size_t k = 0; k < from.out.size(); ++k) {
      if (from.out[k].id == l.out) { out_index = static_cast<int>(k); if (from.out[k].type == PinType::exec) exec_ordinal = ord; break; }
      if (from.out[k].type == PinType::exec) ++ord;
    }
    int in_index = -1;
    for (std::size_t k = 0; k < to.in.size(); ++k) if (to.in[k].id == l.in) { in_index = static_cast<int>(k); break; }
    if (out_index < 0 || in_index < 0) continue;
    if (exec_ordinal >= 0) {
      if (to.in[static_cast<std::size_t>(in_index)].type != PinType::exec) continue;
      from.exec_out[static_cast<std::size_t>(exec_ordinal)] = ti->second;
      from.exec_out_entry[static_cast<std::size_t>(exec_ordinal)] = to.exec_in_index[static_cast<std::size_t>(in_index)];
    } else if (to.in[static_cast<std::size_t>(in_index)].type != PinType::exec && convertible(from.out[static_cast<std::size_t>(out_index)].type, to.in[static_cast<std::size_t>(in_index)].type)) {
      to.data_in[static_cast<std::size_t>(in_index)] = {fi->second, out_index};
    }
  }
  for (const StateMachine& m : graph_.machines) machines_.push_back({&m, 0, 0});
}

Value Instance::get(std::string_view variable) const {
  auto it = vars_.find(std::string(variable));
  return it == vars_.end() ? Value() : it->second;
}

bool Instance::set(std::string_view variable, const Value& v) {
  auto it = vars_.find(std::string(variable));
  if (it == vars_.end()) return false;
  it->second = v.converted(it->second.type);
  return true;
}

std::string Instance::state(std::string_view machine) const {
  for (const Machine& m : machines_)
    if (m.def->name == machine) {
      const State* s = m.def->find(m.state);
      return s ? s->name : std::string();
    }
  return {};
}

bool Instance::set_state(std::string_view machine, std::string_view state) {
  for (std::size_t i = 0; i < machines_.size(); ++i)
    if (machines_[i].def->name == machine) {
      const State* s = machines_[i].def->find(state);
      if (!s) return false;
      rt_.enter_state(*this, i, s->id);
      return true;
    }
  return false;
}

// ---------------------------------------------------------------------------
// runtime

const Value* Event::arg(std::string_view name) const {
  for (const auto& [k, v] : args) if (k == name) return &v;
  return nullptr;
}

Runtime::Runtime(Host& host, const Library& lib) : host_(host), lib_(lib) {}
Runtime::~Runtime() = default;

Instance* Runtime::add(const Graph& graph, ActorRef self, std::string name) {
  auto inst = std::make_unique<Instance>(*this, graph, self, std::move(name));
  for (const Issue& i : check(graph, lib_))
    if (i.error) {
      host_.error(inst->name() + ": " + i.text);
      inst->disabled_ = true;
    }
  instances_.push_back(std::move(inst));
  return instances_.back().get();
}

void Runtime::remove(ActorRef self) {
  for (std::size_t i = 0; i < instances_.size();) {
    if (instances_[i]->self() == self) {
      Instance* gone = instances_[i].get();
      pending_.erase(std::remove_if(pending_.begin(), pending_.end(), [&](const Pending& p) { return p.inst == gone; }), pending_.end());
      instances_.erase(instances_.begin() + static_cast<std::ptrdiff_t>(i));
    } else ++i;
  }
}

void Runtime::clear() {
  instances_.clear();
  pending_.clear();
  trace_.clear();
}

Instance* Runtime::find(ActorRef self) {
  for (auto& i : instances_) if (i->self() == self) return i.get();
  return nullptr;
}

bool Runtime::over_budget(Instance& inst) {
  if (++executed_ - tick_start_ <= budget) return false;
  if (!inst.disabled_) {
    host_.error(inst.name() + ": more than " + std::to_string(budget) + " nodes ran in one step - infinite loop? The graph is stopped.");
    inst.disabled_ = true;
  }
  return true;
}

void Runtime::begin() {
  tick_start_ = executed_;
  for (std::size_t k = 0; k < instances_.size(); ++k) {
    Instance& inst = *instances_[k];
    if (inst.begun_ || inst.disabled_) continue;
    inst.begun_ = true;
    for (std::size_t m = 0; m < inst.machines_.size(); ++m)
      for (const State& s : inst.machines_[m].def->states)
        if (s.initial) { enter_state(inst, m, s.id); break; }
    Event e;
    e.type = "logic.begin";
    e.target = inst.self();
    for (std::size_t i = 0; i < inst.nodes_.size(); ++i)
      if (inst.nodes_[i].def && inst.nodes_[i].def->type == "event.begin") run_from(inst, static_cast<int>(i), 0);
  }
}

void Runtime::tick(f32 dt) {
  tick_start_ = executed_;
  const f64 now = host_.time();
  // Latent actions that are due (in order of their time).
  std::stable_sort(pending_.begin(), pending_.end(), [](const Pending& a, const Pending& b) { return a.at < b.at; });
  std::vector<Pending> due;
  for (std::size_t i = 0; i < pending_.size();) {
    if (pending_[i].at <= now + 1e-9) { due.push_back(pending_[i]); pending_.erase(pending_.begin() + static_cast<std::ptrdiff_t>(i)); }
    else ++i;
  }
  for (const Pending& p : due)
    if (!p.inst->disabled_) run_from(*p.inst, p.node, p.entry);
  // Tick events.
  Event tick;
  tick.type = "logic.tick";
  tick.args.push_back({"delta", Value::number(dt)});
  fire(tick);
  // State machine timers and conditions.
  for (std::size_t k = 0; k < instances_.size(); ++k) check_transitions(*instances_[k], nullptr);
}

void Runtime::fire(const Event& e) {
  // Instances may be added while handlers run (spawned actors): iterate by index.
  for (std::size_t k = 0; k < instances_.size(); ++k) {
    Instance& inst = *instances_[k];
    if (inst.disabled_ || (e.target && inst.self() != e.target && !(e.level && inst.self() == 0))) continue;
    for (std::size_t i = 0; i < inst.nodes_.size(); ++i) {
      Instance::CNode& c = inst.nodes_[i];
      if (!c.def || c.def->kind != NodeKind::event || c.def->event != e.type) continue;
      bool ok;
      if (c.def->accepts) ok = c.def->accepts(*c.node, e);
      else {
        const std::string want = c.def->params.empty() ? std::string() : c.node->param(c.def->params[0].key, c.def->params[0].def);
        ok = e.key.empty() || want.empty() || want == "any" || want == e.key;
      }
      if (!ok) continue;
      for (std::size_t o = 0; o < c.out.size(); ++o)
        if (const Value* v = e.arg(c.out[o].id)) c.outputs[o] = v->converted(c.out[o].type);
      run_from(inst, static_cast<int>(i), 0);
      if (inst.disabled_) break;
    }
  }
}

void Runtime::call(std::string_view name, ActorRef target, std::vector<std::pair<std::string, Value>> args) {
  Event e;
  e.type = "logic.custom";
  e.key = std::string(name);
  e.target = target;
  e.args = std::move(args);
  fire(e);
  const std::string n(name);
  for (std::size_t k = 0; k < instances_.size(); ++k)
    if (!target || instances_[k]->self() == target) check_transitions(*instances_[k], &n);
}

void Runtime::enter_state(Instance& inst, std::size_t machine, u32 state) {
  Instance::Machine& m = inst.machines_[machine];
  const State* old = m.def->find(m.state);
  const State* next = m.def->find(state);
  if (!next) return;
  if (old) {
    Event e;
    e.type = "logic.state_exit";
    e.target = inst.self();
    e.key = m.def->name + "/" + old->name;
    fire(e);
  }
  m.state = state;
  m.entered = host_.time();
  Event e;
  e.type = "logic.state_enter";
  e.target = inst.self();
  e.key = m.def->name + "/" + next->name;
  e.args.push_back({"previous", Value::text(old ? old->name : std::string())});
  fire(e);
}

void Runtime::check_transitions(Instance& inst, const std::string* event) {
  if (inst.disabled_ || !inst.begun_) return;
  const f64 now = host_.time();
  for (std::size_t mi = 0; mi < inst.machines_.size(); ++mi) {
    // One transition per machine per call (automatic chains advance one state per step).
    for (const Transition& t : inst.machines_[mi].def->transitions) {
      const Instance::Machine& m = inst.machines_[mi];
      if (t.from && t.from != m.state) continue;
      if (t.to == m.state && !t.from) continue;
      bool go = false;
      switch (t.trigger) {
        case TriggerKind::event: go = event && *event == t.event; break;
        case TriggerKind::timer: go = !event && now - m.entered >= t.seconds; break;
        case TriggerKind::condition: go = !event && inst.get(t.variable).as_bool(); break;
        case TriggerKind::automatic: go = !event; break;
      }
      if (go) { enter_state(inst, mi, t.to); break; }
    }
  }
}

void Runtime::run_from(Instance& inst, int start, int entry) {
  if (++depth_ > 64) {  // events raised from handlers raising events...
    --depth_;
    host_.error(inst.name() + ": events nest more than 64 deep - the chain is stopped");
    return;
  }
  std::vector<std::pair<int, int>> stack{{start, entry}};
  while (!stack.empty() && !inst.disabled_) {
    auto [n, e] = stack.back();
    stack.pop_back();
    while (n >= 0 && !inst.disabled_) {
      if (over_budget(inst)) break;
      Instance::CNode& c = inst.nodes_[static_cast<std::size_t>(n)];
      if (!c.def) break;
      Context ctx(*this, inst, n, e);
      const int r = c.def->run ? c.def->run(ctx) : 0;
      if (tracing && trace_.size() < 4096) trace_.push_back({inst.name(), c.node->id, host_.time()});
      for (auto it = ctx.continuations_.rbegin(); it != ctx.continuations_.rend(); ++it) stack.push_back(*it);
      if (r < 0 || static_cast<std::size_t>(r) >= c.exec_out.size()) break;
      e = c.exec_out_entry[static_cast<std::size_t>(r)];
      n = c.exec_out[static_cast<std::size_t>(r)];
    }
  }
  --depth_;
}

// ---------------------------------------------------------------------------
// context

Context::Context(Runtime& rt, Instance& inst, int node, int entry) : rt_(rt), inst_(inst), node_(node), entry_(entry) {}

Value Context::pull(int node, int pin) {
  Instance::CNode& src = inst_.nodes_[static_cast<std::size_t>(node)];
  if (src.def && src.def->kind == NodeKind::pure && src.def->run) {
    if (eval_depth_ > 48 || rt_.over_budget(inst_)) return src.outputs[static_cast<std::size_t>(pin)];
    Context c(rt_, inst_, node, 0);
    c.eval_depth_ = eval_depth_ + 1;
    src.def->run(c);
  }
  return src.outputs[static_cast<std::size_t>(pin)];
}

Value Context::in(std::size_t index) {
  Instance::CNode& c = inst_.nodes_[static_cast<std::size_t>(node_)];
  if (index >= c.in.size()) return Value();
  const auto& src = c.data_in[index];
  if (src.node < 0) return c.literal[index];
  return pull(src.node, src.pin).converted(c.in[index].type);
}

Value Context::in(std::string_view pin) {
  const Instance::CNode& c = inst_.nodes_[static_cast<std::size_t>(node_)];
  for (std::size_t i = 0; i < c.in.size(); ++i) if (c.in[i].id == pin) return in(i);
  return Value();
}

void Context::out(std::size_t index, Value v) {
  Instance::CNode& c = inst_.nodes_[static_cast<std::size_t>(node_)];
  if (index < c.outputs.size()) c.outputs[index] = v.converted(c.out[index].type);
}

void Context::out(std::string_view pin, Value v) {
  const Instance::CNode& c = inst_.nodes_[static_cast<std::size_t>(node_)];
  for (std::size_t i = 0; i < c.out.size(); ++i) if (c.out[i].id == pin) { out(i, std::move(v)); return; }
}

int Context::exec(std::string_view pin) const {
  const Instance::CNode& c = inst_.nodes_[static_cast<std::size_t>(node_)];
  int ord = 0;
  for (const PinDef& p : c.out) {
    if (p.type != PinType::exec) continue;
    if (p.id == pin) return ord;
    ++ord;
  }
  return kStop;
}

int Context::wait(f64 seconds, int entry) {
  Runtime::Pending p;
  p.inst = &inst_;
  p.node = node_;
  p.at = rt_.host_.time() + max_of(0.0, seconds);
  p.entry = entry;
  p.serial = ++rt_.serial_;
  rt_.pending_.push_back(p);
  return kSuspend;
}

void Context::cancel_waits() {
  auto& v = rt_.pending_;
  v.erase(std::remove_if(v.begin(), v.end(), [&](const Runtime::Pending& p) { return p.inst == &inst_ && p.node == node_; }), v.end());
}

void Context::then(int exec_output) {
  const Instance::CNode& c = inst_.nodes_[static_cast<std::size_t>(node_)];
  if (exec_output < 0 || static_cast<std::size_t>(exec_output) >= c.exec_out.size()) return;
  const int target = c.exec_out[static_cast<std::size_t>(exec_output)];
  if (target >= 0) continuations_.push_back({target, c.exec_out_entry[static_cast<std::size_t>(exec_output)]});
}

void Context::loop_back(int entry) { continuations_.push_back({node_, entry}); }

}  // namespace kern::logic
