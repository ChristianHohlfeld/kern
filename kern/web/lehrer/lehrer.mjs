// Kern Lehrer page: collectors in Web Workers, one learner worker, a chart (docs/lehrer.md).
const $ = id => document.getElementById(id);
const here = new URL(".", import.meta.url);
const wasmUrl = new URL("../../../generated/kern_lehrer.wasm", here);
const workerUrl = new URL("./worker.mjs", here);

const cores = Math.max(1, (navigator.hardwareConcurrency || 2) - 1);
$("workers").value = Math.min(cores, 8);

let module = null;
let running = false, paused = false;
let learner = null, collectors = [];
let policy = null, iteration = 0, history = [], onnx = null, wakeLock = null, steps = 24;

function status(text, error = false) {
  $("status").textContent = text;
  $("status").classList.toggle("err", error);
}

function call(worker, message, expect, transfer = []) {
  return new Promise((resolve, reject) => {
    worker.onmessage = ({ data }) => {
      if (data.type === "error") reject(Error(data.message));
      else if (data.type === expect) resolve(data);
    };
    worker.onerror = e => reject(Error(e.message || "worker failed"));
    worker.postMessage(message, transfer);
  });
}

async function spawn() {
  const w = new Worker(workerUrl, { type: "module" });
  await call(w, { type: "load", module }, "loaded");
  return w;
}

async function start() {
  if (running) return;
  const task = $("task").value;
  const workers = Math.max(1, Math.min(64, Number($("workers").value) || 1));
  const envs = Math.max(1, Math.min(256, Number($("envs").value) || 8));
  running = true;
  paused = false;
  iteration = 0;
  history = [];
  onnx = null;
  $("start").disabled = true;
  $("pause").disabled = false;
  $("download").disabled = true;
  status(`Starte ${workers} Worker mit je ${envs} Enten ...`);
  try {
    wakeLock = await navigator.wakeLock?.request("screen").catch(() => null);
    const seed = Math.max(1, Number($("seed").value) || 1);
    learner = await spawn();
    collectors = await Promise.all(Array.from({ length: workers }, () => spawn()));
    // Worker i steps ducks i*envs .. (i+1)*envs-1 of one run: the same run as natively with as many
    // ducks in all (bit for bit, docs/lehrer.md).
    await Promise.all(collectors.map((w, i) => call(w, { type: "collector", task, envs, seed, first: i * envs }, "ready")));
    const config = {};
    if (Number($("initstd").value) > 0) config.initStd = Number($("initstd").value);
    if (Number($("lr").value) > 0) config.learningRate = Number($("lr").value);
    const made = await call(learner, { type: "learner", task, seed, config }, "policy");
    policy = made.policy;
    steps = made.steps || 24;
    // First imitation of the demonstration (the jump's recipe), round by round, then practice.
    let left = made.imitationLeft || 0;
    while (running && left > 0) {
      const round = made.imitationLeft - left;
      status(`Nachahmen: Runde ${round + 1} von ${made.imitationLeft} ...`);
      // The round's episodes spread over the collectors (worker i: its share, in episode order).
      const episodes = made.imitationEpisodes, k = collectors.length;
      const shares = await Promise.all(collectors.map((w, i) => {
        const first = Math.floor(i * episodes / k), count = Math.floor((i + 1) * episodes / k) - first;
        return call(w, { type: "dagger", policy: policy.slice(0), round, first, count }, "share").then(d => d.share);
      }));
      const d = await call(learner, { type: "imitate", shares }, "imitated", shares);
      policy = d.policy;
      left = d.left;
      const r = d.report;
      showJump(r);
      status(`Nachahmen: Runde ${r.round} von ${r.rounds}, ${r.samples.toLocaleString()} Beispiele, Fehler ${r.error.toFixed(4)}`);
    }
    if (made.imitationLeft && running) await exportOnnx(task);
    loop(task).catch(error => { status(error.message, true); stop(); });
  } catch (error) {
    status(error.message, true);
    stop();
  }
}

async function loop(task) {
  const total = Math.max(1, Number($("iterations").value) || 1);
  while (running && iteration < total) {
    if (paused) { await new Promise(r => setTimeout(r, 200)); continue; }
    const t0 = performance.now();
    const rollouts = await Promise.all(collectors.map(w =>
      call(w, { type: "collect", policy: policy.slice(0), iteration: iteration + 1, steps }, "rollout").then(d => d.rollout)));
    const learned = await call(learner, { type: "learn", rollouts }, "learned", rollouts);
    policy = learned.policy;
    iteration = learned.stats.iteration;
    const seconds = (performance.now() - t0) / 1000;
    show(learned.stats, seconds, collectors.length * Number($("envs").value) * steps);
    if (iteration % 25 === 0 || iteration === total) {
      if (task === "jump") showJump((await call(learner, { type: "evaluate", episodes: 5 }, "evaluated")).report);
      await exportOnnx(task);
    }
  }
  if (running) {
    status(`Fertig: ${iteration} Iterationen. Das Netz herunterladen oder direkt in Kern Studio nutzen.`);
    await exportOnnx(task);
    stop(true);
  }
}

async function exportOnnx(task) {
  const d = await call(learner, { type: "export" }, "onnx");
  onnx = new Blob([d.onnx], { type: "application/octet-stream" });
  onnx.task = task;
  $("download").disabled = false;
  await saveForStudio(`${task}.onnx`, d.onnx).catch(() => {});
}

/** Kern Studio on this device picks the network up (IndexedDB "kern-lehrer", key = file name), at
 *  once in open tabs (BroadcastChannel): the duck's "jump" move runs jump.onnx. */
function saveForStudio(file, bytes) {
  return new Promise((resolve, reject) => {
    const open = indexedDB.open("kern-lehrer", 1);
    open.onupgradeneeded = () => open.result.createObjectStore("policies");
    open.onerror = () => reject(open.error);
    open.onsuccess = () => {
      const tx = open.result.transaction("policies", "readwrite");
      tx.objectStore("policies").put(bytes, file);
      tx.oncomplete = () => {
        try { new BroadcastChannel("kern-lehrer").postMessage({ file }); } catch { /* no channel */ }
        $("saved").textContent = `Für Kern Studio gespeichert als ${file} (Iteration ${iteration}).`;
        resolve();
      };
      tx.onerror = () => reject(tx.error);
    };
  });
}

function show(s, seconds, steps) {
  $("s-it").textContent = s.iteration;
  $("s-steps").textContent = Math.round(s.steps).toLocaleString();
  $("s-sps").textContent = Math.round(steps / seconds).toLocaleString();
  if (s.episodes) {
    $("s-ret").textContent = s.meanReturn.toFixed(2);
    $("s-len").textContent = (s.meanLength * 0.02).toFixed(2);
    history.push({ it: s.iteration, ret: s.meanReturn, len: s.meanLength * 0.02 });
  }
  $("s-std").textContent = s.meanStd.toFixed(3);
  status(`Lernt: Iteration ${s.iteration}, sammeln ${s.collectSeconds.toFixed(2)} s, lernen ${s.learnSeconds.toFixed(2)} s`);
  draw();
}

/** How the network jumps now (5 episodes on the clean robot). */
function showJump(r) {
  if (!r) return;
  $("s-jump").textContent = `${r.jumps}/${r.episodes}`;
  $("s-height").textContent = r.heightMm.toFixed(1);
  $("s-flight").textContent = Math.round(r.flightS * 1000);
}

function draw() {
  const c = $("chart"), dpr = devicePixelRatio || 1;
  const w = c.clientWidth, h = c.clientHeight;
  c.width = w * dpr;
  c.height = h * dpr;
  const g = c.getContext("2d");
  g.scale(dpr, dpr);
  g.clearRect(0, 0, w, h);
  if (history.length < 2) return;
  const pad = 28;
  const css = getComputedStyle(document.documentElement);
  const line = (key, color) => {
    const v = history.map(p => p[key]);
    const lo = Math.min(...v), hi = Math.max(...v), span = hi - lo || 1;
    g.strokeStyle = color;
    g.lineWidth = 2;
    g.beginPath();
    history.forEach((p, i) => {
      const x = pad + (w - 2 * pad) * (i / (history.length - 1));
      const y = h - pad - (h - 2 * pad) * ((p[key] - lo) / span);
      i ? g.lineTo(x, y) : g.moveTo(x, y);
    });
    g.stroke();
    return [lo, hi];
  };
  g.strokeStyle = css.getPropertyValue("--line2");
  g.lineWidth = 1;
  g.strokeRect(pad, pad, w - 2 * pad, h - 2 * pad);
  const [lo, hi] = line("ret", css.getPropertyValue("--play").trim());
  line("len", css.getPropertyValue("--accent").trim());
  g.fillStyle = css.getPropertyValue("--dim").trim();
  g.font = "11px system-ui";
  g.fillText(hi.toFixed(1), 4, pad + 4);
  g.fillText(lo.toFixed(1), 4, h - pad);
  g.fillText(`Iteration ${history[history.length - 1].it}`, w - pad - 80, h - 8);
}

function stop(done = false) {
  running = false;
  for (const w of [learner, ...collectors]) w?.terminate();
  learner = null;
  collectors = [];
  wakeLock?.release?.().catch(() => {});
  wakeLock = null;
  $("start").disabled = false;
  $("pause").disabled = true;
  $("pause").textContent = "Pause";
  if (!done && !$("status").classList.contains("err")) status("Angehalten.");
}

$("start").onclick = start;
$("pause").onclick = () => {
  paused = !paused;
  $("pause").textContent = paused ? "Weiter" : "Pause";
  status(paused ? "Pausiert." : "Lernt ...");
};
$("download").onclick = () => {
  if (!onnx) return;
  const a = document.createElement("a");
  a.href = URL.createObjectURL(onnx);
  a.download = `${onnx.task || "kern"}_kern.onnx`;
  a.click();
  setTimeout(() => URL.revokeObjectURL(a.href), 1000);
};
addEventListener("resize", draw);

(async () => {
  try {
    module = await WebAssembly.compileStreaming(fetch(wasmUrl)).catch(async () => WebAssembly.compile(await (await fetch(wasmUrl)).arrayBuffer()));
    status(`Bereit. ${navigator.hardwareConcurrency || "?"} Kerne auf diesem Gerät.`);
  } catch (error) {
    status(`Der Lehrer konnte nicht laden: ${error.message}`, true);
    $("start").disabled = true;
  }
})();
