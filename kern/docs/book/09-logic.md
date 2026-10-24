# 9 Logic Graphs

Kern has **no scripting language**. Game logic consists of graphs in the style of Unreal's
Blueprints: a graph is pure data (nodes, wires, literals, variables, state machines), and every node
type is a C++ function in a registry. Graphs are checked, translated into an index table and executed
by a small interpreter in the engine's fixed simulation steps – only on the authority (the
single-player game or the network server). Their effects are replicated like everything else the
simulation does.

So that logic is not slow, Kern translates every graph at runtime into **WebAssembly** (JIT) when
Play or Simulate starts; the browser turns that into machine code. And if you like, you can export a
graph as **readable C++** and compile it into the game. All three tiers must deliver exactly the same
result – why, and how this is ensured, is explained in Section 9.9.

```
include/kern/logic/logic.h     data model, Library, check, Runtime, Instance, Context, codegen API
src/logic/logic.cpp            values, graph JSON, Library, check, interpreter
src/logic/core_nodes.cpp       core nodes (events, flow, variables, state machines, math, text …)
src/logic/codegen.cpp          compile_wasm (JIT) and generate_cpp (export)
src/logic/logic_api.cpp        C ABI between engine and JIT modules
src/game/logic_nodes.cpp       game nodes (actors, physics, combat, robots …)
src/game/logic_world.cpp       graphs in the GameWorld: host, events, replication
src/studio/logic_edit.cpp      editing in Studio
web/studio/logic.mjs           Blueprint editor (user interface)
web/kern.mjs                   JIT connection in the browser
tests/logic_cases.h            cases that must run identically in every tier
```

---

## 9.1 The model

From the header comment of `logic.h` (Unreal Blueprint semantics):

- **Exec pins** (white arrows): what runs next. Event nodes begin a chain, action nodes continue it,
  flow nodes branch, loop or wait.
- **Data pins**: typed values. Inputs **pull**: pure nodes (math, getters) are evaluated on demand;
  outputs of action nodes keep the value they had at their last run. Unconnected inputs use the
  literal entered in the node.
- **Variables**: per graph instance, typed, with a default value.
- **State machines**: states and transitions in the style of the state editor of
  digitalisierungsplanung.de. A transition fires on an event, a timer, a condition variable or
  automatically; entering and leaving a state trigger events.
- **Latent nodes**: Delay, Tween and Retriggerable Delay interrupt the chain and continue it in the
  tick, like latent Blueprint actions.

A level has a **level graph** (Self = none); every actor can carry a graph of its own (its Self).

### Data structures

| Structure | Fields |
|---|---|
| `Graph` | `nodes`, `links`, `variables`, `machines`, `comments`, `next_id` |
| `Node` | `id`, `type` (registry key, e.g. `flow.branch`), `x`, `y` (canvas pixels), `values` (literals of unconnected inputs, per pin ID), `params` (settings: variable name, event name, key …), `comment` |
| `Link` | `from` (node), `out` (output pin), `to`, `in` |
| `Variable` | `name`, `type`, `value` (default), `help` |
| `StateMachine` | `id`, `name`, `states`, `transitions` |
| `State` | `id`, `name`, `x`, `y`, `initial` |
| `Transition` | `id`, `from` (0 = from any state), `to`, `trigger` (`event`, `timer`, `condition`, `automatic`), `event`, `seconds` (default 1), `variable`, `label` |
| `CommentBox` | `id`, `x`, `y`, `w` (400), `h` (200), `text`, `color` (0 = gray) – for the editor only, no effect |

### Pin types and values

`PinType` is `exec`, `boolean`, `integer`, `number`, `vector`, `text`, `actor` (in JSON `exec`,
`bool`, `int`, `float`, `vector`, `text`, `actor`; on reading also `number`, `double`, `boolean`,
`integer`, `string`). A `Value` carries a type and the matching content (`b`, `i`, `n`, `v`, `s`,
`a`; `ActorRef` is the actor ID).

Which wires are allowed is determined by `convertible(from, to)`:

- the same type always; exec only with exec;
- **everything to `text`** ("everything prints");
- numbers among themselves (`bool`, `int`, `float`);
- `actor` → `bool` ("is valid");
- `float`/`int` → `vector` (into all three components).

The conversions (`as_bool`, `as_int`, `as_number`, `as_vector`, `as_text`, `as_actor`) are fixed,
e.g. the text "0" or "false" is false, a vector as a number is its length, a number converted to
`int` is truncated, text is parsed as a number.

### The JSON format

Graphs are stored in the level (`logic` in the level and in every actor,
[Chapter 8](08-game-runtime.md)):

```json
{
  "kind": "kern-logic", "version": 1, "nextId": 5,
  "variables": [{"name": "Spawned", "type": "int", "value": 0}],
  "nodes": [
    {"id": 1, "type": "game.overlap_begin", "x": 0, "y": 0},
    {"id": 2, "type": "game.add_score", "x": 320, "y": 0, "values": {"points": 10}},
    {"id": 3, "type": "game.message", "x": 640, "y": 0, "values": {"text": "+10"}},
    {"id": 4, "type": "game.sound", "x": 960, "y": 0, "params": {"sound": "builtin:pickup"}}
  ],
  "links": [[1, "then", 2, "exec"], [2, "then", 3, "exec"], [3, "then", 4, "exec"]],
  "machines": [],
  "comments": [{"id": 9, "x": -64, "y": -112, "w": 1472, "h": 592, "text": "…"}]
}
```

Wires are arrays `[from, out, to, in]`. Transitions write `event`, `seconds` or `variable` depending
on the trigger; `comments` only appears if there are any. Positions are rounded to whole pixels.

---

## 9.2 Node definitions and the library

Every node type is a `NodeDef`:

| Field | Meaning |
|---|---|
| `type`, `label`, `category`, `help`, `keywords` | key, title, category, help, search words |
| `kind` | `event`, `action`, `pure`, `flow` |
| `event` | events: the `Event::type` the node handles (e.g. `logic.tick`, `game.overlap_begin`) |
| `latent` | waits (Delay, Tween) |
| `inputs`, `outputs` | pins in drawing order, exec pins included (`PinDef`: `id`, `label`, `type`, `def`, `hidden`) |
| `params` | settings (`ParamDef`: `key`, `label`, `kind` – `text`, `choice`, `variable`, `event`, `machine`, `state`, `key`, `sound`, `tag`, `integer` –, `choices`, `def`) |
| `color` | header color (0 = by kind) |
| `shape` | pins that depend on parameters or on the graph (variables, sequence outputs, switch cases) |
| `title` | title that depends on parameters ("Every 3 s", "On Key F") |
| `accepts` | events: does this node handle the event? Default: `key` empty, or equal to the first parameter, or `any` |
| `run` | the C++ function |

`run` returns the **index of the exec output** to continue with (among the exec outputs), or
`kStop` (−1, the chain ends) or `kSuspend` (−2, latent: resumed later from the tick). Pure nodes
write their outputs and return `kStop`.

A node function sees the world through `Context`:

| Method | Purpose |
|---|---|
| `in(index)`, `in(pin)`, `in_bool`, `in_number`, `in_int`, `in_vector`, `in_text`, `in_actor` | inputs (pulls connected pure nodes) |
| `out(pin, value)` | set an output |
| `param(key, fallback)` | setting |
| `exec(pin)` | ordinal of an exec output (for the return value) |
| `entry()` | which exec input started the node (0 = the first; `kResume` = 1000 after a wait) |
| `state()`, `slot(k)` | state of the node instance; `slot(0..3)` lives in memory shared with generated code |
| `wait(seconds, entry)` | latent: continue with `entry` after `seconds`, returns `kSuspend` |
| `cancel_waits()` | discard its own waits (Retriggerable Delay) |
| `then(out)` | continue at this output after the current chain ends (Sequence) |
| `loop_back(entry)` | re-enter this node with `entry` after the current chain ends (loops) |
| `self()`, `host()`, `runtime()`, `instance()` | context |

`Library::core()` contains the built-in nodes; `logic::library()` is core plus whatever the game has
registered. The game nodes register themselves at program start (`logic_nodes.cpp`;
`game::logic_nodes_registered()` ensures that this file gets linked).

A minimal example from `logic_nodes.cpp`, an action:

```cpp
lib.add(action("game.add_score", "Add Score", "Game",
               {data("points", "Points", PinType::number, 10.0)}, {},
               [](Context& c, GameWorld& w) { w.add_score(static_cast<f32>(c.in_number("points"))); },
               "Adds points (wins at the target score when the goal is Score).", "points reward"));
```

`action(...)` builds a `NodeDef` from this with one exec input and one exec output `then`, whose
`run` calls the function with the GameWorld and returns 0.

---

## 9.3 Checking

`check(graph, lib)` returns `Issue`s (`node`, `error`, `text`). Errors:

- unknown node type;
- parameters that point to missing variables, state machines or states; an event without a name;
- a wire to a pin that does not exist;
- an exec wire on a data pin, non-convertible types;
- a data input with more than one wire;
- pure nodes that depend on themselves (cycle);
- a state machine without exactly one initial state; an event transition without an event name; a
  condition transition on a missing variable.

Warning: a node with an exec input that nothing is connected to ("never runs").

A graph with errors is **disabled** at start and reported; the rest of the game keeps running. In
Studio, Compile (F7) shows the results.

---

## 9.4 Execution

### Runtime and instances

A `Runtime` executes all graphs of a world; every graph runs as an `Instance` for an owner (actor or
level).

| Method | Effect |
|---|---|
| `add(graph, self, name)` | translates and adds; check errors disable the instance; afterwards native C++ code (if registered) or a JIT request |
| `remove(self)`, `clear()` | removes (including waiting latent actions) |
| `begin()` | Begin Play for instances that have not begun yet: enter initial states, run `event.begin` |
| `tick(dt)` | one fixed step: due latent actions (sorted by time), then the tick event (`logic.tick` with `delta`), then the state machines' timer and condition transitions |
| `fire(event)` | fires an event synchronously |
| `call(name, target, args)` | custom event on all instances (or the target's) and event transitions with this name |
| `find(self)`, `instances()` | access |
| `trace()`, `tracing` | which nodes ran (for the editor) |
| `budget` | 200,000 node executions per step |
| `jit` | enable the JIT (default: on, where the host can) |

An `Instance` offers `get`/`set` for variables, `state`/`set_state` for state machines,
`disabled()`, `compiled()` and `tier()` – `"interpreter"`, `"compiling"`,
`"compiled (WebAssembly)"` or `"native C++"`.

### Events

```cpp
struct Event {
  std::string type;          // e.g. "game.overlap_begin"
  ActorRef target = 0;       // only instances with this Self (0 = all)
  bool level = false;        // with target: the level graph gets it too
  std::string key;           // event-specific: custom event name, key code, action, "Machine/State"
  std::vector<std::pair<std::string, Value>> args;
};
```

`fire` goes over all non-disabled instances (matching the target), looks for event nodes with a
matching `event` and `accepts`, writes the arguments into the outputs of the same name and executes
the chain. Instances that are added in the meantime (spawned actors) are included.

### Running a chain

The interpreter (`Runtime::run_from`) works with a stack of (node, input) pairs:

1. Execute the node; whatever it schedules via `then` or `loop_back` is pushed onto the stack – in
   reverse order, so that what was scheduled first runs first.
2. If it returns a valid exec output, execution continues at the connected node; otherwise this
   branch ends and the next stack entry runs.

This is how Sequence (output 0 immediately, the others scheduled) and loops (execute the body, then
re-enter the node itself via `loop_back` with `kResume`) work.

Data is **pulled**: an input connected to a pure node executes that node's `run` on every read
(depth at most 48). An input on an action node reads that node's last stored output.

### Protection against infinite loops

- More than `budget` (200,000) nodes in one step: the instance is disabled ("infinite loop? The graph
  is stopped.").
- Events that trigger events may nest at most 64 deep; beyond that, the chain is stopped.
- In compiled code: more than 512 nested loops/sequences stop the instance.

### State machines

- `begin` enters the initial state.
- Entering and leaving fire `logic.state_enter` / `logic.state_exit` with the key
  `"Machine/State"`; "On State Entered" receives the previous state.
- Transitions are checked by `check_transitions` – **at most one transition per machine and call**
  (automatic chains advance one state per step):
  - `event`: on `call` with this name,
  - `timer`: after `seconds` in the state,
  - `condition`: as long as the boolean variable is true,
  - `automatic`: immediately on the next tick.
- `from = 0` means "from any state" (but not back into the target state itself).
- `Set State` (`state.set`) switches directly.

---

## 9.5 Core nodes

`src/logic/core_nodes.cpp` registers 76 nodes:

| Category | Nodes |
|---|---|
| **Events** | `event.begin` Begin Play · `event.tick` Event Tick (Delta Seconds) · `event.every` Every N Seconds (interval parameter, keeps its own time, output Count) · `event.custom` Custom Event (name; outputs Actor, Value, Text; also switches state machine transitions of the same name) |
| **State machines** | `event.state_enter` On State Entered · `event.state_exit` On State Left · `state.set` Set State · `state.get` Current State · `state.is` Is In State |
| **Flow** | `flow.reroute` / `value.reroute` (nodes for wires) · `flow.branch` Branch · `flow.sequence` Sequence (1..16 outputs) · `flow.for` For Loop (First, Last inclusive; Index) · `flow.while` While Loop · `flow.do_once` Do Once (Reset, Start Closed) · `flow.do_n` Do N (N, Reset, Counter) · `flow.gate` Gate (Enter, Open, Close, Toggle, Start Closed) · `flow.flip_flop` Flip Flop (A/B, Is A) · `flow.delay` Delay (calls during the wait are ignored) · `flow.retrigger_delay` Retriggerable Delay · `flow.tween` Tween (Play, Stop, Duration; Update every step with Alpha 0→1, easing linear/smooth/in/out, then Finished) · `flow.switch_int` Switch on Int · `flow.switch_text` Switch on Text (cases separated by commas) · `flow.call` Call Event (Target, Actor, Value, Text) |
| **Variables** | `var.get` · `var.set` · `var.add` (Amount, −1 counts down) |
| **Math** | `math.add`, `sub`, `mul`, `div`, `mod`, `min`, `max`, `clamp`, `abs`, `negate`, `lerp`, `sin`/`cos`/`atan2` (degrees), `sqrt`, `pow`, `round`, `floor`, `random` (float in range), `random_int`, `random_bool` |
| **Logic** | `math.compare` Compare (`==`, `!=`, `<`, `<=`, `>`, `>=`; equality with 10⁻⁹ tolerance) · `logic.and`, `or`, `xor`, `not` · `logic.select` Select |
| **Values** | `value.float`, `value.int`, `value.bool`, `value.text` |
| **Vector** | `vector.make`, `break`, `add`, `sub`, `cross`, `scale`, `length`, `normalize`, `distance`, `dot`, `lerp` |
| **Text** | `text.append`, `equal`, `contains`, `length` |
| **Utilities** | `time.now` Game Time · `debug.print` Print (Text, On Screen) |

Random numbers come from the host (`Host::random()`) and are **deterministic per world** – replays and
the network agree.

---

## 9.6 Game nodes

`src/game/logic_nodes.cpp` registers 71 nodes. They call functions of the GameWorld
([Chapter 8](08-game-runtime.md)); an actor input without a wire (0) means **Self**.

### Events

| Node | Event | Outputs | Triggered by |
|---|---|---|---|
| On Begin Overlap | `game.overlap_begin` | Actor, Other Actor | something enters this actor's sensor collider |
| On End Overlap | `game.overlap_end` | Actor, Other Actor | something leaves it |
| On Entered Sensor | `game.overlapped_begin` | Actor, Sensor Actor | this actor (e.g. a player) enters someone else's sensor |
| On Hit | `game.hit` | Actor, Other, Impact Speed, Point, Normal | Box3D hit from 2 m/s, both sides, at most 32 per step |
| On Collected | `game.pickup` | Actor, Collector, Points | a pickup was collected |
| On Trigger | `game.trigger` | Actor, Player | a player touched this trigger |
| On Damaged | `game.damaged` | Actor, Attacker, Damage, Headshot | damage (requires Health) |
| On Death | `game.death` | Actor, Killer, Headshot | health 0 |
| On Respawn | `game.respawn` | Actor | return after death |
| On Interact | `game.interact` | Actor, Player, Point | Interact (F / Y on the gamepad) while looking at the actor (3 m ray) or next to it (nearest actor with a graph within 2 m) |
| On Player Joined | `game.pawn_added` | Player | a player joined |
| On Game Over | `game.over` | Won, Message | game won or lost (to all graphs) |
| On Input Action | `game.input` | Player, Pressed | action `jump`, `fire`, `aim`, `reload`, `crouch`, `interact`, `boost`; edge pressed/released/both – for every player and every device |
| On Key | `game.key` | Pressed, Player | a key of this keyboard (local player only; parameter "F" → `KeyF`, "1" → `Digit1`) |

Overlaps count the visitor as a whole actor: an overlap begins with the first shape in the sensor and
ends with the last one that leaves it (a player's capsule and head are one visitor).

### Actors

`actor.self` Self · `actor.player` Get Player (0 = this machine's player, 1, 2 … further ones) ·
`actor.player_count` · `actor.find` Find Actor by Name · `actor.find_tag` Find Actor with Tag ·
`actor.random_tag` · `actor.has_tag` · `actor.name` · `actor.valid` · `actor.location` ·
`actor.rotation` · `actor.forward` · `actor.velocity` · `actor.distance` · `actor.set_location` ·
`actor.set_rotation` (degrees) · `actor.move_by` · `actor.spawn` Spawn Actor (copy of a template with
children and logic, replicated to all players) · `actor.destroy` · `actor.enable` Set Enabled ·
`actor.set_property` Set Property (any component value via its path as in the level file, e.g.
`components.mesh.material.color` – replicated)

### Physics

`actor.set_velocity` · `actor.impulse` Add Impulse · `actor.force` Add Force (for one step) ·
`actor.launch` Launch (keeps the horizontal velocity, adds this one) · `actor.simulate` Set Simulate
Physics (dynamic ↔ kinematic) · `actor.joint_motor` Set Joint Motor (hinge, slider, rope: doors,
lifts, winches) · `actor.explode` Explosion (Box3D radial impulse with flash, sound and falling-off
damage) · `physics.trace` Line Trace (Box3D ray)

### Game, combat, effects, audio

`game.score` Get Score · `game.message` Show Message (all players see it) · `game.add_score` ·
`game.win` · `game.lose` · `combat.damage` Apply Damage · `combat.heal` · `combat.kill` ·
`combat.ammo` Give Ammo · `actor.health` Get Health · `actor.set_light` · `actor.set_color` ·
`game.particles` Spawn Sparks · `game.shake` Camera Shake (this machine only) · `game.sound` Play
Sound (3D at the location or at Self, or flat) · `flow.for_each_tag` For Each Actor with Tag

### Robots

`walker.move` Walk (like the gamepad: forward up to 0.4 m/s, sideways 0.3, turning 57 °/s – the
trained ranges; overrides the robot's own behavior) · `walker.look` Look (head: yaw up to 80°, pitch
up to 63°) · `walker.autonomous` Set Autonomous · `walker.skill` Robot Move (`sit`, `pick`,
`kick_left`, `kick_right`, `roulade`, `jump`) · `walker.beak` · `walker.pet` · `walker.hear` ·
`walker.holding` · `walker.quack` Chirp · `walker.tof` Robot ToF · `walker.sees` Robot Camera Sees ·
`walker.state` Robot State. Details in [Chapter 13](13-plumo.md).

---

## 9.7 Logic in the GameWorld

`GameWorld::start_logic` (`src/game/logic_world.cpp`) only creates the runtime if

- the mode is Play or Simulate and the world is **not a replica** (logic only runs on the
  authority), and
- the level or an actor has a non-empty graph.

Then the level graph (Self 0, name "Level") and every actor graph (Self = actor ID, name = actor
name) are added; `start` then calls `begin`. Spawned actors and new pawns bring their graphs along.

The world's **host** (`GameWorld::LogicHost`):

| Method | Implementation |
|---|---|
| `time()` | `sim_time_` – the simulation time in seconds, computed from the step counter, not the wall clock |
| `print(text, screen)` | "Logic: …" into the messages, with `screen` also as a HUD message |
| `error(text)` | "Logic error: …" into the messages (Studio shows them in the Output Log) |
| `random()` | Xorshift32, seeded from the number of actors – deterministic |
| `game()` | the GameWorld (for the game nodes) |

**Events to graphs** (`logic_fire`): the event goes to the graph of the affected actor or, if it has
none, to that of its nearest ancestor with a graph (the welded-in collider of a door belongs to the
door), and always to the level graph as well. Events without an affected actor (game over) go to
all. The logic tick runs at the end of every fixed step, after the physics events
([Chapter 8](08-game-runtime.md), Section 8.9).

If a level has logic, the GameWorld enables **sensor events on all movable shapes and hit events on
all dynamic shapes** – otherwise there would be no overlaps and hits for the graphs.

**Replication.** Whatever logic changes in the world is recorded: `set_property` (path and value),
`spawn_copy` (template, ID, pose) and moving static bodies (`teleport` writes
`transform.position`/`rotation`). The server sends these changes to all clients, including players
who join later ([Chapter 10](10-networking.md)).

---

## 9.8 Codegen: from graph to machine code

`src/logic/codegen.cpp` has two back ends with **one shared plan**:

- `compile_wasm(instance)` writes a WebAssembly module. The host translates it with the browser's
  optimizing compiler and hooks it into the engine's function table. The graph then runs as native
  code and calls the engine's C++ node functions directly (wasm-to-wasm calls, no script engine in
  between).
- `generate_cpp(instance, name)` writes the same program as C++ source. Compiled into a game
  (`kern/build.sh --studio out.wasm datei.cpp`), it replaces the interpreter for this graph.

### What is generated inline

Control flow (Branch, Sequence, For, While, Do Once, Do N, Gate, Flip Flop), numeric variables (Get,
Set, Add), math (`add`, `sub`, `mul`, `div`, `mod`, `min`, `max`, `clamp`, `abs`, `negate`, `lerp`,
`sqrt`, `floor`, `round`), comparisons, boolean logic, number/integer/bool literals and a numeric
`Select`. **Everything else** – game nodes, text, vectors, latent nodes – is a call to the node's C++
function (`call_node`), after which execution continues at the chosen exec output (from a jump
table).

### Shared memory

The shared state lives in the instance, at fixed addresses that never change after creation
(`CodeLayout`):

| Area | Content |
|---|---|
| `slots()` | f64 slots: first the numeric variables (bool, int, float), then **8 per node** – 4 for state (loop index, gate open, flip-flop …) and 4 for data outputs of inline-generated exec nodes |
| `stack()` | i32 continuation stack: `[0]` = depth, then pairs (pc, entry); 512 entries |
| `exec_table()` | per node and exec output: (target, input) |
| `counters()` | `[0]` nodes in this step, `[1]` limit (the budget), `[2]` tracing on |

Non-numeric variables (vector, text, actor) live in a map in the instance and are read via node
functions.

### The program

The generated program is a **loop over a program counter** – one case per node. This keeps every exec
wiring valid, including merges and cycles. Continuations of Sequence and loops go onto the
instance's stack, exactly as in the interpreter. Every iteration increments `counters[0]`; above the
limit the function returns 1 (infinite loop), with a full stack 2.

The WebAssembly module:

- **Types:** `run(pc, entry) → i32`, `call_node(id, node, entry) → i32`,
  `read_pin(id, node, pin) → f64`, `trace(id, node)`.
- **Imports** from `env`: `call_node`, `read_pin`, `trace` and the **engine's memory** – slots, stack
  and table are addressed via their absolute addresses.
- **Export:** `run`.
- **Dispatch:** a `br_table` over the program counter to one block per node.

The exported C++ code has the same shape (`switch (pc)` in an endless loop) and registers itself at
the end via the content hash of the graph:

```cpp
// Generated by Kern Studio from the logic graph "…".
int kern_logic_<name>(void* self, int pc, int entry) {
  Instance& g = *static_cast<Instance*>(self);
  f64* s = g.slots(); i32* st = g.stack(); i32* et = g.exec_table(); i32* ct = g.counters();
  ...
  for (;;) {
    if (pc < 0) { /* next continuation from the stack, or done */ }
    if (++ct[0] > ct[1]) return 1;
    switch (pc) {
      case 0: {  // Begin Play
        ...
        continue;
      }
      ...
      default: return 0;
    }
  }
}
const bool kRegistered = (register_native_graph(0x…ull, kern_logic_<name>), true);
```

`graph_hash` is an FNV-1a over the graph JSON **without positions and comments** – moving a node
therefore does not invalidate the exported code, but any change in content does. After editing, you
must export again.

### The sequence in the browser

1. `Runtime::add` checks the graph. If native code is registered for its hash, that wins.
2. Otherwise it calls `host.jit_compile(compile_wasm(instance), id)`. The default implementation of
   `Host::jit_compile`, which the GameWorld's host also uses, forwards this to
   `platform_jit_compile`, which in WebAssembly calls the host function `kern_env.logic_jit`
   (`src/logic/logic_api.cpp`). Natively there is no compiler: the graph stays in the interpreter
   (or runs as exported C++).
3. `web/kern.mjs` translates the module asynchronously (`WebAssembly.compile`, `instantiate` with the
   engine exports `kern_logic_call_node`, `kern_logic_read_pin`, `kern_logic_trace` and the engine
   memory), places the `run` function into a free slot of the engine's function table and reports
   `kern_logic_jit_ready(id, slot)`. If anything fails, a warning is issued and −1 is reported – the
   graph keeps running in the interpreter.
4. Until then the instance is `"compiling"` and runs in the **interpreter**. From the report onward,
   `run_from` calls the table function directly: an indirect call, no JavaScript in between.
5. When the instance is destroyed, `logic_jit_release` frees the table slot.

According to `docs/studio.md`, the JIT is about 55 times faster than the interpreter; the status line
shows "compiled". The figure is measured by `tests/logic_wasm_test.mjs` with one million loop
iterations.

---

## 9.9 Why the JIT must match the interpreter

The three tiers are **interchangeable**, even in the middle of a game:

- The JIT works asynchronously. A graph starts in the interpreter – Begin Play, the first ticks, Do
  Once, running loop counters – and is taken over as soon as the module is ready. That is why the
  state (variables, loop counters, gates, flip-flops, data outputs of inline-generated nodes) does
  not live in interpreter structures but in **the same slots** that the generated code also reads
  and writes. The interpreter's node functions for these nodes likewise compute via `Context::slot`
  (for instance `flow.for`: "State in slots (index, last): the same in the interpreter and in
  generated code"). A tier switch therefore needs no copying (`enter_compiled` is empty).
- Latent nodes are managed in the interpreter part of the runtime and continue their chain – no
  matter in which tier – with the same semantics.
- Kern is deterministic ([Chapter 6](06-physics.md), Section 6.14). A graph that computed differently
  when compiled in the browser than natively in the interpreter would devalue replays, training and
  debugging. The budget against infinite loops also counts identically in both tiers (node
  executions per step) and disables the same graphs.
- The editor debugs with the same semantics: breakpoints and the live display of running nodes also
  work in compiled code (`trace`).

This is checked in all three tiers with the same cases from `tests/logic_cases.h` (`math_loops`,
`gates_and_sequences`, `while_nested`, `mixed_cpp_nodes`, `state_machine`, `infinite_loop`,
`reroutes`):

| Test | Tier | Checks |
|---|---|---|
| `logic_tier_cases_interpreter` (`tests/test_main.cpp`) | interpreter | expected values of each case (e.g. `Sum == 315`, `Log == 111111`, infinite loop disables); the WebAssembly module is well-formed (magic, version, sections), the C++ export contains `register_native_graph` and `switch (pc)` |
| `tests/logic_wasm_test.mjs` (with `logic_wasm_test.cpp`) | JIT in the engine WebAssembly under Node | each case once in the interpreter, once JIT-compiled (translated by V8 and linked into the function table as in the browser); the tier must be `compiled (WebAssembly)` and the result **identical**; plus the speed measurement |
| `tests/logic_native_test.cpp` | exported C++ | `kern_tests --emit-logic` writes the generated sources and the interpreter results; `run.sh` compiles the sources into this program; every graph must run as `native C++` and deliver exactly the interpreter results |

Anyone who generates a node inline or changes its semantics therefore changes it in three places in
the same way (node function, `WasmGen::node_code`, `CppGen::node_code`) and adds a case to
`logic_cases.h` if needed.

---

## 9.10 Editing in Studio

The Blueprint editor (`web/studio/logic.mjs`) follows Unreal's Blueprint editor; its user interface
is described in [Chapter 11](11-studio.md). The page contains no editor logic: every edit is a
command to the engine (`src/studio/logic_edit.cpp`), **every edit is a level edit and therefore an
undo step**; the page draws what `logic_json()` describes (graph, nodes with pins, problems, while
playing the nodes that ran, live values of variables, states, tier).

| Command | Effect |
|---|---|
| `logicOpen {owner}`, `logicClose`, `logicView {view}` | open or close the graph of an actor (or of the level), view |
| `logicPalette {query, from}` | action menu: search over title, category, help, search words; when dragging from a pin, only nodes that accept the wire; plus "Get/Set/Add to <Variable>", "Call/Event <Name>", "Go to <State>", "On <State> Entered", "Add Reroute Node"; ordered by category, at most 120 |
| `logicAdd {type, x, y, params, from}` | place a node on the 8 px grid; without a variable/state machine one is created; when dropped from a pin it is connected right away |
| `logicConnect`, `logicDisconnect`, `logicDelete`, `logicMove` | wires, deleting, moving (comments including size, states) |
| `logicValue`, `logicParam`, `logicComment` | literal, setting (wires to pins that no longer exist are removed), node comment |
| `logicVariable {op}` | `add`, `remove`, `rename` (also in nodes and transitions), `type` (value is converted), `value` |
| `logicMachine {op}` | create, rename and remove state machines and states, initial state, transitions (`addTransition`, `editTransition`, `removeTransition`; a condition without a variable creates a boolean "Ready") |
| `logicCommentBox {op}` | comment boxes |
| `logicPromote` | "Promote to Variable": a new variable of the pin's type, connected via Get (input) or Set (output) |
| `logicInsertReroute` | double-click on a wire: reroute node |
| `logicCopy`, `logicPaste` | as graph JSON |
| `logicCompile` | `check` results |
| `logicExportCpp` | produces `generate_cpp` (file `logic_<level>_<actor>.cpp`) |
| `logicBreakpoint {node, on}` | F9; if the node runs while playing, the game pauses after this simulation step and the editor jumps to it |
| `logicTrace` | nodes that ran, variables, states, tier |
| `logicTemplate {template}` | insert a template below the existing nodes |
| `logicRemoveGraph` | remove the actor's graph, or clear the level graph |

Several commands with the same `transaction` become one undo step (picking up wires and dropping them
elsewhere), as do consecutive value entries into the same pin. Tests:
`studio_logic_editor_commands`, `studio_blueprint_breakpoints_pause_the_game`,
`studio_blueprint_comments_reroutes_promote`.

---

## 9.11 Examples

All examples are real graphs from the code.

### Score zone (template `score_zone`)

Whoever enters the sensor gets 10 points, a message and a sound – the graph from 9.1:
On Begin Overlap → Add Score (10) → Show Message ("+10") → Play Sound (`builtin:pickup`).

### Door (template `door`)

```
On Interact ─▶ Flip Flop ─A▶ Tween (0.8 s, smooth) ─Update▶ Move By (0, 0, 0.05)
                         └B▶ Tween (0.8 s, smooth) ─Update▶ Move By (0, 0, −0.05)
            └─▶ Play Sound (builtin:confirm)
```

Interact alternates between opening and closing; each tween runs for 0.8 s and pushes the door a
little every step.

### Spawner (template `spawner`)

Every 3 s → Branch (Spawned < 10) → Spawn Actor (template "Physics Cube" at (0, 0, 6)) → Add to
Spawned. Spawned copies are replicated to all players.

### Traffic light (template `traffic_light`)

A state machine "Light": Green → (4 s) → Yellow → (1.5 s) → Red → (4 s) → Green, all transitions with
a timer. "On State Entered (any)" shows `Current State` as a message.

### Avoiding obstacles (template `robot_avoid`)

Begin Play turns off the robot's autonomy. Every 0.25 s: if the ToF sees something closer than
1.6 m, it turns on the spot (45 °/s for 0.6 s), otherwise it walks straight ahead (0.3 m/s).

### Plumo chirps at the ball

The level graph of Plumo sample level (`plumo_level` in `demos.cpp`):

```
Every 0.25 s ─▶ Branch( Robot Camera Sees(Ball).visible AND NOT Ball Seen )
                  ├True▶  Chirp ─▶ Set Ball Seen = visible
                  └False▶ Set Ball Seen = visible
```

The boolean variable "Ball Seen" ensures that the duck chirps only once when the ball comes into
view – not every 0.25 s for as long as it is visible. A comment box in the graph states explicitly
that this is a demo of the robot nodes and not something the robot does on its own.

---

© 2026 Christian Heinrich Hohlfeld · All rights reserved · [Imprint and rights](00-imprint.md)
