// The Lehrer's WebAssembly build (kern/build.sh --lehrer): a collector and the learner run one PPO
// iteration through the C ABI the browser's workers use, and the exported ONNX reads back.
//   node kern/tests/lehrer_wasm_test.mjs generated/kern_lehrer.wasm
import fs from "fs";
import { loadKern } from "../web/kern.mjs";

const file = process.argv[2];
const kern = await loadKern({ bytes: fs.readFileSync(file) });
const e = kern.exports, enc = new TextEncoder();
const put = bytes => { const p = e.kern_alloc(bytes.length); kern.u8.set(bytes, p); return p; };
const take = ptr => kern.u8.slice(ptr, ptr + e.kern_lehrer_result_length());
const check = (ok, what) => { if (!ok) { console.error("FAIL " + what); process.exit(1); } };

const task = enc.encode("jump"), config = enc.encode('{"steps":8}');
const tp = put(task), cp = put(config);
check(e.kern_lehrer_create(tp, task.length, 2, 5, 0) === 1, "collector");
check(e.kern_lehrer_learner(tp, task.length, cp, config.length, 3) === 1, "learner");
const policy = take(e.kern_lehrer_policy());
check(policy.length > 100000, "policy");
check(e.kern_lehrer_set_policy(put(policy), policy.length >> 2) === 1, "set policy");
e.kern_lehrer_progress(1);
const t0 = performance.now();
const rollout = take(e.kern_lehrer_collect(8));
const ms = performance.now() - t0;
check(rollout.length > 1000, "rollout");
const framed = new Uint8Array(4 + rollout.length);
new DataView(framed.buffer).setUint32(0, rollout.length, true);
framed.set(rollout, 4);
const sp = e.kern_lehrer_learn(put(framed), framed.length, 1);
check(sp !== 0, "learn");
const stats = JSON.parse(kern.string(sp, e.kern_lehrer_result_length()));
check(stats.iteration === 1 && stats.steps === 16, "stats " + JSON.stringify(stats));
const onnx = take(e.kern_lehrer_export_onnx());
check(onnx.length > 50000 && new TextDecoder().decode(onnx).includes("actions"), "onnx");
// Bit-identical with every other build (tests/determinism_fingerprint.txt).
{
  const p = e.kern_lehrer_fingerprint(400), n = e.kern_lehrer_result_length() / 8;
  const got = new BigUint64Array(kern.memory.buffer, p, n);
  const want = fs.readFileSync(new URL("./determinism_fingerprint.txt", import.meta.url), "utf8").trim().split("\n").map(l => BigInt("0x" + l.split(" ")[1]));
  check(n === want.length, "fingerprint length");
  const first = want.findIndex((w, i) => w !== got[i]);
  check(first < 0, "deterministic: differs from tick " + first);
}
console.log(`Lehrer wasm ok (bit-identical with native): 16 duck steps in ${ms.toFixed(0)} ms, learn ${stats.learnSeconds.toFixed(2)} s, onnx ${onnx.length} bytes`);
