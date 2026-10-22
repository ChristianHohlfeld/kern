# Kern Engine

Kern ist eine 3D-Engine in **C++20**, die mit wasi-sdk nach **WebAssembly** kompiliert und im Browser über **WebGL2** zeichnet – mit **Box3D**-Physik und dem **Kern Editor**, der Editor3D-Oberfläche als vollständig bedienbarer Game Engine. Kein three.js, kein Emscripten, keine Engine-Logik in JavaScript.

**Live:** https://christianhohlfeld.github.io/kern/ – Editor, Beispielspiel und C++-Beispiele im Browser.

| | |
|---|---|
| Engine, Renderer, Geometrie, App-Laufzeit | [`kern/README.md`](kern/README.md) |
| Kern Editor, Spiel-Komponenten, Protokoll, `.e3d`-Format | [`kern/docs/editor.md`](kern/docs/editor.md) |
| Lizenzen der Fremdbibliotheken (Earcut, Box3D) | [`kern/LICENSES.md`](kern/LICENSES.md) |

## Schnellstart

```sh
# wasi-sdk 25 (Linux x86_64)
curl -sSL https://github.com/WebAssembly/wasi-sdk/releases/download/wasi-sdk-25/wasi-sdk-25.0-x86_64-linux.tar.gz | tar xz -C /opt
export WASI_SDK=/opt/wasi-sdk-25.0-x86_64-linux

tools/build_editor.sh                 # Kern Editor als eine Datei -> generated/editor3d.html
CXX=g++ CC=gcc kern/tests/run.sh      # native Unit-Tests (ASan/UBSan)

# Beispielspiele
kern/build.sh --app kern/examples/hello/hello.wasm kern/examples/hello/hello.cpp
kern/build.sh --app kern/examples/orbrun/orbrun.wasm kern/examples/orbrun/orbrun.cpp
kern/build.sh --app --world kern/examples/scenegame/scenegame.wasm kern/examples/scenegame/scenegame.cpp

# Browser-Test (Editor, alle Menüs und Dialoge, Player, exportiertes Spiel, scenegame)
python3 -m http.server 8771 &
node kern/tests/e2e/editor_e2e.mjs http://127.0.0.1:8771
```

`generated/editor3d.html` läuft offline und direkt per Doppelklick. Zum Entwickeln das Repository statisch ausliefern und `kern/web/editor/index.html` öffnen.

## Herkunft

Kern ist in [SpawnCity](https://spawncity.de) entstanden, das über Kern rendert. Dieses Repository enthält die Engine mit ihrer Commit-Historie aus SpawnCity, auf die Engine-Dateien beschränkt. Wo `kern/README.md` SpawnCity-Dateien nennt (`games/spawncity`, `sim/kern_*.mjs`, `tools/build_kern_wasm.sh`, `tests/kern_*_test.mjs`), liegen diese im SpawnCity-Repository.

## CI und Pages

`.github/workflows/ci.yml` baut alle Module, führt die nativen Tests und den Browser-Test aus und hängt `editor3d.html` sowie die Screenshots als Artefakt an. `.github/workflows/pages.yml` baut bei jedem Push auf `main` Editor und Beispiele und veröffentlicht sie mit der Startseite `site/index.html` auf GitHub Pages.
