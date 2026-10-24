# 8 Die Spiel-Laufzeit

`kern::game` macht aus Kern eine Game Engine. Ein **Level** ist ein Dokument aus Actors mit
Komponenten – das, was Kern Studio bearbeitet und als `.klevel` speichert. Die **GameWorld** zeigt
ein Level mit dem Renderer an und spielt es auf Box3D: mit Controllern, Kamera, Regeln, Triggern,
Sound-Ereignissen und Logik-Graphen. Studio nutzt dieselbe GameWorld für seine Viewports (Editieren,
Simulate, Play), und exportierte Spiele laufen mit genau diesem Code.

Dieses Kapitel beschreibt das Datenmodell (`include/kern/game/level.h`), das Dateiformat
(`src/game/level.cpp`), die Laufzeit (`include/kern/game/runtime.h`, `src/game/runtime.cpp`,
`soldier.cpp`, `logic_world.cpp`), Touch-Steuerung, Sound-Anbindung, die eingebauten Demo-Levels und
den Ablauf eines Frames.

```
include/kern/game/level.h      Level, Actor, Komponenten, WorldSettings, (De)Serialisierung, validate
include/kern/game/runtime.h    GameWorld, PlayInput, SimMode, Hud, SoundCue, plan_bodies
include/kern/game/demos.h      eingebaute Levels
include/kern/game/sound.h      SoundBank, GameAudio
include/kern/game/touch.h      InputArbiter, TouchControls
include/kern/game/net_game.h   Mehrspieler (Kapitel 10)
include/kern/game/walker.h     Micro Duck (Kapitel 13)
src/game/level.cpp             Modell und .klevel
src/game/runtime.cpp           GameWorld: Darstellung, Box3D, Controller, Regeln
src/game/soldier.cpp           Soldat: Bewegung, Waffe, Gesundheit, Kamera
src/game/soldier_art.cpp       Soldaten-Figuren, Waffe in der Hand, Effekte (nur Darstellung)
src/game/logic_world.cpp       Logik-Graphen in der Welt (Kapitel 9)
src/game/sound.cpp             Sound für Spiele
src/game/touch.cpp             Touch-Steuerung
src/game/demos.cpp             Demo-Levels
```

---

## 8.1 Lebenszyklus in fünf Zeilen

Der Kopfkommentar von `runtime.h` zeigt die ganze Benutzung:

```cpp
GameWorld world(renderer, &night_windows);
world.sync(level);                       // Meshes für die Editor-Ansicht
world.start(level, SimMode::play);       // Box3D-Welt, Controller, Regeln
// jeden Frame:
world.step(dt, input); world.update_camera(cam, dt, input); world.present(now_ms, eye);
world.stop();                            // zurück zum bearbeiteten Level
```

`sync` bringt die Meshes des Renderers mit dem Level in Einklang (nur Geändertes wird neu
hochgeladen) und wird während einer laufenden Simulation ignoriert. Eine Stadt, die das neue Level
nicht mehr hat, entfernt `sync` nur mit ihren eigenen Meshes (Chunks, Stämme, Kronen), nicht über die
Actor-Id: Das neue Level kann dieselbe Id einem anderen Actor geben, etwa dem Boden, der sonst nach
einem Levelwechsel fehlte. `start` kopiert das Level,
baut die Box3D-Welt und startet Simulate oder Play. `stop` reißt alles ab und zeigt wieder das
Level, das `sync` zuletzt gesehen hat – Änderungen der Simulation gehen also nicht ins Dokument
(Studio bietet dafür „Keep Simulation Changes“).

---

## 8.2 Konventionen

Aus dem Kopfkommentar von `level.h`:

- Meter, Sekunden, Kilogramm, **Z oben**.
- Rotationen als **Euler-Winkel in Grad** (x Roll, y Pitch, z Yaw), angewendet als
  Yaw · Pitch · Roll – wie ein Rotator im Editor. `euler_to_quat`, `quat_to_euler` (Yaw −180..180,
  Pitch −90..90; im Gimbal-Lock geht alles in Yaw), `transform_matrix` (Translation · Rotation ·
  Skalierung) und `matrix_transform` (Zerlegung ohne Scherung, Skalierung positiv) rechnen um.
- Farben sind **sRGB 0..1**, in der Datei `#rrggbb` (`hex_color`, `parse_hex_color`;
  `srgb_to_linear` für den Renderer).
- Ein Actor hat eine Transform **relativ zu seinem Eltern-Actor** und einen festen Satz optionaler
  Komponenten (ein Mesh, ein Licht, ein Body, ein Controller … sowie beliebig viele Collider und
  Gelenke).
- Kind-Actors ohne eigenen Body werden beim Simulieren als Compound in den Body des nächsten
  Vorfahren geschweißt.

---

## 8.3 Level und Actor

### Level

| Feld | Typ | Bedeutung |
|---|---|---|
| `name` | string | Standard „Untitled“ |
| `world` | `WorldSettings` | Welt-, Physik-, Regel- und Audio-Einstellungen (8.5) |
| `actors` | `vector<Actor>` | in Dokumentreihenfolge |
| `textures` | `vector<TextureAsset>` | importierte Bilder (`id`, `name`, `src` als Data-URL, `width`, `height`) |
| `sounds` | `vector<SoundAsset>` | importierte Sounds (`id`, `name`, `src` als Data-URL, `duration` informativ) |
| `logic` | `logic::Graph` | der Level-Graph (Self = keiner; sieht die Events aller Actors) |
| `camera` | `EditorCamera` | Editor-Kamera: `position` (−24, −30, 18), `yaw` 51°, `pitch` −24° |
| `next_id` | `ActorId` | nächste freie Actor-ID |

Hilfsfunktionen: `find(id)`, `add(actor)` (vergibt eine ID, wenn `actor.id == 0` oder belegt),
`children(parent)` (0 = oberste Ebene), `world_matrix(id)` (Eltern angewendet), `body_owner(id)`
(nächster Vorfahr oder der Actor selbst mit Body), `is_ancestor`, `texture(id)`, `sound(id)`.
`default_player(level)` liefert den Actor, den Play übernimmt: `world.player`, wenn er einen Player
Controller hat, sonst den ersten mit Controller.

### Actor

| Feld | Bedeutung |
|---|---|
| `id` | eindeutig im Level, 0 = keiner (`ActorId = u32`) |
| `name` | Name im Outliner |
| `parent` | Eltern-Actor (0 = oberste Ebene) |
| `folder` | Outliner-Ordner („“ = oberste Ebene) |
| `hidden` | im Editor und im Spiel versteckt |
| `locked` | im Viewport nicht auswählbar |
| `transform` | `position`, `rotation` (Euler-Grad), `scale` (1, 1, 1) – relativ zum Eltern-Actor |
| `tags` | freie Schlagworte (Logik: Find Actor with Tag; Waffen: `head`) |
| `mesh`, `light`, `body`, `controller`, `pickup`, `trigger`, `rotator`, `mover`, `city`, `audio`, `weapon`, `health` | je höchstens eine (`std::optional`) |
| `colliders`, `joints` | beliebig viele |
| `logic` | eigener Logik-Graph (Self = dieser Actor) |

![Ein ausgewählter Actor im Studio: Outliner, Details mit den Komponenten, Gizmo im Viewport](bilder/studio-selected.png)

---

## 8.4 Die Komponenten

Die Tabellen nennen das C++-Feld, den Standardwert und den Schlüssel in der `.klevel`-Datei
(Abschnitt 8.6 beschreibt das Format; dort stehen auch die Wertebereiche, auf die beim Lesen
begrenzt wird).

### Static Mesh (`MeshComponent`, Schlüssel `mesh`)

| Feld | Standard | Schlüssel | Bedeutung |
|---|---|---|---|
| `shape` | `box` | `shape` | `box`, `sphere`, `cylinder`, `cone`, `capsule`, `plane`, `torus`, `ramp`, `mesh` |
| `size` | (1, 1, 1) | `size` | Box, Plane (x, y), Ramp |
| `radius` | 0,5 | `radius` | Sphere, Cylinder, Cone, Capsule, Torus (Hauptradius) |
| `height` | 1 | `height` | Cylinder, Cone, Capsule (gesamt), Torus (Nebenradius) |
| `segments` | 24 | `segments` | Unterteilung |
| `asset` | „“ | `asset` | Form `mesh`: ein eingebautes Mesh (z. B. `duck/xl330`, `kern/game/builtin_meshes.h`), mit `size` skaliert |
| `collision` | `true` | `collision` | erzeugt einen passenden Collider, wenn der Actor keinen eigenen hat |
| `friction` | 0,6 | `friction` | Reibung dieses automatischen Colliders |
| `restitution` | 0 | `restitution` | Restitution 0..1 |
| `rolling_resistance` | 0 | `rollingResistance` | Rollwiderstand |
| `surface` | „“ | `surface` | Oberfläche für Paar-Reibung: „“, `cloth`, `tpu`, `floor` ([Kapitel 6](06-physik.md)) |
| `material` | siehe unten | `material` | Render-Material |

`MaterialDesc` (`material`):

| Feld | Standard | Schlüssel |
|---|---|---|
| `color` | (0,8, 0,8, 0,8) sRGB | `color` |
| `roughness` | 0,6 | `roughness` |
| `metalness` | 0 | `metalness` |
| `emissive` | schwarz, multipliziert mit `emissive_strength` | `emissive` |
| `emissive_strength` | 0 | `emissiveStrength` |
| `opacity` | 1 (< 1: Alpha-Blending) | `opacity` |
| `unlit`, `cast_shadow`, `double_sided` | `false`, `true`, `false` | `unlit`, `castShadow`, `doubleSided` |
| `texture` | „“ (Textur-Asset-ID) | `texture` |
| `tiling` | 1 (Wiederholungen pro Meter bzw. pro Fläche) | `tiling` |

Die Reibungswerte stehen bewusst neben dem Render-Material: Sie sind die Physik des Objekts. Im
Details-Panel liegen sie unter Mesh › **Physics**.

### Point Light (`LightComponent`, `light`)

| Feld | Standard | Schlüssel |
|---|---|---|
| `color` | (1, 0,95, 0,85) | `color` |
| `intensity` | 20 | `intensity` |
| `range` | 12 m | `range` |

### Rigid Body (`BodyComponent`, `body`)

| Feld | Standard | Schlüssel | Bedeutung |
|---|---|---|---|
| `motion` | `dynamic` | `motion` | `static`, `kinematic`, `dynamic` |
| `mass` | 0 | `mass` | kg; 0 = aus den Dichten der Collider |
| `linear_damping`, `angular_damping` | 0, 0,05 | `linearDamping`, `angularDamping` | Box3Ds implizite Dämpfung |
| `gravity_scale` | 1 | `gravityScale` | |
| `bullet` | `false` | `bullet` | Continuous Collision auch gegen dynamische Körper |
| `start_awake`, `allow_sleep` | `true`, `true` | `startAwake`, `allowSleep` | |
| `lock_linear[3]`, `lock_angular[3]` | alle `false` | `lockLinear`, `lockAngular` | Sperren je Weltachse |
| `linear_velocity` | 0 | `linearVelocity` | Anfangsgeschwindigkeit, m/s |
| `angular_velocity` | 0 | `angularVelocity` | Anfangsdrehung, **Grad pro Sekunde** |
| `impact_sound` | `builtin:impact` | `impactSound` | bei harten Stößen („“ = still) |
| `impact_threshold` | 2,5 m/s | `impactThreshold` | Annäherungsgeschwindigkeit für den Aufprall-Sound |
| `center_of_mass` | 0 | `centerOfMass` | expliziter Schwerpunkt (actorlokal) |
| `inertia[6]` | 0 | `inertia` | expliziter Trägheitstensor `xx yy zz xy xz yz` (kg·m²) |

`explicit_inertia()` ist wahr, wenn die Diagonale positiv ist. Nur dann gelten Schwerpunkt und
Tensor (zusammen mit `mass`), wie bei importierten Robotern (MJCF, URDF); sonst entscheiden Formen
und Dichten. Geschrieben werden `centerOfMass` und `inertia` nur in diesem Fall, der Tensor mit
sieben signifikanten Stellen, weil seine Einträge viele Größenordnungen umfassen.

### Collider (`ColliderComponent`, Liste `colliders`)

| Feld | Standard | Schlüssel | Bedeutung |
|---|---|---|---|
| `shape` | `automatic` | `shape` | `auto` (an das Mesh angepasst), `box`, `sphere`, `capsule`, `hull` |
| `center` | 0 | `center` | actorlokaler Versatz |
| `rotation` | 0 | `rotation` | Euler-Grad |
| `size` | (1, 1, 1) | `size` | Box |
| `radius`, `length` | 0,5, 1 | `radius`, `length` | Sphere, Capsule (`length` = Abstand der Kappenmitten, entlang Z) |
| `points` | leer | `points` | Hull-Punkte (actorlokal); leer = die Hülle des Actor-Meshes |
| `density` | 1000 kg/m³ | `density` | |
| `friction`, `restitution`, `rolling_resistance` | 0,6, 0, 0 | `friction`, `restitution`, `rollingResistance` | |
| `surface` | „“ | `surface` | Oberfläche für Paar-Reibung |
| `sensor` | `false` | `sensor` | nur Überlappung |
| `category`, `mask` | 1, `0xffffffff` | `category`, `mask` | Kollisionskanäle |
| `group` | 0 | `group` | Gruppenindex (negativ: nie untereinander) |
| `impact_sound` | `auto` | `impactSound` | `auto` = der des Bodys, „“/`none` = still; so klingt eine Sohle anders als ein Gehäuse |

### Constraint (`JointComponent`, Liste `joints`)

| Feld | Standard | Schlüssel | Bedeutung |
|---|---|---|---|
| `kind` | `hinge` | `kind` | `hinge`, `slider`, `ball`, `fixed`, `distance`, `wheel`, `motor`, `parallel`, `noCollision` |
| `target` | 0 | `target` | der andere Actor (0 = die Welt) |
| `anchor` | 0 | `anchor` | actorlokal |
| `axis` | (0, 0, 1) | `axis` | actorlokal: Achse bei Hinge/Ball/Parallel, Richtung bei Slider/Wheel |
| `collide_connected` | `false` | `collideConnected` | |
| `enable_limit` | `false` | `enableLimit` | |
| `lower`, `upper` | −45, 45 | `lower`, `upper` | Hinge, Ball-Kegel (`upper`): Grad; Slider, Wheel: Meter; Distance: min./max. Länge |
| `reference` | 0 | `reference` | Hinge: der Winkel (Grad), den das Gelenk in der Baupose liest |
| `enable_motor` | `false` | `enableMotor` | |
| `motor_speed` | 0 | `motorSpeed` | Grad/s (Hinge, Wheel) bzw. m/s (Slider, Distance) |
| `max_motor_force` | 100 | `maxMotorForce` | N bzw. N·m; bei Servos das Höchstmoment |
| `enable_spring` | `false` | `enableSpring` | |
| `spring_hertz`, `spring_damping` | 4, 0,5 | `springHertz`, `springDamping` | |
| `length` | 0 | `length` | Ruhelänge des Distance-Gelenks, 0 = Abstand beim Start |
| `break_force`, `break_torque` | 0, 0 | `breakForce`, `breakTorque` | N / N·m, 0 = unzerbrechlich |
| `servo` | `false` | `servo` | Hinge-Servo (Positionsregelung wie ein Dynamixel) |
| `servo_target` | 0 | `servoTarget` | Grad |
| `servo_stiffness` | 50 | `servoStiffness` | N·m/rad |
| `servo_damping` | 2 | `servoDamping` | N·m·s/rad |
| `servo_friction` | 0,1 | `servoFriction` | N·m |
| `break_sound` | `builtin:break` | `breakSound` | beim Brechen („“ = still) |

Das Servo-Gesetz ist Moment = `servo_stiffness` × (`servo_target` − Winkel) − `servo_damping` ×
Geschwindigkeit, höchstens `max_motor_force`, mit `servo_friction` Gelenkreibung
([Kapitel 6](06-physik.md), Abschnitt 6.17).

### Player Controller (`ControllerComponent`, `controller`)

| Feld | Standard | Schlüssel | Bedeutung |
|---|---|---|---|
| `mode` | `hovercar` | `mode` | `hovercar`, `rolling`, `character`, `flyer`, `vehicle`, `soldier`, `walker` |
| `max_speed` | 36 m/s | `maxSpeed` | |
| `acceleration` | 24 m/s² | `acceleration` | |
| `turn_rate` | 130 °/s | `turnRate` | |
| `jump_speed` | 6 m/s | `jumpSpeed` | 0 = kein Sprung |
| `boost` | `true` | `boost` | |
| `boost_speed` | 60 m/s | `boostSpeed` | |
| `hover_height` | 0,6 m | `hoverHeight` | Hovercar |
| `camera` | `chase` | `camera` | `chase`, `firstPerson`, `topDown` |
| `camera_distance`, `camera_height`, `camera_fov` | 12, 4, 62 | `cameraDistance`, `cameraHeight`, `cameraFov` | |
| `engine_sound` | `auto` | `engineSound` | Loop, Pitch und Lautstärke folgen Tempo und Gas; `auto` = der des Bewegungsmodus |
| `jump_sound`, `boost_sound` | `builtin:jump`, `builtin:boost` | `jumpSound`, `boostSound` | |
| `crouch_speed` | 2,6 m/s | `crouchSpeed` | Soldat: Ducken |
| `eye_height` | 1,62 m | `eyeHeight` | Soldat: Augenhöhe über den Füßen (geduckt 60 %) |
| `look_sensitivity` | 1 | `lookSensitivity` | Maus/Touch/Gamepad |
| `head_bob` | `true` | `headBob` | |
| `autonomous` | `true` | `autonomous` | Walker: läuft allein, wenn niemand steuert |
| `gait` | `velstand` | `gait` | Walker: `velstand` oder `alpha` |
| `rollers` | `false` | `rollers` | Walker: Rollschuhe |
| `actions` | `training` | `actions` | Walker: `training` (Skala 1, kein Filter) oder `robot` (Skala 0,9, Tiefpass Kopf 0,5 / Beine 0,7) |
| `kern_extras` | `false` | `kernExtras` | Walker: Kerns Zusätze (die Spielschicht `play.onnx` mit Ball und Socke, die dann statt Pollens BallPlay spielt; Schnabel beim Aufheben) |
| `servo_delay` | 0,02 s | `servoDelay` | Walker: Kommandolatenz der Servos (echte Sekunden) |

Die Walker-Schlüssel werden nur bei `mode = walker` geschrieben (`kernExtras` und `rollers` nur, wenn `true`, `servoDelay` nur, wenn ≠ 0,02).
Beim Lesen wird `gait` auf `velstand`/`alpha` und `actions` auf `training`/`robot` normalisiert.
Was die Walker-Felder bewirken, beschreibt [Kapitel 13](13-micro-duck.md).

### Waffe (`WeaponComponent`, `weapon`)

Eine Hitscan-Waffe für Soldaten-Spieler (im Mehrspieler serverautoritativ und lag-kompensiert).

| Feld | Standard | Schlüssel |
|---|---|---|
| `kind` | `rifle` | `kind` (`rifle`, `pistol`, `shotgun`, `sniper`) |
| `damage` | 24 (pro Treffer bzw. Schrotkugel) | `damage` |
| `fire_rate` | 600 Schuss/min | `fireRate` |
| `automatic` | `true` | `automatic` |
| `magazine`, `reserve` | 30, 120 | `magazine`, `reserve` |
| `reload_time` | 2,0 s | `reloadTime` |
| `spread`, `aim_spread` | 1,2°, 0,15° | `spread`, `aimSpread` |
| `recoil` | 0,8° pro Schuss | `recoil` |
| `range` | 300 m | `range` |
| `headshot` | 2,5 | `headshot` (Schadensfaktor auf Kopf-Collidern) |
| `impulse` | 4 N·s | `impulse` (auf getroffene dynamische Körper) |
| `pellets` | 1 | `pellets` (Schrotflinte: Strahlen pro Schuss) |
| `aim_fov` | 42° | `aimFov` |
| `fire_sound`, `reload_sound` | `builtin:smg`, `builtin:reload` | `fireSound`, `reloadSound` |

`weapon_preset(kind)` liefert die Handhabung eines Typs:

| | Schaden | Rate | Magazin/Reserve | Nachladen | Streuung (Hüfte/Ziel) | Rückstoß | Reichweite | Kopf | Impuls | Zielsicht |
|---|---|---|---|---|---|---|---|---|---|---|
| `rifle` | 24 | 600, auto | 30/120 | 2,0 s | 1,2°/0,15° | 0,8 | 300 m | ×2,5 | 4 | 42° |
| `pistol` | 30 | 360 | 12/60 | 1,4 s | 1,0°/0,25° | 1,1 | 120 m | ×2,0 | 4 | 52° |
| `shotgun` | 11 × 8 | 75 | 6/36 | 2,6 s | 4,5°/3,2° | 2,4 | 45 m | ×1,5 | 2 | 55° |
| `sniper` | 90 | 45 | 5/25 | 2,8 s | 3,0°/0,02° | 3,0 | 1000 m | ×3,0 | 12 | 16° |

Sounds: Pistole `builtin:pistol`, Schrotflinte `builtin:launcher`, Scharfschützengewehr
`builtin:sniper`.

### Gesundheit (`HealthComponent`, `health`)

| Feld | Standard | Schlüssel |
|---|---|---|
| `max_health` | 100 | `maxHealth` |
| `armor` | 0 (fängt Schaden zuerst ab) | `armor` |
| `regen_delay` | 5 s nach dem letzten Schaden (0 = keine Regeneration) | `regenDelay` |
| `regen_rate` | 12 HP/s | `regenRate` |
| `respawn` | `true` (Spieler am Spawn, andere an Ort und Stelle) | `respawn` |
| `respawn_delay` | 3 s | `respawnDelay` |
| `points` | 10 (für den Spieler, der ihn erledigt) | `points` |
| `hit_sound`, `death_sound` | `builtin:hit`, `builtin:down` | `hitSound`, `deathSound` |
| `explode_radius` | 0 (> 0: explodiert beim Zerstören) | `explodeRadius` |
| `explode_damage` | 80 (in der Mitte, zum Rand auf 0) | `explodeDamage` |

### Pickup (`PickupComponent`, `pickup`)

| Feld | Standard | Schlüssel |
|---|---|---|
| `points` | 1 | `points` |
| `burst_color` | (0,55, 1,0, 0,92) | `burstColor` |
| `respawn` | 0 s (0 = für diesen Lauf weg) | `respawn` |
| `sound` | `builtin:pickup` | `sound` |
| `heal` | 0 (Gesundheit für den Sammler) | `heal` |
| `ammo` | 0 (Schuss in die Reserve des Sammlers) | `ammo` |

### Trigger (`TriggerComponent`, `trigger`)

| Feld | Standard | Schlüssel | Bedeutung |
|---|---|---|---|
| `action` | `goal` | `action` | `goal`, `hazard`, `checkpoint`, `message`, `launch`, `teleport`, `damage`, `event` |
| `message` | „“ | `message` | Text der Meldung |
| `once` | `true` | `once` | nur einmal auslösen |
| `sound` | `auto` | `sound` | `auto` = Standard der Aktion, „“ = still |
| `strength` | 12 | `strength` | Launch: m/s nach oben (plus vorwärts entlang X des Triggers); Damage: Schadenspunkte |
| `target` | 0 | `target` | Teleport: Actor, an dem man ankommt |

### Audio Source (`AudioComponent`, `audio`)

| Feld | Standard | Schlüssel |
|---|---|---|
| `sound` | `builtin:hum` | `sound` (`builtin:<name>` oder importierte Sound-ID) |
| `volume` | 1 (0..4) | `volume` |
| `pitch` | 1 | `pitch` |
| `loop`, `autoplay` | `true`, `true` | `loop`, `autoplay` (startet mit Play/Simulate) |
| `spatial` | `true` (3D am Actor; sonst 2D auf seinem Bus) | `spatial` |
| `min_distance`, `max_distance` | 2 m, 40 m | `minDistance`, `maxDistance` |
| `rolloff` | `inverse` | `rolloff` |
| `bus` | `sfx` | `bus` (nicht `master`) |
| `doppler` | 1 | `doppler` |
| `pitch_by_speed` | 0 (zusätzlicher Pitch pro m/s des Bodys) | `pitchBySpeed` |
| `delay` | 0 s nach dem Start | `delay` |

### Rotator, Mover, City

| Komponente | Feld | Standard | Schlüssel |
|---|---|---|---|
| `RotatorComponent` (`rotator`) | `rate` | (0, 0, 60) °/s um die lokalen Achsen | `rate` |
| `MoverComponent` (`mover`) | `offset` | (0, 0, 1) m relativ zur Startposition | `offset` |
| | `period` | 2 s pro Zyklus | `period` |
| | `phase` | 0 (0..1) | `phase` |
| | `mode` | `sine` | `mode` (`sine`, `pingPong`, `loop`) |
| `CityComponent` (`city`) | `seed` | 20261022 | `seed` |
| | `blocks` | 7 pro Seite | `blocks` |
| | `pitch` | 80 m von Straße zu Straße | `pitch` |
| | `street` | 16 m Straßenbreite | `street` |
| | `max_height` | 84 m (höchstes Gebäude im Zentrum) | `maxHeight` |
| | `trees` | `true` | `trees` |
| | `traffic` | 22 Autos | `traffic` |
| | `night_windows` | `true` (Shader-Erweiterung) | `nightWindows` |

Der Mover bewegt `sine` als ½ − ½·cos(2πu), `pingPong` linear hin und zurück, `loop` linear mit
Sprung zurück; u = Zeit / Periode + Phase. Der Rotator dreht um die Achse von `rate` mit deren
Betrag. Beides berechnet `GameWorld::animated_local` aus der Simulationszeit.

---

## 8.5 WorldSettings

| Feld | Standard | Schlüssel | Bedeutung |
|---|---|---|---|
| `gravity` | (0, 0, −9,80665) | `gravity` | |
| `substeps` | 4 | `substeps` | Box3D-Sub-Steps je Schritt (1..16) |
| `step_hz` | 60 | `stepHz` | feste Schrittrate (15..480 Hz) |
| `time_of_day` | 0,42 | `timeOfDay` | 0..1 (0,25 Sonnenaufgang, 0,5 Mittag, 0,75 Untergang) |
| `day_length` | 0 | `dayLength` | Sekunden pro Tag beim Spielen, 0 = eingefroren |
| `sun_azimuth` | 210° | `sunAzimuth` | wo die Sonne mittags steht |
| `fog_density` | 0,0009 | `fogDensity` | |
| `exposure` | 1 | `exposure` | |
| `ao_radius` | 1,6 m | `aoRadius` | Reichweite der Ambient Occlusion, 0 = aus |
| `shadow_distance` | 80 m | `shadowDistance` | wie weit die Schattenkarte der Sonne die Sicht abdeckt (2..2000 m; kleiner: schärfer; [Kapitel 4](04-grafik.md)) |
| `clouds` | `true` | `clouds` | |
| `ground_color` | (0,19, 0,27, 0,15) | `groundColor` | Farbe des endlosen Bodens |
| `ground` | `true` | `ground` | endloser Boden unter allem |
| `ground_friction`, `ground_restitution` | 0,7, 0 | `groundFriction`, `groundRestitution` | Material des Bodens |
| `ground_surface` | „“ | `groundSurface` | „“ oder `floor` |
| `contact_hertz`, `contact_damping`, `contact_speed` | 30 Hz, 10, 3 m/s | `contactHertz`, `contactDamping`, `contactSpeed` | Kontaktweichheit (Box3D) |
| `kill_z` | −50 m | `killZ` | ein Spieler darunter respawnt |
| `title` | „“ | `title` | Spieltitel (sonst der Levelname) |
| `goal` | `none` | `goal` | `none`, `collectAll`, `reachGoal`, `survive`, `score` |
| `time_limit` | 0 | `timeLimit` | Sekunden, 0 = keins |
| `score_target` | 0 | `scoreTarget` | für `score` |
| `player` | 0 | `player` | der gesteuerte Actor (0 = der erste mit Controller) |
| `observer` | `false` | `observer` | Play beginnt mit freier Kamera (fliegen, zuschauen, Körper greifen; F8 übernimmt den Spieler) |
| `master_volume` | 1 | `masterVolume` | |
| `music`, `music_volume` | „“, 0,6 | `music`, `musicVolume` | Musik-Loop auf dem Music-Bus |
| `ambience`, `ambience_volume` | „“, 0,6 | `ambience`, `ambienceVolume` | Ambience-Loop |
| `reverb` | 0,25 | `reverb` | Hallanteil 0..1 |
| `doppler` | 1 | `doppler` | Doppler-Faktor für alle 3D-Sounds |

---

## 8.6 Das `.klevel`-Format

Ein Level ist ein JSON-Dokument. `to_json` / `to_text` schreiben es, `from_json` / `from_text`
lesen es; `actor_to_json`/`actor_from_json` und `world_to_json`/`world_from_json` gibt es auch
einzeln (Studio und Logik nutzen sie).

### Oberste Ebene

| Schlüssel | Inhalt |
|---|---|
| `format` | muss `"kern-level"` sein |
| `version` | 1; ein Level mit größerer Version wird abgelehnt („written by a newer Kern Studio“) |
| `name` | Levelname |
| `world` | WorldSettings (8.5) |
| `camera` | Editor-Kamera: `position`, `yaw`, `pitch` (−90..90) |
| `nextId` | nächste freie Actor-ID |
| `actors` | Liste der Actors (höchstens 100 000) |
| `textures` | Liste: `id`, `name`, `width`, `height`, `src` (Data-URL `data:image/…`) |
| `sounds` | Liste: `id`, `name`, `duration`, `src` (`data:audio/…`, `data:video/…` oder `data:application/ogg`) |
| `logic` | Level-Graph (nur geschrieben, wenn nicht leer; Format in [Kapitel 9](09-logik.md)) |

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

`parent`, `folder`, `hidden`, `locked` und `tags` werden nur geschrieben, wenn sie vom Standard
abweichen. Die Komponenten-Schlüssel stehen in den Tabellen von 8.4.

### Schreiben

- Zahlen und Vektoren werden auf 10⁻⁶ gerundet („genug für ein Editor-Dokument“), `-0` wird `0`.
- Trägheitstensoren mit sieben signifikanten Stellen.
- Farben als `#rrggbb`.
- Optionale Schlüssel, die nur bei Bedarf erscheinen: `mesh.asset` und `mesh.surface` (nicht leer),
  `body.centerOfMass`/`body.inertia` (expliziter Tensor), `colliders[].points` (nicht leer),
  `colliders[].surface` (nicht leer), `colliders[].impactSound` (≠ `auto`), `joints[].reference`
  (≠ 0), die `servo*`-Schlüssel (nur bei `servo = true`), die Walker-Schlüssel des Controllers,
  `world.groundSurface` (nicht leer), `world.observer` (nur `true`), `logic`.

### Lesen

`from_json` ist bewusst nachsichtig bei Fehlendem und streng bei Falschem:

- **Unbekannte Schlüssel werden ignoriert, fehlende bekommen Standardwerte.** Ältere Dateien laden
  so in neueren Versionen.
- **Falsche Typen sind Fehler** mit Meldung („friction must be a number“, „size must be [x, y, z]“,
  „color must be a #rrggbb colour“, „mode: unknown value“). Der Fehler nennt den Actor.
- **Zahlen werden auf ihren Bereich begrenzt**, z. B.:

| Bereich | Schlüssel |
|---|---|
| 0..1 | `roughness`, `metalness`, `opacity`, `restitution`, `mover.phase`, `reverb`, `timeOfDay` |
| 0..10 | `friction`, `rollingResistance`, `groundFriction` |
| 3..128 | `mesh.segments` |
| 1..16 / 15..480 | `substeps` / `stepHz` |
| 1..1000 | `contactHertz` |
| 0,01..100 | `contactSpeed` |
| 0..1000 | `springHertz` |
| 0..100 | `springDamping` |
| −360..360 | `reference` |
| −3600..3600 | `servoTarget` |
| 20..120 | `cameraFov` |
| 1..16 | `city.blocks` (dazu: `street` höchstens `pitch / 2`) |
| 0..4 | Lautstärken, `doppler`; `pitch` 0,05..8 |
| −32768..32767 | `group` |

- Vektorkomponenten müssen endlich und betragsmäßig ≤ 10⁷ sein.
- **IDs:** Fehlende oder doppelte IDs werden neu vergeben. Ein `parent`, der nicht existiert oder auf
  sich selbst zeigt, wird 0; **Eltern-Zyklen werden aufgebrochen.**
- Texturen ohne ID oder ohne `data:image/`-Quelle und Sounds ohne ID, mit `builtin:`-ID oder mit
  unpassender Quelle werden übergangen.
- `timeOfDay ≥ 1` wird 0; `maxDistance ≤ minDistance` wird auf `minDistance + 0,01` gesetzt.

### Prüfen: `validate(level)`

`validate` liefert `Issue`s (`error`, `actor`, `text`). **Fehler** verhindern Play, **Warnungen**
erscheinen im Output Log.

| Fehler | Warnungen |
|---|---|
| fehlende oder doppelte ID | Gelenk an einem Actor ohne Body (wird ignoriert) |
| Eltern-Actor existiert nicht | Gelenkziel ohne Body und ohne Kollision (wird ignoriert) |
| Transform nicht endlich | Sound-Referenz existiert nicht (bleibt still) – für Body, Collider, Gelenk, Controller, Waffe, Gesundheit, Pickup, Trigger, Audio Source, Musik, Ambience |
| Skalierung (nahezu) 0 | Audio Source ohne Sound |
| Gelenk verbindet den Actor mit sich selbst | Waffe ohne Soldaten-Controller |
| Gelenkziel existiert nicht | Controller an einem nicht dynamischen Body |
| Gelenkachse ist null | Pickup/Trigger mit Collidern, aber keiner ist Sensor |
| Controller ohne Body am selben Actor | mehr als ein Player Controller (der erste wird übernommen) |
| City ist nicht auf oberster Ebene | Ziel „reach goal“, aber kein Goal-Trigger |

`sound_reference_ok` akzeptiert „“, `none`, `auto`, ein bekanntes `builtin:<name>` oder eine
importierte Sound-ID.

---

## 8.7 Vom Level zu Box3D: `plan_bodies`

`plan_bodies(level)` beschreibt, welche Box3D-Körper und Compound-Formen ein Level braucht. Die
Simulation baut daraus ihre Welt, und Studio zeichnet damit die Collider im Editor – beide sehen
also dasselbe.

1. **Actors mit Body-Komponente** (außer Städten) bekommen einen Body am Weltframe des Actors (ohne
   Skalierung). Ein statischer Body mit Rotator oder Mover wird kinematisch.
2. **Implizite Bodies:** Actors mit eigenen Collidern, Meshes mit Kollision, Pickups und Trigger *außerhalb* jedes Bodys bekommen
   einen eigenen – statisch, oder kinematisch, wenn sie oder ein Vorfahr animiert sind.
3. **Formen:** Jeder Actor trägt zu seinem Body-Besitzer bei (sich selbst, einem Vorfahren oder
   seinem impliziten Body), mit seiner Transform relativ zum Body-Frame:
   - eigene Collider so, wie sie sind (`auto` wird mit `fit_collider` an das Mesh angepasst),
   - sonst ein an das Mesh angepasster Collider mit dem Physik-Material des Meshes
     (Reibung, Restitution, Rollwiderstand, Oberfläche); bei Pickups und Triggern als Sensor,
   - Pickups und Trigger ohne Mesh und Collider bekommen eine Sensor-Kugel mit 1 m Radius.
4. Implizite Bodies ohne Form fallen weg; Bodies mit Komponente simulieren auch ohne Form
   (Punktmassen).
5. **Städte:** ein statischer Body mit einer Box je Gebäude und Gehweg (Reibung 0,5).
6. **Der endlose Boden:** eine statische Box 20 000 × 20 000 × 2 m, Oberkante bei z = 0, mit
   `groundFriction`, `groundRestitution` und `groundSurface`.

Formregeln (`add_shape`): Skalierung geht exakt in Größen, Radien und Hull-Punkte ein. Bewegte
Formen sind mindestens 1 cm groß, statische mindestens 2 mm ([Kapitel 6](06-physik.md)).
Automatische Collider passt `fit_collider` an die Mesh-Form an: Box und Plane werden eine Box,
Sphere eine Kugel, Capsule eine Kapsel, alle anderen Formen (auch Torus) eine konvexe Hull aus den
Mesh-Punkten.

Test: `game_body_planning` (Wagen mit eingeschweißtem, skaliertem Rad; Wand als impliziter
statischer Body; Münze als impliziter kinematischer Sensor; Boden).

---

## 8.8 Die GameWorld starten

`GameWorld::start(level, mode)` führt diese Schritte aus:

1. `validate`; ein Fehler bricht ab (`last_error()` nennt ihn).
2. `sync(level)`, das Level wird zur laufenden Kopie (`level_`), das Original gemerkt.
3. **Box3D-Welt** mit Schwerkraft und Kontaktweichheit des Levels. Die Hit-Schwelle ist
   `kHitSpeed = 2 m/s` (Logik-Event „On Hit“, Kamerawackeln); hat ein dynamischer Body einen
   Aufprall-Sound mit niedrigerer Schwelle, wird sie bis auf dessen `impactThreshold` gesenkt
   (mindestens 0,05 m/s – die Taps und Schritte eines kleinen Roboters).
4. Alle Actors „lebendig“; ob das Level Logik-Graphen hat (davon hängen die Events ab).
5. **Reihenfolge:** Actors Eltern vor Kindern (`order_`).
6. **Bodies** aus `plan_bodies` (`create_body`, siehe unten).
7. **Gelenke** aller Actors (`create_joints`), dann **Walker** (`create_walkers`).
8. **Stadtverkehr:** kinematische Autos (Box 4 × 1,9 × 1,6 m, schlafen nie) auf den Fahrspuren.
9. **Spieler** (nur Play): `default_player`, wenn er einen dynamischen Body hat, sonst der erste
   übernehmbare Actor. Er bekommt einen Pawn mit Spawn-Pose; Charaktere und Soldaten wechseln auf
   den Character Mover.
10. **Regeln:** HUD zurücksetzen (Bestzeit bleibt), Titel, Zahl der Pickups, Status `playing` (Play)
    bzw. `inactive` (Simulate), Zeitlimit, Boost; in Play erscheint der Titel als Meldung.
11. **Kampf:** Gesundheit und Munition initialisieren.
12. Weltmatrizen berechnen, **Logik** starten (`start_logic`, dann `begin`).

### `create_body`

- Typ, Pose und – bei expliziten Bodies – Dämpfung, Gravity Scale, Bullet, Wach/Schlaf und
  Anfangsgeschwindigkeiten (Drehung Grad/s → rad/s).
- **Spieler-Fahrzeuge** (Hovercar, Vehicle) sind immer Bullets; **Spieler schlafen nie**.
- Jede Form: Dichte, Reibung, Restitution, Rollwiderstand, Oberfläche, Sensor, Kanäle, Gruppe;
  der Benutzerwert ist der beitragende Actor (so findet ein Treffer den eingeschweißten Kopf).
- Events nur, wo sie gelesen werden: Sensor-Events für Sensoren, Spieler und (mit Logik) alles
  Bewegliche; Kontakt-Events nur am Spieler; Hit-Events an dynamischen Bodies, wenn Spieler,
  Aufprall-Sound oder Logik.
- Abgelehnte Formen werden übersprungen und gemeldet („Collider of "…" skipped: …“).
- Masse: `mass > 0` bei dynamischen Bodies → mit explizitem Tensor `set_mass_data`, sonst `set_mass`.
- Sperren aus der Komponente; Controller außer Rolling Ball und Walker sperren zusätzlich Rollen und
  Nicken (Fahrzeuge, Charaktere und Flieger bleiben aufrecht).

### `create_joints`

- Ohne Body am Actor oder ohne Body am Ziel wird das Gelenk mit einer Meldung übergangen. Ziel 0 ist
  die Welt: ein statischer Body am Anker.
- **Körperreihenfolge:** Box3D misst B relativ zu A. Der Actor bewegt sich relativ zu seinem Ziel,
  deshalb ist das **Ziel Body A und der Actor Body B** – positive Slider-Wege, Scharnierwinkel und
  Motorgeschwindigkeiten bewegen den Actor. Ausnahme Wheel: Hier bleibt Box3Ds Ordnung (A = Chassis
  = der Actor, B = Rad).
- Frames aus Anker und Achse in Weltkoordinaten (`frame_from_axis`; Slider entlang X, sonst Z). Das
  Wheel-Gelenk dreht um die Achse und federt entlang der zur Achse senkrecht gemachten Hochachse des
  Chassis.
- `reference ≠ 0` (Hinge): Frame A wird um den Referenzwinkel zurückgedreht, damit das Gelenk in der
  Baupose diesen Winkel liest.
- Federn auf `max_hertz = 0,5 × step_hz × substeps` begrenzt; Servos mit Gelenkweichheit
  `min(240, max_hertz)` Hz und Dämpfung 2.
- Zuordnung: Hinge → Revolute (Grad → Radiant), Slider → Prismatic, Ball → Spherical (Kegel
  `upper`, Motor als Winkelgeschwindigkeit um die Achse), Fixed → Weld (Feder = Weichheit), Distance
  → Distance (Seil am Ursprung des Ziels bzw. am Weltanker, Ruhelänge 0 = Abstand beim Start),
  Wheel → Wheel, Motor → Motor-Gelenk (Feder 4 Hz Standard), Parallel → Parallel (8 Hz Standard),
  No Collision → Filter.
- Bruchschwellen aus `breakForce`/`breakTorque`; Servoparameter in den Laufzeit-Zustand
  (`SimJoint`).

### Modi

| `SimMode` | Bedeutung |
|---|---|
| `none` | keine Simulation (Editor) |
| `simulate` | Physik und Logik laufen, niemand wird gesteuert; Körper lassen sich greifen |
| `play` | wie Simulate, dazu der übernommene Spieler, Regeln und HUD |
| `replica` | Kopie eines Server-Spiels für einen Netzwerk-Client: Körper folgen gesetzten Posen, Regeln laufen auf dem Server ([Kapitel 10](10-netzwerk.md)) |

`set_mode` wechselt zwischen Simulate und Play im Lauf (Studio: F8 „Eject“/„Possess“); der Pawn
behält seinen Zustand. `restart` startet das ursprüngliche Level neu, `set_paused` hält an,
`step_once` macht genau einen festen Schritt (auch pausiert; Studio: „Frame“, Taste `.`).
`session()` zählt Starts, damit Zuhörer einen neuen Lauf bemerken.

![Simulate im Studio: die Physik läuft, niemand wird gesteuert, Körper lassen sich greifen](bilder/studio-simulate.png)

---

## 8.9 Ein Frame

### In der GameWorld

`step(dt, input)` verbraucht die Frame-Zeit in festen Schritten (Akkumulator, Begrenzung auf 0,25 s
und 0,1 s Simulation pro Frame – [Kapitel 6](06-physik.md), Abschnitt 6.13; zum Prinzip des festen
Zeitschritts siehe [Anhang C](C-grundlagen-physik.md), C.2.4). Vor jedem Schritt
merkt es sich die Posen beweglicher Körper. Ein fester Schritt, `fixed_step(input, h)`:

1. **Spieler** (nur bei Status `playing`): für jeden Pawn die Eingabe wählen (lokal: die des
   Frames; entfernt: die zuletzt empfangene), Blickrichtung übernehmen, Eingabe-Events für Logik
   (`logic_inputs`), dann je nach Modus `drive_soldier`, `drive_walker` oder `drive_player`. Der
   lokale Pawn ruht nach Eject in Simulate.
2. **Kinematische Animation:** Weltmatrizen für `t + h`, und jeder Rotator-/Mover-Body bekommt per
   `set_target_pose` sein Ziel.
3. **Verkehr** (`drive_traffic`; Autos halten vor dem Spieler), **Walker** (`drive_walkers`),
   **Servos** (`drive_servos`), **Maus-Griff** (`grab_.update`).
4. **`physics_.step(h, substeps)`** (Sub-Steps 1..16).
5. Schrittzähler und `sim_time_`; HUD-Zeit und Restzeit.
6. **Events** (`handle_events`): Pickups und Trigger des Spielers, Logik-Überlappungen und -Treffer,
   Kamerawackeln, Aufprall-Sounds, brechende Gelenke.
7. **Gesundheit** (Regeneration, Respawns) und **Logik-Tick**.
8. Wiederkehrende Pickups; Zeitlimit (`survive` gewinnt, sonst verloren); **Kill-Z** (ein Pawn
   darunter respawnt); Spielergeschwindigkeit ins HUD; Zielpunktzahl erreicht → gewonnen.

Danach läuft die Tageszeit weiter (`day_length`), die Walker-Stimmen werden vorbereitet, und
`compute_world_matrices` interpoliert die Posen mit `alpha`.

- `update_camera(cam, dt, input)` bewegt die Kamera wie die des Spielers (8.11).
- `present(now_ms, eye)` lädt Posen und Sichtbarkeit in den Renderer (versteckte Actors und
  Kinder versteckter Eltern, eingesammelte Pickups und benutzte Trigger sind unsichtbar), zeichnet
  den Verkehr, Schüsse und Partikel.
- `apply_lighting(eye)` setzt Licht, Himmel, Nebel und Punktlichter und liefert den Nachtfaktor für
  die beleuchteten Fenster (`NightWindows`, eine Shader-Erweiterung des Renderers).

### Im Studio

`Studio::frame(now_ms)` ruft pro Bild (`kern_studio_frame`):

1. Editor-Kameras und Touch-Steuerung aktualisieren.
2. Beim Spielen: Eingabe sammeln; mit Netz `update_net` ([Kapitel 10](10-netzwerk.md)), sonst
   `world_->step(dt, input)`; Logik-Breakpoints prüfen; Play-Kamera (`update_camera`); Meldungen
   der Welt ins Output Log.
3. Beim Editieren: Level mit der Welt synchronisieren.
4. Overlays, `present`, Audio (`update_audio`), dann jeden sichtbaren Viewport rendern.

`dt` ist auf 0,1 s begrenzt.

---

## 8.10 Controller: wie der Spieler fährt, rollt, läuft und fliegt

`PlayInput` ist die Eingabe eines Spielers für einen Frame:

| Feld | Bedeutung |
|---|---|
| `move_x`, `move_y`, `up` | −1..1 (rechts, vorwärts, hoch) |
| `jump`, `boost` | Tasten |
| `robot` | Walker-Bewegungen als Bits: 1 Sitzen/Stehen, 2 Aufheben, 4 Kick links, 8 Kick rechts, 16 Rolle, 32 Schnabel, 64 Zwitschern, 128 Wheee, 256 Sprung (Kern) |
| `look_x`, `look_y` | Kameradrehung in Grad seit dem letzten Frame |
| `zoom` | Kameraabstand (Mausrad) |
| `view_yaw`, `has_view_yaw`, `view_pitch` | Blickrichtung entfernter Spieler (Grad) |
| `fire`, `aim`, `reload`, `crouch`, `interact` | Soldat |
| `look_stick_x`, `look_stick_y`, `gamepad` | rechter Gamepad-Stick, Eingabe vom Pad |
| `rewind` | Server: Sekunden, die der Schütze die Welt in der Vergangenheit sah (Lag-Kompensation) |

Bewegung ist bei Rolling Ball, Charakter und Flieger **kamerarelativ**. Die Modi
(`drive_player`, `drive_soldier`, `drive_walker`):

| Modus | Umsetzung |
|---|---|
| **Hovercar** | Gas als Kraft entlang der Fahrzeugachse (rückwärts 1,25-fach, Boost +42 m/s²), Höchstgeschwindigkeit als Gegenkraft, Rollwiderstand ohne Gas; **Seitenhaftung als Kraft** (stetige Impulse kämpfen gegen den Solver); Lenkung als Gierrate proportional zur Geschwindigkeit bis 9 m/s. Eine **Schwebefeder** über einem Box3D-Strahl hält `hover_height`. Sichtbares Wippen und Neigen sind nur Darstellung. |
| **Vehicle** | wie Hovercar ohne Schwebefeder; angetrieben wird nur mit Bodenkontakt (Kugelprobe). |
| **Rolling Ball** | Kraft in Bewegungsrichtung; **Sprung als Impuls** m · (`jump_speed` − v_z), nur am Boden; Höchstgeschwindigkeit als Gegenkraft. |
| **Character** | Box3Ds **Character Mover** ([Kapitel 6](06-physik.md)): Der Body wird kinematisch, die Kapsel wird aus seinen Formen bemessen (Füße am tiefsten, Kopf am höchsten Punkt, Stufenhöhe 22 % der Höhe), Bodenbewegung wie in klassischen Ego-Shootern, Schwerkraft 1,5-fach (knackiger). Der Körper dreht sich weich in Laufrichtung und folgt per `set_target_pose`. Schritte nach Schrittlänge; Trigger und Pickups über die berührten Ebenen. |
| **Flyer** | Zielgeschwindigkeit inklusive Hoch/Runter, Beschleunigung als Kraft, die die Schwerkraft aufhebt; Gieren zur Kamerarichtung. |
| **Soldier** | Character Mover mit echter Schwerkraft (Sprung 3,6 m/s → 0,66 m), siehe 8.13. |
| **Walker** | Die Eingabe geht an den Controller des Roboters, der seine Servos mit der trainierten Policy treibt ([Kapitel 13](13-micro-duck.md)). |

Gemeinsam: Boost ist ein Vorrat 0..1 (−0,45/s beim Boosten, +0,12/s sonst), Landungen nach freiem
Fall über 3,5 m/s spielen `builtin:land`, Sprünge und Boost ihre Sounds.

### Possession und Pawns

Ein **Pawn** ist der Steuerungszustand eines Spielers (lokal und jeder entfernte): Actor-IDs, letzte
Eingabe, Blickrichtung, Spawn-Pose, Boost, Character Mover, Soldatenzustand. `add_pawn(source, id)`
legt einen weiteren Spieler an – eine Kopie des Spieler-Actors mit seinen Kindern, auf einem Ring um
den ursprünglichen Start (Winkel n · 2,39996 rad, Radius 3,5 + 1,2 · √n m), mit Bodies, Gelenken,
Kampfwerten und Logik; `remove_pawn` entfernt sie wieder (der Level-eigene Spieler bleibt).
`set_pawn_input`, `set_local_pawn`, `pawns`, `pawn_source` gehören dazu – das Netz nutzt sie
([Kapitel 10](10-netzwerk.md)).

---

## 8.11 Kamera

`update_camera` folgt der **interpolierten** Pose des Spielers:

| `CameraMode` | Verhalten |
|---|---|
| `chase` (Fahrzeuge) | hinter der Fahrtrichtung, Abstand wächst mit dem Tempo (`camera_distance` × Zoom + 0,12 · v), gedämpft |
| `chase` (sonst) | Orbit um den Spieler mit Maus/Stick; ein Box3D-Strahl hält die Kamera aus Wänden (bei Walkern ohne die eigenen Glieder) |
| `first_person` | Augen am Körper; Fahrzeuge blicken in Fahrtrichtung |
| `top_down` | von oben, gedämpft |

Das Sichtfeld weitet sich über 30 m/s (bis +10,5°); harte Treffer des Spielers schütteln die
Kamera (`shake_`). Soldaten haben eine eigene Kamera (8.13).

---

## 8.12 Regeln, HUD und Trigger

### HUD

`Hud` ist der Spielzustand für die Anzeige: `status` (`inactive`, `playing`, `won`, `lost`),
`title`, `message` und `message_age`, `score`, `pickups`/`pickups_total`, `elapsed`, `remaining`
(< 0 = kein Limit), `speed`, `boost` (< 0 = kein Boost), `best` (beste Zielzeit der Sitzung) und die
Soldatenwerte (`health`, `armor`, `ammo`, `magazine`, `reserve`, `reload`, `spread`, Trefferanzeige,
Schadensrichtung, `respawn_in`, `aiming`, `fov`).

### Siegbedingungen (`GameGoal`)

| Ziel | Gewonnen / verloren |
|---|---|
| `none` | Sandbox |
| `collectAll` | alle Pickups gesammelt; gibt es einen Goal-Trigger, muss man ihn danach erreichen („Collect everything first“) |
| `reachGoal` | Goal-Trigger erreicht |
| `survive` | Zeitlimit überstanden |
| `score` | Zielpunktzahl erreicht |

Mit Zeitlimit verliert man bei 0, außer bei `survive`. Ein Hazard-Trigger verliert immer.
`finish` setzt den Status, spielt den Jingle, merkt die Bestzeit (bei `collectAll`/`reachGoal`) und
feuert das Logik-Event `game.over`.

### Pickups

Berührt ein Spieler ein Pickup (Sensor-Begin oder Kontakt, bzw. eine Ebene des Character Movers),
wird es eingesammelt: Heilung und Munition an den Sammler (ein reines Heilpaket bleibt liegen,
solange der Sammler volle Gesundheit hat), Punkte, Zähler, Sound, Partikel, Logik-Event
`game.pickup`. Mit `respawn > 0` kommt es nach dieser Zeit zurück.

### Trigger-Aktionen

| Aktion | Wirkung | Standard-Sound |
|---|---|---|
| `goal` | gewinnt (bei `collectAll` erst, wenn alles gesammelt ist) | `builtin:goal` |
| `hazard` | verliert | `builtin:hazard` |
| `checkpoint` | Spawn des Pawns liegt nun 1 m über dem Trigger | `builtin:checkpoint` |
| `message` | zeigt die Meldung | `builtin:confirm` |
| `launch` | hebt den Spieler mit `strength` m/s nach oben und halb so schnell entlang X des Triggers | `builtin:boost` |
| `teleport` | versetzt den Spieler zum Ziel-Actor (Blickrichtung übernommen); 1 s Sperre gegen das sofortige Zurück; bleibt benutzbar | `builtin:powerup` |
| `damage` | `strength` Schaden für den Spieler | – |
| `event` | nur das Logik-Event (und die Meldung) | – |

Vor der Aktion feuert jeder Trigger das Logik-Event `game.trigger`. Mit `once` (außer Goal und
Hazard) ist er danach verbraucht.

---

## 8.13 Soldaten und Figuren

Der Soldat ist ein Ego-Shooter-Spieler nach SpawnCity (`src/game/soldier.cpp`), alle Bewegung in
festen Schritten auf der Autorität.

- **Bewegung:** Character Mover, kamerarelativ. **Ducken** (halten) verkleinert die Kapsel auf 60 %;
  aufstehen nur mit Kopffreiheit (Mover-Probe nach oben). **Sprinten** nur vorwärts, nicht beim
  Zielen oder Ducken. Gehen, Zielen (60 %), Ducken (`crouch_speed`).
- **Sprung** mit Puffer (0,14 s vor der Landung gedrückt zählt) und Coyote-Zeit (0,12 s nach einer
  Kante).
- **Fallschaden:** über 7,7 m/s Landegeschwindigkeit, tödlich ab 15 m/s.
- **Waffe:** Nachladen (auch automatisch bei leerem Magazin), Halbautomatik per Flanke, kein Feuer
  beim Sprinten. Jeder Schuss ist ein Box3D-Strahl aus dem Auge; die **Streuung ist
  deterministisch** (gleichverteilt im Kegel, aus Actor-ID, Schusszahl und Kugel gehasht). Andere
  Pawns werden als Kapseln geprüft, mit **Lag-Kompensation**: Der Server nimmt ihre Füße aus dem
  Positionsverlauf von vor `rewind` Sekunden (höchstens 0,5 s). Ein Treffer an einem Actor mit Tag `head` (oder in der
  oberen 0,32 m der Kapsel) ist ein Kopftreffer. Getroffene dynamische Körper bekommen `impulse`.
  Die Streuung blüht durch Bewegung, Luft (×2,5) und Schüsse auf und klingt ab; Ducken ×0,7.
- **Schaden:** Rüstung fängt bis zu zwei Drittel ab; frisch gespawnte Spieler sind 2,5 s geschützt.
  Bei 0 HP: Sound, Punkte für den Schützen, Logik-Event `game.death`; Pawns respawnen nach
  `respawn_delay` (mindestens 0,5 s) am Spawn, andere Actors verschwinden und kehren mit `respawn`
  an Ort und Stelle zurück. Mit `explode_radius` explodiert der Actor: Box3D-Radialimpuls, Schaden mit
  Abstand fallend, Blitz, Sound, Kamerastoß.
- **Kamera:** eigene Ego-Kamera mit Mausempfindlichkeit, Zoom beim Zielen, Gamepad-Kurven und
  Ratenrampen, Rückstoß, Head-Bob, Landedip.
- **Figuren:** `soldier_art.cpp` zeichnet für jeden Soldaten-Actor SpawnCitys gelenkige Figur
  (gehen, laufen, Luft, zielen, tot), im Editor stehend, beim Spielen aus dem Pawn animiert; dazu
  die Waffe in der Hand mit Handschuhen und Ärmeln, Mündungsfeuer, Tracer, Einschusslöcher, Splitter
  und Hülsen. Alles ist reine Darstellung und ändert die Simulation nicht – Autorität, Replikas und
  Editor zeigen dasselbe. Die Modelle (Maschinenpistole, Pistole, .50-Scharfschützengewehr, 40-mm-Werfer) sind
  prozedural aus Grundformen gebaut (`soldier_models.cpp`).

Test: `game_soldier_moves_shoots_and_respawns`, `soldier_art_in_play`,
`soldier_models_are_closed_and_outward`, im Netz `net_game_soldier_replication`.

![Play im Studio mit HUD](bilder/studio-play.png)

---

## 8.14 Walker

Ein Walker ist ein gelenkiger Roboter, der auf seinen eigenen Servos läuft, getrieben von einer
Steuer-Policy – der Micro Duck mit dem trainierten Netz von Pollen Robotics. Die GameWorld legt beim
Start für jeden Walker-Controller einen Controller-Zustand an (`create_walkers`), treibt ihn jeden
Schritt (`drive_walkers`: Sensoren, Verhalten, Netz alle vier Physikschritte, Kommandoverzögerung)
und die Servos (`drive_servos`). Dazu gibt es eine große Schnittstelle für Sensoren, Befehle,
Training und Spielschicht (`walker_sensors`, `walker_command`, `walker_skill`, `walker_probe`,
`set_walker_action` …). Das alles beschreibt [Kapitel 13](13-micro-duck.md).

---

## 8.15 Sound-Anbindung

Die Simulation spielt nichts selbst ab. Sie erzeugt in ihren festen Schritten **Sound-Cues** und ist
damit deterministisch; der Aufrufer leert sie jeden Frame.

```cpp
struct SoundCue {
  std::string sound;                 // "builtin:<name>" oder eine importierte Sound-ID
  Vec3 position;
  f32 volume = 1, pitch = 1;
  bool spatial = true;
  audio::Bus bus = audio::Bus::sfx;
  ActorId actor = 0;
  bool voice = false, silence = false;   // die Stimme eines Actors: eine zur Zeit
};
```

`cue()` ignoriert „“, `none` und `auto` und hält höchstens 64 Cues (ohne Abnehmer, etwa auf einem
Server, gewinnen die neuesten). Cues entstehen für Sprünge, Boost, Landungen, Schritte, Pickups,
Trigger, Sieg/Niederlage, Respawn, Treffer, Schüsse, Nachladen, Explosionen, brechende Gelenke und
Aufpralle.

**Aufprall-Sounds** (in `handle_events`): Jede Form, deren Collider (oder deren Body bei `auto`)
einen Sound hat, merkt ihn sich. Ein Hit über `max(0,05, impactThreshold)` spielt ihn – höchstens
sechs pro Schritt, ein Ton pro Berührung (Sounds mit „step“ im Namen höchstens alle 0,5 s je Actor,
sonst 0,09 s; eine Sohle rollt ab und setzt in kleinen Prellern auf, die Box3D einzeln meldet).
Lautstärke folgt der Annäherungsgeschwindigkeit, `builtin:impact` wird bei leichten Stößen zu
`builtin:impact_light`, und schwere Körper klingen tiefer (Pitch aus log₁₀ der Masse, 0,7..1,15).

Abgespielt wird über `game::GameAudio` (`include/kern/game/sound.h`) auf Kerns Software-Mixer
([Kapitel 7](07-audio.md)):

```cpp
audio::Mixer mixer;  mixer.init(48000);
SoundBank bank(mixer);
GameAudio audio(mixer, bank);
world.start(level, SimMode::play);  audio.begin(world);
// jeden Frame: bank.prepare(4); audio.update(world, camera, dt);  der Host zieht mixer.render()
world.stop();  audio.end();
```

- `SoundBank` macht aus Referenzen Mixer-Clips: Built-ins werden in Zeitscheiben synthetisiert
  (`prepare(budget_ms)`), importierte Dateien dekodiert der Host und liefert PCM (`set_pcm`).
  `request_level` fordert alles an, was ein Level braucht – Play beginnt dann mit allem an Ort.
- `GameAudio::update` beginnt mit jedem neuen Lauf der Welt (Start, Neustart), pausiert mit ihr, setzt
  den Hörer an die Kamera, startet Musik, Ambience und Audio Sources, führt den **Motor-Loop** des
  Spielers (Pitch und Lautstärke folgen Tempo und Gas; ohne Doppler, weil die Kamera folgt) und
  spielt alle Cues. Ein Cue, dessen Clip noch nicht bereit ist, wartet kurz und fällt weg, wenn er
  zu spät käme.
- `engine_sound_for(controller)`: `auto` ist bei Hovercar und Flyer `builtin:hover`, bei Vehicle
  `builtin:engine`, bei Walker `builtin:servo`, sonst keiner.

---

## 8.16 Touch, Maus und Gamepad

`include/kern/game/touch.h` bringt SpawnCitys Touch-System nach C++, damit Studio-Play, exportierte
Spiele und jeder Controller dieselbe Umsetzung nutzen.

**`InputArbiter`** entscheidet, welches Gerät gerade benutzt wird; das zuletzt benutzte gewinnt, mit
SpawnCitys Regeln:

- Start: Desktop, wenn der primäre Zeiger fein ist oder es keine Touchpunkte gibt.
- Ein Finger schaltet auf Touch.
- Eine Maus schaltet ein Gerät mit grobem primärem Zeiger (Handy, Tablet) **nicht** auf Desktop.
- Jede echte Taste schaltet auf Maus+Tastatur, auch am Handy.
- Gamepad-Nutzung wird zusätzlich vermerkt.
- Pointer-Lock gibt es nur mit Maus+Tastatur.

**`TouchControls`** legt das Overlay an und rechnet Finger in `PlayInput` um:

- **Zwei Sticks** von 168 px bündig in den unteren Ecken an den Safe-Area-Rändern (kleiner nur, wenn
  zwei nicht passen; mindestens 72 px). Links bewegen („STEER / GAS“ bei Fahrzeugen, „FLY“ beim
  Flieger, sonst „MOVE“), rechts schauen („LOOK“).
- **Buttons je Controller**, die untere Reihe links neben dem Look-Stick, weitere darüber:

| Modus | Buttons |
|---|---|
| Soldier | JUMP, RELOAD und AIM (beide nur mit Waffe; AIM ist ein Schalter), CROUCH (Schalter), USE |
| Walker | QUACK, PICK, SIT, KICK L, KICK R, ROLL, BEAK, WHEEE, HOP |
| Character | JUMP (mit Sprung), USE |
| Rolling Ball | JUMP, BOOST (wenn vorhanden) |
| Flyer | UP, DOWN, BOOST |
| Hovercar, Vehicle | BOOST, HOP (mit Sprung), USE |

- **Zuständigkeit pro Finger:** Buttons vor Sticks vor Bildschirm. Ein Finger auf dem Bildschirm
  feuert beim Soldaten (und zielt mit), sonst schaut er.
- **Move-Stick:** analog; bis zum Rand nach vorn gedrückt heißt Sprinten (Charakter, Soldat).
- **Look:** 1:1-Ziehen mit „Flick“-Verstärkung bei schnellen Bewegungen, dazu **Edge Turn**: Wer
  über 92 % des Auslenkradius (0,42 × Ringgröße) hinauszieht, dreht weiter (bis 4,2 rad/s Gieren, 3,3 rad/s Nicken, über 42 px
  eingeblendet, mit SpawnCitys Stick-Kurve). Gezoomte Ansichten drehen langsamer.
- `update` liefert die Eingabe eines Frames inklusive der Walker-Bits; `view()` beschreibt, was die
  Seite zeichnet.

Gamepads (Standardbelegung) übergibt die Seite pro Frame an die Engine; Kern wendet Dead-Zones, Kurven
und die Zielraten des Soldaten an (`web/studio/game.mjs`, `GamepadInput`). Tests:
`touch_controls_layout_and_sticks`, `input_device_arbiter_rules`, `studio_touch_play_without_pointer_lock`.

---

## 8.17 Logik in der Welt

Jeder Actor und das Level können einen Logik-Graphen tragen. Die GameWorld startet sie auf der
Autorität (Standalone oder Server) in Play und Simulate, liefert ihnen Ereignisse (Überlappung,
Treffer, Pickup, Trigger, Schaden, Tod, Respawn, Interact, Eingabe, Taste, Spielende, neuer Spieler)
und bietet ihren Knoten Operationen an (`set_property`, `spawn_copy`, `teleport`, `push_actor`,
`set_joint_motor`, `line_trace`, `add_score`, `end_game` …). Was Logik ändert, wird für das Netz
mitgeschrieben. Alles Weitere in [Kapitel 9](09-logik.md).

---

## 8.18 Die Demo-Levels

`include/kern/game/demos.h` enthält die eingebauten Levels; Studio bietet sie unter File › Example
Levels und File › New Level an.

| Funktion | Level | Inhalt |
|---|---|---|
| `orb_run_level()` | **Orb Run** | Prozedurale Stadt mit Verkehr; ein Box3D-Hovercar (1200 kg, Box-Collider, Reibung 0,25; Kabine, Spoiler, vier leuchtende Düsen und Scheinwerfer als eingeschweißte Kinder) mit Chase-Kamera; 24 Energie-Orbs (Pickups mit Sensor-Kugel 2,4 m, Rotator, Mover, Licht) auf den Straßen; Ziel `collectAll`; spät am Nachmittag mit 480 s Tageslänge – der Lauf endet in der Nacht; Musik `music_drive`, Ambience `city`. Studio startet mit diesem Level. |
| `physics_lab_level()` | **Physics Lab** | Alle Gelenktypen und Bewegungsarten: Kistenpyramide (schlafend gestartet), Pendel an einem Distance-Gelenk, Tür an einem Scharnier mit Limits und Schließfeder, Seilbrücke aus 12 Planken (drei mittlere Gelenke brechen bei 60 000 N), Drehteller mit Motor (45°/s) und drei Mitfahrern, Lift auf einem Slider mit Motor und Limit, ein selbstfahrender Rover auf vier Wheel-Gelenken mit Federung und Antrieb; der Spieler ist eine rollende Kugel, dazu fünf Sterne, ein rotierender Hazard-Kehrer und ein Goal; Ziel `reachGoal`. |
| `shooting_range_level()` | **Shooting Range** | Ego-Shooter-Schießstand: Soldat mit Gewehr, Gesundheit und Kopf; acht Zielpuppen (Gesundheit mit schneller Regeneration, Kopf-Tag) in 10–60 m, Deckung, verschiebbare Kisten und ein Fass, ein Turm mit Rampe; Ziel 150 Punkte; im Mehrspieler unverändert spielbar. |
| `micro_duck_level()` | **Micro Duck** | Ein Zimmer im Maßstab des Roboters (2,5 × 2,5 m echt, also 10 × 10 m in der Welt): Holzboden (Reibung 1, Oberfläche `floor`), Wände, Schuhkarton, Bücher, Kiste, Blumentopf, ein leichter Ball (Rollwiderstand 0,03), zwei Socken; die Ente autonom und mit Kerns Spielnetz; 100 Hz, 16 Sub-Steps, kein endloser Boden; **Observer-Kamera**; ein Level-Graph lässt die Ente zwitschern, sobald ihre Kopfkamera den Ball sieht. |
| `basic_level()` | **Basic** | Boden 40 × 40 m und ein Würfel. |
| `empty_level()` | **Empty** | nichts, auch kein endloser Boden. |

Tests: `game_world_orb_run_play`, `game_world_physics_lab`, `game_shooting_range_template`,
`micro_duck_example_level_runs`, `studio_starts_with_orb_run`.

---

## 8.19 Weitere Schnittstellen der GameWorld

| Bereich | Methoden |
|---|---|
| Darstellung | `set_layers`, `sync`, `clear`, `actor_for_mesh`, `actor_matrix`, `actor_bounds`, `actor_meshes`, `pending_textures`, `set_texture_pixels`, `time_of_day` |
| Zustand | `running`, `mode`, `paused`, `player`, `camera_yaw`, `camera_pitch`, `sim_time`, `hud`, `hud_mut`, `stats` (`SimStats`: Bodies, wache Bodies, Formen, Gelenke, gebrochene, Kontakte, Schritte), `level`, `physics`, `actor_body`, `actor_alive`, `last_input` |
| Debug | `debug_lines(colliders, contacts, joints, …)` – Collider farbig nach Typ (statisch grün, kinematisch blau, dynamisch orange, Sensor violett, schlafend grau), Kontaktpunkte mit Normalen, Gelenke mit Achsen |
| Meldungen | `events()` (Pickups, gebrochene Gelenke, übergangene Collider, Logik-Ausgaben), `sound_cues()`, `shots()` |
| Kampf | `damage`, `health_of`, `set_health`, `heal`, `give_ammo`, `soldier_state`, `apply_soldier_state`, `set_view` |
| Netz | `add_pawn`, `remove_pawn`, `set_pawn_input`, `set_local_pawn`, `pawns`, `take_alive_changes`, `dead_actors`, `set_alive`, `networked_actors`, `start_replica`, `set_replica_pose`, `step_replica`, `take_replicated_changes`, `replicated_state` |
| Logik | `logic_runtime`, `key_event`, `set_property`, `spawn_copy`, `teleport`, `actor_velocity`, `set_actor_velocity`, `push_actor`, `set_simulate_physics`, `set_joint_motor`, `find_actor`, `actors_with_tag`, `has_tag`, `line_trace`, `add_score`, `end_game`, `show_message`, `play_sound`, `particle_burst`, `shake_camera`, `destroy_actor`, `call_logic_event` |
| Griff | `begin_grab`, `move_grab`, `end_grab`, `grabbing` |

---

## 8.20 Tests

| Test | Prüft |
|---|---|
| `game_euler_and_transforms` | Euler ↔ Quaternion, Matrizen |
| `game_level_round_trip_and_validation` | `.klevel` hin und zurück, `validate` |
| `game_body_planning` | `plan_bodies` |
| `game_city_generation` | Stadtgenerator |
| `game_world_presentation` | Darstellung |
| `game_world_orb_run_play` | Orb Run spielen |
| `game_world_physics_lab` | Physics Lab (Gelenke, Bruch, Griff, Play-Wechsel) |
| `game_world_rules` | Goal gewinnt, Hazard verliert, Bestzeit, Restzeit |
| `game_orbit_camera_mouse_sense` | Kamerarichtung |
| `game_soldier_moves_shoots_and_respawns` | Soldat |
| `game_character_mover` | Character Mover |
| `game_gameplay_blocks` | Spielbausteine |
| `audio_level_fields_round_trip`, `audio_collider_impact_sounds`, `audio_game_world_cues` | Sound-Felder, Aufprall-Sounds, Cues |
| `physics_materials_of_meshes_and_the_ground` | Physik-Material von Meshes und Boden |

---

© 2026 Christian Heinrich Hohlfeld · Alle Rechte vorbehalten · [Impressum und Rechte](00-impressum.md)
