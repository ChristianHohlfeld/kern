// ---------------------------------------------------------------------------
// Kern Logic editor: Blueprint-style visual scripting in Kern Studio.
//
// The graph lives in the engine (C++): every change is a Studio command
// (logicAdd, logicConnect, logicMove...) and one undo step; this module only
// draws what the engine describes (state.logic) and turns gestures into
// commands. Look: the digitalisierungsplanung.de state editor (dark navy
// canvas, rounded nodes, orthogonal wires on a 24 px grid); feel: Unreal's
// Blueprint editor (right-click search, drag a wire into empty space to get
// only the nodes that fit, Alt+click breaks links).

const LG_TYPE_COLORS = {exec: "#f2f5f9", bool: "#e5484d", int: "#2dd4bf", float: "#9be36b", vector: "#fcd34d", text: "#f9a8d4", actor: "#38bdf8"};
const LG_KIND_COLORS = {event: "#b4232f", action: "#1e4f8a", pure: "#2f5a3a", flow: "#4b4f5c"};
const LG_GRID = 24;
const lgEsc = s => String(s ?? "").replace(/[&<>"']/g, c => ({"&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;"}[c]));
const lgHex = n => "#" + (n >>> 0).toString(16).padStart(6, "0");
const lgNum = v => (typeof v === "number" ? (Math.abs(v) >= 1e4 || Number.isInteger(v) ? String(v) : String(+v.toFixed(4))) : String(v ?? ""));

export class LogicEditor {
  /** host: element to fill; studio: {engine, state(), flushUi(), openMenu(entries, x, y), toast(text)} */
  constructor(host, studio) {
    this.host = host;
    this.studio = studio;
    this.engine = studio.engine;
    this.data = null;
    this.key = "";
    this.views = new Map();          // owner:view -> {x, y, zoom}
    this.selection = new Set();
    this.drag = null;
    this.palette = null;
    this.ranAt = new Map();          // node id -> time it last ran (glow)
    this.build();
  }

  get view() {
    const k = `${this.data?.owner ?? 0}:${this.data?.view ?? "graph"}`;
    if (!this.views.has(k)) this.views.set(k, {x: 80, y: 80, zoom: 1, fitted: false});
    return this.views.get(k);
  }

  cmd(c) {
    const r = this.engine.command(c);
    this.studio.flushUi();
    if (r && r.ok === false && r.error) this.studio.toast(r.error);
    return r;
  }

  // ---- skeleton -------------------------------------------------------------
  build() {
    this.host.innerHTML = `
      <div class="lg-head">
        <button class="lg-btn lg-back" title="Back to the viewport (Esc)">◀ Viewport</button>
        <select class="lg-owner" title="Whose logic"></select>
        <div class="lg-tabs"></div>
        <button class="lg-btn lg-addmachine" title="Add a state machine">+ State Machine</button>
        <div class="lg-spacer"></div>
        <button class="lg-btn lg-templates" title="Ready-made graphs">Templates ▾</button>
        <button class="lg-btn lg-help" title="How it works">?</button>
        <div class="lg-status"></div>
      </div>
      <div class="lg-body">
        <aside class="lg-side">
          <div class="lg-sec"><div class="lg-sec-h">Variables<button class="lg-mini lg-addvar" title="Add a variable">+</button></div><div class="lg-vars"></div></div>
          <div class="lg-sec"><div class="lg-sec-h">Events</div><div class="lg-events"></div></div>
          <div class="lg-sec"><div class="lg-sec-h">Problems</div><div class="lg-issues"></div></div>
          <div class="lg-tip">Right-click the canvas to add nodes. Drag from a pin to wire it; drop on empty space for nodes that fit. Alt+click a pin breaks its wires. Middle / right drag pans, wheel zooms.</div>
        </aside>
        <div class="lg-canvas" tabindex="0">
          <div class="lg-world"><svg class="lg-wires"></svg><div class="lg-nodes"></div></div>
          <svg class="lg-temp"></svg>
          <div class="lg-marquee" hidden></div>
          <div class="lg-empty" hidden></div>
          <div class="lg-inspector" hidden></div>
        </div>
      </div>`;
    const $ = s => this.host.querySelector(s);
    this.el = {
      owner: $(".lg-owner"), tabs: $(".lg-tabs"), status: $(".lg-status"), vars: $(".lg-vars"), events: $(".lg-events"), issues: $(".lg-issues"),
      canvas: $(".lg-canvas"), world: $(".lg-world"), wires: $(".lg-wires"), nodes: $(".lg-nodes"), temp: $(".lg-temp"), marquee: $(".lg-marquee"),
      empty: $(".lg-empty"), inspector: $(".lg-inspector"),
    };
    $(".lg-back").onclick = () => this.cmd({cmd: "logicClose"});
    this.el.owner.onchange = () => this.cmd({cmd: "logicOpen", owner: +this.el.owner.value});
    $(".lg-addmachine").onclick = () => {
      const r = this.cmd({cmd: "logicMachine", op: "add"});
      if (r?.id) this.cmd({cmd: "logicView", view: "m:" + r.id});
    };
    $(".lg-addvar").onclick = () => this.addVariable();
    $(".lg-templates").onclick = e => {
      const r = e.currentTarget.getBoundingClientRect();
      const t = (label, template) => ({label, run: () => this.cmd({cmd: "logicTemplate", template})});
      this.studio.openMenu([
        {header: "Add to this graph"},
        t("Door that opens on Interact", "door"),
        t("Score zone (overlap +10)", "score_zone"),
        t("Spawner (a cube every 3 s)", "spawner"),
        t("Traffic light (state machine)", "traffic_light"),
      ], r.left, r.bottom + 2);
    };
    $(".lg-help").onclick = () => this.showHelp();
    this.initCanvas();
  }

  showHelp() {
    this.studio.toast("Events (red) start chains, white wires say what runs next, coloured wires carry values. " +
      "Right-click: add nodes · drag a pin: wire it · Alt+click a pin: break wires · Del: delete · Ctrl+C/V/D: copy, paste, duplicate · F: frame all · Ctrl+Z: undo. " +
      "Graphs run as compiled C++ on the server / host; everything they change reaches every player.", [], 14000);
  }

  // ---- render ---------------------------------------------------------------
  render(data) {
    this.data = data;
    if (!data) return;
    const owners = data.owners || [];
    const ownerKey = JSON.stringify(owners) + data.owner;
    if (ownerKey !== this.ownerKey) {
      this.ownerKey = ownerKey;
      const current = owners.some(o => o.id === data.owner) ? "" : `<option value="${data.owner}">${lgEsc(data.ownerName)} (new)</option>`;
      this.el.owner.innerHTML = current + owners.map(o => `<option value="${o.id}"${o.id === data.owner ? " selected" : ""}>${lgEsc(o.name)}${o.nodes ? ` · ${o.nodes}` : ""}</option>`).join("");
    }
    // Tabs: event graph + machines.
    const machines = data.machines || [];
    const tabs = [{id: "graph", label: "Event Graph"}, ...machines.map(m => ({id: "m:" + m.id, label: m.name}))];
    if (data.view !== "graph" && !machines.some(m => "m:" + m.id === data.view)) data.view = "graph";
    this.el.tabs.innerHTML = tabs.map(t => `<button class="lg-tab${t.id === data.view ? " active" : ""}" data-view="${t.id}">${lgEsc(t.label)}</button>`).join("");
    for (const b of this.el.tabs.querySelectorAll("[data-view]")) {
      b.onclick = () => this.cmd({cmd: "logicView", view: b.dataset.view});
      if (b.dataset.view.startsWith("m:")) b.ondblclick = () => this.renameMachine(+b.dataset.view.slice(2));
      if (b.dataset.view.startsWith("m:")) b.oncontextmenu = e => {
        e.preventDefault();
        const id = +b.dataset.view.slice(2);
        this.studio.openMenu([{label: "Rename...", run: () => this.renameMachine(id)}, {label: "Delete State Machine", run: () => this.cmd({cmd: "logicMachine", op: "remove", machine: id})}], e.clientX, e.clientY);
      };
    }
    this.renderSide(data);
    const key = JSON.stringify([data.owner, data.view, data.nodes, data.links, data.machines, data.issues]);
    if (key !== this.key) {
      this.key = key;
      if (data.view === "graph") this.renderGraph(data);
      else this.renderMachine(data, machines.find(m => "m:" + m.id === data.view));
    }
    this.applyView();
    const errors = (data.issues || []).filter(i => i.error).length, warns = (data.issues || []).length - errors;
    this.el.status.className = "lg-status " + (errors ? "bad" : "good");
    this.el.status.textContent = errors ? `${errors} error${errors > 1 ? "s" : ""}` : warns ? `Compiled · ${warns} note${warns > 1 ? "s" : ""}` : "Compiled";
    this.el.status.title = errors ? "Fix the errors (red nodes) - Play refuses graphs with errors" : "The graph is checked and ready; it runs as compiled code when you press Play";
  }

  renderSide(data) {
    const live = this.trace?.variables || {};
    const vk = JSON.stringify([data.variables, live]);
    if (vk !== this.varKey) {
      this.varKey = vk;
      this.el.vars.innerHTML = (data.variables || []).map(v => `
        <div class="lg-var" draggable="true" data-var="${lgEsc(v.name)}" title="Drag into the graph (hold Ctrl for Set)">
          <i class="lg-dot" style="background:${LG_TYPE_COLORS[v.type]}"></i>
          <span class="lg-var-name">${lgEsc(v.name)}</span>
          <span class="lg-var-type">${v.type}</span>
          ${v.name in live ? `<span class="lg-live" title="Value while playing">${lgEsc(lgNum(Array.isArray(live[v.name]) ? live[v.name].map(lgNum).join(", ") : live[v.name]))}</span>` : ""}
          <button class="lg-mini" data-get title="Add a Get node">Get</button><button class="lg-mini" data-set title="Add a Set node">Set</button>
        </div>`).join("") || `<div class="lg-dim">No variables yet</div>`;
      for (const row of this.el.vars.querySelectorAll(".lg-var")) {
        const name = row.dataset.var;
        row.querySelector("[data-get]").onclick = () => this.addAtCenter("var.get", {variable: name});
        row.querySelector("[data-set]").onclick = () => this.addAtCenter("var.set", {variable: name});
        row.ondragstart = e => e.dataTransfer.setData("text/x-kern-var", name);
        row.oncontextmenu = e => { e.preventDefault(); this.variableMenu(data.variables.find(v => v.name === name), e.clientX, e.clientY); };
        row.ondblclick = () => this.variableMenu(data.variables.find(v => v.name === name), row.getBoundingClientRect().left, row.getBoundingClientRect().bottom);
      }
    }
    const ek = JSON.stringify(data.events);
    if (ek !== this.eventKey) {
      this.eventKey = ek;
      this.el.events.innerHTML = (data.events || []).map(e => `<div class="lg-ev"><span>${lgEsc(e)}</span><button class="lg-mini" data-on="${lgEsc(e)}">On</button><button class="lg-mini" data-call="${lgEsc(e)}">Call</button></div>`).join("") ||
        `<div class="lg-dim">Custom events you add appear here</div>`;
      for (const b of this.el.events.querySelectorAll("[data-on]")) b.onclick = () => this.addAtCenter("event.custom", {name: b.dataset.on});
      for (const b of this.el.events.querySelectorAll("[data-call]")) b.onclick = () => this.addAtCenter("flow.call", {name: b.dataset.call});
    }
    const ik = JSON.stringify(data.issues);
    if (ik !== this.issueKey) {
      this.issueKey = ik;
      this.el.issues.innerHTML = (data.issues || []).map((i, k) => `<div class="lg-issue ${i.error ? "err" : "warn"}" data-k="${k}">${lgEsc(i.text)}</div>`).join("") || `<div class="lg-dim">None</div>`;
      for (const row of this.el.issues.querySelectorAll(".lg-issue")) row.onclick = () => { const i = data.issues[+row.dataset.k]; if (i.node) this.focusNode(i.node); };
    }
  }

  // ---- event graph ------------------------------------------------------------
  renderGraph(data) {
    const nodes = data.nodes || [];
    this.el.empty.hidden = nodes.length > 0;
    this.el.empty.innerHTML = `<div><b>${lgEsc(data.ownerName)}</b> has no logic yet.</div><div>Right-click to add a node, or start with</div>
      <div class="lg-starts"><button data-t="event.begin">Begin Play</button><button data-t="game.interact">On Interact</button><button data-t="game.overlap_begin">On Begin Overlap</button><button data-t="event.tick">Event Tick</button><button data-tpl>Templates…</button></div>`;
    for (const b of this.el.empty.querySelectorAll("[data-t]")) b.onclick = () => this.addAtCenter(b.dataset.t);
    this.el.empty.querySelector("[data-tpl]").onclick = () => this.host.querySelector(".lg-templates").click();
    const actors = this.studio.state()?.actors || [];
    this.el.nodes.innerHTML = nodes.map(n => this.nodeHtml(n, actors)).join("");
    for (const el of this.el.nodes.querySelectorAll(".lg-node")) {
      const id = +el.dataset.id;
      el.classList.toggle("selected", this.selection.has(id));
    }
    this.bindNodeInputs();
    // Pins are laid out by the browser; wires follow in the next frame.
    requestAnimationFrame(() => this.drawWires());
    const v = this.view;
    if (!v.fitted && nodes.length) { v.fitted = true; requestAnimationFrame(() => this.frameAll()); }
  }

  nodeHtml(n, actors) {
    const head = n.color ? lgHex(n.color) : LG_KIND_COLORS[n.kind] || LG_KIND_COLORS.action;
    const pin = (p, dir) => {
      const color = LG_TYPE_COLORS[p.type] || "#aaa";
      const dot = p.type === "exec" ? `<svg class="lg-pin exec${p.linked ? " linked" : ""}" viewBox="0 0 12 12"><path d="M2 1.5h4l4.5 4.5L6 10.5H2z" fill="${p.linked ? color : "none"}" stroke="${color}" stroke-width="1.4"/></svg>`
        : `<i class="lg-pin${p.linked ? " linked" : ""}" style="--c:${color}"></i>`;
      let literal = "";
      if (dir === "in" && p.type !== "exec" && !p.linked) literal = this.literalHtml(n, p, actors);
      const label = p.label ? `<span class="lg-pl">${lgEsc(p.label)}</span>` : "";
      return `<div class="lg-row ${dir}" data-pin="${lgEsc(p.id)}" data-dir="${dir}" data-type="${p.type}">${dir === "in" ? dot + label + literal : label + dot}</div>`;
    };
    const params = (n.params || []).map(p => `<label class="lg-param"><span>${lgEsc(p.label)}</span>${this.paramHtml(n, p)}</label>`).join("");
    const ins = n.inputs.map(p => pin(p, "in")).join(""), outs = n.outputs.map(p => pin(p, "out")).join("");
    const pure = n.kind === "pure";
    return `<div class="lg-node ${n.kind}${n.error ? " error" : n.issues ? " warn" : ""}${pure ? " pure" : ""}" data-id="${n.id}" style="left:${n.x}px;top:${n.y}px"
        title="${lgEsc((n.issues ? n.issues.join("\n") + "\n\n" : "") + (n.help || ""))}">
      <div class="lg-nh" style="--h:${head}">${n.kind === "event" ? `<span class="lg-ico">◆</span>` : n.latent ? `<span class="lg-ico">◷</span>` : n.kind === "pure" ? `<span class="lg-ico">ƒ</span>` : ""}<span class="lg-title">${lgEsc(n.title)}</span></div>
      ${n.comment ? `<div class="lg-comment">${lgEsc(n.comment)}</div>` : ""}
      ${params ? `<div class="lg-params">${params}</div>` : ""}
      <div class="lg-pins"><div class="lg-col">${ins}</div><div class="lg-col right">${outs}</div></div>
    </div>`;
  }

  literalHtml(n, p, actors) {
    const v = p.value;
    const attr = `data-lit="${lgEsc(p.id)}" data-node="${n.id}" data-t="${p.type}"`;
    switch (p.type) {
      case "bool": return `<input type="checkbox" class="lg-lit" ${attr}${v ? " checked" : ""}>`;
      case "int":
      case "float": return `<input class="lg-lit num" ${attr} value="${lgEsc(lgNum(v))}" spellcheck="false">`;
      case "text": return `<input class="lg-lit txt" ${attr} value="${lgEsc(v)}" spellcheck="false">`;
      case "vector": return `<span class="lg-vec" ${attr}>${[0, 1, 2].map(i => `<input class="lg-lit num v" data-i="${i}" value="${lgEsc(lgNum(Array.isArray(v) ? v[i] : 0))}" spellcheck="false">`).join("")}</span>`;
      case "actor": return `<select class="lg-lit act" ${attr}><option value="0">${p.id === "target" || p.id === "a" ? "Self" : "None"}</option>${actors.map(([id, name]) => `<option value="${id}"${id === v ? " selected" : ""}>${lgEsc(name)}</option>`).join("")}</select>`;
      default: return "";
    }
  }

  paramHtml(n, p) {
    const attr = `data-param="${lgEsc(p.key)}" data-node="${n.id}"`;
    const choices = p.choices || [];
    if (p.kind === "choice" || p.kind === "variable" || p.kind === "machine" || p.kind === "state" || p.kind === "sound") {
      const has = choices.some(([v]) => v === p.value);
      return `<select class="lg-par" ${attr}>${has ? "" : `<option value="${lgEsc(p.value)}" selected>${lgEsc(p.value || "(none)")}</option>`}${choices.map(([v, l]) => `<option value="${lgEsc(v)}"${v === p.value ? " selected" : ""}>${lgEsc(l)}</option>`).join("")}</select>`;
    }
    if (p.kind === "event") {
      const list = `lgev-${n.id}`;
      return `<input class="lg-par" ${attr} value="${lgEsc(p.value)}" list="${list}" spellcheck="false"><datalist id="${list}">${choices.map(([v]) => `<option value="${lgEsc(v)}">`).join("")}</datalist>`;
    }
    return `<input class="lg-par" ${attr} value="${lgEsc(p.value)}" spellcheck="false">`;
  }

  bindNodeInputs() {
    const send = (node, pin, value) => this.cmd({cmd: "logicValue", node, pin, value});
    for (const el of this.el.nodes.querySelectorAll(".lg-lit")) {
      const node = +(el.dataset.node || el.closest("[data-node]")?.dataset.node);
      el.addEventListener("pointerdown", e => e.stopPropagation());
      el.addEventListener("keydown", e => { e.stopPropagation(); if (e.key === "Enter") el.blur(); });
      if (el.classList.contains("v")) {
        el.onchange = () => {
          const box = el.closest(".lg-vec");
          send(+box.dataset.node, box.dataset.lit, [...box.querySelectorAll("input")].map(i => +i.value || 0));
        };
        continue;
      }
      const pin = el.dataset.lit;
      if (el.type === "checkbox") el.onchange = () => send(node, pin, el.checked);
      else if (el.tagName === "SELECT") el.onchange = () => send(node, pin, +el.value);
      else el.onchange = () => send(node, pin, el.dataset.t === "text" ? el.value : (+el.value || 0));
    }
    for (const el of this.el.nodes.querySelectorAll(".lg-par")) {
      el.addEventListener("pointerdown", e => e.stopPropagation());
      el.addEventListener("keydown", e => { e.stopPropagation(); if (e.key === "Enter") el.blur(); });
      el.onchange = () => this.cmd({cmd: "logicParam", node: +el.dataset.node, key: el.dataset.param, value: el.value});
    }
  }

  // Pin position in world (graph) coordinates.
  pinPoint(node, pin, dir) {
    const el = this.el.nodes.querySelector(`.lg-node[data-id="${node}"] .lg-row.${dir}[data-pin="${CSS.escape(pin)}"] .lg-pin`);
    if (!el) return null;
    const r = el.getBoundingClientRect(), w = this.el.world.getBoundingClientRect(), z = this.view.zoom;
    return {x: (r.left + r.width / 2 - w.left) / z, y: (r.top + r.height / 2 - w.top) / z, el};
  }

  /** The user's editor routes wires orthogonally with rounded corners; long horizontal runs stay readable. */
  wirePath(a, b) {
    const dx = Math.max(24, Math.min(60, Math.abs(b.x - a.x) / 2));
    if (b.x - a.x >= 2 * dx) {
      const mx = Math.round((a.x + b.x) / 2 / 4) * 4, r = Math.min(10, Math.abs(b.y - a.y) / 2, Math.abs(mx - a.x));
      if (Math.abs(b.y - a.y) < 1) return `M${a.x},${a.y}H${b.x}`;
      const s = b.y > a.y ? 1 : -1;
      return `M${a.x},${a.y}H${mx - r}Q${mx},${a.y} ${mx},${a.y + s * r}V${b.y - s * r}Q${mx},${b.y} ${mx + r},${b.y}H${b.x}`;
    }
    // Backwards: out to the right, around, into the left.
    const x1 = a.x + dx, x2 = b.x - dx, ym = (a.y + b.y) / 2 + (Math.abs(b.y - a.y) < 40 ? 70 : 0);
    return `M${a.x},${a.y}H${x1}V${ym}H${x2}V${b.y}H${b.x}`;
  }

  drawWires() {
    if (!this.data || this.data.view !== "graph") return;
    const types = {};
    for (const n of this.data.nodes || []) for (const p of n.outputs) types[n.id + ":" + p.id] = p.type;
    let svg = "";
    let k = 0;
    for (const [from, out, to, inp] of this.data.links || []) {
      const a = this.pinPoint(from, out, "out"), b = this.pinPoint(to, inp, "in");
      if (!a || !b) continue;
      const t = types[from + ":" + out] || "exec";
      const color = LG_TYPE_COLORS[t] || "#aaa";
      const ran = t === "exec" && (this.ranAt.get(to) || 0) > performance.now() - 450;
      svg += `<path class="lg-wire${t === "exec" ? " exec" : ""}${ran ? " ran" : ""}" data-k="${k++}" d="${this.wirePath(a, b)}" stroke="${color}"/>`;
    }
    this.el.wires.innerHTML = svg;
    this.el.wires.setAttribute("width", 20000);
    this.el.wires.setAttribute("height", 20000);
  }

  // ---- state machine ----------------------------------------------------------
  renderMachine(data, m) {
    this.el.empty.hidden = !!m?.states?.length;
    this.el.empty.innerHTML = `<div>Double-click to add a state.</div>`;
    if (!m) { this.el.nodes.innerHTML = ""; this.el.wires.innerHTML = ""; return; }
    const current = this.trace?.states?.[m.name];
    const outgoing = {};
    for (const t of m.transitions) (outgoing[t.from] ||= []).push(t);
    this.el.nodes.innerHTML = m.states.map(s => `
      <div class="lg-state${s.initial ? " initial" : ""}${current === s.name ? " current" : ""}${this.selection.has(s.id) ? " selected" : ""}" data-state="${s.id}" style="left:${s.x}px;top:${s.y}px">
        <div class="lg-sh">${s.initial ? `<span class="lg-start" title="Initial state">▶</span>` : ""}<span class="lg-title">${lgEsc(s.name)}</span></div>
        <div class="lg-sb">${(outgoing[s.id] || []).length} transition${(outgoing[s.id] || []).length === 1 ? "" : "s"}</div>
        <div class="lg-handle" title="Drag to another state for a transition (or into empty space for a new state)">+</div>
      </div>`).join("") + (m.transitions.some(t => !t.from) ? `<div class="lg-state any" data-state="0" style="left:${-260}px;top:${-40}px"><div class="lg-sh"><span class="lg-title">Any State</span></div><div class="lg-sb">transitions from every state</div><div class="lg-handle">+</div></div>` : "");
    requestAnimationFrame(() => this.drawTransitions(m));
    const v = this.view;
    if (!v.fitted && m.states.length) { v.fitted = true; requestAnimationFrame(() => this.frameAll()); }
  }

  statePoint(id, side) {
    const el = this.el.nodes.querySelector(`[data-state="${id}"]`);
    if (!el) return null;
    const x = parseFloat(el.style.left), y = parseFloat(el.style.top), w = el.offsetWidth, h = el.offsetHeight;
    return side === "out" ? {x: x + w, y: y + h / 2, w, h, x0: x, y0: y} : {x, y: y + h / 2, w, h, x0: x, y0: y};
  }

  triggerText(t) {
    switch (t.trigger) {
      case "timer": return `after ${t.seconds} s`;
      case "condition": return `when ${t.variable}`;
      case "auto": return "immediately";
      default: return `on ${t.event}`;
    }
  }

  drawTransitions(m) {
    const palette = ["#7dd3fc", "#fda4af", "#86efac", "#fcd34d", "#c4b5fd", "#5eead4", "#f9a8d4", "#fdba74"];
    let svg = `<defs><marker id="lgArrow" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" orient="auto-start-reverse"><path d="M0 0L10 5L0 10z" fill="context-stroke"/></marker></defs>`;
    const labels = [];
    m.transitions.forEach((t, i) => {
      const a = this.statePoint(t.from, "out"), b = this.statePoint(t.to, "in");
      if (!a || !b) return;
      const color = palette[i % palette.length];
      // Several transitions between the same states fan out vertically.
      const lane = (i % 3 - 1) * 10;
      const pa = {x: a.x, y: a.y + lane}, pb = {x: b.x, y: b.y + lane};
      svg += `<path class="lg-trans" data-t="${t.id}" d="${this.wirePath(pa, pb)}" stroke="${color}" marker-end="url(#lgArrow)"/>`;
      const mid = b.x - a.x >= 48 ? {x: (pa.x + pb.x) / 2, y: (pa.y + pb.y) / 2} : {x: (pa.x + pb.x) / 2, y: (pa.y + pb.y) / 2 + 70};
      labels.push(`<div class="lg-tlabel" data-t="${t.id}" style="left:${mid.x}px;top:${mid.y}px;--c:${color}">${lgEsc(t.label || this.triggerText(t))}</div>`);
    });
    this.el.wires.innerHTML = svg;
    this.el.wires.setAttribute("width", 20000);
    this.el.wires.setAttribute("height", 20000);
    this.el.nodes.insertAdjacentHTML("beforeend", labels.join(""));
    for (const l of this.el.nodes.querySelectorAll(".lg-tlabel")) {
      l.onpointerdown = e => e.stopPropagation();
      l.onclick = e => this.editTransition(m, m.transitions.find(t => t.id === +l.dataset.t), e.clientX, e.clientY);
    }
  }

  editTransition(m, t, x, y) {
    if (!t) return;
    const ins = this.el.inspector;
    const vars = (this.data.variables || []).filter(v => v.type === "bool");
    ins.hidden = false;
    ins.innerHTML = `<div class="lg-ins-h">Transition<button class="lg-mini" data-close>✕</button></div>
      <label>When<select data-k="trigger">${[["event", "an event is called"], ["timer", "time in the state passed"], ["condition", "a bool variable is true"], ["auto", "immediately"]]
        .map(([v, l]) => `<option value="${v}"${v === t.trigger ? " selected" : ""}>${l}</option>`).join("")}</select></label>
      <label${t.trigger === "event" ? "" : " hidden"}>Event<input data-k="event" value="${lgEsc(t.event || "")}" list="lgTrEv"><datalist id="lgTrEv">${(this.data.events || []).map(e => `<option value="${lgEsc(e)}">`).join("")}</datalist></label>
      <label${t.trigger === "timer" ? "" : " hidden"}>Seconds<input data-k="seconds" value="${t.seconds ?? 1}"></label>
      <label${t.trigger === "condition" ? "" : " hidden"}>Variable<select data-k="variable">${vars.map(v => `<option${v.name === t.variable ? " selected" : ""}>${lgEsc(v.name)}</option>`).join("")}</select></label>
      <label>Label<input data-k="label" value="${lgEsc(t.label || "")}" placeholder="(automatic)"></label>
      <div class="lg-ins-f"><button class="lg-btn danger" data-del>Delete transition</button></div>`;
    const r = this.el.canvas.getBoundingClientRect();
    ins.style.left = Math.min(r.width - 260, Math.max(8, x - r.left)) + "px";
    ins.style.top = Math.min(r.height - 220, Math.max(8, y - r.top)) + "px";
    ins.querySelector("[data-close]").onclick = () => { ins.hidden = true; };
    ins.querySelector("[data-del]").onclick = () => { ins.hidden = true; this.cmd({cmd: "logicMachine", op: "removeTransition", machine: m.id, transition: t.id}); };
    for (const f of ins.querySelectorAll("[data-k]")) {
      f.addEventListener("keydown", e => e.stopPropagation());
      f.onchange = () => {
        const c = {cmd: "logicMachine", op: "editTransition", machine: m.id, transition: t.id};
        c[f.dataset.k] = f.dataset.k === "seconds" ? (+f.value || 0) : f.value;
        this.cmd(c);
        const nm = this.data.machines.find(x => x.id === m.id);
        const nt = nm?.transitions.find(x => x.id === t.id);
        if (f.dataset.k === "trigger" && nt) this.editTransition(nm, nt, x, y);
      };
    }
  }

  // ---- view -------------------------------------------------------------------
  applyView() {
    const v = this.view;
    this.el.world.style.transform = `translate(${v.x}px,${v.y}px) scale(${v.zoom})`;
    this.el.canvas.style.backgroundSize = `${LG_GRID * v.zoom}px ${LG_GRID * v.zoom}px, ${LG_GRID * 5 * v.zoom}px ${LG_GRID * 5 * v.zoom}px`;
    this.el.canvas.style.backgroundPosition = `${v.x}px ${v.y}px`;
  }

  toWorld(clientX, clientY) {
    const r = this.el.canvas.getBoundingClientRect(), v = this.view;
    return {x: (clientX - r.left - v.x) / v.zoom, y: (clientY - r.top - v.y) / v.zoom};
  }

  frameAll() {
    const els = [...this.el.nodes.querySelectorAll(".lg-node, .lg-state")];
    const r = this.el.canvas.getBoundingClientRect();
    if (!els.length || !r.width) return;
    let x0 = 1e9, y0 = 1e9, x1 = -1e9, y1 = -1e9;
    for (const e of els) {
      const x = parseFloat(e.style.left), y = parseFloat(e.style.top);
      x0 = Math.min(x0, x); y0 = Math.min(y0, y); x1 = Math.max(x1, x + e.offsetWidth); y1 = Math.max(y1, y + e.offsetHeight);
    }
    const v = this.view;
    v.zoom = Math.max(0.3, Math.min(1.1, Math.min((r.width - 80) / (x1 - x0 + 1), (r.height - 80) / (y1 - y0 + 1))));
    v.x = (r.width - (x1 - x0) * v.zoom) / 2 - x0 * v.zoom;
    v.y = (r.height - (y1 - y0) * v.zoom) / 2 - y0 * v.zoom;
    this.applyView();
    this.drawWires();
  }

  focusNode(id) {
    const el = this.el.nodes.querySelector(`.lg-node[data-id="${id}"]`);
    if (!el) return;
    const r = this.el.canvas.getBoundingClientRect(), v = this.view;
    v.zoom = 1;
    v.x = r.width / 2 - parseFloat(el.style.left) - el.offsetWidth / 2;
    v.y = r.height / 2 - parseFloat(el.style.top) - el.offsetHeight / 2;
    this.selection = new Set([id]);
    for (const n of this.el.nodes.querySelectorAll(".lg-node")) n.classList.toggle("selected", +n.dataset.id === id);
    this.applyView();
    this.drawWires();
  }

  centerWorld() {
    const r = this.el.canvas.getBoundingClientRect();
    return this.toWorld(r.left + r.width / 2 - 100, r.top + r.height / 2 - 40);
  }

  addAtCenter(type, params) {
    const p = this.centerWorld();
    // Do not stack on top of each other.
    p.x += (this.stagger = ((this.stagger || 0) + 24) % 120);
    p.y += this.stagger;
    const r = this.cmd({cmd: "logicAdd", type, x: p.x, y: p.y, params});
    if (r?.id) this.selection = new Set([r.id]);
  }

  // ---- canvas input -----------------------------------------------------------
  initCanvas() {
    const c = this.el.canvas;
    c.addEventListener("wheel", e => {
      e.preventDefault();
      const v = this.view, r = c.getBoundingClientRect();
      const z = Math.max(0.25, Math.min(1.8, v.zoom * Math.pow(1.0015, -e.deltaY)));
      const mx = e.clientX - r.left, my = e.clientY - r.top;
      v.x = mx - (mx - v.x) * (z / v.zoom);
      v.y = my - (my - v.y) * (z / v.zoom);
      v.zoom = z;
      this.applyView();
    }, {passive: false});
    c.addEventListener("contextmenu", e => e.preventDefault());
    c.addEventListener("pointerdown", e => this.onDown(e));
    c.addEventListener("pointermove", e => this.onMove(e));
    c.addEventListener("pointerup", e => this.onUp(e));
    c.addEventListener("dblclick", e => {
      if (this.data?.view?.startsWith("m:") && !e.target.closest(".lg-state")) {
        const p = this.toWorld(e.clientX, e.clientY);
        this.cmd({cmd: "logicMachine", op: "addState", machine: +this.data.view.slice(2), x: p.x - 90, y: p.y - 30});
      } else if (e.target.closest(".lg-state")) {
        this.renameState(+e.target.closest(".lg-state").dataset.state);
      }
    });
    c.addEventListener("dragover", e => { if (e.dataTransfer.types.includes("text/x-kern-var")) e.preventDefault(); });
    c.addEventListener("drop", e => {
      const name = e.dataTransfer.getData("text/x-kern-var");
      if (!name) return;
      e.preventDefault();
      const p = this.toWorld(e.clientX, e.clientY);
      this.cmd({cmd: "logicAdd", type: e.ctrlKey || e.metaKey ? "var.set" : "var.get", x: p.x, y: p.y, params: {variable: name}});
    });
    c.addEventListener("keydown", e => this.onKey(e));
  }

  onKey(e) {
    if (e.target.matches("input, select, textarea")) return;
    // Typing while the search is open goes into the search.
    if (this.palette && e.key.length === 1 && !e.ctrlKey && !e.metaKey) {
      const q = this.palette.querySelector(".lg-pal-q");
      q.focus();
      q.value += e.key;
      q.dispatchEvent(new Event("input"));
      e.preventDefault();
      return;
    }
    const mod = e.ctrlKey || e.metaKey;
    const k = e.key.toLowerCase();
    const ids = [...this.selection];
    const machine = this.data?.view?.startsWith("m:") ? +this.data.view.slice(2) : 0;
    if ((k === "delete" || k === "backspace") && ids.length) {
      if (machine) for (const id of ids) this.cmd({cmd: "logicMachine", op: "removeState", machine, state: id});
      else this.cmd({cmd: "logicDelete", ids});
      this.selection.clear();
    } else if (mod && (k === "c" || k === "x") && ids.length && !machine) {
      const r = this.engine.command({cmd: "logicCopy", ids});
      if (r?.text) { this.clip = r.text; navigator.clipboard?.writeText(r.text).catch(() => {}); }
      if (k === "x") this.cmd({cmd: "logicDelete", ids});
    } else if (mod && k === "v" && !machine) {
      const paste = text => { const p = this.lastPointer || this.centerWorld(); const r = this.cmd({cmd: "logicPaste", text, x: p.x, y: p.y}); if (r?.ids) this.selection = new Set(r.ids); };
      if (navigator.clipboard?.readText) navigator.clipboard.readText().then(t => paste(t?.includes("kern-logic") ? t : this.clip || "")).catch(() => this.clip && paste(this.clip));
      else if (this.clip) paste(this.clip);
    } else if (mod && k === "d" && ids.length && !machine) {
      const r = this.engine.command({cmd: "logicCopy", ids});
      const first = this.data.nodes.find(n => n.id === ids[0]);
      if (r?.text && first) { const p = this.cmd({cmd: "logicPaste", text: r.text, x: first.x + 40, y: first.y + 40}); if (p?.ids) this.selection = new Set(p.ids); }
    } else if (mod && k === "a") {
      this.selection = new Set((machine ? [] : this.data.nodes.map(n => n.id)));
      for (const n of this.el.nodes.querySelectorAll(".lg-node")) n.classList.add("selected");
    } else if (k === "f" || k === "home") {
      this.frameAll();
    } else if (k === "escape") {
      if (this.palette) this.closePalette();
      else this.cmd({cmd: "logicClose"});
    } else return;
    e.preventDefault();
    e.stopPropagation();
  }

  onDown(e) {
    // Controls on the canvas (start buttons, palette, inspector, fields) keep their own clicks.
    if (e.target.closest(".lg-inspector, .lg-palette, .lg-empty button, button, input, select, textarea")) return;
    this.el.canvas.focus({preventScroll: true});
    this.closePalette();
    if (!e.target.closest(".lg-inspector")) this.el.inspector.hidden = true;
    const p = this.toWorld(e.clientX, e.clientY);
    this.lastPointer = p;
    const pan = e.button === 1 || e.button === 2 || (e.button === 0 && e.altKey && !e.target.closest(".lg-row"));
    const row = e.target.closest(".lg-row");
    const nodeEl = e.target.closest(".lg-node");
    const stateEl = e.target.closest(".lg-state");
    this.el.canvas.setPointerCapture(e.pointerId);
    if (row && e.button === 0) {
      const node = +row.closest(".lg-node").dataset.id;
      if (e.altKey) { this.cmd({cmd: "logicDisconnect", node, pin: row.dataset.pin, output: row.dataset.dir === "out"}); return; }
      this.drag = {kind: "wire", node, pin: row.dataset.pin, out: row.dataset.dir === "out", type: row.dataset.type, x0: e.clientX, y0: e.clientY};
      return;
    }
    if (stateEl && e.target.closest(".lg-handle") && e.button === 0) {
      this.drag = {kind: "transition", from: +stateEl.dataset.state, x0: e.clientX, y0: e.clientY};
      return;
    }
    if ((nodeEl || stateEl) && e.button === 0) {
      const el = nodeEl || stateEl;
      const id = +(el.dataset.id ?? el.dataset.state);
      if (e.shiftKey || e.ctrlKey) {
        if (this.selection.has(id)) this.selection.delete(id); else this.selection.add(id);
      } else if (!this.selection.has(id)) this.selection = new Set([id]);
      for (const n of this.el.nodes.querySelectorAll(".lg-node, .lg-state")) n.classList.toggle("selected", this.selection.has(+(n.dataset.id ?? n.dataset.state)));
      const items = [...this.selection].map(sid => {
        const n = this.el.nodes.querySelector(`.lg-node[data-id="${sid}"], .lg-state[data-state="${sid}"]`);
        return n ? {id: sid, el: n, x: parseFloat(n.style.left), y: parseFloat(n.style.top)} : null;
      }).filter(Boolean);
      this.drag = {kind: "nodes", items, x0: e.clientX, y0: e.clientY, moved: false, transaction: "lgmove" + Date.now()};
      return;
    }
    if (pan) {
      this.drag = {kind: "pan", x0: e.clientX, y0: e.clientY, vx: this.view.x, vy: this.view.y, moved: false, button: e.button, target: e.target};
      return;
    }
    if (e.button === 0) {
      if (!e.shiftKey) {
        this.selection.clear();
        for (const n of this.el.nodes.querySelectorAll(".selected")) n.classList.remove("selected");
      }
      this.drag = {kind: "marquee", x0: e.clientX, y0: e.clientY};
    }
  }

  onMove(e) {
    const d = this.drag;
    this.lastPointer = this.toWorld(e.clientX, e.clientY);
    if (!d) return;
    const z = this.view.zoom;
    if (d.kind === "pan") {
      if (Math.abs(e.clientX - d.x0) + Math.abs(e.clientY - d.y0) > 3) d.moved = true;
      this.view.x = d.vx + e.clientX - d.x0;
      this.view.y = d.vy + e.clientY - d.y0;
      this.applyView();
    } else if (d.kind === "nodes") {
      const dx = (e.clientX - d.x0) / z, dy = (e.clientY - d.y0) / z;
      if (Math.abs(dx) + Math.abs(dy) > 2) d.moved = true;
      for (const it of d.items) {
        it.el.style.left = Math.round((it.x + dx) / 8) * 8 + "px";
        it.el.style.top = Math.round((it.y + dy) / 8) * 8 + "px";
      }
      if (this.data.view === "graph") this.drawWires();
      else {
        const m = this.data.machines.find(x => "m:" + x.id === this.data.view);
        for (const l of this.el.nodes.querySelectorAll(".lg-tlabel")) l.remove();
        if (m) this.drawTransitions(m);
      }
    } else if (d.kind === "wire" || d.kind === "transition") {
      const r = this.el.canvas.getBoundingClientRect();
      let a;
      if (d.kind === "wire") {
        const pt = this.pinPoint(d.node, d.pin, d.out ? "out" : "in");
        a = pt ? {x: pt.x * z + this.view.x, y: pt.y * z + this.view.y} : {x: d.x0 - r.left, y: d.y0 - r.top};
      } else {
        const sp = this.statePoint(d.from, "out");
        a = sp ? {x: sp.x * z + this.view.x, y: sp.y * z + this.view.y} : {x: d.x0 - r.left, y: d.y0 - r.top};
      }
      const b = {x: e.clientX - r.left, y: e.clientY - r.top};
      const color = d.kind === "wire" ? LG_TYPE_COLORS[d.type] : "#7dd3fc";
      const path = d.kind === "wire" && !d.out ? this.wirePath(b, a) : this.wirePath(a, b);
      this.el.temp.innerHTML = `<path d="${path}" stroke="${color}" class="lg-wire${d.type === "exec" ? " exec" : ""} temp"/>`;
      // Highlight pins that fit.
      const target = document.elementFromPoint(e.clientX, e.clientY)?.closest?.(".lg-row");
      for (const h of this.el.nodes.querySelectorAll(".lg-row.hot")) h.classList.remove("hot");
      if (target && d.kind === "wire" && this.fits(d, target)) target.classList.add("hot");
    } else if (d.kind === "marquee") {
      const r = this.el.canvas.getBoundingClientRect();
      const x = Math.min(d.x0, e.clientX) - r.left, y = Math.min(d.y0, e.clientY) - r.top, w = Math.abs(e.clientX - d.x0), h = Math.abs(e.clientY - d.y0);
      Object.assign(this.el.marquee.style, {left: x + "px", top: y + "px", width: w + "px", height: h + "px"});
      this.el.marquee.hidden = w + h < 4;
    }
  }

  fits(d, row) {
    if (!row || row.dataset.dir === (d.out ? "out" : "in")) return false;
    if (+row.closest(".lg-node")?.dataset.id === d.node) return false;
    const from = d.out ? d.type : row.dataset.type, to = d.out ? row.dataset.type : d.type;
    if ((from === "exec") !== (to === "exec")) return false;
    if (from === to || to === "text") return true;
    const num = t => t === "int" || t === "float" || t === "bool";
    if (num(from) && num(to)) return true;
    if (to === "bool" && from === "actor") return true;
    if (to === "vector" && (from === "int" || from === "float")) return true;
    return false;
  }

  onUp(e) {
    const d = this.drag;
    this.drag = null;
    this.el.temp.innerHTML = "";
    this.el.marquee.hidden = true;
    for (const h of this.el.nodes.querySelectorAll(".lg-row.hot")) h.classList.remove("hot");
    if (!d) return;
    if (d.kind === "pan") {
      if (!d.moved && d.button === 2) this.contextMenu({clientX: e.clientX, clientY: e.clientY, target: d.target});
      return;
    }
    if (d.kind === "nodes") {
      if (d.moved) {
        const moves = d.items.map(it => ({id: it.id, x: parseFloat(it.el.style.left), y: parseFloat(it.el.style.top)}));
        this.cmd({cmd: "logicMove", moves, transaction: d.transaction});
      }
      return;
    }
    if (d.kind === "wire") {
      const row = document.elementFromPoint(e.clientX, e.clientY)?.closest?.(".lg-row");
      if (row && row.closest(".lg-node")) {
        if (!this.fits(d, row)) { if (+row.closest(".lg-node").dataset.id !== d.node) this.studio.toast("These pins do not fit together"); return; }
        const other = +row.closest(".lg-node").dataset.id;
        if (d.out) this.cmd({cmd: "logicConnect", from: d.node, out: d.pin, to: other, in: row.dataset.pin});
        else this.cmd({cmd: "logicConnect", from: other, out: row.dataset.pin, to: d.node, in: d.pin});
        return;
      }
      if (Math.abs(e.clientX - d.x0) + Math.abs(e.clientY - d.y0) > 6) this.openPalette(e.clientX, e.clientY, {node: d.node, pin: d.pin, output: d.out});
      return;
    }
    if (d.kind === "transition") {
      const machine = +this.data.view.slice(2);
      const st = document.elementFromPoint(e.clientX, e.clientY)?.closest?.(".lg-state");
      if (st && +st.dataset.state !== d.from) this.cmd({cmd: "logicMachine", op: "addTransition", machine, from: d.from, to: +st.dataset.state});
      else if (!st) {
        const p = this.toWorld(e.clientX, e.clientY);
        this.cmd({cmd: "logicMachine", op: "addState", machine, from: d.from, x: p.x, y: p.y - 30});
      }
      return;
    }
    if (d.kind === "marquee") {
      const x0 = Math.min(d.x0, e.clientX), x1 = Math.max(d.x0, e.clientX), y0 = Math.min(d.y0, e.clientY), y1 = Math.max(d.y0, e.clientY);
      if (x1 - x0 + y1 - y0 < 4) return;
      for (const n of this.el.nodes.querySelectorAll(".lg-node, .lg-state")) {
        const r = n.getBoundingClientRect();
        if (r.right > x0 && r.left < x1 && r.bottom > y0 && r.top < y1) {
          this.selection.add(+(n.dataset.id ?? n.dataset.state));
          n.classList.add("selected");
        }
      }
    }
  }

  contextMenu(e) {
    const machine = this.data?.view?.startsWith("m:") ? +this.data.view.slice(2) : 0;
    const nodeEl = e.target.closest?.(".lg-node"), stateEl = e.target.closest?.(".lg-state");
    if (nodeEl) {
      const id = +nodeEl.dataset.id;
      const n = this.data.nodes.find(x => x.id === id);
      this.studio.openMenu([
        {header: n?.title || "Node"},
        {label: "Delete", shortcut: "Del", run: () => this.cmd({cmd: "logicDelete", ids: this.selection.has(id) ? [...this.selection] : [id]})},
        {label: "Duplicate", shortcut: "Ctrl+D", run: () => { this.selection = new Set([id]); this.onKey(new KeyboardEvent("keydown", {key: "d", ctrlKey: true})); }},
        {label: "Break All Wires", run: () => { for (const p of [...(n?.inputs || []), ...(n?.outputs || [])]) if (p.linked) this.engine.command({cmd: "logicDisconnect", node: id, pin: p.id, output: n.outputs.includes(p)}); this.studio.flushUi(); }},
        {label: n?.comment ? "Edit Comment..." : "Add Comment...", run: () => { const t = prompt("Comment", n?.comment || ""); if (t !== null) this.cmd({cmd: "logicComment", node: id, text: t}); }},
      ], e.clientX, e.clientY);
      return;
    }
    if (stateEl && machine) {
      const id = +stateEl.dataset.state;
      this.studio.openMenu([
        {label: "Set as Initial State", run: () => this.cmd({cmd: "logicMachine", op: "initial", machine, state: id})},
        {label: "Rename...", run: () => this.renameState(id)},
        {label: "Transition from Any State", run: () => this.cmd({cmd: "logicMachine", op: "addTransition", machine, from: 0, to: id})},
        {label: "Delete State", run: () => this.cmd({cmd: "logicMachine", op: "removeState", machine, state: id})},
      ], e.clientX, e.clientY);
      return;
    }
    if (machine) {
      const p = this.toWorld(e.clientX, e.clientY);
      this.studio.openMenu([{label: "Add State", run: () => this.cmd({cmd: "logicMachine", op: "addState", machine, x: p.x, y: p.y})}], e.clientX, e.clientY);
      return;
    }
    this.openPalette(e.clientX, e.clientY, null);
  }

  renameState(id) {
    const m = this.data.machines.find(x => "m:" + x.id === this.data.view);
    const s = m?.states.find(x => x.id === id);
    if (!s) return;
    const name = prompt("State name", s.name);
    if (name && name !== s.name) this.cmd({cmd: "logicMachine", op: "renameState", machine: m.id, state: id, name});
  }

  renameMachine(id) {
    const m = this.data.machines.find(x => x.id === id);
    const name = m && prompt("State machine name", m.name);
    if (name && name !== m.name) this.cmd({cmd: "logicMachine", op: "rename", machine: id, name});
  }

  addVariable() {
    const r = this.cmd({cmd: "logicVariable", op: "add", name: "NewVar", type: "float"});
    if (r?.name) setTimeout(() => {
      const row = this.el.vars.querySelector(`[data-var="${CSS.escape(r.name)}"]`);
      if (row) this.variableMenu(this.data.variables.find(v => v.name === r.name), row.getBoundingClientRect().left, row.getBoundingClientRect().bottom);
    }, 30);
  }

  variableMenu(v, x, y) {
    if (!v) return;
    const ins = this.el.inspector;
    ins.hidden = false;
    const val = v.type === "bool" ? `<input type="checkbox" data-k="value"${v.value ? " checked" : ""}>`
      : v.type === "vector" ? `<input data-k="value" value="${lgEsc((v.value || [0, 0, 0]).join(", "))}">`
      : `<input data-k="value" value="${lgEsc(lgNum(v.value))}">`;
    ins.innerHTML = `<div class="lg-ins-h">Variable<button class="lg-mini" data-close>✕</button></div>
      <label>Name<input data-k="name" value="${lgEsc(v.name)}" spellcheck="false"></label>
      <label>Type<select data-k="type">${["bool", "int", "float", "vector", "text", "actor"].map(t => `<option${t === v.type ? " selected" : ""}>${t}</option>`).join("")}</select></label>
      <label>Default${val}</label>
      <div class="lg-ins-f"><button class="lg-btn danger" data-del>Delete variable</button></div>`;
    const r = this.el.canvas.getBoundingClientRect();
    ins.style.left = "8px";
    ins.style.top = Math.min(r.height - 200, Math.max(8, y - r.top)) + "px";
    ins.querySelector("[data-close]").onclick = () => { ins.hidden = true; };
    ins.querySelector("[data-del]").onclick = () => { ins.hidden = true; this.cmd({cmd: "logicVariable", op: "remove", name: v.name}); };
    for (const f of ins.querySelectorAll("[data-k]")) {
      f.addEventListener("keydown", e => e.stopPropagation());
      f.onchange = () => {
        if (f.dataset.k === "name") { if (this.cmd({cmd: "logicVariable", op: "rename", name: v.name, to: f.value})?.ok !== false) v = {...v, name: f.value}; }
        else if (f.dataset.k === "type") { this.cmd({cmd: "logicVariable", op: "type", name: v.name, type: f.value}); ins.hidden = true; }
        else {
          const value = f.type === "checkbox" ? f.checked : v.type === "vector" ? f.value.split(",").map(s => +s || 0) : v.type === "text" ? f.value : (+f.value || 0);
          this.cmd({cmd: "logicVariable", op: "value", name: v.name, value});
        }
      };
    }
  }

  // ---- palette (right-click search) -------------------------------------------
  openPalette(x, y, from) {
    this.closePalette();
    const where = this.toWorld(x, y);
    const box = document.createElement("div");
    box.className = "lg-palette";
    box.innerHTML = `<div class="lg-pal-h">${from ? "Nodes that fit this pin" : "All nodes for this graph"}</div><input class="lg-pal-q" placeholder="Search" spellcheck="false"><div class="lg-pal-list"></div><div class="lg-pal-help"></div>`;
    this.el.canvas.append(box);
    const r = this.el.canvas.getBoundingClientRect();
    box.style.left = Math.min(r.width - 330, Math.max(4, x - r.left)) + "px";
    box.style.top = Math.min(r.height - 420, Math.max(4, y - r.top)) + "px";
    const q = box.querySelector(".lg-pal-q"), list = box.querySelector(".lg-pal-list"), help = box.querySelector(".lg-pal-help");
    let items = [], active = 0;
    const show = () => {
      const res = this.engine.command({cmd: "logicPalette", query: q.value, from: from || undefined});
      items = res?.items || [];
      active = 0;
      let html = "", cat = "";
      items.forEach((it, i) => {
        if (it.category !== cat && !q.value) { cat = it.category; html += `<div class="lg-pal-cat">${lgEsc(cat)}</div>`; }
        html += `<div class="lg-pal-it" data-i="${i}">${lgEsc(it.label)}${q.value ? `<span class="lg-pal-c">${lgEsc(it.category)}</span>` : ""}</div>`;
      });
      list.innerHTML = html || `<div class="lg-dim">Nothing matches</div>`;
      mark();
    };
    const mark = () => {
      for (const el of list.querySelectorAll(".lg-pal-it")) el.classList.toggle("active", +el.dataset.i === active);
      list.querySelector(".lg-pal-it.active")?.scrollIntoView({block: "nearest"});
      help.textContent = items[active]?.help || "";
    };
    const pick = i => {
      const it = items[i];
      if (!it) return;
      this.closePalette();
      const res = this.cmd({cmd: "logicAdd", type: it.type, x: where.x, y: where.y, params: it.params, from: from || undefined});
      if (res?.id) this.selection = new Set([res.id]);
      this.el.canvas.focus({preventScroll: true});
    };
    q.oninput = show;
    q.onkeydown = e => {
      e.stopPropagation();
      if (e.key === "ArrowDown") { active = Math.min(items.length - 1, active + 1); mark(); e.preventDefault(); }
      else if (e.key === "ArrowUp") { active = Math.max(0, active - 1); mark(); e.preventDefault(); }
      else if (e.key === "Enter") { pick(active); e.preventDefault(); }
      else if (e.key === "Escape") { this.closePalette(); this.el.canvas.focus(); e.preventDefault(); }
    };
    list.onpointermove = e => { const it = e.target.closest(".lg-pal-it"); if (it && +it.dataset.i !== active) { active = +it.dataset.i; mark(); } };
    list.onclick = e => { const it = e.target.closest(".lg-pal-it"); if (it) pick(+it.dataset.i); };
    box.onpointerdown = e => e.stopPropagation();
    box.onwheel = e => e.stopPropagation();
    this.palette = box;
    show();
    q.focus({preventScroll: true});
    setTimeout(() => { if (this.palette === box && document.activeElement !== q) q.focus({preventScroll: true}); }, 0);
  }

  closePalette() {
    this.palette?.remove();
    this.palette = null;
  }

  // ---- live (while playing) ----------------------------------------------------
  setTrace(trace) {
    this.trace = trace || null;
    if (!this.data) return;
    const now = performance.now();
    for (const id of trace?.ran || []) this.ranAt.set(id, now);
    let any = false;
    for (const el of this.el.nodes.querySelectorAll(".lg-node")) {
      const ran = (this.ranAt.get(+el.dataset.id) || 0) > now - 450;
      if (el.classList.contains("ran") !== ran) { el.classList.toggle("ran", ran); any = true; }
    }
    if (any) this.drawWires();
    if (this.data.view !== "graph" && trace?.states) {
      const m = this.data.machines.find(x => "m:" + x.id === this.data.view);
      const cur = m && trace.states[m.name];
      for (const el of this.el.nodes.querySelectorAll(".lg-state")) {
        const s = m?.states.find(x => x.id === +el.dataset.state);
        el.classList.toggle("current", !!s && s.name === cur);
      }
    }
    this.renderSide(this.data);
  }
}

export const LOGIC_STYLE = `
#logicEditor{position:absolute;inset:0;z-index:5;display:flex;flex-direction:column;background:#07111f;color:#dbe7f5}
.lg-head{display:flex;align-items:center;gap:6px;height:34px;padding:0 8px;background:#0b1c30;border-bottom:1px solid #12345a;flex:none}
.lg-btn{height:24px;padding:0 10px;border:1px solid #1d4269;border-radius:5px;background:#0f2742;color:#dbe7f5;font-size:12px}
.lg-btn:hover{background:#163a62;border-color:#5f8dbc}
.lg-btn.danger{border-color:#7a2630;background:#3a141a}
.lg-owner{height:24px;background:#0f2742;color:#dbe7f5;border:1px solid #1d4269;border-radius:5px;max-width:200px}
.lg-tabs{display:flex;gap:2px;margin-left:6px;overflow:auto}
.lg-tab{height:26px;padding:0 12px;border:0;border-radius:6px 6px 0 0;background:transparent;color:#8fb0d3;font-weight:600}
.lg-tab.active{background:#12345a;color:#fff}
.lg-spacer{flex:1}
.lg-status{font-size:11px;font-weight:700;letter-spacing:.04em;padding:3px 9px;border-radius:999px}
.lg-status.good{background:rgba(52,211,153,.15);color:#6ee7b7}.lg-status.bad{background:rgba(240,98,79,.18);color:#ff9a8a}
.lg-body{flex:1;display:flex;min-height:0}
.lg-side{width:220px;flex:none;overflow:auto;background:#081627;border-right:1px solid #12345a;padding:6px 0}
.lg-sec{padding:4px 8px 10px}.lg-sec-h{display:flex;align-items:center;justify-content:space-between;font-size:10px;font-weight:700;letter-spacing:.12em;text-transform:uppercase;color:#7fa3c9;margin-bottom:4px}
.lg-mini{height:18px;padding:0 6px;border:1px solid #1d4269;border-radius:4px;background:#0f2742;color:#cfe0f3;font-size:10.5px;margin-left:3px}
.lg-mini:hover{border-color:#5f8dbc}
.lg-var,.lg-ev{display:flex;align-items:center;gap:5px;padding:3px 4px;border-radius:5px;cursor:grab}
.lg-var:hover,.lg-ev:hover{background:#0f2742}
.lg-var-name,.lg-ev span{flex:1;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.lg-var-type{font-size:10px;color:#6f8fb3}.lg-live{font:10px var(--mono,monospace);color:#fcd34d;max-width:60px;overflow:hidden;text-overflow:ellipsis}
.lg-dot{width:9px;height:9px;border-radius:50%;flex:none}
.lg-dim{color:#5f7fa3;font-size:11px;padding:2px 4px}
.lg-issue{font-size:11px;padding:3px 6px;border-radius:4px;margin-bottom:3px;cursor:pointer}.lg-issue.err{background:rgba(240,98,79,.14);color:#ffb4a8}.lg-issue.warn{background:rgba(252,211,77,.1);color:#f5d97a}
.lg-tip{margin:8px;padding:8px;border-radius:6px;background:#0b1c30;color:#8fb0d3;font-size:11px;line-height:1.45}
.lg-canvas{position:relative;flex:1;overflow:hidden;outline:none;cursor:default;background-color:#07111f;
  background-image:linear-gradient(rgba(95,141,188,.07) 1px,transparent 1px),linear-gradient(90deg,rgba(95,141,188,.07) 1px,transparent 1px),
  linear-gradient(rgba(95,141,188,.12) 1px,transparent 1px),linear-gradient(90deg,rgba(95,141,188,.12) 1px,transparent 1px)}
.lg-world{position:absolute;left:0;top:0;transform-origin:0 0}
.lg-wires{position:absolute;left:0;top:0;overflow:visible;pointer-events:none}
.lg-temp{position:absolute;inset:0;width:100%;height:100%;pointer-events:none;overflow:visible}
.lg-wire{fill:none;stroke-width:2;opacity:.85}.lg-wire.exec{stroke-width:2.6;opacity:.9}.lg-wire.temp{stroke-dasharray:6 4}
.lg-wire.ran{stroke:#fff;stroke-width:4;filter:drop-shadow(0 0 4px #7dd3fc)}
.lg-nodes{position:absolute;left:0;top:0}
.lg-node{position:absolute;min-width:180px;max-width:300px;border-radius:10px;background:#12345a;border:1px solid #5f8dbc;box-shadow:0 6px 18px rgba(0,0,0,.45);user-select:none}
.lg-node.pure{background:#163148}
.lg-node.selected{outline:2px solid #fbbf24;outline-offset:1px}
.lg-node.error{border-color:#f0624f;box-shadow:0 0 0 2px rgba(240,98,79,.45),0 6px 18px rgba(0,0,0,.45)}
.lg-node.warn{border-color:#d9b44a}
.lg-node.ran{box-shadow:0 0 0 2px #7dd3fc,0 0 18px rgba(56,189,248,.65)}
.lg-nh{display:flex;align-items:center;gap:6px;height:30px;padding:0 10px;border-radius:9px 9px 0 0;background:linear-gradient(180deg,var(--h),color-mix(in srgb,var(--h) 70%,#0b1c30));font-weight:700;color:#fff;cursor:move}
.lg-ico{opacity:.85;font-size:12px}.lg-title{white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.lg-comment{padding:4px 10px;font-size:11px;color:#fcd34d;background:rgba(252,211,77,.08);border-bottom:1px solid #1d4269}
.lg-params{padding:6px 10px 2px;display:flex;flex-direction:column;gap:4px}
.lg-param{display:flex;align-items:center;gap:6px;font-size:11px;color:#9fbbd9}.lg-param span{min-width:52px}
.lg-par,.lg-lit{height:20px;background:#0b1c30;border:1px solid #1d4269;border-radius:4px;color:#e6eef8;font-size:11px;padding:0 5px;min-width:0}
select.lg-par{max-width:150px}.lg-lit.num{width:58px}.lg-lit.txt{width:110px}.lg-lit.v{width:44px;margin-right:2px}.lg-lit.act{max-width:120px}
.lg-pins{display:flex;justify-content:space-between;gap:14px;padding:6px 0 8px}
.lg-col{display:flex;flex-direction:column;gap:3px}.lg-col.right{align-items:flex-end}
.lg-row{display:flex;align-items:center;gap:6px;min-height:20px;padding:0 8px;font-size:11.5px;color:#cfe0f3;cursor:crosshair;border-radius:4px}
.lg-row:hover,.lg-row.hot{background:rgba(125,211,252,.12)}
.lg-row.hot{outline:1px dashed #7dd3fc}
.lg-pin{display:inline-block;width:11px;height:11px;border-radius:50%;border:2px solid var(--c);flex:none}
.lg-pin.linked{background:var(--c)}
svg.lg-pin{width:13px;height:13px;border:0;border-radius:0}
.lg-pl{white-space:nowrap}
.lg-state{position:absolute;width:216px;border-radius:10px;background:#12345a;border:1px solid #5f8dbc;box-shadow:0 6px 18px rgba(0,0,0,.45);user-select:none}
.lg-state.initial{border-color:#38bdf8}
.lg-state.current{box-shadow:0 0 0 3px #34d399,0 0 22px rgba(52,211,153,.55)}
.lg-state.selected{outline:2px solid #fbbf24;outline-offset:1px}
.lg-state.any{background:#1b2638;border-style:dashed}
.lg-sh{display:flex;align-items:center;gap:6px;height:34px;padding:0 12px;border-radius:9px 9px 0 0;background:#0b1c30;font-weight:700;cursor:move}
.lg-start{color:#38bdf8}
.lg-sb{padding:6px 12px 10px;font-size:11px;color:#8fb0d3}
.lg-handle{position:absolute;right:-11px;top:50%;margin-top:-11px;width:22px;height:22px;border-radius:50%;background:#38bdf8;color:#07111f;font-weight:800;display:grid;place-items:center;cursor:crosshair;opacity:.75}
.lg-state:hover .lg-handle{opacity:1}
.lg-trans{fill:none;stroke-width:2.2}
.lg-tlabel{position:absolute;transform:translate(-50%,-50%);padding:2px 8px;border-radius:999px;background:#0b1c30;border:1px solid var(--c);color:var(--c);font-size:11px;white-space:nowrap;cursor:pointer}
.lg-marquee{position:absolute;border:1px solid #7dd3fc;background:rgba(56,189,248,.1);pointer-events:none}
.lg-empty{position:absolute;left:50%;top:42%;transform:translate(-50%,-50%);text-align:center;color:#8fb0d3;line-height:1.7;pointer-events:auto}
.lg-empty b{color:#fff}.lg-starts{display:flex;gap:6px;justify-content:center;margin-top:8px;flex-wrap:wrap}
.lg-starts button{height:28px;padding:0 12px;border-radius:6px;border:1px solid #5f8dbc;background:#12345a;color:#fff}
.lg-palette{position:absolute;width:320px;max-height:420px;display:flex;flex-direction:column;border-radius:10px;background:#0b1c30;border:1px solid #5f8dbc;box-shadow:0 14px 40px rgba(0,0,0,.6);z-index:3}
.lg-pal-h{padding:8px 10px 4px;font-size:10px;font-weight:700;letter-spacing:.12em;text-transform:uppercase;color:#7fa3c9}
.lg-pal-q{margin:0 8px 6px;height:28px;border-radius:6px;border:1px solid #1d4269;background:#07111f;color:#fff;padding:0 8px}
.lg-pal-list{flex:1;overflow:auto;padding:0 4px}
.lg-pal-cat{padding:6px 8px 2px;font-size:10px;font-weight:700;color:#5f8dbc;text-transform:uppercase;letter-spacing:.08em}
.lg-pal-it{display:flex;justify-content:space-between;padding:4px 10px;border-radius:5px;cursor:pointer}
.lg-pal-it.active{background:#1e4f8a;color:#fff}.lg-pal-c{color:#7fa3c9;font-size:10.5px}
.lg-pal-help{min-height:34px;padding:6px 10px;font-size:11px;color:#9fbbd9;border-top:1px solid #12345a}
.lg-inspector{position:absolute;width:250px;padding:8px 10px 10px;border-radius:10px;background:#0b1c30;border:1px solid #5f8dbc;box-shadow:0 14px 40px rgba(0,0,0,.6);z-index:4;display:flex;flex-direction:column;gap:6px}
.lg-inspector label{display:flex;align-items:center;justify-content:space-between;gap:8px;font-size:11.5px;color:#9fbbd9}
.lg-inspector label[hidden]{display:none}
.lg-inspector input:not([type=checkbox]),.lg-inspector select{width:140px;height:22px;background:#07111f;border:1px solid #1d4269;border-radius:4px;color:#fff;padding:0 6px}
.lg-ins-h{display:flex;justify-content:space-between;align-items:center;font-weight:700;color:#fff}
.lg-ins-f{display:flex;justify-content:flex-end;margin-top:4px}
`;
