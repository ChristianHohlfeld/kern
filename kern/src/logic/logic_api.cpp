// C ABI between the engine and the native code the JIT links in: generated
// modules import these engine functions (wasm-to-wasm calls) and the host
// reports when a module is ready.
#include "kern/api/c_api.h"
#include "kern/logic/logic.h"

#if defined(__wasm__)
extern "C" {
__attribute__((import_module("kern_env"), import_name("logic_jit"))) kern::i32 kern_env_logic_jit(const kern::u8* bytes, kern::u32 length, kern::i32 id);
__attribute__((import_module("kern_env"), import_name("logic_jit_release"))) void kern_env_logic_jit_release(kern::i32 function);
}
#endif

namespace kern::logic {

bool platform_jit_compile(const std::vector<u8>& module, i32 id) {
#if defined(__wasm__)
  return !module.empty() && kern_env_logic_jit(module.data(), static_cast<u32>(module.size()), id) != 0;
#else
  (void)module;
  (void)id;
  return false;
#endif
}

void platform_jit_release(i32 function) {
#if defined(__wasm__)
  kern_env_logic_jit_release(function);
#else
  (void)function;
#endif
}

bool Host::jit_compile(const std::vector<u8>& module, i32 id) { return platform_jit_compile(module, id); }
void Host::jit_release(i32 function) { platform_jit_release(function); }

}  // namespace kern::logic

using kern::f64;
using kern::i32;
using kern::logic::Instance;
using kern::logic::Runtime;

KERN_EXPORT(kern_logic_call_node) i32 kern_logic_call_node(i32 id, i32 node, i32 entry) {
  Instance* inst = Runtime::instance_by_id(id);
  return inst ? inst->call_node(node, entry) : kern::logic::kStop;
}

KERN_EXPORT(kern_logic_read_pin) f64 kern_logic_read_pin(i32 id, i32 node, i32 pin) {
  Instance* inst = Runtime::instance_by_id(id);
  return inst ? inst->read_pin(node, pin) : 0.0;
}

KERN_EXPORT(kern_logic_trace) void kern_logic_trace(i32 id, i32 node) {
  if (Instance* inst = Runtime::instance_by_id(id)) inst->trace_node(node);
}

KERN_EXPORT(kern_logic_jit_ready) void kern_logic_jit_ready(i32 id, i32 function) { Runtime::jit_ready(id, function); }
