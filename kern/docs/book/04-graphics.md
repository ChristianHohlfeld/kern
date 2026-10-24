# Chapter 4 – Graphics

This chapter describes Kern's renderer in full: how the C++ code in the WebAssembly module reaches WebGL2, how meshes and materials are structured, what the single standard shader computes, in which order a frame is produced and how the additional passes (sun shadows, sky, GTAO, bloom) work in detail. It ends with a complete example of a custom shader extension from the game Orb Run.

The chapter assumes general background knowledge – rasterization, coordinate spaces and matrices, the shader pipeline, the depth buffer, the basic idea of PBR, gamma and tonemapping. These are covered in [Appendix B](B-rendering.md). Here the focus is on what Kern concretely makes of them.

![Kern Studio with Orb Run: procedural city, sun shadows, sky and bloom](../buch/bilder/studio.png)

## 4.1 Overview

The renderer consists of only a few parts:

| Part | File | Task |
|---|---|---|
| WebGL2 imports | `include/kern/gfx/gl.h`, `web/kern.mjs` | exactly the GL functions the engine needs, as WebAssembly imports |
| Program and uniform filter | `gfx/program.h`, `src/gfx/program.cpp` | compile and link shaders, avoid redundant `uniform*` calls |
| Standard shader | `gfx/shader.h`, `src/gfx/standard_shader.cpp` | one program for all meshes, extensible through `ShaderExtension` |
| Mesh and material | `gfx/mesh.h`, `src/gfx/mesh.cpp` | CPU copy of the geometry, GPU buffers, material |
| Renderer | `gfx/renderer.h`, `src/gfx/renderer.cpp` | manage meshes, draw the frame, picking, context loss |
| Passes | `gfx/passes.h`, `src/gfx/shadow.cpp`, `sky.cpp`, `bloom.cpp`, `gtao.cpp` | sun shadows, sky, bloom/composite, ambient occlusion |
| Particles and lights | `gfx/particles.h`, `src/gfx/particles.cpp` | billboard particles as one instanced mesh, selection of point lights |

Three principles shape the design:

1. **One program for everything.** Every mesh in the scene is drawn with the same standard shader. There are no program switches within the scene pass. Games change the look through shader extensions that are inserted into this one program, not through programs of their own.
2. **Upload once, then only movement.** A mesh is uploaded once with `upsert`. After that, usually only its instance streams change (`update_instances`). The per-frame CPU cost follows what moves, not the size of the scene.
3. **The CPU copy is the source of truth.** The renderer keeps the positions, normals, colors, UVs, indices and instances of every mesh in memory. From these come the bounds for culling, the BVH for picking and, after a context loss, the complete re-upload to the GPU.

## 4.2 The WebGL2 Binding

### Imports Instead of Emscripten

Kern is compiled to WebAssembly with wasi-sdk, without Emscripten and without generated JavaScript glue. The GL functions are therefore ordinary WebAssembly imports from the module `kern_gl`. `gl.h` declares them with a macro:

```cpp
#if defined(__wasm__)
#define KERN_GL(name) __attribute__((import_module("kern_gl"), import_name(#name)))
#else
#define KERN_GL(name)
#endif

KERN_GL(drawElementsInstanced) void drawElementsInstanced(u32 mode, i32 count, u32 type, i32 offset, i32 instances);
```

Outside WebAssembly (native tests) the macro stays empty, and the tests link a recording GL stub (`tests/gl_stub.cpp`).

According to its comment, the list in `gl.h` is "exactly what the engine uses". Anyone who needs a new GL call adds it in two places: in `gl.h` and in the `kern_gl` import object of `web/kern.mjs`. The enum values (`gl::TRIANGLES`, `gl::RGBA16F`, `gl::TEXTURE_COMPARE_MODE` …) are also in `gl.h`, as WebGL2 numbers.

### Handles, Pointers and Lengths

Only numbers cross the boundary between WebAssembly and JavaScript. Two rules follow from this:

- **GPU objects are `u32` handles**, with `0` meaning null. `KernModule` in `kern.mjs` keeps an `objects` table with a free list for them: `createBuffer()` puts the real `WebGLBuffer` into a slot and returns the slot number, `deleteBuffer()` frees the slot. Uniform locations get their own table, `locations`; `getUniformLocation` returns `0` if the uniform is not active.
- **Data is a pointer into linear memory plus a byte count.** `bufferData(target, ptr, bytes, usage)` calls `gl.bufferData(target, u8, usage, ptr, bytes)` on the JavaScript side with a `Uint8Array` view of the module memory, without copying the data. `ptr == 0` only reserves memory. For `texImage2D`, the platform layer chooses the matching view based on the GL type (`f32` for `FLOAT`, `u16` for `HALF_FLOAT`/`UNSIGNED_SHORT`, `u32` for `UNSIGNED_INT`, otherwise `u8`).

`refresh()` renews the memory views (`u8`, `u32`, `f32` …) as soon as `memory.buffer` changes due to memory growth. Strings (shader source, uniform names) arrive as pointer and length and are read with `TextDecoder`.

Two small details show how deliberately thin this layer is: `drawBuffers` caches the JavaScript arrays per combination of values, so that the call does not allocate every frame. `getExtension("EXT_disjoint_timer_query_webgl2")` remembers the extension object so that `gpuDisjoint()` can later query `GPU_DISJOINT_EXT`.

### What Else `kern.mjs` Does

According to its header comment, `web/kern.mjs` performs exactly three tasks and contains no engine logic:

1. instantiate the module with the imports `kern_gl`, `kern_env` and a minimal WASI shim,
2. own the handle tables and the context-loss machinery,
3. offer `KernRenderer`, a facade that translates typed arrays into the C ABI (`include/kern/api/c_api.h`).

`kern_env` provides, among other things, `now` (`performance.now()`), `terrain_height` (calls `kern.heightSampler(x, y)` if the page has set one), `status`, `pointer_lock`, `log` and the JIT path of the logic graphs (see [Chapter 9](09-logic.md)). The WASI shim knows `fd_write` (output to the console), `random_get`, `clock_time_get`, `proc_exit`, `fd_close` and `fd_seek`; everything else responds with `ENOSYS`. The engine never touches files.

The page creates the WebGL2 context, not the engine. `KernRenderer` and `web/app.mjs` request it with the same attributes:

```js
canvas.getContext("webgl2", { alpha: false, antialias: false, depth: true,
                              powerPreference: "high-performance", ... });
```

`antialias: false` is intentional: the renderer draws the scene into its own framebuffers anyway (see 4.7), where multisampling of the default framebuffer would have no effect. Kern Studio and exported games create their context themselves and request `antialias: true` (`web/studio/studio.mjs`, `web/studio/game.mjs`).

### Two Ways to Use the Renderer

- **C++ games** write a `kern::App` (`include/kern/app/app.h`). `web/app.mjs` (`runKernApp`) creates the context, forwards input and calls `kern_app_frame(now, hidden)` every frame. Inside, `App::update()` runs first, then `Renderer::render()`. The `examples/hello` example and Orb Run work this way; details in [Chapter 12](12-your-own-games.md).
- **JavaScript hosts** (such as SpawnCity) use `KernRenderer` from `kern.mjs`. The facade writes camera, light and mesh descriptors into fixed word layouts (`KERN_MD_*`, `KERN_LI_*`, `KERN_RP_*` from `c_api.h`) and calls the exported C functions. There is exactly one `KernRenderer` per module ("one KernRenderer per module").

Kern Studio and exported games use a third C ABI of their own (`kern_studio_*`, see [Chapter 11](11-studio.md)); internally it is the same `Renderer`.

## 4.3 Programs and the Uniform Cache

### `Program`

`Program::build(vertex, fragment, label)` compiles both stages, links them and deletes the shader objects immediately afterwards. If something fails, the driver log (up to 767 characters) ends up via `fail()` as the module's last error, prefixed with the `label`, e.g. `"standard fragment shader: ..."`. `location(name)` returns `0` for a uniform that is not active in the linked program – and all setters ignore location `0`. A uniform that the compiler optimized away therefore costs nothing and produces no error.

`destroy(contextLost)` distinguishes two cases: with a live context, `deleteProgram` is called; after a context loss the handles are already dead and are simply forgotten.

### `UniformCache`

In GL, uniform values belong to the program state. A value once set therefore stays in place, even across frames and program switches. `UniformCache` exploits this: it remembers the last value sent per location and skips the `gl.uniform*` call if nothing has changed.

```cpp
void UniformCache::set_float(u32 location, f32 v) {
  if (!location) return;
  Entry& e = entry(location);
  if (e.kind == 2 && e.count == 1 && e.f[0] == v) return;
  e.kind = 2; e.count = 1; e.f[0] = v;
  gl::uniform1f(location, v);
}
```

Scalars and vectors are compared directly. Arrays and matrices (`set_vec3_array`, `set_vec4_array`, `set_mat4`) are compared via a 64-bit hash of their contents (`hash_bytes`), so that not every entry has to store the whole array. Bone palettes deliberately go uncached through `set_mat4_array`: for animated characters they change every frame anyway.

Why is this worth it? In the browser, every `uniform*` call is a transition from WebAssembly to JavaScript and then into the WebGL implementation, which validates the call. With hundreds of meshes, often with identical materials, the cache saves most of these calls. `reset()` is only needed when the program has been relinked.

## 4.4 Mesh, Material and MeshInput

### Fixed Vertex Attributes

The standard shader reads its vertex streams at fixed locations (`namespace attr` in `mesh.h`):

| Location | Name in shader | Contents | Per |
|---|---|---|---|
| 0 | `aPosition` | position xyz | vertex |
| 1 | `aNormal` | normal xyz | vertex |
| 2 | `aColor` | color rgb | vertex |
| 3 | `aUV` | texture coordinate uv | vertex |
| 4–7 | `iModel0..3` | columns of the model matrix | instance |
| 8 | `iTint` | tint rgba | instance |
| 9–11 | `iNormal0..2` | columns of the normal matrix (3×3) | instance |
| 12 | `aJoints` | 4 bone indices | vertex |
| 13 | `aWeights` | 4 weights | vertex |
| 14, 15 | free | streams of shader extensions | vertex |

The instance attributes have `vertexAttribDivisor = 1`. Every mesh is therefore drawn with exactly one `drawElementsInstanced` or `drawArraysInstanced`, even if it has only one instance. The renderer computes the normal matrix (inverse transpose of the 3×3 part, see [Appendix B](B-rendering.md)) per instance on the CPU (`normal_matrix3`). A singular matrix is rejected (`"singular world transform"`).

Further limits from `mesh.h`: at most 32 bones (`kMaxBones`), 6 point lights (`kMaxPointLights`), 2 extension streams (`kMaxExtensionAttributes`), 8 integer material parameters for extensions (`kMaxMaterialParams`).

### Material

```cpp
struct Material {
  Vec4 color{1, 1, 1, 1};
  AlphaMode alpha = AlphaMode::opaque;   // opaque, mask, blend
  f32 alpha_cutoff = 0.5f;
  f32 roughness = 1.0f;
  f32 metalness = 0.0f;
  Vec3 emissive{0, 0, 0};      // linear HDR, added after lighting (feeds bloom)
  bool unlit = false;
  bool double_sided = false;
  bool cast_shadow = true;
  bool frustum_culled = true;
  bool viewmodel = false;      // first-person layer: drawn last, after a depth clear
  bool linear_colors = false;  // vertex colors are linear (no sRGB decode)
  bool lines = false;          // line list, always unlit, no shadow, not pickable
  i32 params[kMaxMaterialParams] = {};  // belong to shader extensions
  bool casts_shadow() const { return cast_shadow && alpha == AlphaMode::opaque && !lines; }
};
```

Some fields need explanation:

- **Colors are sRGB.** The shader multiplies vertex color, instance tint, `color.rgb` and texel and then converts the product from sRGB to linear (`srgbToLinear`). The conversion is skipped only when `linear_colors` is set. `emissive`, by contrast, is already linear.
- **Only opaque materials cast shadows** (`casts_shadow()`). Alpha-mask and blend materials never end up in the shadow map.
- **`viewmodel`** belongs to the first-person layer: these meshes are drawn last, after the depth buffer has been cleared. A wall in front of the camera then does not cut off a weapon. Kern Studio also draws its gizmos on this layer.
- **`lines`** turns the mesh into a line list (`gl::LINES`, pairs of vertices). The renderer then sets `unlit = true` and `cast_shadow = false` by itself; line meshes cannot be skinned and are skipped during picking.

The face-culling rule is in `Mesh::cull_mode()`: off for `double_sided`, `lines` or mixed mirrored instances; back-face culling in the normal case; front-face culling if all instances are mirrored (negative determinant). Mirroring reverses the winding order; a mixed mesh cannot be culled correctly in a single draw.

### MeshInput and `upsert`

`MeshInput` is a borrowed view of the caller's data. The spans only need to be valid during the call; the renderer copies everything.

| Field | Required | Meaning |
|---|---|---|
| `positions` | yes | xyz per vertex |
| `normals` | no | if omitted: flat face normals for a triangle soup, otherwise +Z |
| `colors` | no | rgb per vertex, default white |
| `uvs` | no | uv per vertex, default 0 |
| `indices` | no | triangle list (or line pairs) |
| `joints`, `weights`, `bones` | no | skinning: 4 per vertex, palette with 16 floats per bone |
| `extension[2]` | no | streams of the shader extensions in declaration order |
| `transforms` | no | instance matrices, 16 floats; default one identity matrix |
| `tints` | no | rgba per instance, default white |
| `material`, `texture`, `visible`, `layers` | – | material, texture handle, visibility, layer mask |

`upsert(id, input)` creates mesh `id` or replaces it **atomically**. Everything is validated first, before anything changes:

- Positions are triples, all values are finite; without indices the vertex count is divisible by 3 (or 2 for lines).
- Every index lies within the vertex count.
- `normals`/`colors` have as many floats as `positions`, `uvs` two per vertex, extension streams `size` per vertex.
- For skinning: exactly 4 joints and 4 weights per vertex, at most 32 bones, integer joint indices within the palette, weights in [0, 1] summing to 1 (tolerance 0.001).
- Instance matrices finite and non-singular.

Only then does the renderer build the new mesh completely (bounds, possibly a BVH, GPU buffers) and swap it with the old one. If validation fails, the previous mesh remains visible unchanged. This matters for editors: a faulty intermediate state deletes nothing.

Mesh ids come from `allocate_id()`; `upsert` only accepts ids allocated this way. Two owners can therefore never share a slot. If the first `upsert` fails, `release_id()` returns the id. `remove()` frees it and recycles it.

### What Happens During Upload

`Renderer::upload()` creates one vertex array object per mesh and fills:

- static buffers (`STATIC_DRAW`) for positions, normals, colors, UVs and, where present, joints, weights and extension streams,
- three dynamic instance buffers (`DYNAMIC_DRAW`) for model matrices, tints and normal matrices,
- an index buffer. **If the mesh has at most 65,536 vertices, the indices are uploaded as 16-bit**, otherwise as 32-bit. This halves index bandwidth in the common case.

For static world meshes the renderer additionally builds a BVH for picking: if the mesh contains no lines, has exactly one instance with an identity matrix, is not skinned and has at least 36 triangles (`kBvhMinTriangles`). More on this in [Chapter 5](05-scene-and-geometry.md).

### Updating Instances

`update_instances(id, transforms, tints, &changed)` is the path for everything that moves:

- Empty `transforms` mean "keep the instances": a pure tint update. Then only the tint buffer is uploaded; bounds, BVH and shadow map stay valid.
- If `tints` do not have exactly 4 floats per instance, all instances are treated as white.
- If transforms and tints are byte-identical to the state in the renderer (`memcmp`), nothing happens at all, and `changed` stays `false`.
- If the instance count grows beyond the buffer capacity, the capacity is enlarged **geometrically** (at least doubled). If the number of instances fluctuates, this avoids repeated reallocations. Then `bufferSubData` uploads only the bytes in use.
- The scratch vectors are swapped with those of the mesh (`swap`). In steady state, an update does not allocate.

Further setters: `update_bones` (same bone count as at creation), `set_visible`, `set_material` (the primitive type line/triangle must not change), `set_layers`. Every change that affects a shadow caster increments the shadow revision; more on that in 4.8.

### Layers

Every mesh has a bit mask `layers` (default 1). `RenderParams::layers` determines which bits are drawn – and which meshes cast shadows. `PickQuery::layers` filters picking the same way. Kern Studio uses this for the grid, icons, per-viewport gizmos, collider display and the worlds of multiple network clients (`layer` in `include/kern/studio/studio.h`, set in `src/studio/viewport.cpp`).

### Textures

- `create_texture(TextureDesc)` creates an engine-owned RGBA8 texture. The renderer keeps the pixels as a CPU copy and re-uploads them after a context loss. Options: `mipmaps` (on by default, then `LINEAR_MIPMAP_LINEAR`), `repeat` (otherwise `CLAMP_TO_EDGE`), `nearest` (pixel look, without mipmaps).
- `register_external_texture(gl_handle)` registers a texture whose GPU object is owned by the host. This is how `KernRenderer.createTexture(source)` works: it uploads an image, canvas or `ImageData` directly through WebGL and stores a restore function that refills the same handle number after a context loss (`kern.pinned`).

A mesh has at most one albedo texture (`MeshInput::texture`). It is bound to unit 0; the shadow map is on unit 1.

## 4.5 The Standard Shader

`build_standard_shader(extensions)` assembles the vertex and fragment shaders from fixed GLSL building blocks and inserts the extensions' snippets at defined points. The result is exactly one program, which the renderer links in `init()`.

### Structure of the Vertex Shader

1. `kVertexHead`: `#version 300 es`, the fixed attributes 0–13.
2. One line `layout(location=N) in vecK aName;` per extension attribute.
3. `kVertexUniforms`: `uSkinEnabled`, `uBones[32]`, `uViewProjection`, the outputs `vWorld`, `vNormal`, `vColor`, `vUV`, `vOpacity`.
4. Hook `vertex_declarations`.
5. `kVertexMainHead`: model matrix from the instance columns, skinning, world position, normal.
6. Hook `vertex`.
7. `gl_Position = uViewProjection * p;`

Skinning is classic linear blend skinning:

```glsl
if (uSkinEnabled == 1) {
  mat4 skin = aWeights.x * uBones[int(aJoints.x)] + aWeights.y * uBones[int(aJoints.y)]
            + aWeights.z * uBones[int(aJoints.z)] + aWeights.w * uBones[int(aJoints.w)];
  local = skin * local;
  localNormal = mat3(skin) * localNormal;
}
vec4 p = model * local;
vWorld = p.xyz;
vNormal = normalize(mat3(iNormal0, iNormal1, iNormal2) * localNormal);
vColor = aColor * iTint.rgb;
vOpacity = iTint.a;
```

### Structure of the Fragment Shader

1. `kFragmentHead`: inputs, all uniforms and **three outputs**:
   - `location 0` `outColor`: the finished LDR image (tonemapped, sRGB, with fog),
   - `location 1` `outHdr`: the linear HDR color before tonemapping (for bloom),
   - `location 2` `outAo`: a record for ambient occlusion (see 4.8.4).
2. `kGlslToneMap`: `linearToSrgb`, `srgbToLinear`, ACES filmic, color grading.
3. `kFragmentFunctions`: `envIrradiance`, `skyReflection`.
4. Hook `fragment_declarations`.
5. Read the texture, compute alpha, alpha test.
6. Hook `fragment_discard`.
7. `bool linearInput = uLinearColor == 1;` and hook `fragment_input`.
8. `kFragmentBase`: linearize the base color, normal, sun direction, `surfaceGlass = 0`, `emissive = uEmissive`.
9. Hook `fragment_surface`.
10. `kFragmentLighting`: lighting, tonemapping, fog, outputs.

Alpha is computed as `clamp(vOpacity * uMaterial.a * texel.a, 0, 1)`. For `AlphaMode::mask` the renderer sets `uAlphaCutoff` to `alpha_cutoff`, otherwise to 0; anything below is discarded.

### The Lighting and Material Model

The model is based on three.js's `MeshStandardMaterial`; the code explicitly refers to three.js in several places (ACES fit r16x, FogExp2 "as three.js applies it", "unchanged look" with the atmosphere turned off). All quantities are linear; `base` is the linearized base color, `n` the normal, `sun` the normalized sun direction (pointing toward the sun), `rough = clamp(roughness, 0.04, 1)`, `metal = clamp(metalness, 0, 1)`.

**Direct sunlight (Lambert)**

```
ndl    = max(dot(n, sun), 0)
shadow = PCF result of the shadow map (1 = lit)
```

**Hemisphere light.** A mix of ground and sky color according to the inclination of the normal (Z is up):

```
hemis = mix(uGroundColor, uSkyColor, clamp(n.z * 0.5 + 0.5, 0, 1))
```

**Sky irradiance.** An approximation of the prefiltered environment of a dome gradient over a ground disc:

```glsl
vec3 envIrradiance(vec3 n) {
  vec3 sky = mix(uEnvHorizon, uEnvZenith, 0.35);
  float w = clamp(n.z * 0.5 + 0.5, 0.0, 1.0);
  return mix(uEnvGround, sky, smoothstep(0.0, 1.0, w));
}
```

**Point lights.** Up to 6, with a smooth falloff to zero at the radius:

```
d2          = max(|L - P|², 0.04)
radius      = max(0.1, range)
attenuation = (max(0, 1 - d2/radius²))² / (1 + d2/radius²)
point      += color * (intensity * 0.12) * attenuation * max(dot(n, normalize(L - P)), 0)
```

**Composition of the diffuse term.** Indirect light is tracked separately, because ambient occlusion may later darken only this part; the sun and point lights have their own shadowing:

```
indirect = base * (1 - metal) * (hemis / π + envIrradiance(n) * uEnvIntensity)
rgb      = base * (1 - metal) * (sunColor * ndl * shadow + point * π) / π + indirect
```

With `unlit`, `rgb = base` and `indirect = 0`.

**Specular term.** Only if `metal > 0` or `rough < 0.95`:

```
V   = normalize(eye - P),  R = reflect(-V, n),  NdV = clamp(dot(n, V), 0, 1)
F0  = mix(0.04, base, metal)
F   = F0 + (max(1 - rough, F0) - F0) * (1 - NdV)^5          // Schlick with roughness
envR = mix(envIrradiance(R), skyReflection(R, 1), 1 - rough)
rgb      += F * envR * uEnvIntensity
indirect += F * envR * uEnvIntensity

H   = normalize(sun + V),  a2 = rough⁴,  NdH = clamp(dot(n, H), 0, 1)
D   = a2 / (π * (NdH² * (a2 - 1) + 1)²)                    // GGX distribution
rgb += F * sunColor * ndl * shadow * min(D, 40) * 0.25
```

`skyReflection(r, s)` is simply `mix(uEnvHorizon, uEnvZenith, clamp(r.z, 0, 1) * s)`. The clamp `min(D, 40)` prevents extremely bright highlights on very smooth surfaces, which would otherwise show up as individual flickering pixels in the bloom.

**Glass.** An extension can set `surfaceGlass` (0..1). The surface then reflects the sky with a Fresnel term, so that panes look bright instead of black:

```
fr      = (1 - |dot(V, n)|)³
skyRefl = skyReflection(reflect(-V, n), 0.6) * uEnvIntensity
g       = surfaceGlass * (0.45 + 0.4 * fr)
add     = surfaceGlass * mix((0.05, 0.07, 0.09), (0.32, 0.4, 0.5), fr)
rgb     = mix(rgb, skyRefl, g) + add      // likewise for indirect
```

**Emission.** `rgb += emissive` – after lighting, linear and unbounded. Values above 1 later turn into bloom.

### From HDR Value to Pixel

```glsl
outHdr = vec4(max(rgb, vec3(0.0)), alpha);
rgb = acesFilmic(rgb);                       // exposure: c *= uExposure / 0.6
if (uGrade == 1) rgb = gradeColor(rgb);
rgb = linearToSrgb(clamp(rgb, 0.0, 1.0));
```

`acesFilmic` is three.js's (r16x) fit to the ACES RRT/ODT curve, including the factor `uExposure / 0.6`. `gradeColor` is Kern's production grading applied to the already tonemapped image: saturation ×1.14, a soft S-curve (22 %), warmer highlights and cooler shadows. In the scene shader, `RendererOptions::grade = false` turns it off; in the bloom composite, `BloomOptions::grade` decides (the C ABI sets both to the same value, `kern_renderer_init`).

### Fog

As in three.js, fog is applied **after** the sRGB conversion, as `FogExp2`:

```
dist    = |P - eye|
optical = uFogDensity * dist
fog     = 1 - exp(-optical²)
```

Three optional extensions in `Lighting` change this; if all are 0, the fog looks as in three.js:

- **Height falloff** (`fog_height_falloff = k`): the optical density is multiplied by the mean of `exp(-k z)` along the view ray,
  `optical *= (exp(-k z0) - exp(-k z1)) / (k (z1 - z0))` with `z0 = max(eye.z, 0)`, `z1 = max(P.z, 0)`.
  The haze thus lies dense near the ground and clears above the rooftops.
- **Sun scattering** (`fog_sun_scatter`): when looking toward the sun, the fog takes on the sun's color, weighted by `max(dot(view direction, sun), 0)^8`.
- **Horizon** (`fog_horizon`): acts in the sky pass, see 4.8.2.

### The AO Record

In the scene pass, the shader writes (when `uAoOut == 1`) into the third output:

```glsl
outAo = vec4(clamp(indirect / max(outHdr.rgb, 1e-4), 0.0, 1.0) * (1.0 - fog),
             dot(base, vec3(0.2126, 0.7152, 0.0722)));
```

That is, per color channel, the share of indirect light in the total light, already reduced by the fog, and in alpha the luminance of the albedo. Transparent meshes and the first-person layer write `(0, 0, 0, alpha)`, so they count as unoccluded. How this record is used is described in 4.8.4.

## 4.6 Shader Extensions

A `ShaderExtension` (`gfx/shader.h`) supplies GLSL snippets for fixed insertion points and, optionally, its own vertex streams, uniforms and material parameters. The engine stays unchanged; the game hands in its extensions at `Renderer::init()` (in an `App` via `App::extensions()`, in the C ABI via `kern_game_extensions`).

### The Insertion Points

| Hook | Stage | What is visible there and may be changed |
|---|---|---|
| `vertex_declarations` | vertex | own uniforms and `out` variables |
| `vertex` | vertex, after the standard varyings | may change `vColor`, `vOpacity`, `vUV`; sees `model`, `local`, `aColor`, `aUV`, `aNormal`, `iTint`, `iModel*` |
| `fragment_declarations` | fragment | own uniforms, `in` variables, functions |
| `fragment_discard` | fragment, early | may `discard`; sees `vWorld`, `alpha`, `texel` |
| `fragment_input` | fragment | may change `linearInput` |
| `fragment_surface` | fragment, before lighting | may change `base`, `alpha`, `surfaceGlass`, `emissive` or discard; sees `n`, `sun`, `uEye`, `vWorld`, `vUV` |

Each inserted snippet is preceded by a comment line `// --- <name>`, so that compiler errors in the log can be attributed to the right extension.

### Vertex Streams

`attributes()` returns a list of `ExtensionAttribute { key, size, glsl }`: `key` is the name under which hosts supply the stream (e.g. `"wall"`), `size` is 1 to 4 floats per vertex, `glsl` the attribute name in the shader. At most two streams are allowed across all extensions combined; they occupy locations 14 and 15 in declaration order. The renderer sets `attribute_base` accordingly.

Hosts supply the streams as follows:

- **C++:** `MeshInput::extension[i]`, where `i` is the index in `Renderer::extension_attributes()`. `MeshData::as_input(extra_slot)` puts the extra stream of a `MeshData` into slot `extra_slot`; a slot ≥ `kMaxExtensionAttributes` means "not supplied".
- **JavaScript:** `KernRenderer.upsert(id, { ..., [key]: Float32Array })` – the facade reads the keys from `kern_extension_attribute_key`.

A mesh without the stream is valid; the attribute is then disabled and yields the GL default value (0, 0, 0, 1) in the shader.

### Material Parameters and Uniforms

- `material_keys()` names integer material parameters. The renderer distributes them across `Material::params` (at most 8 in total) and sets `material_base`. For each draw it calls `material_uniforms(cache, material, material.params + material_base)`.
- `bind(program)` runs after every (re)link of the standard program, including after a context loss. This is where the extension resolves its uniform locations.
- `frame_uniforms(cache, renderer)` runs once per scene pass, after the engine's frame uniforms.

Extensions should set their uniforms through the provided `UniformCache`; the same savings then apply as for the engine itself.

## 4.7 The Sequence of a Frame

`Renderer::render(params, stats)` produces a frame. The steps in the code:

**0. Validate.** Before any GL state is touched, the renderer checks the viewport rectangle, the camera (`camera_valid`) and the view direction. A rejected frame leaves behind no bound framebuffer, no active scissor and no open timer query. Then the view, projection and view-projection matrices are computed, and from them the frustum (see [Chapter 5](05-scene-and-geometry.md)).

**1. Collect GPU time.** If `EXT_disjoint_timer_query_webgl2` is available, a `TIME_ELAPSED` query wraps the frame. The results arrive asynchronously a few frames later; at most 4 queries are in flight at the same time, and query objects are reused. If a result is invalidated by `GPU_DISJOINT`, it is discarded. `RenderStats::gpu_ms` is −1 as long as no measurement is available.

**2. Sun shadows.** If enabled, with a shadow distance the renderer first determines the map's focus and extent from the view frustum (4.8.1). The shadow map is redrawn only if its key has changed (4.8.1); otherwise the renderer sets `stats.shadow_reused`.

**3. Bind the target.** Without a viewport rectangle (`has_viewport == false`), `BloomPass::begin()` binds the scene target, provided bloom is enabled (`BloomOptions::enabled`) and supported; otherwise the renderer draws directly into the default framebuffer. With a rectangle, drawing goes directly into the default framebuffer using a scissor – without bloom and without ambient occlusion. Then the viewport is set and color and depth are cleared.

**4. Sort.** Every live mesh is checked: invisible, without GPU data or outside the layer mask → skipped. Non-skinned meshes with `frustum_culled` are tested against the frustum (`culled_meshes`). Skinned meshes are never rejected by bounds, because their pose can leave the bind-pose bounds. Then three lists: first-person layer, transparent (`AlphaMode::blend`), opaque. The sort key is the squared distance from the center of the world bounds to the camera.
   - Opaque: **front to back**. Near surfaces fill the depth buffer first; occluded fragments fail the early depth test and cost no shader time.
   - Transparent: **back to front**, so that alpha blending layers correctly.

**5. Opaque meshes.** Depth test `LEQUAL`, depth writes on, no blending, standard program, frame uniforms once (`set_frame_uniforms`), `uAoOut = 1`, then one draw per mesh (`draw_mesh`). Per draw, only material uniforms are set (through the cache), the texture is bound only when it changes and the cull state is switched only when it changes.

**6. Sky.** If `params.sky` is set and the camera is perspective: a full-screen triangle on the far plane that produces fragments only where nothing has been drawn yet (4.8.2).

**7. Transparent meshes.** Blending `SRC_ALPHA, ONE_MINUS_SRC_ALPHA`, depth test on, depth writes off, `uAoOut = 0`.

**8. Ambient occlusion.** GTAO computes from the depth texture of the scene target (4.8.4). If there are first-person meshes, this happens *before* them, because their depth clear would erase the world depth; afterwards the scene target is bound again.

**9. First-person layer.** `clear(DEPTH_BUFFER_BIT)`, then the viewmodel meshes.

**10. Composite.** `BloomPass::end()` extracts the bright parts, blurs them, applies ambient occlusion to the indirect part and writes the result into the default framebuffer (4.8.3).

**11. Statistics.** `RenderStats` is filled with draw calls, shadow draw calls, triangles, instances, culled and visible meshes, CPU time, GPU time, bloom state and whether AO was applied.

### Framebuffers and Textures of a Frame

| Target | Format | Size | Contents |
|---|---|---|---|
| Shadow map | `DEPTH_COMPONENT24`, compare mode `LEQUAL` | `shadow.resolution`² | depth as seen from the sun |
| Scene target, attachment 0 | `RGBA8` | full size | LDR image (tonemapped, sRGB, fog) |
| Scene target, attachment 1 | `RGBA16F` | full size | linear HDR before tonemapping |
| Scene target, attachment 2 | `RGBA8`, `NEAREST` | full size | AO record: indirect share rgb, albedo luminance |
| Scene target, depth | `DEPTH_COMPONENT24` texture, `NEAREST` | full size | scene depth, read by GTAO and the composite |
| Bloom a1, b1 | `RGBA16F`, `LINEAR` | 1/4 | bright-pass extract, blur ping-pong |
| Bloom a2, b2 | `RGBA16F`, `LINEAR` | 1/8 | second, wider blur stage |
| GTAO a, b | `RG16F`, `NEAREST` | half or full | (visibility, view depth), raw and filtered |

The three color attachments of the scene target together occupy 16 bytes per pixel (4 + 8 + 4). The code comments that this is intentional: it is the tile budget of tile-based mobile GPUs. That is why the AO record is RGBA8 and not floating point.

Because the sky must write to the same three outputs ("WebGL needs every draw buffer written"), its shader also writes `outHdr` and `outAo = 0`.

![Four viewports in Kern Studio: partial views draw directly into the default framebuffer with a scissor](../buch/bilder/studio-quad.png)

Kern Studio renders each viewport with its own `render()` call. A single full-screen viewport renders without a rectangle, so that bloom and AO run; in the quad view, every viewport has a rectangle and therefore neither bloom nor AO.

## 4.8 The Passes in Detail

### 4.8.1 Sun Shadows

`SunShadowMap` (`src/gfx/shadow.cpp`) is a single orthographic depth map. It follows the camera's view (shadow distance), snaps to its texel grid and is filtered in the standard shader with Castaño's optimised PCF. This keeps the shadows sharp and still when the camera moves.

**Options and limits** (`ShadowOptions`):

| Field | Default | Valid | Meaning |
|---|---|---|---|
| `resolution` | 512 | power of two 128–4096 | edge length of the map in texels |
| `extent` | 120 | 10–2000 | half edge length of the covered square in metres (without a shadow distance) |
| `bias` | 0.0012 | 0 ≤ bias < 0.04 | depth offset for the comparison |
| `filter` | 9 | 1, 4, 9 | PCF taps: 1 hardware tap (2 × 2), 4 taps (3 × 3 kernel), 9 taps (5 × 5 kernel) |

At runtime `Renderer::set_shadow_filter(taps)` sets the filter level. The Studio page chooses it by device (`defaultShadowFilter` in `web/studio/game.mjs`, via `kern_studio_shadow_filter`): phones (coarse pointer) 4, desktop 9; `?shadowFilter=1|4|9` in the address overrides this.

**Light basis.** From the sun direction `dir` an orthonormal basis `right`, `up` is built (helper vector +Z, or +Y when the sun is nearly vertical).

**Fitted to the view: the shadow distance.** `Renderer::set_shadow_distance(m)` (default 0 = off) sets how far into the view the map reaches. The game runtime sets it every frame from `WorldSettings::shadow_distance` (World Settings › **Shadow distance**, default 80 m; Plumo level uses 10 m for sharp shadows of the duck, the ball and the walls). With a shadow distance and a perspective camera, the map covers the **bounding sphere of the view frustum** from the near plane to the shadow distance (stable shadow maps after Valient 2008). For a symmetric frustum with the tangents of the half opening angles `th`, `tv`:

```cpp
const f32 n = camera.near, f = max_of(camera.near + 0.1f, min_of(shadow_distance_, camera.far));
const f32 tv = std::tan(camera.fov * 0.5f), th = tv * aspect, k = th * th + tv * tv;
const f32 c = min_of(f, (f + n) * (1 + k) * 0.5f);         // centre on the view axis
const f32 r = std::sqrt((f - c) * (f - c) + f * f * k);    // radius
focus = camera.eye + forward * c;  extent = r;
```

Why a sphere? Its size does not change when the camera turns. The texel size therefore stays the same too, and together with the snapping (below), edges fall onto the grid the same way every frame. Without a shadow distance, or with an orthographic camera, the fixed square `extent` around `camera.target` applies as before.

**Snapping to the texel grid.** The focus is decomposed into light coordinates and rounded to whole texels; the texel size follows from the current extent (`extent_`, the fitted or the fixed one):

```cpp
extent_ = extent_for(extent);                       // > 0: the fitted one, otherwise options_.extent
const f32 texel = 2 * extent_ / static_cast<f32>(options_.resolution);
const f64 a = std::round(dot(focus, b.right) / texel);
const f64 c = std::round(dot(focus, b.up) / texel);
const f64 d = std::round(dot(focus, b.dir) / 8.0f);   // coarse along the light
```

Why? If the map moves with the camera by fractions of a texel, shadow edges land on the grid differently every frame – the edges "swim" and flicker. Snapped, the map only moves in whole texels, and an edge always lies the same way. Along the light direction a coarse 8 m step is enough; this movement does not shift any texels, it only has to keep the depth range valid.

**Reuse.** A hash is formed from the rounded coordinates, the sun direction (rounded to 10⁻⁴) and the extent (rounded to millimetres). The renderer combines it with the shadow revision and the frame's layer mask. The map is redrawn only when this key changes. The shadow revision increases when a shadow caster appears, disappears, moves, changes its material, or when `invalidate(true)` is called. With a stationary camera over a static city, the shadow then costs not a single draw call.

**Drawing.** Matrix: `ortho(-e, e, -e, e, 0.2, 4e) * look_at(focus + dir * 2e, focus, up)` with `e` = the current extent and the snapped focus. The light frustum culls meshes whose bounds lie outside (except skinned ones). Colour writes off, depth test `LESS`, `polygonOffset(2, 3)` against shadow acne, a dedicated lean shader (position, instance matrix and skinning only; the fragment shader is empty). Only meshes with `casts_shadow()` in the active layers are drawn, with their own cull mode.

**Sampling.** The depth texture has `TEXTURE_COMPARE_MODE = COMPARE_REF_TO_TEXTURE` and linear filtering. A `texture(sampler2DShadow, ...)` then compares four neighbouring texels in hardware and interpolates the results bilinearly – that is the "hardware PCF", level 1. Levels 4 and 9 build on it, after Castaño (2013, "The Witness"), a tent-filtered 3 × 3 or 5 × 5 kernel: each tap lies between the texels so that the bilinear hardware weighting yields exactly the tent weights (`sunShadow` in the standard shader; sum divided by 16 or 144). For this the shader gets `uSunShadowFilter` and `uSunShadowTexel` (map size, its reciprocal, one texel in metres):

```glsl
if (uSunShadowEnabled == 1 && ndl > 0.0) {
  // Normal offset (Holbein 2011): look the shadow up a texel off the surface along its normal,
  // more where the light grazes it - no acne without a large constant bias.
  vec3 lookup = vWorld + n * (uSunShadowTexel.z * (0.6 + 1.4 * (1.0 - ndl)));
  vec4 clip = uSunShadowProjection * vec4(lookup, 1.0);
  vec3 uvz = clip.xyz / clip.w * 0.5 + 0.5;
  if (uvz.x > 0.0 && uvz.x < 1.0 && uvz.y > 0.0 && uvz.y < 1.0 && uvz.z > 0.0) {
    shadow = sunShadow(uvz);
    float d = length(vWorld - uSunShadowFade.xyz);
    shadow = mix(shadow, 1.0, smoothstep(uSunShadowFade.w * 0.9, uSunShadowFade.w, d));
  }
}
```

Outside the map a surface counts as lit. Surfaces facing away from the sun (`ndl == 0`) do not read the map at all. With a shadow distance, the shadow fades out over its last tenth (measured from the eye, `uSunShadowFade`) instead of breaking off at the map's edge; without a shadow distance there is no fade.

This leads to the most important tuning advice: the extent and `resolution` together determine the texel size. With a shadow distance the extent is the sphere's radius: a smaller shadow distance makes the shadows sharper but reaches less far. Kern Studio creates the map with 2048 texels (fixed `extent` 110 m) and uses the World Settings' shadow distance in levels; the C++ example Orb Run uses 2048 texels over a fixed ±110 m, i.e. just over 10 cm per texel, `examples/hello` 1024 texels over ±40 m.

### 4.8.2 Sky

`SkyPass` (`src/gfx/sky.cpp`) draws a single full-screen triangle, generated by the vertex shader from `gl_VertexID`, on the far plane (`gl_Position.z = w`). It runs **after** the opaque meshes, with depth test `LEQUAL` and depth writes off. Only pixels still at the cleared depth value 1 pass the test. The expensive sky shader therefore runs only where sky is actually visible.

The per-pixel view direction is derived from the camera's forward, right and up vectors and `tan(fov/2)`. The fragment shader composes the sky from these parts:

- **Gradient** between horizon and zenith color with `pow(h, 0.45)` (h = elevation of the direction), blended toward dark blues at night (`SkyState::night`).
- **Golden hour:** `dusk = clamp(1 - |sun.z| * 4, 0, 1) * (1 - night * 0.6)` tints the horizon orange and the zenith violet, plus a glow at the horizon on the sun's side.
- **Sun:** a Mie-like glow `pow(mu, 8) * 0.22 + pow(mu, 64) * 0.5` and a disc `smoothstep(0.9993, 0.99975, mu) * 12` with `mu = dot(direction, sun)`. If the sun is below −0.05, both disappear.
- **Night:** moonlight, the moon disc and twinkling stars from a hash grid.
- **Clouds:** a layer of 5-octave fBm (value noise), projected via `d.xy / (d.z + 0.12)` and shifted by `SkyState::wind * time`. `SkyState::blast_age > 0` pushes the clouds away from an explosion direction (shock wave).
- **Below the horizon** the color blends over to the horizon color.
- **Horizon fog:** with `fog_horizon > 0`, the horizon dissolves into the scene's fog color (same color, same sunlight). The renderer scales the value by `1 - exp(-(fog_density * min(far, 2000))²)`: thin fog yields only a thin haze at the horizon.

Like the scene, the sky writes HDR into `outHdr` and the tonemapped result into `outColor`. There is no sky with orthographic cameras; the clear color then remains.

### 4.8.3 Bloom and Composite

`BloomPass` (`src/gfx/bloom.cpp`) needs rendering into floating-point textures (`EXT_color_buffer_float` or `EXT_color_buffer_half_float`). If that is missing or a program fails, `stats.bloom` reports the state `unsupported`, and the renderer draws directly into the default framebuffer.

**Sequence in `end()`** (every step with a full-screen triangle):

1. **Bright-pass extract** from the HDR attachment into a1 (1/4 resolution) with a soft knee:
   `o = c * smoothstep(0.88, 0.88 + 0.35, max(c.r, c.g, c.b))`.
2. **Blur stage 1:** horizontal a1 → b1, vertical b1 → a1. The kernel has 9 taps but is read with 5 texture fetches: the outer pairs lie at offsets of 1.3846 and 3.2308 texels, and linear filtering blends two neighboring texels in a single fetch. Weights 0.2270 / 0.3162 / 0.0703.
3. **Copy** a1 → a2 (1/8 resolution), **blur stage 2** a2 ↔ b2 – the wider glow stage.
4. **Composite** into the default framebuffer.

The composite shader computes:

```glsl
vec3 scene = texture(tScene, vUv).rgb;                       // finished LDR image
vec3 b = texture(tB1, vUv).rgb * uS1 + texture(tB2, vUv).rgb * uS2;   // 0.13, 0.08
...
vec3 h = max(texture(tHdr, vUv).rgb, 0.0), h2 = h;
if (occluded) h2 *= 1.0 - share.rgb * (1.0 - aoMultiBounce(aoVisibility(px), vec3(share.a)));
o = vec4(clamp(scene + present(h2 + b) - present(h), 0.0, 1.0), 1.0);
```

`present()` is the same chain as in the scene shader (ACES, optional grading, sRGB). So the composite adds to the finished image only the **difference** between "HDR with glow and AO" and "HDR without", both passed through the same tonemapping curve. Why so roundabout? Because the LDR image already contains the fog, which was applied *after* sRGB and is missing from the HDR attachment. If the composite re-tonemapped the HDR image, the fog would be lost. Pixels without glow and without occlusion are copied unchanged by the shader (`if (!glow && !occluded)`), which also saves compute time.

**Frame-time guard.** `begin()` keeps a moving average of the frame interval (factor 0.05; outliers above 200 ms do not count). If it stays above 27 ms for longer than 3.5 s, the glow switches itself off for the session (`BloomState::disabled_slow_device`), unless `BloomOptions::forced` is set. The composite keeps running afterwards, because it also applies the AO. When the page is hidden (`page_hidden`) the guard is reset, so that throttled background frames do not switch off the glow.

### 4.8.4 Ambient Occlusion (GTAO)

Kern implements Ground-Truth Ambient Occlusion after Jimenez, Wu, Pesce and Jarabo (2016) in the form of Intel's XeGTAO (`src/gfx/gtao.cpp`). What is special lies less in the technique than in how it is embedded: **there is no depth prepass and no additional geometry pass.**

**The idea in three steps:**

1. Besides the image, the normal forward pass writes into attachment 2 what share of each pixel comes from indirect light (4.5, "The AO Record").
2. GTAO computes, on the finished scene depth, how strongly each point is occluded by its surroundings.
3. The bloom composite darkens exactly the indirect share. Direct sunlight already has its shadow, and so do point lights; they remain untouched.

This puts AO in the physically correct place (occlusion affects ambient light) without drawing geometry a second time. The README justifies dropping the prepass by noting that the hidden-surface-removal hardware of Apple and Mali FPK discards overdraw anyway.

**Main pass** (one AO texel per fragment, output RG16F = visibility, view depth):

- *View depth from the depth buffer:* `w = P[3][2] / ((d * 2 - 1) + P[2][2])`, view-space position `(ndc.x * w / P[0][0], ndc.y * w / P[1][1], -w)`.
- *Normal from depth:* per axis, the neighbor with the smaller depth jump is taken (it is more likely to lie on the same surface); `N = normalize(cross(dx, dy))`, turned toward the camera.
- *Radius:* `uRadius` (world meters) is converted to AO pixels and limited to 20 % of the image height. Below 1.5 pixels the point counts as unoccluded.
- *Slices:* for each of `uSlices` directions `phi` in the image plane, the normal is projected into the slice plane (`projN`, angle `n`). The shader then searches on both sides, in `uSteps` steps, for the highest horizon: the largest cosine `dot(Δ, V) / |Δ|` among the sample points. The sample distribution is quadratic (`t = t² + minS`), denser close to the point. A falloff over the last 61.5 % of the radius blends distant points back toward the initial horizon, so that far-away geometry does not occlude.
- *Integral:* the horizon angles `h0`, `h1` are clamped to ±π/2 around `n`, then the cosine-weighted visibility is integrated analytically:
  ```
  arc(h) = (cos n + 2 h sin n - cos(2h - n)) / 4
  visibility += |projN| * (arc(h0) + arc(h1))
  ```
  Finally `visibility = pow(visibility / slices, uPower)`, at least 0.03.
- *Noise:* the slice rotation follows an ordered 4×4 Bayer pattern; every 4×4 block contains each rotation exactly once. `acos` is approximated with `fastAcos` (Lagarde, max. error 0.0068 rad).

**Denoising.** A depth-aware 4×4 filter averages over exactly this pattern. Neighbors with a different depth (tolerance `w * 0.04 + 0.05`) get no weight, so that nothing bleeds across edges.

**Applying it in the composite.** `aoVisibility` reads the AO texture with depth-aware bilinear upsampling (tolerance `w * 0.03 + 0.03`). Then comes the multi-bounce fit from the paper – bright albedo wins back light that bounces back and forth between occluders:

```glsl
vec3 aoMultiBounce(float v, vec3 albedo) {
  vec3 a = 2.0404 * albedo - 0.3324, b = -4.7951 * albedo + 0.6417, c = 2.7552 * albedo + 0.6903;
  return max(vec3(v), ((v * a + b) * v + c) * v);
}
```

and `h2 *= 1 - share.rgb * (1 - aoMultiBounce(v, share.a))`.

**Quality levels** as in XeGTAO (slices × steps per side):

| `AoQuality` | Slices | Steps |
|---|---|---|
| `low` | 1 | 2 |
| `medium` | 2 | 2 |
| `high` | 3 | 3 |
| `ultra` | 9 | 3 |

**Options** (`AoOptions`): `enabled` (on), `quality` (`medium`), `half_resolution` (on), `radius` (1.6 m), `power` (2.2), `adaptive` (on). With `adaptive`, a frame-time guard (same thresholds as bloom: 27 ms for 3.5 s) first steps the quality down to `low` and then switches to half resolution.

`set_ambient_occlusion_radius(r)` changes only the radius – a property of the scene, not of the device; 0 turns AO off for that scene. The game runtime sets it from `WorldSettings::ao_radius` ([Chapter 8](08-game-runtime.md)).

**When AO runs:** only with a scene target (so not in partial viewports, and not when bloom is switched off or unsupported), only with a perspective camera and only when enabled. On devices with a coarse pointer (phones), the Studio and exported games choose `medium` at half resolution, otherwise `high` at full resolution; the URL parameters `?ao=off|low|medium|high|ultra` and `?aoRes=half|full` override this (`web/studio/game.mjs`). From JavaScript: `KernRenderer.setAmbientOcclusion({enabled, quality, fullResolution, radius})`.

## 4.9 Particles and Point Lights

### `Particles`

`Particles` (`gfx/particles.h`) draws camera-facing quads – sparks, smoke, muzzle flashes – as **one** instanced, transparent mesh. It is purely visual: no collision, no physics.

```cpp
struct ParticleSpec {
  Vec3 position, velocity;
  Vec4 color{1, 1, 1, 1};  // alpha fades out over the lifetime
  f64 start_ms = 0;        // start time (same clock as tick())
  f32 life_ms = 350;       // 16 .. 10000
  f32 size = 0.35f;        // edge length in meters at birth
  f32 gravity = 0;         // m/s² downward (negative lifts)
  f32 grow = 0;            // size * (1 + grow * t)
  f32 fade_extra = 0;      // alpha *= 1 - fade_extra * t
};
```

- `init(renderer, capacity, mesh_id)` – budget 1 to 4096; without a `mesh_id`, the system obtains an id itself.
- `emit(spec)` – validates all values (finite, colors in [0, 1], lifetime, size) and returns a serial number, or 0 if the budget is full (`stats.dropped`).
- `tick(now_ms, eye)` – time never runs backwards (otherwise faded particles would reappear after a pause). Expired particles are removed. Position: `p0 + v t − (0, 0, ½ g t²)`. The quads are rotated about Z toward the camera, **sorted back to front** (within a single draw the GPU blends in instance order) and uploaded via `update_instances`. Alpha: `color.a * (1 − t) * (1 − fade_extra * t)`.

The mesh is unlit, double-sided, casts no shadow, is not frustum-culled and is invisible as long as no particle is alive. The game runtime uses a system with 512 particles; from JavaScript the same class is available via `kern_fx_*`.

### `LightPool`

The shader knows 6 point lights. `LightPool` selects the most important ones each frame from any number of requests:

```
score = intensity / (1 + distance_to_eye / max(4, range))
```

On a tie, the earlier request wins. `request()` limits intensity to 100 and range to 1000; `resolve(eye)` sorts, fills a `PointLights` structure and empties the queue. Each frame, Orb Run requests a headlight in front of the car and one light per orb and passes the result to `set_point_lights`.

## 4.10 Settings

### `RendererOptions`

| Field | Default | Meaning |
|---|---|---|
| `width`, `height` | 960 × 540 | size of the drawing buffer (later `resize`) |
| `shadows` | `false` | create sun shadows |
| `shadow` | 512 / 120 / 0.0012 / 9 | `ShadowOptions` (4.8.1) |
| `bloom` | `enabled`, not `forced`, `grade` | `BloomOptions` (4.8.3) |
| `ao` | see 4.8.4 | `AoOptions` |
| `grade` | `true` | production grading after tonemapping |

At runtime: `set_shadows(&options)` or `set_shadows(nullptr)`, `set_shadow_distance` (metres, 0 = fixed `extent`), `set_shadow_filter` (1, 4 or 9 taps), `set_ambient_occlusion`, `set_ambient_occlusion_radius`, `set_point_lights`, `invalidate(shadows)`.

### `Lighting` and `SkyState`

`Renderer::lighting` holds all light colors linear and premultiplied by intensity. In the code, the defaults are called "production day":

| Field | Default | Effect |
|---|---|---|
| `sun_direction` | (−0.45, −0.52, 0.72) | direction *toward* the sun |
| `sun_color` | (2.8, 2.47, 2.11) | sunlight |
| `sky_color`, `ground_color` | (0.469, 0.609, 0.85), (0.125, 0.094, 0.054) | hemisphere light |
| `fog_color`, `fog_density` | (0.474, 0.584, 0.716), 0.00085 | FogExp2 (sRGB color) |
| `exposure` | 1 | exposure before ACES |
| `env_intensity` | 0.9 | strength of sky irradiance and reflection |
| `env_zenith`, `env_horizon`, `env_ground` | blue, light blue, greenish brown | environment dome |
| `fog_height_falloff`, `fog_sun_scatter`, `fog_horizon` | 0 | atmosphere (4.5, 4.8.2) |

`Renderer::sky` (`SkyState`) controls the sky separately: sun and moon direction, `night` (0..1), `clouds`, `wind`, explosion direction and age.

The game runtime computes all of this from the time of day (`GameWorld::apply_lighting`): it sets a ratio of sun to sky irradiance of about 5:1, uses the hemisphere light only as a night-time fill, derives `env_ground` from the light reflected by a road with albedo 0.25, and switches on height fog, sun scattering and horizon fog. Details are in [Chapter 8](08-game-runtime.md).

### `RenderParams`

| Field | Default | Meaning |
|---|---|---|
| `clear_color` | (0.045, 0.07, 0.12, 1) | clear color, visible where there is no sky |
| `has_viewport`, `viewport` | off | sub-rectangle with scissor; without bloom and AO |
| `page_hidden` | `false` | suspends the frame-time guards |
| `layers` | all | layer mask for drawing and shadows |
| `sky` | `true` | draw the sky |

### Camera

`Renderer::camera` is a `Camera` from `math/math.h`: `eye`, `target`, `up` (default +Z), `fov` (default π/3), `near` 0.05, `far` 1800, optionally `orthographic` with `ortho_height`. The world is Z-up; units are meters.

## 4.11 Context Loss

A browser may take away the WebGL context at any time – under memory pressure, on driver resets or in the background on mobile devices. Kern handles this completely:

1. **Loss** (`webglcontextlost`): the platform layer calls `preventDefault()` (otherwise there is no restoration), then `kern_context_lost()` and `kern.resetTables()`. `Renderer::context_lost()` forgets all GPU handles without calling `delete*` (the objects are already dead) and destroys programs and passes in "contextLost" mode. From then on, `render()` refuses with `"GL context lost; waiting for restoration"`. Meshes and textures are kept as CPU copies; `upsert` keeps working but uploads nothing.
2. **Restoration** (`webglcontextrestored`): the page first refills the host-owned textures in their reserved slots (`kern.pinned`), then calls `kern_context_restored()`. The renderer rebuilds all programs and passes (`init_gpu`, including `bind()` for all extensions), uploads all engine-owned textures and all meshes from the CPU copies and invalidates scene and shadows.
3. **Failed restoration:** the renderer stays in the lost state and frees whatever it had already created. A later attempt starts cleanly from scratch.

`set_headless()` uses the same mechanism for simulations without any GPU at all, for example in the Lehrer ([Chapter 14](14-lehrer.md)): meshes stay on the CPU, GL is never touched.

## 4.12 Performance Notes

What the code itself suggests:

- **Instance instead of duplicating.** Many identical objects (trees, cars, crates) are one mesh with many instances and therefore one draw call. The runtime's city draws all tree trunks as one mesh and all crowns as a second.
- **Merge static geometry.** `primitives::append` and the chunks of the building and road builders combine many small parts into a few large meshes. Fewer meshes mean fewer draw calls and less sorting work.
- **Upload only what changes.** `update_instances` compares byte for byte and does nothing for identical data; a pure tint update uploads only one buffer and leaves the shadow valid. If you rewrite instances every frame, touch only the meshes that move.
- **Reuse shadows.** Every movement of a shadow caster redraws the entire shadow map. Moving things that need no visible shadow (particles, roads, ground tiles) should have `cast_shadow = false` – Orb Run and the runtime do this for roads and the ground. With a shadow distance the map also follows the camera: a stationary camera reuses it, a moving or turning one redraws it as soon as the focus shifts by a texel.
- **16-bit indices.** Meshes with up to 65,536 vertices automatically get 16-bit indices. `weld` (Chapter 5) turns triangle soups into indexed meshes and saves memory in the process.
- **Don't switch off frustum culling** without a reason. `frustum_culled = false` is meant for things whose bounds are not correct (particles).
- **AO and bloom adapt.** Both have frame-time guards. For phones, AO at half resolution is the default.
- **Measure:** `RenderStats` provides draw calls, shadow draw calls, `shadow_reused`, triangles, instances, culled and visible meshes, CPU and GPU time. In JavaScript, `KernRenderer.render()` returns the same values as an object.

## 4.13 Example: A Custom Shader Extension (Orb Run)

Orb Run (`examples/orbrun/orbrun.cpp`) makes the city's windows light up at night. The buildings have no windows in their geometry. Instead, every vertex of the building meshes carries a "wall record" of four floats generated by the building builder (Chapter 5): wall length (−1 for roof, −2 for cornice), wall height, a building seed and the storey height. The UVs of the walls are meters along the wall (u) and meters above the base (v).

```cpp
class NightWindows final : public ShaderExtension {
 public:
  f32 night = 0;
  const char* name() const override { return "orbrun-night-windows"; }
  Span<const ExtensionAttribute> attributes() const override { return {kAttributes, 1}; }
  const char* vertex_declarations() const override { return "flat out vec4 vWall;\n"; }
  const char* vertex() const override { return "  vWall = aWall;\n"; }
  const char* fragment_declarations() const override {
    return R"GLSL(
// flat: the record is constant per wall; interpolation noise would scramble the window hash.
flat in vec4 vWall;
uniform float uNight;
float windowHash(float n) { return fract(sin(n * 91.7) * 43758.5453); }
)GLSL";
  }
  const char* fragment_surface() const override {
    return R"GLSL(
  if (vWall.x > 0.0 && abs(n.z) < 0.5) {
    float u = vUV.x, z = vUV.y, storey = vWall.w;
    float column = floor(u / 3.0), floorIndex = floor(z / storey);
    vec2 cell = vec2(fract(u / 3.0), fract(z / storey));
    // Fade the grid out where it would alias; distant facades keep an average glow instead.
    vec2 fw = fwidth(vec2(u / 3.0, z / storey));
    float farFade = 1.0 - smoothstep(0.08, 0.3, max(fw.x, fw.y));
    float win = step(0.22, cell.x) * step(cell.x, 0.78) * step(0.28, cell.y) * step(cell.y, 0.78)
              * step(storey, z) * step(z, vWall.y - 1.0) * farFade;
    float lit = step(0.42, windowHash(column * 7.31 + floorIndex * 17.0 + floor(vWall.z * 997.0 + 0.5)));
    base = mix(base, vec3(0.05, 0.06, 0.08), win * 0.85);
    surfaceGlass = max(surfaceGlass, win * (1.0 - lit * uNight) * 0.7);
    float tall = step(storey, z) * step(z, vWall.y - 1.0);
    emissive += (win * lit + (1.0 - farFade) * tall * 0.13) * uNight * vec3(1.0, 0.74, 0.42) * 1.6;
  }
)GLSL";
  }
  void bind(const Program& p) override { u_night_ = p.location("uNight"); }
  void frame_uniforms(UniformCache& cache, const Renderer&) override { cache.set_float(u_night_, night); }

 private:
  static constexpr ExtensionAttribute kAttributes[] = {{"wall", 4, "aWall"}};
  u32 u_night_ = 0;
};
```

What happens here, step by step:

1. **A vertex stream** `wall` with 4 floats, `aWall` in the shader. It ends up at location 14.
2. **Vertex hook:** the record is passed on as `flat`. It is constant per wall; interpolation would only produce rounding noise that scrambles the window hash.
3. **Fragment hook `fragment_surface`:** only on walls (`vWall.x > 0`) with a roughly horizontal normal. The grid has 3 m wide columns and storey-high rows; a window occupies the middle part of each cell, from the first floor up to 1 m below the eaves. A hash of column, storey and building seed decides whether a window is lit (about 58 %).
4. **Against aliasing:** `fwidth` measures how fast the grid coordinate changes per pixel. When the grid becomes finer than about one pixel, `farFade` fades the windows out and replaces them with an average glow of the facade.
5. **Effect on the material:** window areas become dark (`base`), unlit windows become glass (`surfaceGlass` – they reflect the sky), lit ones get a warm `emissive`. Because `emissive` is linear and unbounded, the windows glow through the bloom pass.
6. **Uniform:** `uNight` is resolved in `bind()` and set once per frame through the cache. The game writes `windows_.night` from its day/night cycle.

The extension is hooked in via `App::extensions()`:

```cpp
Span<ShaderExtension* const> extensions() override { return {extensions_, 1}; }
...
ShaderExtension* extensions_[1] = {&windows_};
```

When uploading the building chunks, Orb Run passes the extra stream in slot 0 (`mesh.as_input(0)`); roads and ground tiles get `kMaxExtensionAttributes` as the slot, i.e. no stream – there `vWall.x` is 0 and the hook does nothing.

The game runtime has a class `kern::game::NightWindows` with identical GLSL (declared in `include/kern/game/runtime.h`, implemented in `src/game/runtime.cpp`), which Kern Studio and exported games always register. The "Procedural City" component decides with `night_windows` whether its buildings receive the stream ([Chapter 5](05-scene-and-geometry.md)).

---

© 2026 Christian Heinrich Hohlfeld · All rights reserved · [Imprint and rights](00-imprint.md)
