# Micro Duck: Modell und Policies (Pollen Robotics)

Quellen für Kerns Micro Duck (`src/game/walker.cpp`, Doku: `docs/walker.md`).

| Datei | Herkunft | Lizenz |
|---|---|---|
| `velstand.onnx` | huggingface.co/pollen-robotics/microduck-policies, Branch `main` (v8, Training-Run `pollen-robotics/mjlab_microduck/0045i2si`, Checkpoint 2749), SHA-256 `2fc7f8ee6acd7d0d3f66a99927b5d5a56589aeed02ec714de329b252d5ccf492` | Apache-2.0 |
| `alpha_walking.onnx` | huggingface.co/pollen-robotics/microduck-policies, Branch `main`, SHA-256 `e36332d383997d51401897734cd3e79cf5038406feddb18b4d57ecfb141daa6c` | Apache-2.0 |
| `alpha_stand.onnx` | huggingface.co/pollen-robotics/microduck-policies, Branch `main`, SHA-256 `1569268713e40deea795dd2922dba50d3621e15a872855408b6b1b125b1c094b` | Apache-2.0 |
| `alpha_sitstand.onnx` | huggingface.co/pollen-robotics/microduck-policies, Branch `main`, SHA-256 `a6f102d9fd655782ba85bfffe18175c34621da719cffcdd673be7982a06fc254` | Apache-2.0 |
| `alpha_ground_pick.onnx` | huggingface.co/pollen-robotics/microduck-policies, Branch `main`, SHA-256 `ffbf5109982ff999b0ba53afe86b9ae731bbec679d67fb7f8ab4c52152c88872` | Apache-2.0 |
| `roller.onnx` | huggingface.co/pollen-robotics/microduck-policies, Branch `main`, SHA-256 `cf05651d2708a2f9364212e86b866c97a70ace8131c492500105e8f28bf99afd` | Apache-2.0 |
| `roller_crouch.onnx` | huggingface.co/pollen-robotics/microduck-policies, Branch `main`, SHA-256 `a1a084be240469c76ac9d3fa44d4792f16d4b1da60398b3ecd3cfc5e2244d990` | Apache-2.0 |
| `roulade.onnx` | huggingface.co/pollen-robotics/microduck-policies, Branch `main`, SHA-256 `3d60da08fc13f29c1b57f41977aa898132c0d60042100149d8e775affcbca32b` | Apache-2.0 |
| `ball_kick_left.onnx` | huggingface.co/pollen-robotics/microduck-policies, Branch `main`, SHA-256 `d6928284dccd3dd61e08bf2f760effa74309fbefd97b2b31afb2a60f526d196a` | Apache-2.0 |
| `ball_kick_right.onnx` | huggingface.co/pollen-robotics/microduck-policies, Branch `main`, SHA-256 `147a32c388c6b19111b3ac3b550a9a6dc8b8bf267118af4d8c3712522eedb5af` | Apache-2.0 |
| `pickup_detector.onnx` | github.com/pollen-robotics/microduck @ `942e58e`, `duck-control/models/` (v2, robotds Pick-up-Erkennung), SHA-256 `d93a33878e028c79deadf3f50fef4602a6b601acb700f81b5a4c0a06cc6c1b75`; als Bytes eingebaut in `pickup_detector.inc` | Apache-2.0 |
| `manifest.json` | dasselbe Repository: die Liste aller zehn Netze mit Kommando-Kodierung, Phasen und `action_scale` (Kern liest sie zur Laufzeit) | Apache-2.0 |
| `robot_allcollisions_backlash.xml` | github.com/pollen-robotics/microduck_rl @ `b02fdea`, `src/mjlab_microduck/robot/microduck/`: das MJCF, auf dem die Policy trainiert wurde | Apache-2.0 (`LICENSE`) |
| `robot_groundcontact_rollers_backlash.xml` | dieselbe Quelle: das Rollen-Modell (Kufen und vier freie Räder), auf dem `roller.onnx` und `roller_crouch.onnx` trainiert wurden | Apache-2.0 (`LICENSE`) |

Copyright Pollen Robotics, alles hier unter der Apache License 2.0 (`LICENSE`).

Pollens 3D-Modelle (STL-Meshes aus microduck_rl, CC BY-NC-SA 4.0, nicht kommerziell) sind **nicht**
Teil von Kern. Aussehen und Kollisionsformen der Ente gestaltet Kern selbst (`tools/kern_duck.py`
→ `src/game/duck_meshes.inc`). Übernommen sind nur Messdaten: die Gliedrahmen aus dem MJCF und grobe
Kollisionshüllen (11–40 Punkte je Teil, `tools/kern_duck_envelopes.json`).

Neu erzeugen (aus dem Repository-Root, mit einem Checkout von microduck_rl für die STL-Maße):

```
python3 -I tools/micro_duck_import.py kern/third_party/microduck/robot_allcollisions_backlash.xml \
    <microduck_rl>/src/mjlab_microduck/robot/microduck/assets kern/third_party/microduck/velstand.onnx kern/src/game
python3 -I tools/kern_duck.py
```

Ergebnis: `src/game/micro_duck_model.inc` (Glieder, Gelenke, Massen, Trägheiten, Sensoren aus dem
MJCF) und `src/game/micro_duck_velstand.inc`. Aus den STL-Dateien liest das Werkzeug nur Maße
(Ausdehnung, tiefster Punkt, Masseverteilung des Schnabels); keine Geometrie wird übernommen. Danach
erzeugt `tools/kern_duck.py` Kerns eigene Meshes und die Kollider aus den gemessenen Hüllen
(braucht numpy).
