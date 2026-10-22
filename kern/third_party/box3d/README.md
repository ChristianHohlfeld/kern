# Box3D (vendored)

Erin Catto's [Box3D](https://github.com/erincatto/box3d) physics engine, MIT license (see `LICENSE`).

- Upstream commit: `f36d061ba26998c87cb7dc593a4065aaa03fb7eb` (2026-10-20, "CMake improvements (#201)").
- Only `include/box3d/*.h` and `src/*.{c,h,inl}` are copied, unmodified.
- Kern compiles it as C17 with wasi-sdk (`kern/build.sh --physics`) in scalar mode
  (`-DBOX3D_DISABLE_SIMD`, `simd_scalar.h`; no SSE/NEON on wasm32, single worker, no threads)
  and with `-ffp-contract=off`, so results do not depend on FMA contraction.
- Kern code uses it only through `kern/physics/physics.h`.
