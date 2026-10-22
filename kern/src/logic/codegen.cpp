// Kern Logic: graphs to machine code.
//
// Two back ends share one plan. compile_wasm() writes a WebAssembly module
// that the host compiles with the browser's optimising compiler and links into
// the engine's function table: the graph then runs as native code, calling the
// engine's C++ node functions directly (wasm-to-wasm calls, no script engine
// in between). generate_cpp() writes the same program as C++ source; compiled
// into a game with build.sh it replaces the interpreter for that graph.
//
// What is generated inline: control flow (Branch, Sequence, For / While,
// Do Once, Do N, Gate, Flip Flop), numeric variables (Get / Set / Add), maths,
// comparisons and boolean logic. Everything else (gameplay, text, vectors,
// latent nodes) is a call to the node's C++ function. Shared state (variables,
// loop counters, gates) lives in the instance's slots, so the interpreter, the
// JIT and exported C++ can take over from each other at any time.
//
// The program is a loop over a program counter (one case per node), which
// keeps any exec wiring (merges, cycles) valid; continuations of Sequence and
// loops go onto the instance's stack, exactly as in the interpreter.
#include <cmath>
#include <cstdio>
#include <cstring>

#include "kern/logic/logic.h"

namespace kern::logic {

namespace {

constexpr int kResume = Context::kResume;

// ---------------------------------------------------------------------------
// WebAssembly encoding

struct Bytes {
  std::vector<u8> b;
  void byte(u8 v) { b.push_back(v); }
  void uleb(u64 v) {
    do {
      u8 x = v & 0x7f;
      v >>= 7;
      if (v) x |= 0x80;
      b.push_back(x);
    } while (v);
  }
  void sleb(i64 v) {
    bool more = true;
    while (more) {
      u8 x = v & 0x7f;
      v >>= 7;
      if ((v == 0 && !(x & 0x40)) || (v == -1 && (x & 0x40))) more = false;
      else x |= 0x80;
      b.push_back(x);
    }
  }
  void f64v(f64 v) {
    u8 raw[8];
    std::memcpy(raw, &v, 8);
    for (u8 x : raw) b.push_back(x);
  }
  void name(const char* s) {
    uleb(std::strlen(s));
    for (const char* p = s; *p; ++p) b.push_back(static_cast<u8>(*p));
  }
  void append(const Bytes& o) { b.insert(b.end(), o.b.begin(), o.b.end()); }
  void section(u8 id, const Bytes& content) {
    byte(id);
    uleb(content.b.size());
    append(content);
  }
};

enum Op : u8 {
  kBlock = 0x02, kLoop = 0x03, kIf = 0x04, kElse = 0x05, kEnd = 0x0b, kBr = 0x0c, kBrTable = 0x0e, kReturn = 0x0f, kCall = 0x10, kSelect = 0x1b,
  kLocalGet = 0x20, kLocalSet = 0x21, kLocalTee = 0x22,
  kI32Load = 0x28, kF64Load = 0x2b, kI32Store = 0x36, kF64Store = 0x39,
  kI32Const = 0x41, kF64Const = 0x44,
  kI32Eqz = 0x45, kI32Eq = 0x46, kI32Ne = 0x47, kI32LtS = 0x48, kI32GtS = 0x4a, kI32LeS = 0x4c, kI32GeS = 0x4e, kI32GeU = 0x4f,
  kF64Eq = 0x61, kF64Ne = 0x62, kF64Lt = 0x63, kF64Gt = 0x64, kF64Le = 0x65, kF64Ge = 0x66,
  kI32Add = 0x6a, kI32Sub = 0x6b, kI32And = 0x71, kI32Or = 0x72, kI32Xor = 0x73, kI32Shl = 0x74,
  kF64Abs = 0x99, kF64Neg = 0x9a, kF64Floor = 0x9c, kF64Trunc = 0x9d, kF64Sqrt = 0x9f,
  kF64Add = 0xa0, kF64Sub = 0xa1, kF64Mul = 0xa2, kF64Div = 0xa3, kF64Min = 0xa4, kF64Max = 0xa5, kF64Copysign = 0xa6,
  kF64ConvertI32U = 0xb8,
};
constexpr u8 kVoid = 0x40, kTypeI32 = 0x7f, kTypeF64 = 0x7c;

// Locals of run(pc, entry).
constexpr u32 kPc = 0, kEntry = 1, kBase = 2, kR = 3, kSp = 4, kF0 = 5, kF1 = 6, kF2 = 7;
// Imported functions.
constexpr u32 kFnCallNode = 0, kFnReadPin = 1, kFnTrace = 2;

class WasmGen {
 public:
  explicit WasmGen(const Instance& inst) : in_(const_cast<Instance&>(inst)) {
    slots_ = reinterpret_cast<uintptr_t>(in_.slots());
    stack_ = reinterpret_cast<uintptr_t>(in_.stack());
    table_ = reinterpret_cast<uintptr_t>(in_.exec_table());
    counters_ = reinterpret_cast<uintptr_t>(in_.counters());
  }

  std::vector<u8> module() {
    Bytes code;
    body(code);
    Bytes m;
    for (u8 x : {0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00}) m.byte(x);
    {  // types
      Bytes t;
      t.uleb(4);
      auto fn = [&](std::initializer_list<u8> params, std::initializer_list<u8> results) {
        t.byte(0x60);
        t.uleb(params.size());
        for (u8 p : params) t.byte(p);
        t.uleb(results.size());
        for (u8 r : results) t.byte(r);
      };
      fn({kTypeI32, kTypeI32}, {kTypeI32});            // 0 run
      fn({kTypeI32, kTypeI32, kTypeI32}, {kTypeI32});  // 1 call_node
      fn({kTypeI32, kTypeI32, kTypeI32}, {kTypeF64});  // 2 read_pin
      fn({kTypeI32, kTypeI32}, {});                    // 3 trace
      m.section(1, t);
    }
    {  // imports
      Bytes im;
      im.uleb(4);
      auto func = [&](const char* name, u32 type) {
        im.name("env");
        im.name(name);
        im.byte(0x00);
        im.uleb(type);
      };
      func("call_node", 1);
      func("read_pin", 2);
      func("trace", 3);
      im.name("env");
      im.name("memory");
      im.byte(0x02);
      im.byte(0x00);  // limits: min only
      im.uleb(1);
      m.section(2, im);
    }
    {  // functions
      Bytes f;
      f.uleb(1);
      f.uleb(0);
      m.section(3, f);
    }
    {  // exports
      Bytes e;
      e.uleb(1);
      e.name("run");
      e.byte(0x00);
      e.uleb(3);
      m.section(7, e);
    }
    {  // code
      Bytes c;
      c.uleb(1);
      Bytes fn;
      fn.uleb(3);  // local groups
      fn.uleb(3);
      fn.byte(kTypeI32);  // base, r, sp
      fn.uleb(3);
      fn.byte(kTypeF64);  // f0..f2
      fn.uleb(0);
      fn.byte(kTypeI32);
      fn.append(code);
      c.uleb(fn.b.size());
      c.append(fn);
      m.section(10, c);
    }
    return m.b;
  }

 private:
  Instance& in_;
  uintptr_t slots_, stack_, table_, counters_;
  Bytes* o_ = nullptr;

  void op(u8 x) { o_->byte(x); }
  void i32c(i64 v) { op(kI32Const); o_->sleb(static_cast<i32>(v)); }
  void f64c(f64 v) { op(kF64Const); o_->f64v(v); }
  void lget(u32 l) { op(kLocalGet); o_->uleb(l); }
  void lset(u32 l) { op(kLocalSet); o_->uleb(l); }
  void ltee(u32 l) { op(kLocalTee); o_->uleb(l); }
  void mem(u8 o, u32 align, u32 offset) { op(o); o_->uleb(align); o_->uleb(offset); }
  void call(u32 f) { op(kCall); o_->uleb(f); }
  void br(u32 depth) { op(kBr); o_->uleb(depth); }
  void if_void() { op(kIf); op(kVoid); }
  void ret_i32(i32 v) { i32c(v); op(kReturn); }

  u32 slot_addr(int slot) const { return static_cast<u32>(slots_ + static_cast<uintptr_t>(slot) * 8); }
  void load_slot(int slot) { i32c(slot_addr(slot)); mem(kF64Load, 3, 0); }
  // value on the stack -> slot (uses kF0)
  void store_slot_from_f0(int slot) { i32c(slot_addr(slot)); lget(kF0); mem(kF64Store, 3, 0); }
  void bool_of_f64() { f64c(0); op(kF64Ne); }  // f64 -> i32 0/1
  void to_f64_from_bool() { op(kF64ConvertI32U); }
  void convert(PinType t) {
    if (t == PinType::integer) op(kF64Trunc);
    else if (t == PinType::boolean) { bool_of_f64(); to_f64_from_bool(); }
  }

  // ---- values ----
  /// Input `k` of node `node` as an f64 of the input's type.
  void value(int node, int k, int depth) {
    const PinDef& pin = in_.inputs_at(node)[static_cast<std::size_t>(k)];
    const Instance::SourceRef s = in_.source_of(node, k);
    if (s.node < 0) {
      f64c(in_.literal_of(node, k).as_number());
      return;
    }
    if (depth < 24 && in_.inline_pure(s.node)) {
      pure(s.node, s.pin, depth + 1);
      convert(pin.type);
      return;
    }
    if (const int slot = in_.out_slot(s.node, s.pin); slot >= 0 && in_.def_at(s.node)->kind != NodeKind::pure) {
      load_slot(slot);
      convert(pin.type);
      return;
    }
    // Anything else: the C++ side evaluates the input (events' outputs, text, vectors...).
    i32c(in_.id());
    i32c(node);
    i32c(k);
    call(kFnReadPin);
  }

  int input_index(int node, const char* id) const {
    const auto& ins = in_.inputs_at(node);
    for (std::size_t i = 0; i < ins.size(); ++i) if (ins[i].id == id) return static_cast<int>(i);
    return -1;
  }
  void value(int node, const char* id, int depth) {
    const int k = input_index(node, id);
    if (k < 0) f64c(0);
    else value(node, k, depth);
  }

  void pure(int node, int pin, int depth) {
    (void)pin;
    const std::string& t = in_.def_at(node)->type;
    auto two = [&](const char* a, const char* b) {
      value(node, a, depth);
      value(node, b, depth);
      lset(kF1);
      lset(kF0);
    };
    if (t == "var.get") { load_slot(in_.var_slot(in_.node_at(node).param("variable"))); return; }
    if (t == "value.float" || t == "value.int" || t == "value.bool") { value(node, "value", depth); return; }
    if (t == "math.add") { value(node, "a", depth); value(node, "b", depth); op(kF64Add); return; }
    if (t == "math.sub") { value(node, "a", depth); value(node, "b", depth); op(kF64Sub); return; }
    if (t == "math.mul") { value(node, "a", depth); value(node, "b", depth); op(kF64Mul); return; }
    if (t == "math.min") { value(node, "a", depth); value(node, "b", depth); op(kF64Min); return; }
    if (t == "math.max") { value(node, "a", depth); value(node, "b", depth); op(kF64Max); return; }
    if (t == "math.abs") { value(node, "a", depth); op(kF64Abs); return; }
    if (t == "math.negate") { value(node, "a", depth); op(kF64Neg); return; }
    if (t == "math.sqrt") { value(node, "a", depth); f64c(0); op(kF64Max); op(kF64Sqrt); return; }
    if (t == "math.floor") { value(node, "a", depth); op(kF64Floor); return; }
    if (t == "math.round") {  // llround: half away from zero
      value(node, "a", depth);
      lset(kF0);
      lget(kF0); op(kF64Abs); f64c(0.5); op(kF64Add); op(kF64Floor); lget(kF0); op(kF64Copysign);
      return;
    }
    if (t == "math.div" || t == "math.mod") {
      two("a", "b");
      if (t == "math.div") { lget(kF0); lget(kF1); op(kF64Div); }
      else { lget(kF0); lget(kF1); lget(kF0); lget(kF1); op(kF64Div); op(kF64Trunc); op(kF64Mul); op(kF64Sub); }
      f64c(0);
      lget(kF1); f64c(0); op(kF64Ne);
      op(kSelect);
      return;
    }
    if (t == "math.clamp") {
      value(node, "value", depth); value(node, "min", depth); value(node, "max", depth);
      lset(kF2); lset(kF1); lset(kF0);
      lget(kF1); lget(kF2); op(kF64Min);
      lget(kF0); lget(kF1); lget(kF2); op(kF64Max); op(kF64Min);
      op(kF64Max);
      return;
    }
    if (t == "math.lerp") {
      value(node, "a", depth); value(node, "b", depth); value(node, "alpha", depth);
      lset(kF2); lset(kF1); lset(kF0);
      lget(kF0); lget(kF1); lget(kF0); op(kF64Sub); lget(kF2); op(kF64Mul); op(kF64Add);
      return;
    }
    if (t == "math.compare") {
      two("a", "b");
      const std::string o = in_.node_at(node).param("op", "==");
      if (o == "==" || o == "!=") { lget(kF0); lget(kF1); op(kF64Sub); op(kF64Abs); f64c(1e-9); op(o == "==" ? kF64Le : kF64Gt); }
      else { lget(kF0); lget(kF1); op(o == "<" ? kF64Lt : o == "<=" ? kF64Le : o == ">" ? kF64Gt : kF64Ge); }
      to_f64_from_bool();
      return;
    }
    if (t == "logic.not") { value(node, "a", depth); f64c(0); op(kF64Eq); to_f64_from_bool(); return; }
    if (t == "logic.and" || t == "logic.or" || t == "logic.xor") {
      value(node, "a", depth); bool_of_f64();
      value(node, "b", depth); bool_of_f64();
      op(t == "logic.and" ? kI32And : t == "logic.or" ? kI32Or : kI32Xor);
      to_f64_from_bool();
      return;
    }
    if (t == "logic.select") {
      value(node, "a", depth); value(node, "b", depth); value(node, "pick", depth); bool_of_f64();
      op(kSelect);
      return;
    }
    f64c(0);
  }

  // ---- exec helpers ----
  void next(int node, int ordinal) {
    const auto [target, entry] = in_.exec_target(node, ordinal);
    i32c(target);
    lset(kPc);
    i32c(entry);
    lset(kEntry);
  }
  void stop() { i32c(-1); lset(kPc); }
  void push(int pc, int entry) {
    const u32 st = static_cast<u32>(stack_);
    i32c(st); mem(kI32Load, 2, 0); lset(kSp);
    lget(kSp); i32c(CodeLayout::kStack); op(kI32GeS); if_void(); ret_i32(2); op(kEnd);
    i32c(st + 4); lget(kSp); i32c(3); op(kI32Shl); op(kI32Add); i32c(pc); mem(kI32Store, 2, 0);
    i32c(st + 4); lget(kSp); i32c(3); op(kI32Shl); op(kI32Add); i32c(entry); mem(kI32Store, 2, 4);
    i32c(st); lget(kSp); i32c(1); op(kI32Add); mem(kI32Store, 2, 0);
  }
  void push_exec(int node, int ordinal) {
    const auto [target, entry] = in_.exec_target(node, ordinal);
    if (target >= 0) push(target, entry);
  }
  void slot_cond_set(int slot, f64 v) { f64c(v); lset(kF0); store_slot_from_f0(slot); }

  void node_code(int i) {
    const NodeDef* d = in_.def_at(i);
    if (!d) { stop(); return; }
    const std::string& t = d->type;
    const int base = in_.node_slot(i);
    if (!in_.inline_node(i)) {
      // C++ node: call it, then continue at the exec output it chose.
      i32c(in_.id()); lget(kPc); lget(kEntry); call(kFnCallNode); lset(kR);
      lget(kR); i32c(in_.exec_outputs(i)); op(kI32GeU);
      if_void();
      stop();
      op(kElse);
      const u32 rows = static_cast<u32>(table_) + static_cast<u32>(in_.exec_table_offset(i)) * 4;
      i32c(rows); lget(kR); i32c(3); op(kI32Shl); op(kI32Add); mem(kI32Load, 2, 0); lset(kPc);
      i32c(rows); lget(kR); i32c(3); op(kI32Shl); op(kI32Add); mem(kI32Load, 2, 4); lset(kEntry);
      op(kEnd);
      return;
    }
    if (d->kind == NodeKind::event) { next(i, 0); return; }
    if (t == "flow.branch") {
      value(i, "condition", 0); bool_of_f64();
      if_void(); next(i, 0); op(kElse); next(i, 1); op(kEnd);
      return;
    }
    if (t == "flow.sequence") {
      for (int o = in_.exec_outputs(i) - 1; o >= 1; --o) push_exec(i, o);
      next(i, 0);
      return;
    }
    if (t == "flow.for") {
      lget(kEntry); i32c(kResume); op(kI32Ne);
      if_void();
      value(i, "first", 0); lset(kF0); store_slot_from_f0(base + 0);
      value(i, "last", 0); lset(kF0); store_slot_from_f0(base + 1);
      op(kElse);
      load_slot(base + 0); f64c(1); op(kF64Add); lset(kF0); store_slot_from_f0(base + 0);
      op(kEnd);
      load_slot(base + 0); load_slot(base + 1); op(kF64Gt);
      if_void();
      next(i, 1);  // Completed (exec outputs: body, completed)
      op(kElse);
      load_slot(base + 0); lset(kF0); store_slot_from_f0(in_.out_slot(i, 1));
      push(i, kResume);
      next(i, 0);
      op(kEnd);
      return;
    }
    if (t == "flow.while") {
      value(i, "condition", 0); bool_of_f64();
      if_void(); push(i, kResume); next(i, 0); op(kElse); next(i, 1); op(kEnd);
      return;
    }
    if (t == "flow.do_once") {
      lget(kEntry); i32c(1); op(kI32Eq);
      if_void();
      slot_cond_set(base + 0, 0); slot_cond_set(base + 1, 1); stop();
      op(kElse);
      load_slot(base + 1); f64c(0); op(kF64Eq);
      if_void(); slot_cond_set(base + 1, 1); value(i, "closed", 0); lset(kF0); store_slot_from_f0(base + 0); op(kEnd);
      load_slot(base + 0); bool_of_f64();
      if_void(); stop(); op(kElse); slot_cond_set(base + 0, 1); next(i, 0); op(kEnd);
      op(kEnd);
      return;
    }
    if (t == "flow.do_n") {
      lget(kEntry); i32c(1); op(kI32Eq);
      if_void();
      slot_cond_set(base + 0, 0); stop();
      op(kElse);
      load_slot(base + 0); value(i, "n", 0); op(kF64Ge);
      if_void(); stop();
      op(kElse);
      load_slot(base + 0); f64c(1); op(kF64Add); lset(kF0); store_slot_from_f0(base + 0); store_slot_from_f0(in_.out_slot(i, 1));
      next(i, 0);
      op(kEnd);
      op(kEnd);
      return;
    }
    if (t == "flow.gate") {
      load_slot(base + 1); f64c(0); op(kF64Eq);
      if_void(); slot_cond_set(base + 1, 1); value(i, "closed", 0); f64c(0); op(kF64Eq); to_f64_from_bool(); lset(kF0); store_slot_from_f0(base + 0); op(kEnd);
      stop();
      lget(kEntry); i32c(0); op(kI32Eq);
      if_void(); load_slot(base + 0); bool_of_f64(); if_void(); next(i, 0); op(kEnd); op(kEnd);
      lget(kEntry); i32c(1); op(kI32Eq); if_void(); slot_cond_set(base + 0, 1); op(kEnd);
      lget(kEntry); i32c(2); op(kI32Eq); if_void(); slot_cond_set(base + 0, 0); op(kEnd);
      lget(kEntry); i32c(3); op(kI32Eq);
      if_void(); load_slot(base + 0); f64c(0); op(kF64Eq); to_f64_from_bool(); lset(kF0); store_slot_from_f0(base + 0); op(kEnd);
      return;
    }
    if (t == "flow.flip_flop") {
      load_slot(base + 0); f64c(0); op(kF64Eq); to_f64_from_bool(); lset(kF0);
      store_slot_from_f0(base + 0);
      if (const int o = in_.out_slot(i, 2); o >= 0) store_slot_from_f0(o);
      lget(kF0); bool_of_f64();
      if_void(); next(i, 0); op(kElse); next(i, 1); op(kEnd);
      return;
    }
    if (t == "var.set" || t == "var.add") {
      const Variable* v = in_.graph().variable(in_.node_at(i).param("variable"));
      const int vs = in_.var_slot(in_.node_at(i).param("variable"));
      if (t == "var.set") value(i, "value", 0);
      else { load_slot(vs); value(i, "amount", 0); op(kF64Add); }
      convert(v ? v->type : PinType::number);
      lset(kF0);
      store_slot_from_f0(vs);
      for (int o = 0; o < static_cast<int>(in_.outputs_at(i).size()); ++o)
        if (const int s = in_.out_slot(i, o); s >= 0) store_slot_from_f0(s);
      next(i, 0);
      return;
    }
    stop();
  }

  void body(Bytes& code) {
    o_ = &code;
    const int n = static_cast<int>(in_.node_count());
    const u32 st = static_cast<u32>(stack_), ct = static_cast<u32>(counters_);
    i32c(st); mem(kI32Load, 2, 0); lset(kBase);
    op(kLoop); op(kVoid);
    // pc < 0: continue with a continuation, or return when ours are used up.
    lget(kPc); i32c(0); op(kI32LtS);
    if_void();
    i32c(st); mem(kI32Load, 2, 0); ltee(kSp); lget(kBase); op(kI32LeS);
    if_void(); ret_i32(0); op(kEnd);
    lget(kSp); i32c(1); op(kI32Sub); lset(kSp);
    i32c(st); lget(kSp); mem(kI32Store, 2, 0);
    i32c(st + 4); lget(kSp); i32c(3); op(kI32Shl); op(kI32Add); mem(kI32Load, 2, 0); lset(kPc);
    i32c(st + 4); lget(kSp); i32c(3); op(kI32Shl); op(kI32Add); mem(kI32Load, 2, 4); lset(kEntry);
    op(kEnd);
    // Step budget (infinite loop guard) and tracing for the editor.
    i32c(ct); i32c(ct); mem(kI32Load, 2, 0); i32c(1); op(kI32Add); ltee(kR); mem(kI32Store, 2, 0);
    lget(kR); i32c(ct + 4); mem(kI32Load, 2, 0); op(kI32GtS); if_void(); ret_i32(1); op(kEnd);
    i32c(ct + 8); mem(kI32Load, 2, 0); if_void(); i32c(in_.id()); lget(kPc); call(kFnTrace); op(kEnd);
    // Dispatch: block per node, br_table on pc.
    op(kBlock); op(kVoid);  // exit
    for (int k = n - 1; k >= 0; --k) { op(kBlock); op(kVoid); }
    lget(kPc);
    op(kBrTable);
    o_->uleb(static_cast<u32>(n));
    for (int k = 0; k < n; ++k) o_->uleb(static_cast<u32>(k));
    o_->uleb(static_cast<u32>(n));
    for (int k = 0; k < n; ++k) {
      op(kEnd);  // block k: its code follows
      node_code(k);
      br(static_cast<u32>(n - k));  // back to the loop
    }
    op(kEnd);  // exit block: unknown pc
    ret_i32(0);
    op(kEnd);  // loop
    i32c(0);
    op(kEnd);  // function
  }
};

// ---------------------------------------------------------------------------
// C++ source

class CppGen {
 public:
  explicit CppGen(const Instance& inst) : in_(const_cast<Instance&>(inst)) {}

  std::string source(const std::string& name) {
    std::string fn = "kern_logic_";
    for (char ch : name) fn += std::isalnum(static_cast<unsigned char>(ch)) ? ch : '_';
    char hash[32];
    std::snprintf(hash, sizeof hash, "0x%016llxull", static_cast<unsigned long long>(graph_hash(in_.graph())));
    std::string s;
    s += "// Generated by Kern Studio from the logic graph \"" + name + "\".\n";
    s += "// Compile it into your game (kern/build.sh --studio out.wasm this_file.cpp): the graph then\n";
    s += "// runs as this native code instead of the interpreter. Regenerate after editing the graph.\n";
    s += "#include <cmath>\n\n#include \"kern/logic/logic.h\"\n\nnamespace {\n\nusing namespace kern;\nusing namespace kern::logic;\n\n";
    s += "int " + fn + "(void* self, int pc, int entry) {\n";
    s += "  Instance& g = *static_cast<Instance*>(self);\n";
    s += "  f64* s = g.slots();\n  i32* st = g.stack();\n  i32* et = g.exec_table();\n  i32* ct = g.counters();\n  const i32 base = st[0];\n";
    s += "  auto push = [&](int p, int e) { if (st[0] >= CodeLayout::kStack) return false; st[1 + 2 * st[0]] = p; st[2 + 2 * st[0]] = e; ++st[0]; return true; };\n";
    s += "  (void)et; (void)push; (void)entry;\n";
    s += "  for (;;) {\n";
    s += "    if (pc < 0) {\n      if (st[0] <= base) return 0;\n      --st[0];\n      pc = st[1 + 2 * st[0]];\n      entry = st[2 + 2 * st[0]];\n    }\n";
    s += "    if (++ct[0] > ct[1]) return 1;\n";
    s += "    switch (pc) {\n";
    for (int i = 0; i < static_cast<int>(in_.node_count()); ++i) {
      const NodeDef* d = in_.def_at(i);
      s += "      case " + std::to_string(i) + ": {  // " + (d ? d->label : std::string("?")) + "\n";
      body_ = &s;
      node_code(i);
      s += "        continue;\n      }\n";
    }
    s += "      default: return 0;\n    }\n  }\n}\n\n";
    s += "const bool kRegistered = (register_native_graph(" + std::string(hash) + ", " + fn + "), true);\n\n}  // namespace\n";
    return s;
  }

 private:
  Instance& in_;
  std::string* body_ = nullptr;
  void line(const std::string& l) { *body_ += "        " + l + "\n"; }
  static std::string num(f64 v) {
    if (!std::isfinite(v)) return "0.0";
    char b[64];
    std::snprintf(b, sizeof b, "%.17g", v);
    std::string r = b;
    if (r.find_first_of(".eE") == std::string::npos) r += ".0";
    return r;
  }
  static std::string conv(const std::string& e, PinType t) {
    if (t == PinType::integer) return "std::trunc(" + e + ")";
    if (t == PinType::boolean) return "((" + e + ") != 0.0 ? 1.0 : 0.0)";
    return e;
  }
  int input_index(int node, const char* id) const {
    const auto& ins = in_.inputs_at(node);
    for (std::size_t i = 0; i < ins.size(); ++i) if (ins[i].id == id) return static_cast<int>(i);
    return -1;
  }
  std::string value(int node, const char* id, int depth = 0) {
    const int k = input_index(node, id);
    return k < 0 ? "0.0" : value(node, k, depth);
  }
  std::string value(int node, int k, int depth) {
    const PinDef& pin = in_.inputs_at(node)[static_cast<std::size_t>(k)];
    const Instance::SourceRef src = in_.source_of(node, k);
    if (src.node < 0) return num(in_.literal_of(node, k).as_number());
    if (depth < 24 && in_.inline_pure(src.node)) return conv(pure(src.node, depth + 1), pin.type);
    if (const int slot = in_.out_slot(src.node, src.pin); slot >= 0 && in_.def_at(src.node)->kind != NodeKind::pure)
      return conv("s[" + std::to_string(slot) + "]", pin.type);
    return "g.read_pin(" + std::to_string(node) + ", " + std::to_string(k) + ")";
  }
  std::string pure(int node, int depth) {
    const std::string& t = in_.def_at(node)->type;
    auto v = [&](const char* id) { return value(node, id, depth); };
    if (t == "var.get") return "s[" + std::to_string(in_.var_slot(in_.node_at(node).param("variable"))) + "]";
    if (t == "value.float" || t == "value.int" || t == "value.bool") return v("value");
    if (t == "math.add") return "(" + v("a") + " + " + v("b") + ")";
    if (t == "math.sub") return "(" + v("a") + " - " + v("b") + ")";
    if (t == "math.mul") return "(" + v("a") + " * " + v("b") + ")";
    if (t == "math.min") return "std::fmin(" + v("a") + ", " + v("b") + ")";
    if (t == "math.max") return "std::fmax(" + v("a") + ", " + v("b") + ")";
    if (t == "math.abs") return "std::fabs(" + v("a") + ")";
    if (t == "math.negate") return "(-" + v("a") + ")";
    if (t == "math.sqrt") return "std::sqrt(std::fmax(0.0, " + v("a") + "))";
    if (t == "math.floor") return "std::floor(" + v("a") + ")";
    if (t == "math.round") return "static_cast<f64>(std::llround(" + v("a") + "))";
    if (t == "math.div") return "[&] { const f64 a = " + v("a") + ", b = " + v("b") + "; return b != 0.0 ? a / b : 0.0; }()";
    if (t == "math.mod") return "[&] { const f64 a = " + v("a") + ", b = " + v("b") + "; return b != 0.0 ? std::fmod(a, b) : 0.0; }()";
    if (t == "math.clamp")
      return "[&] { const f64 x = " + v("value") + ", lo = " + v("min") + ", hi = " + v("max") + "; return std::fmax(std::fmin(lo, hi), std::fmin(x, std::fmax(lo, hi))); }()";
    if (t == "math.lerp") return "[&] { const f64 a = " + v("a") + "; return a + (" + v("b") + " - a) * " + v("alpha") + "; }()";
    if (t == "math.compare") {
      const std::string o = in_.node_at(node).param("op", "==");
      if (o == "==") return "(std::fabs(" + v("a") + " - " + v("b") + ") <= 1e-9 ? 1.0 : 0.0)";
      if (o == "!=") return "(std::fabs(" + v("a") + " - " + v("b") + ") > 1e-9 ? 1.0 : 0.0)";
      return "(" + v("a") + " " + o + " " + v("b") + " ? 1.0 : 0.0)";
    }
    if (t == "logic.not") return "(" + v("a") + " == 0.0 ? 1.0 : 0.0)";
    if (t == "logic.and") return "((" + v("a") + " != 0.0) && (" + v("b") + " != 0.0) ? 1.0 : 0.0)";
    if (t == "logic.or") return "((" + v("a") + " != 0.0) || (" + v("b") + " != 0.0) ? 1.0 : 0.0)";
    if (t == "logic.xor") return "((" + v("a") + " != 0.0) != (" + v("b") + " != 0.0) ? 1.0 : 0.0)";
    if (t == "logic.select") return "(" + v("pick") + " != 0.0 ? " + v("a") + " : " + v("b") + ")";
    return "0.0";
  }
  std::string next(int node, int ordinal) {
    const auto [target, entry] = in_.exec_target(node, ordinal);
    return "pc = " + std::to_string(target) + "; entry = " + std::to_string(entry) + ";";
  }
  std::string push_exec(int node, int ordinal) {
    const auto [target, entry] = in_.exec_target(node, ordinal);
    return target < 0 ? std::string() : "if (!push(" + std::to_string(target) + ", " + std::to_string(entry) + ")) return 2;";
  }
  void node_code(int i) {
    const NodeDef* d = in_.def_at(i);
    if (!d) { line("pc = -1;"); return; }
    const std::string& t = d->type;
    const std::string b = std::to_string(in_.node_slot(i));
    auto S = [&](int k) { return "s[" + std::to_string(in_.node_slot(i) + k) + "]"; };
    if (!in_.inline_node(i)) {
      line("const int r = g.call_node(" + std::to_string(i) + ", entry);");
      line("if (r < 0 || r >= " + std::to_string(in_.exec_outputs(i)) + ") { pc = -1; continue; }");
      line("pc = et[" + std::to_string(in_.exec_table_offset(i)) + " + 2 * r]; entry = et[" + std::to_string(in_.exec_table_offset(i)) + " + 2 * r + 1];");
      return;
    }
    if (d->kind == NodeKind::event) { line(next(i, 0)); return; }
    if (t == "flow.branch") { line("if (" + value(i, "condition") + " != 0.0) { " + next(i, 0) + " } else { " + next(i, 1) + " }"); return; }
    if (t == "flow.sequence") {
      for (int o = in_.exec_outputs(i) - 1; o >= 1; --o) if (auto p = push_exec(i, o); !p.empty()) line(p);
      line(next(i, 0));
      return;
    }
    if (t == "flow.for") {
      line("if (entry != " + std::to_string(kResume) + ") { " + S(0) + " = " + value(i, "first") + "; " + S(1) + " = " + value(i, "last") + "; } else " + S(0) + " += 1.0;");
      line("if (" + S(0) + " > " + S(1) + ") { " + next(i, 1) + " } else { s[" + std::to_string(in_.out_slot(i, 1)) + "] = " + S(0) + "; if (!push(" + std::to_string(i) + ", " +
           std::to_string(kResume) + ")) return 2; " + next(i, 0) + " }");
      return;
    }
    if (t == "flow.while") {
      line("if (" + value(i, "condition") + " != 0.0) { if (!push(" + std::to_string(i) + ", " + std::to_string(kResume) + ")) return 2; " + next(i, 0) + " } else { " + next(i, 1) + " }");
      return;
    }
    if (t == "flow.do_once") {
      line("if (entry == 1) { " + S(0) + " = 0.0; " + S(1) + " = 1.0; pc = -1; continue; }");
      line("if (" + S(1) + " == 0.0) { " + S(1) + " = 1.0; " + S(0) + " = " + value(i, "closed") + "; }");
      line("if (" + S(0) + " != 0.0) { pc = -1; } else { " + S(0) + " = 1.0; " + next(i, 0) + " }");
      return;
    }
    if (t == "flow.do_n") {
      line("if (entry == 1) { " + S(0) + " = 0.0; pc = -1; continue; }");
      line("if (" + S(0) + " >= " + value(i, "n") + ") { pc = -1; } else { " + S(0) + " += 1.0; s[" + std::to_string(in_.out_slot(i, 1)) + "] = " + S(0) + "; " + next(i, 0) + " }");
      return;
    }
    if (t == "flow.gate") {
      line("if (" + S(1) + " == 0.0) { " + S(1) + " = 1.0; " + S(0) + " = " + value(i, "closed") + " != 0.0 ? 0.0 : 1.0; }");
      line("pc = -1;");
      line("if (entry == 0 && " + S(0) + " != 0.0) { " + next(i, 0) + " }");
      line("if (entry == 1) " + S(0) + " = 1.0;");
      line("if (entry == 2) " + S(0) + " = 0.0;");
      line("if (entry == 3) " + S(0) + " = " + S(0) + " != 0.0 ? 0.0 : 1.0;");
      return;
    }
    if (t == "flow.flip_flop") {
      line(S(0) + " = " + S(0) + " != 0.0 ? 0.0 : 1.0;");
      if (const int o = in_.out_slot(i, 2); o >= 0) line("s[" + std::to_string(o) + "] = " + S(0) + ";");
      line("if (" + S(0) + " != 0.0) { " + next(i, 0) + " } else { " + next(i, 1) + " }");
      return;
    }
    if (t == "var.set" || t == "var.add") {
      const Variable* v = in_.graph().variable(in_.node_at(i).param("variable"));
      const std::string vs = "s[" + std::to_string(in_.var_slot(in_.node_at(i).param("variable"))) + "]";
      const std::string e = t == "var.set" ? value(i, "value") : "(" + vs + " + " + value(i, "amount") + ")";
      line(vs + " = " + conv(e, v ? v->type : PinType::number) + ";");
      for (int o = 0; o < static_cast<int>(in_.outputs_at(i).size()); ++o)
        if (const int sl = in_.out_slot(i, o); sl >= 0) line("s[" + std::to_string(sl) + "] = " + vs + ";");
      line(next(i, 0));
      return;
    }
    (void)b;
    line("pc = -1;");
  }
};

}  // namespace

std::vector<u8> compile_wasm(const Instance& inst) { return WasmGen(inst).module(); }

std::string generate_cpp(const Instance& inst, const std::string& name) { return CppGen(inst).source(name); }

}  // namespace kern::logic
