// Kern Logic: the built-in nodes (events, flow control, variables, state
// machines, math, logic, vectors, text, debugging). Gameplay nodes live in
// the game module (src/game/logic_nodes.cpp).
#include <cmath>
#include "kern/core/dmath.h"

#include "kern/logic/logic.h"

namespace kern::logic {

namespace {

constexpr f64 kDeg = 3.14159265358979323846 / 180.0;

PinDef exec_in(std::string id = "exec", std::string label = "") { return {std::move(id), std::move(label), PinType::exec, {}}; }
PinDef exec_out(std::string id = "then", std::string label = "") { return {std::move(id), std::move(label), PinType::exec, {}}; }
PinDef data(std::string id, std::string label, PinType t, json::Value def = {}) { return {std::move(id), std::move(label), t, std::move(def)}; }

NodeDef make(std::string type, std::string label, std::string category, NodeKind kind, std::string help) {
  NodeDef d;
  d.type = std::move(type);
  d.label = std::move(label);
  d.category = std::move(category);
  d.kind = kind;
  d.help = std::move(help);
  return d;
}

/// Pure node computing one output from its inputs.
NodeDef pure(std::string type, std::string label, std::string category, std::vector<PinDef> in, PinDef out, std::function<Value(Context&)> fn, std::string help = {},
             std::string keywords = {}) {
  NodeDef d = make(std::move(type), std::move(label), std::move(category), NodeKind::pure, std::move(help));
  d.inputs = std::move(in);
  d.outputs = {std::move(out)};
  d.keywords = std::move(keywords);
  d.run = [fn = std::move(fn)](Context& c) {
    c.out(std::size_t{0}, fn(c));
    return kStop;
  };
  return d;
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

f64 random01(Context& c) { return static_cast<f64>(c.host().random() >> 8) / static_cast<f64>(1u << 24); }

int clamp_count(const Node& n, std::string_view key, int def, int lo, int hi) {
  const std::string s = n.param(key);
  const int v = s.empty() ? def : std::atoi(s.c_str());
  return clamp(v, lo, hi);
}

std::vector<std::string> split_cases(const std::string& s) {
  std::vector<std::string> out;
  std::string cur;
  for (char ch : s + ",") {
    if (ch == ',') {
      while (!cur.empty() && cur.front() == ' ') cur.erase(cur.begin());
      while (!cur.empty() && cur.back() == ' ') cur.pop_back();
      if (!cur.empty()) out.push_back(cur);
      cur.clear();
    } else cur += ch;
  }
  return out;
}

void add_events(Library& lib) {
  {
    NodeDef d = make("event.begin", "Begin Play", "Events", NodeKind::event, "Runs once when play starts (or when the actor is spawned).");
    d.outputs = {exec_out()};
    d.event = "logic.begin";
    d.color = 0xb4232f;
    lib.add(std::move(d));
  }
  {
    NodeDef d = make("event.tick", "Event Tick", "Events", NodeKind::event, "Runs every simulation step. Delta is the step in seconds.");
    d.outputs = {exec_out(), data("delta", "Delta Seconds", PinType::number)};
    d.event = "logic.tick";
    d.color = 0xb4232f;
    lib.add(std::move(d));
  }
  {
    NodeDef d = make("event.every", "Every N Seconds", "Events", NodeKind::event, "Runs repeatedly with a fixed interval (a looping timer).");
    d.params = {param("seconds", "Interval (s)", ParamDef::Kind::text, "1")};
    d.outputs = {exec_out(), data("count", "Count", PinType::integer)};
    d.color = 0xb4232f;
    d.title = [](const NodeDef&, const Node& n) { return "Every " + n.param("seconds", "1") + " s"; };
    // Delivered with the tick: counts its own time.
    d.event = "logic.tick";
    d.accepts = [](const Node&, const Event&) { return true; };
    d.run = [](Context& c) {
      NodeState& s = c.state();
      const f64 period = max_of(0.01, std::atof(c.param("seconds", "1").c_str()));
      if (s.time == 0) s.time = c.host().time() + period;
      if (c.host().time() + 1e-9 < s.time) return kStop;
      s.time += period;
      if (s.time < c.host().time()) s.time = c.host().time() + period;
      c.out("count", Value::integer(++s.counter));
      return 0;
    };
    lib.add(std::move(d));
  }
  {
    NodeDef d = make("event.custom", "Custom Event", "Events", NodeKind::event,
                     "An event you name and raise yourself with Call Event (on this graph, another actor's or all). Also switches state machine transitions with that name.");
    d.params = {param("name", "Name", ParamDef::Kind::event, "MyEvent")};
    d.outputs = {exec_out(), data("actor", "Actor", PinType::actor), data("value", "Value", PinType::number), data("text", "Text", PinType::text)};
    d.color = 0xb4232f;
    d.title = [](const NodeDef&, const Node& n) { return "Event " + n.param("name", "MyEvent"); };
    d.event = "logic.custom";
    lib.add(std::move(d));
  }
  {
    NodeDef d = make("event.state_enter", "On State Entered", "State Machines", NodeKind::event, "Runs when a state machine enters the state.");
    d.params = {param("machine", "Machine", ParamDef::Kind::machine, "State Machine"), param("state", "State", ParamDef::Kind::state, "any")};
    d.outputs = {exec_out(), data("previous", "Previous State", PinType::text)};
    d.color = 0x0e7490;
    d.title = [](const NodeDef&, const Node& n) { return "Entered " + n.param("state", "any"); };
    d.accepts = [](const Node& n, const Event& e) {
      const std::string m = n.param("machine", "State Machine"), s = n.param("state", "any");
      return e.key.rfind(m + "/", 0) == 0 && (s.empty() || s == "any" || e.key == m + "/" + s);
    };
    d.event = "logic.state_enter";
    lib.add(std::move(d));
  }
  {
    NodeDef d = make("event.state_exit", "On State Left", "State Machines", NodeKind::event, "Runs when a state machine leaves the state.");
    d.params = {param("machine", "Machine", ParamDef::Kind::machine, "State Machine"), param("state", "State", ParamDef::Kind::state, "any")};
    d.outputs = {exec_out()};
    d.color = 0x0e7490;
    d.title = [](const NodeDef&, const Node& n) { return "Left " + n.param("state", "any"); };
    d.accepts = [](const Node& n, const Event& e) {
      const std::string m = n.param("machine", "State Machine"), s = n.param("state", "any");
      return e.key.rfind(m + "/", 0) == 0 && (s.empty() || s == "any" || e.key == m + "/" + s);
    };
    d.event = "logic.state_exit";
    lib.add(std::move(d));
  }
}

void add_flow(Library& lib) {
  {
    // Reroute nodes (Blueprint knots): a wire bends through them; they change nothing.
    NodeDef d = make("flow.reroute", "Reroute (exec)", "Flow", NodeKind::flow, "A knot an exec wire passes through, to route wires tidily.");
    d.inputs = {exec_in()};
    d.outputs = {exec_out()};
    d.keywords = "knot reroute wire";
    d.run = [](Context&) { return 0; };
    lib.add(std::move(d));
  }
  {
    NodeDef d = make("value.reroute", "Reroute", "Flow", NodeKind::pure, "A knot a data wire passes through, to route wires tidily.");
    d.params = {param("type", "Type", ParamDef::Kind::choice, "float",
                      {"bool", "Boolean", "int", "Integer", "float", "Float", "vector", "Vector", "text", "Text", "actor", "Actor"})};
    d.keywords = "knot reroute wire";
    d.shape = [](const NodeDef&, const Node& n, const Graph&, std::vector<PinDef>& in, std::vector<PinDef>& out) {
      PinType t = PinType::number;
      if (!pin_type_from_name(n.param("type", "float"), t) || t == PinType::exec) t = PinType::number;
      in = {data("in", "", t, Value::zero(t).to_json())};
      out = {data("out", "", t)};
    };
    d.run = [](Context& c) {
      c.out(std::size_t{0}, c.in(std::size_t{0}));
      return kStop;
    };
    lib.add(std::move(d));
  }
  {
    NodeDef d = make("flow.branch", "Branch", "Flow", NodeKind::flow, "If the condition is true, continue at True, otherwise at False.");
    d.inputs = {exec_in(), data("condition", "Condition", PinType::boolean, true)};
    d.outputs = {exec_out("true", "True"), exec_out("false", "False")};
    d.keywords = "if else condition";
    d.run = [](Context& c) { return c.in_bool("condition") ? 0 : 1; };
    lib.add(std::move(d));
  }
  {
    NodeDef d = make("flow.sequence", "Sequence", "Flow", NodeKind::flow, "Runs its outputs one after the other.");
    d.inputs = {exec_in()};
    d.params = {param("count", "Outputs", ParamDef::Kind::integer, "2")};
    d.shape = [](const NodeDef&, const Node& n, const Graph&, std::vector<PinDef>&, std::vector<PinDef>& out) {
      out.clear();
      const int k = clamp_count(n, "count", 2, 1, 16);
      for (int i = 0; i < k; ++i) out.push_back(exec_out("then" + std::to_string(i), "Then " + std::to_string(i)));
    };
    d.run = [](Context& c) {
      const int k = clamp_count(c.node(), "count", 2, 1, 16);
      for (int i = 1; i < k; ++i) c.then(i);
      return 0;
    };
    lib.add(std::move(d));
  }
  {
    NodeDef d = make("flow.for", "For Loop", "Flow", NodeKind::flow, "Runs Loop Body for every index from First to Last (inclusive), then Completed.");
    d.inputs = {exec_in(), data("first", "First Index", PinType::integer, 0), data("last", "Last Index", PinType::integer, 9)};
    d.outputs = {exec_out("body", "Loop Body"), data("index", "Index", PinType::integer), exec_out("completed", "Completed")};
    d.keywords = "repeat for each count";
    // State in slots (index, last): the same in the interpreter and in generated code.
    d.run = [](Context& c) {
      f64& index = c.slot(0);
      f64& last = c.slot(1);
      if (c.entry() != Context::kResume) {
        index = static_cast<f64>(c.in_int("first"));
        last = static_cast<f64>(c.in_int("last"));
      } else {
        index += 1;
      }
      if (index > last) return c.exec("completed");
      c.out("index", Value::integer(static_cast<i64>(index)));
      c.loop_back(Context::kResume);
      return c.exec("body");
    };
    lib.add(std::move(d));
  }
  {
    NodeDef d = make("flow.while", "While Loop", "Flow", NodeKind::flow, "Runs Loop Body while the condition is true (checked before every pass).");
    d.inputs = {exec_in(), data("condition", "Condition", PinType::boolean, false)};
    d.outputs = {exec_out("body", "Loop Body"), exec_out("completed", "Completed")};
    d.run = [](Context& c) {
      if (!c.in_bool("condition")) return c.exec("completed");
      c.loop_back(Context::kResume);
      return c.exec("body");
    };
    lib.add(std::move(d));
  }
  {
    NodeDef d = make("flow.do_once", "Do Once", "Flow", NodeKind::flow, "Lets the first call through; later calls are ignored until Reset.");
    d.inputs = {exec_in(), exec_in("reset", "Reset"), data("closed", "Start Closed", PinType::boolean, false)};
    d.outputs = {exec_out("completed", "Completed")};
    d.run = [](Context& c) {
      f64& closed = c.slot(0);
      f64& started = c.slot(1);
      if (c.entry() == 1) { closed = 0; started = 1; return kStop; }
      if (started == 0) { started = 1; closed = c.in_bool("closed") ? 1 : 0; }
      if (closed != 0) return kStop;
      closed = 1;
      return 0;
    };
    lib.add(std::move(d));
  }
  {
    NodeDef d = make("flow.do_n", "Do N", "Flow", NodeKind::flow, "Lets N calls through, then blocks until Reset.");
    d.inputs = {exec_in(), data("n", "N", PinType::integer, 3), exec_in("reset", "Reset")};
    d.outputs = {exec_out("exit", "Exit"), data("counter", "Counter", PinType::integer)};
    d.run = [](Context& c) {
      f64& counter = c.slot(0);
      if (c.entry() == 1) { counter = 0; return kStop; }
      if (counter >= static_cast<f64>(c.in_int("n"))) return kStop;
      counter += 1;
      c.out("counter", Value::integer(static_cast<i64>(counter)));
      return 0;
    };
    lib.add(std::move(d));
  }
  {
    NodeDef d = make("flow.gate", "Gate", "Flow", NodeKind::flow, "Enter passes through while the gate is open. Open, Close and Toggle change it.");
    d.inputs = {exec_in("enter", "Enter"), exec_in("open", "Open"), exec_in("close", "Close"), exec_in("toggle", "Toggle"), data("closed", "Start Closed", PinType::boolean, false)};
    d.outputs = {exec_out("exit", "Exit")};
    d.run = [](Context& c) {
      f64& open = c.slot(0);
      f64& started = c.slot(1);
      if (started == 0) { started = 1; open = c.in_bool("closed") ? 0 : 1; }
      switch (c.entry()) {
        case 0: return open != 0 ? 0 : kStop;
        case 1: open = 1; return kStop;
        case 2: open = 0; return kStop;
        case 3: open = open != 0 ? 0 : 1; return kStop;
        default: return kStop;
      }
    };
    lib.add(std::move(d));
  }
  {
    NodeDef d = make("flow.flip_flop", "Flip Flop", "Flow", NodeKind::flow, "Alternates between A and B on every call.");
    d.inputs = {exec_in()};
    d.outputs = {exec_out("a", "A"), exec_out("b", "B"), data("is_a", "Is A", PinType::boolean)};
    d.run = [](Context& c) {
      f64& a = c.slot(0);
      a = a != 0 ? 0 : 1;
      c.out("is_a", Value::boolean(a != 0));
      return a != 0 ? 0 : 1;
    };
    lib.add(std::move(d));
  }
  {
    NodeDef d = make("flow.delay", "Delay", "Flow", NodeKind::flow, "Waits, then continues. Calls while waiting are ignored.");
    d.latent = true;
    d.inputs = {exec_in(), data("duration", "Duration", PinType::number, 1)};
    d.outputs = {exec_out("completed", "Completed")};
    d.keywords = "wait timer sleep";
    d.run = [](Context& c) {
      NodeState& s = c.state();
      if (c.entry() == Context::kResume) { s.flag = false; return 0; }
      if (s.flag) return kStop;
      s.flag = true;
      return c.wait(c.in_number("duration"));
    };
    lib.add(std::move(d));
  }
  {
    NodeDef d = make("flow.retrigger_delay", "Retriggerable Delay", "Flow", NodeKind::flow, "Waits; every new call restarts the wait.");
    d.latent = true;
    d.inputs = {exec_in(), data("duration", "Duration", PinType::number, 1)};
    d.outputs = {exec_out("completed", "Completed")};
    d.run = [](Context& c) {
      if (c.entry() == Context::kResume) return 0;
      c.cancel_waits();
      return c.wait(c.in_number("duration"));
    };
    lib.add(std::move(d));
  }
  {
    NodeDef d = make("flow.tween", "Tween", "Flow", NodeKind::flow,
                     "Runs Update every step for Duration seconds with Alpha going 0 to 1 (eased), then Finished. Use it to move, fade or scale over time (a timeline).");
    d.latent = true;
    d.inputs = {exec_in("play", "Play"), exec_in("stop", "Stop"), data("duration", "Duration", PinType::number, 1)};
    d.params = {param("ease", "Easing", ParamDef::Kind::choice, "smooth", {"linear", "Linear", "smooth", "Smooth", "in", "Ease in", "out", "Ease out"})};
    d.outputs = {exec_out("update", "Update"), exec_out("finished", "Finished"), data("alpha", "Alpha", PinType::number)};
    d.keywords = "timeline lerp interpolate animate over time";
    d.run = [](Context& c) {
      NodeState& s = c.state();
      if (c.entry() == 1) { c.cancel_waits(); s.flag = false; return kStop; }
      const f64 now = c.host().time();
      if (c.entry() == 0) {
        c.cancel_waits();
        s.time = now;
        s.a = max_of(0.0, c.in_number("duration"));
        s.flag = true;
      }
      if (!s.flag) return kStop;
      f64 t = s.a > 0 ? (now - s.time) / s.a : 1.0;
      t = clamp(t, 0.0, 1.0);
      const std::string ease = c.param("ease", "smooth");
      f64 e = t;
      if (ease == "smooth") e = t * t * (3 - 2 * t);
      else if (ease == "in") e = t * t;
      else if (ease == "out") e = 1 - (1 - t) * (1 - t);
      c.out("alpha", Value::number(e));
      if (t >= 1) {
        s.flag = false;
        c.then(c.exec("finished"));
        return c.exec("update");
      }
      c.wait(0, Context::kResume);  // again next step
      return c.exec("update");
    };
    lib.add(std::move(d));
  }
  {
    NodeDef d = make("flow.switch_int", "Switch on Int", "Flow", NodeKind::flow, "Continues at the output matching Selection, or Default.");
    d.inputs = {exec_in(), data("selection", "Selection", PinType::integer, 0)};
    d.params = {param("count", "Cases", ParamDef::Kind::integer, "3")};
    d.shape = [](const NodeDef&, const Node& n, const Graph&, std::vector<PinDef>&, std::vector<PinDef>& out) {
      out.clear();
      const int k = clamp_count(n, "count", 3, 1, 32);
      for (int i = 0; i < k; ++i) out.push_back(exec_out("case" + std::to_string(i), std::to_string(i)));
      out.push_back(exec_out("default", "Default"));
    };
    d.run = [](Context& c) {
      const int k = clamp_count(c.node(), "count", 3, 1, 32);
      const i64 v = c.in_int("selection");
      return v >= 0 && v < k ? static_cast<int>(v) : k;
    };
    lib.add(std::move(d));
  }
  {
    NodeDef d = make("flow.switch_text", "Switch on Text", "Flow", NodeKind::flow, "Continues at the output whose text equals Selection (cases separated by commas), or Default.");
    d.inputs = {exec_in(), data("selection", "Selection", PinType::text, "")};
    d.params = {param("cases", "Cases", ParamDef::Kind::text, "A, B")};
    d.shape = [](const NodeDef&, const Node& n, const Graph&, std::vector<PinDef>&, std::vector<PinDef>& out) {
      out.clear();
      int i = 0;
      for (const std::string& s : split_cases(n.param("cases", "A, B"))) out.push_back(exec_out("case" + std::to_string(i++), s));
      out.push_back(exec_out("default", "Default"));
    };
    d.run = [](Context& c) {
      const auto cases = split_cases(c.param("cases", "A, B"));
      const std::string v = c.in_text("selection");
      for (std::size_t i = 0; i < cases.size(); ++i) if (cases[i] == v) return static_cast<int>(i);
      return static_cast<int>(cases.size());
    };
    lib.add(std::move(d));
  }
  {
    NodeDef d = make("flow.call", "Call Event", "Flow", NodeKind::action,
                     "Raises a Custom Event: on Target's graph, or on every graph when Target is empty. Also switches state machine transitions with that name.");
    d.inputs = {exec_in(), data("target", "Target", PinType::actor, 0), data("actor", "Actor", PinType::actor, 0), data("value", "Value", PinType::number, 0),
                data("text", "Text", PinType::text, "")};
    d.params = {param("name", "Event", ParamDef::Kind::event, "MyEvent")};
    d.outputs = {exec_out()};
    d.keywords = "broadcast dispatch message send trigger";
    d.title = [](const NodeDef&, const Node& n) { return "Call " + n.param("name", "MyEvent"); };
    d.run = [](Context& c) {
      std::vector<std::pair<std::string, Value>> args{{"actor", c.in("actor")}, {"value", c.in("value")}, {"text", c.in("text")}};
      c.runtime().call(c.param("name", "MyEvent"), c.in_actor("target"), std::move(args));
      return 0;
    };
    lib.add(std::move(d));
  }
}

void add_variables(Library& lib) {
  auto var_shape = [](bool input, bool output) {
    return [input, output](const NodeDef&, const Node& n, const Graph& g, std::vector<PinDef>& in, std::vector<PinDef>& out) {
      const Variable* v = g.variable(n.param("variable"));
      const PinType t = v ? v->type : PinType::number;
      for (PinDef& p : in) if (p.id == "value" && input) { p.type = t; p.def = v ? v->value : json::Value(0.0); }
      for (PinDef& p : out) if (p.id == "value" && output) p.type = t;
    };
  };
  {
    NodeDef d = make("var.get", "Get Variable", "Variables", NodeKind::pure, "Reads a variable of this graph.");
    d.params = {param("variable", "Variable", ParamDef::Kind::variable)};
    d.outputs = {data("value", "Value", PinType::number)};
    d.shape = var_shape(false, true);
    d.title = [](const NodeDef&, const Node& n) { return n.param("variable", "?"); };
    d.run = [](Context& c) {
      c.out(std::size_t{0}, c.instance().get(c.param("variable")));
      return kStop;
    };
    lib.add(std::move(d));
  }
  {
    NodeDef d = make("var.set", "Set Variable", "Variables", NodeKind::action, "Writes a variable of this graph.");
    d.params = {param("variable", "Variable", ParamDef::Kind::variable)};
    d.inputs = {exec_in(), data("value", "Value", PinType::number)};
    d.outputs = {exec_out(), data("value", "Value", PinType::number)};
    d.shape = var_shape(true, true);
    d.title = [](const NodeDef&, const Node& n) { return "Set " + n.param("variable", "?"); };
    d.run = [](Context& c) {
      const std::string name = c.param("variable");
      c.instance().set(name, c.in("value"));
      c.out("value", c.instance().get(name));
      return 0;
    };
    lib.add(std::move(d));
  }
  {
    NodeDef d = make("var.add", "Add to Variable", "Variables", NodeKind::action, "Adds Amount to a number variable (use -1 to count down).");
    d.params = {param("variable", "Variable", ParamDef::Kind::variable)};
    d.inputs = {exec_in(), data("amount", "Amount", PinType::number, 1)};
    d.outputs = {exec_out(), data("value", "Value", PinType::number)};
    d.keywords = "increment decrement counter plus";
    d.title = [](const NodeDef&, const Node& n) { return "Add to " + n.param("variable", "?"); };
    d.run = [](Context& c) {
      const std::string name = c.param("variable");
      const Value cur = c.instance().get(name);
      c.instance().set(name, Value::number(cur.as_number() + c.in_number("amount")));
      c.out("value", c.instance().get(name));
      return 0;
    };
    lib.add(std::move(d));
  }
  {
    NodeDef d = make("state.set", "Set State", "State Machines", NodeKind::action, "Switches a state machine to a state directly.");
    d.params = {param("machine", "Machine", ParamDef::Kind::machine, "State Machine"), param("state", "State", ParamDef::Kind::state, "")};
    d.inputs = {exec_in()};
    d.outputs = {exec_out()};
    d.title = [](const NodeDef&, const Node& n) { return "Go to " + n.param("state", "?"); };
    d.run = [](Context& c) {
      c.instance().set_state(c.param("machine", "State Machine"), c.param("state"));
      return 0;
    };
    lib.add(std::move(d));
  }
  {
    NodeDef d = make("state.get", "Current State", "State Machines", NodeKind::pure, "The name of the state a machine is in.");
    d.params = {param("machine", "Machine", ParamDef::Kind::machine, "State Machine")};
    d.outputs = {data("state", "State", PinType::text)};
    d.run = [](Context& c) {
      c.out(std::size_t{0}, Value::text(c.instance().state(c.param("machine", "State Machine"))));
      return kStop;
    };
    lib.add(std::move(d));
  }
  {
    NodeDef d = make("state.is", "Is In State", "State Machines", NodeKind::pure, "True while the machine is in the state.");
    d.params = {param("machine", "Machine", ParamDef::Kind::machine, "State Machine"), param("state", "State", ParamDef::Kind::state, "")};
    d.outputs = {data("result", "Result", PinType::boolean)};
    d.title = [](const NodeDef&, const Node& n) { return "Is " + n.param("state", "?"); };
    d.run = [](Context& c) {
      c.out(std::size_t{0}, Value::boolean(c.instance().state(c.param("machine", "State Machine")) == c.param("state")));
      return kStop;
    };
    lib.add(std::move(d));
  }
}

void add_math(Library& lib) {
  using K = PinType;
  auto ab = [](K t, json::Value a = 0.0, json::Value b = 0.0) { return std::vector<PinDef>{data("a", "A", t, a), data("b", "B", t, b)}; };
  lib.add(pure("math.add", "Add", "Math", ab(K::number), data("result", "", K::number), [](Context& c) { return Value::number(c.in_number("a") + c.in_number("b")); }, "A + B", "+ plus sum"));
  lib.add(pure("math.sub", "Subtract", "Math", ab(K::number), data("result", "", K::number), [](Context& c) { return Value::number(c.in_number("a") - c.in_number("b")); }, "A - B", "- minus"));
  lib.add(pure("math.mul", "Multiply", "Math", ab(K::number, 1.0, 1.0), data("result", "", K::number), [](Context& c) { return Value::number(c.in_number("a") * c.in_number("b")); }, "A × B", "* times"));
  lib.add(pure("math.div", "Divide", "Math", ab(K::number, 1.0, 1.0), data("result", "", K::number), [](Context& c) {
    const f64 b = c.in_number("b");
    return Value::number(b != 0 ? c.in_number("a") / b : 0.0);
  }, "A ÷ B (0 when B is 0)", "/"));
  lib.add(pure("math.mod", "Modulo", "Math", ab(K::number, 0.0, 2.0), data("result", "", K::number), [](Context& c) {
    const f64 b = c.in_number("b");
    return Value::number(b != 0 ? std::fmod(c.in_number("a"), b) : 0.0);
  }, "Remainder of A ÷ B", "% remainder"));
  lib.add(pure("math.min", "Min", "Math", ab(K::number), data("result", "", K::number), [](Context& c) { return Value::number(min_of(c.in_number("a"), c.in_number("b"))); }));
  lib.add(pure("math.max", "Max", "Math", ab(K::number), data("result", "", K::number), [](Context& c) { return Value::number(max_of(c.in_number("a"), c.in_number("b"))); }));
  lib.add(pure("math.clamp", "Clamp", "Math", {data("value", "Value", K::number, 0.0), data("min", "Min", K::number, 0.0), data("max", "Max", K::number, 1.0)},
               data("result", "", K::number), [](Context& c) {
                 const f64 lo = c.in_number("min"), hi = c.in_number("max");
                 return Value::number(clamp(c.in_number("value"), min_of(lo, hi), max_of(lo, hi)));
               }));
  lib.add(pure("math.abs", "Absolute", "Math", {data("a", "A", K::number, 0.0)}, data("result", "", K::number), [](Context& c) { return Value::number(std::fabs(c.in_number("a"))); }));
  lib.add(pure("math.negate", "Negate", "Math", {data("a", "A", K::number, 0.0)}, data("result", "", K::number), [](Context& c) { return Value::number(-c.in_number("a")); }));
  lib.add(pure("math.lerp", "Lerp", "Math", {data("a", "A", K::number, 0.0), data("b", "B", K::number, 1.0), data("alpha", "Alpha", K::number, 0.5)}, data("result", "", K::number),
               [](Context& c) { const f64 a = c.in_number("a"); return Value::number(a + (c.in_number("b") - a) * c.in_number("alpha")); }, "A + (B - A) × Alpha", "interpolate mix blend"));
  lib.add(pure("math.sin", "Sin (degrees)", "Math", {data("a", "Degrees", K::number, 0.0)}, data("result", "", K::number), [](Context& c) { return Value::number(kern::dm::sin(c.in_number("a") * kDeg)); }));
  lib.add(pure("math.cos", "Cos (degrees)", "Math", {data("a", "Degrees", K::number, 0.0)}, data("result", "", K::number), [](Context& c) { return Value::number(kern::dm::cos(c.in_number("a") * kDeg)); }));
  lib.add(pure("math.atan2", "Atan2 (degrees)", "Math", {data("y", "Y", K::number, 0.0), data("x", "X", K::number, 1.0)}, data("result", "", K::number),
               [](Context& c) { return Value::number(kern::dm::atan2(c.in_number("y"), c.in_number("x")) / kDeg); }, "Angle of (X, Y) in degrees", "angle"));
  lib.add(pure("math.sqrt", "Square Root", "Math", {data("a", "A", K::number, 0.0)}, data("result", "", K::number), [](Context& c) { return Value::number(std::sqrt(max_of(0.0, c.in_number("a")))); }));
  lib.add(pure("math.pow", "Power", "Math", {data("a", "Base", K::number, 2.0), data("b", "Exponent", K::number, 2.0)}, data("result", "", K::number),
               [](Context& c) { const f64 r = kern::dm::pow(c.in_number("a"), c.in_number("b")); return Value::number(std::isfinite(r) ? r : 0.0); }));
  lib.add(pure("math.round", "Round", "Math", {data("a", "A", K::number, 0.0)}, data("result", "", K::integer), [](Context& c) { return Value::integer(static_cast<i64>(std::llround(c.in_number("a")))); }));
  lib.add(pure("math.floor", "Floor", "Math", {data("a", "A", K::number, 0.0)}, data("result", "", K::integer), [](Context& c) { return Value::integer(static_cast<i64>(std::floor(c.in_number("a")))); }));
  lib.add(pure("math.random", "Random Float in Range", "Math", {data("min", "Min", K::number, 0.0), data("max", "Max", K::number, 1.0)}, data("result", "", K::number),
               [](Context& c) { const f64 lo = c.in_number("min"); return Value::number(lo + (c.in_number("max") - lo) * random01(c)); }, "Deterministic per world", "rand"));
  lib.add(pure("math.random_int", "Random Integer in Range", "Math", {data("min", "Min", K::integer, 0), data("max", "Max", K::integer, 10)}, data("result", "", K::integer), [](Context& c) {
    const i64 lo = c.in_int("min"), hi = max_of(lo, c.in_int("max"));
    return Value::integer(lo + static_cast<i64>(c.host().random() % static_cast<u32>(min_of<i64>(hi - lo + 1, 0x7fffffff))));
  }, "Min to Max inclusive", "rand"));
  lib.add(pure("math.random_bool", "Random Bool", "Math", {data("chance", "Chance", K::number, 0.5)}, data("result", "", K::boolean),
               [](Context& c) { return Value::boolean(random01(c) < c.in_number("chance")); }, "True with the given chance (0..1)", "rand coin"));
  {
    NodeDef d = pure("math.compare", "Compare", "Logic", {data("a", "A", K::number, 0.0), data("b", "B", K::number, 0.0)}, data("result", "", K::boolean), [](Context& c) {
      const std::string op = c.param("op", "==");
      const Value a = c.in("a"), b = c.in("b");
      const f64 x = a.as_number(), y = b.as_number();
      if (op == "==") return Value::boolean(std::fabs(x - y) <= 1e-9);
      if (op == "!=") return Value::boolean(std::fabs(x - y) > 1e-9);
      if (op == "<") return Value::boolean(x < y);
      if (op == "<=") return Value::boolean(x <= y);
      if (op == ">") return Value::boolean(x > y);
      return Value::boolean(x >= y);
    }, "Compares two numbers", "equal less greater == != < > <= >=");
    d.params = {param("op", "Operator", ParamDef::Kind::choice, "==", {"==", "=", "!=", "≠", "<", "<", "<=", "≤", ">", ">", ">=", "≥"})};
    d.title = [](const NodeDef&, const Node& n) { return "A " + n.param("op", "==") + " B"; };
    lib.add(std::move(d));
  }
  lib.add(pure("logic.and", "AND", "Logic", ab(K::boolean, false, false), data("result", "", K::boolean), [](Context& c) { return Value::boolean(c.in_bool("a") && c.in_bool("b")); }, "", "&& both"));
  lib.add(pure("logic.or", "OR", "Logic", ab(K::boolean, false, false), data("result", "", K::boolean), [](Context& c) { return Value::boolean(c.in_bool("a") || c.in_bool("b")); }, "", "|| either"));
  lib.add(pure("logic.xor", "XOR", "Logic", ab(K::boolean, false, false), data("result", "", K::boolean), [](Context& c) { return Value::boolean(c.in_bool("a") != c.in_bool("b")); }));
  lib.add(pure("logic.not", "NOT", "Logic", {data("a", "A", K::boolean, false)}, data("result", "", K::boolean), [](Context& c) { return Value::boolean(!c.in_bool("a")); }, "", "! invert"));
  {
    NodeDef d = pure("logic.select", "Select", "Logic", {data("pick", "Pick A", K::boolean, true), data("a", "A", K::number, 0.0), data("b", "B", K::number, 0.0)}, data("result", "", K::number),
                     [](Context& c) { return c.in_bool("pick") ? c.in("a") : c.in("b"); }, "A when Pick A is true, otherwise B", "ternary choose if");
    d.params = {param("type", "Type", ParamDef::Kind::choice, "float", {"float", "Number", "int", "Integer", "bool", "Boolean", "vector", "Vector", "text", "Text", "actor", "Actor"})};
    d.shape = [](const NodeDef&, const Node& n, const Graph&, std::vector<PinDef>& in, std::vector<PinDef>& out) {
      PinType t = PinType::number;
      pin_type_from_name(n.param("type", "float"), t);
      if (t == PinType::exec) t = PinType::number;
      for (PinDef& p : in) if (p.id != "pick") { p.type = t; p.def = Value::zero(t).to_json(); }
      out[0].type = t;
    };
    lib.add(std::move(d));
  }
  // Literals.
  lib.add(pure("value.float", "Number", "Values", {data("value", "", K::number, 0.0)}, data("result", "", K::number), [](Context& c) { return c.in("value"); }, "A constant number", "float literal constant"));
  lib.add(pure("value.int", "Integer", "Values", {data("value", "", K::integer, 0)}, data("result", "", K::integer), [](Context& c) { return c.in("value"); }, "A constant integer", "int literal"));
  lib.add(pure("value.bool", "Boolean", "Values", {data("value", "", K::boolean, false)}, data("result", "", K::boolean), [](Context& c) { return c.in("value"); }, "true / false", "literal"));
  lib.add(pure("value.text", "Text", "Values", {data("value", "", K::text, "")}, data("result", "", K::text), [](Context& c) { return c.in("value"); }, "A constant text", "string literal"));
  // Vectors.
  const json::Value zero3 = [] { json::Value a = json::Value::array(); a.push(0.0); a.push(0.0); a.push(0.0); return a; }();
  lib.add(pure("vector.make", "Make Vector", "Vector", {data("x", "X", K::number, 0.0), data("y", "Y", K::number, 0.0), data("z", "Z", K::number, 0.0)}, data("result", "", K::vector),
               [](Context& c) { return Value::vector({static_cast<f32>(c.in_number("x")), static_cast<f32>(c.in_number("y")), static_cast<f32>(c.in_number("z"))}); }));
  {
    NodeDef d = make("vector.break", "Break Vector", "Vector", NodeKind::pure, "X, Y and Z of a vector.");
    d.inputs = {data("v", "Vector", K::vector, zero3)};
    d.outputs = {data("x", "X", K::number), data("y", "Y", K::number), data("z", "Z", K::number)};
    d.run = [](Context& c) {
      const Vec3 v = c.in_vector("v");
      c.out("x", Value::number(v.x));
      c.out("y", Value::number(v.y));
      c.out("z", Value::number(v.z));
      return kStop;
    };
    lib.add(std::move(d));
  }
  auto vv = [&](const char* type, const char* label, std::function<Vec3(Vec3, Vec3)> f, const char* kw = "") {
    lib.add(pure(type, label, "Vector", {data("a", "A", K::vector, zero3), data("b", "B", K::vector, zero3)}, data("result", "", K::vector),
                 [f](Context& c) { return Value::vector(f(c.in_vector("a"), c.in_vector("b"))); }, "", kw));
  };
  vv("vector.add", "Vector + Vector", [](Vec3 a, Vec3 b) { return a + b; }, "add plus");
  vv("vector.sub", "Vector - Vector", [](Vec3 a, Vec3 b) { return a - b; }, "subtract minus");
  vv("vector.cross", "Cross Product", [](Vec3 a, Vec3 b) { return cross(a, b); });
  lib.add(pure("vector.scale", "Vector × Float", "Vector", {data("a", "Vector", K::vector, zero3), data("b", "Scale", K::number, 1.0)}, data("result", "", K::vector),
               [](Context& c) { return Value::vector(c.in_vector("a") * static_cast<f32>(c.in_number("b"))); }, "", "multiply scale"));
  lib.add(pure("vector.length", "Vector Length", "Vector", {data("a", "Vector", K::vector, zero3)}, data("result", "", K::number), [](Context& c) { return Value::number(length(c.in_vector("a"))); }, "", "magnitude size"));
  lib.add(pure("vector.normalize", "Normalize", "Vector", {data("a", "Vector", K::vector, zero3)}, data("result", "", K::vector), [](Context& c) { return Value::vector(normalize(c.in_vector("a"), {0, 0, 0})); }, "", "unit direction"));
  lib.add(pure("vector.distance", "Distance", "Vector", {data("a", "A", K::vector, zero3), data("b", "B", K::vector, zero3)}, data("result", "", K::number),
               [](Context& c) { return Value::number(distance(c.in_vector("a"), c.in_vector("b"))); }));
  lib.add(pure("vector.dot", "Dot Product", "Vector", {data("a", "A", K::vector, zero3), data("b", "B", K::vector, zero3)}, data("result", "", K::number),
               [](Context& c) { return Value::number(dot(c.in_vector("a"), c.in_vector("b"))); }));
  lib.add(pure("vector.lerp", "Lerp Vector", "Vector", {data("a", "A", K::vector, zero3), data("b", "B", K::vector, zero3), data("alpha", "Alpha", K::number, 0.5)}, data("result", "", K::vector),
               [](Context& c) { return Value::vector(lerp(c.in_vector("a"), c.in_vector("b"), static_cast<f32>(c.in_number("alpha")))); }, "", "interpolate"));
  // Text.
  lib.add(pure("text.append", "Append", "Text", {data("a", "A", K::text, ""), data("b", "B", K::text, ""), data("c", "C", K::text, "")}, data("result", "", K::text),
               [](Context& c) { return Value::text(c.in_text("a") + c.in_text("b") + c.in_text("c")); }, "Joins texts (numbers and vectors convert)", "concat string join format"));
  lib.add(pure("text.equal", "Text Equals", "Text", {data("a", "A", K::text, ""), data("b", "B", K::text, "")}, data("result", "", K::boolean),
               [](Context& c) { return Value::boolean(c.in_text("a") == c.in_text("b")); }));
  lib.add(pure("text.contains", "Contains", "Text", {data("a", "Text", K::text, ""), data("b", "Search", K::text, "")}, data("result", "", K::boolean),
               [](Context& c) { return Value::boolean(c.in_text("a").find(c.in_text("b")) != std::string::npos); }));
  lib.add(pure("text.length", "Text Length", "Text", {data("a", "Text", K::text, "")}, data("result", "", K::integer), [](Context& c) { return Value::integer(static_cast<i64>(c.in_text("a").size())); }));
  // Time.
  lib.add(pure("time.now", "Game Time", "Utilities", {}, data("result", "Seconds", K::number), [](Context& c) { return Value::number(c.host().time()); }, "Seconds since play started", "clock elapsed"));
  {
    NodeDef d = make("debug.print", "Print", "Utilities", NodeKind::action, "Writes text to the log (and on screen).");
    d.inputs = {exec_in(), data("text", "Text", K::text, "Hello"), data("screen", "On Screen", K::boolean, true)};
    d.outputs = {exec_out()};
    d.keywords = "log debug message string";
    d.run = [](Context& c) {
      c.host().print(c.in_text("text"), c.in_bool("screen"));
      return 0;
    };
    lib.add(std::move(d));
  }
}

}  // namespace

Library& Library::core() {
  static Library lib;
  static bool ready = false;
  if (!ready) {
    ready = true;
    add_events(lib);
    add_flow(lib);
    add_variables(lib);
    add_math(lib);
  }
  return lib;
}

}  // namespace kern::logic
