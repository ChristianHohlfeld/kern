# Kapitel 2 – Bauen und Testen

Dieses Kapitel beschreibt alle Wege, Kern zu übersetzen und zu prüfen: die Toolchain, das
Build-Skript `kern/build.sh` mit seinen Modi, den Studio-Build, den nativen Lehrer, die nativen
Unit-Tests mit Sanitizern, die Logik-Tests über drei Ausführungsstufen, den Browser-Test, den
Determinismus-Fingerabdruck, die CI-Jobs im Einzelnen und die Veröffentlichung auf GitHub Pages.

Alle Pfade sind relativ zur Repository-Wurzel, sofern nicht anders angegeben.

---

## 2.1 Überblick

| Was | Werkzeug | Ergebnis |
|---|---|---|
| WebAssembly-Modul (Engine + Komponenten + Spielquellen) | `kern/build.sh` | `<out>.wasm` |
| Kern Studio als eine Datei | `tools/build_studio.sh` | `generated/studio.html`, `generated/kern_studio.wasm` |
| Lehrer für den Browser | `kern/build.sh --lehrer generated/kern_lehrer.wasm` | `generated/kern_lehrer.wasm` |
| Lehrer nativ | `kern/tools/lehrer.sh [out-dir]` | `kern/build/kern_lehrer` |
| Native Unit-Tests | `kern/tests/run.sh` | Testlauf mit ASan/UBSan, danach Logik-Export-Test |
| Logik-JIT-Test | `kern/build.sh … logic_wasm_test.cpp` + `node kern/tests/logic_wasm_test.mjs` | Vergleich Interpreter ↔ JIT |
| Lehrer im WebAssembly-Modul | `node kern/tests/lehrer_wasm_test.mjs generated/kern_lehrer.wasm` | eine PPO-Iteration über die C-ABI, Fingerabdruck |
| Browser-Test | `node kern/tests/e2e/studio_e2e.mjs <base-url>` | Studio, Play, Simulate, Export, Telefon |

Der kürzeste Weg zu einem laufenden Studio und grünen Tests (aus `README.md`):

```sh
# wasi-sdk 25 (Linux x86_64)
curl -sSL https://github.com/WebAssembly/wasi-sdk/releases/download/wasi-sdk-25/wasi-sdk-25.0-x86_64-linux.tar.gz | tar xz -C /opt
export WASI_SDK=/opt/wasi-sdk-25.0-x86_64-linux

tools/build_studio.sh                 # Kern Studio als eine Datei -> generated/studio.html
CXX=g++ CC=gcc kern/tests/run.sh      # native Unit-Tests (ASan/UBSan)

# C++-Beispielspiele
kern/build.sh --app kern/examples/hello/hello.wasm kern/examples/hello/hello.cpp
kern/build.sh --app kern/examples/orbrun/orbrun.wasm kern/examples/orbrun/orbrun.cpp

# Browser-Test (Studio, Fly Mode, Panels, Play, Simulate, exportiertes Spiel)
python3 -m http.server 8771 &
node kern/tests/e2e/studio_e2e.mjs http://127.0.0.1:8771
```

`generated/studio.html` läuft danach offline per Doppelklick. Zum Entwickeln liefert man die
Repository-Wurzel statisch aus und öffnet `kern/web/studio/index.html`; sie lädt
`generated/kern_studio.wasm`. `kern/web/studio/play.html?level=orbRun` spielt ein eingebautes Level
ohne Editor (weitere Werte laut Kopfkommentar: `physicsLab`, `shootingRange`, `plumo`, `basic`).

---

## 2.2 Die Toolchain: wasi-sdk 25

Kern braucht [wasi-sdk](https://github.com/WebAssembly/wasi-sdk), **Release 25**: clang, wasm-ld,
wasi-libc und libc++ für das Ziel `wasm32-wasip1`. Emscripten wird nicht benutzt.

- Die Umgebungsvariable `WASI_SDK` zeigt auf das Installationsverzeichnis. Ohne sie nimmt
  `kern/build.sh` `/opt/wasi-sdk` an.
- Die CI lädt `wasi-sdk-25.0-x86_64-linux.tar.gz` von GitHub und entpackt es mit
  `--strip-components=1` nach `/home/runner/wasi-sdk` (siehe [2.10](#210-die-ci-im-detail)).
- Für native Builds (Tests, Lehrer) genügt ein normaler Host-Compiler (GCC oder Clang; auf macOS
  Apple clang).
- `tools/build_studio.sh` braucht zusätzlich `python3`; die Node-Tests brauchen Node (die CI nutzt
  Node 22); der Browser-Test braucht Playwright mit Chromium.

---

## 2.3 `kern/build.sh` – das WebAssembly-Modul

```
kern/build.sh [--app] [--physics] [--game] [--studio] [--lehrer] <out.wasm> [compiler flags / game sources...]
```

### Komponenten

Ohne Schalter übersetzt das Skript alle `*.cpp` unter `kern/src` außer den Ordnern `app`,
`physics`, `game`, `studio` und `lehrer`, also `api`, `audio`, `core`, `geo`, `gfx`, `logic`, `net`
und `scene`, dazu immer die deterministische libm aus `kern/third_party/musl_math`. Die Schalter
fügen hinzu:

| Schalter | Fügt hinzu | Schließt ein |
|---|---|---|
| `--app` | App-Laufzeit `kern/src/app` für Spiele gegen `kern::App`; Host ist `kern/web/app.mjs` | – |
| `--physics` | Box3D (`kern/third_party/box3d`, als C17) und `kern/src/physics` | – |
| `--game` | Levels, Actors, Komponenten, Spiel-Laufzeit, prozedurale Stadt (`kern/src/game`) | `--physics` |
| `--studio` | Kern Studio (`kern/src/studio`) und seine C-ABI | `--game` |
| `--lehrer` | den headless Lehrer (`kern/src/lehrer`) und seine C-ABI für Web Worker | `--game` |

### Weitere Argumente

Nach dem Ausgabepfad folgen beliebig viele Argumente. Endet eines auf `.cpp` oder `.cc`, wird es als
Spielquelle wie die Engine übersetzt; alles andere geht als Compiler-Option an jede
C++-Übersetzung. Beispiel aus der CI (Test-Quelle plus Include-Pfad):

```sh
kern/build.sh /tmp/logic_test.wasm kern/tests/logic_wasm_test.cpp -Ikern/tests
```

### Umgebungsvariablen

| Variable | Standard | Wirkung |
|---|---|---|
| `WASI_SDK` | `/opt/wasi-sdk` | Installationsverzeichnis von wasi-sdk |
| `KERN_BUILD` | `release` | `debug` übersetzt mit `-O1 -g` ohne LTO; sonst `-O3 -flto=thin -DNDEBUG` |
| `KERN_JOBS` | alle Kerne (`nproc`) | Zahl paralleler Compiler und ThinLTO-Jobs |
| `KERN_OBJ_CACHE` | `$TMPDIR/kern-obj` (bzw. `/tmp/kern-obj`) | Objekt-Cache |

Hinweis zu `KERN_BUILD=debug`: Der Link-Schritt übergibt in jedem Modus `-Wl,--strip-debug`; das
fertige Modul enthält daher auch im Debug-Modus keine Debug-Informationen. Der Modus wirkt auf
Optimierung und LTO.

### Übersetzungsoptionen

Die Optionen sind die Grundlage von Robustheit und Determinismus:

```sh
# Conservative feature set: runs on every WebGL2-capable browser (Safari 15+).
features=(-mcpu=mvp -msign-ext -mbulk-memory -mnontrapping-fptoint -mmutable-globals)
target=(--target=wasm32-wasip1 --sysroot="$WASI_SDK/share/wasi-sysroot")
```

```sh
cxxflags=("${target[@]}" -std=c++20 "${opt[@]}" "${features[@]}"
          -fno-exceptions -fno-rtti -fvisibility=hidden -ffunction-sections -fdata-sections -ffp-contract=off
          -Wall -Wextra -Wno-unused-parameter -Werror=return-type
          "${includes[@]}" "${extra_flags[@]}")
```

- **`-ffp-contract=off`** überall (Kern, Box3D, musl): keine FMA-Zusammenfassung, sonst rundet
  `a*b+c` je nach Ziel anders ([Kapitel 3](03-grundlagen.md)).
- **Box3D** wird mit `-std=c17 -DBOX3D_DISABLE_SIMD -ffp-contract=off -w` übersetzt (Skalar-Pfad,
  wie in WebAssembly; Warnungen der Fremdbibliothek aus).
- **musl** mit `-std=c17 -ffp-contract=off -fno-math-errno -w`.
- **C++** ohne Ausnahmen und RTTI, versteckte Sichtbarkeit (exportiert wird nur, was
  `KERN_EXPORT` markiert), eine Section je Funktion und Datum, damit `--gc-sections` Ungenutztes
  entfernt.

### Linken

```sh
"$CXX" "${target[@]}" "${opt[@]}" "${features[@]}" "${lto[@]}" \
  "${cpp_objects[@]}" "${objects[@]}" \
  -mexec-model=reactor \
  -Wl,--gc-sections -Wl,--strip-debug -Wl,-z,stack-size=1048576 -Wl,--export-table -Wl,--growable-table \
  -Wl,--initial-memory=33554432 -Wl,--max-memory=2147483648 \
  ...
```

| Option | Bedeutung |
|---|---|
| `-mexec-model=reactor` | WASI-Reactor: kein `main`, der Host ruft `_initialize` und dann Exporte |
| `-z,stack-size=1048576` | 1 MiB Stack |
| `--initial-memory=33554432`, `--max-memory=2147483648` | 32 MiB Startspeicher, höchstens 2 GiB |
| `--export-table`, `--growable-table` | Die Funktionstabelle ist exportiert und wachsbar. Der Logik-JIT legt dort kompilierte Graphen ab (`table.grow`, `table.set` in `kern.mjs`, [Kapitel 9](09-logik.md)) |
| `--gc-sections`, `--strip-debug` | ungenutzten Code entfernen, Debug-Informationen entfernen |

Am Ende meldet das Skript `built <out> (<n> bytes)`.

### Paralleles Übersetzen und der Objekt-Cache

Jede Quelle wird in ein eigenes Objekt übersetzt, parallel (`KERN_JOBS`), und ThinLTO linkt sie.
Damit ein Neubau nur übersetzt, was sich geändert hat, und das Studio und die Beispielspiele die
Objekte der Engine teilen, gibt es einen Cache:

- **Box3D und musl:** je ein Cache-Ordner unter einem Schlüssel aus Optionen und Compiler-Version
  (`box3d-<key>`, `musl-math-<key>`); ein Objekt wird neu übersetzt, wenn es fehlt oder älter als
  seine Quelle oder der neueste Header ist.
- **C++:** Ordner `cpp-<flags_key>`; jedes Objekt heißt `<name>-<key>.o`, wobei der Schlüssel aus
  dem Hash **aller** Header-Inhalte (unter `include`, `src` und Box3Ds `include`), dem Quellpfad und
  dem Quellinhalt entsteht. Folge: **Jede Header-Änderung übersetzt alle C++-Quellen neu** –
  einfach und sicher, wie der Kommentar im Skript sagt.
- **ThinLTO:** Cache in `$KERN_OBJ_CACHE/thinlto`.

Die CI hält den Cache zwischen Läufen (siehe [2.10](#210-die-ci-im-detail)).

---

## 2.4 `tools/build_studio.sh` – Kern Studio als eine Datei

```
tools/build_studio.sh [--skip-wasm] [out.html]     (default: generated/studio.html)
```

1. `kern/build.sh --studio generated/kern_studio.wasm` – Engine, Box3D, Spiel-Laufzeit, Studio.
   Mit `--skip-wasm` entfällt dieser Schritt; das Modul muss dann schon existieren, sonst endet das
   Skript mit `missing …/kern_studio.wasm (run without --skip-wasm)`.
2. Ein Python-Teil setzt die Seite zusammen:
   - `kern/web/studio/studio.css` ersetzt den `<link>` als `<style>`.
   - `kern.mjs` und `game.mjs` werden zusätzlich als Text eingebettet
     (`<script type="text/plain" id="kern-src-kern">` bzw. `kern-src-game`). Aus ihnen schreibt das
     Studio exportierte Spiele.
   - Das Modul wird base64-kodiert eingebettet (`<script type="text/plain" id="kern-wasm">`).
   - `kern.mjs`, `game.mjs`, `logic.mjs` und `studio.mjs` laufen als **ein** Inline-Modul: Die
     `import { … } from "…";`-Zeilen und die `export`-Schlüsselwörter werden entfernt und die
     Quellen aneinandergehängt. So braucht die Seite keine `blob:`-Importe und funktioniert auch
     unter strengen Content Security Policies.
   - Ein Boot-Block setzt `window.KERN_SOURCES` und ruft `startStudio({base64: …})`; schlägt der Start
     fehl, erscheint die Meldung im Element `engineNotice`.

Enthält eine der Quellen die Zeichenfolge `</script`, bricht der Build mit
`<name> source contains '</script'; cannot inline it` ab – sie würde das Inline-Skript beenden.

Ausgabe: `generated/studio.html: <Größe> bytes (wasm <Größe> bytes)`.

---

## 2.5 Der native Lehrer: `kern/tools/lehrer.sh`

```
kern/tools/lehrer.sh [out-dir]      -> <out-dir>/kern_lehrer (Standard: kern/build)
```

Das Skript baut `kern_lehrer` für Linux und macOS, x86 und ARM, optimiert (`-O3 -DNDEBUG
-ffp-contract=off`) und auf allen Kernen:

- Box3D mit `-DBOX3D_DISABLE_SIMD`, musl mit `-fno-math-errno`.
- Alle Quellen unter `kern/src` **außer** `studio/` und `app/`, dazu `kern/tests/gl_stub.cpp` (die
  WebGL-Attrappe; der Lehrer rendert nicht) und `kern/tools/kern_lehrer.cpp`.
- Gelinkt mit `-lm -lpthread`.
- `KERN_NATIVE=1` (Standard) probiert `-march=native`, dann `-mcpu=native`, und nimmt die erste
  Option, die der Compiler annimmt. Diese Optionen lassen den Compiler nur Befehle wählen; sie
  ändern kein Ergebnis, weil `-ffp-contract=off` gilt.
- `CC`/`CXX` wählen die Compiler, `KERN_JOBS` die Parallelität. Der Job-Pool ist portabel bis zur
  bash 3.2 von macOS.

Die Kommandozeile von `kern_lehrer` steht vollständig in [Kapitel 17](17-referenz.md); das Training
selbst beschreibt [Kapitel 14](14-lehrer.md).

---

## 2.6 Native Unit-Tests: `kern/tests/run.sh`

```sh
CXX=g++ CC=gcc kern/tests/run.sh            # alle Tests
kern/tests/run.sh plumo                 # nur Tests, deren Name "plumo" enthält
```

### Was das Skript tut

1. **Box3D** wird einmal **ohne** Sanitizer übersetzt (`-O2 -g -DBOX3D_DISABLE_SIMD
   -ffp-contract=off`) und gecacht – Fremdcode im Skalar-Pfad wie in WebAssembly.
2. **musl** wird ebenso einmal übersetzt (`-O2 -g -ffp-contract=off -fno-math-errno`).
3. **Aller Kern-C++-Code** (`kern/src/**/*.cpp`, `gl_stub.cpp`, `test_main.cpp`) wird mit
   **AddressSanitizer und UndefinedBehaviorSanitizer** übersetzt:

   ```sh
   flags=(-std=c++20 -O1 -g -ffp-contract=off -fno-exceptions -fno-rtti -fsanitize=address,undefined -fno-sanitize-recover=undefined
          -Wall -Wextra -Wno-unused-parameter -Werror -I"$root/include" -I"$root/third_party/box3d/include" -I"$here")
   ```

   Anders als `build.sh` gilt hier **`-Werror`**: Jede Warnung bricht den Testbuild ab.
   `-fno-sanitize-recover=undefined` macht undefiniertes Verhalten zum Abbruch.
4. Das Testprogramm `kern_tests` läuft im Ordner `kern/tests` mit den Argumenten des Skripts.
5. **Nur ohne Argumente** folgt der Logik-Export-Test (siehe [2.7](#27-logik-tests-drei-stufen-ein-ergebnis)).

Umgebungsvariablen: `CXX`/`CC` (Standard `c++`/`cc`), `KERN_TEST_OUT` (Ausgabeordner, Standard
`$TMPDIR/kern-tests`), `KERN_JOBS`.

### Das Testprogramm

`kern/tests/test_main.cpp` enthält ein eigenes kleines Testgerüst ohne Fremdbibliothek: `TEST(name)`
registriert eine Funktion, `CHECK(cond)` und `CHECK_NEAR(a, b, eps)` zählen Prüfungen und geben bei
einem Fehler Datei, Zeile, Ausdruck und `last_error()` aus. `main` führt alle Tests aus, deren Name
den Filter (erstes Argument) enthält, schreibt je Test `ok` oder `FAIL` und am Ende
`<n> checks, <m> failures`. Der Rückgabewert ist 1 bei mindestens einem Fehler.

Die Tests decken die ganze Engine ab, darunter (Auswahl, Namen wie im Code):

| Bereich | Beispiele |
|---|---|
| Mathematik, Szene | `math_matrices`, `math_camera`, `bvh_matches_brute_force` |
| Geometrie | `earcut_shapes`, `weld_and_hash`, `building_builder`, `road_and_terrain_builders`, `primitives_closed` |
| Renderer gegen die GL-Attrappe | `renderer_meshes`, `renderer_frame`, `renderer_context_loss`, `renderer_id_ownership`, `stream_guards`, `renderer_mirrored_instances` |
| Physik | `physics_world_bodies_and_events`, `physics_joints_and_grab`, `physics_scale_is_a_unit_system` |
| JSON | `json_numbers_match_javascript`, `json_parse_and_stringify` |
| Spiel-Laufzeit | `game_level_round_trip_and_validation`, `game_world_orb_run_play`, `game_world_rules` |
| Logik | `logic_flow_variables_and_latent_nodes`, `logic_state_machine_timers_events_conditions`, `logic_tier_cases_interpreter` |
| Audio | `audio_mixer_spatial`, `audio_builtin_library`, `audio_collider_impact_sounds` |
| Netzwerk | `net_serialization`, `net_reliable_channel_under_bad_network`, `net_client_server`, `net_game_server_and_clients` |
| Studio | `studio_starts_with_orb_run`, `studio_gizmo_translate_and_undo`, `studio_undo_redo_is_exact_and_deterministic`, `studio_save_load_and_textures` |
| Plumo | `plumo_policy_matches_the_onnx_reference`, `plumo_stands_walks_and_turns_on_its_policy`, `plumo_runs_every_move_of_its_set` |
| Lehrer | `lehrer_mlp_gradient_matches_finite_differences`, `lehrer_ppo_learns_to_reach`, `lehrer_exports_onnx_like_pollens_networks`, `lehrer_calibrates_a_servo_from_bench_logs` |
| Determinismus | `determinism_is_bit_identical_across_builds_and_platforms`, `lehrer_runs_the_same_on_any_number_of_workers` |

### Die GL-Attrappe

Nativ gibt es kein WebGL. `kern/tests/gl_stub.cpp` stellt die Funktionen aus `include/kern/gfx/gl.h`
als aufzeichnende Attrappe bereit. Damit prüfen die Renderer-Tests Validierung, Instanzkapazität,
Uniform-Cache, Wiederverwendung der Schattenkarte, Kontextverlust, Linien und Ebenen, ohne GPU.
Derselbe Stub wird in den nativen Lehrer gelinkt.

---

## 2.7 Logik-Tests: drei Stufen, ein Ergebnis

Ein Logik-Graph kann in drei Stufen laufen ([Kapitel 9](09-logik.md)): im **Interpreter**, als
**JIT** (zur Laufzeit erzeugtes WebAssembly-Modul) und als **exportiertes C++**. Alle drei müssen
Bit für Bit dieselben Ergebnisse liefern. Die gemeinsamen Testfälle stehen in
`kern/tests/logic_cases.h` (Graphen als JSON, z. B. `math_loops`, `gates_and_sequences`).

| Stufe | Test | Wo |
|---|---|---|
| Interpreter | `logic_tier_cases_interpreter` in `test_main.cpp` | `run.sh` |
| Exportiertes C++ | `logic_native_test.cpp` | `run.sh` (zweite Phase, nur ohne Argumente) |
| JIT (WebAssembly) | `logic_wasm_test.cpp` + `logic_wasm_test.mjs` | CI-Job `build` |

**Exportiertes C++.** `kern_tests --emit-logic <dir>` schreibt für jeden Fall den generierten
C++-Quelltext (`logic_<name>.cpp`) und die Ergebnisse des Interpreters (`logic_expected.txt`).
`run.sh` übersetzt die generierten Quellen zusammen mit den Objekten von `src/logic` und `src/core`
und musl in `logic_native_test` und vergleicht. Jeder Graph muss dabei in der Stufe `native C++`
laufen.

**JIT.** Die CI baut `logic_wasm_test.cpp` als eigenes Modul und lässt es unter Node laufen:

```sh
kern/build.sh /tmp/logic_test.wasm kern/tests/logic_wasm_test.cpp -Ikern/tests
node kern/tests/logic_wasm_test.mjs /tmp/logic_test.wasm
```

Jeder Fall läuft einmal im Interpreter und einmal JIT-kompiliert; das erzeugte Modul übersetzt V8
und verknüpft es mit der Funktionstabelle der Engine – genau wie im Browser. Die Stufe muss
`compiled (WebAssembly)` heißen und die Ergebnisse müssen gleich sein. Zum Schluss misst ein
Benchmark eine Million Schleifendurchläufe in beiden Stufen und gibt den Faktor aus.

---

## 2.8 Der Browser-Test (e2e)

```
node kern/tests/e2e/studio_e2e.mjs [base-url]     (Standard http://127.0.0.1:8771)
```

Vorher muss die Repository-Wurzel statisch ausgeliefert werden (`python3 -m http.server 8771`) und
`generated/studio.html` samt `generated/kern_studio.wasm` existieren. Playwright wird per `import`
gesucht; für eine globale Installation fällt der Test auf `require()` zurück (ESM ignoriert
`NODE_PATH`).

Der Test startet Chromium mit Software-WebGL (`--use-gl=angle --use-angle=swiftshader
--enable-unsafe-swiftshader --ignore-gpu-blocklist`). Weil SwiftShader die Stadt mit etwa einem
Bild pro Sekunde rendert, wartet jede Prüfung auf ihre Wirkung statt auf eine feste Zeit
(`until(fn, ms)`). Ob der Viewport wirklich gezeichnet hat, misst `ink()`: die Zahl verschiedener
Farben im Canvas.

Geprüft werden unter anderem:

- **Single-File-Build** (`/generated/studio.html`): Start mit Orb Run, Menüs, Fly Mode (rechte Maustaste
  + W), Werkzeuge, Outliner und Details, Bearbeiten der X-Position, Fokus (F), Content Drawer
  (Klick und Ziehen), Auswahl und Verschieben mit der linken Maustaste, Sound über Web Audio,
  Kontextmenüs, deterministisches Undo, Play, Simulate mit Physik-Debug, vier Viewports, neues Level
  aus Vorlage, Shooting Range, Logik-Editor, Plumo mit Kopfkamera und ToF, Hilfe, Mehrspieler
  im Editor und über zwei Tabs (BroadcastChannel), Spiel packen und das gepackte Spiel spielen, das
  Spiel auf einem Telefon.
- **Modul-Build** (`/kern/web/studio/index.html`): Start, Farbwähler.
- **Wiederherstellen** nach einem Neuladen, **Einstellungen** (Standardwerte, Speichern,
  Zurücksetzen), **schmales Fenster**, **Telefon** (kompaktes Layout, Menüs per Touch, Plumo,
  Play/Simulate/Pause/Stop per Tipp), **Android-Dateiauswahl**.

Am Ende steht `<n> checks, <m> failures (screenshots: <ordner>)`; der Rückgabewert ist 1 bei
Fehlern. In GitHub Actions wird jeder Fehler zusätzlich als `::error::`-Annotation ausgegeben.

### Screenshots

Screenshots sind mit Software-WebGL langsam. Der Test nimmt sie deshalb **nur auf, wenn
`KERN_E2E_SHOTS` einen Ordner nennt**:

```sh
KERN_E2E_SHOTS=artifacts/e2e node kern/tests/e2e/studio_e2e.mjs http://127.0.0.1:8771
```

Dann entstehen `studio.png`, `studio-selected.png`, `studio-play.png`, `studio-simulate.png`,
`studio-quad.png`, `studio-physics-lab.png`, `studio-multiplayer.png`, `game.png`,
`game-phone.png`, `studio-narrow.png` sowie je Telefongröße `phone-play-<breite>.png` und
`phone-dock-<breite>.png`. Einige davon verwendet auch dieses Buch.

![Simulate mit Physik-Debug-Ansicht, wie der Browser-Test es prüft](bilder/studio-simulate.png)

---

## 2.9 Der Determinismus-Fingerabdruck

Der Fingerabdruck beweist, dass jede Build-Variante Plumo Bit für Bit gleich simuliert.
`lehrer::determinism_fingerprint(ticks)` (`src/lehrer/fingerprint.cpp`) lässt die Ente mit Pollens
`velstand`-Netz in der Studio-Physik gehen und drehen – 150 Takte vorwärts (Kommando 0,15), danach
Drehen auf der Stelle (0,8) – und bildet je Regeltakt einen FNV-1a-Hash über Position, Rotation,
Geschwindigkeit, Drehgeschwindigkeit, alle Gelenkwinkel und Gelenkgeschwindigkeiten.

Die Referenz für 400 Takte liegt in `kern/tests/determinism_fingerprint.txt`, eine Zeile je Takt:

```
0 7588b1a57dfadf95
1 46e1d239a7ed5cfe
2 4770483abd640417
...
```

Geprüft wird an drei Stellen:

| Plattform | Prüfung |
|---|---|
| GCC auf x86 mit Sanitizern | Test `determinism_is_bit_identical_across_builds_and_platforms` (`run.sh`); gibt bei Abweichung den ersten abweichenden Takt aus |
| WebAssembly in Node | `kern/tests/lehrer_wasm_test.mjs` über `kern_lehrer_fingerprint(400)` |
| Apple clang auf ARM | CI-Job `determinism` auf `macos-14`: `kern_lehrer --fingerprint 400 \| diff - kern/tests/determinism_fingerprint.txt` |

Von Hand:

```sh
kern/tools/lehrer.sh
kern/build/kern_lehrer --fingerprint 400 | diff - kern/tests/determinism_fingerprint.txt
```

Weicht der Fingerabdruck ab, hat sich entweder die Simulation absichtlich geändert (dann muss die
Referenzdatei neu erzeugt und die Änderung begründet werden) oder ein Stück Code rechnet
plattformabhängig – etwa ein `std::sin` statt `kern::dm::sin` in Code, der die Simulation speist,
eine fehlende `-ffp-contract=off`-Option oder eine Summe, deren Reihenfolge von der Zahl der Threads
abhängt ([Kapitel 3](03-grundlagen.md), [Kapitel 14](14-lehrer.md)).

Der Test `lehrer_runs_the_same_on_any_number_of_workers` ergänzt das: Er lernt mit 1, 2 und 4
Workern (insgesamt 8 Enten) und vergleicht die Netze Byte für Byte.

---

## 2.10 Die CI im Detail

`.github/workflows/ci.yml` ist eine einzige Pipeline. Sie läuft bei jedem Push auf `main`, bei Pull
Requests und manuell (`workflow_dispatch`). Die Seite wird einmal gebaut und veröffentlicht, sobald
sie gebaut ist; native und Browser-Tests laufen parallel und halten die Veröffentlichung nicht auf.

```
          ┌──────────┐   ┌──────────────┐
 push ──▶ │ native   │   │ determinism  │      (unabhängig, parallel)
          └──────────┘   └──────────────┘
          ┌──────────┐
          │ build    │──┬──▶ deploy  (nur main, kein Pull Request)
          └──────────┘  └──▶ e2e
```

**Nebenläufigkeit:** Die Gruppe ist `ci-<ref>-pr` für Pull Requests und `ci-<ref>-<sha>` sonst;
abgebrochen werden nur ältere Läufe eines Pull Requests. Auf `main` läuft jeder Push bis zum Ende,
damit jeder grüne Stand veröffentlicht wird (laut Kommentar dauert ein Lauf etwa 17 Minuten).
Rechte: `contents: write` (für den Push nach `gh-pages`).

### Job `native` (ubuntu-latest, 30 min)

```yaml
- name: Native unit tests (ASan/UBSan)
  run: CXX=g++ CC=gcc kern/tests/run.sh
```

Alle Unit-Tests mit Sanitizern, danach der Logik-Export-Test.

### Job `determinism` (macos-14, 30 min)

```yaml
- name: Build the Lehrer (Apple clang, arm64)
  run: KERN_NATIVE=1 kern/tools/lehrer.sh build-mac
- name: Same hashes as x86 and wasm
  run: build-mac/kern_lehrer --fingerprint 400 | diff - kern/tests/determinism_fingerprint.txt
```

Der Lehrer, gebaut mit Apple clang auf ARM und `-mcpu=native`, muss die Ente exakt wie GCC auf x86
und WebAssembly bewegen.

### Job `build` (ubuntu-latest, 30 min)

1. Checkout, Node 22.
2. **wasi-sdk 25** aus dem Cache (`wasi-sdk-25.0`), sonst Download nach `/home/runner/wasi-sdk`.
3. **Objekt-Cache** `/home/runner/kern-obj` (`KERN_OBJ_CACHE`), Schlüssel `kern-obj-<sha>`, wiederhergestellt
   mit dem Präfix `kern-obj-` – also vom jüngsten früheren Lauf.
4. **Bauen:**

   ```sh
   tools/build_studio.sh
   kern/build.sh --app kern/examples/hello/hello.wasm kern/examples/hello/hello.cpp
   kern/build.sh --app kern/examples/orbrun/orbrun.wasm kern/examples/orbrun/orbrun.cpp
   kern/build.sh --lehrer generated/kern_lehrer.wasm
   for f in kern/web/kern.mjs kern/web/app.mjs kern/web/studio/*.mjs kern/web/lehrer/*.mjs tools/net_relay.mjs; do node --check "$f"; done
   ```

   `node --check` prüft die Syntax aller JavaScript-Dateien.
5. **Lehrer in WebAssembly:** `node kern/tests/lehrer_wasm_test.mjs generated/kern_lehrer.wasm`. Der
   Test legt über die C-ABI zwei Enten als Sammler und einen Lerner für `jump` an (Einstellung
   `{"steps":8}`), prüft die Nachahmungsrunden des Rezepts (8 Runden à 96 Episoden), führt einen
   Anteil einer DAgger-Runde und eine Nachahmungsrunde aus, sammelt einen Rollout, lernt eine
   PPO-Iteration, exportiert ONNX und vergleicht den Fingerabdruck mit der Referenzdatei.
6. **Logik-JIT:** wie in [2.7](#27-logik-tests-drei-stufen-ein-ergebnis).
7. **Seite zusammenstellen** in `_site/`:

   | Ziel | Quelle |
   |---|---|
   | `index.html` | `site/index.html` |
   | `LICENSE.txt`, `LICENSES.txt` | `LICENSE` (Wurzel) und `kern/LICENSES.md`; die Fußzeile der Startseite verlinkt beide |
   | `editor/index.html` | `generated/studio.html` mit vorangestelltem Kommentar `<!--BUILD_SHA:<sha>-->` |
   | `editor/build.json` | `{"sha":"<sha>"}` für die Update-Prüfung des installierbaren Editors |
   | `editor/` | `site/editor/` (Manifest, Service Worker, Icons) |
   | `editor/pollen/` | `kern/third_party/pollen/*.onnx` (Pollens Policy-Set, Apache-2.0) |
   | `editor/policies/` | `kern/policies/*.onnx` (Kerns eigene Netze) |
   | `generated/` | `kern_studio.wasm`, `kern_lehrer.wasm` |
   | `lehrer/index.html`, `kern/web/lehrer/*.mjs` | die Lehrer-Seite und ihre Worker |
   | `train/index.html` | Weiterleitung von der alten Adresse `/train/` nach `../lehrer/` |
   | `kern/web/` | `kern.mjs`, `app.mjs`, `studio/*.mjs`, `studio/*.html`, `studio/*.css` |
   | `kern/examples/hello/`, `kern/examples/orbrun/` | `index.html` und `.wasm` der Beispiele |
   | `.nojekyll` | GitHub Pages ohne Jekyll |

8. Hochladen als Artefakt `site` (7 Tage, mit versteckten Dateien).

### Job `deploy` (nur `main`, kein Pull Request; braucht `build`)

Lädt das Artefakt `site`, legt darin ein frisches Git-Repository mit dem Branch `gh-pages` an,
committet alles als `Pages: <sha>` und pusht mit `-f` nach `gh-pages`. Die Gruppe `pages` bricht
einen laufenden Push nicht ab; nur der jüngste wartende folgt. Die Seite erscheint unter
<https://christianhohlfeld.github.io/kern/>.

Hinweis: `README.md` nennt noch eine eigene `pages.yml`. Im Repository gibt es nur `ci.yml`; die
Veröffentlichung ist dort der Job `deploy`.

### Job `e2e` (ubuntu-latest, 30 min; braucht `build`)

1. Lädt das Artefakt `site` und stellt daraus `generated/studio.html`, `generated/kern_studio.wasm`
   und die App-Dateien (`manifest.webmanifest`, `sw.js`, `build.json`, `icons`, `pollen`) wieder
   her – so, wie sie veröffentlicht werden.
2. Playwright 1.56.0 aus dem Cache oder per `npm install` nach `/tmp/e2e`, dann
   `playwright install --with-deps chromium`.
3. Startet `python3 -m http.server 8771 --bind 127.0.0.1` und führt
   `NODE_PATH=/tmp/e2e/node_modules node kern/tests/e2e/studio_e2e.mjs http://127.0.0.1:8771` aus.
4. Lädt `artifacts/e2e/` als Artefakt `e2e-screenshots` hoch (auch bei Fehlern; fehlt der Ordner
   oder ist er leer, wird das ignoriert). Der Schritt setzt `KERN_E2E_SHOTS` nicht; Screenshots
   entstehen nur, wenn die Variable gesetzt ist ([2.8](#28-der-browser-test-e2e)).

---

## 2.11 Lokal wie die CI

Eine lokale Reihenfolge, die der CI entspricht:

```sh
export WASI_SDK=/opt/wasi-sdk-25.0-x86_64-linux

# native Tests
CXX=g++ CC=gcc kern/tests/run.sh

# Determinismus mit dem nativen Lehrer
kern/tools/lehrer.sh
kern/build/kern_lehrer --fingerprint 400 | diff - kern/tests/determinism_fingerprint.txt

# Module
tools/build_studio.sh
kern/build.sh --lehrer generated/kern_lehrer.wasm
node kern/tests/lehrer_wasm_test.mjs generated/kern_lehrer.wasm
kern/build.sh /tmp/logic_test.wasm kern/tests/logic_wasm_test.cpp -Ikern/tests
node kern/tests/logic_wasm_test.mjs /tmp/logic_test.wasm

# Browser
python3 -m http.server 8771 &
node kern/tests/e2e/studio_e2e.mjs http://127.0.0.1:8771
```

Die Entwicklungsseiten laden ihre Dateien relativ: `kern/web/studio/index.html` und `play.html`
laden `../../../generated/kern_studio.wasm`; die Policies von Plumo kommen auf der
Entwicklungsseite aus `kern/third_party/pollen/` und `kern/policies/`, im Single-File-Build unter
`/generated/` aus `../kern/third_party/pollen/` und `../kern/policies/`, auf der veröffentlichten
Seite aus `pollen/` und `policies/` neben dem Editor (`loadPolicies` in `game.mjs`). Die
Lehrer-Seite `kern/web/lehrer/index.html` bindet `../kern/web/lehrer/lehrer.mjs` ein und ist damit
auf die Anordnung der veröffentlichten Seite (`/lehrer/`) ausgelegt.

---

## 2.12 Typische Fehler

| Meldung / Symptom | Ursache | Abhilfe |
|---|---|---|
| `wasi-sdk clang++ not found at …/bin/clang++ (set WASI_SDK)` | `WASI_SDK` fehlt oder zeigt falsch | `export WASI_SDK=…` auf das entpackte wasi-sdk 25 |
| `missing …/generated/kern_studio.wasm (run without --skip-wasm)` | `build_studio.sh --skip-wasm` ohne vorhandenes Modul | einmal ohne `--skip-wasm` bauen |
| `<name> source contains '</script'; cannot inline it` | eine JS-Quelle enthält `</script` | die Zeichenfolge im Quelltext vermeiden (z. B. aufteilen) |
| Testbuild bricht an einer Warnung ab | `run.sh` übersetzt mit `-Werror` | Warnung beheben; `build.sh` ist toleranter, die Tests nicht |
| Abbruch mit `runtime error:` / `AddressSanitizer` | UBSan/ASan fanden einen Fehler | der Bericht nennt Datei und Zeile; mit Filter einzeln nachstellen (`run.sh <name>`) |
| `first differing tick: N` bzw. `diff` gegen den Fingerabdruck schlägt fehl | Simulation rechnet plattformabhängig oder wurde geändert | siehe [2.9](#29-der-determinismus-fingerabdruck) |
| Logik-Test `runs in "interpreter"` statt `compiled (WebAssembly)` | JIT nicht verfügbar oder Modul nicht übersetzbar | Ausgabe des JIT prüfen; die Engine fällt auf den Interpreter zurück (`logic JIT failed …` in der Konsole) |
| Jeder kleine Header-Edit baut alles neu | gewollt: Header-Hash im Cache-Schlüssel | – |
| Browser-Test hängt bei `openStudio` | Seite nicht ausgeliefert oder Modul fehlt; der Test wartet bis 90 s auf `KernStudio.frames() > 5` | Server in der Repository-Wurzel starten, `generated/` bauen |
| Playwright-Import schlägt fehl | Paket nicht gefunden | `npm install playwright` (CI: 1.56.0) und `NODE_PATH` setzen, wie in der CI |
| Lehrer-Seite lokal ohne Modul | Pfade auf die veröffentlichte Anordnung ausgelegt | Seite wie in [2.10](#job-build-ubuntu-latest-30-min) zusammenstellen |

---

© 2026 Christian Heinrich Hohlfeld · Alle Rechte vorbehalten · [Impressum und Rechte](00-impressum.md)
