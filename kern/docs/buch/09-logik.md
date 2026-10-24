# 9 Logik-Graphen

Kern hat **keine Skriptsprache**. Spiellogik sind Graphen im Stil von Unreals Blueprints: Ein Graph
ist reines Datum (Knoten, Drähte, Literale, Variablen, State Machines), und jeder Knotentyp ist eine
C++-Funktion in einer Registry. Graphen werden geprüft, in eine Indextabelle übersetzt und von einem
kleinen Interpreter in den festen Simulationsschritten der Engine ausgeführt – nur auf der Autorität
(dem Einzelspiel oder dem Netzwerk-Server). Ihre Wirkungen werden wie alles andere, was die
Simulation tut, repliziert.

Damit Logik nicht langsam ist, übersetzt Kern jeden Graphen beim Start von Play oder Simulate zur
Laufzeit nach **WebAssembly** (JIT); der Browser macht daraus Maschinencode. Und wer will, exportiert
einen Graphen als **lesbares C++** und kompiliert ihn ins Spiel. Alle drei Stufen müssen exakt
dasselbe Ergebnis liefern – warum und wie das gesichert ist, steht in Abschnitt 9.9.

```
include/kern/logic/logic.h     Datenmodell, Library, check, Runtime, Instance, Context, Codegen-API
src/logic/logic.cpp            Werte, Graph-JSON, Library, check, Interpreter
src/logic/core_nodes.cpp       Kern-Knoten (Events, Flow, Variablen, State Machines, Mathe, Text …)
src/logic/codegen.cpp          compile_wasm (JIT) und generate_cpp (Export)
src/logic/logic_api.cpp        C-ABI zwischen Engine und JIT-Modulen
src/game/logic_nodes.cpp       Spiel-Knoten (Actors, Physik, Kampf, Roboter …)
src/game/logic_world.cpp       Graphen in der GameWorld: Host, Events, Replikation
src/studio/logic_edit.cpp      Bearbeitung im Studio
web/studio/logic.mjs           Blueprint-Editor (Oberfläche)
web/kern.mjs                   JIT-Anbindung im Browser
tests/logic_cases.h            Fälle, die in jeder Stufe gleich laufen müssen
```

---

## 9.1 Das Modell

Aus dem Kopfkommentar von `logic.h` (Unreal-Blueprint-Semantik):

- **Exec-Pins** (weiße Pfeile): was als Nächstes läuft. Event-Knoten beginnen eine Kette,
  Aktionsknoten setzen sie fort, Flow-Knoten verzweigen, schleifen oder warten.
- **Daten-Pins**: typisierte Werte. Eingänge **ziehen**: Pure-Knoten (Mathe, Getter) werden bei
  Bedarf ausgewertet; Ausgänge von Aktionsknoten behalten den Wert, den sie beim letzten Lauf
  hatten. Unverbundene Eingänge nutzen das im Knoten eingetragene Literal.
- **Variablen**: pro Graph-Instanz, typisiert, mit Standardwert.
- **State Machines**: Zustände und Übergänge im Stil des Zustandseditors von
  digitalisierungsplanung.de. Ein Übergang feuert auf ein Event, einen Timer, eine Bedingungsvariable
  oder automatisch; Betreten und Verlassen eines Zustands lösen Events aus.
- **Latente Knoten**: Delay, Tween und Retriggerable Delay unterbrechen die Kette und setzen sie im
  Tick fort, wie latente Blueprint-Aktionen.

Ein Level hat einen **Level-Graphen** (Self = keiner); jeder Actor kann einen eigenen Graphen tragen
(sein Self).

### Datenstrukturen

| Struktur | Felder |
|---|---|
| `Graph` | `nodes`, `links`, `variables`, `machines`, `comments`, `next_id` |
| `Node` | `id`, `type` (Registry-Schlüssel, z. B. `flow.branch`), `x`, `y` (Canvas-Pixel), `values` (Literale unverbundener Eingänge, je Pin-ID), `params` (Einstellungen: Variablenname, Eventname, Taste …), `comment` |
| `Link` | `from` (Knoten), `out` (Ausgangs-Pin), `to`, `in` |
| `Variable` | `name`, `type`, `value` (Standard), `help` |
| `StateMachine` | `id`, `name`, `states`, `transitions` |
| `State` | `id`, `name`, `x`, `y`, `initial` |
| `Transition` | `id`, `from` (0 = aus jedem Zustand), `to`, `trigger` (`event`, `timer`, `condition`, `automatic`), `event`, `seconds` (Standard 1), `variable`, `label` |
| `CommentBox` | `id`, `x`, `y`, `w` (400), `h` (200), `text`, `color` (0 = grau) – nur für den Editor, ohne Wirkung |

### Pin-Typen und Werte

`PinType` ist `exec`, `boolean`, `integer`, `number`, `vector`, `text`, `actor` (im JSON `exec`,
`bool`, `int`, `float`, `vector`, `text`, `actor`; beim Lesen auch `number`, `double`, `boolean`,
`integer`, `string`). Ein `Value` trägt einen Typ und den passenden Inhalt (`b`, `i`, `n`, `v`, `s`,
`a`; `ActorRef` ist die Actor-ID).

Welche Drähte erlaubt sind, sagt `convertible(from, to)`:

- gleicher Typ immer; Exec nur mit Exec;
- **alles nach `text`** („everything prints“);
- Zahlen untereinander (`bool`, `int`, `float`);
- `actor` → `bool` („ist gültig“);
- `float`/`int` → `vector` (in alle drei Komponenten).

Die Umrechnungen (`as_bool`, `as_int`, `as_number`, `as_vector`, `as_text`, `as_actor`) sind
festgelegt, z. B. Text „0“ oder „false“ ist falsch, ein Vektor als Zahl ist seine Länge, eine Zahl
nach `int` wird abgeschnitten, Text wird als Zahl geparst.

### Das JSON-Format

Graphen stehen im Level (`logic` im Level und in jedem Actor, [Kapitel 8](08-spiel-laufzeit.md)):

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

Drähte sind Arrays `[from, out, to, in]`. Übergänge schreiben je nach Trigger `event`, `seconds`
oder `variable`; `comments` erscheinen nur, wenn es welche gibt. Positionen werden auf ganze Pixel
gerundet.

---

## 9.2 Knotendefinitionen und die Library

Jeder Knotentyp ist eine `NodeDef`:

| Feld | Bedeutung |
|---|---|
| `type`, `label`, `category`, `help`, `keywords` | Schlüssel, Titel, Kategorie, Hilfe, Suchwörter |
| `kind` | `event`, `action`, `pure`, `flow` |
| `event` | Events: der `Event::type`, den der Knoten behandelt (z. B. `logic.tick`, `game.overlap_begin`) |
| `latent` | wartet (Delay, Tween) |
| `inputs`, `outputs` | Pins in Zeichenreihenfolge, Exec-Pins eingeschlossen (`PinDef`: `id`, `label`, `type`, `def`, `hidden`) |
| `params` | Einstellungen (`ParamDef`: `key`, `label`, `kind` – `text`, `choice`, `variable`, `event`, `machine`, `state`, `key`, `sound`, `tag`, `integer` –, `choices`, `def`) |
| `color` | Kopffarbe (0 = nach Art) |
| `shape` | Pins, die von Parametern oder dem Graphen abhängen (Variablen, Sequence-Ausgänge, Switch-Fälle) |
| `title` | Titel, der von Parametern abhängt („Every 3 s“, „On Key F“) |
| `accepts` | Events: Behandelt dieser Knoten das Event? Standard: `key` leer, oder gleich dem ersten Parameter, oder `any` |
| `run` | die C++-Funktion |

`run` liefert den **Index des Exec-Ausgangs**, mit dem es weitergeht (unter den Exec-Ausgängen),
oder `kStop` (−1, die Kette endet) bzw. `kSuspend` (−2, latent: wird später aus dem Tick
fortgesetzt). Pure-Knoten schreiben ihre Ausgänge und liefern `kStop`.

Eine Knotenfunktion sieht die Welt über `Context`:

| Methode | Zweck |
|---|---|
| `in(index)`, `in(pin)`, `in_bool`, `in_number`, `in_int`, `in_vector`, `in_text`, `in_actor` | Eingänge (zieht verbundene Pure-Knoten) |
| `out(pin, value)` | Ausgang setzen |
| `param(key, fallback)` | Einstellung |
| `exec(pin)` | Ordinalzahl eines Exec-Ausgangs (für den Rückgabewert) |
| `entry()` | welcher Exec-Eingang den Knoten startete (0 = der erste; `kResume` = 1000 nach einer Wartezeit) |
| `state()`, `slot(k)` | Zustand der Knoteninstanz; `slot(0..3)` liegt im gemeinsamen Speicher mit generiertem Code |
| `wait(seconds, entry)` | latent: nach `seconds` mit `entry` fortsetzen, liefert `kSuspend` |
| `cancel_waits()` | eigene Wartezeiten verwerfen (Retriggerable Delay) |
| `then(out)` | nach Ende der aktuellen Kette an diesem Ausgang weitermachen (Sequence, Schleifen) |
| `self()`, `host()`, `runtime()`, `instance()` | Kontext |

`Library::core()` enthält die eingebauten Knoten; `logic::library()` ist core plus das, was das Spiel
registriert hat. Die Spiel-Knoten registrieren sich beim Start des Programms selbst
(`logic_nodes.cpp`; `game::logic_nodes_registered()` sorgt dafür, dass diese Datei gelinkt wird).

Ein minimales Beispiel aus `logic_nodes.cpp`, eine Aktion:

```cpp
lib.add(action("game.add_score", "Add Score", "Game",
               {data("points", "Points", PinType::number, 10.0)}, {},
               [](Context& c, GameWorld& w) { w.add_score(static_cast<f32>(c.in_number("points"))); },
               "Adds points (wins at the target score when the goal is Score).", "points reward"));
```

`action(...)` baut daraus eine `NodeDef` mit einem Exec-Eingang und einem Exec-Ausgang `then`, deren
`run` die Funktion mit der GameWorld aufruft und 0 liefert.

---

## 9.3 Prüfen

`check(graph, lib)` liefert `Issue`s (`node`, `error`, `text`). Fehler:

- unbekannter Knotentyp;
- Parameter, die auf fehlende Variablen, State Machines oder Zustände zeigen; ein Event ohne Namen;
- ein Draht zu einem Pin, den es nicht gibt;
- ein Exec-Draht an einem Daten-Pin, nicht umwandelbare Typen;
- ein Dateneingang mit mehr als einem Draht;
- Pure-Knoten, die von sich selbst abhängen (Zyklus);
- eine State Machine ohne genau einen Anfangszustand; ein Event-Übergang ohne Eventnamen; ein
  Bedingungsübergang auf eine fehlende Variable.

Warnung: ein Knoten mit Exec-Eingang, an dem nichts hängt („never runs“).

Ein Graph mit Fehlern wird beim Start **abgeschaltet** und gemeldet; der Rest des Spiels läuft
weiter. Im Studio zeigt Compile (F7) die Ergebnisse.

---

## 9.4 Ausführung

### Runtime und Instanzen

Eine `Runtime` führt alle Graphen einer Welt aus; jeder Graph läuft als `Instance` für einen
Besitzer (Actor oder Level).

| Methode | Wirkung |
|---|---|
| `add(graph, self, name)` | übersetzt und fügt hinzu; Prüffehler schalten die Instanz ab; danach nativer C++-Code (falls registriert) oder JIT-Auftrag |
| `remove(self)`, `clear()` | entfernt (samt wartender Latenter) |
| `begin()` | Begin Play für Instanzen, die noch nicht begonnen haben: Anfangszustände betreten, `event.begin` ausführen |
| `tick(dt)` | ein fester Schritt: fällige latente Aktionen (nach Zeit sortiert), dann das Tick-Event (`logic.tick` mit `delta`), dann Timer- und Bedingungsübergänge der State Machines |
| `fire(event)` | löst ein Event synchron aus |
| `call(name, target, args)` | Custom Event auf allen Instanzen (oder der des Ziels) und Event-Übergänge mit diesem Namen |
| `find(self)`, `instances()` | Zugriff |
| `trace()`, `tracing` | welche Knoten liefen (für den Editor) |
| `budget` | 200 000 Knotenausführungen pro Schritt |
| `jit` | JIT einschalten (Standard: an, wo der Host kann) |

Eine `Instance` bietet `get`/`set` von Variablen, `state`/`set_state` von State Machines,
`disabled()`, `compiled()` und `tier()` – `"interpreter"`, `"compiling"`,
`"compiled (WebAssembly)"` oder `"native C++"`.

### Events

```cpp
struct Event {
  std::string type;          // z. B. "game.overlap_begin"
  ActorRef target = 0;       // nur Instanzen mit diesem Self (0 = alle)
  bool level = false;        // mit target: der Level-Graph bekommt es auch
  std::string key;           // eventspezifisch: Custom-Event-Name, Tastencode, Aktion, "Machine/State"
  std::vector<std::pair<std::string, Value>> args;
};
```

`fire` geht über alle nicht abgeschalteten Instanzen (die zum Ziel passen), sucht Event-Knoten mit
passendem `event` und `accepts`, schreibt die Argumente in die gleichnamigen Ausgänge und führt die
Kette aus. Instanzen, die währenddessen dazukommen (gespawnte Actors), werden mitgenommen.

### Eine Kette laufen lassen

Der Interpreter (`Runtime::run_from`) arbeitet mit einem Stapel aus (Knoten, Eingang):

1. Knoten ausführen; was er per `then` vormerkt, kommt auf den Stapel (zuletzt vorgemerkt zuerst).
2. Liefert er einen gültigen Exec-Ausgang, geht es am verbundenen Knoten weiter; sonst endet dieser
   Zweig, und der nächste Stapeleintrag läuft.

So arbeiten Sequence (Ausgang 0 sofort, die übrigen vorgemerkt) und Schleifen (Körper ausführen,
danach den Knoten selbst mit `kResume` erneut betreten).

Daten werden **gezogen**: Ein Eingang, der mit einem Pure-Knoten verbunden ist, führt dessen `run`
bei jedem Lesen aus (Tiefe höchstens 48). Ein Eingang an einem Aktionsknoten liest dessen zuletzt
gespeicherten Ausgang.

### Schutz vor Endlosschleifen

- Mehr als `budget` (200 000) Knoten in einem Schritt: Die Instanz wird abgeschaltet („infinite
  loop? The graph is stopped.“).
- Events, die Events auslösen, dürfen höchstens 64 tief verschachteln; darüber wird die Kette
  gestoppt.
- Im kompilierten Code: mehr als 512 verschachtelte Schleifen/Sequences stoppen die Instanz.

### State Machines

- `begin` betritt den Anfangszustand.
- Betreten und Verlassen feuern `logic.state_enter` / `logic.state_exit` mit dem Schlüssel
  `"Maschine/Zustand"`; „On State Entered“ bekommt den vorigen Zustand.
- Übergänge prüft `check_transitions` – **höchstens ein Übergang pro Maschine und Aufruf**
  (automatische Ketten gehen einen Zustand pro Schritt weiter):
  - `event`: bei `call` mit diesem Namen,
  - `timer`: nach `seconds` im Zustand,
  - `condition`: solange die boolesche Variable wahr ist,
  - `automatic`: sofort beim nächsten Tick.
- `from = 0` heißt „aus jedem Zustand“ (aber nicht in den Zielzustand selbst zurück).
- `Set State` (`state.set`) wechselt direkt.

---

## 9.5 Kern-Knoten

`src/logic/core_nodes.cpp` registriert 74 Knoten:

| Kategorie | Knoten |
|---|---|
| **Events** | `event.begin` Begin Play · `event.tick` Event Tick (Delta Seconds) · `event.every` Every N Seconds (Intervall-Parameter, zählt eigene Zeit, Ausgang Count) · `event.custom` Custom Event (Name; Ausgänge Actor, Value, Text; schaltet auch gleichnamige State-Machine-Übergänge) |
| **State Machines** | `event.state_enter` On State Entered · `event.state_exit` On State Left · `state.set` Set State · `state.get` Current State · `state.is` Is In State |
| **Flow** | `flow.reroute` / `value.reroute` (Knoten für Drähte) · `flow.branch` Branch · `flow.sequence` Sequence (1..16 Ausgänge) · `flow.for` For Loop (First, Last inklusive; Index) · `flow.while` While Loop · `flow.do_once` Do Once (Reset, Start Closed) · `flow.do_n` Do N (N, Reset, Counter) · `flow.gate` Gate (Enter, Open, Close, Toggle, Start Closed) · `flow.flip_flop` Flip Flop (A/B, Is A) · `flow.delay` Delay (Aufrufe während des Wartens werden ignoriert) · `flow.retrigger_delay` Retriggerable Delay · `flow.tween` Tween (Play, Stop, Duration; Update jeden Schritt mit Alpha 0→1, Easing linear/smooth/in/out, dann Finished) · `flow.switch_int` Switch on Int · `flow.switch_text` Switch on Text (Fälle durch Kommas getrennt) · `flow.call` Call Event (Target, Actor, Value, Text) |
| **Variablen** | `var.get` · `var.set` · `var.add` (Amount, −1 zählt herunter) |
| **Mathe** | `math.add`, `sub`, `mul`, `div`, `mod`, `min`, `max`, `clamp`, `abs`, `negate`, `lerp`, `sin`/`cos`/`atan2` (Grad), `sqrt`, `pow`, `round`, `floor`, `random` (Float im Bereich), `random_int`, `random_bool` |
| **Logik** | `math.compare` Compare (`==`, `!=`, `<`, `<=`, `>`, `>=`; Gleichheit mit 10⁻⁹ Toleranz) · `logic.and`, `or`, `xor`, `not` · `logic.select` Select |
| **Werte** | `value.float`, `value.int`, `value.bool`, `value.text` |
| **Vektor** | `vector.make`, `break`, `scale`, `length`, `normalize`, `distance`, `dot`, `lerp` |
| **Text** | `text.append`, `equal`, `contains`, `length` |
| **Utilities** | `time.now` Game Time · `debug.print` Print (Text, On Screen) |

Zufallszahlen kommen vom Host (`Host::random()`) und sind **deterministisch pro Welt** – Replays und
das Netz stimmen überein.

---

## 9.6 Spiel-Knoten

`src/game/logic_nodes.cpp` registriert 71 Knoten. Sie rufen Funktionen der GameWorld
([Kapitel 8](08-spiel-laufzeit.md)); ein Actor-Eingang ohne Draht (0) bedeutet **Self**.

### Events

| Knoten | Event | Ausgänge | Ausgelöst von |
|---|---|---|---|
| On Begin Overlap | `game.overlap_begin` | Actor, Other Actor | etwas betritt den Sensor-Collider dieses Actors |
| On End Overlap | `game.overlap_end` | Actor, Other Actor | etwas verlässt ihn |
| On Entered Sensor | `game.overlapped_begin` | Actor, Sensor Actor | dieser Actor (z. B. ein Spieler) betritt einen fremden Sensor |
| On Hit | `game.hit` | Actor, Other, Impact Speed, Point, Normal | Box3D-Hit ab 2 m/s, beide Seiten, höchstens 32 pro Schritt |
| On Collected | `game.pickup` | Actor, Collector, Points | ein Pickup wurde eingesammelt |
| On Trigger | `game.trigger` | Actor, Player | ein Spieler berührte diesen Trigger |
| On Damaged | `game.damaged` | Actor, Attacker, Damage, Headshot | Schaden (braucht Health) |
| On Death | `game.death` | Actor, Killer, Headshot | Gesundheit 0 |
| On Respawn | `game.respawn` | Actor | Rückkehr nach dem Tod |
| On Interact | `game.interact` | Actor, Player, Point | Interact (F / Y am Gamepad) mit Blick auf den Actor (Strahl 3 m) oder daneben (nächster Actor mit Graph innerhalb 2 m) |
| On Player Joined | `game.pawn_added` | Player | ein Spieler kam dazu |
| On Game Over | `game.over` | Won, Message | Spiel gewonnen oder verloren (an alle Graphen) |
| On Input Action | `game.input` | Player, Pressed | Aktion `jump`, `fire`, `aim`, `reload`, `crouch`, `interact`, `boost`; Flanke pressed/released/both – für jeden Spieler und jedes Gerät |
| On Key | `game.key` | Pressed, Player | eine Taste dieser Tastatur (nur der lokale Spieler; Parameter „F“ → `KeyF`, „1“ → `Digit1`) |

Überlappungen zählen den Besucher als ganzen Actor: Eine Überlappung beginnt mit der ersten Form im
Sensor und endet mit der letzten, die ihn verlässt (Kapsel und Kopf eines Spielers sind ein
Besucher).

### Actors

`actor.self` Self · `actor.player` Get Player (0 = Spieler dieser Maschine, 1, 2 … weitere) ·
`actor.player_count` · `actor.find` Find Actor by Name · `actor.find_tag` Find Actor with Tag ·
`actor.random_tag` · `actor.has_tag` · `actor.name` · `actor.valid` · `actor.location` ·
`actor.rotation` · `actor.forward` · `actor.velocity` · `actor.distance` · `actor.set_location` ·
`actor.set_rotation` (Grad) · `actor.move_by` · `actor.spawn` Spawn Actor (Kopie einer Vorlage mit
Kindern und Logik, an alle Spieler repliziert) · `actor.destroy` · `actor.enable` Set Enabled ·
`actor.set_property` Set Property (jeder Komponentenwert über seinen Pfad wie in der Leveldatei,
z. B. `components.mesh.material.color` – repliziert)

### Physik

`actor.set_velocity` · `actor.impulse` Add Impulse · `actor.force` Add Force (für einen Schritt) ·
`actor.launch` Launch (behält die horizontale Geschwindigkeit, addiert diese) · `actor.simulate` Set
Simulate Physics (dynamisch ↔ kinematisch) · `actor.joint_motor` Set Joint Motor (Scharnier, Slider,
Seil: Türen, Lifte, Winden) · `actor.explode` Explosion (Box3D-Radialimpuls mit Blitz, Sound und
fallendem Schaden) · `physics.trace` Line Trace (Box3D-Strahl)

### Spiel, Kampf, Effekte, Audio

`game.score` Get Score · `game.message` Show Message (alle Spieler sehen sie) · `game.add_score` ·
`game.win` · `game.lose` · `combat.damage` Apply Damage · `combat.heal` · `combat.kill` ·
`combat.ammo` Give Ammo · `actor.health` Get Health · `actor.set_light` · `actor.set_color` ·
`game.particles` Spawn Sparks · `game.shake` Camera Shake (nur diese Maschine) · `game.sound` Play
Sound (3D am Ort bzw. an Self, oder flach) · `flow.for_each_tag` For Each Actor with Tag

### Roboter

`walker.move` Walk (wie das Gamepad: vorwärts bis 0,4 m/s, seitwärts 0,3, drehen 57 °/s – die
trainierten Bereiche; übersteuert das eigene Verhalten) · `walker.look` Look (Kopf: Gieren bis 80°,
Nicken bis 63°) · `walker.autonomous` Set Autonomous · `walker.skill` Robot Move (`sit`, `pick`,
`kick_left`, `kick_right`, `roulade`, `jump`) · `walker.beak` · `walker.pet` · `walker.hear` ·
`walker.holding` · `walker.quack` Chirp · `walker.tof` Robot ToF · `walker.sees` Robot Camera Sees ·
`walker.state` Robot State. Einzelheiten in [Kapitel 13](13-micro-duck.md).

---

## 9.7 Logik in der GameWorld

`GameWorld::start_logic` (`src/game/logic_world.cpp`) legt die Runtime nur an, wenn

- der Modus Play oder Simulate ist und die Welt **keine Replika** ist (Logik läuft nur auf der
  Autorität), und
- das Level oder ein Actor einen nicht leeren Graphen hat.

Dann wird der Level-Graph (Self 0, Name „Level“) und jeder Actor-Graph (Self = Actor-ID, Name =
Actorname) hinzugefügt; `start` ruft anschließend `begin`. Gespawnte Actors und neue Pawns bringen
ihre Graphen mit.

Der **Host** der Welt (`GameWorld::LogicHost`):

| Methode | Umsetzung |
|---|---|
| `time()` | `sim_time_` – die Simulationszeit in Schritten, nicht die Wanduhr |
| `print(text, screen)` | „Logic: …“ in die Meldungen, mit `screen` auch als HUD-Meldung |
| `error(text)` | „Logic error: …“ in die Meldungen (Studio zeigt sie im Output Log) |
| `random()` | Xorshift32, Startwert aus der Zahl der Actors – deterministisch |
| `game()` | die GameWorld (für die Spiel-Knoten) |

**Ereignisse an Graphen** (`logic_fire`): Das Event geht an den Graphen des betroffenen Actors oder,
falls der keinen hat, an den seines nächsten Vorfahren mit Graph (der eingeschweißte Collider einer
Tür gehört zur Tür), und immer auch an den Level-Graphen. Events ohne betroffenen Actor
(Spielende) gehen an alle. Der Logik-Tick läuft am Ende jedes festen Schritts nach den
Physik-Events ([Kapitel 8](08-spiel-laufzeit.md), Abschnitt 8.9).

Hat ein Level Logik, schaltet die GameWorld **Sensor- und Hit-Events an allen beweglichen Formen**
ein – sonst gäbe es keine Überlappungen und Treffer für die Graphen.

**Replikation.** Was Logik an der Welt ändert, wird mitgeschrieben: `set_property` (Pfad und Wert),
`spawn_copy` (Vorlage, ID, Pose) und das Versetzen statischer Körper (`teleport` schreibt
`transform.position`/`rotation`). Der Server schickt diese Änderungen an alle Clients, auch an
Spieler, die später dazukommen ([Kapitel 10](10-netzwerk.md)).

---

## 9.8 Codegen: vom Graphen zu Maschinencode

`src/logic/codegen.cpp` hat zwei Back-Ends mit **einem gemeinsamen Plan**:

- `compile_wasm(instance)` schreibt ein WebAssembly-Modul. Der Host übersetzt es mit dem
  optimierenden Compiler des Browsers und hängt es in die Funktionstabelle der Engine. Der Graph
  läuft dann als nativer Code und ruft die C++-Knotenfunktionen der Engine direkt auf
  (wasm-zu-wasm-Aufrufe, keine Skript-Engine dazwischen).
- `generate_cpp(instance, name)` schreibt dasselbe Programm als C++-Quelltext. In ein Spiel
  kompiliert (`kern/build.sh --studio out.wasm datei.cpp`), ersetzt er für diesen Graphen den
  Interpreter.

### Was inline erzeugt wird

Kontrollfluss (Branch, Sequence, For, While, Do Once, Do N, Gate, Flip Flop), numerische Variablen
(Get, Set, Add), Mathe (`add`, `sub`, `mul`, `div`, `mod`, `min`, `max`, `clamp`, `abs`, `negate`,
`lerp`, `sqrt`, `floor`, `round`), Vergleiche, boolesche Logik, Zahl-/Integer-/Bool-Literale und ein
numerisches `Select`. **Alles andere** – Spielknoten, Text, Vektoren, latente Knoten – ist ein Aufruf
der C++-Funktion des Knotens (`call_node`), danach geht es am gewählten Exec-Ausgang weiter
(aus einer Sprungtabelle).

### Gemeinsamer Speicher

Der gemeinsame Zustand liegt in der Instanz, an festen Adressen, die sich nach dem Anlegen nie
ändern (`CodeLayout`):

| Bereich | Inhalt |
|---|---|
| `slots()` | f64-Slots: zuerst die numerischen Variablen (bool, int, float), dann **8 pro Knoten** – 4 für Zustand (Schleifenindex, Gate offen, Flip-Flop …) und 4 für Datenausgänge inline erzeugter Exec-Knoten |
| `stack()` | i32-Fortsetzungsstapel: `[0]` = Tiefe, dann Paare (pc, entry); 512 Einträge |
| `exec_table()` | je Knoten und Exec-Ausgang: (Ziel, Eingang) |
| `counters()` | `[0]` Knoten in diesem Schritt, `[1]` Grenze (das Budget), `[2]` Tracing an |

Nicht numerische Variablen (Vektor, Text, Actor) liegen in einer Map der Instanz und werden über
Knotenfunktionen gelesen.

### Das Programm

Das erzeugte Programm ist eine **Schleife über einen Programmzähler** – ein Fall je Knoten. So bleibt
jede Exec-Verdrahtung gültig, auch Zusammenführungen und Zyklen. Fortsetzungen von Sequence und
Schleifen gehen auf den Stapel der Instanz, genau wie im Interpreter. Jeder Durchlauf zählt
`counters[0]` hoch; über der Grenze liefert die Funktion 1 (Endlosschleife), bei vollem Stapel 2.

Das WebAssembly-Modul:

- **Typen:** `run(pc, entry) → i32`, `call_node(id, node, entry) → i32`,
  `read_pin(id, node, pin) → f64`, `trace(id, node)`.
- **Importe** aus `env`: `call_node`, `read_pin`, `trace` und der **Speicher der Engine** – Slots,
  Stapel und Tabelle werden über ihre absoluten Adressen angesprochen.
- **Export:** `run`.
- **Dispatch:** ein `br_table` über den Programmzähler auf einen Block je Knoten.

Der exportierte C++-Code hat dieselbe Gestalt (`switch (pc)` in einer Endlosschleife) und registriert
sich am Ende über den Inhalts-Hash des Graphen:

```cpp
// Generated by Kern Studio from the logic graph "…".
int kern_logic_<name>(void* self, int pc, int entry) {
  Instance& g = *static_cast<Instance*>(self);
  f64* s = g.slots(); i32* st = g.stack(); i32* et = g.exec_table(); i32* ct = g.counters();
  ...
  for (;;) {
    if (pc < 0) { /* nächste Fortsetzung vom Stapel oder fertig */ }
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

`graph_hash` ist ein FNV-1a über das Graph-JSON **ohne Positionen und Kommentare** – einen Knoten zu
verschieben macht den exportierten Code also nicht ungültig, jede inhaltliche Änderung schon. Nach
dem Bearbeiten muss neu exportiert werden.

### Der Ablauf im Browser

1. `Runtime::add` prüft den Graphen. Ist für seinen Hash nativer Code registriert, gewinnt der.
2. Sonst ruft er `host.jit_compile(compile_wasm(instance), id)`. Die GameWorld leitet das an
   `platform_jit_compile` weiter, das in WebAssembly die Host-Funktion `kern_env.logic_jit` aufruft
   (`src/logic/logic_api.cpp`). Nativ gibt es keinen Compiler: Der Graph bleibt im Interpreter
   (oder läuft als exportiertes C++).
3. `web/kern.mjs` übersetzt das Modul asynchron (`WebAssembly.compile`, `instantiate` mit den
   Engine-Exporten `kern_logic_call_node`, `kern_logic_read_pin`, `kern_logic_trace` und dem
   Engine-Speicher), legt die `run`-Funktion in einen freien Platz der Funktionstabelle der Engine
   und meldet `kern_logic_jit_ready(id, slot)`. Schlägt etwas fehl, wird gewarnt und mit −1
   gemeldet – der Graph läuft weiter im Interpreter.
4. Bis dahin ist die Instanz `"compiling"` und läuft im **Interpreter**. Ab der Meldung ruft
   `run_from` die Tabellenfunktion direkt auf: ein indirekter Aufruf, kein JavaScript dazwischen.
5. Beim Zerstören der Instanz gibt `logic_jit_release` den Tabellenplatz frei.

Laut `docs/studio.md` ist der JIT etwa 55-mal schneller als der Interpreter; die Statuszeile zeigt
„compiled“. Den Wert misst `tests/logic_wasm_test.mjs` mit einer Million Schleifendurchläufen.

---

## 9.9 Warum der JIT dem Interpreter gleichen muss

Die drei Stufen sind **austauschbar**, und zwar mitten im Spiel:

- Der JIT arbeitet asynchron. Ein Graph beginnt im Interpreter – Begin Play, die ersten Ticks, Do
  Once, laufende Schleifenzähler – und wird übernommen, sobald das Modul bereit ist. Deshalb liegt
  der Zustand (Variablen, Schleifenzähler, Gates, Flip-Flops, Datenausgänge inline erzeugter
  Knoten) nicht in Strukturen des Interpreters, sondern in **denselben Slots**, die auch der
  generierte Code liest und schreibt. Die Knotenfunktionen des Interpreters für diese Knoten
  rechnen ebenfalls über `Context::slot` (etwa `flow.for`: „State in slots (index, last): the same
  in the interpreter and in generated code“). Ein Stufenwechsel braucht so keine Umkopie
  (`enter_compiled` ist leer).
- Latente Knoten werden im Interpreter-Teil der Runtime verwaltet und setzen ihre Kette – egal in
  welcher Stufe – mit derselben Semantik fort.
- Kern ist deterministisch ([Kapitel 6](06-physik.md), Abschnitt 6.14). Ein Graph, der im Browser
  kompiliert anders rechnete als nativ im Interpreter, würde Replays, das Training und
  Fehlersuche entwerten. Auch das Budget gegen Endlosschleifen zählt in beiden Stufen gleich
  (Knotenausführungen pro Schritt) und schaltet dieselben Graphen ab.
- Der Editor debuggt mit derselben Semantik: Breakpoints und die Live-Anzeige laufender Knoten
  funktionieren auch im kompilierten Code (`trace`).

Geprüft wird das in allen drei Stufen mit denselben Fällen aus `tests/logic_cases.h`
(`math_loops`, `gates_and_sequences`, `while_nested`, `mixed_cpp_nodes`, `state_machine`,
`infinite_loop`, `reroutes`):

| Test | Stufe | Prüft |
|---|---|---|
| `logic_tier_cases_interpreter` (`tests/test_main.cpp`) | Interpreter | erwartete Werte jedes Falls (z. B. `Sum == 315`, `Log == 111111`, Endlosschleife schaltet ab); das WebAssembly-Modul ist wohlgeformt (Magic, Version, Sektionen), der C++-Export enthält `register_native_graph` und `switch (pc)` |
| `tests/logic_wasm_test.mjs` (mit `logic_wasm_test.cpp`) | JIT im Engine-WebAssembly unter Node | jeder Fall einmal im Interpreter, einmal JIT-kompiliert (von V8 übersetzt und in die Funktionstabelle gelinkt wie im Browser); die Stufe muss `compiled (WebAssembly)` sein und das Ergebnis **identisch**; dazu die Geschwindigkeitsmessung |
| `tests/logic_native_test.cpp` | exportiertes C++ | `kern_tests --emit-logic` schreibt die generierten Quellen und die Interpreter-Ergebnisse; `run.sh` kompiliert die Quellen in dieses Programm; jeder Graph muss als `native C++` laufen und genau die Interpreter-Ergebnisse liefern |

Wer einen Knoten inline erzeugt oder seine Semantik ändert, ändert ihn deshalb an drei Stellen
gleich (Knotenfunktion, `WasmGen::node_code`, `CppGen::node_code`) und ergänzt bei Bedarf einen Fall
in `logic_cases.h`.

---

## 9.10 Bearbeitung im Studio

Der Blueprint-Editor (`web/studio/logic.mjs`) folgt Unreals Blueprint-Editor; seine Oberfläche
beschreibt [Kapitel 11](11-studio.md). Die Seite enthält keine Editorlogik: Jede Bearbeitung ist ein
Befehl an die Engine (`src/studio/logic_edit.cpp`), **jede Bearbeitung ist eine Level-Bearbeitung
und damit ein Undo-Schritt**; die Seite zeichnet, was `logic_json()` beschreibt (Graph, Knoten mit
Pins, Probleme, beim Spielen die gelaufenen Knoten, Live-Werte der Variablen, Zustände, Stufe).

| Befehl | Wirkung |
|---|---|
| `logicOpen {owner}`, `logicClose`, `logicView {view}` | Graph eines Actors (oder des Levels) öffnen, schließen, Ansicht |
| `logicPalette {query, from}` | Aktionsmenü: Suche über Titel, Kategorie, Hilfe, Suchwörter; beim Ziehen von einem Pin nur Knoten, die den Draht annehmen; dazu „Get/Set/Add to <Variable>“, „Call/Event <Name>“, „Go to <Zustand>“, „On <Zustand> Entered“, „Add Reroute Node“; nach Kategorien geordnet, höchstens 120 |
| `logicAdd {type, x, y, params, from}` | Knoten auf das 8-px-Raster setzen; ohne Variable/State Machine wird eine angelegt; von einem Pin fallen gelassen wird er gleich verbunden |
| `logicConnect`, `logicDisconnect`, `logicDelete`, `logicMove` | Drähte, Löschen, Verschieben (Kommentare samt Größe, Zustände) |
| `logicValue`, `logicParam`, `logicComment` | Literal, Einstellung (Drähte zu weggefallenen Pins werden entfernt), Knotenkommentar |
| `logicVariable {op}` | `add`, `remove`, `rename` (auch in Knoten und Übergängen), `type` (Wert wird umgewandelt), `value` |
| `logicMachine {op}` | State Machines und Zustände anlegen, umbenennen, entfernen, Anfangszustand, Übergänge (`addTransition`, `editTransition`, `removeTransition`; Bedingung ohne Variable legt eine boolesche „Ready“ an) |
| `logicCommentBox {op}` | Kommentarrahmen |
| `logicPromote` | „Promote to Variable“: neue Variable vom Typ des Pins, über Get (Eingang) oder Set (Ausgang) angeschlossen |
| `logicInsertReroute` | Doppelklick auf einen Draht: Reroute-Knoten |
| `logicCopy`, `logicPaste` | als Graph-JSON |
| `logicCompile` | `check`-Ergebnisse |
| `logicExportCpp` | erzeugt `generate_cpp` (Datei `logic_<level>_<actor>.cpp`) |
| `logicBreakpoint {node, on}` | F9; läuft der Knoten beim Spielen, hält das Spiel nach diesem Simulationsschritt an, und der Editor springt hin |
| `logicTrace` | gelaufene Knoten, Variablen, Zustände, Stufe |
| `logicTemplate {template}` | Vorlage unter den vorhandenen Knoten einfügen |
| `logicRemoveGraph` | Graph des Actors entfernen bzw. Level-Graph leeren |

Mehrere Befehle mit derselben `transaction` werden ein Undo-Schritt (Drähte aufnehmen und
woanders ablegen), aufeinanderfolgende Werteingaben in denselben Pin ebenfalls. Test:
`studio_logic_editor_commands`, `studio_blueprint_breakpoints_pause_the_game`,
`studio_blueprint_comments_reroutes_promote`.

---

## 9.11 Beispiele

Alle Beispiele sind echte Graphen aus dem Code.

### Punktezone (Vorlage `score_zone`)

Wer den Sensor betritt, bekommt 10 Punkte, eine Meldung und einen Ton – der Graph aus 9.1:
On Begin Overlap → Add Score (10) → Show Message („+10“) → Play Sound (`builtin:pickup`).

### Tür (Vorlage `door`)

```
On Interact ─▶ Flip Flop ─A▶ Tween (0,8 s, smooth) ─Update▶ Move By (0, 0, 0,05)
                         └B▶ Tween (0,8 s, smooth) ─Update▶ Move By (0, 0, −0,05)
            └─▶ Play Sound (builtin:confirm)
```

Interact wechselt zwischen Öffnen und Schließen; jeder Tween läuft 0,8 s und schiebt die Tür jeden
Schritt ein Stück.

### Spawner (Vorlage `spawner`)

Every 3 s → Branch (Spawned < 10) → Spawn Actor (Vorlage „Physics Cube“ bei (0, 0, 6)) → Add to
Spawned. Gespawnte Kopien werden an alle Spieler repliziert.

### Ampel (Vorlage `traffic_light`)

Eine State Machine „Light“: Green → (4 s) → Yellow → (1,5 s) → Red → (4 s) → Green, alle Übergänge
mit Timer. „On State Entered (any)“ zeigt `Current State` als Meldung.

### Hindernis ausweichen (Vorlage `robot_avoid`)

Begin Play schaltet die Autonomie des Roboters ab. Alle 0,25 s: Sieht der ToF etwas näher als 1,6 m,
dreht er auf der Stelle (45 °/s für 0,6 s), sonst geht er geradeaus (0,3 m/s).

### Micro Duck zwitschert beim Ball

Der Level-Graph des Beispiel-Levels Micro Duck (`micro_duck_level` in `demos.cpp`):

```
Every 0,25 s ─▶ Branch( Robot Camera Sees(Ball).visible AND NOT Ball Seen )
                  ├True▶  Chirp ─▶ Set Ball Seen = visible
                  └False▶ Set Ball Seen = visible
```

Die boolesche Variable „Ball Seen“ sorgt dafür, dass die Ente nur einmal zwitschert, wenn der Ball
ins Bild kommt – nicht alle 0,25 s, solange er sichtbar ist. Ein Kommentarrahmen im Graphen sagt
ausdrücklich, dass das eine Demo der Roboter-Knoten ist und nichts, was der Roboter von selbst tut.
