# Kern Engine

Kern ist eine 3D-Engine in **C++20**, die mit wasi-sdk nach **WebAssembly** kompiliert und im Browser über **WebGL2** zeichnet – mit **Box3D**-Physik, einer **Sound-Engine in C++** (Mixer, 3D-Klang, Hall, Synthesizer), einer Spiel-Laufzeit aus Levels, Actors und Komponenten und **Kern Studio**, einem Engine-Editor im Stil der Unreal Engine: Viewports mit Fly Mode, Gizmos, Outliner, Details, Content Drawer, Play und Simulate im Viewport, Spiele als eine HTML-Datei. Kein three.js, kein Emscripten, keine Engine-Logik in JavaScript.

**Live:** https://christianhohlfeld.github.io/kern/ – Kern Studio (startet mit Orb Run), spielbare Levels und C++-Beispiele im Browser.

| | |
|---|---|
| Engine, Renderer, Geometrie, App-Laufzeit | [`kern/README.md`](kern/README.md) |
| Kern Studio, Steuerung, Komponenten, Sound, `.klevel`-Format, Protokoll | [`kern/docs/studio.md`](kern/docs/studio.md) |
| Lizenzen der Fremdbibliotheken (Earcut, Box3D) | [`kern/LICENSES.md`](kern/LICENSES.md) |

## Schnellstart

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

`generated/studio.html` läuft offline und direkt per Doppelklick. Zum Entwickeln das Repository statisch ausliefern und `kern/web/studio/index.html` öffnen; `kern/web/studio/play.html?level=orbRun` (oder `physicsLab`) spielt ein eingebautes Level ohne Editor.

## Herkunft

Kern ist in [SpawnCity](https://spawncity.de) entstanden, das über Kern rendert. Dieses Repository enthält die Engine mit ihrer Commit-Historie aus SpawnCity, auf die Engine-Dateien beschränkt. Wo `kern/README.md` SpawnCity-Dateien nennt (`games/spawncity`, `sim/kern_*.mjs`, `tools/build_kern_wasm.sh`, `tests/kern_*_test.mjs`), liegen diese im SpawnCity-Repository.

## CI und Pages

`.github/workflows/ci.yml` baut alle Module, führt die nativen Tests und den Browser-Test aus und hängt `studio.html` als Artefakt an (Screenshots des Browser-Tests nur, wenn `KERN_E2E_SHOTS` gesetzt ist). Bei jedem Push auf `main` veröffentlicht der Job `deploy` in derselben Datei Studio und Beispiele mit der Startseite `site/index.html` auf GitHub Pages.
