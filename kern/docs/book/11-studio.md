# 11 Kern Studio

Kern Studio is the editor of the Kern engine. It works like the Unreal Engine (Kern is not a product of Epic Games): a level consists of
actors, each actor carries components, and you edit it in viewports with Unreal navigation and
transform gizmos, in the Outliner, Details and World Settings. **Play** and **Simulate** run directly in
the viewport, and a finished level becomes a single HTML file that runs anywhere, with one click.

What makes Studio special lies beneath the surface: the entire editor logic is C++ and runs in the same
WebAssembly module as the renderer, the Box3D physics and the game runtime. What you see in the viewport
is rendered and simulated by exactly the code that later runs the exported game. The browser side only
draws panels and passes input through (section [11.19](#1119-architecture)).

This chapter is the user manual. How to turn it into a game of your own is shown in
[Chapter 12](12-your-own-games.md); the logic graphs are described in [Chapter 9](09-logic.md), the physics in
[Chapter 6](06-physics.md), sound in [Chapter 7](07-audio.md), the game runtime behind Play in
[Chapter 8](08-game-runtime.md) and the Micro Duck robot in [Chapter 13](13-micro-duck.md).

![Kern Studio with the Orb Run level: menu bar, toolbar, viewport, Outliner and Details](../buch/bilder/studio.png)

In this chapter, paths such as `kern/src/studio/ui.cpp` are relative to the root directory of the
repository, in which the engine lives in the `kern/` folder.

---

## 11.1 Starting Studio

There are three ways to open Studio:

| Way | What you do | What for |
|---|---|---|
| Live | open https://kernengine.de/editor/ | get started immediately, also on phones and tablets |
| Single file | build `tools/build_studio.sh`, then open `generated/studio.html` | offline, without a server; the file contains everything |
| Development | serve the repository statically, open `kern/web/studio/index.html` | working on Studio itself (loads `generated/kern_studio.wasm`) |

The project's home page (https://kernengine.de/) links to Studio, the Lehrer, the
two example games and two levels you can play directly (`kern/web/studio/play.html?level=orbRun` and
`?level=physicsLab`). Its footer names the owner ("© 2026 Christian Heinrich Hohlfeld") and links the licence
(`LICENSING.txt`, Apache 2.0), the third-party components (`LICENSES.txt`), the source code, the Studio
docs, the engine docs, the imprint and the privacy policy.

**What happens at startup:**

1. A splash screen "Starting the engine" appears while the WebAssembly module loads.
2. On the very first start, **Orb Run** is loaded: a procedural city with traffic, a Box3D hover car,
   24 energy orbs and a day that turns into night. The Output Log reports
   "Kern Studio ready - Orb Run loaded".
3. On every later start, Studio opens the level you last worked on, in its saved or opened state. A
   notice "Reopened "…", where you worked last time" offers **Start fresh** to begin anew with Orb Run
   instead.
4. If the last session had unsaved changes, Studio asks
   **Restore "…" from your last session?** with **Restore** and **Dismiss** (section
   [11.15](#1115-saving-loading-autosave-and-settings)).

With **Play** (Alt+P) you can start driving through Orb Run right away.

### Installing as an app

Under `/editor/`, Studio is an installable web app:

- Chrome and Edge (Android, Windows, macOS, Linux) show an **⤓ Install** button in Studio's title bar.
  On iPhone and iPad the same button explains the two steps: **Share**, then **Add to Home Screen**.
- Once installed, Studio starts in full screen; the notch and status bar stay clear.
- A service worker keeps the most recently loaded version for offline use, but always fetches the page
  from the network first.
- Every deploy writes its build ID into the page and into `build.json`. Studio checks at startup,
  whenever you return to the app, when the network comes back, and every 60 s. If a new build is
  available and nothing can be lost (edit mode, nothing unsaved), Studio reloads immediately. Otherwise
  **New version available** appears with **Update** and **Later**; on update, the level moves into the
  new version via the autosave.
- The running version is shown at the bottom right of the status bar, with a note when a newer one is
  available online.

---

## 11.2 The interface at a glance

| Area | Position | Content |
|---|---|---|
| Title bar | at the very top | Kern logo, menu bar (File, Edit, Window, Play, Help), document tab with the level name and a dot for unsaved changes, Install button if applicable, frames per second |
| Toolbar | below it | Save, Undo/Redo, **Add**, tools, coordinate system, grid, camera speed, Play group, layout, **Logic**, Package Game, Panels button |
| Viewports | center | 1, 2 or 4 views, each with its own bar |
| Outliner | top right | all actors as a tree with folders, search, visibility |
| Details / World Settings | bottom right | properties of the selection or of the world, as two tabs |
| Content Drawer | bottom, collapsible | Place Actors, Textures, Sounds, Output Log |
| Status bar | at the very bottom | Content Drawer, Output Log, console, status message, physics statistics, sound, level meter, selection, version |

The divider between the viewport and the side panel can be dragged (at least 240 px, at most 60 % of the
window width), as can the one between the Outliner and Details. Studio remembers both sizes and whether
the Content Drawer was open.

### 11.2.1 Menu bar

A click opens a menu; while one is open, hovering over another menu title switches straight to it.
With the mouse, submenus open on hover **beside** the menu, never on top of it. If there is no room
beside it, or if you tap (touch, pen), the submenu replaces the menu in place, with a back row at the
top. That way no menu ever lies under the finger that opened it. On touch devices, menu rows are 40 px
high. Esc closes any menu.

**File**

| Entry | Shortcut | Effect |
|---|---|---|
| New Level... | Ctrl+N | dialog with six templates (see below) |
| Open Level... | Ctrl+O | open a `.klevel` file |
| Save | Ctrl+S | save (with a file dialog the first time) |
| Save As... | Ctrl+Shift+S | save under a new name |
| Import Texture... | | bring an image (PNG, JPEG, WebP) into the level as a texture |
| Package Game (HTML)... | | export the game as a single HTML file ([11.16](#1116-exporting-a-game)) |
| Example Levels ▸ | | Orb Run, Physics Lab, Shooting Range, Micro Duck, Basic, Empty |

The templates in the **New Level** dialog (double-clicking a card creates the level immediately):

| Template | Description in the dialog |
|---|---|
| Orb Run | procedural night city, Box3D hover car, 24 orbs to collect |
| Physics Lab | every Box3D joint: hinge, slider, wheel, rope bridge, motor, stack |
| Shooting Range | first-person shooter soldier (desktop, gamepad, touch), targets, cover, multiplayer-capable |
| Micro Duck | Pollen Robotics' walking robot on its trained network, walks on its own with ToF and camera |
| Basic | ground, sun and sky, a clean start |
| Empty | nothing at all |

If the current level has unsaved changes, the dialog points out that they remain in the autosave until
you edit the new level.

**Edit**

| Entry | Shortcut | Effect |
|---|---|---|
| Undo *step* | Ctrl+Z | undo the last step (the step's name appears in the entry) |
| Redo *step* | Ctrl+Y | redo |
| History... | | all steps as a list; a click jumps to that state |
| Cut / Copy / Paste | Ctrl+X / C / V | cut, copy, paste |
| Duplicate | Ctrl+D | duplicate the selection |
| Delete | Del | delete the selection |
| Select All | Ctrl+A | select everything |
| Deselect | Esc | clear the selection |
| Group | Ctrl+G | parent the selection to a new actor "Group" |
| Snap to Floor | End | put the selection down on the ground |
| Preferences... | | camera, mouse, grid (table in [11.15](#1115-saving-loading-autosave-and-settings)) |
| Reset Settings to Defaults... | | reset all editor settings |

**Window**

| Entry | Shortcut | Effect |
|---|---|---|
| Viewport Layout ▸ | | 1, 2 or 4 viewports |
| Viewport Type ▸ | Alt+G, Alt+J, Alt+K, Alt+H | Perspective, Top, Bottom, Left, Right, Front, Back (for the active viewport) |
| View Mode ▸ | Alt+4, Alt+3, Alt+2, Alt+5 | Lit, Unlit, Wireframe, Player Collision |
| Show ▸ | | Grid, Icons, Selection Outline, Collision Shapes, Constraints, Contact Points, Bounds, Fog, Sky, Stats, Robot Camera |
| Game View | G | all editor helpers off in the active viewport |
| Focus Selection | F | bring the selection into view |
| Content Drawer | Ctrl+Space | Content Drawer on/off |
| Output Log | | open the Content Drawer with the log |
| Fullscreen | F11 | browser full screen |

**Play**

| Entry | Shortcut | Effect |
|---|---|---|
| Play | Alt+P | play ([11.11](#1111-play-and-simulate)) |
| Simulate | Alt+S | run the physics, camera free |
| Pause / Resume | Pause | pause / resume |
| Advance One Frame | . | one step forward (only while paused) |
| Stop | Esc | back to editing |
| Eject from Player / Possess Player | F8 | leave / take over the player |
| Restart Game | Ctrl+R | restart the game |
| Keep Simulation Changes | K | apply the simulated poses of the selection to the level |
| Number of Players ▸ | | 1 Player to 4 Players (listen server) ([11.17](#1117-multiplayer)) |
| Network Emulation ▸ | | Perfect, LAN (5 ms), Average (50 ms, 1% loss), Bad (150 ms, 5% loss), Terrible (300 ms, 20% loss) |
| Host Online Game... | | offer the game over the network |
| Join Online Game... | | join an offered game |
| Leave Online Game | | leave the online game |

**Help**

| Entry | Shortcut | Effect |
|---|---|---|
| Controls... | F1 | table of controls |
| About Kern Studio | | short description |

Entries that do nothing in the current state (such as **Delete** with nothing selected, or **New Level**
during Play) are greyed out.

### 11.2.2 Toolbar

From left to right:

| Element | Shortcut | Effect |
|---|---|---|
| Floppy disk | Ctrl+S | save |
| Undo / Redo | Ctrl+Z / Ctrl+Y | the tooltip names the step |
| **+ Add ▾** | | menu with all catalog groups ([11.8](#118-content-drawer-and-built-in-content)); locked during Play |
| Select / Move / Rotate / Scale | Q / W / E / R | choose a tool |
| Globe or cube | Ctrl+` | gizmo in world or local axes |
| Grid button + dropdown | [ / ] | grid snap on/off, grid size 1 cm to 100 m |
| Angle button + dropdown | | rotation snap on/off, 1°, 5°, 10°, 15°, 30°, 45°, 90° |
| Scale button + dropdown | | scale snap on/off, 0.0625 to 1 |
| Camera + dropdown | mouse wheel while flying | camera speed 1 to 8 |
| Play | Alt+P | play; during Simulate it takes over the player |
| Simulate | Alt+S | simulate |
| Pause | Pause | pause / resume |
| Frame | . | one step forward (only while paused) |
| Stop | Esc | stop and return to editing |
| Eject | F8 | leave / take over the player |
| Layout | | 1, 2 or 4 viewports |
| **Logic** | | open the level's logic graph ([11.10](#1110-the-logic-editor)) |
| Package | | Package Game |
| Panels | | only on narrow screens: Outliner, Details, World as a sheet ([11.18](#1118-narrow-screens-and-phones)) |

### 11.2.3 Viewports

Each viewport has its own bar at the top:

| Element | Content |
|---|---|
| ≡ (Viewport options) | Game View (G), Field of View (50°, 70°, 90°, 110°), Focus Selection (F), Bookmarks (jump to 1–9, set Ctrl+1–9), Layout |
| View type | Perspective or orthographic: Top, Bottom, Left, Right, Front, Back |
| View Mode | Lit, Unlit, Wireframe, Player Collision |
| Show | Grid, Icons (lights, empties), Selection outline, Collision shapes, Constraints, Contact points, Bounds, Fog, Sky, Stats; the list stays open while you toggle |
| Maximize | switches between four viewports and this one alone |

Clicking an element of the bar makes the viewport the active one. When you switch from perspective to
an orthographic view, Studio switches from Lit to Wireframe, and back to Lit when you return to
perspective. **Constraints** is off by default: otherwise every chain, door and rope would draw its
debug lines.

In the viewport itself Studio shows:

- **Axis indicator** at the bottom left (X red, Y green, Z blue) in the active perspective view. Z points up.
- **Info line**: in perspective, "Speed *n*" and the camera position; in orthographic views, the visible
  width in meters or kilometers. With the **Stats** show flag, additionally frames per second and,
  during simulation, bodies, contacts and step count.
- **Hint line** depending on state, for example while flying
  "Flying · WASD move · Q/E down/up · wheel speed … · Shift faster · Z/C zoom".
- **Gizmo readout** while dragging: translation (Δ in m), angle (°) or scale factors.

![Four viewports: perspective and three orthographic views](../buch/bilder/studio-quad.png)

### 11.2.4 Outliner

The Outliner lists all actors of the level in the columns visibility (eye), **Label** and **Type**.
The type follows from the components: Procedural City, Player, Trigger, Pickup, Audio Source,
Constrained Mesh, Physics Mesh, Static Mesh, Point Light, Physics Body or Empty.

| Action | Effect |
|---|---|
| Click | select |
| Shift+click | add everything between the last selected actor and this one |
| Ctrl+click | add or remove individually |
| Double-click | focus the actor in the viewport; on a folder: rename |
| F2 | rename (directly in the row; Enter confirms, Esc cancels) |
| Eye | show/hide; on folders, all actors inside |
| Arrow | expand and collapse children or folder contents |
| Drag onto an actor | attach as a child (world transform is kept) |
| Drag onto a folder | move into the folder |
| Drag into empty space | detach from the parent actor and take out of the folder |
| Folder button in the panel header | create a new folder "New Folder" for the selection |
| Right-click | context menu (as in the viewport); on folders additionally **Rename Folder** and **Remove Folder (keep actors)** |
| Search field | filters by name, type and tags |

The actor that Play controls carries the badge **PLAYER**; locked actors carry a padlock. The footer
counts actors and the selection. Folders apply only to top-level actors.

### 11.2.5 Details

Details shows the selection: at the top the icon, the name (for a multiple selection, the count) and the
buttons **Logic** (this actor's graph) and **+ Add** (add a component). Below that, a search field and
the sections **Actor**, **Transform** and one section per component. Each section collapses when you
click its header; components have a trash can there for removing them.

With a multiple selection, only components that all selected actors have are shown. Differing values
show "Multiple"; an input sets the value for all of them.

Editing:

| Field type | How to use it |
|---|---|
| Number | type and press Enter; arithmetic expressions such as `2*3` or `10/4+1` are allowed, a comma counts as a decimal point |
| Dragging a number | drag the label (or an unfocused number field) sideways; Shift ×10, Ctrl ×0.1 |
| Number with arrow keys | ↑/↓ in the field; Shift ×10, Ctrl ×0.1 |
| Slider | fields with a range of 0 to 1 additionally have a slider |
| Vector | three fields X, Y, Z; for **Scale**, a lock for proportional scaling |
| Color | color picker or hex value (`#rrggbb`); the picker stays open while you drag in it, and every color takes effect immediately |
| Dropdown | list (shape, type, mode, sound, texture, target actor …) |
| Tags | separated by commas |
| Reset | arrow to the right of the field, visible when the value differs from the default |
| Right-click on a field | **Reset to Default**, **Copy Value**, **Paste Value**; in text fields additionally cut, copy, paste, select all |

**Why a drag is only one undo step:** Studio gives every continuous operation (dragging a label, a
slider, the color picker) a transaction ID. The engine merges all changes to the same property with the
same ID into one step. Typed values and clicks are separate steps.

Every change goes through the actor's JSON form, the same form that is read when a file is loaded. Values
are therefore checked and clamped exactly as if they came from a `.klevel` file.

![Selected actor with the Move gizmo and its components in Details](../buch/bilder/studio-selected.png)

### 11.2.6 Status bar and console

| Element | Effect |
|---|---|
| Content Drawer | drawer on/off (Ctrl+Space) |
| Output Log | drawer with the log |
| **Cmd** | console; ↑/↓ scroll through earlier inputs, Esc leaves the field |
| Status message | the editor's last message |
| Statistics | during Simulate/Play: bodies, awake bodies, joints (of which broken), contacts; online additionally role, players, ping, loss, data rate |
| Speaker | sound on/off |
| Level meter | left/right output level |
| Selection | "*n* of *m* selected" or the number of actors |
| Version | running build |

Console commands (`help` lists them):

| Command | Effect |
|---|---|
| `play`, `simulate`, `stop` | same as the buttons |
| `spawn <item>` | place a catalog item, e.g. `spawn physics_cube` (IDs in [11.8](#118-content-drawer-and-built-in-content)) |
| `select <name>` | select all actors whose name contains the text |
| `tod <0..1>` | set the time of day |
| `gravity <x y z>` | set gravity (without values: 0 0 −9.80665) |
| `camspeed <1-8>` | camera speed |
| `stat` | frames per second and counters to the log |
| `clear` | clear the log |

### 11.2.7 Context menu

A right-click without dragging in a viewport selects the actor under the cursor and opens the context
menu. On touch devices, a long press (at least 0.5 s without movement) opens the same menu. Studio never
lets the browser's context menu through: wherever no menu of its own fits, a Studio menu appears.

| Situation | Entries |
|---|---|
| Editing, selection present | Focus, Rename, Cut, Copy, Paste, Duplicate, Delete, **Edit Logic** or **Add Logic (Blueprint)** (one actor), **Make Player (control it in Play)** (actor with Player Controller), Snap to Floor, Group, **Attach to …** and **Connect with Constraint ▸** (two or more actors), **Detach** (when attached), Hide, Lock/Unlock, Select Children, **Place Actor ▸** |
| Editing, nothing selected | Paste, Select All, Show All, Place Actor ▸ |
| Simulate/Play, selection present | Focus, Keep Simulation Changes |

**Place Actor ▸** puts a Cube, Sphere, Physics Cube, Point Light, Orb or Empty Actor at the spot you
clicked.

---

## 11.3 Navigating the viewport

The controls follow the Unreal Engine. All camera movements apply to the viewport under the pointer;
a click makes it the active one.

### 11.3.1 Mouse and keyboard

| Input | Effect |
|---|---|
| Hold right mouse button + W A S D | fly mode: forward, left, back, right |
| … + Q / E | down / up (also Page Down / Page Up) |
| … + arrow keys | like W A S D |
| … + Shift | 2.5 times faster |
| … + mouse wheel | change camera speed 1–8 |
| … + Z / C | zoom in / zoom out (field of view) |
| … + move the mouse | look around |
| Right + left mouse button | move up/down and sideways |
| Drag with middle mouse button | pan |
| Alt + left mouse button | orbit around the selection (or around the point under the cursor) |
| Alt + right mouse button | dolly: closer to / farther from the pivot |
| Alt + middle mouse button | pan |
| Mouse wheel | forward/back; in orthographic views, zoom toward the cursor |
| Right or middle mouse button in orthographic views | pan |
| F | frame the selection (with nothing selected: the whole level) |
| Double-click in the viewport | focus the selection |
| Ctrl+1 … Ctrl+9 | set camera bookmark |
| 1 … 9 | jump to bookmark |

Whether Alt+left orbits around the selection is controlled by the setting **Orbit around selection**
(on by default). With nothing selected, the camera orbits around the point under the cursor, otherwise
around a point on the ground in front of it.

While you are flying, W, E, R, Q belong to the movement keys; tools do not switch then. Movement reads
the physical key position (WASD stays in the same place on AZERTY and QWERTZ), whereas shortcuts such
as Ctrl+Z read the labeled key.

Camera speed in meters per second:

| Level | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 |
|---|---|---|---|---|---|---|---|---|
| m/s | 1 | 2.5 | 5 | 10 | 20 | 40 | 80 | 160 |

Level 4 is the default.

### 11.3.2 Touch gestures

In the editor and in Simulate, you control the camera as in a 3D viewer on a phone:

| Gesture | Effect |
|---|---|
| Drag one finger | orbit around the point under the finger (with "Orbit around selection", around the selection); in orthographic views, pan |
| Two fingers | zoom toward the point between them (spreading: closer) and pan with them |
| Tap | select |
| Double tap | frame the selection (like F) |
| Long press | context menu |
| Finger on a gizmo handle | move, rotate, scale as with the mouse |
| Finger in another viewport | makes it the active one |

When zooming with two fingers, whatever lies under the fingers stays under them. The scale is the
distance to that point; that is why the gesture feels the same on a sock as on a city block. After a
pinch, the next one-finger drag orbits around the point you zoomed to, as long as nothing else moves the
camera or changes the selection. The page itself never zooms along with the pinch.

---

## 11.4 Keyboard shortcuts

The complete key map, as implemented by `Studio::key` in `kern/src/studio/ui.cpp`. **F1** also shows the
most important shortcuts inside Studio.

### 11.4.1 Editing

| Key | Effect |
|---|---|
| Q / W / E / R | tool Select / Move / Rotate / Scale |
| Space | Move → Rotate → Scale → Move |
| Ctrl+` | world / local axes |
| [ / ] | grid size smaller / larger (0.01; 0.05; 0.1; 0.25; 0.5; 1; 2; 5; 10; 50; 100 m) |
| Del, Backspace | delete the selection |
| F | focus the selection |
| End | put the selection down on the ground |
| H | hide the selection |
| Shift+H | hide everything else |
| Ctrl+H | show everything |
| G | Game View in the active viewport |
| Esc | cancel a gizmo drag, otherwise clear the selection |
| F2 | rename the primary actor |
| F11 | full screen |
| F1 | Controls dialog |
| Ctrl+Z / Ctrl+Shift+Z / Ctrl+Y | Undo / Redo / Redo |
| Ctrl+C / X / V / D | copy / cut / paste / duplicate |
| Ctrl+A | select everything |
| Ctrl+G | group |
| Ctrl+S / Ctrl+Shift+S | Save / Save As |
| Ctrl+O | open a level |
| Ctrl+N | New Level |
| Ctrl+Space | Content Drawer |
| Ctrl+1 … 9 / 1 … 9 | set / jump to camera bookmark |
| Alt+G / J / H / K | Perspective / Top / Front / Side (Right) |
| Alt+2 / 3 / 4 / 5 | Wireframe / Unlit / Lit / Player Collision |
| Alt+P / Alt+S | Play / Simulate |
| Alt + drag gizmo | drag a copy |

Copied actors end up as text (`{"format": "kern-actors", "actors": […]}`) on the system clipboard.
Ctrl+V in another tab or another session pastes them there.

### 11.4.2 Play and Simulate

| Key | Effect |
|---|---|
| Esc | Stop (first cancels a running gizmo drag) |
| F8 | Eject (leave the player) or Possess (take over the player) |
| Shift+F1 | release the mouse without pausing |
| Pause or Alt+Shift+P | Pause / Resume |
| Alt+P | end Play |
| Alt+S | switch to Simulate |
| Ctrl+R | restart the game (not with the soldier: there Ctrl is crouch and R is reload) |
| . | one step forward when paused (Simulate or after Eject) |
| K | Keep Simulation Changes (Simulate) |
| F | focus the selection (Simulate or after Eject) |
| G | Game View (Simulate or after Eject) |

When the browser has captured the mouse (pointer lock), it swallows the first Esc to release it. Studio
treats this loss of pointer lock as Esc and stops immediately, so a single press is enough. Switching
windows only releases the mouse.

The game keys themselves:

| Input | Effect |
|---|---|
| W A S D, arrow keys | move, steer, throttle |
| Mouse (after clicking into the viewport) | look around |
| Space | jump; with the flyer, up (also E) |
| Q, left Shift, left Ctrl | with the flyer, down |
| Shift | boost or sprint |
| Left mouse button | fire (soldier); the click that captures the mouse does not fire |
| Right mouse button | aim (soldier) |
| R | reload |
| C or left Ctrl | crouch |
| F | use (interact) |
| Mouse wheel | zoom the game camera |
| 1–7, V | Micro Duck movements ([11.13](#1113-controlling-the-micro-duck)) |

Gamepads and touch controls are described in [11.12](#1112-mobile-touch-and-gamepad).

### 11.4.3 Logic editor

| Key | Effect |
|---|---|
| F7 | Compile |
| Ctrl+F | search |
| F9 | set/remove breakpoint |
| Q | straighten connections |
| Shift+W / A / S / D | align |
| B / S / D / N / O / G / L + click | insert Branch / Sequence / Delay / Do N / Do Once / Gate / For Loop |
| C | comment around the selection |
| Ctrl+C / X / V / D | copy / cut / paste / duplicate |
| Del | delete |
| F / Home | frame selection / everything |
| Ctrl+Z | undo |
| Esc | back to the viewport |

While the logic editor is open, letters and Del belong to the graph; only Ctrl+Z, Ctrl+Y, Ctrl+S and F1
reach the level.

---

## 11.5 Selection, gizmos and snapping

### Selecting

| Input | Effect |
|---|---|
| Left-click | select; Shift adds, Ctrl toggles |
| Click into empty space | clear the selection (without Shift/Ctrl) |
| Drag with left mouse button | marquee selection; Shift adds, Ctrl toggles |
| Outliner | see [11.2.4](#1124-outliner) |

Left-dragging never moves the camera and never moves an actor, not even when you start on an actor; it
always draws a selection rectangle (the actor you started on is then included). As in Unreal, actors are
moved only by the gizmo. Marquee selection takes every visible, unlocked actor whose origin lies inside
the rectangle; the procedural city is left out.

Lights, empties and other actors without a mesh are selected via their icon in the viewport
(Show › Icons). Locked actors (Lock) cannot be selected in the viewport: the click passes through them to
whatever lies behind.

The **primary** selection is the most recently selected actor. The gizmo sits on it, and Details shows
its name.

### Gizmos

| Tool | Handles |
|---|---|
| Move (W) | three axis arrows, three plane squares for two axes each, center point for the screen plane |
| Rotate (E) | three rings |
| Scale (R) | three axes and a center handle for uniform scaling |
| Select (Q) | no gizmo |

Axes are red (X), green (Y) and blue (Z); the handle under the pointer lights up yellow. With the globe
or cube button (Ctrl+`), the gizmo aligns with the world or with the primary actor. All selected actors
whose ancestors are not themselves selected are moved; children follow their parents.

**Alt + drag gizmo** drags a copy and leaves the original in place. Copy and movement are a single undo
step. **Esc** while dragging cancels and restores the initial state.

### Snapping

| Snap | Default | Effect |
|---|---|---|
| Grid | on, 0.5 m | Move snaps to multiples of the grid size (in world axes to absolute positions, in local axes to increments) |
| Rotation | on, 15° | Rotate turns in increments |
| Scale | off, 0.25 | Scale changes factors in increments |

Grid snap also applies when placing new actors: their X and Y position is put on the grid. Choosing a
value in the toolbar dropdown switches the respective snap on as well. The perspective viewport draws a
ground grid, orthographic views their own.

---

## 11.6 Levels, actors and components

A level consists of **World Settings**, **actors**, textures, sounds, the level's logic graph and the
editor camera. Units are meters, seconds and kilograms; Z points up. Rotations are stored as Euler angles
in degrees (x roll, y pitch, z yaw), colors as sRGB.

An actor has a name, a parent actor, an Outliner folder, tags, Hidden, Locked, a transform and
components.

### 11.6.1 Creating actors

1. Open the Content Drawer (Ctrl+Space) and click an item: it appears where the active camera is looking
   (on the object hit, otherwise on the ground in front of the camera, otherwise 10 m ahead).
2. Or **drag** an item into a viewport: it lands exactly where you release it.
3. Or use **+ Add** in the toolbar, the context menu **Place Actor ▸** or the console (`spawn <id>`).

New actors receive unique names ("Cube", "Cube 2", …) and are selected afterwards. Placing an actor from
the **Players** group makes it the player; the log reports
"… is the player now - press Play (Alt+P) to control it".

### 11.6.2 Hierarchy and folders

- **Attach:** select two or more actors, the future parent last, then context menu **Attach to …**; or
  drag onto an actor in the Outliner. The world transform is preserved.
- **Detach:** **Detach** in the context menu, or drag into empty space in the Outliner.
- **Group:** Ctrl+G places an empty actor "Group" at the center of the selection and attaches the
  selection to it.
- **Folders:** the **Outliner folder** field in Details, the folder button in the Outliner, or dragging
  onto a folder.
- **Select children:** **Select Children** in the context menu.

When simulating, child actors without a rigid body of their own are welded as a compound into the body
of the nearest ancestor. Meshes with collision, pickups and triggers without a body receive an implicit
static body, or a kinematic one when animated by a Rotator or Mover ([Chapter 6](06-physics.md)).

### 11.6.3 Components

**+ Add** in Details offers only what the selection does not have yet; colliders and constraints are
lists and can occur more than once. Procedural City is only available for a single actor without a
parent.

| Component | Main fields |
|---|---|
| **Static Mesh** | Shape (Box, Sphere, Cylinder, Cone, Capsule, Plane, Torus, Ramp, Mesh), Size or Radius/Height/Segments (only what the shape uses); *Physics:* Generate collision, Friction, Restitution, Rolling resistance; *Material:* Base color, Roughness, Metallic, Emissive color and strength, Opacity, Unlit, Cast shadow, Two sided, Texture, Texture tiling |
| **Point Light** | Light color, Intensity, Attenuation radius |
| **Rigid Body (Box3D)** | Mobility (Static, Kinematic, Dynamic), Mass (0 = from density), Linear/Angular damping, Gravity scale, Continuous (bullet), Start awake, Allow sleep; *Constraints:* Lock position, Lock rotation (each X/Y/Z); *Initial state:* Initial velocity, Initial spin; *Sound:* Impact sound, Impact threshold |
| **Collider** (multiple) | Shape (Fit to mesh, Box, Sphere, Capsule, Convex hull), Offset, Rotation, Box size, Radius, Capsule length; *Physical material:* Density, Friction, Restitution, Rolling resistance; *Collision:* Sensor, Channel bits, Collides with bits, Group index; *Sound:* Impact sound (Automatic = the body's) |
| **Constraint** (multiple) | Type (Hinge, Slider, Ball, Fixed, Distance/rope, Wheel, Motor, Parallel, No collision), Connected actor (World = fixed point), Anchor and Axis (local), Collide connected; *Limits*, *Motor* (Speed, Max force/torque), *Spring* (Frequency, Damping ratio), Rest length; *Breaking:* Break force, Break torque, Break sound. An info line names the units (degrees or meters) |
| **Player Controller** | Movement (Hover car, Rolling ball, Character, Flyer, Vehicle, Soldier, Walker), Max speed, Acceleration, Turn rate, Jump speed, Boost, Boost speed, Hover height; *Camera:* Chase, First person, Top down with Distance, Height, Field of view; *Soldier:* Crouch speed, Eye height, Look sensitivity, Head bob; *Walker:* Autonomous, Walking network, Actions to servo goals, Kern extras; *Sound:* Engine loop, Jump, Boost |
| **Weapon** | Type (Rifle, Pistol, Shotgun, Sniper rifle), Damage, Headshot multiplier, Fire rate, Automatic, Pellets, Range, Impulse; *Ammo:* Magazine, Reserve, Reload time; *Handling:* Hip/Aimed spread, Recoil, Aim field of view; *Sound:* Fire, Reload |
| **Health** | Health, Armor, Regenerate after, Regeneration, Respawn, Respawn delay, Points for a kill; *Sound:* Hit, Down; *Explosion:* radius, damage |
| **Pickup** | Points, Burst color, Respawn after, Sound, Heals, Ammo |
| **Trigger** | Action (Goal, Hazard, Checkpoint, Message, Jump pad, Teleport, Damage zone, Logic event only), Message, Only once, Strength, Teleport target, Sound (Automatic = matching the action) |
| **Rotator** | Rotation rate (°/s per axis) |
| **Mover** | Offset, Period, Phase, Motion (Sine, Ping-pong, Loop) |
| **Procedural City** | Seed, Blocks per side, Block pitch, Street width, Tallest building, Trees, Traffic cars, Lit windows at night |
| **Audio Source** | Sound, Volume, Pitch, Loop, Play on start, Start delay, Bus; *Attenuation:* Spatial (3D), Inner radius, Falloff distance, Roll-off, Doppler, Pitch by speed |

What the fields mean physically and which values work well is covered in [Chapter 6](06-physics.md)
(body, collider, constraint), [Chapter 7](07-audio.md) (Audio Source, sounds) and
[Chapter 8](08-game-runtime.md) (controller, pickup, trigger, weapon, health).

### 11.6.4 Joints with "Connect"

Faster than adding a constraint by hand:

1. Select the actor that should hang on the joint (body B).
2. With Shift or Ctrl, add the actor it hangs from (body A), **last**.
3. Right-click › **Connect with Constraint ▸** › Hinge, Slider, Ball, Fixed (weld), Distance / rope, Wheel
   or Motor.

Studio places the anchor halfway between the two actors (except for Distance and Wheel) and gives the
attached actor a rigid body if it does not have one. Several attached actors each receive a joint to the
one selected last.

### 11.6.5 The player

Play controls an actor with a **Player Controller**. It can be set in three ways: place an actor from
**Players**, right-click › **Make Player (control it in Play)**, or World Settings › **Player**. If the
level has no actor with a controller, Play starts a simulation instead and reports
"Nothing to control yet".

### 11.6.6 Errors and warnings

Before Play and Simulate, Studio checks the level (`game::validate`). **Errors** such as dangling
references or invalid values prevent the start: "Fix the errors above to play". **Warnings** appear only
in the Output Log.

---

## 11.7 Undo and History

Each step stores the complete document after the change. This makes undo and redo exact: there are no
inverse operations that could drift apart, and the result depends only on the sequence of changes,
never on timing. Studio keeps up to 500 steps.

- **Edit › History...** shows all steps; a click jumps to that state.
- Consecutive changes to the same property within one drag are a single step
  ([11.2.5](#1125-details)).
- A change that changes nothing does not create a step.
- During Play and Simulate the level is locked: "Stop playing to edit the level".

---

## 11.8 Content Drawer and built-in content

The Content Drawer (Ctrl+Space, **Content Drawer** in the status bar) has four tabs and a search field.
It opens below the viewports.

### 11.8.1 Place Actors

A click places an item; dragging into a viewport places it at the drop point. Each tile's tooltip
describes the item. In the console and in commands, items are referred to by their ID.

| Group | Item | ID | Description |
|---|---|---|---|
| Players | Soldier | `soldier` | first-person shooter: rifle, health, respawn; mouse, gamepad, touch; online-capable |
| Players | Character | `character` | third-person runner: walk, sprint, jump, climb steps |
| Players | Hover Car | `hover_car` | fast Box3D hover car with boost |
| Players | Rolling Ball | `rolling_ball` | physics ball for rolling and jumping |
| Players | Drone | `drone` | flyer: Space/E up, Q/Ctrl down |
| Players | Micro Duck | `micro_duck` | Pollen Robotics' biped on its trained networks |
| Players | Micro Duck (Rollers) | `micro_duck_rollers` | the Micro Duck on roller skates: `roller.onnx` drives, 2 makes it crouch |
| Gameplay | Orb | `orb` | glowing pickup, 1 point |
| Gameplay | Star | `star` | spinning pickup, 10 points |
| Gameplay | Health Pack | `health_pack` | heals 50; stays put at full health; comes back after 15 s |
| Gameplay | Ammo Box | `ammo_box` | 60 rounds for soldiers; comes back after 20 s |
| Gameplay | Goal | `goal` | trigger that wins the game |
| Gameplay | Hazard | `hazard` | trigger that loses the game |
| Gameplay | Damage Zone | `damage_zone` | hurts whoever walks in (25 per entry) |
| Gameplay | Checkpoint | `checkpoint` | respawn point |
| Gameplay | Jump Pad | `jump_pad` | launches players up and forward |
| Gameplay | Teleporter | `teleporter` | two linked pads: step on A, arrive at B |
| Gameplay | Message Trigger | `message` | shows a text on contact |
| Gameplay | Moving Platform | `moving_platform` | kinematic platform with a Mover |
| Gameplay | Spinner | `spinner` | rotating kinematic bar |
| Gameplay | Target Dummy | `target_dummy` | shootable target with health and a head; comes back |
| Gameplay | Explosive Barrel | `explosive_barrel` | explodes when shot: pushes and hurts whatever is close |
| Shapes | Cube | `cube` | 1 m cube with collision |
| Shapes | Sphere | `sphere` | 1 m sphere |
| Shapes | Cylinder | `cylinder` | upright cylinder |
| Shapes | Cone | `cone` | upright cone |
| Shapes | Capsule | `capsule` | capsule along Z |
| Shapes | Plane | `plane` | 10 × 10 m floor |
| Shapes | Torus | `torus` | ring |
| Shapes | Ramp | `ramp` | wedge to drive or roll up |
| Shapes | Empty Actor | `empty` | transform only: parent or pivot |
| Physics | Physics Cube | `physics_cube` | dynamic Box3D crate |
| Physics | Physics Sphere | `physics_sphere` | dynamic sphere |
| Physics | Physics Barrel | `physics_barrel` | dynamic cylinder (convex hull) |
| Physics | Crate Stack | `crate_stack` | pyramid of 10 crates |
| Physics | Pendulum | `pendulum` | ball on a distance joint |
| Physics | Hinged Door | `hinged_door` | door on a sprung hinge with limits |
| Physics | Chain | `chain` | 8 links on ball joints |
| Physics | See-Saw | `see_saw` | plank on a hinge |
| Physics | Sock | `micro_duck_sock` | crumpled sock at Micro Duck scale (soft lumps on springs); the duck picks it up with 2 or on its own |
| Lights | Point Light | `point_light` | colored light with a radius |
| Audio | Audio Source | `audio_source` | 3D sound source (machine hum) |
| Audio | Ambient Wind | `ambient_wind` | looping wind, audible everywhere |
| Audio | City Ambience | `city_ambience` | looping city noise with falloff |
| Audio | Radio | `radio` | plays the calm music loop in 3D |
| World | Procedural City | `city` | blocks, streets, trees and traffic |

When a Micro Duck is inserted, Studio sets the physics to 100 Hz with 16 sub-steps (World Settings ›
Step rate and Sub-steps) and reports this in the log: servo-driven robots are long joint chains that
would stretch without enough sub-steps, and their network needs its fixed rate.

### 11.8.2 Textures

This tab shows all textures of the level with a preview and their size.

- **Import image** (or File › Import Texture..., or dragging an image onto the page) brings PNG, JPEG or
  WebP into the level. Studio scales down images with an edge length above 2048 px. If something is
  selected during import, every selected mesh receives the texture and its color becomes white.
- **Double-clicking** a texture applies it to the selection.
- The trash can removes it; meshes that used it lose it.

Textures are stored in the level as data URLs; the `.klevel` file is therefore self-contained.

### 11.8.3 Sounds

This tab lists the imported sounds and the built-in ones, by category.

- **Clicking** plays a sound preview (the "Playing preview" button at the top stops it).
- **Double-clicking** sets it as the sound of an Audio Source on the selection (the component is added
  if it is missing).
- **Dragging into a viewport** places an Audio Source with this sound.
- **Import audio** (or dragging files onto the page) brings in WAV, MP3, OGG, M4A, FLAC and anything else
  the browser can decode, up to 24 MB per file. Undo reverts an import.

Every sound field in Details has a preview button. Sound only starts after the first click or key press
(a browser rule); without Web Audio, Studio works silently. The sound engine is described in
[Chapter 7](07-audio.md).

### 11.8.4 Output Log

All messages with timestamps, warnings and errors in color. The log shows the last 200 entries. During
Play and Simulate, game events also appear here; `clear` in the console empties it.

---

## 11.9 World Settings

The **World Settings** tab next to Details (on narrow screens the **World** tab) applies to the whole
level.

| Field | Meaning |
|---|---|
| Level name | name of the level (also the file name when saving) |
| *Game* | |
| Game title | title in the HUD and in the exported game |
| Player (controlled in Play) | which actor is controlled |
| Observer camera | Play starts with a free camera: fly, watch, drag bodies ([11.11.4](#11114-observer-camera)) |
| Win condition | None (sandbox), Collect all pickups, Reach a goal trigger, Survive the time limit, Reach a target score |
| Time limit (0 = none) | seconds |
| Target score | points for "Reach a target score" |
| Kill Z (respawn below) | below this height the player is reset |
| *Sky and light* | |
| Time of day | 0 to just under 1 |
| Day length (0 = frozen) | seconds for a full day |
| Sun azimuth | degrees |
| Fog density | fog density |
| Exposure | exposure |
| Ambient occlusion radius (0 = off) | meters |
| Shadow distance (smaller: sharper) | meters of the view covered by the sun's shadow map (2–2000, default 80; the Micro Duck level 10) |
| Clouds | clouds on/off |
| Endless ground | infinite ground plane |
| Ground color | ground color |
| *Physics (Box3D)* | |
| Gravity | m/s² as a vector |
| Sub-steps | 1 to 16 |
| Ground friction | friction of the ground |
| Ground bounciness | restitution of the ground |
| Contact stiffness | Hz |
| Contact damping ratio | damping of the contacts |
| Contact push-out speed | m/s |
| Step rate | 15 to 480 Hz |
| *Audio* | |
| Master volume | overall volume |
| Music, Music volume | music track and volume |
| Ambience, Ambience volume | background sound and volume |
| Reverb | reverb amount |
| Doppler | strength of the Doppler effect |

The physics values are explained in [Chapter 6](06-physics.md), sky and light in [Chapter 4](04-graphics.md).

---

## 11.10 The logic editor

Every actor and the level itself can carry a logic graph, modeled on the Unreal Engine's Blueprints. It
is opened via **Logic** in the toolbar (the level's graph), via **Logic** in Details, or via
**Edit Logic** / **Add Logic (Blueprint)** in the context menu (the actor's graph). The editor then
covers the viewports.

| Area | Content |
|---|---|
| Toolbar | **Viewport** (back, Esc), **Compile** (F7, with a status of OK / warning / error), **Save**, **Find** (Ctrl+F), Blueprint selector (level or actor), **Templates**, **C++** (export), marquee selection (touch), help |
| My Blueprint | graphs (Event Graph, state machines), variables with type color and live value while playing, events |
| Graph | tabs, zoom levels as in Unreal, nodes and wires |
| Details | selected node, variable, comment, state or transition |
| Compiler Results / Find Results | problems and search hits; a click jumps to the node |

An empty graph offers start buttons: **Begin Play**, **On Interact**, **On Begin Overlap**,
**Event Tick** and **Templates…**. The templates insert ready-made graphs:

| Template | What it does |
|---|---|
| Door that opens on Interact | Interact toggles between opening and closing, with sound |
| Score zone (overlap +10) | entering gives 10 points, shows "+10" and plays a sound |
| Spawner (a cube every 3 s) | a Physics Cube every 3 s, at most 10 |
| Traffic light (state machine) | traffic light as a state machine (green, yellow, red) |
| Robot: walk and turn away with its ToF | the Micro Duck walks and turns away when its ToF sees something closer than 1.6 m |

An example, as the browser test performs it: Basic level, **Logic**, **Begin Play**, drag a wire from
the "then" output into empty space, type "show message", Enter, enter a text, **Viewport**, place a
Rolling Ball, **Play**: the HUD shows the text.

When Play or Simulate starts, Kern compiles every graph to WebAssembly machine code; graphs with errors
prevent Play from starting. **C++** downloads the graph as readable C++ source code that runs as native
code when built into a game ([Chapter 12](12-your-own-games.md#1243-logic-graphs-as-c)). Breakpoints set
with F9 pause the game as soon as the node runs. All nodes, variables, state machines, the controls in
detail and the three execution tiers are described in [Chapter 9](09-logic.md).

---

## 11.11 Play and Simulate

Studio has three states: **Editing**, **Simulating** and **Playing**. In both running states the level
is locked; changes made in the simulation disappear on Stop, unless you apply them with
**Keep Simulation Changes**.

### 11.11.1 Play

**Play** (Alt+P, Play button) starts the game in the active viewport: the game runtime builds the Box3D
world, the player receives control, the camera follows it, and the HUD shows the title, collected
pickups, points, remaining or elapsed time, best time, speed in km/h with a boost bar (for the soldier:
crosshair, health, ammunition). The viewport's bar and info line disappear.

1. On the desktop, Studio captures the mouse (pointer lock). When it is free, the viewport shows
   "Click the viewport to control the game · Esc stops · F8 ejects"; a click captures it again.
2. On victory or defeat, a "Victory" or "Game over" banner appears with **Play again**.
3. **Ctrl+R** or Play › Restart Game restarts.
4. **Esc** or **Stop** ends the game.

![Play in the viewport: the hover car in Orb Run with HUD](../buch/bilder/studio-play.png)

### 11.11.2 Eject and Possess

**F8** leaves the player: the game keeps running, the camera is free, and you can fly, select and grab
bodies as when editing. Studio switches to the Simulating state. Pressing **F8** again (or the Play
button) takes over the player again.

### 11.11.3 Simulate

**Simulate** (Alt+S) runs the physics and the game rules without controlling a player. The camera
remains the editor's.

- **Grabbing bodies:** the left mouse button on a dynamic body (or a finger) grabs it with the mouse
  joint. Dragging moves it, dragging the pointer upward lifts it, the mouse wheel or a second finger
  (spread/pinch) pulls it closer or pushes it away, and releasing throws it with its momentum. Ctrl is
  not needed for this. A short click selects the body. The hint in the viewport reads "Simulating ·
  drag grabs bodies · K keeps poses · Esc stops"; the Controls dialog (F1) lists "Drag (Simulate) –
  Grab and throw physics bodies".
- **A Micro Duck held by its trunk** is additionally held in its orientation, the way a hand holds
  the robot, instead of swinging on a point (`begin_grab` with `hold_rotation`). Lifted like this, it
  pauses as the real robot does ([Chapter 13](13-micro-duck.md)).
- **Why the body stays physical:** a motor joint with a spring (7.5 Hz, critically damped) pulls the
  grabbed point toward the pointer. The body bumps into things, tips over and swings like any other.
- **K** applies the simulated poses of the selected actors to the level (one undo step) and ends the
  simulation.
- **Pause**, then **.** steps through one frame at a time.

The show flag **Collision Shapes** displays the Box3D shapes, **Constraints** the joints,
**Contact Points** the contacts. The status bar counts bodies, awake bodies, joints, broken joints and
contacts.

![Simulate with collision shapes displayed](../buch/bilder/studio-simulate.png)

![Physics Lab: all joint types in simulation](../buch/bilder/studio-physics-lab.png)

### 11.11.4 Observer camera

With World Settings › **Observer camera**, Play does not start behind the player but with a free camera:
the game runs, and you watch and intervene. The Micro Duck level uses this.

| Device | Controls |
|---|---|
| Desktop | W A S D to fly, Q / E down / up, Shift faster, without holding a mouse button; dragging into empty space looks around; right mouse button and mouse wheel as in the editor |
| Phone | one finger in empty space looks around; two fingers zoom and pan |
| Both | drag, lift and throw a body as in Simulate |
| F8 | take over the player (for example the duck via gamepad); F8 again returns to watching |

---

## 11.12 Mobile, touch and gamepad

### 11.12.1 Which device is in control

The engine detects touch, mouse with keyboard, and gamepad by itself and switches to the most recently
used one (`InputArbiter` in `kern/src/game/touch.cpp`, following SpawnCity's rules):

- A finger switches to touch.
- A mouse switches to desktop only on devices with a fine pointer; a mouse on a tablet keeps the touch
  layout.
- Every real key press switches to desktop, even on a phone.
- A gamepad counts as soon as a button is pressed or a stick is deflected beyond 0.22.
- Pointer lock exists only with a mouse.

### 11.12.2 Play on touch devices

Play on a touch device fills the whole screen and switches to a single viewport. At the top left are
**⟵ EXIT** (Stop) and **‖ PAUSE** or **▶ RESUME**.

The engine creates the controls itself (`TouchControls`); the page merely draws them:

- **Two sticks** of 168 px in the bottom corners, flush with the safe-area edges; smaller (at least
  72 px) only if two do not fit. Left to move (label **MOVE**, for vehicles **STEER / GAS**, for the
  flyer **FLY**), right to look (**LOOK**) with edge turning: if you drag the right stick beyond its
  ring, the camera keeps turning.
- **Sprint:** push the left stick all the way forward (character and soldier); the ring lights up.
- **Dragging on the screen:** looks around. For the soldier with a weapon, a finger on the screen fires
  and keeps aiming as long as it stays down.
- **Buttons** to the left of and above the right stick, depending on the controller:

| Controller | Buttons |
|---|---|
| Soldier | JUMP, RELOAD (with weapon), AIM (with weapon, toggle), CROUCH (toggle), USE |
| Character | JUMP (if Jump speed > 0), USE |
| Rolling ball | JUMP (if Jump speed > 0), BOOST (if boost is on) |
| Flyer | UP, DOWN, BOOST (if boost is on) |
| Hover car, Vehicle | BOOST (if boost is on), HOP (if Jump speed > 0), USE |
| Walker (Micro Duck) | QUACK, PICK, SIT, KICK L, KICK R, ROLL, BEAK, WHEEE, HOP |

When the last finger lifts off the screen or the page goes into the background, the engine releases
everything; nothing stays stuck down.

![Play on a phone in portrait: EXIT, PAUSE and the touch sticks](../buch/bilder/phone-play-390.png)

### 11.12.3 Gamepad

Gamepads with the standard layout (the browser's standard mapping) work in editor Play and in the exported
game, with dead zones and curves as in SpawnCity:

| Input | Effect |
|---|---|
| Left stick | move |
| Right stick | look |
| RT | fire |
| LT | aim |
| A | jump |
| B | crouch |
| X | reload |
| Y | use |
| L3 or LB | sprint/boost |
| RB / LB | with the flyer, up / down |
| Start | pause |

---

## 11.13 Controlling the Micro Duck

The Micro Duck is Pollen Robotics' two-legged robot, running on its real, trained networks. How the
model, servos and networks work is described in [Chapter 13](13-micro-duck.md); this section is about
controlling it in Studio.

![The Micro Duck example level: a room at the duck's scale (duck shown in this book's own design)](../buch/bilder/duck-level.png)

**The example level** (File › Example Levels › Micro Duck): a room at its scale with furniture, a ball
and two socks. The duck walks autonomously. Even without extras it plays ball as in Pollen's original
runtime (BallPlay: walk up, line up, kick with the left foot); with **Kern extras** switched on (on in
the example level), Kern's play layer plays with the ball and socks instead. The level Blueprint makes it quack as soon as its head camera sees the ball. Play starts with
the observer camera: you watch and can lift the ball with the mouse or a finger; F8 takes over the duck.

**Without input**, the duck walks on its own (Details › Player Controller › Walker › **Autonomous**): it
explores the room with its head ToF and camera, avoids obstacles and gets back up by itself after a fall.
Lifted by its trunk, it pauses (Robot State "Held") and walks on once set down.

**Keyboard** (in Play, with the player taken over):

| Key | Movement |
|---|---|
| W / S | walk forward / backward |
| A / D | turn |
| Space | quack |
| 1 | sit down / stand up |
| 2 | pick up (Pick) |
| 3 | kick with the left foot |
| 4 | kick with the right foot |
| 5 | roll (held: keeps rolling) |
| 6 | open / close beak |
| **7** | **jump** – Kern's own movement from `jump.onnx` |
| V | "Wheee" ride, as long as held |

**Touch:** the buttons from [11.12.2](#11122-play-on-touch-devices). **HOP** is the jump (key 7),
**QUACK** quacks, **WHEEE** starts the ride, and the others correspond to keys 1–6.

**Gamepad:** RT quacks, LT starts the Wheee ride (both open the beak while doing so), B sits down or
stands up, Y picks up, X rolls.

Logic controls the duck with the nodes of the **Robots** category (Walk, Look, Robot ToF, Robot Camera
Sees, Robot Move with the movement "jump", and others, [Chapter 9](09-logic.md)).

**Head camera and ToF:** during the game, Studio shows what the duck sees at the top right of the active
viewport (Window › Show › **Robot Camera**, on by default):

- **HEAD CAMERA** – the image from its head camera, plus its state or "Fallen".
- **TOF 8×8** – the 8×8 grid of its distance sensor: red for an obstacle (brighter the closer), green
  for floor, yellow for too close, dark for nothing; plus the smallest distance in real meters or
  "clear".

![The Micro Duck in Play with head camera and ToF grid (duck shown in this book's own design)](../buch/bilder/duck-play.png)

---

## 11.14 Loading policies (jump.onnx, play.onnx)

The duck's movements are neural networks in ONNX format. The walking network `velstand.onnx` is built
into the module; the page loads all others on demand as soon as a level contains a duck:

| Network | Origin | Folder on the server | What for |
|---|---|---|---|
| Pollen's set (`roller.onnx`, `roulade.onnx`, `ball_kick_left.onnx`, `ball_kick_right.onnx`, `alpha_*.onnx`, `roller_crouch.onnx` …, per manifest) | Pollen Robotics, `kern/third_party/microduck/` | `microduck/` | sitting, picking up, kicking, rolling, roller skates, older gait |
| `jump.onnx` | Kern Lehrer, `kern/policies/` | `policies/` | the jump (key 7, HOP, Robot Move "jump") |
| `play.onnx` | Kern Lehrer, `kern/policies/` | `policies/` | the play layer: kick the ball, pick up the sock, carry it away, spit it out (only with **Kern extras**) |

This is how loading works:

1. Via `kern_studio_pending_policies()`, the engine reports which files the level needs that are still
   missing.
2. The page fetches each file: Kern's own networks from `policies/`, Pollen's from `microduck/`, each next
   to the page. The development page reads the copies in the repository (`kern/third_party/microduck/`,
   `kern/policies/`), as does the single-file version in `generated/`.
3. `kern_studio_load_policy(file, bytes)` hands over the bytes; the engine checks whether it is a network
   it can run and registers it under its file name.
4. If a file fails, the page retries after 5 s, 10 s, 20 s …, at most once a minute. Until then the
   movement is unavailable; the duck stands still and says why.

**Self-trained networks:** what the Kern Lehrer trains in the browser (page `/lehrer/`,
[Chapter 14](14-lehrer.md)) it stores in the same browser (IndexedDB `kern-lehrer`, store `policies`,
key = file name such as `jump.onnx`). Studio loads these networks at startup and again as soon as the
Lehrer announces a newer one (BroadcastChannel `kern-lehrer`). A network trained on the same device thus
replaces the bundled one without copying any file.

On publishing, the CI copies the networks to `editor/microduck/` and `editor/policies/`. If you host an
exported game with a duck yourself, put these two folders next to the HTML file
([11.16](#1116-exporting-a-game)). The licence of `microduck/` (`kern/third_party/microduck/LICENSE`)
and the `NOTICE` go with it; a game with the duck's look is non-commercial
([Chapter 12](12-your-own-games.md#126-publishing)).

---

## 11.15 Saving, loading, autosave and settings

### 11.15.1 The `.klevel` format

A level is a JSON file with `"format": "kern-level"` and `"version": 1`. It contains `name`, `world`, the
editor camera (`camera`), `nextId`, `actors`, `textures`, `sounds` and, if present, `logic`. Textures and
imported sounds are stored in it as data URLs; a file is therefore complete. Studio rejects a file from
a newer Studio version with "level was written by a newer Kern Studio". The format is described in
detail in [Chapter 8](08-game-runtime.md).

### 11.15.2 Saving and opening

| Action | What happens |
|---|---|
| Save (Ctrl+S) | with File System Access (Chrome, Edge), a save dialog the first time, then directly into the same file; otherwise a download `<level name>.klevel` |
| Save As (Ctrl+Shift+S) | always with a dialog or download |
| Open (Ctrl+O) | file dialog for `.klevel` and `.json` |
| Dragging a file onto the page | `.klevel`/`.json` opens, images become textures, audio files become sounds |

The file name is the level name, with everything except letters, digits, `-` and `_` replaced by `_`.

**Android** has no `showOpenFilePicker`; Studio opens the regular file dialog there. That dialog does not
know the `.klevel` extension and, with a filter, would grey out all saved levels. On Android Studio
therefore does not filter; if the chosen file is not a level, "… is not a Kern level" appears and the
editor keeps running.

### 11.15.3 Autosave and restore

Studio protects unsaved work without asking on every reload:

- The engine writes a copy no later than 2 s after each change, but only while the document has unsaved
  changes. The page keeps at most one copy (`localStorage`, `kern-studio.autosave.v1`).
- After saving, or when you undo back to the saved state, the page deletes the copy again.
- When leaving the page, when switching to the background and before an update, it writes any pending
  copy immediately. If the document has unsaved changes, the browser asks for confirmation on leaving.
- Nothing is written during Play and Simulate; a pending copy follows after Stop.
- On the next start, Studio offers **Restore** or **Dismiss**. Restore loads the copy, but the level
  remains unsaved (dot on the tab), because its content is not in any file. Until it is saved, it is
  offered again on every start. Dismiss deletes the copy.
- If you open another level, the copy of the previous one is kept until the new one gets changes of its
  own.

Separately, Studio remembers the most recently edited level in its clean state (`kern-studio.last.v1`)
and reopens it on the next start.

### 11.15.4 Settings

Studio stores in the browser (`kern-studio.settings.v1`) **only the settings you changed yourself**:
Preferences, viewport layout, snapping, world/local axes, sound on/off and, per viewport, view type, view
mode, field of view and show flags. Anything you never touched follows the defaults of the respective
version, even if those change. The page stores panel sizes and the Content Drawer separately
(`kern-studio.layout.v1`). Multiplayer Play switching to four viewports on its own does not count as your
own choice.

**Edit › Preferences...**:

| Field | Range | Default |
|---|---|---|
| Camera speed (1-8) | 1–8 | 4 |
| Mouse sensitivity | 0.1–5 | 1 |
| Invert mouse Y | on/off | off |
| Orbit around selection (Alt+LMB) | on/off | on |
| Grid snapping, Grid size | 0.001–1000 m | on, 0.5 m |
| Rotation snapping, Rotation step | 0.1–180° | on, 15° |
| Scale snapping, Scale step | 0.001–100 | off, 0.25 |

**Edit › Reset Settings to Defaults...** asks for confirmation in a Studio dialog and resets all settings,
panel sizes and the Content Drawer. Levels remain untouched.

---

## 11.16 Exporting a game

**File › Package Game (HTML)...** or the package button in the toolbar opens the **Package Game** dialog.

1. Enter the **Game title** (prefilled with World Settings › Game title or the level name).
2. Click **Package**.
3. Studio downloads `<title>.html` and reports "Packaged … (*n* MB)" with **Play it**, which starts the
   file in a new tab.

The file is a complete game: it contains the engine module as Base64, `kern.mjs` and `game.mjs` as one
inline module, and the level as JSON. It needs no server and no network and also runs from `file://`. At
startup it loads the module in player mode (`kern_studio_init(…, player_only = 1)`): no editor, just the
level, straight into Play.

![The exported Orb Run as a standalone HTML file](../buch/bilder/game.png)

**Controls in the exported game:**

| Input | Effect |
|---|---|
| W A S D / arrows, Space, Shift, mouse | as in Play ([11.4.2](#1142-play-and-simulate)) |
| Click | captures the mouse for mouse look |
| Esc | releases the mouse and pauses ("Paused – Click to continue") |
| P | pause |
| R or Enter after victory/defeat, **Play again** | restart |
| Ctrl+R | restart |
| F8 | with the observer camera: take over the player and back |
| Gamepad | as in [11.12.3](#11123-gamepad) |
| Touch | sticks and buttons as in Play, plus a ↻ at the top left to restart |

At the bottom there is a hint with the key bindings, hidden on touch devices.

![The exported game on a phone with touch sticks](../buch/bilder/game-phone.png)

**Playing online with the exported file:** the file understands three parameters in the address:
`?host=<room>` offers a game, `?join=<room>` joins, `&relay=ws://…` uses a relay for other computers
([11.17](#1117-multiplayer)). If establishing the connection fails, you keep playing alone.

**What is not in the file:** the Micro Duck's on-demand networks ([11.14](#1114-loading-policies-jumponnx-playonnx)).
Walking works without them because `velstand.onnx` is built in. For sitting, kicking, jumping and the play
layer, the folders `microduck/` and `policies/` belong next to the HTML file, together with the
licence of `microduck/` (`kern/third_party/microduck/LICENSE`) and the `NOTICE`. A game with the duck's
look is non-commercial ([Chapter 12](12-your-own-games.md#126-publishing)).

---

## 11.17 Multiplayer

### 11.17.1 Multiple players in the editor

**Play › Number of Players** (like Unreal's "Number of Players") starts Play with 2, 3 or 4 players: the
level's world is a listen server, and each additional player is a network client with its own world and
its own viewport, connected via a network within the same process.

1. Play › Number of Players › **2 Players (listen server)**.
2. **Play**. Studio switches to four viewports if only one was open: viewport 1 shows the server,
   viewport 2 the first client, and so on. Each shows its own HUD.
3. Input goes to the player of the active viewport; the others stand still. Clicking into another
   viewport switches the player.
4. **Play › Network Emulation** burdens the connection to the local clients with latency, jitter and
   packet loss, from "Perfect" to "Terrible (300 ms, 20% loss)".
5. The status bar shows role, player count, ping, loss and data rate.

![Two players in the editor: listen server and local client in their own viewports](../buch/bilder/studio-multiplayer.png)

### 11.17.2 Online between browsers

| Step | Host | Guest |
|---|---|---|
| 1 | open the same level | open the same level |
| 2 | Play › **Host Online Game...** | Play › **Join Online Game...** |
| 3 | choose a **Room** (default "kern"), leave **Relay** empty or fill it in | the same room, and the same relay if any |
| 4 | **Host** | **Join** |

- **Without a relay**, tabs of the same browser on the same computer connect via a BroadcastChannel; no
  server is needed. If there is no host in the room, Join reports this after 4 s.
- **With a relay**, computers on the network play together: start `node tools/net_relay.mjs [port]`
  (default port 8790, requires the npm package `ws`) and enter its address, for example
  `ws://host:8790`.
- **Leave Online Game** leaves the game. The host notices when a guest leaves.
- A host in a background tab keeps running on a worker clock so that its players do not run into a
  timeout.

The relay is deliberately dumb: it only forwards datagrams. Acknowledgments, retransmissions, channels,
connection setup and timeouts are handled by Kern's own network stack in C++ ([Chapter 10](10-networking.md)).
Logic runs only on the authority; changed properties and spawned actors are replicated, including to
players who join later.

---

## 11.18 Narrow screens and phones

Up to a window width of 900 px, or on touch devices with a height of at most 600 px, Studio switches to a
compact layout:

- **The viewport gets the whole area.** Outliner, Details and World Settings open via the **Panels**
  button as a sheet with large tabs (**Outliner**, **Details**, **World**): in landscape to the right of
  the viewport, in portrait from the bottom (64 % of the height). With nothing selected, it opens on the
  Outliner.
- **Play and Simulate are always one tap away.** Together with the Panels button they sit in a dock at
  the right edge of the toolbar. The rest of the toolbar scrolls sideways beneath it while the dock stays
  put, even with the sheet, the Content Drawer or the logic editor open. During Simulate and Play, the
  dock holds Pause, Stop and Eject, or Play to take over; buttons that currently do nothing are hidden,
  and the rest are touch-sized (at least 38 px).
- Up to a width of 900 px, the toolbar hides the grid, angle and scale dropdowns as well as the axes
  button, and the status bar hides the console and physics statistics.
- The Content Drawer takes up at most just under half the height.
- Menus open submenus in place with a back row ([11.2.1](#1121-menu-bar)).
- Play fills the whole screen with EXIT and PAUSE ([11.12.2](#11122-play-on-touch-devices)).
- The page never scrolls sideways.

![Studio in an 820 px wide window](../buch/bilder/studio-narrow.png)

![Phone in portrait: the dock with Play, Simulate and Panels](../buch/bilder/phone-dock-390.png)

---

## 11.19 Architecture

### 11.19.1 The editor logic is C++

Studio is the class `kern::studio::Studio` (`kern/include/kern/studio/studio.h`). It owns the document,
the history, the selection, four viewports with cameras and navigation, the gizmos, overlays, the
Details schema, the catalog, menus, dialogs, settings, Play/Simulate and network play. The page contains
no editor logic.

| File | Content |
|---|---|
| `kern/src/studio/studio.cpp` | document, history, selection, actor operations, Play, networking |
| `kern/src/studio/viewport.cpp` | cameras, navigation, touch, picking, rendering, game input |
| `kern/src/studio/gizmo.cpp` | Move, Rotate, Scale, snapping, Alt duplicate |
| `kern/src/studio/overlays.cpp` | grid, selection outline, icons, debug lines |
| `kern/src/studio/details.cpp` | property schema, Details, World Settings |
| `kern/src/studio/catalog.cpp` | Place Actors |
| `kern/src/studio/ui.cpp` | keyboard, menus, dialogs, commands, panel JSON |
| `kern/src/studio/logic_edit.cpp` | logic editor |
| `kern/src/studio/settings.cpp` | settings |
| `kern/src/studio/api.cpp` | `kern_studio_*` C ABI |

**Why this design?** First, the editor runs exactly the code that also runs the exported game; there is
no second implementation that could diverge. Second, the whole editor can be tested without a browser:
the unit tests (`kern/tests/run.sh studio`) drive navigation, gizmos, catalog, commands, Outliner, Play,
saving, autosave and settings directly in C++.

### 11.19.2 The C ABI `kern_studio_*`

The page talks to Studio through flat C functions exported by the WebAssembly module. Strings are passed
as a pointer and length into linear memory; JSON responses come back as a pointer to a buffer that stays
valid until the next call (`kern_studio_result_length()` gives the length).

| Function | Purpose |
|---|---|
| `kern_studio_init(w, h, player_only)` | renderer and Studio; `player_only` for exported games |
| `kern_studio_frame(now_ms)` | cameras, simulation, overlays, rendering of all visible viewports |
| `kern_studio_resize(w, h)` | size of the drawing buffer |
| `kern_studio_set_viewport(i, x, y, w, h, css_w, css_h, dpr, visible)` | rectangle of a viewport (origin at bottom left) |
| `kern_studio_pointer(i, type, x, y, button, buttons, mods, wheel)` | pointer: 0 down, 1 move, 2 up, 3 cancel, 4 wheel, 5 leave |
| `kern_studio_touch(type, id, x, y, time)` | finger; 1 if the touch controls took it |
| `kern_studio_key(key, code, down, mods, repeat)` | keyboard; response `{"handled": bool}` |
| `kern_studio_look(dx, dy)` | mouse look under pointer lock |
| `kern_studio_blur()` | focus lost: release all keys |
| `kern_studio_command(json)` | command, response `{"ok": …}` |
| `kern_studio_state()` / `kern_studio_status()` | panel state or per-frame values |
| `kern_studio_revision()` | changes when `state()` must be read again |
| `kern_studio_take_ui()` | requests to the page (pointer lock, clipboard, menus, dialogs, files …) |
| `kern_studio_pending_textures()`, `…_texture_source(id)`, `…_texture_pixels(…)` | the page decodes textures |
| `kern_studio_pending_sounds()`, `…_sound_source(id)`, `…_sound_pcm(…)`, `…_sound_failed(…)` | the page decodes imported sounds |
| `kern_studio_audio_init(rate)`, `…_audio_render(frames)`, `…_audio_prepare(ms)` | start the mixer, pull stereo PCM, prepare built-in sounds |
| `kern_studio_pending_policies()`, `kern_studio_load_policy(file, bytes)` | Micro Duck networks |
| `kern_studio_net_push(from, bytes)`, `…_net_take()`, `…_net_out()`, `…_net_out_to()` | datagrams for the network stack |
| `kern_studio_take_autosave(now)`, `…_take_autosave_discard()`, `kern_studio_dirty()` | autosave |
| `kern_studio_ao_set(…)` | ambient occlusion |
| `kern_studio_shadow_filter(taps)` | sun shadow filter: 1, 4 (phones) or 9 (desktop) PCF taps |
| `kern_studio_context_lost()`, `…_context_restored()` | WebGL context loss |

Everything you do in Studio is a command as a JSON object `{"cmd": …}`, including every menu entry: a
menu entry in the JSON of `state()` carries the command it executes. The main groups:

| Group | Commands |
|---|---|
| Selection | `select`, `selectRange`, `selectAll`, `deselect`, `selectChildren`, `selectFolder` |
| Actors | `spawn`, `delete`, `duplicate`, `copy`, `cut`, `paste`, `pasteText`, `rename`, `attach`, `moveTo`, `setFolder`, `renameFolder`, `group`, `snapToFloor`, `hide`, `show`, `showAll`, `lock`, `focus`, `connect`, `setPlayer` |
| Properties | `set {path, value}`, `setWorld`, `addComponent`, `removeComponent`, `importTexture`, `removeTexture` |
| History | `undo`, `redo`, `historyJump` |
| Play | `play {mode}`, `stop`, `pause`, `stepFrame`, `eject`, `restart`, `keep`, `gameInput`, `gamepad`, `device`, `touchRelease`, `pointerCaptured` |
| Network | `netPlayers`, `netSimulate`, `netAddress`, `netHost`, `netJoin`, `netStop` |
| Views | `tool`, `space`, `snap`, `cameraSpeed`, `layout`, `maximize`, `activeViewport`, `viewType`, `viewMode`, `showFlag`, `gameView`, `viewCamera`, `fov`, `bookmark` |
| Panels | `filter`, `collapse`, `section`, `menu`, `dialog`, `submitDialog`, `catalog`, `clearLog`, `ui` |
| Documents | `new {template}`, `open`, `restore`, `save`, `saved`, `packageText`, `validate`, `console` |
| Sound | `previewSound`, `stopSound`, `importSound`, `removeSound`, `renameSound`, `mute` |
| Settings | `settings`, `applySettings`, `takeSettings`, `resetSettings` |
| Helpers | `project {id or point}` returns the screen position |
| Logic | all commands beginning with `logic` ([Chapter 9](09-logic.md)) |

Property paths follow the actor's JSON form, for example `transform.position`,
`components.mesh.material.color` or `components.joints.0.lower`.

### 11.19.3 `studio.mjs` as a thin layer

`kern/web/studio/studio.mjs` loads the module, creates the DOM panels and runs a simple loop:

1. Each display frame: report viewport rectangles, poll the gamepad, call `kern_studio_frame`, refill
   audio, send network packets.
2. Fetch and execute `take_ui()`: pointer lock, clipboard, context menu, dialog, save, open, package,
   open network …
3. If `revision()` has changed, read `state()` and redraw the panels; each panel only if its JSON has
   changed.
4. Read `status()` and update the HUD, statistics, gizmo readout, marquee selection, robot camera and
   touch controls.
5. Every 200 ms load pending textures, sounds and networks; every 500 ms check autosave and settings.

The page does only what a browser can do and C++ cannot: file dialogs, downloads, clipboard, pointer
lock, image and audio decoding, `localStorage`, Web Audio output, datagrams over BroadcastChannel or
WebSocket, and writing exported games. `kern/web/studio/game.mjs` contains the shared parts (JS wrapper
`StudioEngine` around the C ABI, audio output `KernAudio`, network `KernNet`, gamepad, touch overlay,
HUD) and is at the same time the game host of exported games.

The page also provides `window.KernStudio`: `command(c)`, `state`, `status`, `key(…)`, `pointer(…)`,
`viewportRect(i)` and more. Through it, the browser test `kern/tests/e2e/studio_e2e.mjs` drives Studio
with Playwright: menus, fly mode, tools, Outliner, Details with undo, Content Drawer, context menu, Play,
Simulate, layouts, templates, logic, Micro Duck, multiplayer, export, restore, settings, phone layouts
and the Android file dialog. Most of the images in this chapter come from this test; in the figures the duck shows this
book's own design.

How to use the building blocks yourself is shown in [Chapter 12](12-your-own-games.md); all commands and
functions are collected in [Chapter 17](17-reference.md).

---

© 2026 Christian Heinrich Hohlfeld · All rights reserved · [Imprint and rights](00-imprint.md)
