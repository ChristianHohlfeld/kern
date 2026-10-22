# Box3D (vendored)

Erin Catto's [Box3D](https://github.com/erincatto/box3d) physics engine, MIT license (see `LICENSE`).

- Upstream commit: `51f056e0a9d299326b10f10a63270c81d901df21` (2026-10-02, "SAT benchmark (#177)"),
  the same revision the previous Editor3D used through box3d.js.
- Only `include/box3d/*.h` and `src/*.{c,h,inl}` are copied, unmodified.
- Kern compiles it as C17 with wasi-sdk (`kern/build.sh --physics`) in scalar mode
  (no SSE/NEON on wasm32, single worker, no threads).
- Kern code uses it only through `kern/physics/physics.h`.
