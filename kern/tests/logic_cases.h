// Logic graphs whose results must be identical in every tier: the interpreter,
// the JIT (WebAssembly, tests/logic_wasm_test.*) and exported C++ (run.sh builds
// the generated sources into tests/logic_native_test.cpp).
#pragma once

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "kern/core/json.h"
#include "kern/logic/logic.h"

namespace logic_cases {

struct Case {
  const char* name;
  const char* graph;
};

inline const std::vector<Case>& cases() {
  static const std::vector<Case> list = {
      {"math_loops",
       R"({"variables":[{"name":"Sum","type":"float","value":0},{"name":"Odd","type":"int","value":0},{"name":"Flag","type":"bool","value":false},{"name":"Avg","type":"float","value":0}],
        "nodes":[{"id":1,"type":"event.begin"},{"id":2,"type":"flow.for","values":{"first":1,"last":20}},
          {"id":3,"type":"var.add","params":{"variable":"Sum"}},{"id":4,"type":"math.mul","values":{"b":1.5}},
          {"id":5,"type":"flow.branch"},{"id":6,"type":"math.compare","params":{"op":"=="},"values":{"b":1}},{"id":7,"type":"math.mod","values":{"b":2}},
          {"id":8,"type":"var.add","params":{"variable":"Odd"}},
          {"id":9,"type":"var.set","params":{"variable":"Flag"}},{"id":10,"type":"logic.and"},{"id":11,"type":"math.compare","params":{"op":">"},"values":{"b":100}},
          {"id":12,"type":"var.get","params":{"variable":"Sum"}},{"id":13,"type":"logic.not"},{"id":14,"type":"math.compare","params":{"op":"<"},"values":{"b":5}},
          {"id":15,"type":"var.set","params":{"variable":"Avg"}},{"id":16,"type":"math.div","values":{"b":20}},{"id":17,"type":"math.clamp","values":{"min":0,"max":14}},
          {"id":18,"type":"math.round"},{"id":19,"type":"math.lerp","values":{"alpha":0.25}},{"id":20,"type":"math.sqrt"}],
        "links":[[1,"then",2,"exec"],[2,"body",3,"exec"],[2,"index",4,"a"],[4,"result",3,"amount"],
          [3,"then",5,"exec"],[2,"index",7,"a"],[7,"result",6,"a"],[6,"result",5,"condition"],[5,"true",8,"exec"],
          [2,"completed",9,"exec"],[12,"value",11,"a"],[11,"result",10,"a"],[14,"result",13,"a"],[13,"result",10,"b"],[12,"value",14,"a"],[10,"result",9,"value"],
          [9,"then",15,"exec"],[12,"value",16,"a"],[16,"result",17,"value"],[17,"result",18,"a"],[18,"result",19,"b"],[20,"result",19,"a"],[12,"value",20,"a"],[19,"result",15,"value"]]})"},
      {"gates_and_sequences",
       R"({"variables":[{"name":"Log","type":"float","value":0},{"name":"Once","type":"int","value":0},{"name":"N","type":"int","value":0},{"name":"Gate","type":"int","value":0},{"name":"Flips","type":"int","value":0}],
        "nodes":[{"id":1,"type":"event.custom","params":{"name":"Go"}},{"id":2,"type":"flow.sequence","params":{"count":"5"}},
          {"id":3,"type":"var.set","params":{"variable":"Log"}},{"id":4,"type":"math.add"},{"id":5,"type":"math.mul","values":{"b":10}},{"id":6,"type":"var.get","params":{"variable":"Log"}},
          {"id":7,"type":"flow.do_once"},{"id":8,"type":"var.add","params":{"variable":"Once"}},
          {"id":9,"type":"flow.do_n","values":{"n":2}},{"id":10,"type":"var.add","params":{"variable":"N"}},
          {"id":11,"type":"flow.gate","values":{"closed":true}},{"id":12,"type":"var.add","params":{"variable":"Gate"}},
          {"id":13,"type":"flow.flip_flop"},{"id":14,"type":"var.add","params":{"variable":"Flips"},"values":{"amount":1}},{"id":15,"type":"var.add","params":{"variable":"Flips"},"values":{"amount":100}},
          {"id":16,"type":"event.custom","params":{"name":"Open"}},{"id":17,"type":"value.float","values":{"value":1}}],
        "links":[[1,"then",2,"exec"],[2,"then0",3,"exec"],[6,"value",5,"a"],[5,"result",4,"a"],[17,"result",4,"b"],[4,"result",3,"value"],
          [2,"then1",7,"exec"],[7,"completed",8,"exec"],[2,"then2",9,"exec"],[9,"exit",10,"exec"],[2,"then3",11,"enter"],[11,"exit",12,"exec"],
          [2,"then4",13,"exec"],[13,"a",14,"exec"],[13,"b",15,"exec"],[16,"then",11,"open"]]})"},
      {"while_nested",
       R"({"variables":[{"name":"I","type":"int","value":0},{"name":"Total","type":"float","value":0}],
        "nodes":[{"id":1,"type":"event.begin"},{"id":2,"type":"flow.while"},{"id":3,"type":"math.compare","params":{"op":"<"},"values":{"b":7}},{"id":4,"type":"var.get","params":{"variable":"I"}},
          {"id":5,"type":"flow.sequence","params":{"count":"2"}},{"id":6,"type":"var.add","params":{"variable":"I"}},{"id":7,"type":"flow.for","values":{"first":0,"last":3}},
          {"id":8,"type":"var.add","params":{"variable":"Total"}},{"id":9,"type":"math.mul"},{"id":10,"type":"var.get","params":{"variable":"I"}}],
        "links":[[1,"then",2,"exec"],[4,"value",3,"a"],[3,"result",2,"condition"],[2,"body",5,"exec"],[5,"then0",6,"exec"],[5,"then1",7,"exec"],
          [7,"body",8,"exec"],[7,"index",9,"a"],[10,"value",9,"b"],[9,"result",8,"amount"]]})"},
      {"mixed_cpp_nodes",
       R"({"variables":[{"name":"Text","type":"text","value":""},{"name":"Count","type":"int","value":0},{"name":"Done","type":"bool","value":false}],
        "nodes":[{"id":1,"type":"event.custom","params":{"name":"Go"}},{"id":2,"type":"var.add","params":{"variable":"Count"}},{"id":3,"type":"var.set","params":{"variable":"Text"}},
          {"id":4,"type":"text.append","values":{"a":"n="}},{"id":5,"type":"debug.print"},{"id":6,"type":"flow.branch"},{"id":7,"type":"math.compare","params":{"op":">="},"values":{"b":3}},
          {"id":8,"type":"flow.delay","values":{"duration":0.5}},{"id":9,"type":"var.set","params":{"variable":"Done"},"values":{"value":true}},{"id":10,"type":"math.add"}],
        "links":[[1,"then",2,"exec"],[2,"then",3,"exec"],[2,"value",4,"b"],[4,"result",3,"value"],[3,"then",5,"exec"],[3,"value",5,"text"],
          [5,"then",6,"exec"],[2,"value",7,"a"],[7,"result",6,"condition"],[6,"true",8,"exec"],[8,"completed",9,"exec"],[1,"value",10,"a"],[2,"value",10,"b"]]})"},
      {"state_machine",
       R"({"variables":[{"name":"Entered","type":"int","value":0},{"name":"Ready","type":"bool","value":false}],
        "machines":[{"id":30,"name":"M","states":[{"id":31,"name":"A","initial":true},{"id":32,"name":"B"},{"id":33,"name":"C"}],
          "transitions":[{"id":40,"from":31,"to":32,"trigger":"event","event":"Go"},{"id":41,"from":32,"to":33,"trigger":"timer","seconds":0.25},{"id":42,"from":33,"to":31,"trigger":"condition","variable":"Ready"}]}],
        "nodes":[{"id":1,"type":"event.state_enter","params":{"machine":"M","state":"any"}},{"id":2,"type":"var.add","params":{"variable":"Entered"}},
          {"id":3,"type":"event.state_enter","params":{"machine":"M","state":"C"}},{"id":4,"type":"var.set","params":{"variable":"Ready"},"values":{"value":true}}],
        "links":[[1,"then",2,"exec"],[3,"then",4,"exec"]]})"},
      {"infinite_loop",
       R"({"variables":[{"name":"N","type":"int","value":0}],
        "nodes":[{"id":1,"type":"event.begin"},{"id":2,"type":"flow.while","values":{"condition":true}},{"id":3,"type":"var.add","params":{"variable":"N"}}],
        "links":[[1,"then",2,"exec"],[2,"body",3,"exec"]]})"},
  };
  return list;
}

/// Host with a clock and a log; records prints.
struct Host : kern::logic::Host {
  kern::f64 t = 0;
  kern::u32 seed = 1;
  std::string log;
  kern::f64 time() const override { return t; }
  void print(const std::string& text, bool) override { log += "print " + text + "\n"; }
  void error(const std::string& text) override { log += "error " + text.substr(0, 40) + "\n"; }
  kern::u32 random() override { seed = seed * 1664525u + 1013904223u; return seed; }
};

inline kern::logic::Graph parse(const Case& c) {
  kern::json::Value j;
  kern::logic::Graph g;
  if (kern::json::parse(c.graph, j)) kern::logic::graph_from_json(j, g);
  return g;
}

/// The scenario every tier runs: Begin Play, 3 x Go, Open, 2 x Go, ticks for 2 s, Go.
/// Returns the variables and the log as text.
inline std::string play(kern::logic::Runtime& rt, Host& host, kern::logic::Instance* inst) {
  using kern::logic::Value;
  rt.budget = 20000;
  rt.begin();
  for (int k = 0; k < 3; ++k) rt.call("Go", 0, {{"value", Value::number(k * 2)}});
  rt.call("Open");
  for (int k = 0; k < 2; ++k) rt.call("Go", 0, {{"value", Value::number(5)}});
  for (int i = 0; i < 120; ++i) { host.t += 1.0 / 60; rt.tick(1.0f / 60); }
  rt.call("Go", 0, {{"value", Value::number(1)}});
  for (int i = 0; i < 60; ++i) { host.t += 1.0 / 60; rt.tick(1.0f / 60); }
  std::string out;
  for (const auto& v : inst->graph().variables) out += v.name + "=" + inst->get(v.name).as_text() + "\n";
  for (const auto& m : inst->graph().machines) out += m.name + ":" + inst->state(m.name) + "\n";
  out += inst->disabled() ? "disabled\n" : "running\n";
  return out + host.log;
}

}  // namespace logic_cases
