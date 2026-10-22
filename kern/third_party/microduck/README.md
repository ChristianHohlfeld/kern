# Micro Duck: Modell und Laufpolicy (Pollen Robotics)

Quellen für Kerns Micro Duck (`src/game/walker.cpp`, Doku: `docs/walker.md`).

| Datei | Herkunft | Lizenz |
|---|---|---|
| `velstand.onnx` | huggingface.co/pollen-robotics/microduck-policies, Branch `main` (v8, Training-Run `pollen-robotics/mjlab_microduck/0045i2si`, Checkpoint 2749), SHA-256 `2fc7f8ee6acd7d0d3f66a99927b5d5a56589aeed02ec714de329b252d5ccf492` | Apache-2.0 |
| `manifest.json` | dasselbe Repository | Apache-2.0 |
| `robot_allcollisions_backlash.xml` | github.com/pollen-robotics/microduck_rl @ `b02fdea`, `src/mjlab_microduck/robot/microduck/`: das MJCF, auf dem die Policy trainiert wurde | Apache-2.0 (`LICENSE`) |

Copyright Pollen Robotics. Die STL-Meshes des Roboters stehen unter CC BY-NC-SA 4.0 und liegen
**nicht** in Kern. Der Import liest aus ihnen nur die Außenmaße der kollidierenden Teile, also
Servogehäuse und Sohlen.

Neu erzeugen (aus dem Repository-Root, mit einem Checkout von microduck_rl für die STL-Maße):

```
python3 -I tools/micro_duck_import.py kern/third_party/microduck/robot_allcollisions_backlash.xml \
    <microduck_rl>/src/mjlab_microduck/robot/microduck/assets kern/third_party/microduck/velstand.onnx kern/src/game
```

Ergebnis: `src/game/micro_duck_model.inc` und `src/game/micro_duck_velstand.inc`.
