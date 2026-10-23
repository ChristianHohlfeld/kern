// A Lehrer Web Worker (kern/web/lehrer/): one Kern module (kern_lehrer.wasm) per worker. A worker is
// a collector (its own ducks, the policy it is handed, returns rollouts) or the learner (PPO).
import { loadKern } from "../kern.mjs";

let kern = null;
const enc = new TextEncoder();

/** Copies bytes into the module's memory; the caller frees the pointer. */
function put(bytes) {
  const ptr = kern.exports.kern_alloc(bytes.byteLength);
  kern.u8.set(new Uint8Array(bytes.buffer, bytes.byteOffset, bytes.byteLength), ptr);
  return ptr;
}
function withBytes(bytes, fn) {
  const ptr = put(bytes);
  try { return fn(ptr, bytes.byteLength); } finally { kern.exports.kern_free(ptr); }
}
function withText(text, fn) { return withBytes(enc.encode(text), fn); }
function resultBytes(ptr) {
  if (!ptr) return null;
  return kern.u8.slice(ptr, ptr + kern.exports.kern_lehrer_result_length());
}

self.onmessage = async ({ data: m }) => {
  try {
    if (m.type === "load") {
      kern = await loadKern({ module: m.module });
      self.postMessage({ type: "loaded" });
    } else if (m.type === "collector") {
      const ok = withText(m.task, (p, n) => kern.exports.kern_lehrer_create(p, n, m.envs, m.seed, m.first));
      self.postMessage({ type: "ready", ok: !!ok });
    } else if (m.type === "collect") {
      kern.exports.kern_lehrer_progress(m.iteration);
      const policy = new Float32Array(m.policy);
      const ok = withBytes(policy, (p, n) => kern.exports.kern_lehrer_set_policy(p, n >> 2));
      if (!ok) throw Error("the policy did not load");
      const t0 = performance.now();
      const rollout = resultBytes(kern.exports.kern_lehrer_collect(m.steps));
      if (!rollout) throw Error("collect failed");
      self.postMessage({ type: "rollout", rollout: rollout.buffer, ms: performance.now() - t0 }, [rollout.buffer]);
    } else if (m.type === "learner") {
      const ok = withText(m.task, (tp, tn) => withText(JSON.stringify(m.config || {}), (cp, cn) => kern.exports.kern_lehrer_learner(tp, tn, cp, cn, m.seed)));
      if (!ok) throw Error("unknown task " + m.task);
      self.postMessage({ type: "policy", policy: policyBuffer(), steps: kern.exports.kern_lehrer_steps(), imitationLeft: kern.exports.kern_lehrer_imitation_left(), imitationEpisodes: kern.exports.kern_lehrer_imitation_episodes() }, []);
    } else if (m.type === "dagger") {
      // A collector's share of an imitation round: its episodes, labelled by the demonstration.
      const policy = new Float32Array(m.policy);
      if (!withBytes(policy, (p, n) => kern.exports.kern_lehrer_set_policy(p, n >> 2))) throw Error("the policy did not load");
      const share = resultBytes(kern.exports.kern_lehrer_dagger(m.round, m.first, m.count));
      if (!share) throw Error("imitation episodes failed");
      self.postMessage({ type: "share", share: share.buffer }, [share.buffer]);
    } else if (m.type === "imitate") {
      // One round of imitation (the demonstration, docs/lehrer.md) from the collectors' shares, in
      // episode order; then the network as it is now.
      let total = 0;
      for (const b of m.shares) total += 4 + b.byteLength;
      const all = new Uint8Array(total);
      const view = new DataView(all.buffer);
      let at = 0;
      for (const b of m.shares) {
        view.setUint32(at, b.byteLength, true);
        all.set(new Uint8Array(b), at + 4);
        at += 4 + b.byteLength;
      }
      const p = withBytes(all, (q, n) => kern.exports.kern_lehrer_imitate(q, n, m.shares.length));
      if (!p) throw Error("imitation failed");
      const report = JSON.parse(kern.string(p, kern.exports.kern_lehrer_result_length()));
      const policy = policyBuffer();
      self.postMessage({ type: "imitated", report, policy, left: kern.exports.kern_lehrer_imitation_left() }, [policy]);
    } else if (m.type === "evaluate") {
      const p = kern.exports.kern_lehrer_evaluate(m.episodes || 5);
      self.postMessage({ type: "evaluated", report: p ? JSON.parse(kern.string(p, kern.exports.kern_lehrer_result_length())) : null });
    } else if (m.type === "learn") {
      // The rollouts one after the other, each with its byte size in front (u32).
      let total = 0;
      for (const r of m.rollouts) total += 4 + r.byteLength;
      const all = new Uint8Array(total);
      const view = new DataView(all.buffer);
      let at = 0;
      for (const r of m.rollouts) {
        view.setUint32(at, r.byteLength, true);
        all.set(new Uint8Array(r), at + 4);
        at += 4 + r.byteLength;
      }
      const statsPtr = withBytes(all, (p, n) => kern.exports.kern_lehrer_learn(p, n, m.rollouts.length));
      if (!statsPtr) throw Error("learn failed");
      const stats = JSON.parse(kern.string(statsPtr, kern.exports.kern_lehrer_result_length()));
      const policy = policyBuffer();
      self.postMessage({ type: "learned", stats, policy }, [policy]);
    } else if (m.type === "export") {
      const onnx = resultBytes(kern.exports.kern_lehrer_export_onnx());
      self.postMessage({ type: "onnx", onnx: onnx.buffer }, [onnx.buffer]);
    }
  } catch (error) {
    self.postMessage({ type: "error", message: String(error?.message || error) });
  }
};

function policyBuffer() {
  const ptr = kern.exports.kern_lehrer_policy();
  const bytes = kern.u8.slice(ptr, ptr + kern.exports.kern_lehrer_result_length());
  return bytes.buffer;
}
