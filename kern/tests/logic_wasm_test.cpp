// JIT tier: built as its own WebAssembly module (tests/logic_wasm_test.mjs runs it in node).
// Each logic case runs twice - interpreter and JIT-compiled WebAssembly - and the results
// must match exactly.
#include <memory>
#include <string>

#include "kern/api/c_api.h"
#include "logic_cases.h"

namespace {
struct Run {
  logic_cases::Host host;
  std::unique_ptr<kern::logic::Runtime> rt;
  kern::logic::Instance* inst = nullptr;
  std::string result;
};
Run g_run;
}  // namespace

/// Starts case `index` (jit = 1 asks the host to compile it). Returns 1 when a compile is pending.
KERN_EXPORT(logic_test_prepare) int logic_test_prepare(int index, int jit) {
  const auto& cases = logic_cases::cases();
  if (index < 0 || static_cast<std::size_t>(index) >= cases.size()) return -1;
  g_run.inst = nullptr;
  g_run.rt.reset();
  g_run.host = logic_cases::Host{};
  g_run.rt = std::make_unique<kern::logic::Runtime>(g_run.host);
  g_run.rt->jit = jit != 0;
  g_run.inst = g_run.rt->add(logic_cases::parse(cases[static_cast<std::size_t>(index)]), 0, cases[static_cast<std::size_t>(index)].name);
  return std::string(g_run.inst->tier()) == "compiling" ? 1 : 0;
}

/// The tier the instance runs in now ("compiled (WebAssembly)", "interpreter"...).
KERN_EXPORT(logic_test_tier) const char* logic_test_tier() { return g_run.inst ? g_run.inst->tier() : ""; }

/// Plays the scenario; returns the result text (0-terminated).
KERN_EXPORT(logic_test_play) const char* logic_test_play() {
  g_run.result = g_run.inst ? logic_cases::play(*g_run.rt, g_run.host, g_run.inst) : std::string();
  return g_run.result.c_str();
}

KERN_EXPORT(logic_test_count) int logic_test_count() { return static_cast<int>(logic_cases::cases().size()); }

/// Benchmark: a For loop over `n` with arithmetic and a variable in the body. Returns ms.
KERN_EXPORT(logic_test_bench_prepare) int logic_test_bench_prepare(int jit) {
  static const logic_cases::Case bench{"bench", R"({"variables":[{"name":"Acc","type":"float","value":0}],
    "nodes":[{"id":1,"type":"event.custom","params":{"name":"Go"}},{"id":2,"type":"flow.for","values":{"first":1,"last":1000000}},
      {"id":3,"type":"var.set","params":{"variable":"Acc"}},{"id":4,"type":"math.add"},{"id":5,"type":"var.get","params":{"variable":"Acc"}},
      {"id":6,"type":"math.mul","values":{"b":0.5}},{"id":7,"type":"math.mod","values":{"b":7}}],
    "links":[[1,"then",2,"exec"],[2,"body",3,"exec"],[5,"value",4,"a"],[6,"result",4,"b"],[7,"result",6,"a"],[2,"index",7,"a"],[4,"result",3,"value"]]})"};
  g_run.inst = nullptr;
  g_run.rt.reset();
  g_run.host = logic_cases::Host{};
  g_run.rt = std::make_unique<kern::logic::Runtime>(g_run.host);
  g_run.rt->jit = jit != 0;
  g_run.rt->budget = 100000000;
  g_run.inst = g_run.rt->add(logic_cases::parse(bench), 0, "bench");
  return 0;
}

KERN_EXPORT(logic_test_bench_run) double logic_test_bench_run() {
  const double t0 = kern::now_ms();
  g_run.rt->call("Go");
  const double t1 = kern::now_ms();
  return t1 - t0 + 0 * g_run.inst->get("Acc").as_number();
}

KERN_EXPORT(logic_test_bench_value) double logic_test_bench_value() { return g_run.inst->get("Acc").as_number(); }
