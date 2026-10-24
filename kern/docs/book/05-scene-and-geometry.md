# Chapter 5 – Scene and Geometry

This chapter covers two building blocks that sit around the renderer:

- **`kern::scene`** (`include/kern/scene`, `src/scene`): visibility tests against the camera frustum, a bounding volume hierarchy (BVH) and picking – that is, the question "which triangle is under the mouse pointer?".
- **`kern::geo`** (`include/kern/geo`, `src/geo`): generating geometry on the CPU – polygon triangulation with Earcut, primitives, lossless welding, buildings from footprints, road ribbons, terrain tiles.

After that comes how the game runtime builds the procedural city from these, and which meshes are compiled into the engine.

The renderer itself is covered in [Chapter 4](04-graphics.md). The mathematical background (matrices, coordinate spaces, projection) is in [Appendix B](B-rendering.md).

## 5.1 Conventions

- **Z is up**, units are meters. This applies to the renderer, physics, audio and all geometry builders.
- **`MeshData`** (`geo/mesh_data.h`) is the common container of all builders: `positions`, `normals`, `colors`, `uvs` as flat `f32` vectors, optionally an extra stream `extra` with `extra_size` floats per vertex, and `indices`. Without indices the mesh is a *triangle soup*: every three consecutive vertices form a triangle.
- **Colors in `MeshData` are sRGB**, like all vertex colors that the standard shader receives.
- `MeshData::as_input(extra_slot)` returns a borrowed `MeshInput` view for `Renderer::upsert`. The extra stream goes into extension slot `extra_slot`; a value ≥ `kMaxExtensionAttributes` (2) leaves it out.

## 5.2 Frustum Culling

### What It Does

A mesh that lies completely outside the camera's view volume does not need to be drawn. The test for this must be much cheaper than drawing itself – otherwise it does not pay off. Kern therefore does not test the triangles but a single sphere per mesh against six planes: six dot products instead of thousands of vertex transformations and a draw call.

### Planes from the Matrix (Gribb/Hartmann)

`Frustum::from_matrix(vp)` extracts the six planes directly from the view-projection matrix. A point `p` lies inside in clip space if `-w ≤ x, y, z ≤ w`. Each of these six inequalities is linear in `p` and thus a plane: `row4 ± row_i`. The code reads the rows from the column-major matrix and normalizes each plane:

```cpp
for (int axis = 0; axis < 3; ++axis) {
  for (int sign = 1; sign >= -1; sign -= 2) {
    const f32 a = m.m[3] + sign * m.m[axis], b = m.m[7] + sign * m.m[4 + axis];
    const f32 c = m.m[11] + sign * m.m[8 + axis], d = m.m[15] + sign * m.m[12 + axis];
    const f32 len = std::sqrt(a * a + b * b + c * c);
    f.planes[o++] = len > 1e-12f ? Vec4{a / len, b / len, c / len, d / len} : Vec4{0, 0, static_cast<f32>(sign), 1e6f};
  }
}
```

Normalized planes are needed so that `a*x + b*y + c*z + d` is the true distance in meters and can be compared with the sphere radius.

### The Sphere Test

```cpp
bool sphere_visible(const Sphere& s) const {
  if (!(s.radius >= 0) || !finite(s.center)) return true;
  for (const Vec4& p : planes)
    if (p.x * s.center.x + p.y * s.center.y + p.z * s.center.z + p.w < -s.radius) return false;
  return true;
}
```

The test is **conservative**: it returns `false` only if the sphere lies definitely outside one plane. A sphere near a frustum corner may count as visible even though it is not – it is then drawn unnecessarily, but never wrongly omitted. Invalid spheres count as visible.

### Where the Spheres Come From

The renderer computes two spheres on `upsert` (`src/gfx/mesh.cpp`):

- `local_bounds`: center of the bounding box of the positions, radius out to the farthest vertex.
- `world_bounds`: the local sphere is transformed by every instance. For the radius the code uses the Frobenius norm of the 3×3 part of the instance matrix. It is always at least as large as the matrix's strongest stretch, and thus a safe upper bound. A box is formed from all instance spheres, and from that the world sphere.

For instanced meshes, `world_bounds` encloses all instances together. A forest whose trees are spread across the whole map is therefore almost always "visible". If you spread many instances over large areas, you gain more culling by splitting them into spatial groups.

Culling exceptions: skinned meshes are never rejected by their bounds (a pose can leave the bind-pose bounds), and neither are meshes with `Material::frustum_culled = false`. The shadow pass uses the same test against the sun's frustum.

## 5.3 BVH

### Why

Picking has to test a ray against triangles. A city tile easily has tens of thousands of triangles. Testing each one individually costs tens of thousands of Möller–Trumbore tests on every mouse click or every hover. A BVH arranges the triangles into a hierarchy of axis-aligned boxes. If the ray misses a box, all the triangles inside are dealt with at once. With a balanced tree, the cost drops from linear to roughly logarithmic in the number of triangles, plus the few triangles in the leaves that are hit.

### Construction

`Bvh::build(positions, vertex_count, indices, index_count, leaf_size = 8)` (`src/scene/bvh.cpp`):

1. One box per triangle, plus its center.
2. A work stack of tasks `(node, begin, end)` over an index array `order_`.
3. For each task: compute the box of all triangles. If there are at most `leaf_size` triangles, or all centers coincide, the node becomes a leaf.
4. Otherwise choose the axis with the largest extent of the centers and split at the **median** with `std::nth_element`. The two children always sit directly next to each other in the node array (`right = left + 1`).

`nth_element` partitions in linear time; over all levels this gives O(n log n). The median split halves every level, so the depth is at most 32. That is why a fixed stack of 64 entries is sufficient for traversal. Everything lives in flat arrays, with no per-node allocation.

### Traversal

`traverse(ray, max_distance, visit)` works with its own stack:

- Test the ray against the node box (slab test `intersect_aabb`, NaN-safe when the ray lies exactly on a box edge).
- In a leaf, call `visit(triangle, max_distance)` for each triangle; `visit` returns the new maximum distance, i.e. the nearest hit so far.
- For inner nodes, test both children and **push the farther one onto the stack first**, so that the nearer one is processed first. If a near hit is found early, `max_distance` shrinks and farther boxes drop out at the next slab test.

### When the Renderer Builds a BVH

Only for static world geometry: no lines, exactly one instance with an identity matrix, not skinned, at least 36 triangles (`kBvhMinTriangles` in `renderer.cpp`). If such a mesh is later moved or gets several instances, `update_instances` discards the BVH; if it becomes static again, it rebuilds it. Everything else uses the exact brute-force path.

## 5.4 Picking

### The Screen Ray

`screen_ray(camera, x, y, width, height, ray)` (`src/scene/picking.cpp`) inverts the view-projection matrix (in `f64`), converts the pixel into normalized device coordinates (`nx = x/width*2-1`, `ny = 1-y/height*2`) and projects it back onto the near and far planes. The direction is the normalized difference. With a perspective camera, the ray starts at the eye with the camera's `near`/`far`; with an orthographic camera it starts on the near plane under the cursor, and all rays are parallel.

`Renderer::pick(x, y, query, hit)` is `screen_ray` plus `pick_ray`. The coordinates are pixels in the drawing buffer.

### `pick_meshes`

For each mesh that is not excluded (invisible without `include_invisible`, `ignore_id`, lines, wrong layers, empty):

1. **Coarse test:** ray against the world sphere. If it misses, or the sphere lies behind the best hit so far, the mesh is done (`bounds_rejected`). Skinned meshes skip this test.
2. **Fine test:**
   - with a BVH: traversal, triangles in world coordinates,
   - without a BVH: every instance, every triangle, transformed with the instance matrix. For skinned meshes, `Mesh::posed_vertex` supplies the current pose from the bone matrices. So whatever you can see, you can also click – even on animated characters.
3. **Triangle test:** Möller–Trumbore (`intersect_triangle` in `bvh.h`). Back faces count only for `double_sided` materials.

The result `PickHit` contains the mesh id, instance, triangle, distance, hit point, face normal and barycentric coordinates. `PickStats` counts examined triangles and meshes rejected by bounds.

```cpp
PickHit hit;
if (engine.renderer.pick(in.pointer().x, in.pointer().y, {}, hit) && hit.mesh == boxes_) {
  // examples/hello: color the clicked crate red
  tints_[hit.instance * 4 + 0] = 1.0f; tints_[hit.instance * 4 + 1] = 0.25f; tints_[hit.instance * 4 + 2] = 0.2f;
}
```

`PickQuery` has `max_distance` (1800), `ignore_id`, `include_invisible` and `layers`. Kern Studio picks only the `layer::scene` layer, maps mesh ids to actors with `GameWorld::actor_for_mesh` and continues the ray with `ignore_id` past the ground or locked actors. Actors without a mesh (lights, empties) are selected by the screen distance of their icon (`Studio::pick` in `src/studio/viewport.cpp`).

## 5.5 Earcut: Triangulating Polygons

### What For

A building footprint is a polygon, often non-convex (L-shaped) and sometimes with holes (courtyards). For roof and floor the renderer needs triangles. `kern::Earcut` (`geo/earcut.h`, `src/geo/earcut.cpp`) is a C++ port of Mapbox Earcut v3.0.2 (ISC license, see `LICENSES.md`). The port follows the JavaScript original and, according to its header comment, produces the same results. The parity test against the JS version mentioned in `kern/README.md` (`tests/kern_geo_parity_test.mjs`) lives in the SpawnCity repository, not in this one; here, `earcut_shapes` checks area fidelity and triangle counts.

### Interface

```cpp
Earcut earcut;
std::vector<u32> tris;
// xy: outer ring, then the holes; hole_starts: vertex index at which each hole begins
earcut.triangulate(xy, vertex_count, hole_starts, hole_count, tris);
f64 err = Earcut::deviation(xy, vertex_count, hole_starts, hole_count, tris.data(), tris.size());
```

Coordinates are `f64`. The nodes live in blocks of 512 that are reused between calls; one `Earcut` object per builder saves allocations. `deviation` compares the polygon area (outer ring minus holes) with the sum of the triangle areas; 0 means exact.

### How Ear Clipping Works

A simple polygon with n vertices always has at least two "ears": vertices whose triangle formed from predecessor, vertex and successor lies entirely inside the polygon. Cutting off an ear leaves a polygon with n−1 vertices. Repeating this yields n−2 triangles.

Earcut keeps the polygon as a doubly linked circular list. `is_ear(ear)` checks, for vertex b with neighbors a and c:

1. Is the vertex convex? (`area(a, b, c) < 0` in Earcut's orientation.) A reflex vertex cannot be an ear.
2. Does another polygon vertex lie inside triangle abc? Only vertices within the triangle's bounding box are tested, and only reflex ones (a convex vertex inside the triangle implies a reflex one). If one lies inside, b is not an ear.

`earcut_linked` loops around the list, cuts off every ear it finds and continues with the successor.

### Special Cases

A polygon from real-world data (such as OpenStreetMap) is rarely clean. Earcut has a chain of fallback stages for this. If a pass goes all the way around without finding an ear:

- **Pass 0 → 1:** `filter_points` removes duplicate points and collinear vertices, then tries again.
- **Pass 1 → 2:** `cure_local_intersections` repairs local self-intersections (two crossing neighboring edges) and emits a triangle directly in the process.
- **Pass 2:** `split_earcut` looks for a valid diagonal, splits the polygon in two and triangulates both halves separately.

### Holes

Earcut can only clip ears from a single ring. Holes are therefore woven into the outer ring beforehand (`eliminate_holes`): for each hole, its leftmost point is found; the holes are sorted by this x. `find_hole_bridge` uses Eberly's method to find a visible vertex of the outer ring to the left of it. The hole is cut in along this "bridge": two edges there and back, with duplicated endpoints (`split_polygon`). Afterwards everything is a single ring, albeit one that touches itself at the bridge.

### z-Order Hashing for Large Polygons

The test "does another vertex lie inside the triangle?" is linear in n for each vertex; the whole thing becomes quadratic. With more than 80 vertices, Earcut switches to `is_ear_hashed`:

- All coordinates are scaled onto a grid from 0 to 32767.
- Each node gets a **z-order value** (Morton code): the bits of x and y are interleaved. Points that are close together in space usually have close z values.
- The nodes are additionally linked into a list sorted by z (`prev_z`, `next_z`).
- For an ear-candidate triangle, Earcut computes the z range of its bounding box and walks only through the nodes in this z range, from the vertex outward in both directions.

This way only spatially nearby vertices are checked instead of all of them.

### How the Builders Safeguard Earcut

`BuildingBuilder` calls Earcut only with cleaned rings and checks `deviation` afterwards. If the triangle area deviates from the polygon area by more than 0.7 %, the building is considered broken and is skipped (`skipped()`), rather than standing in the city with a wrong roof.

## 5.6 Primitives

`kern::primitives` (`geo/primitives.h`) generates indexed meshes, Z up, centered on the origin unless stated otherwise:

| Function | Shape | Details |
|---|---|---|
| `box(size, color)` | cuboid | 6 quads with flat normals, 24 vertices, UV 0..1 per face |
| `box_on_ground(size, color)` | cuboid | like `box`, bottom at z = 0 |
| `sphere(radius, segments = 24, rings = 16, color)` | UV sphere | smooth normals |
| `capsule(radius, length, segments = 16, rings = 8, color)` | capsule along Z | cylinder of length `length` between two hemispheres |
| `torus(major, minor, segments = 24, tube_segments = 8, color)` | torus around Z | |
| `cylinder(radius, height, segments = 20, color)` | cylinder with caps | stands on z = 0 |
| `cone(radius, height, segments = 16, color)` | cone with base | stands on z = 0 |
| `plane(width, depth, color, uv_scale = 1)` | rectangle, normal +Z | UVs in meters × `uv_scale` |
| `append(dst, src, m)` | – | appends `src` to `dst` with matrix `m` |

`append` transforms positions with `m` and normals with the normal matrix of `m`. This makes it possible to merge static parts into one mesh – Orb Run builds its car from three cuboids this way, and all of the city's ground tiles as one mesh. `append` does not copy the extra stream `extra`.

## 5.7 Welding and Content Hash

### Why Welding

The building and road builders produce triangle soups: every triangle has its own three vertices. That is simple to write, but corners where several triangles meet are stored several times. An indexed mesh stores each unique vertex once and refers to it by index. This saves memory and upload time, and the GPU can reuse already transformed vertices.

### `weld(mesh, min_saving)`

`weld` (`src/geo/mesh_data.cpp`) merges vertices whose position, normal, color, UV and extra stream are **bit-identical** – the IEEE 754 bit patterns are compared, never rounded values. This makes the procedure lossless: a hard edge (two faces with different normals at the same position) stays hard, because the normals differ.

Procedure:

1. For each vertex, a key row of 11 to 15 `u32` words (3 position, 3 normal, 3 color, 2 UV, up to 4 extra values).
2. An open-addressing hash table (capacity: next power of two ≥ 2n), hashing with FNV-1a over the words, post-mixed with a 64-bit finalizer. Collisions are resolved with linear probing; equality is checked with `memcmp` of the whole row.
3. Each vertex gets the number of its first occurrence (`remap`).
4. **Only if it pays off:** the code then computes the bytes (unique vertices plus indices of 2 or 4 bytes) and only indexes if the saving is greater than `min_saving`. Otherwise the soup is left unchanged.

Precondition: the mesh is a soup (no indices) and the extra stream has at most 4 floats. `WeldStats` reports vertices before and after, bytes before and after, and whether the mesh was indexed.

The README cites a measurement on a real Konstanz snapshot with 1300 buildings: building and welding take about 45 ms in Kern, compared with about 1260 ms for the previous JavaScript implementation. One reason is that `weld` compares bit patterns in a hash table instead of building strings as keys.

### `content_hash(mesh)`

FNV-1a (32-bit) over the bit patterns of all streams and indices. With it, a host can tell whether a newly built chunk differs from the old one at all. The C ABI stores it in `GeoChunk::hash`.

## 5.8 Buildings from Footprints

`BuildingBuilder` (`geo/buildings.h`, `src/geo/buildings.cpp`) extrudes 2D footprints into buildings: roof (via Earcut), walls and optionally a cornice. It works **incrementally under a time budget** and groups the result into square chunks.

### The Input Stream

Footprints arrive as a flat `f64` stream, so that JavaScript hosts can pass them without an object graph:

```
count,
then per building:
  seed_hash, base, top, has_center, center_x, center_y, ring_count,
  { vertex_count, x0, y0, x1, y1, ... } × ring_count        (first ring = outer)
```

In C++, `FootprintStream` writes this stream:

```cpp
FootprintStream footprints;
footprints.begin(seed, 0, height);        // start a building: seed, base, eaves
footprints.ring(outer, 4);                // outer ring
footprints.ring(court, 4);                // hole (courtyard)
```

`seed_hash` (u32) controls colors and facade variation. Without `has_center`, the first point of the outer ring determines which chunk the building falls into.

### What Happens per Building

1. **Clean the rings** (`clean_ring`): non-finite points and points less than 1.5 cm from their predecessor are dropped, as is the closing duplicate point. If fewer than 3 points remain, or an area ≤ 0.08 m², the ring becomes empty.
2. **Normalize the orientation:** outer ring counterclockwise, holes clockwise (`ring_area`, reversed if necessary).
3. **Reject** if a ring is empty, the wall height is ≤ 0.1 m, Earcut returns nothing or the area deviation exceeds 0.7 %.
4. **Colors** from the style: wall color `walls[h % n]`, roof color `roofs[(h >> 8) % n]`, cornice color as the mix `mix(wall, trim, trim_mix)` in `f64`, so that it matches a double-precision reference exactly. The storey height lies between 3.1 and 3.6 m, depending on the seed.
5. **Roof:** every Earcut triangle at height `top`, normal +Z; the winding is flipped per triangle so that the face points upward. Courtyards stay open.
6. **Walls:** for every edge of every ring (including the courtyard rings), two triangles from `base` to `top`, normal pointing outward. UVs: u = meters along the wall, v = meters above the base.
7. **Cornice** (if `cornice`, edge > 1.2 m, wall height > 3.5 m): a 0.55 m high band projecting 0.24 m at the eaves, with front, top and bottom faces.

### The Wall Record

If `wall_records` is on (default), every vertex carries four extra values in `MeshData::extra`:

| Value | Meaning |
|---|---|
| x | wall length in meters; −1 = roof; −2 = cornice |
| y | wall height (`top − base`) |
| z | building seed in [0, 1) (`(h % 997) / 997`) |
| w | storey height |

These values are intended for facade shaders. The shader extension `NightWindows` from [Chapter 4](04-graphics.md#413-example-a-custom-shader-extension-orb-run) reads them as the attribute `aWall` and draws window grids from them.

### Chunks and Time Budget

`chunk_for(x, y)` assigns the building to chunk `floor(x / chunk_size), floor(y / chunk_size)`; each chunk is its own `MeshData` and later becomes its own mesh. Larger chunks mean fewer draw calls, smaller ones better frustum culling.

```cpp
BuildingBuilder b;
b.begin(stream, length, options);
while (!b.step(4.0)) { /* next frame */ }    // at most ~4 ms per call
for (GeoChunk& c : b.chunks()) { weld(c.mesh); /* upload */ }
```

`step(budget_ms)` builds buildings until the budget is used up and returns `true` when everything is done. A host can thus spread a large city across several frames without any frame stalling. Orb Run and the runtime build their small city in one go with `step(1e9)`.

`BuildingOptions`: `chunk_size` (320, at least 20), `max_buildings` (2600), `cornice` (on), `wall_records` (on), `style` (`BuildingStyle` with lists of wall and roof colors, `trim`, `trim_mix`). Counters: `processed()`, `built()`, `skipped()`, `triangles()`.

## 5.9 Roads

`RoadBuilder` (`geo/roads.h`, `src/geo/roads.cpp`) lays road ribbons onto the terrain: optionally a curb band, the roadway and a dashed center line, likewise in chunks and under a time budget.

### Input

```
count,
then per road:
  width, flags (1 = curb, 2 = marking), surface_color, marking_color,
  point_count, x0, y0, x1, y1, ...
```

`surface_color` and `marking_color` are indices into `RoadOptions::palette`. In C++, `RoadStream::road(points, count, width, curb, surface_color, marking_color)` writes the stream; `marking_color = -1` means no marking.

### Bands

For each polyline segment, `band()` generates a rectangle of the desired width. So that the band follows hilly terrain, it is split into pieces of at most `segment_length` (6 m); each vertex gets its height from the `HeightSampler` plus a small offset:

| Layer | Height above terrain | Width |
|---|---|---|
| Curb | `curb_z` = 0.09 m | `width + curb_extra` (3.4 m) |
| Roadway | `surface_z` = 0.112 m | `width` |
| Marking | `marking_z` = 0.135 m | `marking_width` = 0.42 m |

The staggered heights prevent z-fighting between the stacked bands. The marking consists of dashes of length `dash_length` (3 m) at intervals of `dash_period` (7 m), starting at `dash_start` (1 m). Segments farther than `radius` (plus a margin) from `center_x/center_y` are skipped; this makes it possible to build a section of a large map. UVs are world coordinates × `uv_scale`.

### `HeightSampler`

```cpp
struct HeightSampler {
  f64 (*fn)(void* user, f64 x, f64 y) = nullptr;
  void* user = nullptr;
  f64 operator()(f64 x, f64 y) const { return fn ? fn(user, x, y) : 0.0; }
};
```

An empty sampler returns 0, i.e. flat ground. The comment calls the sampler "physics truth": the game should supply the same height function that its physics uses, so that vehicles drive exactly on the visible road. If the sampler returns a non-finite value, the builder aborts with an error (`failed()`). In JavaScript you set `kern.heightSampler = (x, y) => ...`; the engine calls it through the import `kern_env.terrain_height`.

## 5.10 Terrain Tiles

```cpp
bool build_terrain_tile(f64 x0, f64 y0, f64 step, u32 cells, Rgb albedo, HeightSampler heights,
                        MeshData& out, f64* min_height = nullptr, f64* max_height = nullptr);
```

Builds a regular grid of `cells × cells` quads (1 to 128) with edge length `step`, one vertex per height node, as an indexed mesh. Each quad is split along the southwest–northeast diagonal.

The normals are computed from central differences of the neighboring nodes. For this, the function samples a grid with a one-node border, so that edge vertices also get correct normals and neighboring tiles fit together seamlessly:

```
dx = h(i+1, j) − h(i−1, j),   dy = h(i, j+1) − h(i, j−1)
n  = normalize(−dx, −dy, 2·step)
```

The UVs run across the tile from 0 to 1; `min_height`/`max_height` return the height range (useful for bounds or LOD decisions).

`GeoProjector` converts WGS84 longitude and latitude into meters east and north of an origin (local tangent plane, earth radius 6,378,137 m). This lets footprints from map data be brought directly into Kern coordinates.

## 5.11 Geometry from JavaScript

The builders are also reachable through the C ABI (`src/api/geo_api.cpp`) and wrapped in `KernRenderer`:

```js
const job = renderer.beginBuildings(footprintStream /* Float64Array */, options /* Float64Array */);
await job.run({ yieldFrame: () => new Promise(requestAnimationFrame), budgetMs: 4 });
const { chunks } = job.stats();              // processed, built, skipped, triangles, chunks
for (let i = 0; i < chunks; i++) job.commit(i, "buildings-" + i, { material, extraKey: "wall" });
job.free();
```

`extraKey` names the extension stream that receives the builder's record; without `extraKey` it is the first. A name the renderer does not know as a stream makes `commit` throw an error (`KernGeoJob.commit: no extension stream "…"`) instead of silently using another stream. `kern_geo_commit` welds the chunk, uploads it as a mesh and frees the chunk memory immediately, because the renderer keeps its own copy. It reports the bytes before and after welding. `renderer.upsertTerrainTile(id, {x0, y0, step, cells, albedo, material})` builds and uploads a terrain tile in a single call. A `MeshInput` descriptor can also carry the flag `KERN_MF_WELD` (`input.weld === true` in the facade); the engine then welds a supplied soup before uploading – provided it is a pure triangle soup with normals (no indices, no skinning, no lines, at most one extension stream with 1–4 floats); otherwise it is uploaded unchanged.

## 5.12 The Game Runtime's Procedural City

The game runtime has a **Procedural City** component (`CityComponent` in `include/kern/game/level.h`). It is Orb Run's city generator as a level component: in the Studio you place it in a level via the catalog entry "Procedural City" (group "World" in `src/studio/catalog.cpp`), and it generates buildings, roads, ground tiles, trees, collision bodies and traffic.

![An exported game: the procedural city with buildings, roads and trees](../buch/bilder/game.png)

### Parameters

```cpp
struct CityComponent {
  u32 seed = 20261022;
  i32 blocks = 7;               // blocks per side
  f32 pitch = 80;               // meters from street to street
  f32 street = 16;              // street width
  f32 max_height = 84;          // tallest building, in the center
  bool trees = true;
  i32 traffic = 22;             // cars in the lanes
  bool night_windows = true;    // lit windows at night (shader extension)
};
```

`build_city` rejects invalid values: `blocks` 1–16, `pitch` ≥ 30, `street` ≥ 6 and at most half of `pitch`.

### The Generator

`build_city(city, out)` in `src/game/geometry.cpp` is deterministic for a given seed. The random number generator `game::Rng` is an xorshift32 – according to the comment, the same sequence as in Orb Run.

1. **Block grid.** `blocks × blocks` blocks with center spacing `pitch`, with streets of width `street` in between. Each block gets an 18 cm high ground tile (all tiles together form one mesh via `primitives::append`) and a collision box for it.
2. **Height by proximity to the center.** `centrality = 1 − clamp(|center| / (half · 1.2), 0, 1)`, maximum `14 + centrality² · (max_height − 14)`. The center thus becomes the high-rise district, while the outskirts stay low.
3. **Development type per block** (uniformly chosen from four cases, lot inset by 3 m):
   - one block-sized building, in 60 % of cases with a 14 m deep courtyard (a hole in the footprint; collision as four wings),
   - four separate buildings with 5 m alleys,
   - an L-shaped building plus a smaller one in the free corner,
   - three slab blocks.
4. **Streets.** Every block edge is a street center line, extended 60 m beyond the city, width `street · 0.75`, with curb and center line.
5. **Building.** `BuildingBuilder` with `chunk_size = 200`, six wall and three roof colors; `RoadBuilder` on flat ground with `chunk_size = 200`. All chunks are welded.
6. **Trees** (if `trees`): along the block edges every 11 m, with 35 % gaps, random size, rotation and shade of green. Trunks are cylinders, crowns are cones; each as **one** instanced mesh.

The result `CityGeometry` contains the building chunks (with wall records), road chunks, ground tiles, tree meshes with instance matrices and tints, the collision boxes (`CityBox`), the half extent and the number of buildings.

### Display

`GameWorld::sync_city` (`src/game/runtime.cpp`) uploads the geometry:

- Each chunk becomes a mesh with the city actor's world matrix as its only instance. If you move the actor, only this matrix changes (`update_instances`); nothing is rebuilt.
- Buildings get the wall record in extension slot 0 if `night_windows` is on; otherwise no stream.
- Roads: no shadow, double-sided, roughness 1. Ground tiles: roughness 0.95.
- Trees: instance matrices = city matrix × local tree matrix.
- A content key (FNV over the component's JSON) decides whether a rebuild is needed. Only a real parameter change triggers a rebuild.

If the city sits at the origin without rotation, the instance matrix is the identity, and the renderer builds a BVH for each chunk. Picking in the city (for example in the Studio) then goes through the hierarchy.

### Physics and Traffic

`plan_bodies` turns each city into one static Box3D body with one box per building wing and ground tile (friction 0.5); see [Chapter 6](06-physics.md). Traffic (`plan_traffic`, seed `city.seed ^ 0x7a3f1c9b`) places `traffic` cars on random street lines and directions of travel, at 9–15 m/s and in one of five colors. In the editor they stand parked; in Play and Simulate they are kinematic Box3D bodies that drive along their lane, jump to the other side at the edge of the city, and stop when the player stands less than 10 m ahead of them in the lane. All cars are a single instanced mesh.

`city_street_point(city, rng)` returns a random point on a street line, for example for scattering orbs.

## 5.13 Runtime Shapes and Built-In Meshes

### `shape_mesh`

A static mesh in a level (`MeshComponent`) has a shape: `box`, `sphere`, `cylinder`, `cone`, `capsule`, `plane`, `torus`, `ramp` or `mesh`. `shape_mesh(mc)` generates a mesh for it, centered on the actor origin, with white vertex colors (the material colors it). Box, plane and ramp get UVs in meters × `tiling`, so that a texture appears the same size on large and small surfaces; the round shapes come from `primitives` with scaled UVs, with cylinder and cone shifted down by half their height. The ramp is a wedge, full height at +X, zero at −X.

In addition there are `shape_bounds`, `shape_hull_points` (points for convex hulls of cylinder, cone, ramp and torus) and `fit_collider`, which chooses a matching collider for the shape (box, sphere, capsule, otherwise hull). That is the subject of [Chapter 6](06-physics.md).

### Built-In Meshes

The `mesh` shape uses `MeshComponent::asset` to refer to a triangle mesh compiled into the engine (`include/kern/game/builtin_meshes.h`). Today these are the parts of the Micro Duck by Pollen Robotics (CC BY-NC-SA 4.0, `third_party/microduck`), generated by `tools/micro_duck_import.py`. The names start with `duck/`, for example `duck/trunk_base`, `duck/neck`, `duck/upper_leg_left`, `duck/ankle_right` or `duck/roller/passive_LF_wheel`; `builtin_mesh_names()` returns the complete list.

They are stored compactly:

- positions as 16-bit values quantized over the bounding box (`lo`, `hi`),
- normals as 8-bit values × 127 (renormalized after decoding),
- sRGB colors as 8 bits,
- 16-bit indices,
- plus the convex-hull points for collision bodies.

`builtin_mesh(name)` decodes on first access and keeps the result for the lifetime of the program. Natively a mutex protects the cache, because the Lehrer builds worlds on several threads at the same time ([Chapter 14](14-lehrer.md)); WebAssembly runs on one thread, so there is no mutex there (`#if !defined(__wasi__)`). `shape_mesh` scales a built-in mesh per axis by `size` (normals by the reciprocal) and flips the winding if the overall scale is negative. An unknown name yields a 10 cm cube, so that the error is visible in the level and can be fixed.

How the duck is assembled from these parts is described in [Chapter 13](13-micro-duck.md).

---

© 2026 Christian Heinrich Hohlfeld · All rights reserved · [Imprint and rights](00-imprint.md)
