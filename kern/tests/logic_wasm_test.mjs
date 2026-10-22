// Runs the logic cases inside the engine's WebAssembly build, once in the interpreter and
// once JIT-compiled (the generated module is compiled by V8 and linked into the engine's
// function table, exactly as in the browser). Results must be identical.
//   node kern/tests/logic_wasm_test.mjs <logic_test.wasm>
import fs from "node:fs";
import {loadKern} from "../web/kern.mjs";

const kern = await loadKern({bytes: fs.readFileSync(process.argv[2])});
const x = kern.exports;
const text = ptr => { const m = new Uint8Array(x.memory.buffer); let e = ptr; while (m[e]) e++; return new TextDecoder().decode(m.subarray(ptr, e)); };
const settle = async () => { for (let i = 0; i < 50; i++) await new Promise(r => setTimeout(r, 2)); };
let failures = 0;
for (let i = 0; i < x.logic_test_count(); i++) {
  x.logic_test_prepare(i, 0);
  const interpreted = text(x.logic_test_play());
  const pending = x.logic_test_prepare(i, 1);
  await settle();
  const tier = text(x.logic_test_tier());
  const compiled = text(x.logic_test_play());
  const name = interpreted.split("\n")[0] ? `case ${i}` : `case ${i}`;
  if (pending !== 1 || tier !== "compiled (WebAssembly)") { console.log(`FAIL ${name}: runs in "${tier}" (compile requested: ${pending})`); failures++; }
  if (compiled !== interpreted) { console.log(`FAIL ${name}: JIT results differ\n--- interpreter\n${interpreted}--- JIT\n${compiled}`); failures++; }
  else console.log(`ok   ${name}: identical in the interpreter and as compiled WebAssembly`);
}
// Speed: one million loop iterations with arithmetic and a variable.
x.logic_test_bench_prepare(0);
const ti = x.logic_test_bench_run(), vi = x.logic_test_bench_value();
x.logic_test_bench_prepare(1);
await settle();
x.logic_test_bench_run();  // warm up
x.logic_test_bench_prepare(1);
await settle();
const tj = x.logic_test_bench_run(), vj = x.logic_test_bench_value();
if (vi !== vj) { console.log(`FAIL benchmark results differ: ${vi} vs ${vj}`); failures++; }
console.log(`1,000,000 loop iterations: interpreter ${ti.toFixed(1)} ms, compiled ${tj.toFixed(1)} ms (${(ti / tj).toFixed(1)}x faster)`);
console.log(failures ? `${failures} failures` : "logic JIT: all cases match");
process.exit(failures ? 1 : 0);
