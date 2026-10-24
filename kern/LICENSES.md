# Third-party code in Kern

Kern itself is proprietary: see `LICENSE` at the repository root (all rights reserved). The
components below are not covered by it; each stays under its own license.

## Earcut (C++ port in `kern/src/geo/earcut.cpp`)

Port of Mapbox Earcut v3.0.2 (https://github.com/mapbox/earcut).

```
ISC License

Copyright (c) 2024, Mapbox

Permission to use, copy, modify, and/or distribute this software for any purpose
with or without fee is hereby granted, provided that the above copyright notice
and this permission notice appear in all copies.

THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH
REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND
FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT,
INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS
OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF
THIS SOFTWARE.
```

## Box3D (`kern/third_party/box3d`)

Box3D by Erin Catto (https://github.com/erincatto/box3d), vendored at commit
51f056e0 and compiled unchanged as C17 into modules built with `--physics`,
`--game`, `--studio` or `--lehrer`.

```
MIT License

Copyright (c) 2026 Erin Catto

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## musl math (`kern/third_party/musl_math`)

The deterministic libm functions (sin, cos, exp, log, pow, atan2, ...) are taken from musl
(https://musl.libc.org), Copyright © 2005-2020 Rich Felker, et al., MIT License; the full text and
the per-file notices are in `kern/third_party/musl_math/COPYRIGHT`.

## Micro Duck idle look-around (port in `kern/src/game/walker_brain.cpp`)

The idle look-around (`GameWorld::WalkerBrain::Idle`) is a C++ port of `robotd/src/idle_head.rs`
and the `[idle]` defaults of `robotd-params` from Pollen Robotics' microduck
(https://github.com/pollen-robotics/microduck, commit 942e58e), licensed under the Apache License,
Version 2.0 (https://www.apache.org/licenses/LICENSE-2.0). Changes: translated to C++, ticked from
Kern's control step instead of the wall clock, seeded from the actor id, the beak and the breath
computed but not applied.

## Micro Duck autonomous brain (port in `kern/src/game/walker_brain.cpp`)

The autonomous behaviour (`GameWorld::think_walker`, `GameWorld::WalkerBrain::Brain`) is a C++ port
of `autod/src/brain.rs` and `autod/src/obstacle.rs` from Pollen Robotics' microduck
(https://github.com/pollen-robotics/microduck, branch `autonomous-mode`, commit df9a750, author
Antoine Pirrone, 2026-10-01), Copyright Pollen Robotics, licensed under the Apache License,
Version 2.0 (https://www.apache.org/licenses/LICENSE-2.0). Changes: translated to C++, ticked from
Kern's control step, seeded from the actor id; the obstacle model reads Kern's simulated ToF frame
instead of `kinematics::tof::Reprojector`; hearing and petting come from API calls (Kern has no
microphone or touch sensor). Added by Kern, not part of the original: ball play and sock fetching
(the Fetch / Kick / Carry states).

## Micro Duck voice (port in `kern/src/audio/duck_voice.cpp`)

The duck's voice synthesiser is a C++ port of the `sounds` crate (lib.rs, personality.rs, rng.rs,
synth.rs, voices.rs) of Pollen Robotics' microduck (https://github.com/pollen-robotics/microduck,
commit 942e58e), itself a port of apirrone/microduck_sounds, licensed under the Apache License,
Version 2.0. When the duck speaks is ported from robotd/src/sound.rs and main.rs of the same
repository. Changes: translated to C++, played through Kern's mixer.

## Micro Duck meshes (`kern/third_party/microduck/micro_duck_meshes.inc`)

The Micro Duck's 3D model (STL meshes from https://github.com/pollen-robotics/microduck_rl,
`src/mjlab_microduck/robot/microduck/assets`), Copyright Pollen Robotics, licensed under the
Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International License (CC BY-NC-SA 4.0,
https://creativecommons.org/licenses/by-nc-sa/4.0/). Modified: simplified, merged per link, posed,
normals recomputed, quantised (tools/micro_duck_import.py). This adaptation is licensed under
CC BY-NC-SA 4.0 as well. Not for commercial use.

## Test data

`tests/fixtures/konstanz-osm-sample.json` contains OpenStreetMap data,
© OpenStreetMap contributors, available under the Open Database License (ODbL 1.0).
