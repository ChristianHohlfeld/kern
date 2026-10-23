// ---------------------------------------------------------------------------
// Kern Logic editor: the Blueprint editor of Kern Studio.
//
// The graph lives in the engine (C++): every change is a Studio command (logicAdd,
// logicConnect, logicMove...) and one undo step. This module only draws what the
// engine describes (state.logic) and turns gestures into commands.
//
// It follows Unreal Engine's Blueprint editor:
//   * layout      toolbar (Compile, Save, Find), My Blueprint (graphs, variables,
//                 events), the graph with tabs, Details, Compiler Results / Find.
//   * graph       grey grid, nodes with coloured title bars by kind, compact
//                 operator and variable nodes, spline wires coloured by type,
//                 reroute knots, comment boxes, live execution while playing.
//   * hands       right-click (or a wire dropped in empty space) opens the
//                 context-sensitive action menu; Alt+click breaks wires, Ctrl+drag
//                 moves them, double-click a wire adds a knot, C comments the
//                 selection, Q straightens, B / S / D / N / O / G + click place
//                 Branch / Sequence / Delay / Do N / Do Once / Gate, F7 compiles.
//   * touch       one finger pans and drags, two fingers zoom, a long press is the
//                 right click, tapping a pin offers the nodes that fit it; panels
//                 become sheets on a phone.

const LG_TYPE_COLORS = {exec: "#ffffff", bool: "#d93b3b", int: "#1fe0b0", float: "#9be36b", vector: "#f6c12b", text: "#f06bc8", actor: "#2aa7f5"};
const LG_TYPE_NAMES = {exec: "Exec", bool: "Boolean", int: "Integer", float: "Float", vector: "Vector", text: "Text", actor: "Actor"};
const LG_KIND_COLORS = {event: "#8f1d1d", action: "#1f4e8c", pure: "#3c7a3c", flow: "#5c5c5c"};
const LG_GRID = 16;
const LG_SNAP = 16;
const LG_ZOOMS = [0.1, 0.125, 0.15, 0.175, 0.2, 0.25, 0.3, 0.375, 0.5, 0.625, 0.75, 0.875, 1, 1.25, 1.5, 1.75, 2];
const LG_ZOOM_ONE = LG_ZOOMS.indexOf(1);
const LG_QUICK = {b: "flow.branch", s: "flow.sequence", d: "flow.delay", n: "flow.do_n", o: "flow.do_once", g: "flow.gate", l: "flow.for"};
const LG_COMMENT_COLORS = [0, 0x2f4f7f, 0x2f6f4f, 0x7f6a2f, 0x7f2f2f, 0x5f3f7f];
const lgEsc = s => String(s ?? "").replace(/[&<>"']/g, c => ({"&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;"}[c]));
const lgHex = n => "#" + (n >>> 0).toString(16).padStart(6, "0");
const lgNum = v => (typeof v === "number" ? (Math.abs(v) >= 1e4 || Number.isInteger(v) ? String(v) : String(+v.toFixed(4))) : String(v ?? ""));
const lgSnap = v => Math.round(v / LG_SNAP) * LG_SNAP;
const lgValueText = v => Array.isArray(v) ? v.map(lgNum).join(", ") : typeof v === "boolean" ? (v ? "true" : "false") : lgNum(v);

function lgPinIcon(type, linked) {
  const c = LG_TYPE_COLORS[type] || "#aaa";
  if (type === "exec")
    return `<svg class="lg-pin exec" viewBox="0 0 14 14"><path d="M2 1.8h5.2l5 5.2-5 5.2H2z" fill="${linked ? c : "none"}" stroke="${c}" stroke-width="1.5" stroke-linejoin="round"/></svg>`;
  return `<svg class="lg-pin" viewBox="0 0 14 14"><circle cx="6" cy="7" r="4.2" fill="${linked ? c : "none"}" stroke="${c}" stroke-width="1.7"/><path d="M11 4.6l2.4 2.4L11 9.4z" fill="${c}"/></svg>`;
}

const LG_ICONS = {
  event: '<path d="M8 1.5l6.5 6.5L8 14.5 1.5 8z" fill="currentColor"/>',
  action: '<path d="M3 2h8l3 3v9H3z" fill="none" stroke="currentColor" stroke-width="1.5"/><path d="M5 7h6M5 10h6" stroke="currentColor" stroke-width="1.3"/>',
  pure: '<path d="M11 2.5c-2.4-.6-3.6.4-4 2.6L5.4 13.5M4.5 6.5h5" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linecap="round"/>',
  flow: '<path d="M2 8h5l3-4h4M7 8l3 4h4" fill="none" stroke="currentColor" stroke-width="1.6"/>',
  latent: '<circle cx="8" cy="8" r="6" fill="none" stroke="currentColor" stroke-width="1.5"/><path d="M8 4.5V8l2.5 1.8" fill="none" stroke="currentColor" stroke-width="1.5"/>',
  graph: '<rect x="1.5" y="2.5" width="5" height="4" rx="1" fill="none" stroke="currentColor" stroke-width="1.4"/><rect x="9.5" y="9.5" width="5" height="4" rx="1" fill="none" stroke="currentColor" stroke-width="1.4"/><path d="M6.5 4.5h3a1.5 1.5 0 011.5 1.5v3.5" fill="none" stroke="currentColor" stroke-width="1.4"/>',
  machine: '<circle cx="4" cy="4.5" r="2.5" fill="none" stroke="currentColor" stroke-width="1.4"/><circle cx="12" cy="11.5" r="2.5" fill="none" stroke="currentColor" stroke-width="1.4"/><path d="M6 6l4 4" stroke="currentColor" stroke-width="1.4"/>',
  ok: '<circle cx="8" cy="8" r="7" fill="#3fa34d"/><path d="M4.5 8.2l2.3 2.3 4.7-4.8" fill="none" stroke="#fff" stroke-width="1.8"/>',
  warn: '<path d="M8 1.5l7 12.5H1z" fill="#e0a526"/><path d="M8 6v4M8 11.6v.8" stroke="#1b1b1b" stroke-width="1.7"/>',
  error: '<circle cx="8" cy="8" r="7" fill="#d23b3b"/><path d="M5.3 5.3l5.4 5.4M10.7 5.3l-5.4 5.4" stroke="#fff" stroke-width="1.8"/>',
  back: '<path d="M10 3L5 8l5 5" fill="none" stroke="currentColor" stroke-width="1.8"/>',
  save: '<path d="M2.5 2.5h9l2 2v9h-11z" fill="none" stroke="currentColor" stroke-width="1.4"/><path d="M5 2.5v4h5v-4M5 13.5v-4h6v4" fill="none" stroke="currentColor" stroke-width="1.3"/>',
  find: '<circle cx="7" cy="7" r="4.5" fill="none" stroke="currentColor" stroke-width="1.6"/><path d="M10.5 10.5L14 14" stroke="currentColor" stroke-width="1.8"/>',
  plus: '<path d="M8 3v10M3 8h10" stroke="currentColor" stroke-width="1.8"/>',
  menu: '<path d="M2.5 4h11M2.5 8h11M2.5 12h11" stroke="currentColor" stroke-width="1.6"/>',
  info: '<circle cx="8" cy="8" r="6.5" fill="none" stroke="currentColor" stroke-width="1.5"/><path d="M8 7v4.5M8 4.4v.9" stroke="currentColor" stroke-width="1.7"/>',
  box: '<rect x="2" y="2" width="12" height="12" rx="1.5" fill="none" stroke="currentColor" stroke-width="1.4" stroke-dasharray="2.5 2"/>',
  code: '<path d="M5.5 4L2 8l3.5 4M10.5 4L14 8l-3.5 4" fill="none" stroke="currentColor" stroke-width="1.6"/>',
  tpl: '<rect x="2" y="2" width="5" height="5" fill="none" stroke="currentColor" stroke-width="1.4"/><rect x="9" y="2" width="5" height="5" fill="none" stroke="currentColor" stroke-width="1.4"/><rect x="2" y="9" width="12" height="5" fill="none" stroke="currentColor" stroke-width="1.4"/>',
  help: '<circle cx="8" cy="8" r="6.5" fill="none" stroke="currentColor" stroke-width="1.5"/><path d="M6 6.2a2 2 0 113 1.7c-.7.4-1 .8-1 1.6M8 11.6v.8" fill="none" stroke="currentColor" stroke-width="1.5"/>',
  chevron: '<path d="M4 6l4 4 4-4" fill="none" stroke="currentColor" stroke-width="1.6"/>',
  close: '<path d="M4 4l8 8M12 4l-8 8" stroke="currentColor" stroke-width="1.7"/>',
  frame: '<path d="M2 5V2h3M11 2h3v3M14 11v3h-3M5 14H2v-3" fill="none" stroke="currentColor" stroke-width="1.6"/>',
};
const lgIco = (name, cls = "") => `<svg class="lg-i ${cls}" viewBox="0 0 16 16" aria-hidden="true">${LG_ICONS[name] || ""}</svg>`;

export class LogicEditor {
  /** host: element to fill; studio: {engine, state(), flushUi(), openMenu(entries, x, y), toast(text), uiAction(a)} */
  constructor(host, studio) {
    this.host = host;
    this.studio = studio;
    this.engine = studio.engine;
    this.data = null;
    this.key = "";
    this.views = new Map();          // owner:view -> {x, y, zoom, fitted}
    this.selection = new Set();      // node / comment / state ids
    this.selLink = null;             // selected wire [from, out, to, in]
    this.selVar = null;              // variable shown in Details
    this.selTransition = null;
    this.drag = null;
    this.palette = null;
    this.ranAt = new Map();          // node id -> time it last ran
    this.pins = new Map();           // "node:dir:pin" -> {dx, dy} offset from the node's corner
    this.nodePos = new Map();        // node id -> {x, y, w, h} (graph px)
    this.collapsed = new Set();      // My Blueprint sections
    this.keysDown = new Set();       // quick place keys held
    this.touches = new Map();
    this.panel = "";                 // phone: "my" | "details" | ""
    this.results = "compiler";       // bottom tab
    this.resultsOpen = false;
    this.findQuery = "";
    this.build();
  }

  get view() {
    const k = `${this.data?.owner ?? 0}:${this.data?.view ?? "graph"}`;
    if (!this.views.has(k)) this.views.set(k, {x: 120, y: 120, zoom: 1, fitted: false});
    return this.views.get(k);
  }
  get machine() { return this.data?.view?.startsWith("m:") ? this.data.machines?.find(m => "m:" + m.id === this.data.view) : null; }
  get phone() { return this.host.clientWidth < 760; }

  cmd(c) {
    const r = this.engine.command(c);
    this.studio.flushUi();
    if (r && r.ok === false && r.error) this.studio.toast(r.error);
    return r;
  }

  // ===========================================================================
  // skeleton
  build() {
    this.host.innerHTML = `
      <div class="lg-bar">
        <button class="lg-tb lg-back" title="Back to the level viewport (Esc)">${lgIco("back")}<span>Viewport</span></button>
        <button class="lg-tb lg-panelbtn lg-mybtn" title="My Blueprint">${lgIco("menu")}</button>
        <span class="lg-sep"></span>
        <button class="lg-tb lg-compile" title="Compile (F7): checks the graph; Play runs it as compiled machine code">${lgIco("ok", "lg-cstate")}<span>Compile</span></button>
        <button class="lg-tb lg-save" title="Save the level (Ctrl+S)">${lgIco("save")}<span>Save</span></button>
        <button class="lg-tb lg-findbtn" title="Find in Blueprint (Ctrl+F)">${lgIco("find")}<span>Find</span></button>
        <span class="lg-sep"></span>
        <label class="lg-ownerbox" title="Whose Blueprint: the level's or an actor's">${lgIco("graph")}<select class="lg-owner"></select></label>
        <div class="lg-spacer"></div>
        <button class="lg-tb lg-templates" title="Ready-made graphs">${lgIco("tpl")}<span>Templates</span>${lgIco("chevron", "lg-dd")}</button>
        <button class="lg-tb lg-cpp" title="Download the graph as C++ source: built into a game it runs as native code">${lgIco("code")}<span>C++</span></button>
        <button class="lg-tb lg-boxbtn" title="Box select (touch)">${lgIco("box")}</button>
        <button class="lg-tb lg-help" title="Shortcuts and how it works">${lgIco("help")}</button>
        <button class="lg-tb lg-panelbtn lg-detbtn" title="Details">${lgIco("info")}</button>
      </div>
      <div class="lg-main">
        <aside class="lg-my">
          <div class="lg-ph"><span>My Blueprint</span><button class="lg-x" data-close-panel>${lgIco("close")}</button></div>
          <div class="lg-search">${lgIco("find")}<input class="lg-myq" placeholder="Search" spellcheck="false"></div>
          <div class="lg-mylist"></div>
        </aside>
        <section class="lg-center">
          <div class="lg-tabs"></div>
          <div class="lg-canvas" tabindex="0">
            <div class="lg-water"></div>
            <div class="lg-world"><div class="lg-cboxes"></div><svg class="lg-wires"></svg><div class="lg-nodes"></div></div>
            <svg class="lg-temp"></svg>
            <div class="lg-marquee" hidden></div>
            <div class="lg-crumb"></div>
            <div class="lg-zoom"></div>
            <div class="lg-empty" hidden></div>
            <div class="lg-debug" hidden><span class="lg-dbg-t"></span><button class="lg-resume">Resume</button><button class="lg-step" title="Advance one simulation step">Step</button><button class="lg-clearbp" title="Remove all breakpoints">Clear Breakpoints</button></div>
            <button class="lg-fab" title="Add a node">${lgIco("plus")}</button>
            <button class="lg-fit" title="Frame everything (Home)">${lgIco("frame")}</button>
          </div>
          <div class="lg-results">
            <div class="lg-rtabs">
              <button data-r="compiler">Compiler Results <b class="lg-rcount"></b></button>
              <button data-r="find">Find Results</button>
              <div class="lg-spacer"></div>
              <button class="lg-rtoggle" title="Show / hide">${lgIco("chevron")}</button>
            </div>
            <div class="lg-rbody">
              <div class="lg-rfind"><div class="lg-search">${lgIco("find")}<input class="lg-findq" placeholder="Find nodes, pins, variables, comments" spellcheck="false"></div></div>
              <div class="lg-rlist"></div>
            </div>
          </div>
        </section>
        <aside class="lg-details">
          <div class="lg-ph"><span>Details</span><button class="lg-x" data-close-panel>${lgIco("close")}</button></div>
          <div class="lg-dbody"></div>
        </aside>
      </div>
      <div class="lg-scrim"></div>`;
    const $ = s => this.host.querySelector(s);
    this.el = {
      owner: $(".lg-owner"), tabs: $(".lg-tabs"), my: $(".lg-my"), mylist: $(".lg-mylist"), myq: $(".lg-myq"), details: $(".lg-details"), dbody: $(".lg-dbody"),
      canvas: $(".lg-canvas"), world: $(".lg-world"), cboxes: $(".lg-cboxes"), wires: $(".lg-wires"), nodes: $(".lg-nodes"), temp: $(".lg-temp"),
      marquee: $(".lg-marquee"), crumb: $(".lg-crumb"), water: $(".lg-water"), zoom: $(".lg-zoom"), empty: $(".lg-empty"), compile: $(".lg-compile"),
      results: $(".lg-results"), rlist: $(".lg-rlist"), rcount: $(".lg-rcount"), findq: $(".lg-findq"), boxbtn: $(".lg-boxbtn"),
    };
    $(".lg-back").onclick = () => this.cmd({cmd: "logicClose"});
    this.el.owner.onchange = () => this.cmd({cmd: "logicOpen", owner: +this.el.owner.value});
    this.el.compile.onclick = () => this.compile();
    $(".lg-save").onclick = () => this.studio.uiAction?.({action: "save"});
    $(".lg-findbtn").onclick = () => this.openFind();
    $(".lg-templates").onclick = e => this.templatesMenu(e.currentTarget);
    $(".lg-cpp").onclick = () => this.exportCpp();
    $(".lg-help").onclick = () => this.showHelp();
    $(".lg-mybtn").onclick = () => this.togglePanel("my");
    $(".lg-detbtn").onclick = () => this.togglePanel("details");
    $(".lg-scrim").onclick = () => this.togglePanel("");
    for (const b of this.host.querySelectorAll("[data-close-panel]")) b.onclick = () => this.togglePanel("");
    this.el.boxbtn.onclick = () => { this.boxMode = !this.boxMode; this.el.boxbtn.classList.toggle("on", this.boxMode); };
    $(".lg-fab").onclick = () => { const r = this.el.canvas.getBoundingClientRect(); this.openPalette(r.left + r.width / 2, r.top + r.height / 2, null, true); };
    $(".lg-fit").onclick = () => this.frame(false);
    $(".lg-resume").onclick = () => this.cmd({cmd: "pause", value: false});
    $(".lg-step").onclick = () => this.cmd({cmd: "stepFrame"});
    $(".lg-clearbp").onclick = () => this.cmd({cmd: "logicBreakpoint", node: 0, on: false, clearAll: true});
    this.el.myq.oninput = () => { this.myKey = ""; this.renderMy(this.data); };
    for (const b of this.host.querySelectorAll(".lg-rtabs [data-r]")) b.onclick = () => { this.results = b.dataset.r; this.resultsOpen = true; this.renderResults(); if (this.results === "find") this.el.findq.focus(); };
    $(".lg-rtoggle").onclick = () => { this.resultsOpen = !this.resultsOpen; this.renderResults(); };
    this.el.findq.oninput = () => { this.findQuery = this.el.findq.value; this.renderResults(); };
    this.el.findq.onkeydown = e => { e.stopPropagation(); if (e.key === "Escape") this.el.canvas.focus(); };
    this.el.myq.onkeydown = e => e.stopPropagation();
    this.initCanvas();
  }

  togglePanel(which) {
    this.panel = this.panel === which ? "" : which;
    this.host.classList.toggle("show-my", this.panel === "my");
    this.host.classList.toggle("show-details", this.panel === "details");
  }

  showHelp() {
    this.studio.toast("Right-click (long press): add a node · drag a pin: wire it, drop it in empty space for the nodes that fit · Alt+click: break wires · Ctrl+drag a pin: move its wires · " +
      "double-click a wire: reroute knot · C: comment · Q: straighten · Shift+W/A/S/D: align · B/S/D/N/O/G + click: Branch, Sequence, Delay, Do N, Do Once, Gate · " +
      "Ctrl+C/X/V/D: copy, cut, paste, duplicate · Del: delete · F: frame selection · Home: frame all · Ctrl+F: find · F7: compile · Ctrl+Z: undo. " +
      "Graphs compile to machine code when you press Play and run on the host; what they change reaches every player.", [], 20000);
  }

  templatesMenu(btn) {
    const r = btn.getBoundingClientRect();
    const t = (label, template) => ({label, run: () => this.cmd({cmd: "logicTemplate", template})});
    this.studio.openMenu([
      {header: "Add to this graph"},
      t("Door that opens on Interact", "door"),
      t("Score zone (overlap +10)", "score_zone"),
      t("Spawner (a cube every 3 s)", "spawner"),
      t("Traffic light (state machine)", "traffic_light"),
    ], r.left, r.bottom + 2);
  }

  exportCpp() {
    const r = this.cmd({cmd: "logicExportCpp"});
    if (!r?.text) return;
    const a = document.createElement("a");
    a.href = URL.createObjectURL(new Blob([r.text], {type: "text/x-c++src"}));
    a.download = r.file || "logic.cpp";
    a.click();
    setTimeout(() => URL.revokeObjectURL(a.href), 2000);
    this.studio.toast("C++ source downloaded: add it to your game's build (kern/build.sh ... " + (r.file || "logic.cpp") + ") and the graph runs as native code.");
  }

  compile() {
    const r = this.engine.command({cmd: "logicCompile"});
    const issues = r?.issues || [];
    const errors = issues.filter(i => i.error).length;
    this.results = "compiler";
    this.resultsOpen = issues.length > 0;
    this.renderResults();
    this.el.compile.classList.add("pulse");
    setTimeout(() => this.el.compile.classList.remove("pulse"), 500);
    this.studio.toast(errors ? `Compile: ${errors} error${errors > 1 ? "s" : ""} - see Compiler Results` : issues.length ? `Compiled with ${issues.length} warning${issues.length > 1 ? "s" : ""}` : "Compiled: ready to play");
  }

  openFind() {
    this.results = "find";
    this.resultsOpen = true;
    this.renderResults();
    this.el.findq.focus();
    this.el.findq.select();
  }

  // ===========================================================================
  // render
  render(data) {
    this.data = data;
    if (!data) return;
    this.host.classList.toggle("machine-view", data.view !== "graph");
    const owners = data.owners || [];
    const ownerKey = JSON.stringify(owners) + data.owner;
    if (ownerKey !== this.ownerKey) {
      this.ownerKey = ownerKey;
      const current = owners.some(o => o.id === data.owner) ? "" : `<option value="${data.owner}">${lgEsc(data.ownerName)} (new)</option>`;
      this.el.owner.innerHTML = current + owners.map(o => `<option value="${o.id}"${o.id === data.owner ? " selected" : ""}>${lgEsc(o.name)}</option>`).join("");
    }
    if (data.view !== "graph" && !this.machine) data.view = "graph";
    this.renderTabs(data);
    this.renderMy(data);
    const key = JSON.stringify([data.owner, data.view, data.nodes, data.links, data.machines, data.comments, data.issues]);
    if (key !== this.key) {
      this.key = key;
      // Drop selections that no longer exist.
      const ids = new Set([...(data.nodes || []).map(n => n.id), ...(data.comments || []).map(c => c.id), ...(this.machine?.states || []).map(s => s.id)]);
      for (const id of [...this.selection]) if (!ids.has(id)) this.selection.delete(id);
      if (this.selLink && !(data.links || []).some(l => l.join() === this.selLink.join())) this.selLink = null;
      if (data.view === "graph") this.renderGraph(data);
      else this.renderMachine(data, this.machine);
    }
    this.applyView();
    this.renderStatus(data);
    this.renderResults();
    this.renderDetails();
    this.renderDebug(data);
    // Focus work queued by an edit runs once the engine's answer is drawn (a render would replace it).
    const after = this.afterRender;
    this.afterRender = null;
    after?.();
  }

  renderTabs(data) {
    const machines = data.machines || [];
    const tabs = [{id: "graph", label: "Event Graph", icon: "graph"}, ...machines.map(m => ({id: "m:" + m.id, label: m.name, icon: "machine"}))];
    const k = JSON.stringify([tabs, data.view]);
    if (k === this.tabKey) return;
    this.tabKey = k;
    this.el.tabs.innerHTML = tabs.map(t => `<button class="lg-tab${t.id === data.view ? " active" : ""}" data-view="${t.id}">${lgIco(t.icon)}<span>${lgEsc(t.label)}</span></button>`).join("") +
      `<button class="lg-tab add" title="Add a state machine">${lgIco("plus")}</button>`;
    for (const b of this.el.tabs.querySelectorAll("[data-view]")) {
      b.onclick = () => this.openView(b.dataset.view);
      if (b.dataset.view.startsWith("m:")) {
        const id = +b.dataset.view.slice(2);
        b.ondblclick = () => this.renameMachine(id);
        b.oncontextmenu = e => { e.preventDefault(); this.machineMenu(id, e.clientX, e.clientY); };
      }
    }
    this.el.tabs.querySelector(".add").onclick = () => this.addMachine();
  }

  openView(view) {
    this.selection.clear();
    this.selLink = null;
    this.selTransition = null;
    this.cmd({cmd: "logicView", view});
  }

  addMachine() {
    const r = this.cmd({cmd: "logicMachine", op: "add"});
    if (r?.id) this.openView("m:" + r.id);
  }

  machineMenu(id, x, y) {
    this.studio.openMenu([{label: "Open", run: () => this.openView("m:" + id)}, {label: "Rename...", run: () => this.renameMachine(id)},
      {label: "Delete State Machine", run: () => this.cmd({cmd: "logicMachine", op: "remove", machine: id})}], x, y);
  }

  renderStatus(data) {
    const issues = data.issues || [];
    const errors = issues.filter(i => i.error).length, warns = issues.length - errors;
    const state = errors ? "error" : warns ? "warn" : "ok";
    const icon = this.el.compile.querySelector(".lg-cstate");
    if (icon.dataset.s !== state) { icon.dataset.s = state; icon.innerHTML = LG_ICONS[state]; }
    this.el.compile.title = errors ? `${errors} error${errors > 1 ? "s" : ""}: Play refuses graphs with errors (F7 lists them)` : warns ? `${warns} warning${warns > 1 ? "s" : ""}` : "Up to date: the graph is checked and ready";
    this.el.rcount.textContent = issues.length ? issues.length : "";
    this.el.rcount.className = "lg-rcount " + state;
    if (this.trace?.tier) {
      this.el.compile.querySelector("span").textContent = this.trace.disabled ? "Stopped" : this.trace.tier === "compiled (WebAssembly)" ? "Running · native" : "Running";
    } else this.el.compile.querySelector("span").textContent = "Compile";
  }

  // ---- My Blueprint ---------------------------------------------------------
  renderMy(data) {
    const live = this.trace?.variables || {};
    const q = this.el.myq.value.trim().toLowerCase();
    const k = JSON.stringify([data.owner, data.view, data.variables, data.events, data.machines?.map(m => [m.id, m.name]), live, q, [...this.collapsed], this.selVar]);
    if (k === this.myKey) return;
    this.myKey = k;
    const match = s => !q || s.toLowerCase().includes(q);
    const section = (id, title, add, body) => `<div class="lg-sec${this.collapsed.has(id) ? " closed" : ""}" data-sec="${id}">
        <div class="lg-sec-h"><button class="lg-sec-t">${lgIco("chevron", "lg-car")}${title}</button>${add ? `<button class="lg-add" data-add="${id}" title="${add}">${lgIco("plus")}</button>` : ""}</div>
        <div class="lg-sec-b">${body}</div></div>`;
    const graphs = [`<div class="lg-item${data.view === "graph" ? " on" : ""}" data-graph="graph">${lgIco("graph")}<span>Event Graph</span></div>`,
      ...(data.machines || []).filter(m => match(m.name)).map(m => `<div class="lg-item${data.view === "m:" + m.id ? " on" : ""}" data-graph="m:${m.id}">${lgIco("machine")}<span>${lgEsc(m.name)}</span></div>`)].join("");
    const vars = (data.variables || []).filter(v => match(v.name)).map(v => `
        <div class="lg-item lg-var${this.selVar === v.name ? " on" : ""}" draggable="true" data-var="${lgEsc(v.name)}" title="Drag into the graph: Ctrl = Get, Alt = Set">
          <i class="lg-pill" style="--c:${LG_TYPE_COLORS[v.type]}"></i><span class="lg-vn">${lgEsc(v.name)}</span>
          ${v.name in live ? `<span class="lg-live" title="Value while playing">${lgEsc(lgValueText(live[v.name]))}</span>` : `<span class="lg-vt">${LG_TYPE_NAMES[v.type] || v.type}</span>`}
          <span class="lg-acts"><button data-get title="Add a Get node">Get</button><button data-set title="Add a Set node">Set</button></span>
        </div>`).join("") || `<div class="lg-none">No variables${q ? " match" : " yet: + adds one"}</div>`;
    const events = (data.events || []).filter(match).map(e => `
        <div class="lg-item lg-ev" draggable="true" data-ev="${lgEsc(e)}" title="Drag into the graph: the event; Ctrl = Call">${lgIco("event", "red")}<span class="lg-vn">${lgEsc(e)}</span>
          <span class="lg-acts"><button data-on title="Add the event node">Event</button><button data-call title="Add a Call node">Call</button></span></div>`).join("") ||
      `<div class="lg-none">Custom events you add appear here</div>`;
    this.el.mylist.innerHTML = section("graphs", "Graphs", "Add a state machine", graphs) + section("vars", "Variables", "Add a variable", vars) + section("events", "Events", "Add a custom event", events);
    for (const s of this.el.mylist.querySelectorAll(".lg-sec")) {
      s.querySelector(".lg-sec-t").onclick = () => { const id = s.dataset.sec; if (this.collapsed.has(id)) this.collapsed.delete(id); else this.collapsed.add(id); this.myKey = ""; this.renderMy(this.data); };
      const add = s.querySelector("[data-add]");
      if (add) add.onclick = () => s.dataset.sec === "graphs" ? this.addMachine() : s.dataset.sec === "vars" ? this.addVariable() : this.addCustomEvent();
    }
    for (const it of this.el.mylist.querySelectorAll("[data-graph]")) {
      it.onclick = () => { this.openView(it.dataset.graph); if (this.phone) this.togglePanel(""); };
      if (it.dataset.graph.startsWith("m:")) it.oncontextmenu = e => { e.preventDefault(); this.machineMenu(+it.dataset.graph.slice(2), e.clientX, e.clientY); };
    }
    for (const row of this.el.mylist.querySelectorAll(".lg-var")) {
      const name = row.dataset.var;
      row.onclick = e => { if (e.target.closest("button")) return; this.selectVariable(name); };
      row.querySelector("[data-get]").onclick = () => this.addAtCenter("var.get", {variable: name});
      row.querySelector("[data-set]").onclick = () => this.addAtCenter("var.set", {variable: name});
      row.ondragstart = e => { e.dataTransfer.setData("text/x-kern-var", name); e.dataTransfer.effectAllowed = "copy"; };
      row.oncontextmenu = e => { e.preventDefault(); this.variableMenu(name, e.clientX, e.clientY); };
    }
    for (const row of this.el.mylist.querySelectorAll(".lg-ev")) {
      const name = row.dataset.ev;
      row.querySelector("[data-on]").onclick = () => this.addAtCenter("event.custom", {name});
      row.querySelector("[data-call]").onclick = () => this.addAtCenter("flow.call", {name});
      row.ondragstart = e => { e.dataTransfer.setData("text/x-kern-event", name); e.dataTransfer.effectAllowed = "copy"; };
      row.ondblclick = () => this.findText(name);
    }
  }

  selectVariable(name) {
    this.selVar = name;
    this.selection.clear();
    this.selLink = null;
    this.markSelection();
    this.myKey = "";
    this.renderMy(this.data);
    this.renderDetails();
    if (this.phone) this.togglePanel("details");
  }

  variableMenu(name, x, y) {
    this.studio.openMenu([
      {header: name},
      {label: "Get " + name, run: () => this.addAtCenter("var.get", {variable: name})},
      {label: "Set " + name, run: () => this.addAtCenter("var.set", {variable: name})},
      {label: "Find References", run: () => this.findText(name)},
      {label: "Rename / Edit...", run: () => this.selectVariable(name)},
      {separator: true},
      {label: "Delete Variable", run: () => { if (this.selVar === name) this.selVar = null; this.cmd({cmd: "logicVariable", op: "remove", name}); }},
    ], x, y);
  }

  addVariable() {
    const r = this.cmd({cmd: "logicVariable", op: "add", name: "NewVar", type: "float"});
    if (r?.name) {
      this.selectVariable(r.name);
      this.afterRender = () => { const f = this.el.dbody.querySelector("[data-vk='name']"); f?.focus(); f?.select(); };
    }
  }

  addCustomEvent() {
    const used = new Set(this.data.events || []);
    let name = "MyEvent";
    for (let i = 2; used.has(name); i++) name = "MyEvent" + i;
    this.addAtCenter("event.custom", {name});
  }

  // ---- event graph ------------------------------------------------------------
  renderGraph(data) {
    const nodes = data.nodes || [];
    this.el.empty.hidden = nodes.length > 0 || (data.comments || []).length > 0;
    this.el.empty.innerHTML = `<div class="lg-e1"><b>${lgEsc(data.ownerName)}</b> has no logic yet</div><div class="lg-e2">Right-click (long press) anywhere to add a node, or start with</div>
      <div class="lg-starts"><button data-t="event.begin">Begin Play</button><button data-t="game.interact">On Interact</button><button data-t="game.overlap_begin">On Begin Overlap</button><button data-t="event.tick">Event Tick</button><button data-tpl>Templates…</button></div>`;
    for (const b of this.el.empty.querySelectorAll("[data-t]")) b.onclick = () => this.addAtCenter(b.dataset.t);
    this.el.empty.querySelector("[data-tpl]").onclick = () => this.templatesMenu(this.host.querySelector(".lg-templates"));
    this.el.crumb.innerHTML = `${lgEsc(data.ownerName)} <span>›</span> Event Graph`;
    this.el.water.textContent = "EVENT GRAPH";
    const actors = this.studio.state()?.actors || [];
    this.el.cboxes.innerHTML = (data.comments || []).map(c => this.commentHtml(c)).join("");
    this.el.nodes.innerHTML = nodes.map(n => this.nodeHtml(n, actors)).join("");
    this.bindNodeInputs();
    this.measure();
    this.markSelection();
    this.drawWires();
    const v = this.view;
    if (!v.fitted && (nodes.length || data.comments?.length)) { v.fitted = true; this.frame(false); }
  }

  nodeTitle(n) { return n.title; }

  nodeHtml(n, actors) {
    const pinRow = (p, dir, compact) => {
      let lit = "";
      if (dir === "in" && p.type !== "exec" && !p.linked) lit = this.literalHtml(n, p, actors);
      const label = p.label && !compact ? `<span class="lg-pl">${lgEsc(p.label)}</span>` : "";
      const tip = `${LG_TYPE_NAMES[p.type] || p.type}${p.label ? " · " + p.label : ""}`;
      return `<div class="lg-row ${dir}" data-pin="${lgEsc(p.id)}" data-dir="${dir}" data-type="${p.type}" title="${lgEsc(tip)}">${dir === "in" ? lgPinIcon(p.type, p.linked) + label + lit : label + lgPinIcon(p.type, p.linked)}</div>`;
    };
    const cls = `lg-node k-${n.kind}${n.error ? " error" : n.issues ? " warn" : ""}${n.breakpoint ? " bp" : ""}${n.hit ? " hit" : ""}`;
    const tip = lgEsc((n.issues ? n.issues.join("\n") + "\n\n" : "") + (n.help || ""));
    const bubble = (n.comment ? `<div class="lg-bubble">${lgEsc(n.comment)}</div>` : "") + (n.breakpoint ? `<i class="lg-bpdot" title="Breakpoint (F9)"></i>` : "");
    const badge = n.error ? `<div class="lg-badge err">ERROR</div>` : n.issues ? `<div class="lg-badge warn">WARNING</div>` : "";
    const pos = `left:${n.x}px;top:${n.y}px`;
    if (n.compact === "reroute") {
      const t = n.inputs[0]?.type || "exec";
      return `<div class="lg-node knot" data-id="${n.id}" style="${pos};--c:${LG_TYPE_COLORS[t]}" title="Reroute node">
        <div class="lg-row in" data-pin="${lgEsc(n.inputs[0]?.id || "in")}" data-dir="in" data-type="${t}"></div><i class="lg-knot"></i>
        <div class="lg-row out" data-pin="${lgEsc(n.outputs[0]?.id || "out")}" data-dir="out" data-type="${t}"></div></div>`;
    }
    if (n.compact === "get") {
      const o = n.outputs[0];
      const name = (n.params || []).find(p => p.key === "variable")?.value || n.title;
      return `<div class="${cls} compact var" data-id="${n.id}" style="${pos};--c:${LG_TYPE_COLORS[o?.type] || "#888"}" title="${tip}">${bubble}
        <span class="lg-vname">${lgEsc(name)}</span>${o ? pinRow(o, "out", true) : ""}${badge}</div>`;
    }
    if (n.compact) {
      const sym = n.compact;
      return `<div class="${cls} compact op" data-id="${n.id}" style="${pos}" title="${lgEsc(n.title)}\n${tip}">${bubble}
        <div class="lg-col in">${n.inputs.map(p => pinRow(p, "in", true)).join("")}</div>
        <div class="lg-sym${sym.length > 2 ? " word" : ""}">${lgEsc(sym)}</div>
        <div class="lg-col out">${n.outputs.map(p => pinRow(p, "out", true)).join("")}</div>${badge}</div>`;
    }
    const head = n.color ? lgHex(n.color) : LG_KIND_COLORS[n.kind] || LG_KIND_COLORS.action;
    const icon = n.kind === "event" ? "event" : n.latent ? "latent" : n.kind === "pure" ? "pure" : n.kind === "flow" ? "flow" : "action";
    const params = (n.params || []).map(p => `<div class="lg-param"><span>${lgEsc(p.label)}</span>${this.paramHtml(n, p)}</div>`).join("");
    // Exec pins first (UE: the exec row sits under the title), then data.
    const ins = n.inputs.map(p => pinRow(p, "in")).join(""), outs = n.outputs.map(p => pinRow(p, "out")).join("");
    return `<div class="${cls}" data-id="${n.id}" style="${pos};--h:${head}" title="${tip}">${bubble}
      <div class="lg-nh">${lgIco(icon, "lg-ki")}<span class="lg-title">${lgEsc(n.title)}</span>${n.latent ? `<span class="lg-clock" title="Latent: waits">${lgIco("latent")}</span>` : ""}</div>
      ${params ? `<div class="lg-params">${params}</div>` : ""}
      <div class="lg-pins"><div class="lg-col in">${ins}</div><div class="lg-col out">${outs}</div></div>${badge}
    </div>`;
  }

  literalHtml(n, p, actors) {
    const v = p.value;
    const attr = `data-lit="${lgEsc(p.id)}" data-node="${n.id}" data-t="${p.type}"`;
    switch (p.type) {
      case "bool": return `<input type="checkbox" class="lg-lit chk" ${attr}${v ? " checked" : ""}>`;
      case "int":
      case "float": return `<input class="lg-lit num" ${attr} value="${lgEsc(lgNum(v))}" spellcheck="false" inputmode="decimal">`;
      case "text": return `<input class="lg-lit txt" ${attr} value="${lgEsc(v)}" spellcheck="false">`;
      case "vector": return `<span class="lg-vec" ${attr}>${["X", "Y", "Z"].map((a, i) => `<label><i>${a}</i><input class="lg-lit num v" data-i="${i}" value="${lgEsc(lgNum(Array.isArray(v) ? v[i] : 0))}" spellcheck="false" inputmode="decimal"></label>`).join("")}</span>`;
      case "actor": return `<select class="lg-lit act" ${attr}><option value="0">${p.id === "target" || p.id === "a" ? "Self" : "None"}</option>${actors.map(([id, name]) => `<option value="${id}"${id === v ? " selected" : ""}>${lgEsc(name)}</option>`).join("")}</select>`;
      default: return "";
    }
  }

  paramHtml(n, p) {
    const attr = `data-param="${lgEsc(p.key)}" data-node="${n.id}"`;
    const choices = p.choices || [];
    if (["choice", "variable", "machine", "state", "sound"].includes(p.kind)) {
      const has = choices.some(([v]) => v === p.value);
      return `<select class="lg-par" ${attr}>${has ? "" : `<option value="${lgEsc(p.value)}" selected>${lgEsc(p.value || "(none)")}</option>`}${choices.map(([v, l]) => `<option value="${lgEsc(v)}"${v === p.value ? " selected" : ""}>${lgEsc(l)}</option>`).join("")}</select>`;
    }
    if (p.kind === "event") {
      const list = `lgev-${n.id}-${p.key}`;
      return `<input class="lg-par" ${attr} value="${lgEsc(p.value)}" list="${list}" spellcheck="false"><datalist id="${list}">${choices.map(([v]) => `<option value="${lgEsc(v)}">`).join("")}</datalist>`;
    }
    return `<input class="lg-par" ${attr} value="${lgEsc(p.value)}" spellcheck="false"${p.kind === "integer" ? ' inputmode="numeric"' : ""}>`;
  }

  /** Edits of inline values (on nodes and in Details) go to the engine on change. */
  bindInputs(root) {
    const send = (node, pin, value) => this.cmd({cmd: "logicValue", node, pin, value});
    for (const el of root.querySelectorAll(".lg-lit")) {
      el.addEventListener("pointerdown", e => e.stopPropagation());
      el.addEventListener("keydown", e => { e.stopPropagation(); if (e.key === "Enter") el.blur(); });
      if (el.classList.contains("v")) {
        el.onchange = () => {
          const box = el.closest(".lg-vec");
          send(+box.dataset.node, box.dataset.lit, [...box.querySelectorAll("input")].map(i => +i.value || 0));
        };
        continue;
      }
      const node = +el.dataset.node, pin = el.dataset.lit;
      if (el.type === "checkbox") el.onchange = () => send(node, pin, el.checked);
      else if (el.tagName === "SELECT") el.onchange = () => send(node, pin, +el.value);
      else el.onchange = () => send(node, pin, el.dataset.t === "text" ? el.value : (+el.value || 0));
    }
    for (const el of root.querySelectorAll(".lg-par")) {
      el.addEventListener("pointerdown", e => e.stopPropagation());
      el.addEventListener("keydown", e => { e.stopPropagation(); if (e.key === "Enter") el.blur(); });
      el.onchange = () => this.cmd({cmd: "logicParam", node: +el.dataset.node, key: el.dataset.param, value: el.value});
    }
  }
  bindNodeInputs() { this.bindInputs(this.el.nodes); }

  commentHtml(c) {
    const color = c.color ? lgHex(c.color) : "#5a5a5a";
    return `<div class="lg-cbox${this.selection.has(c.id) ? " selected" : ""}" data-cid="${c.id}" style="left:${c.x}px;top:${c.y}px;width:${c.w}px;height:${c.h}px;--c:${color}">
      <div class="lg-ch">${lgEsc(c.text)}</div><div class="lg-cres" title="Resize"></div></div>`;
  }

  // ---- geometry ---------------------------------------------------------------
  /** Pin offsets from each node's corner, measured once after a render (moves reuse them). */
  measure() {
    this.pins.clear();
    this.nodePos.clear();
    const z = this.view.zoom;
    for (const el of this.el.nodes.querySelectorAll(".lg-node")) {
      const id = +el.dataset.id;
      const x = parseFloat(el.style.left), y = parseFloat(el.style.top);
      const nr = el.getBoundingClientRect();
      this.nodePos.set(id, {x, y, w: el.offsetWidth, h: el.offsetHeight});
      if (el.classList.contains("knot")) {
        const k = el.querySelector(".lg-knot").getBoundingClientRect();
        const c = {dx: (k.left + k.width / 2 - nr.left) / z, dy: (k.top + k.height / 2 - nr.top) / z};
        for (const row of el.querySelectorAll(".lg-row")) this.pins.set(`${id}:${row.dataset.dir}:${row.dataset.pin}`, c);
        continue;
      }
      for (const row of el.querySelectorAll(".lg-row")) {
        const pin = row.querySelector(".lg-pin");
        if (!pin) continue;
        const r = pin.getBoundingClientRect();
        const cx = row.dataset.dir === "out" ? r.left + r.width * (row.dataset.type === "exec" ? 0.62 : 0.5) : r.left + r.width * 0.42;
        this.pins.set(`${id}:${row.dataset.dir}:${row.dataset.pin}`, {dx: (cx - nr.left) / z, dy: (r.top + r.height / 2 - nr.top) / z});
      }
    }
  }

  pinPoint(node, pin, dir) {
    const off = this.pins.get(`${node}:${dir}:${pin}`), p = this.nodePos.get(node);
    if (!off || !p) return null;
    return {x: p.x + off.dx, y: p.y + off.dy};
  }

  /** Unreal's spline: horizontal tangents that grow with the distance; backward wires loop out. */
  wirePath(a, b) {
    const dx = b.x - a.x, dy = b.y - a.y;
    const t = dx >= 0 ? Math.min(320, Math.max(36, dx * 0.55)) : Math.min(420, Math.max(90, -dx * 0.9 + Math.abs(dy) * 0.25));
    return `M${a.x.toFixed(1)},${a.y.toFixed(1)}C${(a.x + t).toFixed(1)},${a.y.toFixed(1)} ${(b.x - t).toFixed(1)},${b.y.toFixed(1)} ${b.x.toFixed(1)},${b.y.toFixed(1)}`;
  }

  drawWires() {
    if (!this.data || this.data.view !== "graph") return;
    const types = {};
    for (const n of this.data.nodes || []) for (const p of n.outputs) types[n.id + ":" + p.id] = p.type;
    const now = performance.now();
    const sel = this.selLink?.join();
    let lines = "", hits = "";
    (this.data.links || []).forEach(([from, out, to, inp], k) => {
      const a = this.pinPoint(from, out, "out"), b = this.pinPoint(to, inp, "in");
      if (!a || !b) return;
      const t = types[from + ":" + out] || "exec";
      const d = this.wirePath(a, b);
      const ran = t === "exec" && (this.ranAt.get(to) || 0) > now - 600;
      const hot = this.selection.has(from) || this.selection.has(to);
      const selected = sel === [from, out, to, inp].join();
      lines += `<path class="lg-wire${t === "exec" ? " exec" : ""}${ran ? " ran" : ""}${hot ? " hot" : ""}${selected ? " sel" : ""}" d="${d}" stroke="${LG_TYPE_COLORS[t] || "#aaa"}"/>`;
      hits += `<path class="lg-hit" data-k="${k}" d="${d}"/>`;
    });
    this.el.wires.innerHTML = lines + hits;
  }

  // ---- state machine (Unreal's state machine graph) ----------------------------
  renderMachine(data, m) {
    this.el.crumb.innerHTML = `${lgEsc(data.ownerName)} <span>›</span> ${lgEsc(m?.name || "")}`;
    this.el.water.textContent = "STATE MACHINE";
    this.el.cboxes.innerHTML = "";
    this.el.empty.hidden = !!m?.states?.length;
    this.el.empty.innerHTML = `<div class="lg-e2">Double-click (or long press) to add a state</div>`;
    if (!m) { this.el.nodes.innerHTML = ""; this.el.wires.innerHTML = ""; return; }
    const current = this.trace?.states?.[m.name];
    const initial = m.states.find(s => s.initial);
    const out = {};
    for (const t of m.transitions) (out[t.from] ||= []).push(t);
    let html = initial ? `<div class="lg-entry" style="left:${initial.x - 150}px;top:${initial.y + 10}px">Entry</div>` : "";
    html += m.states.map(s => `
      <div class="lg-state${s.initial ? " initial" : ""}${current === s.name ? " current" : ""}" data-state="${s.id}" style="left:${s.x}px;top:${s.y}px">
        <div class="lg-sh">${lgEsc(s.name)}</div>
        <div class="lg-sb">${(out[s.id] || []).length} transition${(out[s.id] || []).length === 1 ? "" : "s"}</div>
        <div class="lg-handle" title="Drag to another state: a transition (into empty space: a new state)">${lgIco("plus")}</div>
      </div>`).join("");
    if (m.transitions.some(t => !t.from)) html += `<div class="lg-state any" data-state="0" style="left:-260px;top:-60px"><div class="lg-sh">Any State</div><div class="lg-sb">transitions from every state</div><div class="lg-handle">${lgIco("plus")}</div></div>`;
    this.el.nodes.innerHTML = html;
    this.markSelection();
    this.drawTransitions(m);
    const v = this.view;
    if (!v.fitted && m.states.length) { v.fitted = true; this.frame(false); }
  }

  stateRect(id) {
    const el = this.el.nodes.querySelector(`[data-state="${id}"]`);
    if (!el) return null;
    return {x: parseFloat(el.style.left), y: parseFloat(el.style.top), w: el.offsetWidth, h: el.offsetHeight};
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
    // Straight arrows between the state boxes with a round transition icon in the middle; a pair of
    // opposite transitions runs side by side.
    const edge = (r, from, to) => {
      const cx = r.x + r.w / 2, cy = r.y + r.h / 2, dx = to.x - from.x, dy = to.y - from.y;
      const sx = dx ? (r.w / 2) / Math.abs(dx) : Infinity, sy = dy ? (r.h / 2) / Math.abs(dy) : Infinity, s = Math.min(sx, sy);
      return {x: cx + dx * s, y: cy + dy * s};
    };
    let svg = `<defs><marker id="lgArrow" viewBox="0 0 10 10" refX="8" refY="5" markerWidth="8" markerHeight="8" orient="auto"><path d="M0 0L10 5L0 10z" fill="#e6e6e6"/></marker></defs>`;
    let icons = "";
    const sel = this.selTransition;
    for (const t of m.transitions) {
      const a = this.stateRect(t.from), b = this.stateRect(t.to);
      if (!a || !b) continue;
      const ca = {x: a.x + a.w / 2, y: a.y + a.h / 2}, cb = {x: b.x + b.w / 2, y: b.y + b.h / 2};
      const len = Math.hypot(cb.x - ca.x, cb.y - ca.y) || 1, nx = -(cb.y - ca.y) / len * 9, ny = (cb.x - ca.x) / len * 9;
      const twin = m.transitions.some(o => o.from === t.to && o.to === t.from);
      const oa = twin ? {x: ca.x + nx, y: ca.y + ny} : ca, ob = twin ? {x: cb.x + nx, y: cb.y + ny} : cb;
      const p1 = edge(a, oa, ob), p2 = edge(b, ob, oa);
      svg += `<path class="lg-trans${sel === t.id ? " sel" : ""}" d="M${p1.x},${p1.y}L${p2.x},${p2.y}" marker-end="url(#lgArrow)"/>`;
      const mid = {x: (p1.x + p2.x) / 2, y: (p1.y + p2.y) / 2};
      const glyph = t.trigger === "timer" ? "⏱" : t.trigger === "condition" ? "✓" : t.trigger === "auto" ? "→" : "⚡";
      icons += `<div class="lg-tico${sel === t.id ? " sel" : ""}" data-t="${t.id}" style="left:${mid.x}px;top:${mid.y}px" title="${lgEsc(t.label || this.triggerText(t))}">${glyph}<span>${lgEsc(t.label || this.triggerText(t))}</span></div>`;
    }
    this.el.wires.innerHTML = svg;
    for (const e of this.el.nodes.querySelectorAll(".lg-tico")) e.remove();
    this.el.nodes.insertAdjacentHTML("beforeend", icons);
  }

  // ---- selection --------------------------------------------------------------
  markSelection() {
    for (const n of this.el.nodes.querySelectorAll(".lg-node, .lg-state")) n.classList.toggle("selected", this.selection.has(+(n.dataset.id ?? n.dataset.state)));
    for (const c of this.el.cboxes.querySelectorAll(".lg-cbox")) c.classList.toggle("selected", this.selection.has(+c.dataset.cid));
  }

  select(ids, add = false) {
    if (!add) this.selection.clear();
    for (const id of ids) this.selection.add(id);
    this.selLink = null;
    this.selTransition = null;
    if (ids.length) this.selVar = null;
    this.markSelection();
    if (this.data?.view === "graph") this.drawWires();
    else if (this.machine) this.drawTransitions(this.machine);
    this.renderDetails();
  }

  // ---- details ------------------------------------------------------------------
  renderDetails() {
    const d = this.data;
    if (!d) return;
    const ids = [...this.selection];
    const node = ids.length === 1 ? d.nodes?.find(n => n.id === ids[0]) : null;
    const box = ids.length === 1 ? d.comments?.find(c => c.id === ids[0]) : null;
    const m = this.machine;
    const state = ids.length === 1 && m ? m.states.find(s => s.id === ids[0]) : null;
    const trans = this.selTransition && m ? m.transitions.find(t => t.id === this.selTransition) : null;
    const variable = this.selVar ? d.variables?.find(v => v.name === this.selVar) : null;
    const k = JSON.stringify([node, box, state, trans, variable, ids.length, this.selLink, d.variables?.length, d.events, this.trace?.variables?.[this.selVar]]);
    if (k === this.detKey) return;
    // Do not rebuild under the user's typing.
    if (this.el.dbody.contains(document.activeElement) && document.activeElement.matches("input, textarea") && this.detSame?.(node, box, state, trans, variable)) return;
    this.detKey = k;
    this.detSame = (n2, b2, s2, t2, v2) => (n2?.id ?? -1) === (node?.id ?? -1) && (b2?.id ?? -1) === (box?.id ?? -1) && (s2?.id ?? -1) === (state?.id ?? -1) && (t2?.id ?? -1) === (trans?.id ?? -1) && (v2?.name ?? "") === (variable?.name ?? "");
    const b = this.el.dbody;
    if (node) b.innerHTML = this.nodeDetails(node);
    else if (box) b.innerHTML = this.commentDetails(box);
    else if (trans) b.innerHTML = this.transitionDetails(m, trans);
    else if (state) b.innerHTML = this.stateDetails(m, state);
    else if (variable) b.innerHTML = this.variableDetails(variable);
    else if (ids.length > 1) b.innerHTML = `<div class="lg-dh">${ids.length} selected</div><div class="lg-dsec"><div class="lg-dtxt">Drag one to move them all. C puts a comment around them; Q straightens their wires; the context menu aligns them.</div>
      <div class="lg-dacts"><button data-act="comment">Comment</button><button data-act="straighten">Straighten</button><button data-act="delete" class="danger">Delete</button></div></div>`;
    else b.innerHTML = this.graphDetails();
    this.bindDetails(node, box, m, state, trans, variable);
  }

  nodeDetails(n) {
    const actors = this.studio.state()?.actors || [];
    const head = n.color ? lgHex(n.color) : LG_KIND_COLORS[n.kind] || LG_KIND_COLORS.action;
    const inputs = n.inputs.filter(p => p.type !== "exec");
    const rows = inputs.map(p => `<div class="lg-drow"><label>${lgPinIcon(p.type, p.linked)}${lgEsc(p.label || p.id)}</label><div>${p.linked ? `<span class="lg-dim">wired</span>` : this.literalHtml(n, p, actors)}</div></div>`).join("");
    const params = (n.params || []).map(p => `<div class="lg-drow"><label>${lgEsc(p.label)}</label><div>${this.paramHtml(n, p)}</div></div>`).join("");
    const issues = (n.issues || []).map(t => `<div class="lg-dissue ${n.error ? "err" : "warn"}">${lgIco(n.error ? "error" : "warn")}${lgEsc(t)}</div>`).join("");
    return `<div class="lg-dh"><i class="lg-dhc" style="background:${head}"></i>${lgEsc(n.title)}</div>
      <div class="lg-dsub">${lgEsc(n.category || "")}${n.kind ? " · " + lgEsc({event: "Event", action: "Function", pure: "Pure function", flow: "Flow control"}[n.kind] || n.kind) : ""}</div>
      ${issues}
      ${n.help ? `<div class="lg-dtxt">${lgEsc(n.help)}</div>` : ""}
      ${params ? `<div class="lg-dsec"><div class="lg-dst">Settings</div>${params}</div>` : ""}
      ${rows ? `<div class="lg-dsec"><div class="lg-dst">Inputs</div>${rows}</div>` : ""}
      <div class="lg-dsec"><div class="lg-dst">Comment</div><textarea class="lg-dcomment" rows="2" placeholder="A note shown above the node">${lgEsc(n.comment || "")}</textarea></div>
      <div class="lg-dacts"><button data-act="duplicate">Duplicate</button><button data-act="break">Break Links</button><button data-act="delete" class="danger">Delete</button></div>`;
  }

  commentDetails(c) {
    return `<div class="lg-dh"><i class="lg-dhc" style="background:${c.color ? lgHex(c.color) : "#5a5a5a"}"></i>Comment</div>
      <div class="lg-dsec"><div class="lg-dst">Text</div><textarea class="lg-dctext" rows="3">${lgEsc(c.text)}</textarea></div>
      <div class="lg-dsec"><div class="lg-dst">Color</div><div class="lg-swatches">${LG_COMMENT_COLORS.map(col => `<button data-col="${col}" class="${(c.color || 0) === col ? "on" : ""}" style="background:${col ? lgHex(col) : "#5a5a5a"}"></button>`).join("")}</div></div>
      <div class="lg-dtxt">Moving the comment moves the nodes inside it. Drag its corner to resize.</div>
      <div class="lg-dacts"><button data-act="delete" class="danger">Delete Comment</button></div>`;
  }

  variableDetails(v) {
    const live = this.trace?.variables?.[v.name];
    const val = v.type === "bool" ? `<input type="checkbox" data-vk="value"${v.value ? " checked" : ""}>`
      : v.type === "vector" ? `<input data-vk="value" value="${lgEsc((v.value || [0, 0, 0]).join(", "))}">`
      : v.type === "actor" ? `<span class="lg-dim">none (set it while playing)</span>`
      : `<input data-vk="value" value="${lgEsc(lgNum(v.value))}">`;
    const refs = (this.data.nodes || []).filter(n => (n.params || []).some(p => p.key === "variable" && p.value === v.name)).length;
    return `<div class="lg-dh"><i class="lg-pill big" style="--c:${LG_TYPE_COLORS[v.type]}"></i>${lgEsc(v.name)}</div><div class="lg-dsub">Variable</div>
      <div class="lg-dsec"><div class="lg-dst">Variable</div>
        <div class="lg-drow"><label>Name</label><div><input data-vk="name" value="${lgEsc(v.name)}" spellcheck="false"></div></div>
        <div class="lg-drow"><label>Type</label><div><select data-vk="type">${["bool", "int", "float", "vector", "text", "actor"].map(t => `<option value="${t}"${t === v.type ? " selected" : ""}>${LG_TYPE_NAMES[t]}</option>`).join("")}</select></div></div>
      </div>
      <div class="lg-dsec"><div class="lg-dst">Default Value</div><div class="lg-drow"><label>${lgEsc(v.name)}</label><div>${val}</div></div>
        ${live !== undefined ? `<div class="lg-drow"><label>While playing</label><div class="lg-live">${lgEsc(lgValueText(live))}</div></div>` : ""}</div>
      <div class="lg-dsec"><div class="lg-dst">References</div><div class="lg-dtxt">${refs} node${refs === 1 ? "" : "s"} use${refs === 1 ? "s" : ""} ${lgEsc(v.name)}.</div></div>
      <div class="lg-dacts"><button data-act="get">Add Get</button><button data-act="set">Add Set</button><button data-act="refs">Find</button><button data-act="delete" class="danger">Delete</button></div>`;
  }

  transitionDetails(m, t) {
    const vars = (this.data.variables || []).filter(v => v.type === "bool");
    return `<div class="lg-dh">Transition</div><div class="lg-dsub">${lgEsc(m.states.find(s => s.id === t.from)?.name || "Any State")} → ${lgEsc(m.states.find(s => s.id === t.to)?.name || "?")}</div>
      <div class="lg-dsec"><div class="lg-dst">Rule</div>
        <div class="lg-drow"><label>When</label><div><select data-tk="trigger">${[["event", "an event is called"], ["timer", "time in the state passed"], ["condition", "a bool variable is true"], ["auto", "immediately"]]
          .map(([v, l]) => `<option value="${v}"${v === t.trigger ? " selected" : ""}>${l}</option>`).join("")}</select></div></div>
        ${t.trigger === "event" ? `<div class="lg-drow"><label>Event</label><div><input data-tk="event" value="${lgEsc(t.event || "")}" list="lgTrEv"><datalist id="lgTrEv">${(this.data.events || []).map(e => `<option value="${lgEsc(e)}">`).join("")}</datalist></div></div>` : ""}
        ${t.trigger === "timer" ? `<div class="lg-drow"><label>Seconds</label><div><input data-tk="seconds" value="${t.seconds ?? 1}" inputmode="decimal"></div></div>` : ""}
        ${t.trigger === "condition" ? `<div class="lg-drow"><label>Variable</label><div><select data-tk="variable">${vars.map(v => `<option${v.name === t.variable ? " selected" : ""}>${lgEsc(v.name)}</option>`).join("")}</select></div></div>` : ""}
        <div class="lg-drow"><label>Label</label><div><input data-tk="label" value="${lgEsc(t.label || "")}" placeholder="(automatic)"></div></div>
      </div>
      <div class="lg-dacts"><button data-act="delete" class="danger">Delete Transition</button></div>`;
  }

  stateDetails(m, s) {
    return `<div class="lg-dh">${lgEsc(s.name)}</div><div class="lg-dsub">State of ${lgEsc(m.name)}</div>
      <div class="lg-dsec"><div class="lg-drow"><label>Name</label><div><input data-sk="name" value="${lgEsc(s.name)}" spellcheck="false"></div></div>
        <div class="lg-drow"><label>Initial state</label><div><input type="checkbox" data-sk="initial"${s.initial ? " checked disabled" : ""}></div></div></div>
      <div class="lg-dtxt">Graph events On State Entered / On State Left run when the machine switches. Drag the + handle to another state for a transition.</div>
      <div class="lg-dacts"><button data-act="anyto">From Any State</button><button data-act="delete" class="danger">Delete State</button></div>`;
  }

  graphDetails() {
    const d = this.data;
    const m = this.machine;
    if (m) return `<div class="lg-dh">${lgEsc(m.name)}</div><div class="lg-dsub">State machine</div>
      <div class="lg-dtxt">${m.states.length} states, ${m.transitions.length} transitions. Double-click empty space for a new state; select a transition arrow to edit its rule.</div>
      <div class="lg-dacts"><button data-act="rename-m">Rename</button><button data-act="delete-m" class="danger">Delete</button></div>`;
    return `<div class="lg-dh">${lgEsc(d.ownerName)}</div><div class="lg-dsub">${d.owner ? "Actor Blueprint" : "Level Blueprint"}</div>
      <div class="lg-dtxt">${(d.nodes || []).length} nodes · ${(d.links || []).length} wires · ${(d.variables || []).length} variables. Select a node, a variable or a comment to edit it here.</div>
      <div class="lg-dsec"><div class="lg-dst">Shortcuts</div><table class="lg-keys">
        <tr><td>Right-click / long press</td><td>Add node</td></tr><tr><td>Drag pin</td><td>Wire (empty space: nodes that fit)</td></tr>
        <tr><td>Alt+click</td><td>Break wire</td></tr><tr><td>Ctrl+drag pin</td><td>Move wires</td></tr><tr><td>Double-click wire</td><td>Reroute knot</td></tr>
        <tr><td>B / S / D + click</td><td>Branch / Sequence / Delay</td></tr><tr><td>C</td><td>Comment selection</td></tr><tr><td>Q</td><td>Straighten</td></tr>
        <tr><td>F / Home</td><td>Frame selection / all</td></tr><tr><td>Ctrl+F</td><td>Find</td></tr><tr><td>F7</td><td>Compile</td></tr></table></div>`;
  }

  bindDetails(node, box, m, state, trans, variable) {
    const b = this.el.dbody;
    this.bindInputs(b);
    for (const f of b.querySelectorAll("input, select, textarea")) f.addEventListener("keydown", e => { e.stopPropagation(); if (e.key === "Enter" && f.tagName !== "TEXTAREA") f.blur(); });
    const act = (name, fn) => { const x = b.querySelector(`[data-act="${name}"]`); if (x) x.onclick = fn; };
    const ids = [...this.selection];
    act("delete", () => {
      if (variable && !node && !box) { this.selVar = null; this.cmd({cmd: "logicVariable", op: "remove", name: variable.name}); return; }
      if (trans) { this.selTransition = null; this.cmd({cmd: "logicMachine", op: "removeTransition", machine: m.id, transition: trans.id}); return; }
      this.deleteSelection();
    });
    act("duplicate", () => this.duplicate());
    act("break", () => this.breakAll(node));
    act("comment", () => this.commentSelection());
    act("straighten", () => this.straighten());
    act("get", () => this.addAtCenter("var.get", {variable: variable.name}));
    act("set", () => this.addAtCenter("var.set", {variable: variable.name}));
    act("refs", () => this.findText(variable.name));
    act("anyto", () => this.cmd({cmd: "logicMachine", op: "addTransition", machine: m.id, from: 0, to: state.id}));
    act("rename-m", () => this.renameMachine(m.id));
    act("delete-m", () => this.cmd({cmd: "logicMachine", op: "remove", machine: m.id}));
    const comment = b.querySelector(".lg-dcomment");
    if (comment) comment.onchange = () => this.cmd({cmd: "logicComment", node: node.id, text: comment.value});
    const ctext = b.querySelector(".lg-dctext");
    if (ctext) ctext.onchange = () => this.cmd({cmd: "logicCommentBox", op: "edit", id: box.id, text: ctext.value});
    for (const s of b.querySelectorAll("[data-col]")) s.onclick = () => this.cmd({cmd: "logicCommentBox", op: "edit", id: box.id, color: +s.dataset.col});
    for (const f of b.querySelectorAll("[data-vk]")) {
      f.onchange = () => {
        const k = f.dataset.vk;
        if (k === "name") { if (this.cmd({cmd: "logicVariable", op: "rename", name: variable.name, to: f.value.trim()})?.ok !== false) this.selVar = f.value.trim(); }
        else if (k === "type") this.cmd({cmd: "logicVariable", op: "type", name: variable.name, type: f.value});
        else {
          const value = f.type === "checkbox" ? f.checked : variable.type === "vector" ? f.value.split(/[,\s]+/).filter(Boolean).map(s => +s || 0) : variable.type === "text" ? f.value : (+f.value || 0);
          this.cmd({cmd: "logicVariable", op: "value", name: variable.name, value});
        }
      };
    }
    for (const f of b.querySelectorAll("[data-tk]")) {
      f.onchange = () => {
        const c = {cmd: "logicMachine", op: "editTransition", machine: m.id, transition: trans.id};
        c[f.dataset.tk] = f.dataset.tk === "seconds" ? (+f.value || 0) : f.value;
        this.cmd(c);
      };
    }
    for (const f of b.querySelectorAll("[data-sk]")) {
      f.onchange = () => {
        if (f.dataset.sk === "name") this.cmd({cmd: "logicMachine", op: "renameState", machine: m.id, state: state.id, name: f.value.trim()});
        else if (f.checked) this.cmd({cmd: "logicMachine", op: "initial", machine: m.id, state: state.id});
      };
    }
    void ids;
  }

  // ---- results (Compiler Results, Find Results) -----------------------------
  renderResults() {
    const d = this.data;
    if (!d) return;
    this.el.results.classList.toggle("open", this.resultsOpen);
    for (const b of this.host.querySelectorAll(".lg-rtabs [data-r]")) b.classList.toggle("on", b.dataset.r === this.results);
    this.host.querySelector(".lg-rfind").hidden = this.results !== "find";
    const k = JSON.stringify([this.results, this.resultsOpen, d.issues, this.results === "find" ? [this.findQuery, d.nodes?.map(n => [n.id, n.title, n.comment]), d.variables, d.comments] : 0]);
    if (k === this.resKey) return;
    this.resKey = k;
    let html = "";
    if (this.results === "compiler") {
      const issues = d.issues || [];
      html = issues.map((i, n) => `<div class="lg-res ${i.error ? "err" : "warn"}" data-node="${i.node || 0}" data-n="${n}">${lgIco(i.error ? "error" : "warn")}<span>${lgEsc(i.text)}</span></div>`).join("") ||
        `<div class="lg-res ok">${lgIco("ok")}<span>No problems. Play runs this graph as compiled machine code.</span></div>`;
    } else {
      const q = this.findQuery.trim().toLowerCase();
      if (q) {
        const hits = [];
        for (const n of d.nodes || []) {
          const hay = [n.title, n.comment, n.category, ...(n.params || []).map(p => p.value), ...n.inputs.map(p => p.label), ...n.outputs.map(p => p.label)].join(" ").toLowerCase();
          if (hay.includes(q)) hits.push(`<div class="lg-res" data-node="${n.id}">${lgIco(n.kind === "event" ? "event" : n.kind === "pure" ? "pure" : "action")}<span>${lgEsc(n.title)}</span><em>${lgEsc(n.category || "")}</em></div>`);
        }
        for (const c of d.comments || []) if (c.text.toLowerCase().includes(q)) hits.push(`<div class="lg-res" data-node="${c.id}">${lgIco("box")}<span>Comment: ${lgEsc(c.text)}</span></div>`);
        for (const v of d.variables || []) if (v.name.toLowerCase().includes(q)) hits.push(`<div class="lg-res" data-var="${lgEsc(v.name)}"><i class="lg-pill" style="--c:${LG_TYPE_COLORS[v.type]}"></i><span>Variable ${lgEsc(v.name)}</span><em>${LG_TYPE_NAMES[v.type]}</em></div>`);
        html = hits.join("") || `<div class="lg-res none">Nothing found for "${lgEsc(this.findQuery)}"</div>`;
      } else html = `<div class="lg-res none">Type to find nodes, values, comments and variables</div>`;
    }
    this.el.rlist.innerHTML = html;
    for (const r of this.el.rlist.querySelectorAll("[data-node]")) r.onclick = () => { const id = +r.dataset.node; if (id) this.focusNode(id); };
    for (const r of this.el.rlist.querySelectorAll("[data-var]")) r.onclick = () => this.selectVariable(r.dataset.var);
  }

  findText(text) {
    this.findQuery = text;
    this.el.findq.value = text;
    this.results = "find";
    this.resultsOpen = true;
    this.renderResults();
  }

  // ---- view ---------------------------------------------------------------------
  applyView() {
    const v = this.view;
    this.el.world.style.transform = `translate(${v.x}px,${v.y}px) scale(${v.zoom})`;
    const g = LG_GRID * v.zoom;
    this.el.canvas.style.backgroundSize = `${g}px ${g}px, ${g}px ${g}px, ${g * 8}px ${g * 8}px, ${g * 8}px ${g * 8}px`;
    this.el.canvas.style.backgroundPosition = `${v.x}px ${v.y}px`;
    this.el.canvas.classList.toggle("far", v.zoom < 0.5);
    const zi = LG_ZOOMS.reduce((best, z, i) => Math.abs(z - v.zoom) < Math.abs(LG_ZOOMS[best] - v.zoom) ? i : best, 0);
    this.el.zoom.textContent = zi === LG_ZOOM_ONE ? "Zoom 1:1" : `Zoom ${zi > LG_ZOOM_ONE ? "+" : ""}${zi - LG_ZOOM_ONE}`;
  }

  setZoom(z, cx, cy) {
    const v = this.view, r = this.el.canvas.getBoundingClientRect();
    z = Math.max(LG_ZOOMS[0], Math.min(LG_ZOOMS[LG_ZOOMS.length - 1], z));
    const mx = cx - r.left, my = cy - r.top;
    v.x = mx - (mx - v.x) * (z / v.zoom);
    v.y = my - (my - v.y) * (z / v.zoom);
    v.zoom = z;
    this.applyView();
  }

  toWorld(clientX, clientY) {
    const r = this.el.canvas.getBoundingClientRect(), v = this.view;
    return {x: (clientX - r.left - v.x) / v.zoom, y: (clientY - r.top - v.y) / v.zoom};
  }

  /** Frames the selection (or everything). */
  frame(selection = true) {
    const r = this.el.canvas.getBoundingClientRect();
    if (!r.width) return;
    let x0 = 1e9, y0 = 1e9, x1 = -1e9, y1 = -1e9;
    const take = (x, y, w, h) => { x0 = Math.min(x0, x); y0 = Math.min(y0, y); x1 = Math.max(x1, x + w); y1 = Math.max(y1, y + h); };
    const sel = selection && this.selection.size;
    for (const e of this.el.nodes.querySelectorAll(".lg-node, .lg-state")) {
      if (sel && !this.selection.has(+(e.dataset.id ?? e.dataset.state))) continue;
      take(parseFloat(e.style.left), parseFloat(e.style.top), e.offsetWidth, e.offsetHeight);
    }
    for (const c of this.el.cboxes.querySelectorAll(".lg-cbox")) {
      if (sel && !this.selection.has(+c.dataset.cid)) continue;
      take(parseFloat(c.style.left), parseFloat(c.style.top), c.offsetWidth, c.offsetHeight);
    }
    if (x0 > x1) return;
    const v = this.view;
    const fit = Math.min((r.width - 100) / (x1 - x0 + 1), (r.height - 100) / (y1 - y0 + 1));
    // On a phone a readable zoom wins over seeing everything: start at the graph's top left.
    v.zoom = Math.max(this.phone ? 0.5 : 0.2, Math.min(1, fit));
    const w = (x1 - x0) * v.zoom, h = (y1 - y0) * v.zoom;
    v.x = w <= r.width - 40 ? (r.width - w) / 2 - x0 * v.zoom : 24 - x0 * v.zoom;
    v.y = h <= r.height - 40 ? (r.height - h) / 2 - y0 * v.zoom : 48 - y0 * v.zoom;
    this.applyView();
  }

  focusNode(id) {
    const el = this.el.nodes.querySelector(`.lg-node[data-id="${id}"]`) || this.el.cboxes.querySelector(`[data-cid="${id}"]`);
    if (!el) return;
    const r = this.el.canvas.getBoundingClientRect(), v = this.view;
    v.zoom = 1;
    v.x = r.width / 2 - parseFloat(el.style.left) - el.offsetWidth / 2;
    v.y = r.height / 2 - parseFloat(el.style.top) - el.offsetHeight / 2;
    this.select([id]);
    this.applyView();
    el.classList.add("flash");
    setTimeout(() => el.classList.remove("flash"), 900);
  }

  centerWorld() {
    const r = this.el.canvas.getBoundingClientRect();
    return this.toWorld(r.left + r.width / 2 - 90, r.top + r.height / 2 - 40);
  }

  addAt(type, p, params, from) {
    const r = this.cmd({cmd: "logicAdd", type, x: lgSnap(p.x), y: lgSnap(p.y), params, from: from || undefined});
    if (r?.id) { this.selection = new Set([r.id]); this.selVar = null; this.selLink = null; this.markSelection(); this.renderDetails(); }
    return r;
  }

  addAtCenter(type, params) {
    const p = this.centerWorld();
    // Do not stack new nodes on top of each other.
    this.stagger = ((this.stagger || 0) + 32) % 160;
    this.addAt(type, {x: p.x + this.stagger, y: p.y + this.stagger}, params);
  }

  // ===========================================================================
  // input
  initCanvas() {
    const c = this.el.canvas;
    c.addEventListener("wheel", e => {
      e.preventDefault();
      if (e.ctrlKey && Math.abs(e.deltaY) < 50) { this.setZoom(this.view.zoom * Math.pow(1.01, -e.deltaY), e.clientX, e.clientY); return; }  // pinch on a trackpad
      const v = this.view;
      let i = LG_ZOOMS.reduce((best, z, k) => Math.abs(z - v.zoom) < Math.abs(LG_ZOOMS[best] - v.zoom) ? k : best, 0);
      i = Math.max(0, Math.min(LG_ZOOMS.length - 1, i + (e.deltaY < 0 ? 1 : -1)));
      this.setZoom(LG_ZOOMS[i], e.clientX, e.clientY);
    }, {passive: false});
    c.addEventListener("contextmenu", e => e.preventDefault());
    c.addEventListener("pointerdown", e => this.onDown(e));
    c.addEventListener("pointermove", e => this.onMove(e));
    c.addEventListener("pointerup", e => this.onUp(e));
    c.addEventListener("pointercancel", e => this.onUp(e, true));
    c.addEventListener("dragover", e => { if (e.dataTransfer.types.includes("text/x-kern-var") || e.dataTransfer.types.includes("text/x-kern-event")) e.preventDefault(); });
    c.addEventListener("drop", e => {
      const v = e.dataTransfer.getData("text/x-kern-var"), ev = e.dataTransfer.getData("text/x-kern-event");
      if (!v && !ev) return;
      e.preventDefault();
      const p = this.toWorld(e.clientX, e.clientY);
      if (ev) { this.addAt(e.ctrlKey || e.metaKey ? "flow.call" : "event.custom", p, {name: ev}); return; }
      if (e.ctrlKey || e.metaKey) this.addAt("var.get", p, {variable: v});
      else if (e.altKey) this.addAt("var.set", p, {variable: v});
      else this.studio.openMenu([{header: v}, {label: "Get " + v, run: () => this.addAt("var.get", p, {variable: v})}, {label: "Set " + v, run: () => this.addAt("var.set", p, {variable: v})}], e.clientX, e.clientY);
    });
    c.addEventListener("keydown", e => this.onKey(e, true));
    c.addEventListener("keyup", e => this.onKey(e, false));
    c.addEventListener("blur", () => this.keysDown.clear());
    new ResizeObserver(() => { if (this.data && !this.host.hidden) this.applyView(); }).observe(c);
  }

  onKey(e, down) {
    if (e.target.matches("input, select, textarea")) return;
    const k = e.key.toLowerCase();
    if (!down) { this.keysDown.delete(k); return; }
    // Typing while the action menu is open goes into its search.
    if (this.palette && e.key.length === 1 && !e.ctrlKey && !e.metaKey) {
      const q = this.palette.querySelector(".lg-pal-q");
      q.focus();
      q.value += e.key;
      q.dispatchEvent(new Event("input"));
      e.preventDefault();
      return;
    }
    const mod = e.ctrlKey || e.metaKey;
    const ids = [...this.selection];
    const machine = this.machine;
    if (!mod && !e.altKey && !e.shiftKey && LG_QUICK[k] && !machine) { this.keysDown.add(k); e.preventDefault(); return; }  // B / S / D ... + click
    if (k === "delete" || k === "backspace") {
      if (this.selLink) { this.cmd({cmd: "logicDisconnect", link: this.selLink}); this.selLink = null; }
      else if (this.selTransition && machine) { this.cmd({cmd: "logicMachine", op: "removeTransition", machine: machine.id, transition: this.selTransition}); this.selTransition = null; }
      else if (ids.length) this.deleteSelection();
    } else if (mod && (k === "c" || k === "x") && ids.length && !machine) {
      const r = this.engine.command({cmd: "logicCopy", ids});
      if (r?.text) { this.clip = r.text; navigator.clipboard?.writeText(r.text).catch(() => {}); }
      if (k === "x") this.deleteSelection();
    } else if (mod && k === "v" && !machine) {
      const paste = text => { const p = this.lastPointer || this.centerWorld(); const r = this.cmd({cmd: "logicPaste", text, x: p.x, y: p.y}); if (r?.ids) this.select(r.ids); };
      if (navigator.clipboard?.readText) navigator.clipboard.readText().then(t => paste(t?.includes("kern-logic") ? t : this.clip || "")).catch(() => this.clip && paste(this.clip));
      else if (this.clip) paste(this.clip);
    } else if (mod && (k === "d" || k === "w") && ids.length && !machine) {
      this.duplicate();
    } else if (mod && k === "a") {
      this.select(machine ? machine.states.map(s => s.id) : [...(this.data.nodes || []).map(n => n.id), ...(this.data.comments || []).map(c => c.id)]);
    } else if (mod && k === "f") {
      this.openFind();
    } else if (k === "f7") {
      this.compile();
    } else if (k === "f9" && ids.length && !machine) {
      for (const id of ids) if (this.data.nodes?.some(n => n.id === id)) this.engine.command({cmd: "logicBreakpoint", node: id});
      this.studio.flushUi();
    } else if ((k === "f5" || k === "f8") && this.data.paused) {
      this.cmd({cmd: "pause", value: false});
    } else if (k === "f" && !mod) {
      this.frame(true);
    } else if (k === "home") {
      this.frame(false);
    } else if (k === "c" && !mod && !machine) {
      this.commentSelection();
    } else if (k === "q" && !mod && !machine) {
      this.straighten();
    } else if (e.shiftKey && !mod && ["w", "a", "s", "d"].includes(k) && ids.length > 1) {
      this.align({w: "top", a: "left", s: "bottom", d: "right"}[k]);
    } else if (k === "escape") {
      if (this.palette) this.closePalette();
      else if (this.selection.size || this.selLink) this.select([]);
      else this.cmd({cmd: "logicClose"});
    } else return;
    e.preventDefault();
    e.stopPropagation();
  }

  // ---- pointer ----------------------------------------------------------------
  onDown(e) {
    // Controls on the canvas keep their own clicks.
    if (e.target.closest(".lg-palette, .lg-empty button, .lg-fab, .lg-fit, button, input, select, textarea, label.lg-ownerbox")) return;
    this.el.canvas.focus({preventScroll: true});
    this.closePalette();
    this.touches.set(e.pointerId, {x: e.clientX, y: e.clientY});
    const touch = e.pointerType === "touch" || e.pointerType === "pen";
    if (this.touches.size === 2) {
      // Second finger: pinch zoom and two-finger pan replace what the first one started.
      this.cancelDrag();
      const [a, b] = [...this.touches.values()];
      this.drag = {kind: "pinch", d0: Math.hypot(a.x - b.x, a.y - b.y), z0: this.view.zoom, cx: (a.x + b.x) / 2, cy: (a.y + b.y) / 2, vx: this.view.x, vy: this.view.y};
      return;
    }
    if (this.touches.size > 2) return;
    // Double click / double tap (detected here: re-rendering between the two clicks replaces the
    // element under the pointer, so the browser's dblclick would not fire reliably).
    const now = performance.now(), last = this.lastDown;
    this.lastDown = {t: now, x: e.clientX, y: e.clientY, button: e.button};
    if (last && e.button === 0 && last.button === 0 && now - last.t < 380 && Math.hypot(e.clientX - last.x, e.clientY - last.y) < (touch ? 18 : 6)) {
      this.lastDown = null;
      this.drag = null;
      this.onDouble(e);
      return;
    }
    const p = this.toWorld(e.clientX, e.clientY);
    this.lastPointer = p;
    this.el.canvas.setPointerCapture?.(e.pointerId);
    const row = e.target.closest(".lg-row");
    const nodeEl = e.target.closest(".lg-node");
    const stateEl = e.target.closest(".lg-state");
    const cbox = e.target.closest(".lg-cbox");
    const hit = e.target.closest(".lg-hit");
    const tico = e.target.closest(".lg-tico");
    const quick = [...this.keysDown].find(k => LG_QUICK[k]);
    if (e.button === 0 && quick && !row && !nodeEl) {
      this.addAt(LG_QUICK[quick], p);
      return;
    }
    if (e.button === 0 && row && nodeEl) {
      const node = +nodeEl.dataset.id, dir = row.dataset.dir, pin = row.dataset.pin;
      if (e.altKey) { this.cmd({cmd: "logicDisconnect", node, pin, output: dir === "out"}); return; }
      // Ctrl+drag: pick up the pin's existing wires and move them (Blueprint).
      if ((e.ctrlKey || e.metaKey) && this.pickUpWires(node, pin, dir, e)) return;
      this.drag = {kind: "wire", node, pin, out: dir === "out", type: row.dataset.type, x0: e.clientX, y0: e.clientY, touch};
      this.highlightTargets(this.drag);
      return;
    }
    if (e.button === 0 && tico) {
      this.selTransition = +tico.dataset.t;
      this.selection.clear();
      this.markSelection();
      this.drawTransitions(this.machine);
      this.renderDetails();
      return;
    }
    if (e.button === 0 && hit) {
      const link = this.data.links[+hit.dataset.k];
      if (e.altKey) { this.cmd({cmd: "logicDisconnect", link}); return; }
      this.selection.clear();
      this.selLink = link;
      this.markSelection();
      this.drawWires();
      this.renderDetails();
      this.drag = {kind: "link", x0: e.clientX, y0: e.clientY, link, touch};
      if (touch) this.armLongPress(e, () => this.linkMenu(link, e.clientX, e.clientY));
      return;
    }
    if (stateEl && e.target.closest(".lg-handle") && e.button === 0) {
      this.drag = {kind: "transition", from: +stateEl.dataset.state, x0: e.clientX, y0: e.clientY};
      return;
    }
    if ((nodeEl || stateEl || (cbox && (e.target.closest(".lg-ch") || e.target.closest(".lg-cres")))) && e.button === 0) {
      const id = +(nodeEl ? nodeEl.dataset.id : stateEl ? stateEl.dataset.state : cbox.dataset.cid);
      if (cbox && e.target.closest(".lg-cres")) {
        const c = this.data.comments.find(x => x.id === id);
        this.select([id]);
        this.drag = {kind: "resize", id, el: cbox, w: c.w, h: c.h, x0: e.clientX, y0: e.clientY, transaction: "lgres" + Date.now()};
        return;
      }
      if (e.shiftKey || e.ctrlKey || e.metaKey) {
        if (this.selection.has(id)) this.selection.delete(id); else this.selection.add(id);
        this.select([...this.selection]);
      } else if (!this.selection.has(id)) this.select([id]);
      this.drag = {kind: "nodes", items: this.dragItems(), x0: e.clientX, y0: e.clientY, moved: false, transaction: "lgmove" + Date.now(), touch, id};
      if (touch) this.armLongPress(e, () => this.contextMenu({clientX: e.clientX, clientY: e.clientY, target: e.target}));
      return;
    }
    const pan = e.button === 1 || e.button === 2 || (touch && !this.boxMode) || (e.button === 0 && e.altKey);
    if (pan) {
      this.drag = {kind: "pan", x0: e.clientX, y0: e.clientY, vx: this.view.x, vy: this.view.y, moved: false, button: e.button, target: e.target, touch};
      if (touch) this.armLongPress(e, () => { this.drag = null; this.contextMenu({clientX: e.clientX, clientY: e.clientY, target: e.target}); });
      return;
    }
    if (e.button === 0) {
      if (!e.shiftKey && !e.ctrlKey) this.select([]);
      this.drag = {kind: "marquee", x0: e.clientX, y0: e.clientY, base: new Set(this.selection)};
    }
  }

  armLongPress(e, fn) {
    clearTimeout(this.longTimer);
    const d = this.drag;
    this.longTimer = setTimeout(() => {
      if (this.drag !== d || (d && d.moved)) return;
      if (d?.kind === "nodes") this.drag = null;
      navigator.vibrate?.(12);
      fn();
    }, 520);
  }

  cancelDrag() {
    clearTimeout(this.longTimer);
    const d = this.drag;
    this.drag = null;
    this.el.temp.innerHTML = "";
    this.el.marquee.hidden = true;
    this.clearTargets();
    if (d?.kind === "nodes" && d.moved) this.render(this.data);  // put them back
  }

  dragItems() {
    // Nodes, states and comments that move; a comment carries the nodes inside it.
    const items = new Map();
    const add = (id) => {
      if (items.has(id)) return;
      const n = this.el.nodes.querySelector(`.lg-node[data-id="${id}"], .lg-state[data-state="${id}"]`);
      if (n) { items.set(id, {id, el: n, x: parseFloat(n.style.left), y: parseFloat(n.style.top)}); return; }
      const c = this.el.cboxes.querySelector(`[data-cid="${id}"]`);
      if (!c) return;
      const cx = parseFloat(c.style.left), cy = parseFloat(c.style.top), cw = c.offsetWidth, ch = c.offsetHeight;
      items.set(id, {id, el: c, x: cx, y: cy});
      for (const [nid, p] of this.nodePos) if (p.x >= cx && p.y >= cy && p.x + p.w <= cx + cw && p.y + p.h <= cy + ch) add(nid);
    };
    for (const id of this.selection) add(id);
    return [...items.values()];
  }

  pickUpWires(node, pin, dir, e) {
    const links = (this.data.links || []).filter(([f, o, t, i]) => dir === "in" ? t === node && i === pin : f === node && o === pin);
    if (!links.length) return false;
    // Keep the far ends; the wires follow the pointer until dropped on another pin.
    const ends = links.map(([f, o, t, i]) => dir === "in" ? {node: f, pin: o, out: true} : {node: t, pin: i, out: false});
    const transaction = "lgrewire" + Date.now();
    for (const l of links) this.engine.command({cmd: "logicDisconnect", link: l, transaction});
    this.studio.flushUi();
    const type = (this.data.nodes.find(n => n.id === ends[0].node)?.[ends[0].out ? "outputs" : "inputs"] || []).find(p => p.id === ends[0].pin)?.type || "exec";
    this.drag = {kind: "wire", node: ends[0].node, pin: ends[0].pin, out: ends[0].out, type, ends, transaction, x0: e.clientX, y0: e.clientY};
    this.highlightTargets(this.drag);
    return true;
  }

  onMove(e) {
    if (this.touches.has(e.pointerId)) this.touches.set(e.pointerId, {x: e.clientX, y: e.clientY});
    const d = this.drag;
    this.lastPointer = this.toWorld(e.clientX, e.clientY);
    if (!d) return;
    const z = this.view.zoom;
    const far = Math.abs(e.clientX - (d.x0 ?? e.clientX)) + Math.abs(e.clientY - (d.y0 ?? e.clientY)) > (d.touch ? 8 : 3);
    if (far) clearTimeout(this.longTimer);
    if (d.kind === "pinch") {
      const [a, b] = [...this.touches.values()];
      if (!a || !b) return;
      const dist = Math.hypot(a.x - b.x, a.y - b.y), cx = (a.x + b.x) / 2, cy = (a.y + b.y) / 2;
      const v = this.view, r = this.el.canvas.getBoundingClientRect();
      const z1 = Math.max(LG_ZOOMS[0], Math.min(LG_ZOOMS[LG_ZOOMS.length - 1], d.z0 * dist / Math.max(1, d.d0)));
      // Keep the world point under the first centre under the current centre.
      const wx = (d.cx - r.left - d.vx) / d.z0, wy = (d.cy - r.top - d.vy) / d.z0;
      v.zoom = z1;
      v.x = cx - r.left - wx * z1;
      v.y = cy - r.top - wy * z1;
      this.applyView();
    } else if (d.kind === "pan") {
      if (far) d.moved = true;
      this.view.x = d.vx + e.clientX - d.x0;
      this.view.y = d.vy + e.clientY - d.y0;
      this.applyView();
    } else if (d.kind === "nodes") {
      if (!far && !d.moved) return;
      d.moved = true;
      const dx = (e.clientX - d.x0) / z, dy = (e.clientY - d.y0) / z;
      for (const it of d.items) {
        const nx = lgSnap(it.x + dx), ny = lgSnap(it.y + dy);
        it.el.style.left = nx + "px";
        it.el.style.top = ny + "px";
        const p = this.nodePos.get(it.id);
        if (p) { p.x = nx; p.y = ny; }
      }
      if (this.data.view === "graph") this.drawWires();
      else if (this.machine) this.drawTransitions(this.machine);
    } else if (d.kind === "resize") {
      d.moved = true;
      d.el.style.width = Math.max(96, lgSnap(d.w + (e.clientX - d.x0) / z)) + "px";
      d.el.style.height = Math.max(64, lgSnap(d.h + (e.clientY - d.y0) / z)) + "px";
    } else if (d.kind === "link") {
      if (far) d.moved = true;
    } else if (d.kind === "wire" || d.kind === "transition") {
      if (far) d.moved = true;
      const r = this.el.canvas.getBoundingClientRect(), v = this.view;
      const b = {x: e.clientX - r.left, y: e.clientY - r.top};
      let path = "";
      if (d.kind === "wire") {
        const starts = d.ends ? d.ends : [{node: d.node, pin: d.pin, out: d.out}];
        for (const s of starts) {
          const pt = this.pinPoint(s.node, s.pin, s.out ? "out" : "in");
          const a = pt ? {x: pt.x * z + v.x, y: pt.y * z + v.y} : {x: d.x0 - r.left, y: d.y0 - r.top};
          path += `<path d="${s.out ? this.wirePath(a, b) : this.wirePath(b, a)}" stroke="${LG_TYPE_COLORS[d.type]}" class="lg-wire${d.type === "exec" ? " exec" : ""} temp"/>`;
        }
        const target = document.elementFromPoint(e.clientX, e.clientY)?.closest?.(".lg-row");
        for (const h of this.el.nodes.querySelectorAll(".lg-row.hover")) h.classList.remove("hover");
        if (target && this.fits(d, target)) target.classList.add("hover");
      } else {
        const sr = this.stateRect(d.from);
        const a = sr ? {x: (sr.x + sr.w / 2) * z + v.x, y: (sr.y + sr.h / 2) * z + v.y} : {x: d.x0 - r.left, y: d.y0 - r.top};
        path = `<path d="M${a.x},${a.y}L${b.x},${b.y}" class="lg-trans temp"/>`;
      }
      this.el.temp.innerHTML = path;
    } else if (d.kind === "marquee") {
      const r = this.el.canvas.getBoundingClientRect();
      const x = Math.min(d.x0, e.clientX) - r.left, y = Math.min(d.y0, e.clientY) - r.top, w = Math.abs(e.clientX - d.x0), h = Math.abs(e.clientY - d.y0);
      Object.assign(this.el.marquee.style, {left: x + "px", top: y + "px", width: w + "px", height: h + "px"});
      this.el.marquee.hidden = w + h < 4;
    }
  }

  fits(d, row) {
    if (!row || row.dataset.dir === (d.out ? "out" : "in")) return false;
    const other = +row.closest(".lg-node")?.dataset.id;
    if (!other || other === d.node) return false;
    const from = d.out ? d.type : row.dataset.type, to = d.out ? row.dataset.type : d.type;
    if ((from === "exec") !== (to === "exec")) return false;
    if (from === to || to === "text") return true;
    const numeric = t => t === "int" || t === "float" || t === "bool";
    if (numeric(from) && numeric(to)) return true;
    if (to === "bool" && from === "actor") return true;
    if (to === "vector" && (from === "int" || from === "float")) return true;
    return false;
  }

  highlightTargets(d) {
    this.el.nodes.classList.add("wiring");
    for (const row of this.el.nodes.querySelectorAll(".lg-row")) row.classList.toggle("can", this.fits(d, row));
  }
  clearTargets() {
    this.el.nodes.classList.remove("wiring");
    for (const row of this.el.nodes.querySelectorAll(".lg-row.can, .lg-row.hover")) row.classList.remove("can", "hover");
  }

  onUp(e, cancelled = false) {
    this.touches.delete(e.pointerId);
    clearTimeout(this.longTimer);
    const d = this.drag;
    if (d?.kind === "pinch") { if (this.touches.size === 0) this.drag = null; return; }
    this.drag = null;
    this.el.temp.innerHTML = "";
    this.el.marquee.hidden = true;
    this.clearTargets();
    if (!d || cancelled) return;
    if (d.kind === "pan") {
      if (!d.moved && d.button === 2) this.contextMenu({clientX: e.clientX, clientY: e.clientY, target: d.target});
      else if (!d.moved && d.touch) { if (this.selection.size || this.selLink) this.select([]); }
      return;
    }
    if (d.kind === "nodes") {
      if (d.moved) {
        const moves = d.items.map(it => ({id: it.id, x: parseFloat(it.el.style.left), y: parseFloat(it.el.style.top)}));
        this.cmd({cmd: "logicMove", moves, transaction: d.transaction});
      } else if (!e.shiftKey && !e.ctrlKey && !e.metaKey) {
        this.select([d.id]);  // a click on one of several selected nodes selects only it
        if (this.phone && d.touch) this.renderDetails();
      }
      return;
    }
    if (d.kind === "resize") {
      if (d.moved) {
        const c = this.data.comments.find(x => x.id === d.id);
        this.cmd({cmd: "logicMove", moves: [{id: d.id, x: c.x, y: c.y, w: parseFloat(d.el.style.width), h: parseFloat(d.el.style.height)}], transaction: d.transaction});
      }
      return;
    }
    if (d.kind === "link") return;
    if (d.kind === "wire") {
      const row = document.elementFromPoint(e.clientX, e.clientY)?.closest?.(".lg-row");
      const starts = d.ends || [{node: d.node, pin: d.pin, out: d.out}];
      if (row && row.closest(".lg-node") && this.fits(d, row)) {
        const other = +row.closest(".lg-node").dataset.id;
        for (const s of starts) {
          if (s.out) this.engine.command({cmd: "logicConnect", from: s.node, out: s.pin, to: other, in: row.dataset.pin, transaction: d.transaction});
          else this.engine.command({cmd: "logicConnect", from: other, out: row.dataset.pin, to: s.node, in: s.pin, transaction: d.transaction});
        }
        this.studio.flushUi();
        return;
      }
      if (row && row.closest(".lg-node") && +row.closest(".lg-node").dataset.id !== d.node && d.moved) { this.studio.toast(`A ${LG_TYPE_NAMES[d.type] || d.type} pin does not fit a ${LG_TYPE_NAMES[row.dataset.type] || row.dataset.type} pin`); return; }
      if (d.ends) return;  // picked-up wires dropped in empty space: they stay broken (Blueprint)
      // A wire dropped into empty space (or a tap on a pin): the nodes that fit.
      if (d.moved || d.touch) this.openPalette(e.clientX, e.clientY, {node: d.node, pin: d.pin, output: d.out});
      return;
    }
    if (d.kind === "transition") {
      const machine = this.machine;
      const st = document.elementFromPoint(e.clientX, e.clientY)?.closest?.(".lg-state");
      if (st && +st.dataset.state !== d.from) this.cmd({cmd: "logicMachine", op: "addTransition", machine: machine.id, from: d.from, to: +st.dataset.state});
      else if (!st && d.moved) {
        const p = this.toWorld(e.clientX, e.clientY);
        this.cmd({cmd: "logicMachine", op: "addState", machine: machine.id, from: d.from, x: lgSnap(p.x - 90), y: lgSnap(p.y - 30)});
      }
      return;
    }
    if (d.kind === "marquee") {
      const x0 = Math.min(d.x0, e.clientX), x1 = Math.max(d.x0, e.clientX), y0 = Math.min(d.y0, e.clientY), y1 = Math.max(d.y0, e.clientY);
      if (x1 - x0 + y1 - y0 < 4) { if (this.boxMode) this.select([]); return; }
      const ids = new Set(d.base);
      for (const n of this.el.nodes.querySelectorAll(".lg-node, .lg-state")) {
        const r = n.getBoundingClientRect();
        if (r.right > x0 && r.left < x1 && r.bottom > y0 && r.top < y1) ids.add(+(n.dataset.id ?? n.dataset.state));
      }
      for (const c of this.el.cboxes.querySelectorAll(".lg-cbox")) {
        const r = c.getBoundingClientRect();
        if (r.left >= x0 && r.right <= x1 && r.top >= y0 && r.bottom <= y1) ids.add(+c.dataset.cid);
      }
      this.select([...ids]);
    }
  }

  onDouble(ev) {
    // Pointer capture retargets the double click to the canvas: look at what is under the pointer.
    const e = {clientX: ev.clientX, clientY: ev.clientY, target: document.elementFromPoint(ev.clientX, ev.clientY) || ev.target};
    const machine = this.machine;
    if (machine) {
      const st = e.target.closest(".lg-state");
      if (st) this.renameState(+st.dataset.state);
      else if (!e.target.closest(".lg-tico")) {
        const p = this.toWorld(e.clientX, e.clientY);
        this.cmd({cmd: "logicMachine", op: "addState", machine: machine.id, x: lgSnap(p.x - 90), y: lgSnap(p.y - 30)});
      }
      return;
    }
    const hit = e.target.closest(".lg-hit");
    if (hit) {
      // Double-click on a wire: a reroute knot at that point.
      const p = this.toWorld(e.clientX, e.clientY);
      this.cmd({cmd: "logicInsertReroute", link: this.data.links[+hit.dataset.k], x: p.x - 8, y: p.y - 8});
      return;
    }
    const ch = e.target.closest(".lg-ch");
    if (ch) { this.editCommentTitle(+ch.closest(".lg-cbox").dataset.cid); return; }
    const node = e.target.closest(".lg-node");
    if (node && !e.target.closest(".lg-row, input, select")) {
      const n = this.data.nodes.find(x => x.id === +node.dataset.id);
      const variable = (n?.params || []).find(p => p.key === "variable")?.value;
      if (variable) this.selectVariable(variable);
      else if (this.phone) this.togglePanel("details");
    }
  }

  editCommentTitle(id) {
    const box = this.el.cboxes.querySelector(`[data-cid="${id}"]`), c = this.data.comments.find(x => x.id === id);
    if (!box || !c) return;
    const h = box.querySelector(".lg-ch");
    h.innerHTML = `<input class="lg-cedit" value="${lgEsc(c.text)}" spellcheck="false">`;
    const input = h.querySelector("input");
    input.focus();
    input.select();
    const done = ok => { if (ok && input.value !== c.text) this.cmd({cmd: "logicCommentBox", op: "edit", id, text: input.value}); else h.textContent = c.text; };
    input.onkeydown = ev => { ev.stopPropagation(); if (ev.key === "Enter") input.blur(); if (ev.key === "Escape") { input.onblur = null; done(false); } };
    input.onblur = () => done(true);
    input.onpointerdown = ev => ev.stopPropagation();
  }

  // ---- menus ----------------------------------------------------------------------
  contextMenu(e) {
    const machine = this.machine;
    const nodeEl = e.target.closest?.(".lg-node"), stateEl = e.target.closest?.(".lg-state"), row = e.target.closest?.(".lg-row"), cbox = e.target.closest?.(".lg-cbox");
    const hit = e.target.closest?.(".lg-hit");
    if (row && nodeEl) { this.pinMenu(+nodeEl.dataset.id, row, e.clientX, e.clientY); return; }
    if (hit) { this.linkMenu(this.data.links[+hit.dataset.k], e.clientX, e.clientY); return; }
    if (nodeEl) {
      const id = +nodeEl.dataset.id;
      if (!this.selection.has(id)) this.select([id]);
      const n = this.data.nodes.find(x => x.id === id);
      const many = this.selection.size > 1;
      this.studio.openMenu([
        {header: many ? `${this.selection.size} nodes` : n?.title || "Node"},
        {label: "Delete", shortcut: "Del", run: () => this.deleteSelection()},
        {label: "Cut", shortcut: "Ctrl+X", run: () => this.onKey(new KeyboardEvent("keydown", {key: "x", ctrlKey: true}), true)},
        {label: "Copy", shortcut: "Ctrl+C", run: () => this.onKey(new KeyboardEvent("keydown", {key: "c", ctrlKey: true}), true)},
        {label: "Duplicate", shortcut: "Ctrl+W", run: () => this.duplicate()},
        {separator: true},
        {label: "Break Node Link(s)", run: () => { for (const nid of this.selection) this.breakAll(this.data.nodes.find(x => x.id === nid)); }},
        {label: n?.comment ? "Edit Node Comment" : "Add Node Comment", run: () => { this.renderDetails(); if (this.phone) this.togglePanel("details"); setTimeout(() => this.el.dbody.querySelector(".lg-dcomment")?.focus(), 30); }},
        {label: "Create Comment from Selection", shortcut: "C", run: () => this.commentSelection()},
        {label: "Straighten Connections", shortcut: "Q", run: () => this.straighten()},
        {label: n?.breakpoint ? "Remove Breakpoint" : "Add Breakpoint", shortcut: "F9", run: () => { for (const nid of this.selection) this.engine.command({cmd: "logicBreakpoint", node: nid, on: !n?.breakpoint}); this.studio.flushUi(); }},
        {label: "Alignment", disabled: !many, submenu: [
          {label: "Align Top", shortcut: "Shift+W", run: () => this.align("top")}, {label: "Align Middle", run: () => this.align("middle")},
          {label: "Align Bottom", shortcut: "Shift+S", run: () => this.align("bottom")}, {label: "Align Left", shortcut: "Shift+A", run: () => this.align("left")},
          {label: "Align Center", run: () => this.align("center")}, {label: "Align Right", shortcut: "Shift+D", run: () => this.align("right")},
          {separator: true}, {label: "Distribute Horizontally", run: () => this.distribute("x")}, {label: "Distribute Vertically", run: () => this.distribute("y")}]},
      ], e.clientX, e.clientY);
      return;
    }
    if (cbox) {
      const id = +cbox.dataset.cid;
      this.select([id]);
      this.studio.openMenu([{header: "Comment"}, {label: "Rename", run: () => this.editCommentTitle(id)},
        {label: "Color", submenu: LG_COMMENT_COLORS.map((c, i) => ({label: ["Grey", "Blue", "Green", "Yellow", "Red", "Purple"][i], run: () => this.cmd({cmd: "logicCommentBox", op: "edit", id, color: c})}))},
        {label: "Delete Comment", run: () => this.cmd({cmd: "logicDelete", ids: [id]})}], e.clientX, e.clientY);
      return;
    }
    if (stateEl && machine) {
      const id = +stateEl.dataset.state;
      this.studio.openMenu([
        {label: "Set as Initial State", run: () => this.cmd({cmd: "logicMachine", op: "initial", machine: machine.id, state: id})},
        {label: "Rename...", run: () => this.renameState(id)},
        {label: "Transition from Any State", run: () => this.cmd({cmd: "logicMachine", op: "addTransition", machine: machine.id, from: 0, to: id})},
        {label: "Delete State", run: () => this.cmd({cmd: "logicMachine", op: "removeState", machine: machine.id, state: id})},
      ], e.clientX, e.clientY);
      return;
    }
    if (machine) {
      const p = this.toWorld(e.clientX, e.clientY);
      this.studio.openMenu([{label: "Add State", run: () => this.cmd({cmd: "logicMachine", op: "addState", machine: machine.id, x: lgSnap(p.x), y: lgSnap(p.y)})}], e.clientX, e.clientY);
      return;
    }
    this.openPalette(e.clientX, e.clientY, null);
  }

  pinMenu(node, row, x, y) {
    const n = this.data.nodes.find(k => k.id === node);
    const out = row.dataset.dir === "out", pin = row.dataset.pin;
    const p = (out ? n?.outputs : n?.inputs)?.find(q => q.id === pin);
    const links = (this.data.links || []).filter(([f, o, t, i]) => out ? f === node && o === pin : t === node && i === pin);
    const label = p?.label || pin;
    const entries = [{header: `${label} (${LG_TYPE_NAMES[p?.type] || p?.type})`},
      {label: links.length > 1 ? `Break All Pin Links (${links.length})` : "Break Link", disabled: !links.length, run: () => this.cmd({cmd: "logicDisconnect", node, pin, output: out})}];
    for (const l of links.length > 1 ? links : []) {
      const other = this.data.nodes.find(k => k.id === (out ? l[2] : l[0]));
      entries.push({label: `Break link to ${other?.title || "?"}`, run: () => this.cmd({cmd: "logicDisconnect", link: l})});
    }
    entries.push({label: "Add Nodes That Fit…", run: () => this.openPalette(x, y, {node, pin, output: out})});
    if (p && p.type !== "exec") entries.push({label: "Promote to Variable", run: () => { const r = this.cmd({cmd: "logicPromote", node, pin, output: out}); if (r?.variable) this.studio.toast(`New variable ${r.variable}`); }});
    if (p && !out && p.type !== "exec" && !p.linked) entries.push({label: "Reset to Default", run: () => this.cmd({cmd: "logicValue", node, pin, value: p.type === "vector" ? [0, 0, 0] : p.type === "text" ? "" : p.type === "bool" ? false : 0})});
    this.studio.openMenu(entries, x, y);
  }

  linkMenu(link, x, y) {
    this.studio.openMenu([{header: "Wire"}, {label: "Break This Link", run: () => this.cmd({cmd: "logicDisconnect", link})},
      {label: "Add Reroute Node", run: () => { const p = this.toWorld(x, y); this.cmd({cmd: "logicInsertReroute", link, x: p.x - 8, y: p.y - 8}); }}], x, y);
  }

  // ---- edits ----------------------------------------------------------------------
  deleteSelection() {
    const ids = [...this.selection];
    const machine = this.machine;
    if (!ids.length) return;
    if (machine) for (const id of ids) this.cmd({cmd: "logicMachine", op: "removeState", machine: machine.id, state: id});
    else this.cmd({cmd: "logicDelete", ids});
    this.select([]);
  }

  duplicate() {
    const ids = [...this.selection];
    if (!ids.length || this.machine) return;
    const r = this.engine.command({cmd: "logicCopy", ids});
    let x = 1e9, y = 1e9;
    for (const id of ids) { const p = this.nodePos.get(id) || this.data.comments?.find(c => c.id === id); if (p) { x = Math.min(x, p.x); y = Math.min(y, p.y); } }
    if (r?.text && x < 1e9) { const p = this.cmd({cmd: "logicPaste", text: r.text, x: x + 32, y: y + 32}); if (p?.ids) this.select(p.ids); }
  }

  breakAll(n) {
    if (!n) return;
    const transaction = "lgbreak" + Date.now() + Math.random();
    for (const l of (this.data.links || []).filter(([f, , t]) => f === n.id || t === n.id)) this.engine.command({cmd: "logicDisconnect", link: l, transaction});
    this.studio.flushUi();
  }

  commentSelection() {
    let x0 = 1e9, y0 = 1e9, x1 = -1e9, y1 = -1e9;
    for (const id of this.selection) {
      const p = this.nodePos.get(id);
      if (!p) continue;
      x0 = Math.min(x0, p.x); y0 = Math.min(y0, p.y); x1 = Math.max(x1, p.x + p.w); y1 = Math.max(y1, p.y + p.h);
    }
    if (x0 > x1) { const p = this.lastPointer || this.centerWorld(); x0 = p.x; y0 = p.y; x1 = p.x + 400; y1 = p.y + 160; }
    const r = this.cmd({cmd: "logicCommentBox", op: "add", x: lgSnap(x0 - 32), y: lgSnap(y0 - 64), w: lgSnap(x1 - x0 + 64), h: lgSnap(y1 - y0 + 96), text: "Comment"});
    if (r?.id) { this.select([r.id]); this.afterRender = () => this.editCommentTitle(r.id); }
  }

  /** Q: every selected node moves so that its first wired input runs straight from its source. */
  straighten() {
    const moves = [];
    for (const id of this.selection) {
      const n = this.data.nodes?.find(x => x.id === id);
      if (!n) continue;
      const links = (this.data.links || []).filter(l => l[2] === id);
      const l = links.find(k => (n.inputs.find(p => p.id === k[3])?.type === "exec")) || links[0];
      if (!l) continue;
      const a = this.pinPoint(l[0], l[1], "out"), b = this.pinPoint(id, l[3], "in"), p = this.nodePos.get(id);
      if (!a || !b || !p) continue;
      moves.push({id, x: p.x, y: Math.round(p.y + (a.y - b.y))});
    }
    if (moves.length) this.cmd({cmd: "logicMove", moves});
  }

  align(how) {
    const items = [...this.selection].map(id => ({id, p: this.nodePos.get(id)})).filter(i => i.p);
    if (items.length < 2) return;
    const top = Math.min(...items.map(i => i.p.y)), bottom = Math.max(...items.map(i => i.p.y + i.p.h));
    const left = Math.min(...items.map(i => i.p.x)), right = Math.max(...items.map(i => i.p.x + i.p.w));
    const moves = items.map(({id, p}) => {
      let x = p.x, y = p.y;
      if (how === "top") y = top;
      if (how === "bottom") y = bottom - p.h;
      if (how === "middle") y = (top + bottom) / 2 - p.h / 2;
      if (how === "left") x = left;
      if (how === "right") x = right - p.w;
      if (how === "center") x = (left + right) / 2 - p.w / 2;
      return {id, x: Math.round(x), y: Math.round(y)};
    });
    this.cmd({cmd: "logicMove", moves});
  }

  distribute(axis) {
    const items = [...this.selection].map(id => ({id, p: this.nodePos.get(id)})).filter(i => i.p).sort((a, b) => a.p[axis] - b.p[axis]);
    if (items.length < 3) return;
    const size = axis === "x" ? "w" : "h";
    const total = items.reduce((s, i) => s + i.p[size], 0);
    const span = items[items.length - 1].p[axis] + items[items.length - 1].p[size] - items[0].p[axis];
    const gap = (span - total) / (items.length - 1);
    let at = items[0].p[axis];
    const moves = items.map(({id, p}) => { const m = {id, x: p.x, y: p.y}; m[axis] = Math.round(at); at += p[size] + gap; return m; });
    this.cmd({cmd: "logicMove", moves});
  }

  renameState(id) {
    const s = this.machine?.states.find(x => x.id === id);
    if (!s) return;
    this.select([id]);
    if (this.phone) this.togglePanel("details");
    setTimeout(() => { const f = this.el.dbody.querySelector("[data-sk='name']"); f?.focus(); f?.select(); }, 30);
  }

  renameMachine(id) {
    const m = this.data.machines.find(x => x.id === id);
    const name = m && prompt("State machine name", m.name);
    if (name && name !== m.name) this.cmd({cmd: "logicMachine", op: "rename", machine: id, name});
  }

  // ---- action menu (Blueprint's "All Actions for this Blueprint") ---------------
  openPalette(x, y, from, center = false) {
    this.closePalette();
    const where = this.toWorld(x, y);
    const box = document.createElement("div");
    box.className = "lg-palette";
    box.innerHTML = `<div class="lg-pal-h"><span>${from ? "Actions taking this pin" : "All Actions for this Blueprint"}</span>
        ${from ? `<label class="lg-pal-ctx"><input type="checkbox" checked> Context Sensitive</label>` : ""}</div>
      <div class="lg-search">${lgIco("find")}<input class="lg-pal-q" placeholder="Search" spellcheck="false" autocomplete="off"></div>
      ${from ? "" : `<div class="lg-pal-extra"><button data-extra="comment">${lgIco("box")}Add Comment</button></div>`}
      <div class="lg-pal-list"></div><div class="lg-pal-help"></div>`;
    this.el.canvas.append(box);
    const r = this.el.canvas.getBoundingClientRect();
    if (this.phone) box.classList.add("sheet");
    else {
      const w = 340, h = Math.min(460, r.height - 16);
      box.style.left = (center ? r.width / 2 - w / 2 : Math.min(r.width - w - 8, Math.max(8, x - r.left))) + "px";
      box.style.top = (center ? Math.max(8, r.height / 2 - h / 2) : Math.min(r.height - h - 8, Math.max(8, y - r.top))) + "px";
      box.style.maxHeight = h + "px";
    }
    const q = box.querySelector(".lg-pal-q"), list = box.querySelector(".lg-pal-list"), help = box.querySelector(".lg-pal-help"), ctx = box.querySelector(".lg-pal-ctx input");
    let items = [], active = 0;
    const closedCats = this.paletteClosed ||= new Set();
    const show = () => {
      const res = this.engine.command({cmd: "logicPalette", query: q.value, from: from && (!ctx || ctx.checked) ? from : undefined});
      items = res?.items || [];
      active = 0;
      let html = "", cat = "";
      const word = q.value.trim().toLowerCase();
      const mark = s => { if (!word) return lgEsc(s); const i = s.toLowerCase().indexOf(word); return i < 0 ? lgEsc(s) : lgEsc(s.slice(0, i)) + "<b>" + lgEsc(s.slice(i, i + word.length)) + "</b>" + lgEsc(s.slice(i + word.length)); };
      items.forEach((it, i) => {
        if (it.category !== cat) {
          cat = it.category;
          html += `<div class="lg-pal-cat${!word && closedCats.has(cat) ? " closed" : ""}" data-cat="${lgEsc(cat)}">${lgIco("chevron", "lg-car")}${lgEsc(cat)}</div>`;
        }
        if (!word && closedCats.has(cat)) return;
        const kind = it.type.startsWith("event.") || it.type === "game.interact" || it.type.includes("overlap") ? "event" : "";
        html += `<div class="lg-pal-it" data-i="${i}">${lgIco(kind || (it.type.startsWith("math.") || it.type.startsWith("vector.") || it.type.startsWith("logic.") ? "pure" : it.type.startsWith("flow.") ? "flow" : "action"), kind ? "red" : "")}<span>${mark(it.label)}</span></div>`;
      });
      list.innerHTML = html || `<div class="lg-none">Nothing matches</div>`;
      for (const c of list.querySelectorAll(".lg-pal-cat")) c.onclick = () => { const k = c.dataset.cat; if (closedCats.has(k)) closedCats.delete(k); else closedCats.add(k); show(); };
      markActive();
    };
    const visible = () => [...list.querySelectorAll(".lg-pal-it")].map(el => +el.dataset.i);
    const markActive = () => {
      const vis = visible();
      if (!vis.includes(active)) active = vis[0] ?? 0;
      for (const el of list.querySelectorAll(".lg-pal-it")) el.classList.toggle("active", +el.dataset.i === active);
      list.querySelector(".lg-pal-it.active")?.scrollIntoView({block: "nearest"});
      help.textContent = items[active]?.help || "";
    };
    const pick = i => {
      const it = items[i];
      if (!it) return;
      this.closePalette();
      this.addAt(it.type, where, it.params, from && (!ctx || ctx.checked) ? from : from);
      this.el.canvas.focus({preventScroll: true});
    };
    q.oninput = show;
    if (ctx) ctx.onchange = show;
    q.onkeydown = e => {
      e.stopPropagation();
      const vis = visible(), k = vis.indexOf(active);
      if (e.key === "ArrowDown") { active = vis[Math.min(vis.length - 1, k + 1)] ?? active; markActive(); e.preventDefault(); }
      else if (e.key === "ArrowUp") { active = vis[Math.max(0, k - 1)] ?? active; markActive(); e.preventDefault(); }
      else if (e.key === "Enter") { pick(active); e.preventDefault(); }
      else if (e.key === "Escape") { this.closePalette(); this.el.canvas.focus(); e.preventDefault(); }
    };
    list.onpointermove = e => { const it = e.target.closest(".lg-pal-it"); if (it && +it.dataset.i !== active) { active = +it.dataset.i; markActive(); } };
    // The click that follows the tap which opened the menu must not pick an action under the finger.
    const opened = performance.now();
    list.onclick = e => { const it = e.target.closest(".lg-pal-it"); if (it && performance.now() - opened > 350) pick(+it.dataset.i); };
    box.querySelector("[data-extra='comment']")?.addEventListener("click", () => { this.closePalette(); this.lastPointer = where; this.selection.clear(); this.commentSelection(); });
    box.onpointerdown = e => e.stopPropagation();
    box.onwheel = e => e.stopPropagation();
    this.palette = box;
    show();
    if (!this.phone) {
      q.focus({preventScroll: true});
      setTimeout(() => { if (this.palette === box && document.activeElement !== q) q.focus({preventScroll: true}); }, 0);
    }
  }

  closePalette() {
    this.palette?.remove();
    this.palette = null;
  }

  // ---- debugging ------------------------------------------------------------------
  renderDebug(data) {
    const bar = this.host.querySelector(".lg-debug");
    const hit = (data.nodes || []).find(n => n.hit);
    const show = !!data.playing && (data.paused || data.breakpoints > 0);
    bar.hidden = !show;
    if (!show) return;
    bar.classList.toggle("paused", !!data.paused);
    this.host.querySelector(".lg-dbg-t").textContent = hit ? `Breakpoint: ${hit.title}` : data.paused ? "Paused" : `${data.breakpoints} breakpoint${data.breakpoints === 1 ? "" : "s"} armed`;
    this.host.querySelector(".lg-resume").hidden = !data.paused;
    this.host.querySelector(".lg-step").hidden = !data.paused;
  }

  /** The engine stopped at a breakpoint: show the node. */
  onBreak(node) {
    this.afterRender = () => this.focusNode(node);
    if (this.data?.nodes?.some(n => n.id === node)) { this.focusNode(node); this.afterRender = null; }
  }

  // ---- live (while playing) --------------------------------------------------------
  setTrace(trace) {
    this.trace = trace || null;
    if (!this.data) return;
    const now = performance.now();
    for (const id of trace?.ran || []) this.ranAt.set(id, now);
    let any = false;
    for (const el of this.el.nodes.querySelectorAll(".lg-node")) {
      const ran = (this.ranAt.get(+el.dataset.id) || 0) > now - 600;
      if (el.classList.contains("ran") !== ran) { el.classList.toggle("ran", ran); any = true; }
    }
    if (any) this.drawWires();
    if (this.data.view !== "graph" && trace?.states) {
      const m = this.machine;
      const cur = m && trace.states[m.name];
      for (const el of this.el.nodes.querySelectorAll(".lg-state")) {
        const s = m?.states.find(x => x.id === +el.dataset.state);
        el.classList.toggle("current", !!s && s.name === cur);
      }
    }
    this.renderMy(this.data);
    this.renderStatus(this.data);
    if (this.selVar) this.renderDetails();
  }
}

export const LOGIC_STYLE = `
#logicEditor{position:absolute;inset:0;z-index:5;display:flex;flex-direction:column;background:#151515;color:#d6d6d6;font-size:12px;--lg-acc:#0f6fd6;--lg-sel:#f2a32b;--lg-panel:#1e1e1e;--lg-line:#0c0c0c;--lg-hi:#2b2b2b}
#logicEditor .lg-i{width:16px;height:16px;flex:none;display:block}
#logicEditor .lg-i.red{color:#d5483f}
#logicEditor button{font:inherit;color:inherit;cursor:pointer}
.lg-bar{display:flex;align-items:center;gap:2px;height:40px;padding:0 6px;background:#1a1a1a;border-bottom:1px solid var(--lg-line);flex:none;overflow-x:auto;scrollbar-width:none}
.lg-bar::-webkit-scrollbar{display:none}
.lg-tb{display:flex;align-items:center;gap:6px;height:30px;padding:0 9px;border:1px solid transparent;border-radius:4px;background:transparent;white-space:nowrap}
.lg-tb:hover{background:#2a2a2a;border-color:#3a3a3a}
.lg-tb.on{background:#2d3f58;border-color:#3d5f8f}
.lg-tb .lg-dd{width:12px;height:12px;opacity:.7}
.lg-compile{font-weight:600}.lg-compile.pulse{background:#203a20}
.lg-sep{width:1px;height:22px;background:#333;margin:0 4px;flex:none}
.lg-spacer{flex:1}
.lg-ownerbox{display:flex;align-items:center;gap:6px;height:30px;padding:0 8px;border-radius:4px;background:#111;border:1px solid #333}
.lg-owner{background:transparent;color:#e8e8e8;border:0;max-width:180px;font:inherit;font-weight:600;outline:none}
.lg-owner option{background:#1e1e1e}
.lg-panelbtn,.lg-boxbtn,.lg-fab{display:none!important}
.lg-main{flex:1;display:flex;min-height:0;position:relative;overflow:hidden}
.lg-my,.lg-details{width:250px;flex:none;display:flex;flex-direction:column;background:var(--lg-panel);min-height:0}
.lg-my{border-right:1px solid var(--lg-line)}.lg-details{width:290px;border-left:1px solid var(--lg-line)}
.lg-ph{display:flex;align-items:center;justify-content:space-between;height:30px;padding:0 10px;background:#242424;border-bottom:1px solid var(--lg-line);font-weight:600;color:#eee;flex:none}
.lg-x{display:none;border:0;background:transparent;padding:4px}
.lg-search{display:flex;align-items:center;gap:6px;margin:6px;padding:0 8px;height:28px;border-radius:4px;background:#0f0f0f;border:1px solid #333}
.lg-search .lg-i{width:13px;height:13px;opacity:.6}
.lg-search input{flex:1;min-width:0;background:transparent;border:0;outline:none;color:#eee;font:inherit}
.lg-search:focus-within{border-color:var(--lg-acc)}
.lg-mylist{flex:1;overflow:auto;padding-bottom:12px}
.lg-sec-h{display:flex;align-items:center;height:28px;padding:0 4px 0 2px;background:#262626;border-top:1px solid #303030;border-bottom:1px solid #141414}
.lg-sec-t{flex:1;display:flex;align-items:center;gap:4px;height:100%;border:0;background:transparent;font-weight:700!important;text-transform:uppercase;letter-spacing:.06em;font-size:11px!important;color:#cfcfcf;text-align:left}
.lg-car{width:12px!important;height:12px!important;transition:transform .12s}
.lg-sec.closed .lg-car,.lg-pal-cat.closed .lg-car{transform:rotate(-90deg)}
.lg-sec.closed .lg-sec-b{display:none}
.lg-add{display:flex;align-items:center;justify-content:center;width:24px;height:22px;border:1px solid #3a3a3a;border-radius:3px;background:#1a1a1a}
.lg-add:hover{background:#2f6f2f;border-color:#4c9c4c}
.lg-add .lg-i{width:12px;height:12px}
.lg-sec-b{padding:3px 0}
.lg-item{display:flex;align-items:center;gap:7px;min-height:26px;padding:2px 10px 2px 14px;cursor:pointer;user-select:none}
.lg-item:hover{background:var(--lg-hi)}.lg-item.on{background:#24384f}
.lg-item .lg-i{width:14px;height:14px;opacity:.85}
.lg-vn{flex:1;min-width:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.lg-vt{color:#8a8a8a;font-size:11px}
.lg-live{font:11px ui-monospace,Menlo,monospace;color:#f2c94c;max-width:90px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.lg-pill{display:inline-block;width:20px;height:9px;border-radius:5px;background:var(--c);flex:none;box-shadow:inset 0 0 0 1px rgba(0,0,0,.4)}
.lg-pill.big{width:26px;height:12px;margin-right:6px}
.lg-acts{display:none;gap:3px}
.lg-item:hover .lg-acts{display:flex}.lg-item:hover .lg-vt,.lg-item:hover .lg-live{display:none}
.lg-acts button{height:20px;padding:0 6px;border:1px solid #444;border-radius:3px;background:#161616;font-size:11px!important}
.lg-acts button:hover{border-color:var(--lg-acc)}
.lg-none{padding:4px 14px;color:#808080;font-size:11.5px}
.lg-center{flex:1;display:flex;flex-direction:column;min-width:0;min-height:0}
.lg-tabs{display:flex;align-items:flex-end;gap:2px;height:32px;padding:0 6px;background:#1a1a1a;border-bottom:1px solid var(--lg-line);flex:none;overflow-x:auto;scrollbar-width:none}
.lg-tab{display:flex;align-items:center;gap:6px;height:27px;padding:0 12px;border:1px solid transparent;border-bottom:0;border-radius:5px 5px 0 0;background:transparent;color:#a8a8a8;white-space:nowrap}
.lg-tab .lg-i{width:13px;height:13px}
.lg-tab.active{background:#262626;border-color:#333;color:#fff;box-shadow:inset 0 2px 0 var(--lg-acc)}
.lg-tab:hover:not(.active){color:#ddd;background:#222}
.lg-tab.add{padding:0 8px}
.lg-canvas{position:relative;flex:1;overflow:hidden;outline:none;cursor:default;touch-action:none;background-color:#262626;
  background-image:linear-gradient(#2c2c2c 1px,transparent 1px),linear-gradient(90deg,#2c2c2c 1px,transparent 1px),linear-gradient(#1c1c1c 1px,transparent 1px),linear-gradient(90deg,#1c1c1c 1px,transparent 1px)}
.lg-canvas.far{background-image:linear-gradient(transparent 1px,transparent 1px),linear-gradient(90deg,transparent 1px,transparent 1px),linear-gradient(#1c1c1c 1px,transparent 1px),linear-gradient(90deg,#1c1c1c 1px,transparent 1px)}
.lg-world{position:absolute;left:0;top:0;transform-origin:0 0}
.lg-wires{position:absolute;left:0;top:0;width:1px;height:1px;overflow:visible;pointer-events:none}
.lg-temp{position:absolute;inset:0;width:100%;height:100%;pointer-events:none;overflow:visible;z-index:2}
.lg-wire{fill:none;stroke-width:2;stroke-linecap:round;opacity:.92}
.lg-wire.exec{stroke-width:2.8}
.lg-wire.hot{stroke-width:3.2;opacity:1;filter:drop-shadow(0 0 3px rgba(255,255,255,.35))}
.lg-wire.exec.hot{stroke-width:3.8}
.lg-wire.sel{stroke:#f2a32b!important;stroke-width:3.6;opacity:1}
.lg-wire.temp{stroke-width:2.6;opacity:1}
.lg-wire.ran{stroke-dasharray:3 9;animation:lgflow .35s linear infinite;stroke-width:4;filter:drop-shadow(0 0 4px #9bd2ff)}
@keyframes lgflow{to{stroke-dashoffset:-12}}
.lg-hit{fill:none;stroke:transparent;stroke-width:14;pointer-events:stroke;cursor:pointer}
.lg-hit:hover{stroke:rgba(255,255,255,.07)}
.lg-nodes{position:absolute;left:0;top:0}
.lg-cboxes{position:absolute;left:0;top:0}
.lg-crumb{position:absolute;left:12px;top:8px;font-size:13px;color:#bdbdbd;font-weight:600;pointer-events:none;text-shadow:0 1px 2px #000}
.lg-crumb span{color:#777;margin:0 4px}
.lg-water{position:absolute;right:18px;bottom:12px;font-size:34px;font-weight:800;letter-spacing:.04em;color:rgba(255,255,255,.09);pointer-events:none;user-select:none}
.lg-zoom{position:absolute;right:18px;top:8px;font-size:15px;font-weight:700;color:rgba(255,255,255,.28);pointer-events:none}
.lg-fit{position:absolute;right:10px;bottom:60px;width:34px;height:34px;border-radius:50%;border:1px solid #444;background:#1c1c1c;display:grid;place-items:center;opacity:.75}
.lg-fit:hover{opacity:1}
.lg-fab{position:absolute;right:14px;bottom:110px;width:52px;height:52px;border-radius:50%;border:0;background:var(--lg-acc);color:#fff;place-items:center;box-shadow:0 6px 20px rgba(0,0,0,.5)}
.lg-fab .lg-i{width:24px;height:24px}
/* nodes */
.lg-node{position:absolute;min-width:150px;max-width:340px;border-radius:7px;background:rgba(14,14,14,.9);border:1px solid #050505;box-shadow:0 4px 14px rgba(0,0,0,.55);user-select:none;-webkit-user-select:none}
.lg-node.selected{box-shadow:0 0 0 2px var(--lg-sel),0 4px 14px rgba(0,0,0,.55)}
.lg-node.flash{animation:lgflash .9s ease}
@keyframes lgflash{30%{box-shadow:0 0 0 4px #fff}}
.lg-node.error{box-shadow:0 0 0 2px #d23b3b,0 4px 14px rgba(0,0,0,.55)}
.lg-node.warn:not(.selected){box-shadow:0 0 0 1px #c9921e,0 4px 14px rgba(0,0,0,.55)}
.lg-node.ran{box-shadow:0 0 0 2px #8fd0ff,0 0 22px rgba(120,190,255,.55)}
.lg-nh{display:flex;align-items:center;gap:7px;height:28px;padding:0 10px 0 8px;border-radius:6px 6px 0 0;background:linear-gradient(90deg,var(--h) 0%,color-mix(in srgb,var(--h) 55%,#101010) 100%);color:#fff;font-weight:700;cursor:move;border-bottom:1px solid rgba(0,0,0,.5)}
.lg-ki{width:14px!important;height:14px!important;opacity:.95}
.k-event .lg-nh{border-radius:12px 6px 0 0}
.lg-title{white-space:nowrap;overflow:hidden;text-overflow:ellipsis;text-shadow:0 1px 1px rgba(0,0,0,.6)}
.lg-clock{margin-left:auto;opacity:.8}
.lg-params{padding:6px 10px 2px;display:flex;flex-direction:column;gap:4px;border-bottom:1px solid #1d1d1d}
.lg-param{display:flex;align-items:center;justify-content:space-between;gap:8px;color:#a9a9a9;font-size:11.5px}
.lg-pins{display:flex;justify-content:space-between;gap:16px;padding:5px 0 7px}
.lg-col{display:flex;flex-direction:column;gap:2px;min-width:0}.lg-col.out{align-items:flex-end}
.lg-row{display:flex;align-items:center;gap:5px;min-height:22px;padding:0 8px 0 4px;color:#d9d9d9;cursor:crosshair;border-radius:3px;white-space:nowrap}
.lg-row.out{padding:0 4px 0 8px}
.lg-row:hover .lg-pin{filter:drop-shadow(0 0 3px #fff)}
.lg-pin{width:15px;height:15px;flex:none;display:block}
.lg-pin.exec{width:16px;height:16px}
.lg-pl{font-size:12px}
.wiring .lg-row:not(.can){opacity:.35}
.wiring .lg-row.can .lg-pin{filter:drop-shadow(0 0 4px #fff)}
.lg-row.hover{background:rgba(255,255,255,.12)}
.lg-lit,.lg-par{height:20px;background:#0a0a0a;border:1px solid #3c3c3c;border-radius:3px;color:#f0f0f0;font:11.5px/1 inherit;padding:0 5px;min-width:0;outline:none}
.lg-lit:focus,.lg-par:focus{border-color:var(--lg-acc)}
.lg-lit.num{width:56px}.lg-lit.txt{width:120px}.lg-lit.act,.lg-par{max-width:150px}
select.lg-par,select.lg-lit{padding:0 2px}
.lg-lit.chk{width:15px;height:15px;accent-color:#d93b3b;margin:0 2px}
.lg-vec{display:flex;gap:3px}.lg-vec label{display:flex;align-items:center;gap:2px}.lg-vec i{font-style:normal;font-size:10px;color:#888}
.lg-vec .lg-lit.v{width:42px}
.lg-badge{position:absolute;left:50%;bottom:-11px;transform:translateX(-50%);padding:1px 8px;border-radius:3px;font-size:10px;font-weight:800;letter-spacing:.06em}
.lg-badge.err{background:#c62c2c;color:#fff}.lg-badge.warn{background:#c9921e;color:#111}
.lg-bubble{position:absolute;left:0;bottom:calc(100% + 8px);max-width:300px;padding:4px 9px;border-radius:5px;background:#e8e8e8;color:#111;font-size:11.5px;white-space:pre-wrap;pointer-events:none}
.lg-bubble:after{content:"";position:absolute;left:14px;top:100%;border:6px solid transparent;border-top-color:#e8e8e8}
/* compact nodes */
.lg-node.compact{min-width:0;display:flex;align-items:center;border-radius:12px;padding:4px 2px;background:rgba(18,18,18,.92);cursor:move}
.lg-node.compact.var{border-radius:14px;padding:3px 0 3px 12px;border-left:3px solid var(--c);gap:4px}
.lg-vname{font-weight:700;color:#f0f0f0;white-space:nowrap}
.lg-node.compact.op{gap:2px;padding:4px 0}
.lg-node.compact.op .lg-row{min-height:24px}
.lg-sym{min-width:34px;padding:0 6px;text-align:center;font-size:26px;font-weight:300;color:#cfcfcf;line-height:1}
.lg-sym.word{font-size:14px;font-weight:800;letter-spacing:.04em}
.lg-node.knot{min-width:0;width:24px;height:24px;border:0;background:transparent;box-shadow:none;cursor:move}
.lg-knot{position:absolute;left:6px;top:6px;width:12px;height:12px;border-radius:50%;background:var(--c);box-shadow:0 0 0 2px rgba(0,0,0,.5);pointer-events:none}
.lg-node.knot.selected{box-shadow:none}.lg-node.knot.selected .lg-knot{box-shadow:0 0 0 3px var(--lg-sel)}
.lg-node.knot .lg-row{position:absolute;top:0;height:24px;min-height:0;padding:0;width:12px}
.lg-node.knot .lg-row.in{left:-2px}.lg-node.knot .lg-row.out{right:-2px}
/* comments */
.lg-cbox{position:absolute;border-radius:5px;background:color-mix(in srgb,var(--c) 22%,transparent);border:1px solid color-mix(in srgb,var(--c) 60%,#000)}
.lg-cbox.selected{border-color:var(--lg-sel);box-shadow:0 0 0 1px var(--lg-sel)}
.lg-ch{height:30px;line-height:30px;padding:0 10px;border-radius:4px 4px 0 0;background:color-mix(in srgb,var(--c) 70%,#111);color:#fff;font-size:15px;font-weight:600;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;cursor:move}
.lg-cedit{width:100%;height:24px;background:#0a0a0a;border:1px solid var(--lg-acc);color:#fff;font:inherit;padding:0 6px}
.lg-cres{position:absolute;right:0;bottom:0;width:18px;height:18px;cursor:nwse-resize;background:linear-gradient(135deg,transparent 50%,rgba(255,255,255,.35) 50%,rgba(255,255,255,.35) 60%,transparent 60%,transparent 70%,rgba(255,255,255,.35) 70%,rgba(255,255,255,.35) 80%,transparent 80%)}
/* state machines */
.lg-state{position:absolute;min-width:170px;border-radius:12px;background:#1d2a38;border:2px solid #3e5c7a;box-shadow:0 4px 14px rgba(0,0,0,.55);user-select:none;cursor:move}
.lg-state.initial{border-color:#5aa0e6}
.lg-state.current{box-shadow:0 0 0 3px #4cc06e,0 0 24px rgba(76,192,110,.55)}
.lg-state.selected{border-color:var(--lg-sel)}
.lg-state.any{background:#262626;border-style:dashed;border-color:#666}
.lg-sh{padding:10px 30px 2px 14px;font-size:14px;font-weight:700;color:#fff}
.lg-sb{padding:0 14px 10px;font-size:11px;color:#9cb0c6}
.lg-handle{position:absolute;right:-12px;top:50%;margin-top:-12px;width:24px;height:24px;border-radius:50%;background:#5aa0e6;color:#0d1622;display:grid;place-items:center;cursor:crosshair;opacity:.8}
.lg-handle .lg-i{width:13px;height:13px}
.lg-state:hover .lg-handle{opacity:1}
.lg-entry{position:absolute;padding:6px 14px;border-radius:14px;background:#3b3b3b;color:#ddd;font-weight:700;border:1px solid #555}
.lg-entry:after{content:"";position:absolute;left:100%;top:50%;width:62px;border-top:2px solid #bbb}
.lg-trans{fill:none;stroke:#e6e6e6;stroke-width:2.2}.lg-trans.sel{stroke:var(--lg-sel)}.lg-trans.temp{stroke:#5aa0e6;stroke-dasharray:6 4}
.lg-tico{position:absolute;transform:translate(-50%,-50%);width:24px;height:24px;border-radius:50%;background:#2b2b2b;border:2px solid #bbb;display:grid;place-items:center;font-size:12px;cursor:pointer;z-index:1}
.lg-tico span{position:absolute;left:28px;top:50%;transform:translateY(-50%);padding:2px 7px;border-radius:4px;background:rgba(0,0,0,.8);white-space:nowrap;font-size:11px;color:#ddd;pointer-events:none}
.lg-tico.sel{border-color:var(--lg-sel)}
.lg-marquee{position:absolute;border:1px solid #6fb2ff;background:rgba(80,150,255,.12);pointer-events:none;z-index:3}
.lg-empty{position:absolute;left:50%;top:42%;transform:translate(-50%,-50%);text-align:center;color:#a8a8a8;line-height:1.8;max-width:90%}
.lg-e1{font-size:16px;color:#ddd}.lg-e1 b{color:#fff}
.lg-starts{display:flex;gap:6px;justify-content:center;margin-top:10px;flex-wrap:wrap}
.lg-starts button{height:32px;padding:0 14px;border-radius:5px;border:1px solid #6a2020;background:#3a1414;color:#fff}
.lg-starts button:hover{background:#5a1c1c}
.lg-starts button[data-tpl]{border-color:#444;background:#222}
/* action menu */
.lg-palette{position:absolute;width:340px;display:flex;flex-direction:column;border-radius:6px;background:#1c1c1c;border:1px solid #444;box-shadow:0 16px 44px rgba(0,0,0,.7);z-index:6}
.lg-pal-h{display:flex;align-items:center;justify-content:space-between;gap:8px;padding:8px 10px 2px;font-weight:700;color:#eee}
.lg-pal-ctx{display:flex;align-items:center;gap:4px;font-weight:400;font-size:11px;color:#aaa}
.lg-pal-extra{padding:0 6px 4px}.lg-pal-extra button{display:flex;align-items:center;gap:6px;width:100%;height:26px;padding:0 8px;border:0;border-radius:3px;background:transparent;text-align:left}
.lg-pal-extra button:hover{background:#2a2a2a}
.lg-pal-list{flex:1;min-height:120px;overflow:auto;padding:0 4px 4px;border-top:1px solid #2a2a2a}
.lg-pal-cat{display:flex;align-items:center;gap:4px;padding:5px 6px 3px;font-weight:700;color:#e0e0e0;cursor:pointer;user-select:none}
.lg-pal-it{display:flex;align-items:center;gap:7px;padding:3px 8px 3px 22px;border-radius:3px;cursor:pointer;color:#cfcfcf}
.lg-pal-it .lg-i{width:13px;height:13px;opacity:.75}
.lg-pal-it b{color:#f2c94c}
.lg-pal-it.active{background:var(--lg-acc);color:#fff}
.lg-pal-help{min-height:36px;padding:6px 10px;font-size:11px;color:#a8a8a8;border-top:1px solid #2a2a2a;line-height:1.4}
/* results */
.lg-results{flex:none;display:flex;flex-direction:column;background:var(--lg-panel);border-top:1px solid var(--lg-line)}
.lg-rtabs{display:flex;align-items:center;height:28px;padding:0 4px;background:#1a1a1a}
.lg-rtabs [data-r]{height:24px;padding:0 10px;border:0;border-radius:3px;background:transparent;color:#aaa}
.lg-rtabs [data-r].on{color:#fff;background:#2a2a2a}
.lg-rcount{margin-left:4px;padding:0 5px;border-radius:7px;font-size:10px}.lg-rcount.error{background:#c62c2c;color:#fff}.lg-rcount.warn{background:#c9921e;color:#111}
.lg-rtoggle{border:0;background:transparent;padding:4px}.lg-rtoggle .lg-i{transform:rotate(180deg)}
.lg-results.open .lg-rtoggle .lg-i{transform:none}
.lg-rbody{display:none;height:140px;overflow:auto}
.lg-results.open .lg-rbody{display:block}
.lg-res{display:flex;align-items:center;gap:8px;padding:4px 10px;cursor:pointer;border-bottom:1px solid #1a1a1a}
.lg-res:hover{background:var(--lg-hi)}.lg-res .lg-i{width:14px;height:14px}
.lg-res em{margin-left:auto;color:#808080;font-style:normal;font-size:11px}
.lg-res.none,.lg-res.ok{cursor:default;color:#9a9a9a}
/* details */
.lg-dbody{flex:1;overflow:auto;padding:8px 10px 20px}
.lg-dh{display:flex;align-items:center;gap:8px;font-size:14px;font-weight:700;color:#fff;margin:2px 0}
.lg-dhc{width:12px;height:12px;border-radius:3px;flex:none}
.lg-dsub{color:#8c8c8c;margin-bottom:8px}
.lg-dtxt{color:#b4b4b4;line-height:1.5;margin:6px 0}
.lg-dsec{margin:10px 0;border-top:1px solid #2c2c2c;padding-top:6px}
.lg-dst{font-size:11px;font-weight:700;letter-spacing:.06em;text-transform:uppercase;color:#9a9a9a;margin-bottom:4px}
.lg-drow{display:flex;align-items:center;justify-content:space-between;gap:8px;min-height:28px}
.lg-drow label{display:flex;align-items:center;gap:5px;color:#c8c8c8;min-width:0}
.lg-drow>div{display:flex;justify-content:flex-end;min-width:0}
.lg-drow input:not([type=checkbox]),.lg-drow select{width:150px;height:24px;background:#0f0f0f;border:1px solid #3c3c3c;border-radius:3px;color:#fff;padding:0 6px;font:inherit}
.lg-drow .lg-lit.num{width:80px}.lg-drow .lg-vec .lg-lit.v{width:46px}
.lg-dbody textarea{width:100%;box-sizing:border-box;background:#0f0f0f;border:1px solid #3c3c3c;border-radius:3px;color:#fff;padding:5px 6px;font:inherit;resize:vertical}
.lg-dim{color:#808080}
.lg-dissue{display:flex;gap:6px;align-items:flex-start;padding:5px 7px;border-radius:4px;margin:4px 0;line-height:1.4}
.lg-dissue.err{background:rgba(210,59,59,.15);color:#ffb3ae}.lg-dissue.warn{background:rgba(201,146,30,.14);color:#f5d27f}
.lg-dacts{display:flex;flex-wrap:wrap;gap:6px;margin-top:12px}
.lg-dacts button{height:28px;padding:0 12px;border:1px solid #444;border-radius:4px;background:#262626}
.lg-dacts button:hover{border-color:var(--lg-acc)}
.lg-dacts button.danger{border-color:#6a2626;background:#3a1717}
.lg-swatches{display:flex;gap:6px}.lg-swatches button{width:26px;height:26px;border-radius:5px;border:2px solid transparent}.lg-swatches button.on{border-color:#fff}
.lg-keys{width:100%;border-collapse:collapse;font-size:11.5px}.lg-keys td{padding:3px 0;color:#bbb;vertical-align:top}.lg-keys td:first-child{color:#e8e8e8;padding-right:8px;white-space:nowrap}
.lg-scrim{display:none}
.lg-bpdot{position:absolute;left:-7px;top:-7px;width:14px;height:14px;border-radius:50%;background:#e03131;box-shadow:0 0 0 2px #111;z-index:1}
.lg-node.hit{box-shadow:0 0 0 3px #e03131,0 0 26px rgba(224,49,49,.7)!important}
.lg-debug{position:absolute;left:50%;top:10px;transform:translateX(-50%);display:flex;align-items:center;gap:8px;padding:5px 6px 5px 12px;border-radius:6px;background:#2a1414;border:1px solid #7a2424;z-index:4;white-space:nowrap}
.lg-debug:not(.paused){background:#1f1f1f;border-color:#444}
.lg-debug button{height:26px;padding:0 12px;border-radius:4px;border:1px solid #555;background:#262626}
.lg-debug .lg-resume{background:#2f6f2f;border-color:#4c9c4c}
.lg-debug [hidden]{display:none}
/* tablets: Details on demand */
@media (max-width:1100px){
  .lg-details{position:absolute;right:0;top:0;bottom:0;z-index:8;transform:translateX(100%);transition:transform .18s;box-shadow:-10px 0 30px rgba(0,0,0,.5)}
  #logicEditor.show-details .lg-details{transform:none}
  .lg-detbtn{display:flex!important}
  .lg-details .lg-x{display:block}
}
/* phones: panels are sheets, big touch targets, a + button */
@media (max-width:760px){
  .lg-tb span,.lg-sep,.lg-cpp,.lg-help{display:none}
  .lg-mybtn,.lg-boxbtn,.lg-fab{display:flex!important}
  .lg-fab{display:grid!important}
  .lg-owner{max-width:120px}
  .lg-my{position:absolute;left:0;top:0;bottom:0;z-index:8;width:min(86vw,320px);transform:translateX(-100%);transition:transform .18s;box-shadow:10px 0 30px rgba(0,0,0,.5)}
  #logicEditor.show-my .lg-my{transform:none}
  .lg-details{left:0;right:0;top:auto;width:auto;height:min(62%,460px);border-left:0;border-top:1px solid #333;border-radius:12px 12px 0 0;transform:translateY(100%)}
  #logicEditor.show-details .lg-details{transform:none}
  .lg-my .lg-x{display:block}
  #logicEditor.show-my .lg-scrim,#logicEditor.show-details .lg-scrim{display:block;position:absolute;inset:40px 0 0;z-index:7;background:rgba(0,0,0,.35)}
  .lg-item{min-height:38px}.lg-acts{display:flex}.lg-vt{display:none}
  .lg-acts button{height:28px;padding:0 10px}
  .lg-row{min-height:28px}.lg-pin{width:18px;height:18px}.lg-pin.exec{width:19px;height:19px}
  .lg-lit,.lg-par{height:26px;font-size:13px}
  .lg-results:not(.open) .lg-rtabs [data-r="find"]{display:none}
  .lg-water{font-size:22px}.lg-zoom{top:auto;bottom:12px;left:12px;right:auto;font-size:12px}
  .lg-palette.sheet{left:0!important;right:0;bottom:0;top:auto!important;width:auto;max-height:72%;border-radius:12px 12px 0 0}
  .lg-palette.sheet .lg-pal-it{padding:9px 10px 9px 24px;font-size:14px}
  .lg-palette.sheet .lg-pal-cat{padding:9px 6px 5px;font-size:13px}
  .lg-crumb{font-size:12px}
}
`;
