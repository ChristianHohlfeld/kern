// Kern Studio: editing logic graphs (the level's and the actors'). Every edit is
// a level edit (one undo step); the page draws what logic_json() describes.
#include <algorithm>
#include <cctype>
#include <cmath>

#include "internal.h"
#include "kern/audio/synth.h"
#include "kern/studio/studio.h"

namespace kern::studio {

namespace {

json::Value ok_result(bool v = true) {
  json::Value r = json::Value::object();
  r.set("ok", v);
  return r;
}
json::Value fail_result(const std::string& text) {
  json::Value r = json::Value::object();
  r.set("ok", false);
  r.set("error", text);
  return r;
}

std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

const char* kind_name(logic::NodeKind k) {
  switch (k) {
    case logic::NodeKind::event: return "event";
    case logic::NodeKind::action: return "action";
    case logic::NodeKind::pure: return "pure";
    case logic::NodeKind::flow: return "flow";
  }
  return "action";
}

const char* param_kind_name(logic::ParamDef::Kind k) {
  using K = logic::ParamDef::Kind;
  switch (k) {
    case K::text: return "text";
    case K::choice: return "choice";
    case K::variable: return "variable";
    case K::event: return "event";
    case K::machine: return "machine";
    case K::state: return "state";
    case K::key: return "key";
    case K::sound: return "sound";
    case K::tag: return "tag";
    case K::integer: return "integer";
  }
  return "text";
}

logic::Graph* graph_of(game::Level& l, u32 owner, bool create) {
  if (!owner) return &l.logic;
  game::Actor* a = l.find(owner);
  if (!a) return nullptr;
  if (!a->logic) {
    if (!create) return nullptr;
    a->logic = logic::Graph{};
  }
  return &*a->logic;
}

const logic::Graph* graph_of(const game::Level& l, u32 owner) {
  if (!owner) return &l.logic;
  const game::Actor* a = l.find(owner);
  return a && a->logic ? &*a->logic : nullptr;
}

/// Custom event names used anywhere in the level (for Call Event and transitions).
std::vector<std::string> event_names(const game::Level& l) {
  std::vector<std::string> out;
  auto scan = [&](const logic::Graph& g) {
    for (const logic::Node& n : g.nodes)
      if (n.type == "event.custom" || n.type == "flow.call") {
        const std::string name = n.param("name", "MyEvent");
        if (!name.empty() && std::find(out.begin(), out.end(), name) == out.end()) out.push_back(name);
      }
    for (const logic::StateMachine& m : g.machines)
      for (const logic::Transition& t : m.transitions)
        if (t.trigger == logic::TriggerKind::event && !t.event.empty() && std::find(out.begin(), out.end(), t.event) == out.end()) out.push_back(t.event);
  };
  scan(l.logic);
  for (const game::Actor& a : l.actors) if (a.logic) scan(*a.logic);
  std::sort(out.begin(), out.end());
  return out;
}

bool pin_of(const logic::Graph& g, u32 node, const std::string& pin, bool output, logic::PinDef& out) {
  const logic::Node* n = g.find(node);
  if (!n) return false;
  std::vector<logic::PinDef> in, outs;
  logic::library().pins(*n, g, in, outs);
  for (const logic::PinDef& p : output ? outs : in)
    if (p.id == pin) { out = p; return true; }
  return false;
}

/// Adds a link with the editor's rules: an exec output and a data input take one wire
/// (the new one replaces the old); exec inputs and data outputs fan in / out freely.
bool connect(logic::Graph& g, u32 from, const std::string& out, u32 to, const std::string& in, std::string& error) {
  if (from == to) { error = "a node cannot connect to itself"; return false; }
  logic::PinDef po, pi;
  if (!pin_of(g, from, out, true, po) || !pin_of(g, to, in, false, pi)) { error = "no such pin"; return false; }
  if ((po.type == logic::PinType::exec) != (pi.type == logic::PinType::exec)) { error = "exec pins connect to exec pins only"; return false; }
  if (!logic::convertible(po.type, pi.type)) {
    error = std::string("a ") + logic::pin_type_name(po.type) + " cannot go into a " + logic::pin_type_name(pi.type) + " pin";
    return false;
  }
  auto& links = g.links;
  if (po.type == logic::PinType::exec) links.erase(std::remove_if(links.begin(), links.end(), [&](const logic::Link& l) { return l.from == from && l.out == out; }), links.end());
  else links.erase(std::remove_if(links.begin(), links.end(), [&](const logic::Link& l) { return l.to == to && l.in == in; }), links.end());
  links.push_back({from, out, to, in});
  return true;
}

/// The first pin of `node` that `type` can connect to (output side when `want_output`).
std::string compatible_pin(const logic::Graph& g, const logic::Node& node, logic::PinType type, bool want_output) {
  std::vector<logic::PinDef> in, out;
  logic::library().pins(node, g, in, out);
  for (const logic::PinDef& p : want_output ? out : in) {
    if ((p.type == logic::PinType::exec) != (type == logic::PinType::exec)) continue;
    if (want_output ? logic::convertible(p.type, type) : logic::convertible(type, p.type)) return p.id;
  }
  return {};
}

}  // namespace

// ---------------------------------------------------------------------------
// view

json::Value Studio::logic_json() const {
  json::Value o = json::Value::object();
  o.set("open", logic_open_);
  o.set("owner", logic_owner_);
  const game::Actor* owner = logic_owner_ ? level_.find(logic_owner_) : nullptr;
  o.set("ownerName", logic_owner_ ? (owner ? owner->name : std::string("?")) : std::string("Level"));
  o.set("view", logic_view_);
  // Owners to switch between: the level and every actor with a graph.
  json::Value owners = json::Value::array();
  {
    json::Value lv = json::Value::object();
    lv.set("id", 0);
    lv.set("name", "Level");
    lv.set("nodes", static_cast<u32>(level_.logic.nodes.size()));
    owners.push(std::move(lv));
  }
  for (const game::Actor& a : level_.actors)
    if (a.logic) {
      json::Value x = json::Value::object();
      x.set("id", a.id);
      x.set("name", a.name);
      x.set("nodes", static_cast<u32>(a.logic->nodes.size()));
      owners.push(std::move(x));
    }
  o.set("owners", std::move(owners));
  const logic::Graph* g = graph_of(level_, logic_owner_);
  o.set("exists", g != nullptr);
  json::Value events = json::Value::array();
  for (const std::string& e : event_names(level_)) events.push(e);
  o.set("events", std::move(events));
  if (!g) return o;
  const logic::Library& lib = logic::library();
  std::unordered_map<u32, std::vector<const logic::Issue*>> by_node;
  const std::vector<logic::Issue> issues = logic::check(*g, lib);
  for (const logic::Issue& i : issues) by_node[i.node].push_back(&i);
  json::Value nodes = json::Value::array();
  for (const logic::Node& n : g->nodes) {
    const logic::NodeDef* d = lib.find(n.type);
    json::Value x = json::Value::object();
    x.set("id", n.id);
    x.set("type", n.type);
    x.set("title", lib.title(n));
    x.set("x", static_cast<f64>(n.x));
    x.set("y", static_cast<f64>(n.y));
    if (!n.comment.empty()) x.set("comment", n.comment);
    if (d) {
      x.set("kind", kind_name(d->kind));
      x.set("category", d->category);
      x.set("help", d->help);
      if (d->color) x.set("color", d->color);
      if (d->latent) x.set("latent", true);
    } else {
      x.set("kind", "action");
    }
    std::vector<logic::PinDef> in, out;
    lib.pins(n, *g, in, out);
    auto pins = [&](const std::vector<logic::PinDef>& v, bool output) {
      json::Value arr = json::Value::array();
      for (const logic::PinDef& p : v) {
        if (p.hidden) continue;
        json::Value pj = json::Value::object();
        pj.set("id", p.id);
        pj.set("label", p.label);
        pj.set("type", logic::pin_type_name(p.type));
        bool linked = false;
        for (const logic::Link& l : g->links)
          if ((output && l.from == n.id && l.out == p.id) || (!output && l.to == n.id && l.in == p.id)) linked = true;
        pj.set("linked", linked);
        if (!output && p.type != logic::PinType::exec) {
          const json::Value* v = n.value(p.id);
          pj.set("value", logic::Value::from_json(v ? *v : p.def, p.type).to_json());
        }
        arr.push(std::move(pj));
      }
      return arr;
    };
    x.set("inputs", pins(in, false));
    x.set("outputs", pins(out, true));
    if (d && !d->params.empty()) {
      json::Value params = json::Value::array();
      for (const logic::ParamDef& pd : d->params) {
        json::Value pj = json::Value::object();
        pj.set("key", pd.key);
        pj.set("label", pd.label);
        pj.set("kind", param_kind_name(pd.kind));
        pj.set("value", n.param(pd.key, pd.def));
        json::Value choices = json::Value::array();
        auto choice = [&](const std::string& v, const std::string& label) {
          json::Value c = json::Value::array();
          c.push(v);
          c.push(label);
          choices.push(std::move(c));
        };
        using K = logic::ParamDef::Kind;
        if (pd.kind == K::choice)
          for (std::size_t i = 0; i + 1 < pd.choices.size(); i += 2) choice(pd.choices[i], pd.choices[i + 1]);
        if (pd.kind == K::variable)
          for (const logic::Variable& v : g->variables) choice(v.name, v.name + " (" + logic::pin_type_name(v.type) + ")");
        if (pd.kind == K::machine)
          for (const logic::StateMachine& m : g->machines) choice(m.name, m.name);
        if (pd.kind == K::state) {
          if (n.type == "event.state_enter" || n.type == "event.state_exit") choice("any", "Any state");
          if (const logic::StateMachine* m = g->machine(n.param("machine", "State Machine")))
            for (const logic::State& s : m->states) choice(s.name, s.name);
        }
        if (pd.kind == K::event)
          for (const std::string& e : event_names(level_)) choice(e, e);
        if (pd.kind == K::sound) {
          for (const audio::BuiltinSound& b : audio::builtin_sounds()) choice(std::string("builtin:") + b.name, b.label);
          for (const game::SoundAsset& sa : level_.sounds) choice(sa.id, sa.name);
        }
        pj.set("choices", std::move(choices));
        params.push(std::move(pj));
      }
      x.set("params", std::move(params));
    }
    if (auto it = by_node.find(n.id); it != by_node.end()) {
      json::Value msgs = json::Value::array();
      bool error = false;
      for (const logic::Issue* i : it->second) { msgs.push(i->text); error |= i->error; }
      x.set("issues", std::move(msgs));
      x.set("error", error);
    }
    nodes.push(std::move(x));
  }
  o.set("nodes", std::move(nodes));
  json::Value links = json::Value::array();
  for (const logic::Link& l : g->links) {
    json::Value x = json::Value::array();
    x.push(l.from);
    x.push(l.out);
    x.push(l.to);
    x.push(l.in);
    links.push(std::move(x));
  }
  o.set("links", std::move(links));
  json::Value vars = json::Value::array();
  for (const logic::Variable& v : g->variables) {
    json::Value x = json::Value::object();
    x.set("name", v.name);
    x.set("type", logic::pin_type_name(v.type));
    x.set("value", logic::Value::from_json(v.value, v.type).to_json());
    vars.push(std::move(x));
  }
  o.set("variables", std::move(vars));
  json::Value graph = logic::graph_to_json(*g);
  o.set("machines", graph["machines"]);
  json::Value iss = json::Value::array();
  for (const logic::Issue& i : issues) {
    json::Value x = json::Value::object();
    x.set("node", i.node);
    x.set("error", i.error);
    x.set("text", i.text);
    iss.push(std::move(x));
  }
  o.set("issues", std::move(iss));
  return o;
}

json::Value Studio::logic_trace_json() const {
  json::Value o = json::Value::object();
  logic::Runtime* rt = world_->logic_runtime();
  if (!rt) return o;
  rt->tracing = true;
  const logic::Instance* inst = rt->find(static_cast<logic::ActorRef>(logic_owner_));
  json::Value ran = json::Value::array();
  const std::string name = logic_owner_ ? (level_.find(logic_owner_) ? level_.find(logic_owner_)->name : std::string()) : std::string("Level");
  for (const auto& t : rt->trace())
    if (t.instance == name) ran.push(t.node);
  rt->clear_trace();
  o.set("ran", std::move(ran));
  if (inst) {
    json::Value vars = json::Value::object();
    for (const logic::Variable& v : inst->graph().variables) vars.set(v.name, inst->get(v.name).to_json());
    o.set("variables", std::move(vars));
    json::Value states = json::Value::object();
    for (const logic::StateMachine& m : inst->graph().machines) states.set(m.name, inst->state(m.name));
    o.set("states", std::move(states));
    o.set("disabled", inst->disabled());
    o.set("tier", inst->tier());
  }
  return o;
}

// ---------------------------------------------------------------------------
// palette

json::Value Studio::logic_palette(const json::Value& c) const {
  const logic::Library& lib = logic::library();
  const logic::Graph* g = graph_of(level_, logic_owner_);
  logic::Graph empty;
  if (!g) g = &empty;
  const std::string query = lower(c["query"].is_string() ? c["query"].as_string() : std::string());
  std::vector<std::string> words;
  {
    std::string w;
    for (char ch : query + " ") {
      if (ch == ' ') { if (!w.empty()) words.push_back(w); w.clear(); }
      else w += ch;
    }
  }
  // Context: dragging from a pin keeps only nodes that can take the wire.
  bool from_pin = false, from_output = false;
  logic::PinType from_type = logic::PinType::exec;
  if (c["from"].is_object()) {
    logic::PinDef p;
    from_output = c["from"]["output"].as_bool();
    if (pin_of(*g, static_cast<u32>(c["from"]["node"].as_number()), c["from"]["pin"].as_string(), from_output, p)) {
      from_pin = true;
      from_type = p.type;
    }
  }
  struct Entry { std::string label, category, help, type; std::vector<std::pair<std::string, std::string>> params; int rank = 0; };
  std::vector<Entry> entries;
  auto consider = [&](Entry e) {
    // Fits the wire?
    if (from_pin) {
      logic::Node probe;
      probe.type = e.type;
      for (auto& [k, v] : e.params) probe.set_param(k, v);
      if (compatible_pin(*g, probe, from_type, !from_output).empty()) return;
    }
    const std::string hay = lower(e.label + " " + e.category + " " + e.help + " " + (lib.find(e.type) ? lib.find(e.type)->keywords : std::string()));
    for (const std::string& w : words) if (hay.find(w) == std::string::npos) return;
    const std::string l = lower(e.label);
    e.rank = query.empty() ? 0 : l.rfind(query, 0) == 0 ? 0 : l.find(query) != std::string::npos ? 1 : 2;
    entries.push_back(std::move(e));
  };
  for (const auto& d : lib.all()) {
    if (d->type == "var.get" || d->type == "var.set" || d->type == "var.add") continue;  // per variable below
    Entry e{d->label, d->category, d->help, d->type, {}};
    if (d->type == "event.state_enter" || d->type == "event.state_exit" || d->type == "state.set" || d->type == "state.is" || d->type == "state.get") {
      if (g->machines.empty()) continue;
      e.params.push_back({"machine", g->machines.front().name});
    }
    consider(std::move(e));
  }
  for (const logic::Variable& v : g->variables) {
    consider({"Get " + v.name, "Variables", std::string("Reads ") + v.name, "var.get", {{"variable", v.name}}});
    consider({"Set " + v.name, "Variables", std::string("Writes ") + v.name, "var.set", {{"variable", v.name}}});
    if (v.type == logic::PinType::number || v.type == logic::PinType::integer)
      consider({"Add to " + v.name, "Variables", "Adds to " + v.name, "var.add", {{"variable", v.name}}});
  }
  for (const std::string& ev : event_names(level_)) {
    consider({"Call " + ev, "Events", "Raises the custom event " + ev, "flow.call", {{"name", ev}}});
    consider({"Event " + ev, "Events", "Runs when " + ev + " is called", "event.custom", {{"name", ev}}});
  }
  for (const logic::StateMachine& m : g->machines)
    for (const logic::State& s : m.states) {
      consider({"Go to " + s.name, "State Machines", m.name + ": switch to " + s.name, "state.set", {{"machine", m.name}, {"state", s.name}}});
      consider({"On " + s.name + " Entered", "State Machines", m.name + " entered " + s.name, "event.state_enter", {{"machine", m.name}, {"state", s.name}}});
    }
  // Categories in the order people reach for them.
  static const char* order[] = {"Events", "Flow", "Variables", "Game", "Actors", "Physics", "Combat", "Effects", "Audio", "State Machines",
                                "Math", "Logic", "Vector", "Text", "Values", "Utilities"};
  auto cat_rank = [](const std::string& c) {
    for (int i = 0; i < 16; ++i) if (c == order[i]) return i;
    return 16;
  };
  std::stable_sort(entries.begin(), entries.end(), [&](const Entry& a, const Entry& b) {
    if (a.rank != b.rank) return a.rank < b.rank;
    if (a.category != b.category) return cat_rank(a.category) < cat_rank(b.category);
    return a.label < b.label;
  });
  json::Value out = json::Value::array();
  std::size_t n = 0;
  for (const Entry& e : entries) {
    if (++n > 120) break;
    json::Value x = json::Value::object();
    x.set("label", e.label);
    x.set("category", e.category);
    x.set("help", e.help);
    x.set("type", e.type);
    if (!e.params.empty()) {
      json::Value p = json::Value::object();
      for (const auto& [k, v] : e.params) p.set(k, v);
      x.set("params", std::move(p));
    }
    out.push(std::move(x));
  }
  json::Value r = ok_result();
  r.set("items", std::move(out));
  return r;
}

// ---------------------------------------------------------------------------
// commands

json::Value Studio::logic_command(const std::string& name, const json::Value& c) {
  auto num = [&](const char* k, f64 fallback = 0) { return c[k].is_number() ? c[k].as_number() : fallback; };
  auto str = [&](const char* k) -> std::string { return c[k].is_string() ? c[k].as_string() : std::string(); };
  if (name == "logicOpen") {
    const u32 owner = static_cast<u32>(num("owner", c["owner"].is_null() && !selection_.empty() ? primary() : 0));
    if (owner && !level_.find(owner)) return fail_result("no such actor");
    logic_owner_ = owner;
    logic_open_ = true;
    logic_view_ = "graph";
    touch();
    return ok_result();
  }
  if (name == "logicClose") {
    logic_open_ = false;
    touch();
    return ok_result();
  }
  if (name == "logicView") {
    logic_view_ = str("view").empty() ? "graph" : str("view");
    touch();
    return ok_result();
  }
  if (name == "logicPalette") return logic_palette(c);
  if (name == "logicTrace") {
    json::Value r = ok_result();
    r.set("trace", logic_trace_json());
    return r;
  }
  if (name == "logicCopy") {
    const logic::Graph* g = graph_of(level_, logic_owner_);
    if (!g) return fail_result("no graph");
    logic::Graph part;
    std::vector<u32> ids;
    for (const json::Value& v : c["ids"].items()) ids.push_back(static_cast<u32>(v.as_number()));
    for (const logic::Node& n : g->nodes) if (std::find(ids.begin(), ids.end(), n.id) != ids.end()) part.nodes.push_back(n);
    for (const logic::Link& l : g->links)
      if (std::find(ids.begin(), ids.end(), l.from) != ids.end() && std::find(ids.begin(), ids.end(), l.to) != ids.end()) part.links.push_back(l);
    json::Value r = ok_result();
    r.set("text", json::stringify(logic::graph_to_json(part)));
    return r;
  }
  if (name == "logicExportCpp") {
    // The graph as C++ source (compiled into a game it replaces the interpreter / JIT for this graph).
    const logic::Graph* g = graph_of(level_, logic_owner_);
    if (!g) return fail_result("this actor has no logic yet");
    struct Quiet : logic::Host {
      f64 time() const override { return 0; }
      void print(const std::string&, bool) override {}
      void error(const std::string&) override {}
      u32 random() override { return 4; }
      bool jit_compile(const std::vector<u8>&, i32) override { return false; }
    } host;
    logic::Runtime rt(host);
    rt.jit = false;
    const game::Actor* a = logic_owner_ ? level_.find(logic_owner_) : nullptr;
    const std::string name = (level_.name.empty() ? std::string("Level") : level_.name) + "_" + (a ? a->name : std::string("Level"));
    logic::Instance* inst = rt.add(*g, static_cast<logic::ActorRef>(logic_owner_), name);
    json::Value r = ok_result();
    r.set("text", logic::generate_cpp(*inst, name));
    std::string file;
    for (char ch : name) file += std::isalnum(static_cast<unsigned char>(ch)) ? static_cast<char>(std::tolower(static_cast<unsigned char>(ch))) : '_';
    r.set("file", "logic_" + file + ".cpp");
    return r;
  }
  if (name == "logicCompile") {
    json::Value r = ok_result();
    const logic::Graph* g = graph_of(level_, logic_owner_);
    json::Value iss = json::Value::array();
    if (g)
      for (const logic::Issue& i : logic::check(*g)) {
        json::Value x = json::Value::object();
        x.set("node", i.node);
        x.set("error", i.error);
        x.set("text", i.text);
        iss.push(std::move(x));
      }
    r.set("issues", std::move(iss));
    return r;
  }

  // ---- edits ----
  std::string error;
  json::Value result = ok_result();
  const u32 owner = logic_owner_;
  auto graph_edit = [&](const char* label, auto&& fn) {
    const bool done = edit(label, [&](game::Level& l) {
      logic::Graph* g = graph_of(l, owner, true);
      if (!g) { error = "the graph's actor is gone"; return false; }
      return fn(*g, l);
    });
    if (!done && !error.empty()) return fail_result(error);
    result.set("ok", done || error.empty());
    return result;
  };
  if (name == "logicAdd") {
    const std::string type = str("type");
    if (!logic::library().find(type)) return fail_result("unknown node type " + type);
    return graph_edit("Add Node", [&](logic::Graph& g, game::Level&) {
      logic::Node n;
      n.id = g.next_id++;
      n.type = type;
      n.x = static_cast<f32>(std::round(num("x") / 8) * 8);
      n.y = static_cast<f32>(std::round(num("y") / 8) * 8);
      for (const auto& [k, v] : c["params"].members()) n.set_param(k, v.is_string() ? v.as_string() : json::stringify(v));
      // New variables nodes on a graph without variables: offer one (Get / Set need a name).
      if ((type == "var.get" || type == "var.set" || type == "var.add") && n.param("variable").empty()) {
        if (g.variables.empty()) g.variables.push_back({"Count", logic::PinType::number, json::Value(0.0), {}});
        n.set_param("variable", g.variables.front().name);
      }
      if ((type.rfind("event.state", 0) == 0 || type.rfind("state.", 0) == 0) && n.param("machine").empty()) {
        if (g.machines.empty()) {
          logic::StateMachine m;
          m.id = g.next_id++;
          m.states.push_back({g.next_id++, "Idle", 0, 0, true});
          m.states.push_back({g.next_id++, "Active", 260, 0, false});
          g.machines.push_back(std::move(m));
        }
        n.set_param("machine", g.machines.front().name);
      }
      g.nodes.push_back(n);
      result.set("id", n.id);
      // Dropped from a dragged wire: connect to the first fitting pin.
      if (c["from"].is_object()) {
        const u32 from_node = static_cast<u32>(c["from"]["node"].as_number());
        const std::string from_pin = c["from"]["pin"].as_string();
        const bool output = c["from"]["output"].as_bool();
        logic::PinDef p;
        if (pin_of(g, from_node, from_pin, output, p)) {
          const std::string other = compatible_pin(g, g.nodes.back(), p.type, !output);
          std::string ignore;
          if (!other.empty()) {
            if (output) connect(g, from_node, from_pin, n.id, other, ignore);
            else connect(g, n.id, other, from_node, from_pin, ignore);
          }
        }
      }
      return true;
    });
  }
  if (name == "logicPaste") {
    json::Value j;
    logic::Graph part;
    if (!json::parse(str("text"), j) || !logic::graph_from_json(j, part, &error)) return fail_result(error.empty() ? "not logic nodes" : error);
    return graph_edit("Paste Nodes", [&](logic::Graph& g, game::Level&) {
      if (part.nodes.empty()) return false;
      f32 minx = 1e9f, miny = 1e9f;
      for (const logic::Node& n : part.nodes) { minx = min_of(minx, n.x); miny = min_of(miny, n.y); }
      std::unordered_map<u32, u32> remap;
      json::Value ids = json::Value::array();
      for (logic::Node n : part.nodes) {
        remap[n.id] = g.next_id;
        n.id = g.next_id++;
        n.x = n.x - minx + static_cast<f32>(num("x"));
        n.y = n.y - miny + static_cast<f32>(num("y"));
        ids.push(n.id);
        g.nodes.push_back(std::move(n));
      }
      for (const logic::Link& l : part.links)
        if (remap.count(l.from) && remap.count(l.to)) g.links.push_back({remap[l.from], l.out, remap[l.to], l.in});
      result.set("ids", std::move(ids));
      return true;
    });
  }
  if (name == "logicMove") {
    coalesce_ = "logic-move:" + std::to_string(owner);
    for (const json::Value& m : c["moves"].items()) coalesce_ += "," + std::to_string(static_cast<u32>(m["id"].as_number()));
    transaction_ = str("transaction");
    return graph_edit("Move Nodes", [&](logic::Graph& g, game::Level&) {
      for (const json::Value& m : c["moves"].items()) {
        const u32 id = static_cast<u32>(m["id"].as_number());
        const f32 x = static_cast<f32>(std::round(m["x"].as_number() / 8) * 8), y = static_cast<f32>(std::round(m["y"].as_number() / 8) * 8);
        if (logic::Node* n = g.find(id)) { n->x = x; n->y = y; continue; }
        for (logic::StateMachine& sm : g.machines)
          for (logic::State& s : sm.states)
            if (s.id == id) { s.x = x; s.y = y; }
      }
      return true;
    });
  }
  if (name == "logicConnect") {
    return graph_edit("Connect", [&](logic::Graph& g, game::Level&) {
      return connect(g, static_cast<u32>(num("from")), str("out"), static_cast<u32>(num("to")), str("in"), error);
    });
  }
  if (name == "logicDisconnect") {
    return graph_edit("Break Link", [&](logic::Graph& g, game::Level&) {
      const u32 node = static_cast<u32>(num("node"));
      const std::string pin = str("pin");
      const bool output = c["output"].as_bool();
      const std::size_t before = g.links.size();
      g.links.erase(std::remove_if(g.links.begin(), g.links.end(), [&](const logic::Link& l) {
                      if (c["link"].is_array()) {
                        const json::Value& k = c["link"];
                        return l.from == static_cast<u32>(k[0].as_number()) && l.out == k[1].as_string() && l.to == static_cast<u32>(k[2].as_number()) && l.in == k[3].as_string();
                      }
                      return output ? (l.from == node && l.out == pin) : (l.to == node && l.in == pin);
                    }),
                    g.links.end());
      return g.links.size() != before;
    });
  }
  if (name == "logicDelete") {
    return graph_edit("Delete Nodes", [&](logic::Graph& g, game::Level&) {
      std::vector<u32> ids;
      for (const json::Value& v : c["ids"].items()) ids.push_back(static_cast<u32>(v.as_number()));
      auto gone = [&](u32 id) { return std::find(ids.begin(), ids.end(), id) != ids.end(); };
      const std::size_t before = g.nodes.size();
      g.nodes.erase(std::remove_if(g.nodes.begin(), g.nodes.end(), [&](const logic::Node& n) { return gone(n.id); }), g.nodes.end());
      g.links.erase(std::remove_if(g.links.begin(), g.links.end(), [&](const logic::Link& l) { return gone(l.from) || gone(l.to); }), g.links.end());
      return g.nodes.size() != before;
    });
  }
  if (name == "logicValue") {
    coalesce_ = "logic-value:" + std::to_string(owner) + ":" + std::to_string(static_cast<u32>(num("node"))) + ":" + str("pin");
    return graph_edit("Edit Value", [&](logic::Graph& g, game::Level&) {
      logic::Node* n = g.find(static_cast<u32>(num("node")));
      logic::PinDef p;
      if (!n || !pin_of(g, n->id, str("pin"), false, p)) { error = "no such input"; return false; }
      n->set_value(p.id, logic::Value::from_json(c["value"], p.type).to_json());
      return true;
    });
  }
  if (name == "logicParam") {
    return graph_edit("Edit Node", [&](logic::Graph& g, game::Level&) {
      logic::Node* n = g.find(static_cast<u32>(num("node")));
      if (!n) { error = "no such node"; return false; }
      const json::Value& v = c["value"];
      n->set_param(str("key"), v.is_string() ? v.as_string() : v.is_number() ? std::to_string(static_cast<i64>(v.as_number())) : json::stringify(v));
      // A changed variable / count can remove pins: drop wires to pins that no longer exist.
      std::vector<logic::PinDef> in, out;
      logic::library().pins(*n, g, in, out);
      auto has = [](const std::vector<logic::PinDef>& v, const std::string& id) { for (const auto& p : v) if (p.id == id) return true; return false; };
      g.links.erase(std::remove_if(g.links.begin(), g.links.end(), [&](const logic::Link& l) {
                      return (l.from == n->id && !has(out, l.out)) || (l.to == n->id && !has(in, l.in));
                    }),
                    g.links.end());
      return true;
    });
  }
  if (name == "logicComment") {
    return graph_edit("Comment", [&](logic::Graph& g, game::Level&) {
      logic::Node* n = g.find(static_cast<u32>(num("node")));
      if (!n) return false;
      n->comment = str("text");
      return true;
    });
  }
  if (name == "logicVariable") {
    const std::string op = str("op");
    return graph_edit("Edit Variable", [&](logic::Graph& g, game::Level&) {
      const std::string var = str("name");
      if (op == "add") {
        std::string base = var.empty() ? "NewVar" : var, n = base;
        for (int i = 2; g.variable(n); ++i) n = base + std::to_string(i);
        logic::PinType t = logic::PinType::number;
        logic::pin_type_from_name(str("type").empty() ? "float" : str("type"), t);
        if (t == logic::PinType::exec) t = logic::PinType::number;
        g.variables.push_back({n, t, logic::Value::zero(t).to_json(), {}});
        result.set("name", n);
        return true;
      }
      logic::Variable* v = g.variable(var);
      if (!v) { error = "no variable " + var; return false; }
      if (op == "remove") {
        g.variables.erase(std::remove_if(g.variables.begin(), g.variables.end(), [&](const logic::Variable& x) { return x.name == var; }), g.variables.end());
        return true;
      }
      if (op == "rename") {
        const std::string to = str("to");
        if (to.empty() || to.size() > 64 || g.variable(to)) { error = "that name is empty or taken"; return false; }
        for (logic::Node& n : g.nodes)
          if (n.param("variable") == var) n.set_param("variable", to);
        for (logic::StateMachine& m : g.machines)
          for (logic::Transition& t : m.transitions)
            if (t.variable == var) t.variable = to;
        v->name = to;
        return true;
      }
      if (op == "type") {
        logic::PinType t;
        if (!logic::pin_type_from_name(str("type"), t) || t == logic::PinType::exec) { error = "unknown type"; return false; }
        v->value = logic::Value::from_json(v->value, v->type).converted(t).to_json();
        v->type = t;
        return true;
      }
      if (op == "value") {
        v->value = logic::Value::from_json(c["value"], v->type).to_json();
        return true;
      }
      error = "unknown variable operation";
      return false;
    });
  }
  if (name == "logicMachine") {
    const std::string op = str("op");
    return graph_edit("Edit State Machine", [&](logic::Graph& g, game::Level&) {
      if (op == "add") {
        logic::StateMachine m;
        m.id = g.next_id++;
        std::string base = str("name").empty() ? "State Machine" : str("name"), n = base;
        for (int i = 2; g.machine(n); ++i) n = base + " " + std::to_string(i);
        m.name = n;
        m.states.push_back({g.next_id++, "Idle", 0, 0, true});
        g.machines.push_back(std::move(m));
        result.set("id", g.machines.back().id);
        return true;
      }
      logic::StateMachine* m = g.machine(static_cast<u32>(num("machine")));
      if (!m) { error = "no such state machine"; return false; }
      if (op == "remove") {
        const u32 id = m->id;
        g.machines.erase(std::remove_if(g.machines.begin(), g.machines.end(), [&](const logic::StateMachine& x) { return x.id == id; }), g.machines.end());
        return true;
      }
      if (op == "rename") {
        const std::string to = str("name");
        if (to.empty() || g.machine(to)) { error = "that name is empty or taken"; return false; }
        for (logic::Node& n : g.nodes)
          if (n.param("machine") == m->name) n.set_param("machine", to);
        m->name = to;
        return true;
      }
      if (op == "addState") {
        std::string base = str("name").empty() ? "State" : str("name"), n = base;
        for (int i = 2; m->find(n); ++i) n = base + " " + std::to_string(i);
        logic::State s{g.next_id++, n, static_cast<f32>(num("x")), static_cast<f32>(num("y")), m->states.empty()};
        m->states.push_back(s);
        result.set("id", s.id);
        if (c["from"].is_number()) {  // dragged out of a state: connect with a transition
          logic::Transition t;
          t.id = g.next_id++;
          t.from = static_cast<u32>(num("from"));
          t.to = s.id;
          t.trigger = logic::TriggerKind::timer;
          t.seconds = 1;
          m->transitions.push_back(t);
        }
        return true;
      }
      const u32 sid = static_cast<u32>(num("state"));
      if (op == "renameState") {
        const std::string to = str("name");
        logic::State* s = nullptr;
        for (logic::State& x : m->states) if (x.id == sid) s = &x;
        if (!s || to.empty() || m->find(to)) { error = "that name is empty or taken"; return false; }
        for (logic::Node& n : g.nodes)
          if (n.param("machine") == m->name && n.param("state") == s->name) n.set_param("state", to);
        s->name = to;
        return true;
      }
      if (op == "initial") {
        for (logic::State& x : m->states) x.initial = x.id == sid;
        return true;
      }
      if (op == "removeState") {
        m->states.erase(std::remove_if(m->states.begin(), m->states.end(), [&](const logic::State& x) { return x.id == sid; }), m->states.end());
        m->transitions.erase(std::remove_if(m->transitions.begin(), m->transitions.end(), [&](const logic::Transition& t) { return t.from == sid || t.to == sid; }),
                             m->transitions.end());
        if (!m->states.empty() && std::none_of(m->states.begin(), m->states.end(), [](const logic::State& x) { return x.initial; })) m->states.front().initial = true;
        return true;
      }
      if (op == "addTransition" || op == "editTransition") {
        logic::Transition* t = nullptr;
        if (op == "addTransition") {
          const u32 from = static_cast<u32>(num("from")), to = static_cast<u32>(num("to"));
          if (!m->find(to) || (from && !m->find(from)) || from == to) { error = "pick two different states"; return false; }
          logic::Transition nt;
          nt.id = g.next_id++;
          nt.from = from;
          nt.to = to;
          nt.trigger = logic::TriggerKind::event;
          nt.event = "Go";
          m->transitions.push_back(nt);
          t = &m->transitions.back();
          result.set("id", t->id);
        } else {
          for (logic::Transition& x : m->transitions) if (x.id == static_cast<u32>(num("transition"))) t = &x;
          if (!t) { error = "no such transition"; return false; }
        }
        const std::string trig = str("trigger");
        if (!trig.empty())
          t->trigger = trig == "timer" ? logic::TriggerKind::timer : trig == "condition" ? logic::TriggerKind::condition : trig == "auto" ? logic::TriggerKind::automatic : logic::TriggerKind::event;
        if (c["event"].is_string()) t->event = str("event");
        if (c["seconds"].is_number()) t->seconds = static_cast<f32>(clamp(num("seconds"), 0.0, 1e6));
        if (c["variable"].is_string()) t->variable = str("variable");
        if (c["label"].is_string()) t->label = str("label");
        if (t->trigger == logic::TriggerKind::condition && t->variable.empty()) {
          for (const logic::Variable& v : g.variables) if (v.type == logic::PinType::boolean) { t->variable = v.name; break; }
          if (t->variable.empty()) {
            std::string n = "Ready";
            for (int i = 2; g.variable(n); ++i) n = "Ready" + std::to_string(i);
            g.variables.push_back({n, logic::PinType::boolean, json::Value(false), {}});
            t->variable = n;
          }
        }
        return true;
      }
      if (op == "removeTransition") {
        const u32 tid = static_cast<u32>(num("transition"));
        m->transitions.erase(std::remove_if(m->transitions.begin(), m->transitions.end(), [&](const logic::Transition& t) { return t.id == tid; }), m->transitions.end());
        return true;
      }
      error = "unknown state machine operation";
      return false;
    });
  }
  if (name == "logicRemoveGraph") {
    if (!owner) return graph_edit("Clear Level Logic", [&](logic::Graph& g, game::Level&) { g = logic::Graph{}; return true; });
    const bool done = edit("Remove Logic", [&](game::Level& l) {
      game::Actor* a = l.find(owner);
      if (!a || !a->logic) return false;
      a->logic.reset();
      return true;
    });
    logic_owner_ = 0;
    return ok_result(done);
  }
  if (name == "logicTemplate") {
    // Ready-made graphs (the quickest way to see how it works).
    const std::string which = str("template");
    std::string text;
    if (which == "door")
      text = R"({"nodes":[{"id":1,"type":"game.interact","x":0,"y":0},{"id":2,"type":"flow.flip_flop","x":260,"y":0},
        {"id":3,"type":"flow.tween","x":480,"y":-80,"params":{"ease":"smooth"},"values":{"duration":0.8}},
        {"id":4,"type":"flow.tween","x":480,"y":120,"params":{"ease":"smooth"},"values":{"duration":0.8}},
        {"id":5,"type":"actor.move_by","x":760,"y":-80,"values":{"offset":[0,0,0.05]}},{"id":6,"type":"actor.move_by","x":760,"y":120,"values":{"offset":[0,0,-0.05]}},
        {"id":7,"type":"game.sound","x":480,"y":300,"params":{"sound":"builtin:confirm"}}],
        "links":[[1,"then",2,"exec"],[2,"a",3,"play"],[2,"b",4,"play"],[3,"update",5,"exec"],[4,"update",6,"exec"],[1,"then",7,"exec"]]})";
    else if (which == "score_zone")
      text = R"({"nodes":[{"id":1,"type":"game.overlap_begin","x":0,"y":0},{"id":2,"type":"game.add_score","x":300,"y":0,"values":{"points":10}},
        {"id":3,"type":"game.message","x":560,"y":0,"values":{"text":"+10"}},{"id":4,"type":"game.sound","x":820,"y":0,"params":{"sound":"builtin:pickup"}}],
        "links":[[1,"then",2,"exec"],[2,"then",3,"exec"],[3,"then",4,"exec"]]})";
    else if (which == "spawner")
      text = R"({"variables":[{"name":"Spawned","type":"int","value":0}],"nodes":[{"id":1,"type":"event.every","x":0,"y":0,"params":{"seconds":"3"}},
        {"id":2,"type":"flow.branch","x":260,"y":0},{"id":3,"type":"math.compare","x":20,"y":160,"params":{"op":"<"},"values":{"b":10}},
        {"id":4,"type":"var.get","x":-220,"y":160,"params":{"variable":"Spawned"}},{"id":5,"type":"actor.spawn","x":480,"y":0,"params":{"template":"Physics Cube"},"values":{"location":[0,0,6]}},
        {"id":6,"type":"var.add","x":760,"y":0,"params":{"variable":"Spawned"}}],
        "links":[[1,"then",2,"exec"],[4,"value",3,"a"],[3,"result",2,"condition"],[2,"true",5,"exec"],[5,"then",6,"exec"]]})";
    else if (which == "traffic_light")
      text = R"({"machines":[{"id":20,"name":"Light","states":[{"id":21,"name":"Green","x":0,"y":0,"initial":true},{"id":22,"name":"Yellow","x":280,"y":0},{"id":23,"name":"Red","x":140,"y":200}],
        "transitions":[{"id":30,"from":21,"to":22,"trigger":"timer","seconds":4},{"id":31,"from":22,"to":23,"trigger":"timer","seconds":1.5},{"id":32,"from":23,"to":21,"trigger":"timer","seconds":4}]}],
        "nodes":[{"id":1,"type":"event.state_enter","x":0,"y":0,"params":{"machine":"Light","state":"any"}},{"id":2,"type":"state.get","x":0,"y":160,"params":{"machine":"Light"}},
        {"id":3,"type":"game.message","x":300,"y":0}],"links":[[1,"then",3,"exec"],[2,"state",3,"text"]]})";
    else return fail_result("unknown template");
    json::Value j;
    logic::Graph part;
    if (!json::parse(text, j) || !logic::graph_from_json(j, part, &error)) return fail_result(error);
    return graph_edit("Add Logic Template", [&](logic::Graph& g, game::Level&) {
      // Below what is there.
      f32 y0 = 0;
      for (const logic::Node& n : g.nodes) y0 = max_of(y0, n.y + 260);
      std::unordered_map<u32, u32> remap;
      for (const logic::Variable& v : part.variables) if (!g.variable(v.name)) g.variables.push_back(v);
      for (logic::StateMachine m : part.machines) {
        if (g.machine(m.name)) continue;
        std::unordered_map<u32, u32> sids;
        m.id = g.next_id++;
        for (logic::State& s : m.states) { sids[s.id] = g.next_id; s.id = g.next_id++; }
        for (logic::Transition& t : m.transitions) { t.id = g.next_id++; t.from = t.from ? sids[t.from] : 0; t.to = sids[t.to]; }
        g.machines.push_back(std::move(m));
      }
      for (logic::Node n : part.nodes) {
        remap[n.id] = g.next_id;
        n.id = g.next_id++;
        n.y += y0;
        g.nodes.push_back(std::move(n));
      }
      for (const logic::Link& l : part.links) g.links.push_back({remap[l.from], l.out, remap[l.to], l.in});
      return true;
    });
  }
  return fail_result("unknown logic command " + name);
}

}  // namespace kern::studio
