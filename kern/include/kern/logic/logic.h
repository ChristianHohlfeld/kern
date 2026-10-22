// Kern Logic: visual scripting in the engine, backed entirely by C++.
//
// There is no script language. A logic graph is data (nodes, links, literal
// values, variables, state machines); every node type is a C++ function in a
// registry (core flow / math / variables here, gameplay nodes in the game
// module). Graphs are checked, compiled to an index table and run by a small
// interpreter in the engine's fixed simulation steps, on the authority only
// (the standalone game or the network server). Their effects replicate like
// everything else the simulation does.
//
// Model (Unreal Blueprint semantics):
//   exec pins     white arrows: what runs next. Event nodes start a chain,
//                 action nodes continue it, flow nodes branch / loop / wait.
//   data pins     typed values. Inputs pull: pure nodes (math, getters) are
//                 evaluated on demand, outputs of action nodes keep the value
//                 they had when the node last ran. Unlinked inputs use the
//                 literal typed into the node.
//   variables     per graph instance, typed, with a default.
//   state machines  states and transitions in the style of the
//                 digitalisierungsplanung.de state editor: a transition fires
//                 on an event, a timer, a condition variable or automatically;
//                 entering / leaving a state raises events the graph handles.
//   latent nodes  Delay, Tween, Retriggerable Delay suspend the chain and
//                 resume it from the tick, like Blueprint latent actions.
//
// A level has one level graph; any actor can carry its own graph (its Self).
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "kern/core/base.h"
#include "kern/core/json.h"
#include "kern/math/math.h"

namespace kern::logic {

using ActorRef = u32;  // game actor id (0 = none)

enum class PinType : u8 { exec, boolean, integer, number, vector, text, actor, count };
const char* pin_type_name(PinType t);
bool pin_type_from_name(std::string_view name, PinType& out);
/// Data that can flow from `from` into an input of type `to` (autocasts).
bool convertible(PinType from, PinType to);

/// A value on a data pin.
struct Value {
  PinType type = PinType::number;
  bool b = false;
  i64 i = 0;
  f64 n = 0;
  Vec3 v;
  std::string s;
  ActorRef a = 0;

  static Value boolean(bool x) { Value r; r.type = PinType::boolean; r.b = x; return r; }
  static Value integer(i64 x) { Value r; r.type = PinType::integer; r.i = x; return r; }
  static Value number(f64 x) { Value r; r.type = PinType::number; r.n = x; return r; }
  static Value vector(Vec3 x) { Value r; r.type = PinType::vector; r.v = x; return r; }
  static Value text(std::string x) { Value r; r.type = PinType::text; r.s = std::move(x); return r; }
  static Value actor(ActorRef x) { Value r; r.type = PinType::actor; r.a = x; return r; }
  static Value zero(PinType t);

  bool as_bool() const;
  i64 as_int() const;
  f64 as_number() const;
  Vec3 as_vector() const;
  std::string as_text() const;
  ActorRef as_actor() const;
  Value converted(PinType to) const;
  json::Value to_json() const;
  static Value from_json(const json::Value& j, PinType type);
  bool operator==(const Value& o) const;
};

// ---------------------------------------------------------------------------
// graph data

struct Node {
  u32 id = 0;
  std::string type;                                   // registry key, e.g. "flow.branch"
  f32 x = 0, y = 0;                                   // canvas position (px)
  std::vector<std::pair<std::string, json::Value>> values;  // literals of unlinked inputs, by pin id
  std::vector<std::pair<std::string, std::string>> params;  // node settings (variable name, event name, key...)
  std::string comment;
  const json::Value* value(std::string_view pin) const;
  std::string param(std::string_view key, std::string_view fallback = {}) const;
  void set_value(std::string_view pin, json::Value v);
  void set_param(std::string_view key, std::string v);
};

struct Link {
  u32 from = 0;          // node id
  std::string out;       // output pin id
  u32 to = 0;
  std::string in;        // input pin id
  bool operator==(const Link& o) const { return from == o.from && out == o.out && to == o.to && in == o.in; }
};

struct Variable {
  std::string name;
  PinType type = PinType::number;
  json::Value value;     // default
  std::string help;
};

/// A state machine in the style of the digitalisierungsplanung.de editor.
enum class TriggerKind : u8 { event, timer, condition, automatic };
const char* trigger_kind_name(TriggerKind k);

struct State {
  u32 id = 0;
  std::string name;
  f32 x = 0, y = 0;
  bool initial = false;
};

struct Transition {
  u32 id = 0;
  u32 from = 0, to = 0;  // state ids (from 0 = from any state)
  TriggerKind trigger = TriggerKind::event;
  std::string event;     // event: custom event name that switches
  f32 seconds = 1;       // timer: seconds in the from state
  std::string variable;  // condition: boolean variable that must be true
  std::string label;
};

struct StateMachine {
  u32 id = 0;
  std::string name = "State Machine";
  std::vector<State> states;
  std::vector<Transition> transitions;
  const State* find(u32 state) const;
  const State* find(std::string_view name) const;
};

struct Graph {
  std::vector<Node> nodes;
  std::vector<Link> links;
  std::vector<Variable> variables;
  std::vector<StateMachine> machines;
  u32 next_id = 1;
  bool empty() const { return nodes.empty() && variables.empty() && machines.empty(); }
  Node* find(u32 id);
  const Node* find(u32 id) const;
  const Variable* variable(std::string_view name) const;
  Variable* variable(std::string_view name);
  StateMachine* machine(u32 id);
  const StateMachine* machine(std::string_view name) const;
  bool operator==(const Graph& o) const;  // via JSON
};

json::Value graph_to_json(const Graph& g);
bool graph_from_json(const json::Value& j, Graph& out, std::string* error = nullptr);

// ---------------------------------------------------------------------------
// node library

struct PinDef {
  std::string id, label;
  PinType type = PinType::exec;
  json::Value def;       // default literal (data inputs)
  bool hidden = false;   // not drawn (advanced)
};

struct ParamDef {
  std::string key, label;
  enum class Kind : u8 { text, choice, variable, event, machine, state, key, sound, tag, integer } kind = Kind::text;
  std::vector<std::string> choices;  // value, label pairs flattened for Kind::choice
  std::string def;
};

class Context;

enum class NodeKind : u8 { event, action, pure, flow };

struct NodeDef {
  std::string type, label, category, help;
  NodeKind kind = NodeKind::action;
  std::string event;     // events: the Event::type it handles (e.g. "logic.tick", "game.overlap")
  bool latent = false;
  std::vector<PinDef> inputs, outputs;  // exec pins included, in drawing order
  std::vector<ParamDef> params;
  std::string keywords;
  u32 color = 0;         // header colour 0xRRGGBB (0 = by kind)
  /// Pins that depend on params / the graph (variables, Sequence outputs, Switch cases...).
  std::function<void(const NodeDef& def, const Node& node, const Graph& graph, std::vector<PinDef>& in, std::vector<PinDef>& out)> shape;
  /// Title that depends on params ("Set Score", "On Key F").
  std::function<std::string(const NodeDef& def, const Node& node)> title;
  /// Events: does this node handle `e`? (default: e.key is empty, or equals the node's first param / is "any").
  std::function<bool(const Node& node, const struct Event& e)> accepts;
  /// Runs the node. Returns the index of the exec output to continue with
  /// (among exec outputs), or kStop / kSuspend. Pure nodes write outputs and return kStop.
  std::function<int(Context& c)> run;
};

constexpr int kStop = -1;      // the chain ends here
constexpr int kSuspend = -2;   // latent: resumed later from the tick

class Library {
 public:
  void add(NodeDef def);
  const NodeDef* find(std::string_view type) const;
  const std::vector<std::unique_ptr<NodeDef>>& all() const { return defs_; }
  /// Resolved pins of a node (static or shaped).
  void pins(const Node& node, const Graph& graph, std::vector<PinDef>& in, std::vector<PinDef>& out) const;
  std::string title(const Node& node) const;
  /// Built-in nodes: events Begin Play / Tick / Custom Event / state machine
  /// events, flow, variables, math, logic, vectors, text, debugging.
  static Library& core();

 private:
  std::vector<std::unique_ptr<NodeDef>> defs_;
  std::unordered_map<std::string, std::size_t> index_;
};

/// The node library a graph uses: core + whatever the game registered.
Library& library();

struct Issue {
  u32 node = 0;          // 0 = the graph itself
  bool error = true;
  std::string text;
};
/// Unknown node types, links between missing pins or incompatible types,
/// unknown variables / events / states, unlinked required inputs, cycles of pure nodes.
std::vector<Issue> check(const Graph& g, const Library& lib = library());

// ---------------------------------------------------------------------------
// running graphs

/// What logic can do to the world. The game implements it (GameWorld), tests can fake it.
class Host {
 public:
  virtual ~Host() = default;
  virtual f64 time() const = 0;                          // simulation seconds
  virtual void print(const std::string& text, bool screen) = 0;
  virtual void error(const std::string& text) = 0;
  /// Random numbers: deterministic per world (replays and the network agree).
  virtual u32 random() = 0;
  /// The game world behind this host (gameplay nodes), or null.
  virtual void* game() { return nullptr; }
  /// Native code: the host compiles the WebAssembly module of an instance (asynchronously)
  /// and calls jit_ready(id, function) when it is linked. False: no compiler here (interpreter).
  virtual bool jit_compile(const std::vector<u8>& module, i32 id);
  virtual void jit_release(i32 function);
};

/// The platform's compiler for generated WebAssembly (the browser's, through kern.mjs).
/// False where there is none (native builds): graphs then run in the interpreter or as
/// C++ compiled into the binary.
bool platform_jit_compile(const std::vector<u8>& module, i32 id);
void platform_jit_release(i32 function);

/// Generated code (WebAssembly from the JIT, or C++ exported from Studio) works on this
/// layout: f64 slots for numeric variables and node state, an i32 continuation stack,
/// and per node a table of exec targets. Everything else calls back into the C++ nodes.
struct CodeLayout {
  static constexpr int kNodeSlots = 8;   // per node: 4 state + 4 data outputs
  static constexpr int kStack = 512;     // continuation entries (pc, entry)
};

/// Compiles an instance's graph to a WebAssembly module (the JIT's input).
std::vector<u8> compile_wasm(const class Instance& inst);
/// The graph as C++ source: compiled into a game (build.sh) it replaces the interpreter
/// for graphs with the same content (matched by hash). `name` becomes the function name.
std::string generate_cpp(const class Instance& inst, const std::string& name);
/// Content hash of a graph (what generated C++ registers under).
u64 graph_hash(const Graph& g);
/// Code compiled into the binary: a generated function for a graph hash.
using NativeGraphFn = int (*)(void* inst, int pc, int entry);
void register_native_graph(u64 hash, NativeGraphFn fn);

struct NodeState {           // per node instance: Do Once, Gate, Flip Flop, loops, latent waits
  f64 time = 0, a = 0, b = 0;
  i64 counter = 0, index = 0, last = 0;
  bool flag = false, flag2 = false;
  Value value;
};

class Runtime;

/// One graph running for one owner (an actor, or the level).
class Instance {
 public:
  Instance(Runtime& rt, const Graph& graph, ActorRef self, std::string name);
  ~Instance();
  Instance(const Instance&) = delete;
  Instance& operator=(const Instance&) = delete;
  ActorRef self() const { return self_; }
  const std::string& name() const { return name_; }
  const Graph& graph() const { return graph_; }
  Value get(std::string_view variable) const;
  bool set(std::string_view variable, const Value& v);
  /// Current state of a machine ("" = none).
  std::string state(std::string_view machine) const;
  bool set_state(std::string_view machine, std::string_view state);
  bool disabled() const { return disabled_; }
  /// Runs as machine code (JIT-compiled WebAssembly or generated C++) instead of the interpreter.
  bool compiled() const { return jit_fn_ >= 0 || native_ != nullptr; }
  const char* tier() const { return native_ ? "native C++" : jit_fn_ >= 0 ? "compiled (WebAssembly)" : jit_pending_ ? "compiling" : "interpreter"; }
  i32 id() const { return id_; }

  // ---- generated code support (compiled modules and exported C++ call these) ----
  /// Runs the C++ function of node `node` (returns the exec output ordinal, kStop or kSuspend).
  int call_node(int node, int entry);
  /// Input `pin` (index) of node `node`, as a number.
  f64 read_pin(int node, int pin);
  /// Whether node `node` is generated inline (else generated code calls call_node).
  bool inline_node(int node) const;
  bool inline_pure(int node) const;
  f64* slots() { return slots_.data(); }
  i32* stack() { return stack_.data(); }
  i32* exec_table() { return exec_table_.data(); }
  int var_slot(std::string_view name) const;
  int node_slot(int node) const { return node_slot_base_ + node * CodeLayout::kNodeSlots; }
  /// Data output index -> slot (inline nodes with outputs other generated code / C++ read), -1 none.
  int out_slot(int node, int out) const;
  std::size_t node_count() const { return nodes_.size(); }
  const Node& node_at(int i) const { return *nodes_[static_cast<std::size_t>(i)].node; }
  const NodeDef* def_at(int i) const { return nodes_[static_cast<std::size_t>(i)].def; }
  const std::vector<PinDef>& inputs_at(int i) const { return nodes_[static_cast<std::size_t>(i)].in; }
  const std::vector<PinDef>& outputs_at(int i) const { return nodes_[static_cast<std::size_t>(i)].out; }
  struct SourceRef { int node = -1, pin = -1; };
  SourceRef source_of(int node, int input) const {
    const auto& s = nodes_[static_cast<std::size_t>(node)].data_in[static_cast<std::size_t>(input)];
    return {s.node, s.pin};
  }
  const Value& literal_of(int node, int input) const { return nodes_[static_cast<std::size_t>(node)].literal[static_cast<std::size_t>(input)]; }
  std::pair<int, int> exec_target(int node, int ordinal) const;
  int exec_outputs(int node) const { return static_cast<int>(nodes_[static_cast<std::size_t>(node)].exec_out.size()); }
  int exec_table_offset(int node) const { return exec_table_offset_[static_cast<std::size_t>(node)]; }
  /// Generated code ran node `node` (the editor's live view).
  void trace_node(int node);
  /// Shared counters the generated code checks: [0] nodes run this step, [1] limit, [2] tracing.
  i32* counters();

 private:
  friend class Runtime;
  friend class Context;
  void enter_compiled();              // copies interpreter state into the slots (tier switch)
  struct CNode {                     // compiled node
    const NodeDef* def = nullptr;
    const Node* node = nullptr;
    std::vector<PinDef> in, out;
    std::vector<int> exec_out;       // output index -> compiled index of the exec target (-1 none), by exec output order
    std::vector<int> exec_out_entry; // entry (exec input index) at the target
    struct Source { int node = -1, pin = -1; };
    std::vector<Source> data_in;     // per input: linked output (node, out index)
    std::vector<Value> literal;      // per input: literal / default
    std::vector<int> exec_in_index;  // input index -> exec input ordinal (-1 data)
    std::vector<Value> outputs;      // last output values
    NodeState state;
  };
  struct Machine {
    const StateMachine* def = nullptr;
    u32 state = 0;
    f64 entered = 0;
  };
  Runtime& rt_;
  Graph graph_;
  ActorRef self_ = 0;
  std::string name_;
  std::vector<CNode> nodes_;
  std::unordered_map<u32, int> index_;  // node id -> compiled index
  std::unordered_map<std::string, Value> vars_;   // non-numeric variables
  std::unordered_map<std::string, int> var_slots_; // numeric variables live in slots
  std::vector<Machine> machines_;
  std::vector<f64> slots_;            // fixed size: generated code holds absolute addresses
  std::vector<i32> stack_;            // [0] = depth, then (pc, entry) pairs
  std::vector<i32> exec_table_;       // per node: (target, entry) per exec output
  std::vector<int> exec_table_offset_;
  int node_slot_base_ = 0;
  i32 id_ = 0;
  i32 jit_fn_ = -1;                   // table index of the compiled run function
  bool jit_pending_ = false;
  NativeGraphFn native_ = nullptr;
  bool disabled_ = false;
  bool begun_ = false;
};

/// Event delivered to graphs: type (e.g. "game.overlap"), optional filter
/// (the actor the event is about) and named arguments for the event's outputs.
struct Event {
  std::string type;
  ActorRef target = 0;                 // only instances whose Self is target (0 = every instance)
  bool level = false;                  // with a target: the level graph (Self = none) receives it too
  std::string key;                     // event-specific match (custom event name, key code, action...)
  std::vector<std::pair<std::string, Value>> args;
  const Value* arg(std::string_view name) const;
};

class Runtime {
 public:
  explicit Runtime(Host& host, const Library& lib = library());
  ~Runtime();
  Host& host() { return host_; }
  const Library& lib() const { return lib_; }
  /// Compiles and adds a graph. Errors from check() disable the instance and are reported.
  Instance* add(const Graph& graph, ActorRef self, std::string name);
  void remove(ActorRef self);
  void clear();
  /// Begin Play for instances that have not begun.
  void begin();
  /// One fixed step: latent actions, timers, state machine timers, Tick events.
  void tick(f32 dt);
  /// Raises an event (synchronously runs the handlers).
  void fire(const Event& e);
  /// Custom event by name on every instance (or on `target`'s).
  void call(std::string_view name, ActorRef target = 0, std::vector<std::pair<std::string, Value>> args = {});
  Instance* find(ActorRef self);
  const std::vector<std::unique_ptr<Instance>>& instances() const { return instances_; }
  /// Node ids that ran recently (Studio flashes them while playing): instance name -> ids.
  struct Trace { std::string instance; u32 node = 0; f64 time = 0; };
  const std::vector<Trace>& trace() const { return trace_; }
  void clear_trace() { trace_.clear(); }
  bool tracing = false;
  /// Node executions in one tick before the runtime assumes an infinite loop.
  u32 budget = 200000;
  u64 executed() const { return executed_; }
  /// The host linked the native code of instance `id` (`function` = table index, < 0 failed).
  static void jit_ready(i32 id, i32 function);
  static Instance* instance_by_id(i32 id);
  /// Compile graphs to native code (on by default where the host can).
  bool jit = true;

 private:
  friend class Context;
  friend class Instance;
  struct Pending {                 // a suspended latent node
    Instance* inst = nullptr;
    int node = -1;
    f64 at = 0;                    // resume time
    int entry = 0;                 // entry passed back to the node
    u64 serial = 0;                // retriggering cancels older waits
  };
  void run_from(Instance& inst, int node, int entry);
  void enter_state(Instance& inst, std::size_t machine, u32 state);
  void check_transitions(Instance& inst, const std::string* event);
  bool over_budget(Instance& inst);
  Host& host_;
  const Library& lib_;
  std::vector<std::unique_ptr<Instance>> instances_;
  std::vector<Pending> pending_;
  std::vector<Trace> trace_;
  u64 executed_ = 0, tick_start_ = 0, serial_ = 0;
  int depth_ = 0;
  i32 counters_[4] = {0, 200000, 0, 0};  // generated code: nodes run, limit, tracing
  bool run_compiled(Instance& inst, int node, int entry);
};

/// What a node function sees while it runs.
class Context {
 public:
  Context(Runtime& rt, Instance& inst, int node, int entry);
  Runtime& runtime() { return rt_; }
  Host& host() { return rt_.host_; }
  Instance& instance() { return inst_; }
  ActorRef self() const { return inst_.self_; }
  const Node& node() const { return *inst_.nodes_[node_].node; }
  std::string param(std::string_view key, std::string_view fallback = {}) const { return node().param(key, fallback); }
  /// Which exec input started the node (0 for the first, kResume after a latent wait).
  int entry() const { return entry_; }
  /// Data input by index among *all* inputs (exec inputs included) or by pin id.
  Value in(std::size_t index);
  Value in(std::string_view pin);
  bool in_bool(std::string_view pin) { return in(pin).as_bool(); }
  f64 in_number(std::string_view pin) { return in(pin).as_number(); }
  i64 in_int(std::string_view pin) { return in(pin).as_int(); }
  Vec3 in_vector(std::string_view pin) { return in(pin).as_vector(); }
  std::string in_text(std::string_view pin) { return in(pin).as_text(); }
  ActorRef in_actor(std::string_view pin) { return in(pin).as_actor(); }
  std::size_t input_count() const { return inst_.nodes_[node_].in.size(); }
  const PinDef& input(std::size_t i) const { return inst_.nodes_[node_].in[i]; }
  void out(std::string_view pin, Value v);
  void out(std::size_t index, Value v);
  /// Exec output ordinal by pin id (for returning from run()).
  int exec(std::string_view pin) const;
  NodeState& state() { return inst_.nodes_[node_].state; }
  /// State slot k (0..3) of this node: shared with generated code (loops, gates, flip flops).
  f64& slot(int k) { return inst_.slots_[static_cast<std::size_t>(inst_.node_slot(node_) + k)]; }
  /// Latent: resume this node after `seconds` with entry kResume (or `entry`).
  int wait(f64 seconds, int entry = kResume);
  /// Cancels this node's pending waits (Retriggerable Delay).
  void cancel_waits();
  /// After the current chain ends, continue at this node's exec output (Sequence, loops).
  void then(int exec_output);
  /// Re-enter this node with `entry` once the chain started by the return value finishes (loops).
  void loop_back(int entry);
  static constexpr int kResume = 1000;

 private:
  friend class Runtime;
  friend class Instance;
  Runtime& rt_;
  Instance& inst_;
  int node_;
  int entry_;
  std::vector<std::pair<int, int>> continuations_;  // (node, entry) to run after the chain, LIFO
  bool compiled_ = false;                           // continuations go to the instance's stack
  int eval_depth_ = 0;
  Value pull(int node, int pin);
};

}  // namespace kern::logic
