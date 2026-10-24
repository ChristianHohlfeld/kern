# 6 Physics

Kern simulates rigid bodies with [Box3D](https://github.com/erincatto/box3d), Erin Catto's 3D
successor to Box2D v3. Box3D sits unmodified under `third_party/box3d` and is compiled into the same
WebAssembly module as the renderer and the game runtime. On top of it sits a thin C++ layer,
`kern::physics` (`include/kern/physics/physics.h`, `src/physics/physics.cpp`). It is the **only
place** where Kern talks to Box3D.

This chapter describes that layer completely: world, bodies, shapes, materials, joints, queries,
contacts, the mouse grab and the character mover. After that come time step, sub-steps and
determinism, the binding rules from `docs/physics.md`, and finally the servo model
(`game/servo.cpp`) on which Plumo in [Chapter 13](13-plumo.md) runs.

One ground rule up front, because it shapes everything else: **Box3D is never adapted to Kern's
needs.** Neither its source nor its constants are changed, and Kern does not tweak solver parameters
to "make a model work". If a model does not fit Box3D's working range, the model is adapted (scaled,
masses redistributed, sub-steps increased), not the physics. Whatever a real device does beyond
rigid-body physics, such as a servo with a controller and friction, is a small device model of its
own in real units (Section 6.17).

The basic concepts this chapter assumes (rigid-body state, integration, collision, contacts,
constraints, solvers, servos as control loops, determinism) are explained in
[Appendix C](C-physics.md).

![The Physics Lab sample level in Kern Studio: stack, pendulum, door, rope bridge, turntable, lift and rover](../buch/bilder/studio-physics-lab.png)

---

## 6.1 Structure of the layer

```
include/kern/physics/physics.h   World, Body/Shape/Joint handles, settings, MouseGrab,
                                 CharacterMover, hull_mesh
src/physics/physics.cpp          implementation on top of the Box3D C API
third_party/box3d                Box3D (unmodified)
```

Conventions of the layer (header comment of `physics.h`):

- Meters, seconds, kilograms; **Z points up**; quaternions in the order (x, y, z, w) – the same
  conventions as the renderer.
- Angles in the physics layer are **radians**. Degrees exist only in level files and in the Details
  panel; the game runtime converts when building.
- Handles (`Body`, `Shape`, `Joint`) are plain 64-bit values into which Box3D's IDs are packed. `0`
  means "none", and `valid()` checks exactly that.
- A `World` owns everything created in it. When the world is destroyed (including in its
  destructor), everything is released.
- Failed calls return an invalid handle or `false` and set `last_error()` (see
  [Chapter 3](03-foundations.md)). All inputs are checked for finiteness: a pose containing `NaN`, a
  negative radius or a flat hull never reaches Box3D.

The layer is built with `kern/build.sh --physics` ([Chapter 2](02-building-and-testing.md)).

---

## 6.2 Units and sizes

Box3D works with tolerances that are fixed **in meters**
(`box3d:include/box3d/constants.h:47-97`):

| Tolerance | Value |
|---|---|
| Linear slop | 5 mm |
| Speculative distance | 20 mm |
| AABB margin | up to 50 mm |
| Contact recycling | 50 mm |

This leads to the first rule: **moving bodies belong between 0.1 m and 10 m, never below 1 cm**
(`box3d:docs/loose_ends.md:56-58`, `box3d:docs/overview.md:171-181`). A body whose links are on the
order of the tolerances can no longer be held together cleanly by the solver.

The game runtime enforces this when building: shapes of moving bodies are raised to at least 1 cm
edge length (spheres: 5 mm radius), static ones may be as thin as 2 mm (`add_shape` in
`src/game/runtime.cpp`).

*[measured]* Plumo at real size (links of 2–4 cm): the trunk sank 9 cm through the legs
within 10 frames, even with welded joints. At 4× size the joints hold.

### Scale instead of adjusting tolerances

Box3D offers `b3SetLengthUnitsPerMeter`, which changes the tolerances. Kern does **not** use it: it
applies to the *whole* world, and in a Kern level cars and small robots stand side by side. Instead
the rule is (`box3d:docs/loose_ends.md:59-61`): **convert units in exactly one place, at the boundary
to Box3D**, never scattered across the simulation code.

Kern's rule for small real-world models: scale the whole model by a factor S, each quantity
according to its dimension (Froude similarity – poses and gait stay the same). For Plumo,
S = 4 (`game::kWalkerScale` in `include/kern/game/walker.h`, in the code as `kDuckScale`):

| Quantity | Factor | at S = 4 | Remark |
|---|---|---|---|
| Length | S | ×4 | |
| Time | √S | ×2 | 1 real second = 2 seconds of world time |
| Mass | S³ | ×64 | same density |
| Moment of inertia (kg·m²) | S⁵ | ×1024 | `scaled()` in `src/game/walker.cpp` |
| Torque, stiffness (N·m/rad) | S⁴ | ×256 | |
| Damping (N·m·s/rad) | S^4.5 | ×512 | |
| Velocity | √S | ×2 | |
| Acceleration, gravity | 1 | ×1 | g stays 9.81 m/s² |
| Frequencies (springs, contact and joint softness) | 1/√S | ×½ | |

**Is the 4× duck still the real robot?** Yes. Scaling is a change of units, not a bigger robot: if
every quantity is scaled according to its dimension, Box3D computes the same motion. Everything that
goes outside is in real units: `walker_probe`, the networks' observations, logs and calibration
([Chapter 16](16-calibration.md)).

The test `physics_scale_is_a_unit_system` (`tests/test_main.cpp`) builds a small servo leg (72 g
trunk, two 5 cm links, spring servos with friction) at 1×, 4× and 8× size:

- **Hanging freely** (joints, springs, friction, gravity): 1× and 4× run **bit for bit identically**.
  The factors are powers of two, so floating-point rounding is identical.
- **On the ground, bending the knees:** here Box3D's fixed lengths (linear slop 5 mm, speculative
  distance 20 mm) come into play, and they do not scale. 4× and 8× differ by 0.23 mm, 1× and 4× by
  0.27 mm. At 4×, the tolerances correspond to 1.25 mm and 5 mm on the real robot – finer than at
  real size.

Whatever is fixed in real units is therefore **calibrated, not scaled**:

| Value in the world | corresponds on the real robot to |
|---|---|
| Contact stiffness 30 Hz, damping 10 | 60 Hz, damping 10 |
| Contact push-out speed 3 m/s | 1.5 m/s |
| Restitution threshold 1 m/s | 0.5 m/s |
| Joint softness of servo joints 240 Hz, damping 2 | 480 Hz, damping 2 |

Deviations from the real robot therefore do not come from the scaling but from the models (servo,
contact, masses). These are matched against measurements on the real device and corrected later
through logs ([Chapter 16](16-calibration.md)). A robot of a different size simply gets a different
S.

---

## 6.3 The world

```cpp
physics::WorldSettings ws;          // defaults: see table
physics::World world;
if (!world.create(ws)) { /* last_error() */ }
...
world.step(1.0f / 60, 4);           // one fixed step with 4 sub-steps
```

`physics::WorldSettings` corresponds to Box3D's `b3WorldDef`:

| Field | Default | Meaning |
|---|---|---|
| `gravity` | (0, 0, −9.80665) | m/s², must be finite |
| `enable_sleep` | `true` | resting islands go to sleep |
| `enable_continuous` | `true` | continuous collision (see 6.15, "Continuous collision, events, sleep") |
| `restitution_threshold` | 1 m/s | slower impacts do not bounce |
| `restitution_iterations` | 2 | 0..63 |
| `restitution_propagation` | `false` | Box3D's `enableRestitutionPropagation` |
| `hit_event_threshold` | 1 m/s | approach speed above which Box3D reports hit events |
| `contact_hertz` | 30 Hz | contact stiffness ("contact softness") |
| `contact_damping_ratio` | 10 | contact damping |
| `contact_speed` | 3 m/s | upper limit at which overlaps are pushed out |
| `maximum_linear_speed` | 400 m/s | Box3D's speed limit |

Values ≤ 0 for the contact quantities and the maximum speed leave Box3D's default in place.

`World::create` always sets two things itself:

- **`workerCount = 1`.** WebAssembly runs single-threaded; Box3D gets no task scheduler.
- **The friction callback** `pair_friction` (Section 6.6) for surface pairs.

Box3D keeps its worlds in a global table; creating and destroying are not thread-safe. Natively,
`WorldTableLock` (a `std::mutex`) protects these calls, because the Lehrer (Kern's trainer) runs one
world per environment on several threads ([Chapter 14](14-lehrer.md)). In WebAssembly the lock is
empty.

### One step

`World::step(dt, substeps)`:

1. clears the event lists of the previous step,
2. ignores invalid `dt` (≤ 0, not finite),
3. calls `b3World_Step` with `substeps` clamped to 1..64,
4. increments `step_count()`,
5. collects the contact, hit, sensor and joint events of this step (Section 6.10).

The fixed time step and the number of sub-steps are not chosen by the physics layer but by its
caller – in games, `GameWorld::step` (Section 6.13).

Further world queries: `valid()`, `body_count()`, `awake_body_count()`, `step_count()`,
`gravity()`.

---

## 6.4 Bodies

```cpp
physics::BodySettings bs;
bs.type = physics::BodyType::dynamic;
bs.position = {0, 0, 3};
bs.user = 42;                       // free to use, returned by body_user()
const physics::Body ball = world.create_body(bs);
```

`BodyType` knows `fixed` (static), `kinematic` and `dynamic`. The fields of `BodySettings`:

| Field | Default | Meaning |
|---|---|---|
| `type` | `dynamic` | body type |
| `position`, `rotation` | origin, identity | initial pose (finite, otherwise an error) |
| `linear_velocity`, `angular_velocity` | 0 | initial velocities (m/s, rad/s) |
| `linear_damping`, `angular_damping` | 0 | Box3D's built-in (implicit) damping, ≥ 0 |
| `gravity_scale` | 1 | factor applied to gravity |
| `enable_sleep` | `true` | may sleep |
| `awake` | `true` | starts awake |
| `enabled` | `true` | takes part in the simulation |
| `bullet` | `false` | continuous collision against dynamic bodies too |
| `user` | 0 | user value (`body_user()`) |

### State and forces

| Method | Effect |
|---|---|
| `pose(b)` / `set_pose(b, p)` | read / set the pose (teleport, see rules) |
| `set_target_pose(b, p, dt)` | move a kinematic body so that it reaches `p` after `dt`; wakes bodies it touches |
| `linear_velocity`, `angular_velocity`, `set_linear_velocity`, `set_angular_velocity` | velocities |
| `apply_force(b, f, p)`, `apply_force_to_center(b, f)`, `apply_torque(b, t)` | continuous loads, valid for **one** step |
| `apply_linear_impulse(b, i, p)`, `apply_linear_impulse_to_center(b, i)`, `apply_angular_impulse(b, i)` | impulses (N·s) |
| `set_awake`, `awake` | sleep state |
| `mass(b)`, `body_type(b)`, `set_body_type(b, t)` | mass, type (changing the type is expensive) |
| `local_point`, `world_point`, `world_vector` | coordinate conversion |
| `body_user(b)`, `shape_user(s)`, `shape_body(s)` | user values, ownership |
| `set_motion_locks(b, locks)` | lock translation / rotation per world axis (`MotionLocks`) |

All force and impulse functions wake the body.

### Mass and inertia

Without intervention, the shapes and their densities determine mass, center of mass and inertia.
There are two ways to override that:

- **`set_mass(b, m)`** sets only the mass and scales the inertia of the shapes by the same ratio. The
  body thus stays self-consistent.
- **`set_mass_data(b, m, center, inertia[6])`** sets everything: mass (kg), center of mass
  (body-local) and the full inertia tensor about the center of mass in body axes, in the order
  `xx yy zz xy xz yz` (kg·m²). This is the path for imported robots (MJCF, URDF): the description
  supplies the mass properties, and the collision shapes then only determine contact. The diagonal
  must be positive, otherwise nothing happens.

---

## 6.5 Shapes

Shapes are attached to exactly one body. A body can carry any number of shapes (a *compound*).

```cpp
physics::ShapeSettings ss;
ss.kind = physics::ShapeKind::sphere;
ss.radius = 0.5f;
ss.material.friction = 0.8f;
const physics::Shape s = world.create_shape(ball, ss);
```

`ShapeKind` and what each shape reads:

| Kind | Fields | Implementation |
|---|---|---|
| `box` | `size` (full edge lengths, all > 0), `center`, `rotation` | a transformed box hull (`b3MakeTransformedBoxHull`) |
| `sphere` | `radius` > 0, `center` | Box3D sphere |
| `capsule` | `radius` > 0, `length` ≥ 0, `center`, `rotation` | capsule along the shape's Z axis; `length` is the distance between the hemisphere centers |
| `hull` | `points` (≥ 4), `center`, `rotation` | convex hull (`b3CreateHull`, reduced to Box3D's maximum vertex count) |
| `mesh` | `points` (vertices), `indices` (3 per triangle, counter-clockwise) | triangle mesh, **only on static bodies** |

What is rejected (invalid handle, `last_error()` explains why): non-finite placement, radius ≤ 0,
negative capsule length, a box with an edge length ≤ 0, fewer than four hull points or a
flat/degenerate hull, a mesh on a non-static body, indices outside the vertex range, or an index
count that is not whole triangles.

For meshes, Kern welds vertices with a 0.5 mm tolerance and lets Box3D identify interior edges
(`identifyEdges`) – this avoids "ghost collisions" at edges between two triangles. The mesh data
belongs to the world and is released after it.

### Material and filter

Every shape carries a `ShapeMaterial`:

| Field | Default | Meaning |
|---|---|---|
| `density` | 1 kg/m³ | at least 0.0001 |
| `friction` | 0.6 | Coulomb friction coefficient, ≥ 0 |
| `restitution` | 0 | bounciness 0..1 |
| `rolling_resistance` | 0 | rolling resistance, ≥ 0 (Section 6.7) |
| `surface` | `Surface::none` | surface material for pair friction (Section 6.6) |
| `sensor` | `false` | overlap only, no contact |
| `category_bits`, `mask_bits` | 1, all bits | collision channels: A and B collide if each one's category is in the other's mask |
| `group_index` | 0 | the same positive group always collides, the same negative group never |
| `contact_events`, `sensor_events`, `hit_events` | `true`, `true`, `false` | which events this shape generates |
| `user` | 0 | user value (`shape_user()`) |

The game runtime sets the event switches selectively (see 6.10), because events cost time.

### `hull_mesh`

`physics::hull_mesh(points, out, color)` produces the flat-shaded triangles of the convex hull using
Box3D's hull builder – the same hull that collides. The game runtime uses it to draw hull colliders
as debug lines (`shape_lines` in `src/game/runtime.cpp`). For fewer than 4 or more than 4096 points,
or a flat point set, it returns `false` (test `physics_hull_mesh`).

---

## 6.6 Surface materials with pair friction

By default, Box3D combines the friction of two shapes as **√(a · b)** and restitution as the
**maximum**. For most things that is enough. But it is not right for every real pair: two materials
do not simply combine: *TPU on cotton has its own measured value, not the mean of TPU on TPU and
cotton on cotton* (comment in `physics.h`).
For such pairs, Kern knows surface materials with their own measured pair friction.

### The model

```cpp
enum class Surface : u8 { none, cloth, tpu, floor };
const char* surface_name(Surface s);                  // "", "cloth", "tpu", "floor"
bool parse_surface(const std::string& name, Surface& out);
/// Friction of a pair, or < 0 if the pair has no value of its own.
f32 surface_friction(Surface a, Surface b);
```

The pair table (`surface_friction` in `src/physics/physics.cpp`, symmetric):

| Pair | Friction | Origin according to the source |
|---|---|---|
| cloth – cloth | 0.5 | cotton knit on cotton knit (yarn on yarn 0.3, knit on itself 0.4–0.6) |
| cloth – tpu | 0.8 | TPU (the soft printed pads of the beak) on cotton (elastomers on textile 0.6–1.0, the softer, the higher) |
| cloth – floor | 0.4 | cotton on wooden or laminate floor (fabric on wood 0.3–0.5) |

These are static friction values for dry pairs; the solver knows only one Coulomb coefficient.

### How it works in Box3D

Kern does not change Box3D for this; it uses two extension points that Box3D provides:

1. `create_shape` writes the material into the shape as `baseMaterial.userMaterialId`.
2. `World::create` sets the world's friction callback:

```cpp
float pair_friction(float fa, uint64_t ma, float fb, uint64_t mb) {
  const f32 f = ma && mb && ma <= 3 && mb <= 3
                    ? surface_friction(static_cast<Surface>(ma), static_cast<Surface>(mb)) : -1.0f;
  return f >= 0 ? f : sqrtf(fa * fb);
}
```

Box3D calls it on every contact update with the friction values and material IDs of both shapes
(`b3UpdateConvexContact` in `box3d:src/contact.c`, mesh contacts in `src/mesh_contact.c`). If the
pair has its own value, that value applies; otherwise the usual combination √(a · b). A pair in which
one side is `none` therefore always uses the shapes' friction values. Restitution and rolling
resistance are unaffected.

> If you add a surface: the callback checks the IDs against the largest enum value (`<= 3`). A new
> value in `Surface` therefore also needs this bound adjusted, plus a name in `surface_name` /
> `parse_surface` and its pairs in `surface_friction`.

### In the level

Three places carry a surface, each as a string (`""`, `"cloth"`, `"tpu"`, `"floor"`):

| Place | Field (C++) | Key in the `.klevel` |
|---|---|---|
| automatically generated collider of a mesh | `MeshComponent::surface` | `components.mesh.surface` |
| custom collider | `ColliderComponent::surface` | `components.colliders[i].surface` |
| endless ground | `WorldSettings::ground_surface` | `world.groundSurface` |

The keys are only written when they are not empty. When building, `GameWorld::create_body`
translates the string with `parse_surface`; an unknown name yields `none`. As of this code, the
Details panel has no field of its own for the surface; it is set in the level document.

Plumo uses all three ([Chapter 13](13-plumo.md)):

- The lumps of the sock (`plumo_sock_actors`) are `cloth`.
- The printed pads of the beak (hull colliders with friction > 0 that are not soles) are `tpu`.
- In Plumo sample level, the wooden floor (`floor.mesh->surface`) is `floor`. The level also
  sets `ground_surface = "floor"` but switches the endless ground off (`ground = false`) – the wooden
  floor is the ground there.

The duck's soles and housings have **no** surface: they keep the friction values the policy was
trained with (soles 1.0 on a ground of 1.0 gives √(1 · 1) = 1).

![Plumo sample level: wooden floor, ball and two socks with their own pair friction](../buch/bilder/duck-level.png)

---

## 6.7 Friction, restitution, rolling resistance

**Contact friction** is set per shape (`ColliderComponent::friction`, default 0.6). In the editor:
Details › Mesh › **Physics** (friction, restitution, rolling resistance of the automatically
generated collider, directly above the render material) or Details › Collider for custom colliders.
The endless ground: World Settings › Physics (Box3D) › Ground friction / Ground bounciness.

**Restitution** (0..1) is combined by Box3D as the maximum of the two shapes. Impacts below the
restitution threshold (`restitution_threshold`, default 1 m/s) do not bounce.

**Contact softness** (Box3D's world definition) is set in the World Settings: Contact stiffness
(30 Hz), Contact damping ratio (10), Contact push-out speed (3 m/s).

**Rolling resistance** brakes rolling across the contact normal. In the bundled Box3D version
(`box3d:src/contact.c`, `src/contact_solver.c`):

- The contact gets the **maximum** of the two rolling resistances, multiplied by the larger
  "rolling radius" of the two shapes: the radius for spheres and capsules, a quarter of the inner
  radius for hulls.
- In the solver it limits the rolling impulse to rolling resistance × total normal impulse of the
  contact.

`docs/physics.md` and the comment in `physics.h` say the same: rolling resistance acts on spheres
and capsules (radius) and on convex hulls (a quarter of their inner radius; boxes are hulls in
Box3D too). It is used for round things: the ball in Plumo level (0.03,
"a rubber ball on a wooden floor rolls to a stop"), the rover's tires in the Physics Lab (0.02) and
the sock lumps (0.5: "a lump of fabric does not roll; round lumps would, and the pile would crawl").

**Robot feet** are wide, flat boxes or hulls with high friction.

---

## 6.8 Joints

Kern maps Box3D's native joints. A `JointSettings` describes every joint; each type reads the fields
it needs.

```cpp
physics::JointSettings j;
j.type = physics::JointType::revolute;
j.body_a = frame_body;  j.body_b = door;
j.frame_a = physics::frame_from_axis(world.local_point(frame_body, hinge), {0, 0, 1}, false);
j.frame_b = physics::frame_from_axis(world.local_point(door, hinge), {0, 0, 1}, false);
j.enable_limit = true;  j.lower = -1.7f;  j.upper = 1.7f;   // radians
const physics::Joint joint = world.create_joint(j);
```

### Local frames and axes

Every joint connects two bodies via one local frame per body (`frame_a`, `frame_b`, body-local).
Box3D measures B relative to A.

- Revolute, spherical and parallel joints work **about the Z axis** of the frame.
- Prismatic and wheel joints work **along the X axis** of the frame.
- `physics::frame_from_axis(anchor, axis, x_axis)` builds a frame at the anchor point whose X or Z
  axis points along `axis` (also for the exactly opposite direction).
- The zero angle is where the two frames coincide.

### Types

| `JointType` | Box3D | Spring | Limit | Motor |
|---|---|---|---|---|
| `revolute` | Revolute | `enable_spring`, `hertz`, `damping_ratio` | `lower`/`upper` (rad) | `motor_speed` (rad/s), `max_motor_torque` |
| `distance` | Distance | yes | `lower`/`upper` = min./max. length; `length` = rest length (≥ 1 mm) | `motor_speed` (m/s), `max_motor_force` |
| `prismatic` | Prismatic | yes | `lower`/`upper` (m) | `motor_speed` (m/s), `max_motor_force` |
| `wheel` | Wheel | suspension (`hertz`, `damping_ratio`) | suspension travel `lower`/`upper` (m) | drive `motor_speed` (rad/s), `max_motor_torque` |
| `weld` | Weld | softness `linear_hertz`, `angular_hertz` (+ damping; 0 Hz = rigid) | – | – |
| `motor` | Motor | spring toward frame alignment (`linear_hertz`, `angular_hertz`, `max_spring_force/_torque`) | – | velocity drive (`linear_velocity`, `angular_velocity`, `max_velocity_force/_torque`) |
| `spherical` | Spherical | yes | cone with opening `upper` | `motor_velocity` (vector), `max_motor_torque` |
| `parallel` | Parallel | `hertz`, `damping_ratio` | – | `max_torque` |
| `filter` | Filter | – | – | – (only prevents two bodies from colliding) |

For revolute, prismatic and wheel joints, lower and upper limits are swapped if necessary, and
negative maximum forces are set to 0. A rope is a limited distance joint; the editor lists it as
"Distance / rope".

Common fields: `collide_connected` (default: connected bodies do not collide), `user` and the
**solver's joint softness** `constraint_hertz` / `constraint_damping_ratio` (0 = Box3D's default,
60 Hz and 2). A joint that would connect a body to itself, or one with invalid bodies, is rejected.

### At runtime

| Method | Effect |
|---|---|
| `set_joint_motor(j, enable, speed, max_force)` | motor of revolute (rad/s, N·m), prismatic (m/s, N), distance (m/s, N), spherical (torque only); wakes the bodies |
| `joint_angle(j)` | revolute: angle of B relative to A (rad, from the reference angle); otherwise 0 |
| `joint_speed(j)` | revolute: angular velocity of B relative to A about the hinge axis (rad/s); otherwise 0 |
| `set_joint_spring(j, target, stiffness, damping)` | revolute: spring-damper toward the target angle in physical units (see below) |
| `joint_force(j)`, `joint_torque(j)` | constraint force (N) and constraint torque (N·m) of the joint |
| `set_joint_thresholds(j, force, torque)` | above these values `joint_events()` reports the joint (0 = never) |
| `destroy_joint(j, wake)` | removes the joint, wakes the bodies |

### Joint forces and breaking

`joint_force` and `joint_torque` return Box3D's constraint force and torque from the last step. With
`set_joint_thresholds`, Box3D reports a joint in `joint_events()` after the step as soon as it
exceeds the threshold (internally `FLT_MAX` for "never"). The game runtime turns this into breakable
joints: `JointComponent::break_force` / `break_torque` are set as thresholds, and
`GameWorld::handle_events` destroys reported joints, counts them (`SimStats::broken_joints`), writes
"Joint on "…" broke" to the log and plays the break sound. The rope bridge in the Physics Lab breaks
this way (test `game_world_physics_lab`).

### The joint spring in physical units

Box3D sets springs in hertz and damping ratio, relative to the effective mass of the joint. Servos
and robot descriptions work in N·m/rad and N·m·s/rad. `set_joint_spring` converts at the boundary,
exactly as Box3D does internally in `revolute_joint.c`:

- axial effective mass m = 1 / (a · (I_A⁻¹ + I_B⁻¹) · a), with the hinge axis a in world coordinates
  (Z axis of frame A) and the inverse world inertias of the bodies (static and kinematic bodies
  contribute 0),
- ω = √(k / m), spring frequency = ω / 2π,
- damping ratio ζ = c / (2 m ω),
- target angle clamped to ±π (Box3D checks |target| ≤ π).

Because the effective mass depends on the current configuration, the function is called **every
step**. The spring is solved implicitly by Box3D (soft step) and is stable at any stiffness.
`stiffness ≤ 0` turns it off.

---

## 6.9 Queries: rays, spheres, explosions

| Method | Result |
|---|---|
| `cast_ray(origin, translation, mask, ignore)` | nearest hit along `origin + translation`; sensors are skipped, as are the shapes of the body `ignore` (e.g. the body in which the ray starts) |
| `cast_ray_ignoring(origin, translation, ignore[], mask)` | as above, with a list of ignored bodies (a robot sensor does not see its own links) |
| `cast_sphere(origin, radius, translation, mask, ignore)` | "thick ray": a sphere is swept along `translation` – ground probes that do not miss any edge |
| `explode(center, radius, falloff, impulse_per_area, mask)` | radial impulse on spheres, capsules and hulls (`b3World_Explode`); negative = implosion |
| `push_body(b, point, normal, velocity)` | impulse that a mover moving at `velocity` applies to a dynamic body (only as much as needed so the body at that point is not slower than the mover along the normal) |

`RayHit` contains `hit`, `shape`, `body`, `point`, `normal` and `fraction` (0..1 along the segment).
`mask` filters via the shapes' category bits.

For the character mover there are two further queries on Box3D's mover API: `cast_mover` (how far a
capsule can travel, 0..1) and `collide_mover` (the planes it touches; at most 32).

---

## 6.10 Contacts and events

After every `step`, the events of **that** step are available:

| List | Type | Content |
|---|---|---|
| `contact_begin()`, `contact_end()` | `TouchEvent` | shapes start / stop touching |
| `sensor_begin()`, `sensor_end()` | `TouchEvent` | `shape_a`/`body_a` = sensor, `shape_b`/`body_b` = visitor |
| `hits()` | `HitEvent` | impact above the hit threshold: point, normal, approach speed |
| `joint_events()` | `Joint` | joints above their force/torque threshold |

Shapes may have been destroyed since the step; the events therefore also carry the bodies, and for
invalid shapes the body is 0.

**Events are not free.** Box3D reports contact, sensor and hit events only for shapes that ask for
them. The game runtime therefore enables them only where the game reads them
(`GameWorld::create_body`):

- Sensor events: on sensors, on player bodies, and on all non-static shapes if the level has logic
  graphs.
- Contact events: only on the player (pickups and triggers).
- Hit events: on dynamic bodies if they are the player, have an impact sound, or the level has logic.

### Contact points and touches

Two queries read Box3D's contact data of a body directly:

- **`contact_points(body, out)`** returns all touching manifold points as `ContactPoint` (world
  position, normal from shape A to B, separation – negative on penetration – and the normal impulse
  of the last sub-step). `GameWorld::debug_lines` uses it to draw contacts (show flag
  "Contact Points" in the viewport).
- **`touching(body, out)`** returns the bodies that `body` is currently touching (manifolds with
  points), each with the sum of the normal impulses of the last sub-step (N·s) – what a contact
  sensor reports: a sole on the floor, one link on another. The walker probe uses this for foot
  contact and self-collision ([Chapter 13](13-plumo.md)).

---

## 6.11 MouseGrab: dragging bodies with the mouse

`physics::MouseGrab` drags a dynamic body with a spring – the same technique as in the Box3D
samples:

```cpp
physics::MouseGrab grab;
if (grab.begin(world, body, hit_point)) {   // spring frequency default 7.5 Hz; 5th argument hold_rotation
  grab.set_target(cursor_point);
  // every step, before World::step:
  grab.update(h);
  world.step(h, substeps);
}
grab.end();
```

How it works:

- `begin` creates a **kinematic "mouse" body** at the grab point (never sleeps) and a **motor joint**
  to the grabbed body, whose frame B sits at the grab point inside the body.
- The linear spring has `stiffness_hertz` (default 7.5 Hz) and damping ratio 1 (critically damped).
  Its maximum force is limited to 100 × mass × g (g at least 1 m/s²). The angular spring is off: the
  body may tilt and swing at the grab point.
- With `hold_rotation = true` the grab also keeps the orientation the body had when grabbed, as a
  hand does: an angular spring at half the frequency, critically damped, maximum torque a quarter of
  the maximum force (a quarter-metre lever). `GameWorld::begin_grab` uses this for a Plumo's
  trunk, so that it stays upright when lifted instead of swinging on a point
  ([Chapter 13](13-plumo.md)).
- `update(dt)` moves the mouse body to the target via `set_target_pose`. The body stays fully in the
  physics: it bumps into things, tilts, and flies on with its momentum when released.
- Static and kinematic bodies cannot be grabbed (`begin` returns `false`).
- If the body disappears, `update` ends the grab by itself.

Studio uses the grab in Simulate, after Eject and with the observer camera
(`GameWorld::begin_grab` / `move_grab` / `end_grab`; tests `physics_joints_and_grab`,
`studio_observer_camera_grabs_bodies`).

---

## 6.12 CharacterMover

`physics::CharacterMover` is a kinematic character controller on Box3D's experimental mover API
(`box3d:docs/character.md`, Box3D sample "Mover", Z up). The capsule runs **outside the solver**: it
does not fight against contacts but computes its own motion and pushes dynamic bodies it walks into.

`Settings` (defaults):

| Field | Default | Meaning |
|---|---|---|
| `radius` | 0.4 m | capsule radius |
| `height` | 1.8 m | feet to head |
| `step_height` | 0.45 m | how high the "pogo" spring carries the capsule above the ground (steps) |
| `max_speed` | 6 m/s | |
| `accelerate` | 10 1/s | |
| `friction` | 8 1/s | ground friction |
| `air_control` | 0.25 | fraction of acceleration in the air |
| `stop_speed` | 3 m/s | below this, friction acts as a constant |
| `jump_speed` | 6 m/s | |
| `sprint` | 1.6 | speed factor when sprinting |
| `gravity` | 20 m/s² | its own gravity (snappier than the world's) |
| `max_slope_deg` | 50° | steeper surfaces are not ground |
| `push_strength` | 1 | 0 = dynamic bodies are not pushed |
| `mask_bits` | all | |

One `step(world, dt, wish, jump, sprint, self)`:

1. Ground friction (linear above `stop_speed`, constant below), acceleration in the wish direction
   up to the wish speed, jump, gravity.
2. **Pogo spring:** a sphere probe (`cast_sphere`, 0.6 × radius) below the lower hemisphere measures
   the distance to the ground; a damped spring (4 Hz, damping ratio 0.7, integrated implicitly) holds
   the capsule at `step_height` – this is how it walks up steps and ramps.
3. Up to five iterations: `collide_mover` collects contact planes, `b3SolvePlanes` solves the motion
   against them, `cast_mover` limits it, and the remainder is clipped against the planes
   (`b3ClipVector`) – pushing against a wall does not build up velocity.
4. Dynamic bodies in the touched planes get a push via `push_body`.

`position()` is the feet, `planes()` the most recently touched planes (triggers and pickups the mover
walked into), and `jump(speed)` makes it take off. The game runtime uses the mover for character and
soldier players and makes their bodies kinematic for that ([Chapter 8](08-game-runtime.md); test
`game_character_mover`).

---

## 6.13 Time step and sub-steps

### Fixed time step

Box3D **never gets a variable dt**. The step rate is a level setting (`WorldSettings::step_hz`,
default 60 Hz, allowed 15..480 Hz; never use coarser than 30 Hz). `GameWorld::step` follows Glenn
Fiedler's accumulator ([Fix Your Timestep](https://gafferongames.com/post/fix_your_timestep/)):

```cpp
const f64 h = 1.0 / clamp(level_.world.step_hz, 15.0f, 480.0f);
accumulator_ += clamp(dt, 0.0, 0.25);                     // limit long frames
const u32 max_steps = std::ceil(0.1 / h);                 // at most 0.1 s of simulation per frame
while (accumulator_ >= h && steps < max_steps) { ... fixed_step(input, h); accumulator_ -= h; }
if (accumulator_ >= h) accumulator_ = std::fmod(accumulator_, h);   // falling behind: whole steps are dropped
const f32 alpha = accumulator_ / h;                       // remainder for interpolation
```

- The frame supplies time; the simulation consumes it in fixed steps h; the remainder stays in the
  accumulator.
- A long frame (hitch, background tab) is clamped to 0.25 s, and at most 0.1 s of simulation runs per
  frame – against the "spiral of death", in which a step costs more than h and the simulation never
  catches up. If it still falls behind, whole steps are dropped; the phase is preserved.
- **Simulation time counts steps** (`step_count_`; `sim_time_ = step_count_ / step_hz`) instead of
  summing floating-point time, which would drift over the run.
- **Inputs apply per fixed step.** Controllers that compute less often (a policy at 50 Hz) count
  steps, not seconds.
- Identical inputs thus yield a bit-for-bit identical run
  ([Deterministic Lockstep](https://gafferongames.com/post/deterministic_lockstep/)).

### Interpolation when drawing

Before every fixed step, `GameWorld` remembers the pose of every non-static body. Drawing happens
between the two most recent physics states (`alpha` = remainder / h; position linear, rotation
normalized linear along the shorter path). Motion is thus smooth at any frame rate. The play camera
follows the player's **interpolated** pose (`presented_player_`). With the raw physics pose, fast
cars and balls jittered on screen; `studio_fast_player_does_not_shake_on_screen` guards against this.

### Sub-steps

Every fixed step is divided into sub-steps in Box3D (`WorldSettings::substeps`, default 4, levels
allow 1..16). Long joint chains need more: "long joint chains will stretch less with more sub-steps"
(`box3d:docs/simulation.md:155-159`).

- *[measured]* The duck's hip at 4 sub-steps: the joint gap grew to 5 cm under load and sprang back.
  At 16 sub-steps it stays below 1 mm.
- When Studio inserts a robot, it sets 16 sub-steps and the robot's step rate (Plumo: 100 Hz,
  `kWalkerStepHz`; `add_plumo` sets `kWalkerSubsteps = 16`) and reports it.

### Stiffness is capped by the sub-step rate

Box3D limits `constraintHertz` to 0.25 × the sub-step rate and contact hertz to 0.125 ×
(`box3d:src/joint.c:1484-1486`, `src/physics_world.c:1127-1129`). Raising `constraintHertz` therefore
only helps together with more sub-steps.

Springs stay below the Nyquist limit. `GameWorld::create_joints` limits every spring in the level to
`max_hertz = 0.5 × step_hz × substeps` (half the sub-step frequency). Servo joints get a joint
softness of `min(240 Hz, max_hertz)` with damping ratio 2.

---

## 6.14 Determinism

Kern is bit-for-bit deterministic – not only on the same machine, but across platforms (x86, ARM,
WebAssembly in every browser). Physics contributes to this:

- fixed time step, simulation time as a step counter, inputs per step (6.13);
- `workerCount = 1`: no ordering that depends on threads;
- Box3D on its scalar path (`BOX3D_DISABLE_SIMD`), as in WebAssembly;
- everything compiled with `-ffp-contract=off` (no FMA contraction of `a*b+c`);
- transcendental functions from Kern's own libm (`kern::dm::sin` etc., declared in
  `include/kern/core/dmath.h`, the musl implementations live in `third_party/musl_math`); `sqrt`,
  `fabs`, `floor` are exact in IEEE and stay `std::`. Box3D has its own deterministic trigonometric
  functions.

This is checked with a fingerprint: the duck walks 400 control ticks, every tick is hashed, and the
hashes must match `tests/determinism_fingerprint.txt` – natively
(`determinism_is_bit_identical_across_builds_and_platforms`), as WebAssembly
(`tests/lehrer_wasm_test.mjs`) and on ARM. Details in [Chapter 14](14-lehrer.md).
**Rule for new code that feeds the simulation:** `kern::dm::` instead of `std::` for sin, cos, tan,
asin, acos, atan, atan2, exp, expm1, log, log10, pow and hypot; no sums whose order depends on
threads; no `-ffast-math` options.

---

## 6.15 Rules and no-gos

`docs/physics.md` is binding for all code in Kern that creates bodies, shapes, joints or motors –
**read it before changing `src/physics` or `src/game/runtime.cpp`, or building a robot, a vehicle or
a ragdoll preset.** The rules rest on three sources: Erin's own documentation and samples (cited as
`box3d:file:line`), his blog posts and GDC talks on box2d.org, and measurements in Kern
(*[measured]*, each backed by a Kern test). Here they are collected in full, each with its reason.

### Sizes and units (§1)

| Rule | Why |
|---|---|
| MKS throughout (m, kg, s); radians in physics, degrees only in level files and the Details panel. | One unit system, one conversion. |
| Moving bodies between 0.1 m and 10 m, never below 1 cm. | Box3D's tolerances are fixed in meters (5 mm slop, 20 mm speculative). |
| Convert units in exactly one place, at the boundary to Box3D. | Scattered conversions make errors impossible to find (`box3d:docs/loose_ends.md:59-61`). |
| Scale small real models as a whole by S (lengths ×S, masses ×S³, torques ×S⁴, times ×√S). | Froude similarity: the same motion within Box3D's working range. |
| **No-go:** `b3SetLengthUnitsPerMeter`. | Changes the tolerances of the whole world; cars and robots share a level. |

### Mass ratios (§2)

| Rule | Why |
|---|---|
| Do not hang heavy bodies from light ones; keep neighboring masses within about 10:1. | Extreme ratios lead to "joint stretching" (Box2D v2.4 `docs/loose_ends.md`, `box3d:docs/loose_ends.md:133-134`). Erin's solution for a tank at 467:1: make light parts heavier and heavy parts lighter ([Discussion #132](https://github.com/erincatto/box3d/discussions/132)). |
| A rigid part is **one** body with several shapes. **No-go:** welding bodies together. | "the approximate solver cannot hold many bodies together rigidly" (`box3d:docs/simulation.md:1523-1526`). Kern welds children that have only a mesh into the nearest ancestor with a body as shapes. |
| Give light links a minimum mass instead of tweaking the solver. | The duck's 5 g hip brackets weigh at least 20 g at real size (×64 at S = 4). |

### Time step and sub-steps (§3)

| Rule | Why |
|---|---|
| Fixed time step, default 60 Hz, never coarser than 30 Hz. **No-go:** variable dt. | Stability and determinism (6.13). |
| Accumulator with clamping, interpolation, time as a step counter, inputs per step. | Smooth display, no spiral of death, bit-for-bit repeatability. |
| The camera follows the interpolated pose. | Otherwise fast bodies jitter on screen. |
| Long joint chains: more sub-steps (robots 16). | Less stretching (`box3d:docs/simulation.md:155-159`); *[measured]* hip gap 5 cm → < 1 mm. |
| Only raise `constraintHertz` together with more sub-steps. | Box3D caps it at 0.25 × the sub-step rate. |
| Springs below half the sub-step frequency (`max_hertz = 0.5 × step_hz × substeps`). | Nyquist; stiffer springs become unstable (Box3D "Distance Joint"). |

### Joints (§4)

| Rule | Why |
|---|---|
| **Build** every preset with joints **in its rest pose**; limits must contain zero. | The zero angle is where the frames coincide (`box3d:docs/simulation.md:1414-1417`). |
| Revolute joints rotate about the frame's Z axis; Kern builds the frame from the axis in the actor (`frame_from_axis`). | One convention instead of computing by hand. |
| Self-collision: connected bodies do not collide (without `collideConnected`); filter the rest with one negative collision group per robot, or filter joints for individual pairs. | This is how Erin's ragdoll does it (`box3d:shared/human.c:399-410`). |
| Leave the defaults `constraintHertz = 60` and damping 2 alone; Kern raises them only for servo joints, together with the sub-step limit. | `box3d:src/joint.c:30-31`; higher values without sub-steps have no effect or become unstable. |

### Motors, servos and friction (§5)

| Rule | Why |
|---|---|
| Always set a **finite** maximum motor torque or maximum motor force. | An unlimited motor is a rigid constraint without compliance (`box3d:docs/simulation.md:1419-1429`). |
| Position servos are the **joint spring** of the revolute joint with a target angle (`b3RevoluteJoint_SetTargetAngle`). | Solved implicitly, stable at any stiffness. |
| Stiffness (N·m/rad) and damping (N·m·s/rad) are physical servo values; `set_joint_spring` converts them at the boundary. | One conversion, like `revolute_joint.c`. |
| The holding torque arises from clamping the error to torque / stiffness. | This is how a Dynamixel's PWM saturates. |
| **Not** Erin's other recipe `motorSpeed = gain × error` with a maximum torque for robots with a trained policy. | That is rigid velocity control (`box3d:docs/simulation.md:1240-1243`); the policy learned a compliant PD servo. |
| **No-go: bypassing the physics.** No extra torques that "emulate" something; no set velocities on bodies with joints or contacts; no gyros, locks or support forces for robots. | They do not exist in the real world either – a policy that walks with helper forces will not walk on the real device. |
| A gearbox's rotor inertia (MuJoCo "armature") is a mass property: a disc on the joint axis carried by the link. Whatever Box3D cannot represent at all is documented, not faked. | `docs/walker.md` §6; a purely axial tensor would not be a real rigid body, and Box3D gains energy from it (`add_armature` in `walker.cpp`). |
| **Joint friction** is a motor with speed 0 and a small maximum torque. | Erin's recipe (`box3d:docs/simulation.md:1431-1434`); his ragdoll uses 5 N·m × a factor per joint (`box3d:shared/human.c:347-393`). |
| Contact friction per shape; Box3D combines two materials as √(a·b) (friction) and max (restitution); pairs with their own value via `Surface`. | 6.6, 6.7. |
| Rolling resistance for round things (spheres, capsules; on hulls it acts with a quarter of the inner radius); robot feet wide, flat, with high friction. | 6.7. |
| To keep a body softly upright: a **parallel joint** (hertz, damping, maximum torque), no overwritten velocities. | Erin's way (`box3d:shared/human.c:526-605`). |

### Forces, velocities, damping (§6)

| Rule | Why |
|---|---|
| Continuous loads as **forces, every step**. | Forces are reset after every step (`box3d:docs/simulation.md:1966`). |
| A jump is an impulse (`apply_linear_impulse_to_center`, m · Δv), not a set velocity. | Set velocities bypass the solver. For characters (all rotation axes locked) and kinematic bodies that is fine; on bodies with joints or contacts it fights the constraints and pumps energy in. *[measured]* A velocity-level "gyro" made the duck's trunk jump. |
| Use the built-in damping (`linear_damping`, `angular_damping`). **No-go:** a home-made `F = −c·v` with a large c. | Box3D's damping is implicit (`box3d:src/solver.c:93-94`); the explicit variant becomes unstable on light bodies. |

### Bodies, characters, kinematics (§7)

| Rule | Why |
|---|---|
| Create bodies at their final pose; do not teleport them in-game via `SetTransform`. | Creating at the origin and then moving costs twice (`box3d:include/box3d/box3d.h:551-554`). |
| Fix the body type at creation. | `SetType` is expensive. |
| Move kinematic bodies via velocity or `SetTargetTransform`. | A target slower than the sleep threshold is skipped (`box3d:include/box3d/box3d.h:580-584`). |
| Characters and soldiers on Box3D's character mover: clip velocity against the planes every step, a sensible capsule radius, no fast spinning. | `box3d:docs/character.md`. |
| Motion locks are soft constraints; Kern locks pitch and roll only for its own controllers (vehicles, characters, fliers), **never for walkers**. | A robot must be able to fall, otherwise it learns nothing real. |
| Imported robots get mass, center of mass and the full inertia tensor from the description (`BodyComponent::center_of_mass`, `inertia`, `set_mass_data`). | The collision shapes then only determine contact. |
| Joints that are not built in their zero position carry the angle of the build pose as `JointComponent::reference`. | The joint then reads the description's angle. |

### Continuous collision, events, sleep (§8)

| Rule | Why |
|---|---|
| Leave continuous collision on; fast player vehicles are bullets. | It only covers dynamic against static; bullets also against dynamic bodies. |
| Enable events only on shapes whose events the game reads. | In Box3D events are off by default and cost time (6.10); Kern's `ShapeMaterial`, by contrast, enables contact and sensor events by default, and the game runtime sets them selectively. |
| Players and robots never sleep. | A slow gait lies below the sleep threshold of 5 cm/s. |

### Checklist for a new articulated preset (§9)

1. Every moving part is ≥ 10 cm at real size. Otherwise scale the model by a factor.
2. One body per rigid segment; children that have only a mesh are welded in as shapes.
3. Neighboring masses within about 10:1, a minimum mass per link.
4. Build in the rest pose, limits around 0, axes in the actor's frame.
5. Servos as a joint spring with physical stiffness, damping and holding torque; joint friction as a
   motor with speed 0. No helper forces.
6. Set 8–16 sub-steps for the level, or raise them when inserting the preset.
7. Filter self-collision with a negative group.
8. Write a native test: the preset stands, its joints hold (gap < 1 % of the link length), and it
   performs its task (walks, drives, swings) – deterministically.

---

## 6.16 How the game runtime uses the physics

Games do not create bodies and joints by hand, but from a level ([Chapter 8](08-game-runtime.md)).
The path in brief:

- `game::plan_bodies(level)` plans which bodies and compound shapes the level needs (actors with a
  body component, welded-in children, implicit bodies for meshes, pickups and triggers, cities, the
  endless ground). Studio uses the same plan to draw the colliders in the editor.
- `GameWorld::create_body` turns each plan into `World::create_body` / `create_shape`, including
  material, surface, filter, events, mass, inertia and motion locks.
- `GameWorld::create_joints` turns `JointComponent`s into `JointSettings`: degrees → radians, frames
  from anchor and axis, reference angle, spring limit, servo softness, break thresholds.
- `GameWorld::fixed_step` drives controllers, kinematic animations, traffic, walkers, servos and the
  mouse grab in every step, then `World::step`, then the events.

---

## 6.17 The servo model – bridge to Plumo

Box3D computes rigid bodies, joints and contacts. Whatever a real device does beyond that – a servo
with a controller, friction and latency – is a small **device model** with parameters in real units,
the **profile** (`include/kern/game/servo.h`, `src/game/servo.cpp`). A servo type is a profile; a
different servo, motor or actuator is a different profile (and, if it behaves fundamentally
differently, a different model). Profiles are calibrated against measurements of the real device and
validated against other measurements ([Chapter 16](16-calibration.md)).

### The law

The position servo (a Dynamixel XL330 and its kind) as Kern simulates it:

- torque = clamp(K × (target − angle), −T, T) − D × velocity,
- plus Coulomb friction F in the joint,
- the rotor's inertia through the gearbox (armature A) on the joint axis,
- the command arrives `delay` seconds after it is sent.

`game::servo_step` implements this for **one** step with Box3D's own joint features – the same
function in Studio (`GameWorld::drive_servos`), in the Lehrer and on the calibration test bench:

```cpp
void servo_step(physics::World& world, const physics::Joint& joint, f32 target,
                f32 stiffness, f32 damping, f32 friction, f32 torque) {
  const f32 q = world.joint_angle(joint);
  const f32 k = max_of(1e-6f, stiffness);
  const f32 reach = torque > 0 ? torque / k : 1e9f;
  world.set_joint_spring(joint, q + clamp(target - q, -reach, reach), k, max_of(0.0f, damping));
  world.set_joint_motor(joint, friction > 0, 0, max_of(0.0f, friction));
}
```

- **Spring:** the joint spring pulls toward the target, whose distance from the current angle is
  clamped to T/K. K × error thus never exceeds the maximum torque T – like the servo's saturating
  PWM. The spring is implicit and stable at any stiffness.
- **Friction:** a motor with speed 0 whose maximum torque is the friction F (Erin's joint friction).
- All values in world units (scaled for walkers, see below).

`GameWorld::drive_servos` calls `servo_step` every physics step for every unbroken servo joint.

### Servos in the level

A hinge (`JointComponent`, `kind = hinge`) becomes a servo with `servo = true`:

| Field | Default | `.klevel` key |
|---|---|---|
| `servo_target` | 0° | `servoTarget` (degrees) |
| `servo_stiffness` | 50 N·m/rad | `servoStiffness` |
| `servo_damping` | 2 N·m·s/rad | `servoDamping` |
| `servo_friction` | 0.1 N·m | `servoFriction` |
| Maximum torque T | = `max_motor_force` | `maxMotorForce` |

Servo joints get a stiffer joint softness (`min(240 Hz, max_hertz)`, damping 2) so that light links
do not stretch (6.13).

### Profiles

```cpp
struct ServoProfile {
  std::string name = "xl330", source;
  f32 stiffness, damping, friction, torque, armature, delay;     // K, D, F, T, A, delay
  f32 spread_stiffness, spread_damping, spread_friction, spread_torque, spread_armature;
  f32 delay_min, delay_max;
};
```

The `spread*` values and `delay_min/max` state how certain the values are: training draws separate
values within these bounds for each servo, so that the policy does not depend on what the measurement
could not pin down. Profiles are JSON (`to_json`, `servo_profile_from_json`) with `"kind": "servo"`,
`name`, `source` and the keys `stiffness`, `damping`, `friction`, `torque`, `armature`, `delay`,
`spreadStiffness`, `spreadDamping`, `spreadFriction`, `spreadTorque`, `spreadArmature`, `delayMin`,
`delayMax`. Values outside their ranges are rejected; stiffness and torque must be positive.

`game::xl330_profile()` is the XL330-M288 as Pollen Robotics trains it (BAM model "m6" at 7.4 V):

| Parameter | Value |
|---|---|
| K stiffness | 0.554 N·m/rad |
| D damping | 0.053 N·m·s/rad |
| F friction | 0.0048 N·m |
| T maximum torque | 0.963 N·m |
| A armature | 0.0018 kg·m² |
| Delay | 20 ms (training: 15–30 ms) |
| Spread | armature ±10 %, friction ±10 % |

### The boundary to Box3D

`set_plumo_servo(level, trunk, link, profile)` writes a profile into the actors of a duck – with
the scale factor in exactly this one place:

| Quantity | Conversion |
|---|---|
| Stiffness, friction, maximum torque | × S⁴ |
| Damping | × S⁴ · √S |
| Inertia of the link incl. armature | × S⁵ |

The armature is added as a thin disc on the joint axis carried by the link: `armature` about the
axis, half of it across (I = a/2 · (E + axis·axisᵀ)). A purely axial tensor would not be a real rigid
body, and Box3D gains energy from it (`add_armature`). `set_plumo_servos` sets all servos and
the bus delay (`ControllerComponent::servo_delay`). The walker controller computes the delay itself
in whole physics steps: the targets pass through a queue and reach `servo_target` only `delay` steps
later (at 100 Hz and S = 4: 20 ms real = 4 steps).

How Plumo walks with this is described in [Chapter 13](13-plumo.md); how profiles are
measured and fitted, in [Chapter 16](16-calibration.md).

---

## 6.18 Tests

| Test | Checks |
|---|---|
| `physics_world_bodies_and_events` | bodies, shapes, user values, sensor and contact events, raycast (sensors ignored), rejection of invalid shapes, destruction |
| `physics_joints_and_grab` | all nine joint types, self-connection rejected, MouseGrab lifts a body, static bodies not grabbable, `frame_from_axis` |
| `physics_scale_is_a_unit_system` | scaling as a change of units (bit for bit when hanging, < 1 mm on the ground) |
| `physics_hull_mesh` | hull triangles, orientation, flat point sets |
| `physics_materials_of_meshes_and_the_ground` | friction, restitution, rolling resistance and contact softness pass through level file and body plan down to the Box3D shapes |
| `game_world_physics_lab` | pendulum, turntable, lift, rover, bridge, breaking, debug lines, grab |
| `game_character_mover` | the character mover |
| `determinism_is_bit_identical_across_builds_and_platforms` | fingerprint over 400 control ticks |

---

© 2026 Christian Heinrich Hohlfeld · All rights reserved · [Imprint and rights](00-imprint.md)
