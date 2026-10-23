// The trainer's WebAssembly build (kern/build.sh --train): a collector and the learner run one PPO
// iteration through the C ABI the browser's workers use, and the exported ONNX reads back.
//   node kern/tests/train_wasm_test.mjs generated/kern_train.wasm
import fs from "fs";
import { loadKern } from "../web/kern.mjs";

const file = process.argv[2];
const kern = await loadKern({ bytes: fs.readFileSync(file) });
const e = kern.exports, enc = new TextEncoder();
const put = bytes => { const p = e.kern_alloc(bytes.length); kern.u8.set(bytes, p); return p; };
const take = ptr => kern.u8.slice(ptr, ptr + e.kern_train_result_length());
const check = (ok, what) => { if (!ok) { console.error("FAIL " + what); process.exit(1); } };

const task = enc.encode("jump"), config = enc.encode('{"steps":8}');
const tp = put(task), cp = put(config);
check(e.kern_train_create(tp, task.length, 2, 5) === 1, "collector");
check(e.kern_train_learner(tp, task.length, cp, config.length, 3) === 1, "learner");
const policy = take(e.kern_train_policy());
check(policy.length > 100000, "policy");
check(e.kern_train_set_policy(put(policy), policy.length >> 2) === 1, "set policy");
e.kern_train_progress(1, 0);
const t0 = performance.now();
const rollout = take(e.kern_train_collect(8));
const ms = performance.now() - t0;
check(rollout.length > 1000, "rollout");
const framed = new Uint8Array(4 + rollout.length);
new DataView(framed.buffer).setUint32(0, rollout.length, true);
framed.set(rollout, 4);
const sp = e.kern_train_learn(put(framed), framed.length, 1);
check(sp !== 0, "learn");
const stats = JSON.parse(kern.string(sp, e.kern_train_result_length()));
check(stats.iteration === 1 && stats.steps === 16, "stats " + JSON.stringify(stats));
const onnx = take(e.kern_train_export_onnx());
check(onnx.length > 50000 && new TextDecoder().decode(onnx).includes("actions"), "onnx");
console.log(`trainer wasm ok: 16 duck steps in ${ms.toFixed(0)} ms, learn ${stats.learnSeconds.toFixed(2)} s, onnx ${onnx.length} bytes`);
