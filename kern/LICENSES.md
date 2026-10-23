# Third-party code in Kern

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
`--world` or `--editor`.

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

## Micro Duck idle look-around (port in `kern/src/game/walker_brain.cpp`)

The idle look-around (`GameWorld::WalkerBrain::Idle`) is a C++ port of `robotd/src/idle_head.rs`
and the `[idle]` defaults of `robotd-params` from Pollen Robotics' microduck
(https://github.com/pollen-robotics/microduck, commit 942e58e), licensed under the Apache License,
Version 2.0 (https://www.apache.org/licenses/LICENSE-2.0). Changes: translated to C++, ticked from
Kern's control step instead of the wall clock, seeded from the actor id, the beak and the breath
computed but not applied.

## Test data

`tests/fixtures/konstanz-osm-sample.json` contains OpenStreetMap data,
© OpenStreetMap contributors, available under the Open Database License (ODbL 1.0).
