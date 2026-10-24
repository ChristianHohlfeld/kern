# Third-party code in Kern

Kern itself is licensed under the Apache License 2.0 (`LICENSE`, `LICENSING.md` at the repository
root). The components below are not Kern's; each stays under its own license. A summary is in
`NOTICE`.

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

## Plumo idle look-around (port in `kern/src/game/walker_brain.cpp`)

The idle look-around (`GameWorld::WalkerBrain::Idle`) is a C++ port of `robotd/src/idle_head.rs`
and the `[idle]` defaults of `robotd-params` from Pollen Robotics' robot repository
(https://github.com/pollen-robotics/microduck, commit 942e58e), licensed under the Apache License,
Version 2.0 (https://www.apache.org/licenses/LICENSE-2.0). Changes: translated to C++, ticked from
Kern's control step instead of the wall clock, seeded from the actor id, the beak and the breath
computed but not applied.

## Plumo autonomous brain (port in `kern/src/game/walker_brain.cpp`)

The autonomous behaviour (`GameWorld::think_walker`, `GameWorld::WalkerBrain::Brain`) is a C++ port
of `autod/src/brain.rs` and `autod/src/obstacle.rs` from Pollen Robotics' robot repository
(https://github.com/pollen-robotics/microduck, branch `autonomous-mode`, commit df9a750, author
Antoine Pirrone, 2026-10-01), Copyright Pollen Robotics, licensed under the Apache License,
Version 2.0 (https://www.apache.org/licenses/LICENSE-2.0). Changes: translated to C++, ticked from
Kern's control step, seeded from the actor id; the obstacle model reads Kern's simulated ToF frame
instead of `kinematics::tof::Reprojector`; hearing and petting come from API calls (Kern has no
microphone or touch sensor). Added by Kern, not part of the original: ball play and sock fetching
(the Fetch / Kick / Carry states).

## Plumo voice (port in `kern/src/audio/duck_voice.cpp`)

The duck's voice synthesiser is a C++ port of the `sounds` crate (lib.rs, personality.rs, rng.rs,
synth.rs, voices.rs) of Pollen Robotics' microduck (https://github.com/pollen-robotics/microduck,
commit 942e58e), itself a port of apirrone/microduck_sounds, licensed under the Apache License,
Version 2.0. When the duck speaks is ported from robotd/src/sound.rs and main.rs of the same
repository. Changes: translated to C++, played through Kern's mixer.

## Plumo model, policies and pick-up detector (`kern/third_party/pollen`)

The robot description (MJCF: `robot_allcollisions_backlash.xml`,
`robot_groundcontact_rollers_backlash.xml`, from https://github.com/pollen-robotics/microduck_rl),
the control policies (`*.onnx`, from https://huggingface.co/pollen-robotics/microduck-policies),
their `manifest.json` and the pick-up detector (`pickup_detector.onnx`, embedded as
`pickup_detector.inc`, from https://github.com/pollen-robotics/microduck), Copyright Pollen
Robotics, licensed under the Apache License, Version 2.0; the full text is in
`kern/third_party/pollen/LICENSE`. Kern reads the MJCF's bodies, joints, masses and inertias
(converted by `tools/plumo_import.py` into `kern/src/game/plumo_model.inc` and
`plumo_velstand.inc`) and runs the policies unchanged. Details and checksums:
`kern/third_party/pollen/README.md`.

## Plumo's look and colliders

The look is Kern's own design (`tools/kern_duck.py` → `kern/src/game/duck_meshes.inc`), licensed
like Kern. The colliders (`kern/src/game/duck_colliders.inc`) come from measurement data in
`tools/kern_duck_envelopes.json`: coarse convex hulls (11–40 points each) of how far the real
robot's colliding parts reach, so that the duck touches the world as in training. Kern does not
contain Pollen Robotics' 3D model files (CC BY-NC-SA 4.0) or meshes derived from them.
