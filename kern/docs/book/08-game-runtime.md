# 8 The Game Runtime

`kern::game` turns Kern into a game engine. A **level** is a document made of actors with
components – what Kern Studio edits and saves as `.klevel`. The **GameWorld** displays a level with
the renderer and plays it on Box3D: with controllers, camera, rules, triggers, sound events and
logic graphs. Studio uses the same GameWorld for its viewports (editing, Simulate, Play), and
exported games run on exactly this code.

This chapter describes the data model (`include/kern/game/level.h`), the file format
(`src/game/level.cpp`), the runtime (`include/kern/game/runtime.h`, `src/game/runtime.cpp`,
`soldier.cpp`, `logic_world.cpp`), touch controls, the sound connection, the built-in demo levels and
the sequence of a frame.

```
include/kern/game/level.h      Level, Actor, components, WorldSettings, (de)serialization, validate
include/kern/game/runtime.h    GameWorld, PlayInput, SimMode, Hud, SoundCue, plan_bodies
include/kern/game/demos.h      built-in levels
include/kern/game/sound.h      SoundBank, GameAudio
include/kern/game/touch.h      InputArbiter, TouchControls
include/kern/game/net_game.h   multiplayer (Chapter 10)
include/kern/game/walker.h     Micro Duck (Chapter 13)
src/game/level.cpp             model and .klevel
src/game/runtime.cpp           GameWorld: presentation, Box3D, controllers, rules
src/game/soldier.cpp           soldier: movement, weapon, health, camera
src/game/soldier_art.cpp       soldier figures, weapon in hand, effects (presentation only)
src/game/logic_world.cpp       logic graphs in the world (Chapter 9)
src/game/sound.cpp             sound for games
src/game/touch.cpp             touch controls
src/game/demos.cpp             demo levels
```

---

## 8.1 Lifecycle in five lines

The header comment of `runtime.h` shows the entire usage:

```cpp
GameWorld world(renderer, &night_windows);
world.sync(level);                       // meshes for the editor view
world.start(level, SimMode::play);       // Box3D world, controllers, rules
// every frame:
world.step(dt, input); world.update_camera(cam, dt, input); world.present(now_ms, eye);
world.stop();                            // back to the edited level
```

`sync` brings the renderer's meshes in line with the level (only what changed is re-uploaded) and is
ignored while a simulation is running. A city that the new level no longer has is removed by `sync`
with its own meshes only (chunks, trunks, crowns), not via the actor id: the new level may give the
same id to another actor, such as the floor, which would otherwise be missing after a level switch. `start` copies the level, builds the Box3D world and starts
Simulate or Play. `stop` tears everything down and shows the level that `sync` last saw again –
changes made by the simulation therefore do not go into the document (Studio offers "Keep
Simulation Changes" for that).

---

## 8.2 Conventions

From the header comment of `level.h`:

- Meters, seconds, kilograms, **Z up**.
- Rotations as **Euler angles in degrees** (x roll, y pitch, z yaw), applied as
  yaw · pitch · roll – like a rotator in the editor. `euler_to_quat`, `quat_to_euler` (yaw
  −180..180, pitch −90..90; in gimbal lock everything goes into yaw), `transform_matrix`
  (translation · rotation · scale) and `matrix_transform` (decomposition without shear, positive
  scale) convert between them.
- Colors are **sRGB 0..1**, in the file `#rrggbb` (`hex_color`, `parse_hex_color`;
  `srgb_to_linear` for the renderer).
- An actor has a transform **relative to its parent actor** and a fixed set of optional components
  (a mesh, a light, a body, a controller … plus any number of colliders and joints).
- Child actors without a body of their own are welded as a compound into the body of the nearest
  ancestor when simulating.

---

## 8.3 Level and actor

### Level

| Field | Type | Meaning |
|---|---|---|
| `name` | string | default "Untitled" |
| `world` | `WorldSettings` | world, physics, rule and audio settings (8.5) |
| `actors` | `vector<Actor>` | in document order |
| `textures` | `vector<TextureAsset>` | imported images (`id`, `name`, `src` as data URL, `width`, `height`) |
| `sounds` | `vector<SoundAsset>` | imported sounds (`id`, `name`, `src` as data URL, `duration` informational) |
| `logic` | `logic::Graph` | the level graph (Self = none; sees the events of all actors) |
| `camera` | `EditorCamera` | editor camera: `position` (−24, −30, 18), `yaw` 51°, `pitch` −24° |
| `next_id` | `ActorId` | next free actor ID |

Helper functions: `find(id)`, `add(actor)` (assigns an ID if `actor.id == 0` or taken),
`children(parent)` (0 = top level), `world_matrix(id)` (parents applied), `body_owner(id)` (nearest
ancestor, or the actor itself, with a body), `is_ancestor`, `texture(id)`, `sound(id)`.
`default_player(level)` returns the actor that Play takes over: `world.player` if it has a player
controller, otherwise the first one with a controller.

### Actor

| Field | Meaning |
|---|---|
| `id` | unique within the level, 0 = none (`ActorId = u32`) |
| `name` | name in the Outliner |
| `parent` | parent actor (0 = top level) |
| `folder` | Outliner folder ("" = top level) |
| `hidden` | hidden in the editor and in the game |
| `locked` | not selectable in the viewport |
| `transform` | `position`, `rotation` (Euler degrees), `scale` (1, 1, 1) – relative to the parent actor |
| `tags` | free keywords (logic: Find Actor with Tag; weapons: `head`) |
| `mesh`, `light`, `body`, `controller`, `pickup`, `trigger`, `rotator`, `mover`, `city`, `audio`, `weapon`, `health` | at most one each (`std::optional`) |
| `colliders`, `joints` | any number |
| `logic` | its own logic graph (Self = this actor) |

![A selected actor in Studio: Outliner, Details with the components, gizmo in the viewport](../buch/bilder/studio-selected.png)

---

## 8.4 The components

The tables give the C++ field, the default value and the key in the `.klevel` file (Section 8.6
describes the format; it also lists the value ranges to which values are clamped on reading).

### Static Mesh (`MeshComponent`, key `mesh`)

| Field | Default | Key | Meaning |
|---|---|---|---|
| `shape` | `box` | `shape` | `box`, `sphere`, `cylinder`, `cone`, `capsule`, `plane`, `torus`, `ramp`, `mesh` |
| `size` | (1, 1, 1) | `size` | box, plane (x, y), ramp |
| `radius` | 0.5 | `radius` | sphere, cylinder, cone, capsule, torus (major radius) |
| `height` | 1 | `height` | cylinder, cone, capsule (total), torus (minor radius) |
| `segments` | 24 | `segments` | subdivision |
| `asset` | "" | `asset` | shape `mesh`: a built-in mesh (e.g. `duck/xl330`, `kern/game/builtin_meshes.h`), scaled by `size` |
| `collision` | `true` | `collision` | generates a matching collider if the actor has none of its own |
| `friction` | 0.6 | `friction` | friction of this automatic collider |
| `restitution` | 0 | `restitution` | restitution 0..1 |
| `rolling_resistance` | 0 | `rollingResistance` | rolling resistance |
| `surface` | "" | `surface` | surface for pair friction: "", `cloth`, `tpu`, `floor` ([Chapter 6](06-physics.md)) |
| `material` | see below | `material` | render material |

`MaterialDesc` (`material`):

| Field | Default | Key |
|---|---|---|
| `color` | (0.8, 0.8, 0.8) sRGB | `color` |
| `roughness` | 0.6 | `roughness` |
| `metalness` | 0 | `metalness` |
| `emissive` | black, multiplied by `emissive_strength` | `emissive` |
| `emissive_strength` | 0 | `emissiveStrength` |
| `opacity` | 1 (< 1: alpha blending) | `opacity` |
| `unlit`, `cast_shadow`, `double_sided` | `false`, `true`, `false` | `unlit`, `castShadow`, `doubleSided` |
| `texture` | "" (texture asset ID) | `texture` |
| `tiling` | 1 (repetitions per meter or per face) | `tiling` |

The friction values deliberately sit next to the render material: they are the object's physics. In
the Details panel they are under Mesh › **Physics**.

### Point Light (`LightComponent`, `light`)

| Field | Default | Key |
|---|---|---|
| `color` | (1, 0.95, 0.85) | `color` |
| `intensity` | 20 | `intensity` |
| `range` | 12 m | `range` |

### Rigid Body (`BodyComponent`, `body`)

| Field | Default | Key | Meaning |
|---|---|---|---|
| `motion` | `dynamic` | `motion` | `static`, `kinematic`, `dynamic` |
| `mass` | 0 | `mass` | kg; 0 = from the colliders' densities |
| `linear_damping`, `angular_damping` | 0, 0.05 | `linearDamping`, `angularDamping` | Box3D's implicit damping |
| `gravity_scale` | 1 | `gravityScale` | |
| `bullet` | `false` | `bullet` | continuous collision against dynamic bodies too |
| `start_awake`, `allow_sleep` | `true`, `true` | `startAwake`, `allowSleep` | |
| `lock_linear[3]`, `lock_angular[3]` | all `false` | `lockLinear`, `lockAngular` | locks per world axis |
| `linear_velocity` | 0 | `linearVelocity` | initial velocity, m/s |
| `angular_velocity` | 0 | `angularVelocity` | initial spin, **degrees per second** |
| `impact_sound` | `builtin:impact` | `impactSound` | on hard impacts ("" = silent) |
| `impact_threshold` | 2.5 m/s | `impactThreshold` | approach speed for the impact sound |
| `center_of_mass` | 0 | `centerOfMass` | explicit center of mass (actor-local) |
| `inertia[6]` | 0 | `inertia` | explicit inertia tensor `xx yy zz xy xz yz` (kg·m²) |

`explicit_inertia()` is true if the diagonal is positive. Only then do center of mass and tensor
apply (together with `mass`), as for imported robots (MJCF, URDF); otherwise shapes and densities
decide. `centerOfMass` and `inertia` are only written in this case, the tensor with seven
significant digits, because its entries span many orders of magnitude.

### Collider (`ColliderComponent`, list `colliders`)

| Field | Default | Key | Meaning |
|---|---|---|---|
| `shape` | `automatic` | `shape` | `auto` (fitted to the mesh), `box`, `sphere`, `capsule`, `hull` |
| `center` | 0 | `center` | actor-local offset |
| `rotation` | 0 | `rotation` | Euler degrees |
| `size` | (1, 1, 1) | `size` | box |
| `radius`, `length` | 0.5, 1 | `radius`, `length` | sphere, capsule (`length` = distance between cap centers, along Z) |
| `points` | empty | `points` | hull points (actor-local); empty = the hull of the actor's mesh |
| `density` | 1000 kg/m³ | `density` | |
| `friction`, `restitution`, `rolling_resistance` | 0.6, 0, 0 | `friction`, `restitution`, `rollingResistance` | |
| `surface` | "" | `surface` | surface for pair friction |
| `sensor` | `false` | `sensor` | overlap only |
| `category`, `mask` | 1, `0xffffffff` | `category`, `mask` | collision channels |
| `group` | 0 | `group` | group index (negative: never with each other) |
| `impact_sound` | `auto` | `impactSound` | `auto` = the body's, ""/`none` = silent; this lets a sole sound different from a housing |

### Constraint (`JointComponent`, list `joints`)

| Field | Default | Key | Meaning |
|---|---|---|---|
| `kind` | `hinge` | `kind` | `hinge`, `slider`, `ball`, `fixed`, `distance`, `wheel`, `motor`, `parallel`, `noCollision` |
| `target` | 0 | `target` | the other actor (0 = the world) |
| `anchor` | 0 | `anchor` | actor-local |
| `axis` | (0, 0, 1) | `axis` | actor-local: axis for hinge/ball/parallel, direction for slider/wheel |
| `collide_connected` | `false` | `collideConnected` | |
| `enable_limit` | `false` | `enableLimit` | |
| `lower`, `upper` | −45, 45 | `lower`, `upper` | hinge, ball cone (`upper`): degrees; slider, wheel: meters; distance: min./max. length |
| `reference` | 0 | `reference` | hinge: the angle (degrees) the joint reads in the build pose |
| `enable_motor` | `false` | `enableMotor` | |
| `motor_speed` | 0 | `motorSpeed` | degrees/s (hinge, wheel) or m/s (slider, distance) |
| `max_motor_force` | 100 | `maxMotorForce` | N or N·m; for servos the maximum torque |
| `enable_spring` | `false` | `enableSpring` | |
| `spring_hertz`, `spring_damping` | 4, 0.5 | `springHertz`, `springDamping` | |
| `length` | 0 | `length` | rest length of the distance joint, 0 = distance at start |
| `break_force`, `break_torque` | 0, 0 | `breakForce`, `breakTorque` | N / N·m, 0 = unbreakable |
| `servo` | `false` | `servo` | hinge servo (position control like a Dynamixel) |
| `servo_target` | 0 | `servoTarget` | degrees |
| `servo_stiffness` | 50 | `servoStiffness` | N·m/rad |
| `servo_damping` | 2 | `servoDamping` | N·m·s/rad |
| `servo_friction` | 0.1 | `servoFriction` | N·m |
| `break_sound` | `builtin:break` | `breakSound` | on breaking ("" = silent) |

The servo law is torque = `servo_stiffness` × (`servo_target` − angle) − `servo_damping` ×
velocity, at most `max_motor_force`, with `servo_friction` joint friction
([Chapter 6](06-physics.md), Section 6.17).

### Player Controller (`ControllerComponent`, `controller`)

| Field | Default | Key | Meaning |
|---|---|---|---|
| `mode` | `hovercar` | `mode` | `hovercar`, `rolling`, `character`, `flyer`, `vehicle`, `soldier`, `walker` |
| `max_speed` | 36 m/s | `maxSpeed` | |
| `acceleration` | 24 m/s² | `acceleration` | |
| `turn_rate` | 130 °/s | `turnRate` | |
| `jump_speed` | 6 m/s | `jumpSpeed` | 0 = no jump |
| `boost` | `true` | `boost` | |
| `boost_speed` | 60 m/s | `boostSpeed` | |
| `hover_height` | 0.6 m | `hoverHeight` | hovercar |
| `camera` | `chase` | `camera` | `chase`, `firstPerson`, `topDown` |
| `camera_distance`, `camera_height`, `camera_fov` | 12, 4, 62 | `cameraDistance`, `cameraHeight`, `cameraFov` | |
| `engine_sound` | `auto` | `engineSound` | loop whose pitch and volume follow speed and throttle; `auto` = the one for the movement mode |
| `jump_sound`, `boost_sound` | `builtin:jump`, `builtin:boost` | `jumpSound`, `boostSound` | |
| `crouch_speed` | 2.6 m/s | `crouchSpeed` | soldier: crouching |
| `eye_height` | 1.62 m | `eyeHeight` | soldier: eye height above the feet (crouched 60 %) |
| `look_sensitivity` | 1 | `lookSensitivity` | mouse/touch/gamepad |
| `head_bob` | `true` | `headBob` | |
| `autonomous` | `true` | `autonomous` | walker: walks on its own when nobody is controlling it |
| `gait` | `velstand` | `gait` | walker: `velstand` or `alpha` |
| `rollers` | `false` | `rollers` | walker: roller skates |
| `actions` | `training` | `actions` | walker: `training` (scale 1, no filter) or `robot` (scale 0.9, low-pass head 0.5 / legs 0.7) |
| `kern_extras` | `false` | `kernExtras` | walker: Kern's extras (the play layer `play.onnx` with ball and sock, which then plays instead of Pollen's BallPlay; beak when picking up) |
| `servo_delay` | 0.02 s | `servoDelay` | walker: command latency of the servos (real seconds) |

The walker keys are only written when `mode = walker` (`kernExtras` and `rollers` only when `true`,
`servoDelay` only when ≠ 0.02). On reading, `gait` is normalized to `velstand`/`alpha` and `actions`
to `training`/`robot`. What the walker fields do is described in [Chapter 13](13-micro-duck.md).

### Weapon (`WeaponComponent`, `weapon`)

A hitscan weapon for soldier players (server-authoritative and lag-compensated in multiplayer).

| Field | Default | Key |
|---|---|---|
| `kind` | `rifle` | `kind` (`rifle`, `pistol`, `shotgun`, `sniper`) |
| `damage` | 24 (per hit or per pellet) | `damage` |
| `fire_rate` | 600 rounds/min | `fireRate` |
| `automatic` | `true` | `automatic` |
| `magazine`, `reserve` | 30, 120 | `magazine`, `reserve` |
| `reload_time` | 2.0 s | `reloadTime` |
| `spread`, `aim_spread` | 1.2°, 0.15° | `spread`, `aimSpread` |
| `recoil` | 0.8° per shot | `recoil` |
| `range` | 300 m | `range` |
| `headshot` | 2.5 | `headshot` (damage factor on head colliders) |
| `impulse` | 4 N·s | `impulse` (on hit dynamic bodies) |
| `pellets` | 1 | `pellets` (shotgun: rays per shot) |
| `aim_fov` | 42° | `aimFov` |
| `fire_sound`, `reload_sound` | `builtin:smg`, `builtin:reload` | `fireSound`, `reloadSound` |

`weapon_preset(kind)` returns the handling of a type:

| | Damage | Rate | Magazine/reserve | Reload | Spread (hip/aim) | Recoil | Range | Head | Impulse | Aim FOV |
|---|---|---|---|---|---|---|---|---|---|---|
| `rifle` | 24 | 600, auto | 30/120 | 2.0 s | 1.2°/0.15° | 0.8 | 300 m | ×2.5 | 4 | 42° |
| `pistol` | 30 | 360 | 12/60 | 1.4 s | 1.0°/0.25° | 1.1 | 120 m | ×2.0 | 4 | 52° |
| `shotgun` | 11 × 8 | 75 | 6/36 | 2.6 s | 4.5°/3.2° | 2.4 | 45 m | ×1.5 | 2 | 55° |
| `sniper` | 90 | 45 | 5/25 | 2.8 s | 3.0°/0.02° | 3.0 | 1000 m | ×3.0 | 12 | 16° |

Sounds: pistol `builtin:pistol`, shotgun `builtin:launcher`, sniper rifle `builtin:sniper`.

### Health (`HealthComponent`, `health`)

| Field | Default | Key |
|---|---|---|
| `max_health` | 100 | `maxHealth` |
| `armor` | 0 (absorbs damage first) | `armor` |
| `regen_delay` | 5 s after the last damage (0 = no regeneration) | `regenDelay` |
| `regen_rate` | 12 HP/s | `regenRate` |
| `respawn` | `true` (players at the spawn, others in place) | `respawn` |
| `respawn_delay` | 3 s | `respawnDelay` |
| `points` | 10 (for the player who takes it out) | `points` |
| `hit_sound`, `death_sound` | `builtin:hit`, `builtin:down` | `hitSound`, `deathSound` |
| `explode_radius` | 0 (> 0: explodes when destroyed) | `explodeRadius` |
| `explode_damage` | 80 (at the center, falling to 0 at the edge) | `explodeDamage` |

### Pickup (`PickupComponent`, `pickup`)

| Field | Default | Key |
|---|---|---|
| `points` | 1 | `points` |
| `burst_color` | (0.55, 1.0, 0.92) | `burstColor` |
| `respawn` | 0 s (0 = gone for this run) | `respawn` |
| `sound` | `builtin:pickup` | `sound` |
| `heal` | 0 (health for the collector) | `heal` |
| `ammo` | 0 (rounds into the collector's reserve) | `ammo` |

### Trigger (`TriggerComponent`, `trigger`)

| Field | Default | Key | Meaning |
|---|---|---|---|
| `action` | `goal` | `action` | `goal`, `hazard`, `checkpoint`, `message`, `launch`, `teleport`, `damage`, `event` |
| `message` | "" | `message` | text of the message |
| `once` | `true` | `once` | fire only once |
| `sound` | `auto` | `sound` | `auto` = the action's default, "" = silent |
| `strength` | 12 | `strength` | launch: m/s upward (plus forward along the trigger's X); damage: damage points |
| `target` | 0 | `target` | teleport: the actor you arrive at |

### Audio Source (`AudioComponent`, `audio`)

| Field | Default | Key |
|---|---|---|
| `sound` | `builtin:hum` | `sound` (`builtin:<name>` or an imported sound ID) |
| `volume` | 1 (0..4) | `volume` |
| `pitch` | 1 | `pitch` |
| `loop`, `autoplay` | `true`, `true` | `loop`, `autoplay` (starts with Play/Simulate) |
| `spatial` | `true` (3D at the actor; otherwise 2D on its bus) | `spatial` |
| `min_distance`, `max_distance` | 2 m, 40 m | `minDistance`, `maxDistance` |
| `rolloff` | `inverse` | `rolloff` |
| `bus` | `sfx` | `bus` (not `master`) |
| `doppler` | 1 | `doppler` |
| `pitch_by_speed` | 0 (additional pitch per m/s of the body) | `pitchBySpeed` |
| `delay` | 0 s after the start | `delay` |

### Rotator, Mover, City

| Component | Field | Default | Key |
|---|---|---|---|
| `RotatorComponent` (`rotator`) | `rate` | (0, 0, 60) °/s about the local axes | `rate` |
| `MoverComponent` (`mover`) | `offset` | (0, 0, 1) m relative to the start position | `offset` |
| | `period` | 2 s per cycle | `period` |
| | `phase` | 0 (0..1) | `phase` |
| | `mode` | `sine` | `mode` (`sine`, `pingPong`, `loop`) |
| `CityComponent` (`city`) | `seed` | 20261022 | `seed` |
| | `blocks` | 7 per side | `blocks` |
| | `pitch` | 80 m from street to street | `pitch` |
| | `street` | 16 m street width | `street` |
| | `max_height` | 84 m (tallest building in the center) | `maxHeight` |
| | `trees` | `true` | `trees` |
| | `traffic` | 22 cars | `traffic` |
| | `night_windows` | `true` (shader extension) | `nightWindows` |

The mover moves `sine` as ½ − ½·cos(2πu), `pingPong` linearly back and forth, `loop` linearly with a
jump back; u = time / period + phase. The rotator rotates about the axis of `rate` at its magnitude.
Both are computed by `GameWorld::animated_local` from the simulation time.

---

## 8.5 WorldSettings

| Field | Default | Key | Meaning |
|---|---|---|---|
| `gravity` | (0, 0, −9.80665) | `gravity` | |
| `substeps` | 4 | `substeps` | Box3D sub-steps per step (1..16) |
| `step_hz` | 60 | `stepHz` | fixed step rate (15..480 Hz) |
| `time_of_day` | 0.42 | `timeOfDay` | 0..1 (0.25 sunrise, 0.5 noon, 0.75 sunset) |
| `day_length` | 0 | `dayLength` | seconds per day while playing, 0 = frozen |
| `sun_azimuth` | 210° | `sunAzimuth` | where the sun stands at noon |
| `fog_density` | 0.0009 | `fogDensity` | |
| `exposure` | 1 | `exposure` | |
| `ao_radius` | 1.6 m | `aoRadius` | reach of ambient occlusion, 0 = off |
| `shadow_distance` | 80 m | `shadowDistance` | how far into the view the sun's shadow map reaches (2..2000 m; smaller: sharper; [Chapter 4](04-graphics.md)) |
| `clouds` | `true` | `clouds` | |
| `ground_color` | (0.19, 0.27, 0.15) | `groundColor` | color of the endless ground |
| `ground` | `true` | `ground` | endless ground beneath everything |
| `ground_friction`, `ground_restitution` | 0.7, 0 | `groundFriction`, `groundRestitution` | material of the ground |
| `ground_surface` | "" | `groundSurface` | "" or `floor` |
| `contact_hertz`, `contact_damping`, `contact_speed` | 30 Hz, 10, 3 m/s | `contactHertz`, `contactDamping`, `contactSpeed` | contact softness (Box3D) |
| `kill_z` | −50 m | `killZ` | a player below this respawns |
| `title` | "" | `title` | game title (otherwise the level name) |
| `goal` | `none` | `goal` | `none`, `collectAll`, `reachGoal`, `survive`, `score` |
| `time_limit` | 0 | `timeLimit` | seconds, 0 = none |
| `score_target` | 0 | `scoreTarget` | for `score` |
| `player` | 0 | `player` | the controlled actor (0 = the first one with a controller) |
| `observer` | `false` | `observer` | Play starts with a free camera (fly, watch, grab bodies; F8 takes over the player) |
| `master_volume` | 1 | `masterVolume` | |
| `music`, `music_volume` | "", 0.6 | `music`, `musicVolume` | music loop on the music bus |
| `ambience`, `ambience_volume` | "", 0.6 | `ambience`, `ambienceVolume` | ambience loop |
| `reverb` | 0.25 | `reverb` | reverb amount 0..1 |
| `doppler` | 1 | `doppler` | Doppler factor for all 3D sounds |

---

## 8.6 The `.klevel` format

A level is a JSON document. `to_json` / `to_text` write it, `from_json` / `from_text` read it;
`actor_to_json`/`actor_from_json` and `world_to_json`/`world_from_json` also exist separately
(Studio and logic use them).

### Top level

| Key | Content |
|---|---|
| `format` | must be `"kern-level"` |
| `version` | 1; a level with a higher version is rejected ("written by a newer Kern Studio") |
| `name` | level name |
| `world` | WorldSettings (8.5) |
| `camera` | editor camera: `position`, `yaw`, `pitch` (−90..90) |
| `nextId` | next free actor ID |
| `actors` | list of actors (at most 100,000) |
| `textures` | list: `id`, `name`, `width`, `height`, `src` (data URL `data:image/…`) |
| `sounds` | list: `id`, `name`, `duration`, `src` (`data:audio/…`, `data:video/…` or `data:application/ogg`) |
| `logic` | level graph (only written if not empty; format in [Chapter 9](09-logic.md)) |

### Actor

```json
{
  "id": 12, "name": "Door", "parent": 3, "folder": "Door", "hidden": true, "locked": true,
  "transform": {"position": [5.2, -12, 1.5], "rotation": [0, 0, 0], "scale": [1, 1, 1]},
  "tags": ["door"],
  "components": {
    "mesh": {...}, "body": {...}, "colliders": [{...}], "joints": [{...}], "logic": {...}
  }
}
```

`parent`, `folder`, `hidden`, `locked` and `tags` are only written if they differ from the default.
The component keys are listed in the tables of 8.4.

### Writing

- Numbers and vectors are rounded to 10⁻⁶ ("enough for an editor document"); `-0` becomes `0`.
- Inertia tensors with seven significant digits.
- Colors as `#rrggbb`.
- Optional keys that only appear when needed: `mesh.asset` and `mesh.surface` (not empty),
  `body.centerOfMass`/`body.inertia` (explicit tensor), `colliders[].points` (not empty),
  `colliders[].surface` (not empty), `colliders[].impactSound` (≠ `auto`), `joints[].reference`
  (≠ 0), the `servo*` keys (only when `servo = true`), the controller's walker keys,
  `world.groundSurface` (not empty), `world.observer` (only `true`), `logic`.

### Reading

`from_json` is deliberately lenient about what is missing and strict about what is wrong:

- **Unknown keys are ignored; missing ones get default values.** Older files thus load in newer
  versions.
- **Wrong types are errors** with a message ("friction must be a number", "size must be [x, y, z]",
  "color must be a #rrggbb colour", "mode: unknown value"). The error names the actor.
- **Numbers are clamped to their range**, e.g.:

| Range | Keys |
|---|---|
| 0..1 | `roughness`, `metalness`, `opacity`, `restitution`, `mover.phase`, `reverb`, `timeOfDay` |
| 0..10 | `friction`, `rollingResistance`, `groundFriction` |
| 3..128 | `mesh.segments` |
| 1..16 / 15..480 | `substeps` / `stepHz` |
| 1..1000 | `contactHertz` |
| 0.01..100 | `contactSpeed` |
| 0..1000 | `springHertz` |
| 0..100 | `springDamping` |
| −360..360 | `reference` |
| −3600..3600 | `servoTarget` |
| 20..120 | `cameraFov` |
| 1..16 | `city.blocks` (additionally: `street` at most `pitch / 2`) |
| 0..4 | volumes, `doppler`; `pitch` 0.05..8 |
| −32768..32767 | `group` |

- Vector components must be finite and ≤ 10⁷ in magnitude.
- **IDs:** missing or duplicate IDs are reassigned. A `parent` that does not exist or points to
  itself becomes 0; **parent cycles are broken.**
- Textures without an ID or without a `data:image/` source, and sounds without an ID, with a
  `builtin:` ID or with an unsuitable source, are skipped.
- `timeOfDay ≥ 1` becomes 0; `maxDistance ≤ minDistance` is set to `minDistance + 0.01`.

### Checking: `validate(level)`

`validate` returns `Issue`s (`error`, `actor`, `text`). **Errors** prevent Play, **warnings** appear
in the Output Log.

| Errors | Warnings |
|---|---|
| missing or duplicate ID | joint on an actor without a body (ignored) |
| parent actor does not exist | joint target without a body and without collision (ignored) |
| transform not finite | sound reference does not exist (stays silent) – for body, collider, joint, controller, weapon, health, pickup, trigger, audio source, music, ambience |
| scale (almost) 0 | audio source without a sound |
| joint connects the actor to itself | weapon without a soldier controller |
| joint target does not exist | controller on a non-dynamic body |
| joint axis is zero | pickup/trigger with colliders, but none is a sensor |
| controller without a body on the same actor | more than one player controller (the first is taken over) |
| city is not at the top level | goal "reach goal", but no goal trigger |

`sound_reference_ok` accepts "", `none`, `auto`, a known `builtin:<name>` or an imported sound ID.

---

## 8.7 From level to Box3D: `plan_bodies`

`plan_bodies(level)` describes which Box3D bodies and compound shapes a level needs. The simulation
builds its world from it, and Studio uses it to draw the colliders in the editor – so both see the
same thing.

1. **Actors with a body component** (except cities) get a body at the actor's world frame (without
   scale). A static body with a rotator or mover becomes kinematic.
2. **Implicit bodies:** actors with their own colliders, meshes with collision, pickups and triggers
   *outside* any body get one of their own – static, or kinematic if they or an ancestor are
   animated.
3. **Shapes:** every actor contributes to its body owner (itself, an ancestor or its implicit body),
   with its transform relative to the body frame:
   - its own colliders as they are (`auto` is fitted to the mesh with `fit_collider`),
   - otherwise a collider fitted to the mesh with the mesh's physics material (friction,
     restitution, rolling resistance, surface); for pickups and triggers as a sensor,
   - pickups and triggers without a mesh and colliders get a sensor sphere with a 1 m radius.
4. Implicit bodies without a shape are dropped; bodies with a component simulate even without a shape
   (point masses).
5. **Cities:** a static body with one box per building and sidewalk (friction 0.5).
6. **The endless ground:** a static box of 20,000 × 20,000 × 2 m, top face at z = 0, with
   `groundFriction`, `groundRestitution` and `groundSurface`.

Shape rules (`add_shape`): scale goes exactly into sizes, radii and hull points. Moving shapes are at
least 1 cm in size, static ones at least 2 mm ([Chapter 6](06-physics.md)). `fit_collider` fits
automatic colliders to the mesh shape: box and plane become a box, sphere a sphere, capsule a
capsule, and all other shapes (including torus) a convex hull from the mesh points.

Test: `game_body_planning` (cart with a welded-in, scaled wheel; wall as an implicit static body;
coin as an implicit kinematic sensor; ground).

---

## 8.8 Starting the GameWorld

`GameWorld::start(level, mode)` performs these steps:

1. `validate`; an error aborts (`last_error()` names it).
2. `sync(level)`; the level becomes the running copy (`level_`), the original is remembered.
3. **Box3D world** with the level's gravity and contact softness. The hit threshold is
   `kHitSpeed = 2 m/s` (logic event "On Hit", camera shake); if a dynamic body has an impact sound
   with a lower threshold, it is lowered down to that body's `impactThreshold` (at least 0.05 m/s –
   the taps and footsteps of a small robot).
4. All actors "alive"; whether the level has logic graphs (the events depend on this).
5. **Order:** actors, parents before children (`order_`).
6. **Bodies** from `plan_bodies` (`create_body`, see below).
7. **Joints** of all actors (`create_joints`), then **walkers** (`create_walkers`).
8. **City traffic:** kinematic cars (box 4 × 1.9 × 1.6 m, never sleep) on the lanes.
9. **Player** (Play only): `default_player` if it has a dynamic body, otherwise the first actor that
   can be taken over. It gets a pawn with a spawn pose; characters and soldiers switch to the
   character mover.
10. **Rules:** reset the HUD (best time is kept), title, number of pickups, status `playing` (Play)
    or `inactive` (Simulate), time limit, boost; in Play the title appears as a message.
11. **Combat:** initialize health and ammunition.
12. Compute world matrices, start **logic** (`start_logic`, then `begin`).

### `create_body`

- Type, pose and – for explicit bodies – damping, gravity scale, bullet, awake/sleep and initial
  velocities (spin degrees/s → rad/s).
- **Player vehicles** (hovercar, vehicle) are always bullets; **players never sleep**.
- Every shape: density, friction, restitution, rolling resistance, surface, sensor, channels, group;
  the user value is the contributing actor (this is how a hit finds the welded-in head).
- Events only where they are read: sensor events for sensors, players and (with logic) everything
  movable; contact events only on the player; hit events on dynamic bodies if player, impact sound
  or logic.
- Rejected shapes are skipped and reported ("Collider of "…" skipped: …").
- Mass: `mass > 0` on dynamic bodies → `set_mass_data` with an explicit tensor, otherwise `set_mass`.
- Locks from the component; controllers other than Rolling Ball and Walker additionally lock roll and
  pitch (vehicles, characters and fliers stay upright).

### `create_joints`

- Without a body on the actor or without a body on the target, the joint is skipped with a message.
  Target 0 is the world: a static body at the anchor.
- **Body order:** Box3D measures B relative to A. The actor moves relative to its target, so the
  **target is body A and the actor body B** – positive slider travel, hinge angles and motor speeds
  move the actor. Exception wheel: here Box3D's ordering stays (A = chassis = the actor, B = wheel).
- Frames from anchor and axis in world coordinates (`frame_from_axis`; slider along X, otherwise Z).
  The wheel joint rotates about the axis and suspends along the chassis's up axis, made
  perpendicular to the axis.
- `reference ≠ 0` (hinge): frame A is rotated back by the reference angle so that the joint reads
  this angle in the build pose.
- Springs limited to `max_hertz = 0.5 × step_hz × substeps`; servos with joint softness
  `min(240, max_hertz)` Hz and damping 2.
- Mapping: hinge → revolute (degrees → radians), slider → prismatic, ball → spherical (cone `upper`,
  motor as angular velocity about the axis), fixed → weld (spring = softness), distance → distance
  (rope at the target's origin or at the world anchor, rest length 0 = distance at start), wheel →
  wheel, motor → motor joint (spring 4 Hz default), parallel → parallel (8 Hz default), no collision
  → filter.
- Break thresholds from `breakForce`/`breakTorque`; servo parameters into the runtime state
  (`SimJoint`).

### Modes

| `SimMode` | Meaning |
|---|---|
| `none` | no simulation (editor) |
| `simulate` | physics and logic run, nobody is controlled; bodies can be grabbed |
| `play` | like Simulate, plus the possessed player, rules and HUD |
| `replica` | copy of a server game for a network client: bodies follow set poses, rules run on the server ([Chapter 10](10-networking.md)) |

`set_mode` switches between Simulate and Play while running (Studio: F8 "Eject"/"Possess"); the pawn
keeps its state. `restart` restarts the original level, `set_paused` pauses, `step_once` performs
exactly one fixed step (also while paused; Studio: "Frame", key `.`). `session()` counts starts so
that listeners notice a new run.

![Simulate in Studio: the physics runs, nobody is controlled, bodies can be grabbed](../buch/bilder/studio-simulate.png)

---

## 8.9 A frame

### In the GameWorld

`step(dt, input)` consumes the frame time in fixed steps (accumulator, clamped to 0.25 s and 0.1 s of
simulation per frame – [Chapter 6](06-physics.md), Section 6.13; for the principle of the fixed time
step see [Appendix C](C-physics.md), C.2.4). Before each step it remembers the poses of movable
bodies. One fixed step, `fixed_step(input, h)`:

1. **Players** (only with status `playing`): for each pawn, choose the input (local: the frame's;
   remote: the last one received), adopt the view direction, input events for logic
   (`logic_inputs`), then depending on the mode `drive_soldier`, `drive_walker` or `drive_player`.
   The local pawn rests in Simulate after Eject.
2. **Kinematic animation:** world matrices for `t + h`, and every rotator/mover body gets its target
   via `set_target_pose`.
3. **Traffic** (`drive_traffic`; cars stop in front of the player), **walkers** (`drive_walkers`),
   **servos** (`drive_servos`), **mouse grab** (`grab_.update`).
4. **`physics_.step(h, substeps)`** (sub-steps 1..16).
5. Step counter and `sim_time_`; HUD time and remaining time.
6. **Events** (`handle_events`): the player's pickups and triggers, logic overlaps and hits, camera
   shake, impact sounds, breaking joints.
7. **Health** (regeneration, respawns) and **logic tick**.
8. Respawning pickups; time limit (`survive` wins, otherwise lost); **kill Z** (a pawn below it
   respawns); player speed into the HUD; target score reached → won.

After that, the time of day advances (`day_length`), the walker voices are prepared, and
`compute_world_matrices` interpolates the poses with `alpha`.

- `update_camera(cam, dt, input)` moves the camera like the player's (8.11).
- `present(now_ms, eye)` loads poses and visibility into the renderer (hidden actors and children of
  hidden parents, collected pickups and used triggers are invisible) and draws traffic, shots and
  particles.
- `apply_lighting(eye)` sets light, sky, fog and point lights and returns the night factor for the
  lit windows (`NightWindows`, a shader extension of the renderer).

### In Studio

`Studio::frame(now_ms)` calls per frame (`kern_studio_frame`):

1. Update editor cameras and touch controls.
2. While playing: collect input; with networking `update_net` ([Chapter 10](10-networking.md)),
   otherwise `world_->step(dt, input)`; check logic breakpoints; play camera (`update_camera`); the
   world's messages into the Output Log.
3. While editing: synchronize the level with the world.
4. Overlays, `present`, audio (`update_audio`), then render every visible viewport.

`dt` is clamped to 0.1 s.

---

## 8.10 Controllers: how the player drives, rolls, walks and flies

`PlayInput` is one player's input for one frame:

| Field | Meaning |
|---|---|
| `move_x`, `move_y`, `up` | −1..1 (right, forward, up) |
| `jump`, `boost` | buttons |
| `robot` | walker moves as bits: 1 sit/stand, 2 pick up, 4 kick left, 8 kick right, 16 roll, 32 beak, 64 chirp, 128 wheee, 256 hop (Kern) |
| `look_x`, `look_y` | camera rotation in degrees since the last frame |
| `zoom` | camera distance (mouse wheel) |
| `view_yaw`, `has_view_yaw`, `view_pitch` | view direction of remote players (degrees) |
| `fire`, `aim`, `reload`, `crouch`, `interact` | soldier |
| `look_stick_x`, `look_stick_y`, `gamepad` | right gamepad stick, input from the pad |
| `rewind` | server: seconds by which the shooter saw the world in the past (lag compensation) |

For rolling ball, character and flyer, movement is **camera-relative**. The modes (`drive_player`,
`drive_soldier`, `drive_walker`):

| Mode | Implementation |
|---|---|
| **Hovercar** | throttle as a force along the vehicle axis (reverse 1.25×, boost +42 m/s²), top speed as a counterforce, rolling resistance without throttle; **lateral grip as a force** (continuous impulses fight the solver); steering as a yaw rate proportional to speed up to 9 m/s. A **hover spring** over a Box3D ray holds `hover_height`. Visible bobbing and leaning are presentation only. |
| **Vehicle** | like the hovercar without the hover spring; it is only driven with ground contact (sphere probe). |
| **Rolling Ball** | force in the direction of movement; **jump as an impulse** m · (`jump_speed` − v_z), only on the ground; top speed as a counterforce. |
| **Character** | Box3D's **character mover** ([Chapter 6](06-physics.md)): the body becomes kinematic, the capsule is sized from its shapes (feet at the lowest, head at the highest point, step height 22 % of the height), classic first-person-shooter ground movement, gravity 1.5× (snappier). The body turns smoothly toward the direction of travel and follows via `set_target_pose`. Footsteps by stride length; triggers and pickups via the touched planes. |
| **Flyer** | target velocity including up/down, acceleration as a force that cancels gravity; yaw toward the camera direction. |
| **Soldier** | character mover with real gravity (jump 3.6 m/s → 0.66 m), see 8.13. |
| **Walker** | the input goes to the robot's controller, which drives its servos with the trained policy ([Chapter 13](13-micro-duck.md)). |

Common to all: boost is a reserve 0..1 (−0.45/s while boosting, +0.12/s otherwise); landings after a
free fall above 3.5 m/s play `builtin:land`; jumps and boost play their sounds.

### Possession and pawns

A **pawn** is a player's control state (the local one and every remote one): actor IDs, last input,
view direction, spawn pose, boost, character mover, soldier state. `add_pawn(source, id)` creates
another player – a copy of the player actor with its children, on a ring around the original start
(angle n · 2.39996 rad, radius 3.5 + 1.2 · √n m), with bodies, joints, combat values and logic;
`remove_pawn` removes it again (the level's own player stays). `set_pawn_input`, `set_local_pawn`,
`pawns`, `pawn_source` belong to this – the network uses them ([Chapter 10](10-networking.md)).

---

## 8.11 Camera

`update_camera` follows the player's **interpolated** pose:

| `CameraMode` | Behavior |
|---|---|
| `chase` (vehicles) | behind the direction of travel, distance grows with speed (`camera_distance` × zoom + 0.12 · v), damped |
| `chase` (otherwise) | orbit around the player with mouse/stick; a Box3D ray keeps the camera out of walls (for walkers, excluding their own links) |
| `first_person` | eyes on the body; vehicles look in the direction of travel |
| `top_down` | from above, damped |

The field of view widens above 30 m/s (up to +10.5°); hard hits on the player shake the camera
(`shake_`). Soldiers have a camera of their own (8.13).

---

## 8.12 Rules, HUD and triggers

### HUD

`Hud` is the game state for display: `status` (`inactive`, `playing`, `won`, `lost`), `title`,
`message` and `message_age`, `score`, `pickups`/`pickups_total`, `elapsed`, `remaining` (< 0 = no
limit), `speed`, `boost` (< 0 = no boost), `best` (best goal time of the session) and the soldier
values (`health`, `armor`, `ammo`, `magazine`, `reserve`, `reload`, `spread`, hit marker, damage
direction, `respawn_in`, `aiming`, `fov`).

### Win conditions (`GameGoal`)

| Goal | Won / lost |
|---|---|
| `none` | sandbox |
| `collectAll` | all pickups collected; if there is a goal trigger, it must be reached afterwards ("Collect everything first") |
| `reachGoal` | goal trigger reached |
| `survive` | time limit survived |
| `score` | target score reached |

With a time limit, you lose at 0, except with `survive`. A hazard trigger always loses. `finish`
sets the status, plays the jingle, remembers the best time (for `collectAll`/`reachGoal`) and fires
the logic event `game.over`.

### Pickups

When a player touches a pickup (sensor begin or contact, or a plane of the character mover), it is
collected: healing and ammunition go to the collector (a pure health pack stays where it is as long
as the collector has full health), points, counter, sound, particles, logic event `game.pickup`.
With `respawn > 0` it comes back after that time.

### Trigger actions

| Action | Effect | Default sound |
|---|---|---|
| `goal` | wins (with `collectAll` only once everything is collected) | `builtin:goal` |
| `hazard` | loses | `builtin:hazard` |
| `checkpoint` | the pawn's spawn is now 1 m above the trigger | `builtin:checkpoint` |
| `message` | shows the message | `builtin:confirm` |
| `launch` | lifts the player upward at `strength` m/s and at half that speed along the trigger's X | `builtin:boost` |
| `teleport` | moves the player to the target actor (view direction adopted); 1 s lockout against immediately going back; stays usable | `builtin:powerup` |
| `damage` | `strength` damage to the player | – |
| `event` | only the logic event (and the message) | – |

Before the action, every trigger fires the logic event `game.trigger`. With `once` (except goal and
hazard) it is used up afterwards.

---

## 8.13 Soldiers and figures

The soldier is a first-person-shooter player modeled on SpawnCity (`src/game/soldier.cpp`), with all
movement in fixed steps on the authority.

- **Movement:** character mover, camera-relative. **Crouching** (hold) shrinks the capsule to 60 %;
  standing up only with headroom (mover probe upward). **Sprinting** only forward, not while aiming
  or crouching. Walking, aiming (60 %), crouching (`crouch_speed`).
- **Jump** with buffering (pressed 0.14 s before landing counts) and coyote time (0.12 s after an
  edge).
- **Fall damage:** above 7.7 m/s landing speed, lethal from 15 m/s.
- **Weapon:** reloading (also automatically on an empty magazine), semi-automatic by edge detection,
  no firing while sprinting. Every shot is a Box3D ray from the eye; the **spread is
  deterministic** (uniformly distributed in the cone, hashed from actor ID, shot count and pellet).
  Other pawns are checked as capsules, with **lag compensation**: the server takes their feet from
  the position history of `rewind` seconds ago (at most 0.5 s). A hit on an actor tagged `head` (or
  in the top 0.32 m of the capsule) is a headshot. Hit dynamic bodies receive `impulse`. The spread
  blooms with movement, air (×2.5) and shots, then recovers; crouching ×0.7.
- **Damage:** armor absorbs up to two thirds; freshly spawned players are protected for 2.5 s. At
  0 HP: sound, points for the shooter, logic event `game.death`; pawns respawn after
  `respawn_delay` (at least 0.5 s) at the spawn, other actors disappear and, with `respawn`, return
  in place. With `explode_radius` the actor explodes: Box3D radial impulse, damage falling with
  distance, flash, sound, camera kick.
- **Camera:** its own first-person camera with mouse sensitivity, zoom when aiming, gamepad curves and
  rate ramps, recoil, head bob, landing dip.
- **Figures:** `soldier_art.cpp` draws SpawnCity's articulated figure for every soldier actor (walk,
  run, air, aim, dead), standing in the editor, animated from the pawn while playing; plus the weapon
  in hand with gloves and sleeves, muzzle flash, tracers, bullet holes, debris and shell casings.
  All of this is pure presentation and does not change the simulation – authority, replicas and
  editor show the same thing. The models (a submachine gun, a pistol, a .50 sniper rifle, a 40 mm launcher) are
  built procedurally from primitives (`soldier_models.cpp`).

Tests: `game_soldier_moves_shoots_and_respawns`, `soldier_art_in_play`,
`soldier_models_are_closed_and_outward`, and for networking `net_game_soldier_replication`.

![Play in Studio with HUD](../buch/bilder/studio-play.png)

---

## 8.14 Walkers

A walker is an articulated robot that walks on its own servos, driven by a control policy – the
Micro Duck with the trained network from Pollen Robotics. At start, the GameWorld creates a
controller state for every walker controller (`create_walkers`) and drives it every step
(`drive_walkers`: sensors, behavior, network every four physics steps, command delay) along with the
servos (`drive_servos`). On top of that there is a large interface for sensors, commands, training
and the game layer (`walker_sensors`, `walker_command`, `walker_skill`, `walker_probe`,
`set_walker_action` …). All of this is described in [Chapter 13](13-micro-duck.md).

---

## 8.15 Sound connection

The simulation does not play anything itself. In its fixed steps it produces **sound cues** and is
therefore deterministic; the caller empties them every frame.

```cpp
struct SoundCue {
  std::string sound;                 // "builtin:<name>" or an imported sound ID
  Vec3 position;
  f32 volume = 1, pitch = 1;
  bool spatial = true;
  audio::Bus bus = audio::Bus::sfx;
  ActorId actor = 0;
  bool voice = false, silence = false;   // an actor's voice: one at a time
};
```

`cue()` ignores "", `none` and `auto` and holds at most 64 cues (without a consumer, for instance on
a server, the newest win). Cues are produced for jumps, boost, landings, footsteps, pickups,
triggers, win/loss, respawn, hits, shots, reloading, explosions, breaking joints and impacts.

**Impact sounds** (in `handle_events`): every shape whose collider (or whose body, for `auto`) has a
sound remembers it. A hit above `max(0.05, impactThreshold)` plays it – at most six per step, one
sound per touch (sounds with "step" in the name at most every 0.5 s per actor, otherwise 0.09 s; a
sole rolls off and settles in small bounces that Box3D reports individually). Volume follows the
approach speed, `builtin:impact` becomes `builtin:impact_light` for light impacts, and heavy bodies
sound lower (pitch from log₁₀ of the mass, 0.7..1.15).

Playback goes through `game::GameAudio` (`include/kern/game/sound.h`) on Kern's software mixer
([Chapter 7](07-audio.md)):

```cpp
audio::Mixer mixer;  mixer.init(48000);
SoundBank bank(mixer);
GameAudio audio(mixer, bank);
world.start(level, SimMode::play);  audio.begin(world);
// every frame: bank.prepare(4); audio.update(world, camera, dt);  the host pulls mixer.render()
world.stop();  audio.end();
```

- `SoundBank` turns references into mixer clips: built-ins are synthesized in time slices
  (`prepare(budget_ms)`), imported files are decoded by the host, which supplies PCM (`set_pcm`).
  `request_level` requests everything a level needs – Play then starts with everything in place.
- `GameAudio::update` begins with every new run of the world (start, restart), pauses with it, puts
  the listener at the camera, starts music, ambience and audio sources, runs the player's **engine
  loop** (pitch and volume follow speed and throttle; without Doppler, because the camera follows)
  and plays all cues. A cue whose clip is not ready yet waits briefly and is dropped if it would come
  too late.
- `engine_sound_for(controller)`: `auto` is `builtin:hover` for hovercar and flyer, `builtin:engine`
  for vehicle, `builtin:servo` for walker, otherwise none.

---

## 8.16 Touch, mouse and gamepad

`include/kern/game/touch.h` brings SpawnCity's touch system to C++, so that Studio Play, exported
games and every controller share the same implementation.

**`InputArbiter`** decides which device is currently in use; the most recently used one wins, with
SpawnCity's rules:

- Start: desktop if the primary pointer is fine or there are no touch points.
- A finger switches to touch.
- A mouse does **not** switch a device with a coarse primary pointer (phone, tablet) to desktop.
- Every real key switches to mouse+keyboard, even on a phone.
- Gamepad use is additionally recorded.
- Pointer lock exists only with mouse+keyboard.

**`TouchControls`** lays out the overlay and converts fingers into `PlayInput`:

- **Two sticks** of 168 px, flush in the lower corners at the safe-area edges (smaller only if two
  do not fit; at least 72 px). Left moves ("STEER / GAS" for vehicles, "FLY" for the flyer,
  otherwise "MOVE"), right looks ("LOOK").
- **Buttons per controller**, the bottom row to the left of the look stick, further ones above it:

| Mode | Buttons |
|---|---|
| Soldier | JUMP, RELOAD and AIM (both only with a weapon; AIM is a toggle), CROUCH (toggle), USE |
| Walker | QUACK, PICK, SIT, KICK L, KICK R, ROLL, BEAK, WHEEE, HOP |
| Character | JUMP (with jump), USE |
| Rolling Ball | JUMP, BOOST (if available) |
| Flyer | UP, DOWN, BOOST |
| Hovercar, Vehicle | BOOST, HOP (with jump), USE |

- **Ownership per finger:** buttons before sticks before screen. A finger on the screen fires for the
  soldier (and aims along), otherwise it looks.
- **Move stick:** analog; pushed all the way forward to the edge means sprinting (character,
  soldier).
- **Look:** 1:1 dragging with "flick" amplification for fast movements, plus **edge turn**: dragging
  beyond 92 % of the deflection radius (0.42 × ring size) keeps turning (up to 4.2 rad/s yaw,
  3.3 rad/s pitch, faded in over 42 px, with SpawnCity's stick curve). Zoomed views turn more slowly.
- `update` returns one frame's input including the walker bits; `view()` describes what the page
  draws.

The page passes gamepads (standard layout) to the engine every frame; Kern applies dead zones, curves
and the soldier's aim rates (`web/studio/game.mjs`, `GamepadInput`). Tests:
`touch_controls_layout_and_sticks`, `input_device_arbiter_rules`,
`studio_touch_play_without_pointer_lock`.

---

## 8.17 Logic in the world

Every actor and the level can carry a logic graph. The GameWorld starts them on the authority
(standalone or server) in Play and Simulate, delivers events to them (overlap, hit, pickup, trigger,
damage, death, respawn, interact, input, key, game over, new player) and offers operations to their
nodes (`set_property`, `spawn_copy`, `teleport`, `push_actor`, `set_joint_motor`, `line_trace`,
`add_score`, `end_game` …). Whatever logic changes is recorded for the network. Everything else is
in [Chapter 9](09-logic.md).

---

## 8.18 The demo levels

`include/kern/game/demos.h` contains the built-in levels; Studio offers them under File › Example
Levels and File › New Level.

| Function | Level | Content |
|---|---|---|
| `orb_run_level()` | **Orb Run** | Procedural city with traffic; a Box3D hovercar (1200 kg, box collider, friction 0.25; cabin, spoiler, four glowing thrusters and headlights as welded-in children) with a chase camera; 24 energy orbs (pickups with a 2.4 m sensor sphere, rotator, mover, light) on the streets; goal `collectAll`; late afternoon with a 480 s day length – the run ends at night; music `music_drive`, ambience `city`. Studio starts with this level. |
| `physics_lab_level()` | **Physics Lab** | All joint types and kinds of motion: pyramid of crates (started asleep), pendulum on a distance joint, door on a hinge with limits and a closing spring, rope bridge of 12 planks (three middle joints break at 60,000 N), turntable with a motor (45°/s) and three riders, lift on a slider with motor and limit, a self-driving rover on four wheel joints with suspension and drive; the player is a rolling ball, plus five stars, a rotating hazard sweeper and a goal; goal `reachGoal`. |
| `shooting_range_level()` | **Shooting Range** | First-person-shooter range: soldier with rifle, health and head; eight target dummies (health with fast regeneration, head tag) at 10–60 m, cover, pushable crates and a barrel, a tower with a ramp; goal 150 points; playable unchanged in multiplayer. |
| `micro_duck_level()` | **Micro Duck** | A room at the robot's scale (2.5 × 2.5 m real, i.e. 10 × 10 m in the world): wooden floor (friction 1, surface `floor`), walls, shoebox, books, crate, flower pot, a light ball (rolling resistance 0.03), two socks; the duck autonomous and with Kern's play network; 100 Hz, 16 sub-steps, no endless ground; **observer camera**; a level graph makes the duck chirp as soon as its head camera sees the ball. |
| `basic_level()` | **Basic** | ground 40 × 40 m and a cube. |
| `empty_level()` | **Empty** | nothing, not even an endless ground. |

Tests: `game_world_orb_run_play`, `game_world_physics_lab`, `game_shooting_range_template`,
`micro_duck_example_level_runs`, `studio_starts_with_orb_run`.

---

## 8.19 Further GameWorld interfaces

| Area | Methods |
|---|---|
| Presentation | `set_layers`, `sync`, `clear`, `actor_for_mesh`, `actor_matrix`, `actor_bounds`, `actor_meshes`, `pending_textures`, `set_texture_pixels`, `time_of_day` |
| State | `running`, `mode`, `paused`, `player`, `camera_yaw`, `camera_pitch`, `sim_time`, `hud`, `hud_mut`, `stats` (`SimStats`: bodies, awake bodies, shapes, joints, broken ones, contacts, steps), `level`, `physics`, `actor_body`, `actor_alive`, `last_input` |
| Debug | `debug_lines(colliders, contacts, joints, …)` – colliders colored by type (static green, kinematic blue, dynamic orange, sensor violet, sleeping gray), contact points with normals, joints with axes |
| Messages | `events()` (pickups, broken joints, skipped colliders, logic output), `sound_cues()`, `shots()` |
| Combat | `damage`, `health_of`, `set_health`, `heal`, `give_ammo`, `soldier_state`, `apply_soldier_state`, `set_view` |
| Network | `add_pawn`, `remove_pawn`, `set_pawn_input`, `set_local_pawn`, `pawns`, `take_alive_changes`, `dead_actors`, `set_alive`, `networked_actors`, `start_replica`, `set_replica_pose`, `step_replica`, `take_replicated_changes`, `replicated_state` |
| Logic | `logic_runtime`, `key_event`, `set_property`, `spawn_copy`, `teleport`, `actor_velocity`, `set_actor_velocity`, `push_actor`, `set_simulate_physics`, `set_joint_motor`, `find_actor`, `actors_with_tag`, `has_tag`, `line_trace`, `add_score`, `end_game`, `show_message`, `play_sound`, `particle_burst`, `shake_camera`, `destroy_actor`, `call_logic_event` |
| Grab | `begin_grab`, `move_grab`, `end_grab`, `grabbing` |

---

## 8.20 Tests

| Test | Checks |
|---|---|
| `game_euler_and_transforms` | Euler ↔ quaternion, matrices |
| `game_level_round_trip_and_validation` | `.klevel` round trip, `validate` |
| `game_body_planning` | `plan_bodies` |
| `game_city_generation` | city generator |
| `game_world_presentation` | presentation |
| `game_world_orb_run_play` | playing Orb Run |
| `game_world_physics_lab` | Physics Lab (joints, breaking, grab, Play switch) |
| `game_world_rules` | goal wins, hazard loses, best time, remaining time |
| `game_orbit_camera_mouse_sense` | camera direction |
| `game_soldier_moves_shoots_and_respawns` | soldier |
| `game_character_mover` | character mover |
| `game_gameplay_blocks` | gameplay building blocks |
| `audio_level_fields_round_trip`, `audio_collider_impact_sounds`, `audio_game_world_cues` | sound fields, impact sounds, cues |
| `physics_materials_of_meshes_and_the_ground` | physics material of meshes and ground |

---

© 2026 Christian Heinrich Hohlfeld · All rights reserved · [Imprint and rights](00-imprint.md)
