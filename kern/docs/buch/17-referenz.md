# 17 Referenz

Dieses Kapitel ist zum Nachschlagen. Es listet die C-ABI des WebAssembly-Moduls (Importe und
Exporte, gruppiert), die Kommandozeile von `kern_lehrer`, die Dateiformate und ein Glossar.
Erklärungen stehen in den jeweiligen Kapiteln; hier stehen Signaturen, Bedeutungen und Verweise.

---

## 17.1 Die C-ABI

### Grundregeln

- Jede exportierte Funktion ist `extern "C"` und wird unter **demselben Namen** aus dem Modul
  exportiert. Das Makro steht in `include/kern/api/c_api.h`:

  ```cpp
  #if defined(__wasm__)
  #define KERN_EXPORT(name) extern "C" __attribute__((export_name(#name), used))
  #else
  #define KERN_EXPORT(name) extern "C"
  #endif
  ```

  Nativ (Tests, Lehrer) sind dieselben Funktionen normale C-Funktionen.
- Das Modul ist ein WASI-Reactor: Der Host ruft **einmal `_initialize`**, dann die Exporte
  (`kern.mjs` tut das in `attach()`).
- Daten gehen durch den linearen Speicher: Der Host reserviert mit `kern_alloc`, schreibt und
  übergibt Zeiger. Zeiger sind 32 Bit.
- **Rückgabe 1/0** heißt Erfolg/Fehler; bei 0 steht die Meldung in `kern_last_error()`.
- **Strings** hinein: (Zeiger, Länge) in Bytes, UTF-8.
- **Ergebnis-Puffer** (JSON-Texte, Byte-Puffer) gehören dem Modul und bleiben bis zum nächsten
  Aufruf derselben Gruppe gültig; die Länge liefert `kern_studio_result_length()` bzw.
  `kern_lehrer_result_length()`.
- Welche Funktionen ein Modul enthält, hängt von den Build-Schaltern ab
  ([Kapitel 2](02-bauen-und-testen.md)): Renderer, Partikel, Geometrie und Logik sind immer da,
  `kern_app_*` nur mit `--app`, `kern_studio_*` nur mit `--studio`, `kern_lehrer_*` nur mit `--lehrer`.

### Importe: was das Modul vom Host braucht

| Modul | Funktion | Zweck | Quelle |
|---|---|---|---|
| `kern_gl` | WebGL2-Einsprungpunkte (`createBuffer`, `bindBuffer`, `bufferData`, …, `drawElementsInstanced`, Queries) | GPU-Objekte sind `u32`-Handles (0 = null), Daten (Zeiger, Bytes) | `include/kern/gfx/gl.h` |
| `kern_env` | `now()` | Millisekunden, monoton | `src/core/base.cpp` |
| `kern_env` | `log(level, ptr, length)` | Logausgabe | `src/core/base.cpp` |
| `kern_env` | `terrain_height(x, y)` → `f64` | Höhenabfrage der Geometrie-Jobs | `src/api/geo_api.cpp` |
| `kern_env` | `status(ptr, length)` | Statustext eines C++-Spiels | `src/app/app.cpp` |
| `kern_env` | `pointer_lock(lock)` | Pointer-Lock anfordern/lösen | `src/app/app.cpp` |
| `kern_env` | `logic_jit(ptr, length, id)` → `i32` | erzeugtes WebAssembly-Modul eines Logik-Graphen übersetzen lassen | `src/logic/logic_api.cpp` |
| `kern_env` | `logic_jit_release(slot)` | Tabellenplatz freigeben | `src/logic/logic_api.cpp` |
| `wasi_snapshot_preview1` | `fd_write`, `random_get`, `clock_time_get`, `proc_exit`, `fd_close`, `fd_seek`, sonst `ENOSYS` | minimaler Shim; die Engine berührt keine Dateien | `kern/web/kern.mjs` |

Die Liste der `kern_gl`-Funktionen ist genau das, was die Engine benutzt. Ein neuer
Einsprungpunkt muss in `gl.h` **und** im `kern_gl`-Importobjekt von `kern.mjs` ergänzt werden.

### Schwache und spielseitige Symbole

| Symbol | Bedeutung |
|---|---|
| `extern "C" u32 kern_game_extensions(ShaderExtension*** out)` | Ein Spiel im Modul kann seine Shader-Erweiterungen liefern; schwache Standardversion: keine (`src/api/c_api.cpp`). Wird von `kern_renderer_init` abgefragt. |
| `kern::App* kern_create_app()` | Muss ein Spiel mit `--app` definieren (`include/kern/app/app.h`); `kern_app_start` ruft sie. |
| `extern "C" void kern_set_native_height_sampler(f64 (*)(f64, f64))` | **nur nativ**: ersetzt den Import `terrain_height` (Tests, Werkzeuge). |

---

### Speicher und Fehler (`src/api/c_api.cpp`)

| Funktion | Beschreibung |
|---|---|
| `void* kern_alloc(u32 bytes)` | `malloc` (mindestens 1 Byte) |
| `void kern_free(void* p)` | `free` |
| `const char* kern_last_error()` | Meldung des letzten `fail()` |
| `void kern_clear_error()` | Meldung löschen |

### Renderer (`src/api/c_api.cpp`)

Alle Funktionen wirken auf den prozessweiten Renderer `c_api_renderer()` (genutzt von
`KernRenderer` in `kern.mjs` und von der App-Laufzeit).

| Funktion | Beschreibung |
|---|---|
| `i32 kern_renderer_init(const u32* d)` | Initialisierung aus einem `KERN_ID_*`-Deskriptor; fragt `kern_game_extensions` ab |
| `void kern_renderer_shutdown()` | Renderer abbauen |
| `i32 kern_renderer_resize(i32 w, i32 h)` | Größe des Drawing Buffers |
| `u32 kern_material_key_count()`, `const char* kern_material_key(u32 i)` | Namen der Materialparameter der Erweiterungen |
| `u32 kern_extension_attribute_count()`, `const char* kern_extension_attribute_key(u32 i)`, `u32 kern_extension_attribute_size(u32 i)` | eigene Vertex-Streams der Erweiterungen (Name, Komponenten) |

**Meshes**

| Funktion | Beschreibung |
|---|---|
| `u32 kern_mesh_alloc_id()` | neue Mesh-Id; sie gehört dem Aufrufer |
| `void kern_mesh_release_id(u32 id)` | Id freigeben |
| `i32 kern_mesh_upsert(u32 id, const u32* d)` | Mesh anlegen/ersetzen aus einem `KERN_MD_*`-Deskriptor; mit `KERN_MF_WELD` wird eine passende Dreieckssuppe vorher indexiert. Ungültige Eingaben lassen das vorhandene Mesh unverändert |
| `i32 kern_mesh_instances(u32 id, const f32* t, u32 t_count, const f32* tint, u32 tint_count)` | Instanzen setzen: **−1** Fehler, **0** unverändert, **1** geändert. Leere Transforms heißen „Instanzen behalten“ (reines Tint-Update) |
| `i32 kern_mesh_bones(u32 id, const f32* bones, u32 count)` | Knochenmatrizen fürs Skinning |
| `i32 kern_mesh_visible(u32 id, i32 visible)` | Sichtbarkeit |
| `i32 kern_mesh_remove(u32 id)` | Mesh entfernen |
| `i32 kern_mesh_exists(u32 id)`, `u32 kern_mesh_count()` | Abfragen |
| `i32 kern_mesh_info(u32 id, f64* out)` | `out[8]`: Vertices, Dreiecke, Instanzen, sichtbar, Bounds-Mittelpunkt xyz, Bounds-Radius |
| `const void* kern_mesh_stream(u32 id, u32 stream, u32* count)` | Lesezugriff auf CPU-Ströme: 0 positions, 1 normals, 2 colors, 3 uvs, 4 transforms, 5 tints (`f32`), 6 indices (`u32`) |

**Texturen**

| Funktion | Beschreibung |
|---|---|
| `u32 kern_texture_external(u32 handle)` | eine Textur registrieren, deren GPU-Objekt der Host besitzt (und nach Kontextverlust im selben Slot wiederherstellt) |
| `u32 kern_texture_create(const u8* rgba, i32 w, i32 h, i32 mipmaps, i32 repeat)` | Engine-Textur aus RGBA8 |
| `i32 kern_texture_delete(u32 id)` | löschen |

**Szenenzustand**

| Funktion | Beschreibung |
|---|---|
| `i32 kern_camera_set(const f32* c)` | 12 `f32`: eye xyz, target xyz, up xyz, fov, near, far; prüft Endlichkeit und Bereiche (`invalid camera`) |
| `void kern_lighting_set(const f32* f)`, `void kern_lighting_get(f32* f)` | Beleuchtung, `KERN_LI_*` (27 Slots) |
| `void kern_atmosphere_set(f32 height_falloff, f32 sun_scatter, f32 horizon)` | Höhennebel, Sonnenstreuung, Horizont; alles 0 = reines FogExp2 |
| `void kern_ao_set(i32 enabled, i32 quality, i32 half, f32 radius)` | GTAO: Qualität 0 low … 3 ultra, halbe Auflösung, Radius (0 = Standard) |
| `void kern_sky_set(const f32* f)` | Himmel, `KERN_SK_*` (14 Slots) |
| `i32 kern_point_lights_set(u32 count, const f32* pos_range, const f32* color_intensity)` | bis zu 6 Punktlichter; je 4 `f32` (xyz + Reichweite, rgb + Intensität) |
| `i32 kern_shadows_set(i32 enabled, i32 resolution, f32 extent, f32 bias)` | Sonnenschatten ein/aus und Parameter |
| `void kern_invalidate(i32 shadows)` | Szene als geändert markieren; mit `shadows` wird auch die zwischengespeicherte Schattenkarte ungültig |

**Frame, Abfragen, Kontext**

| Funktion | Beschreibung |
|---|---|
| `i32 kern_render(const u32* p, f64* out)` | ein Frame; Parameter `KERN_RP_*`, Statistik `KERN_RS_*` |
| `i32 kern_screen_ray(f32 x, f32 y, f64* out)` | Strahl durch einen Bildpunkt der aktuellen Kamera; `out[8]`: origin xyz, direction xyz, near, far |
| `i32 kern_camera_screen_ray(const f32* c, f32 x, f32 y, f32 w, f32 h, f64* out)` | dasselbe für eine explizite Kamera (12 `f32`) und Viewport-Größe |
| `i32 kern_pick(f32 x, f32 y, f32 max_distance, u32 ignore_id, f64* out)` | exaktes Dreieck-Picking unter dem Bildpunkt; Ergebnis `KERN_PK_*` |
| `i32 kern_pick_ray(const f32* ray, f32 max_distance, u32 ignore_id, f64* out)` | Picking entlang eines Strahls (origin xyz, direction xyz) |
| `void kern_context_lost()`, `i32 kern_context_restored()` | WebGL-Kontextverlust melden / Wiederherstellung (baut Programme neu, lädt Meshes neu) |

### Deskriptoren (`include/kern/api/c_api.h`)

Die Offsets müssen mit den Konstanten in `kern/web/kern.mjs` übereinstimmen (`MD`, `MF`, `ID`, `RP`,
`RS`, `LI_SLOTS`, `SK_SLOTS`, `PK_SLOTS`).

**Mesh-Deskriptor `KERN_MD_*`** (46 `u32`-Wörter; Ströme als Paar Zeiger, Anzahl)

| Offset | Feld | Offset | Feld |
|---|---|---|---|
| 0 | `POSITIONS` (ptr, Anzahl Floats) | 21 | `TRANSFORMS` |
| 2 | `NORMALS` | 23 | `TINTS` |
| 4 | `COLORS` | 25 | `TEXTURE` |
| 6 | `UVS` | 26 | `FLAGS` (`KERN_MF_*`) |
| 8 | `INDICES` (ptr, Anzahl Indizes) | 27 | `ALPHA_MODE` (0 opak, 1 mask, 2 blend) |
| 10 | `INDEX_BYTES` (2 oder 4) | 28 | `COLOR` (4 × `f32`) |
| 11 | `JOINTS` | 32 | `ALPHA_CUTOFF` (`f32`) |
| 13 | `WEIGHTS` | 33 | `ROUGHNESS` (`f32`) |
| 15 | `BONES` | 34 | `METALNESS` (`f32`) |
| 17 | `EXT0` | 35 | `PARAMS` (8 × `i32`, Materialparameter der Erweiterungen) |
| 19 | `EXT1` | 43 | `EMISSIVE` (3 × `f32`, linear) |

**Mesh-Flags `KERN_MF_*`**

| Bit | Name | Bedeutung |
|---|---|---|
| 1 << 0 | `VISIBLE` | sichtbar |
| 1 << 1 | `UNLIT` | ohne Beleuchtung |
| 1 << 2 | `DOUBLE_SIDED` | beidseitig |
| 1 << 3 | `CAST_SHADOW` | wirft Schatten |
| 1 << 4 | `FRUSTUM_CULLED` | wird gecullt |
| 1 << 5 | `VIEWMODEL` | Viewmodel-Ebene (nach Depth-Clear, z. B. Gizmos) |
| 1 << 6 | `LINEAR_COLORS` | Vertexfarben sind linear (kein sRGB-Dekodieren) |
| 1 << 7 | `WELD` | Dreieckssuppe vor dem Hochladen indexieren |
| 1 << 8 | `LINES` | Linienliste statt Dreiecke (immer unbeleuchtet, kein Schatten, nicht pickbar) |

**Init-Deskriptor `KERN_ID_*`** (9 Wörter): 0 `WIDTH`, 1 `HEIGHT`, 2 `SHADOWS`,
3 `SHADOW_RESOLUTION`, 4 `SHADOW_EXTENT` (`f32`), 5 `SHADOW_BIAS` (`f32`), 6 `BLOOM`,
7 `BLOOM_FORCED`, 8 `GRADE`.

**Render-Parameter `KERN_RP_*`** (10 Wörter): 0 `CLEAR` (4 × `f32`), 4 `HAS_VIEWPORT`, 5 `X`,
6 `Y`, 7 `W`, 8 `H`, 9 `PAGE_HIDDEN`.

**Render-Statistik `KERN_RS_*`** (11 `f64`): `DRAW_CALLS`, `SHADOW_DRAW_CALLS`,
`SHADOW_RESOLUTION`, `SHADOW_REUSED`, `TRIANGLES`, `INSTANCES`, `CULLED`, `CPU_MS`, `GPU_MS`,
`BLOOM` (Zustand; in `kern.mjs`: `off`, `active`, `glow-off-slow-device`, `unsupported`),
`VISIBLE`.

**Beleuchtung `KERN_LI_*`** (27 `f32`): 0 `SUN_DIRECTION`, 3 `SUN_COLOR`, 6 `SKY_COLOR`,
9 `GROUND_COLOR`, 12 `FOG_COLOR`, 15 `FOG_DENSITY`, 16 `EXPOSURE`, 17 `ENV_INTENSITY`,
18 `ENV_ZENITH`, 21 `ENV_HORIZON`, 24 `ENV_GROUND`.

**Himmel `KERN_SK_*`** (14 `f32`): 0 `SUN`, 3 `MOON`, 6 `NIGHT`, 7 `CLOUDS` (> 0,5 = an),
8 `BLAST_DIRECTION`, 11 `BLAST_AGE` (< 0: keine Druckwelle), 12 `WIND` (2 Werte).

**Pick-Ergebnis `KERN_PK_*`** (15 `f64`): 0 `MESH`, 1 `INSTANCE`, 2 `TRIANGLE`, 3 `DISTANCE`,
4 `POINT` (xyz), 7 `NORMAL` (xyz), 10 `BARYCENTRIC` (3), 13 `TRIANGLES_EXAMINED`,
14 `BOUNDS_REJECTED`.

### Partikel (`src/api/fx_api.cpp`)

Ein Partikelsystem je Modul; es zeichnet in ein Mesh, das der Host vorher reserviert hat
([Kapitel 4](04-grafik.md)).

| Funktion | Beschreibung |
|---|---|
| `i32 kern_fx_init(u32 mesh_id, u32 capacity)` | System (neu) anlegen |
| `u32 kern_fx_emit(const f64* s)` | ein Partikel; 16 `f64`: position xyz, velocity xyz, rgba, start_ms, life_ms, size, gravity, grow, fade_extra. Rückgabe: Id, 0 bei Budget-Überlauf oder ungültig |
| `void kern_fx_tick(f64 now_ms, f32 eye_x, f32 eye_y, f32 eye_z, u32* out)` | fortschreiben und sortieren; `out[5]`: aktiv, verworfen, hochgeladen, abgelaufen, Mesh sichtbar |
| `u32 kern_fx_active()`, `u32 kern_fx_dropped()` | Zähler |
| `void kern_fx_clear()` | alle entfernen |

### Geometrie (`src/api/geo_api.cpp`)

Inkrementelle Jobs für Gebäude und Straßen sowie synchrone Terrain-Kacheln; Ergebnisse gehen
geweldet direkt in den Renderer ([Kapitel 5](05-szene-und-geometrie.md)).

| Funktion | Beschreibung |
|---|---|
| `u32 kern_geo_buildings_begin(const f64* stream, u32 length, const f64* o, u32 option_count)` | Gebäude-Job; Optionen: chunk_size, max_buildings, cornice, wall_records, trim rgb, trim_mix, wall_count, roof_count, walls rgb…, roofs rgb…. Rückgabe Job-Id, 0 bei Fehler |
| `u32 kern_geo_roads_begin(const f64* stream, u32 length, const f64* o, u32 option_count)` | Straßen-Job; Optionen: center x/y, radius, chunk_size, segment_length, curb_extra, curb_z, surface_z, marking_z, marking_width, dash_start, dash_length, dash_period, uv_scale, curb rgb, palette_count, palette rgb… |
| `i32 kern_geo_step(u32 id, f64 budget_ms)` | arbeitet höchstens `budget_ms`: 1 fertig, 0 weiter, −1 Fehler |
| `i32 kern_geo_stats(u32 id, f64* out)` | verarbeitet, gebaut, übersprungen, Dreiecke, Zahl der Chunks |
| `i32 kern_geo_chunk(u32 id, u32 index, f64* out)` | Raster x, y, Elemente, Dreiecke, Inhalts-Hash (nie 0) |
| `i32 kern_geo_commit(u32 id, u32 index, u32 mesh_id, const u32* desc, u32 extra_slot, f64* out)` | Chunk welden und als Mesh hochladen (Material aus `KERN_MD_*`); `out`: Bytes vor/nach dem Welding; der Chunk-Speicher wird danach freigegeben |
| `void kern_geo_free(u32 id)` | Job freigeben |
| `i32 kern_geo_terrain_tile(u32 mesh_id, f64 x0, f64 y0, f64 step, u32 cells, f64 r, f64 g, f64 b, const u32* desc, f64* out)` | Terrain-Kachel aus der Höhenabfrage; `out`: min, max Höhe, Bytes |
| `const void* kern_geo_chunk_stream(u32 id, u32 index, u32 stream, u32* count)` | Ströme eines Chunks (0 positions, 1 normals, 2 colors, 3 uvs, 4 extra, 5 indices) – Werkzeuge und Tests |
| `i32 kern_geo_weld(u32 id, u32 index)` | Chunk welden ohne Hochladen – Werkzeuge und Tests |
| `i32 kern_geo_terrain_probe(f64 x0, f64 y0, f64 step, u32 cells, f64 r, f64 g, f64 b)`, `const void* kern_geo_terrain_probe_stream(u32 stream, u32* count)` | Terrain-Kachel in einen Prüfpuffer – Werkzeuge und Tests |

### App-Laufzeit (`src/app/app.cpp`, nur mit `--app`)

Vom Host `kern/web/app.mjs` getrieben ([Kapitel 12](12-eigene-spiele.md)).

| Funktion | Beschreibung |
|---|---|
| `void kern_app_set_args(const char* text, u32 length)` | Startparameter vor dem Start (`k=v&k2=v2`, URL-kodiert) |
| `i32 kern_app_start(i32 width, i32 height)` | `kern_create_app()`, `configure()`, Renderer-Init, `start()` |
| `i32 kern_app_frame(f64 now_ms, i32 page_hidden)` | ein Frame: `update()` (dt auf 0…0,1 s begrenzt), dann Rendern; 0, wenn das Rendern scheitert |
| `i32 kern_app_resize(i32 width, i32 height)` | Größe; ruft `resized()` |
| `void kern_app_key(u32 key, i32 down)` | Taste (`kern::Key`) |
| `void kern_app_pointer(f32 x, f32 y, f32 dx, f32 dy, u32 buttons)` | Zeigerposition, Bewegung, Tastenbits |
| `void kern_app_wheel(f32 dy)` | Mausrad |
| `void kern_app_touch(i32 id, i32 phase, f32 x, f32 y)` | Finger; Phase 0 beginnt, 2 endet, sonst Bewegung |
| `void kern_app_focus(i32 focused)` | Fokus; bei Verlust werden alle Tasten losgelassen |
| `void kern_app_stop()` | `stop()` und Renderer abbauen |

### Kern Studio (`src/studio/api.cpp`, nur mit `--studio`)

Die Seite `kern/web/studio/` und exportierte Spiele (`game.mjs`) sprechen den Editor über diese
Funktionen an. JSON-Antworten kommen als Zeiger zurück (gültig bis zum nächsten Aufruf, Länge per
`kern_studio_result_length()`); vor einer erfolgreichen Initialisierung liefern fast alle Funktionen
`nullptr` bzw. 0 (Ausnahme: `kern_studio_load_policy` registriert Policies auch vorher). Die Befehle für `kern_studio_command` beschreibt [Kapitel 11](11-studio.md)
(Quelle: `docs/studio.md`, Abschnitt „Protokoll“).

**Lebenszyklus und Rendern**

| Funktion | Beschreibung |
|---|---|
| `i32 kern_studio_init(i32 width, i32 height, i32 player_only)` | Renderer (Schatten 2048 px, Ausdehnung 110, Bias 0,0014, Bloom, Erweiterung `NightWindows`) und Studio; `player_only = 1` startet die Laufzeit exportierter Spiele ohne Editor |
| `void kern_studio_ao_set(i32 enabled, i32 quality, i32 half, f32 radius)` | GTAO wie `kern_ao_set` |
| `void kern_studio_shadow_filter(i32 taps)` | PCF-Taps des Sonnenschattens: 1, 4 (Telefone) oder 9 (Desktop); die Seite wählt nach dem Gerät, `?shadowFilter=1\|4\|9` überschreibt |
| `i32 kern_studio_resize(i32 width, i32 height)` | Drawing Buffer |
| `void kern_studio_frame(f64 now_ms)` | Kameras, Simulation, Overlays, Rendern aller sichtbaren Viewports (nicht bei verlorenem Kontext) |
| `void kern_studio_set_viewport(u32 index, i32 x, i32 y, i32 w, i32 h, f32 css_w, f32 css_h, f32 dpr, i32 visible)` | Rechteck eines Viewports im Drawing Buffer (Ursprung unten links) |
| `void kern_studio_context_lost()`, `i32 kern_studio_context_restored()` | Kontextverlust |

**Protokoll**

| Funktion | Beschreibung |
|---|---|
| `const char* kern_studio_command(const char* json, u32 length)` | ein Befehl `{"cmd": …}`; Antwort `{"ok": …}`; unlesbares JSON ergibt `{"ok":false,"error":"invalid command: …"}` (Tiefe bis 120) |
| `const char* kern_studio_state()` | voller Panel-Zustand (Outliner, Details, Menüs …) |
| `const char* kern_studio_status()` | Werte pro Frame (HUD, Statistik, Kamera, Overlays) |
| `f64 kern_studio_revision()` | ändert sich, wenn `state()` neu gelesen werden muss |
| `const char* kern_studio_take_ui()` | Aufträge an die Seite (Pointer fangen, Zwischenablage, Dialoge, Speichern, Öffnen, Spiel packen …) |
| `u32 kern_studio_result_length()` | Länge des letzten Ergebnisses |

**Eingabe**

| Funktion | Beschreibung |
|---|---|
| `const char* kern_studio_pointer(u32 viewport, u32 type, f32 x, f32 y, i32 button, i32 buttons, u32 modifiers, f32 wheel)` | Zeiger; `type` 0 down, 1 move, 2 up, 3 cancel, 4 wheel, 5 leave; Modifikatoren 1 Shift, 2 Ctrl/Meta, 4 Alt |
| `i32 kern_studio_touch(u32 type, i32 id, f32 x, f32 y, f64 time_ms)` | Finger/Stift im aktiven Viewport (0 down, 1 move, 2 up, 3 cancel); 1, wenn die Touch-Steuerung ihn benutzt hat |
| `void kern_studio_look(f32 dx, f32 dy)` | Maus-Look bei Pointer-Lock |
| `const char* kern_studio_key(const char* key, u32 key_length, const char* code, u32 code_length, i32 down, u32 modifiers, i32 repeat)` | Tastatur (`KeyboardEvent.key`/`.code`); Antwort `{"handled": bool}` |
| `void kern_studio_blur()` | Fenster hat den Fokus verloren |

**Texturen, Policies, Sound**

| Funktion | Beschreibung |
|---|---|
| `const char* kern_studio_pending_textures()` | Texturen, die dekodierte Pixel brauchen: `["tex1", …]` |
| `const char* kern_studio_texture_source(const char* id, u32 length)` | Quelle einer Textur |
| `i32 kern_studio_texture_pixels(const char* id, u32 length, i32 width, i32 height, const u8* rgba)` | dekodierte Pixel übergeben |
| `const char* kern_studio_pending_policies()` | noch fehlende Policy-Dateien der Micro Duck (aus `manifest.json`, dazu `jump.onnx` und `play.onnx`) – nur wenn das Level einen Walker enthält |
| `i32 kern_studio_load_policy(const char* file, u32 length, const u8* data, u32 bytes)` | eine `.onnx`-Datei; 1, wenn es ein rsl_rl-Actor ist, den Kern rechnen kann |
| `i32 kern_studio_audio_init(u32 sample_rate)` | Mixer mit der Rate des Ausgabegeräts starten |
| `const f32* kern_studio_audio_render(u32 frames)` | `frames` (höchstens 16384) Stereo-Frames, verschränkt |
| `i32 kern_studio_audio_prepare(f64 budget_ms)` | eingebaute Klänge für etwa `budget_ms` vorrechnen (Standard 2 ms) |
| `const char* kern_studio_pending_sounds()`, `const char* kern_studio_sound_source(const char* id, u32 length)` | importierte Sounds, die PCM brauchen |
| `i32 kern_studio_sound_pcm(const char* id, u32 length, const f32* samples, u32 frames, u32 channels, u32 rate)` | dekodiertes PCM (1 oder 2 Kanäle, beliebige Rate) |
| `void kern_studio_sound_failed(const char* id, u32 length, const char* reason, u32 reason_length)` | Dekodieren gescheitert |

**Netzwerk** (die Seite bewegt nur Datagramme; `KernNet` in `game.mjs`, [Kapitel 10](10-netzwerk.md))

| Funktion | Beschreibung |
|---|---|
| `void kern_studio_net_push(f64 from, const u8* data, u32 bytes)` | eingehendes Datagramm von Peer `from` (53-Bit-Id der Seite, > 0) |
| `u32 kern_studio_net_take()` | nächstes ausgehendes Datagramm: Größe (0 = keins) |
| `const u8* kern_studio_net_out()`, `f64 kern_studio_net_out_to()` | dessen Bytes und Ziel |

**Autosave** (`docs/studio.md`, „Wiederherstellen“)

| Funktion | Beschreibung |
|---|---|
| `const char* kern_studio_take_autosave(f64 now_ms)` | Leveltext, wenn eine Sicherung fällig ist und ungespeicherte Änderungen existieren; sonst `nullptr` |
| `i32 kern_studio_take_autosave_discard()` | 1 (einmal), wenn die gespeicherte Sicherung keine ungespeicherte Arbeit mehr enthält und gelöscht werden soll |
| `i32 kern_studio_dirty()` | Dokument hat ungespeicherte Änderungen |

### Lehrer (`src/lehrer/lehrer_api.cpp`, nur mit `--lehrer`)

Ein Modul je Web Worker; ein Worker ist Sammler und/oder Lerner ([Kapitel 14](14-lehrer.md)).
Ergebnisse sind Zeiger mit `kern_lehrer_result_length()` Bytes, gültig bis zum nächsten Aufruf.

| Funktion | Beschreibung |
|---|---|
| `u32 kern_lehrer_result_length()` | Bytes des letzten Ergebnisses |
| `i32 kern_lehrer_create(const char* task, u32 task_length, i32 envs, u32 seed, i32 first)` | Umgebungen `first … first+envs−1` eines Laufs (`jump`, `stand`) mit dessen Rezept; Worker w von k bekommt `first = w × envs` |
| `void kern_lehrer_progress(i32 iteration)` | vor Iteration `iteration` (ab 1): Lehrpläne und Stöße laut Rezept |
| `i32 kern_lehrer_set_policy(const f32* data, u32 count)` | Policy zum Handeln (`Policy::serialize`, `count` Floats) |
| `const u8* kern_lehrer_collect(i32 steps)` | `steps` Schritte jeder Umgebung; Rückgabe: serialisierter Rollout |
| `i32 kern_lehrer_learner(const char* task, u32 task_length, const char* config, u32 config_length, u32 seed)` | Lerner mit Rezept; JSON überschreibt `learningRate`, `initStd`, `entropyCoef`, `desiredKl`, `steps`, `epochs`, `minibatches`, `warmStart` (`"velstand"` \| `"none"`), `imitate` |
| `i32 kern_lehrer_steps()` | Schritte je Umgebung und Iteration des Rezepts |
| `const char* kern_lehrer_evaluate(i32 episodes)` | nur `jump`: JSON `jumps`, `episodes`, `heightMm`, `riseMm`, `tiltDeg`, `flightS`, `falls` |
| `const u8* kern_lehrer_dagger(i32 round, i32 first, i32 count)` | Anteil eines Sammlers an einer Nachahmungsrunde: `u32` Takte, dann Takte × 61 Beobachtungen und Takte × 14 Lehrer-Aktionen (`f32`) |
| `i32 kern_lehrer_imitation_episodes()`, `i32 kern_lehrer_imitation_left()` | Episoden je Runde; verbleibende Runden |
| `const char* kern_lehrer_imitate(const u8* data, u32 bytes, u32 count)` | eine Nachahmungsrunde aus `count` Anteilen (je mit `u32`-Größe davor; `count = 0`: der Lerner rechnet die Runde selbst); nach der letzten beginnt das Üben. JSON: `round`, `rounds`, `samples`, `error` und die Sprungwerte |
| `const f32* kern_lehrer_policy()` | aktuelle Policy des Lerners (Länge in Bytes) |
| `const char* kern_lehrer_learn(const u8* data, u32 bytes, u32 count)` | lernt aus `count` Rollouts (je mit `u32`-Größe davor); JSON: `iteration`, `steps`, `meanReward`, `meanReturn`, `meanLength`, `episodes`, `valueLoss`, `surrogateLoss`, `entropy`, `learningRate`, `meanStd`, `collectSeconds`, `learnSeconds` |
| `const u8* kern_lehrer_export_onnx()` | Actor als ONNX im Format von Pollens Netzen |
| `const u64* kern_lehrer_fingerprint(i32 ticks)` | Determinismus-Fingerabdruck, ein `u64` je Regeltakt |

### Logik (`src/logic/logic_api.cpp`)

Diese Funktionen ruft **erzeugter Code**, nicht der Host: Ein JIT-kompilierter Graph importiert sie
als `env.call_node`, `env.read_pin` und `env.trace` (wasm-zu-wasm-Aufrufe), und der Host meldet mit
`kern_logic_jit_ready` den fertigen Tabellenplatz ([Kapitel 9](09-logik.md)).

| Funktion | Beschreibung |
|---|---|
| `i32 kern_logic_call_node(i32 id, i32 node, i32 entry)` | C++-Funktion eines Knotens der Instanz `id` ausführen |
| `f64 kern_logic_read_pin(i32 id, i32 node, i32 pin)` | Datenpin lesen |
| `void kern_logic_trace(i32 id, i32 node)` | Ausführung eines Knotens melden (Debugger) |
| `void kern_logic_jit_ready(i32 id, i32 function)` | Host: Modul für Instanz `id` liegt im Tabellenplatz `function`; −1 = gescheitert, der Graph bleibt im Interpreter |

---

## 17.2 Kommandozeile von `kern_lehrer`

`kern_lehrer` ist der Lehrer als natives Programm (`kern/tools/kern_lehrer.cpp`, gebaut mit
`kern/tools/lehrer.sh` nach `kern/build/kern_lehrer`; [Kapitel 2](02-bauen-und-testen.md),
[Kapitel 14](14-lehrer.md)).

### Ablauf und Betriebsarten

Nach dem Lesen aller Optionen lädt das Programm Pollens Policies (`--policies`) und wählt die
**erste** zutreffende Betriebsart in dieser Reihenfolge:

| # | Bedingung | Betriebsart |
|---|---|---|
| 1 | `--fingerprint N` | Determinismus-Fingerabdruck: N Zeilen `<takt> <hash>`, Ende |
| – | (Prüfung) | `--task` muss `jump` oder `stand` sein, sonst `unknown task …`, Rückgabe 2 |
| 2 | `--eval FILE` | Sprung-Bewertung eines Netzes |
| 3 | `--trace FILE` | eine Episode Takt für Takt |
| 4 | `--kalibrieren LISTE` | Servo-Kalibrierung |
| 5 | `--abgleich FILE` | Abgleich mit einem Roboter-Log |
| 6 | `--beispiele DIR` | Beispiel-Logs schreiben |
| 7 | `--vorbild-suchen FILE` | Sprung-Vorbild suchen (CMA-ES) |
| 8 | `--vorbild-pruefen X` | Vorbild bewerten |
| 9 | `--spielen-pruefen X` | Spiel-Netz oder Vorbild bewerten |
| 10 | `--spielen-ueben FILE` | Spiel-Netz üben (PPO) |
| 11 | `--spielen-spur` | eine Spiel-Episode, Entscheidung für Entscheidung |
| 12 | `--biss-fenster` | messen, wo der Schnabel eine Socke greift |
| 13 | `--halt-messen` | messen, wie weit die Ente nach „Stopp“ läuft |
| 14 | `--demo-spur` | das Micro-Duck-Beispiellevel des Studios abspielen |
| 15 | `--spielen` | Spiel-Netz durch Nachahmung (DAgger) |
| 16 | (sonst) | **Training** von `--task` |

`--spielen`, `--spielen-spur`, `--biss-fenster`, `--halt-messen` und `--demo-spur` setzen denselben
internen Schalter; gilt der letzte auf der Kommandozeile. Eine unbekannte Option gibt die Kurzhilfe
aus und endet mit Rückgabe 2.

### Optionen

**Allgemein**

| Option | Standard | Bedeutung |
|---|---|---|
| `--task jump\|stand` | `jump` | Aufgabe (Training, `--trace`) |
| `--threads N` | Hardware-Threads | Rechen-Threads |
| `--envs N` | 16 × Threads | Enten insgesamt; wird auf ein Vielfaches der Threads abgerundet (mindestens 1 je Thread). Das Ergebnis hängt von der Zahl der Enten ab, nicht von der Zahl der Threads |
| `--seed N` | 1 | Seed. Bei `--trace` und `--spielen-spur` Episodenseed (intern 1000 + N); bei `--demo-spur` die **Dauer in Sekunden** (Werte über 1; ohne `--seed` oder mit 0 bzw. 1 sind es 120 s) |
| `--iterations N` | 600 | Training: letzte Iteration. Bei `--kalibrieren` Generationen (Standard 80), `--vorbild-suchen` Generationen (Standard 120), `--spielen-ueben` Iterationen (Standard 300) – jeweils, wenn nicht abweichend von 600 angegeben |
| `--episodes N` | 10 | Episoden für `--eval`, `--vorbild-pruefen`, `--spielen-pruefen` |
| `--out FILE` | `<task>.onnx` | Ausgabe. `--kalibrieren`: `<profil>_calibrated.json`; `--spielen`: `play.onnx`; `--spielen-ueben`: `play_geuebt.onnx` |
| `--policies DIR` | `kern/third_party/microduck/` (relativ zum Quellpfad beim Bauen) | Pollens ONNX-Dateien |
| `--profil FILE` | XL330 | Servoprofil (JSON) für Training, Startwert der Kalibrierung, Abgleich |

**Training (PPO) – überschreibt das Rezept** (`recipe.h`; [Kapitel 14](14-lehrer.md))

| Option | Rezept `jump` | Rezept `stand` | Bedeutung |
|---|---|---|---|
| `--steps N` | 48 | 24 | Schritte je Ente und Iteration |
| `--init-std X` | 0,3 | 1,0 | anfängliche Aktions-Standardabweichung |
| `--lr X` | 1e-4 | 1e-3 | Lernrate |
| `--entropy X` | 0,005 | 0,01 | Entropie-Koeffizient |
| `--critic A,B,…` | 256,128 | 256,128 | Schichten des Critic |
| `--actor A,B,…` | 512,256,128 | 512,256,128 | Schichten des Actor |
| `--epochs N` | 5 | 5 | Epochen je Update |
| `--minibatches N` | 4 | 4 | Minibatches je Epoche |
| `--adaptive-lr 0\|1` | 1 | 1 | Lernrate aus der KL-Divergenz anpassen |
| `--kl X` | 0,01 | 0,01 | Ziel-KL der adaptiven Lernrate |
| `--substeps N` | 16 | 16 | Box3D-Substeps (`game::kWalkerSubsteps`) |
| `--warm-start NAME` | `velstand` | `none` | `velstand`: Der Actor startet als Pollens `velstand`-Netz; jeder andere Wert ergibt keinen Warmstart (`make_learner` in `src/lehrer/recipe.cpp`) |

**Vorbild und Nachahmung im Training**

| Option | Standard (`jump`) | Bedeutung |
|---|---|---|
| `--vorbild builtin\|demo.json\|none` | `builtin` | Demonstration zum Nachahmen; `none` schaltet die Nachahmung ab |
| `--nachahmen N` | 8 | DAgger-Runden (bei `--spielen`: Standard 8) |
| `--episoden N` | 96 | Episoden je Runde (bei `--spielen`: Standard 64) |
| `--uebungs-std X` | 0,1 | Aktionsrauschen beim Üben nach der Nachahmung (bei `--spielen-ueben`: Standard 0,1) |
| `--nachahmungs-lohn X` | 10 | Gewicht der Belohnung für vorbildgetreues Handeln beim Üben |

**Ausgabe und Fortsetzen im Training**

| Option | Standard | Bedeutung |
|---|---|---|
| `--log FILE` | – | CSV je Iteration: `iteration,steps,seconds,mean_reward,mean_return,mean_length,episodes,value_loss,entropy,mean_std,learning_rate,steps_per_second,jumps,height_mm,flight_s,falls` (beim Fortsetzen angehängt) |
| `--checkpoint FILE` | – | ganzer Lerner alle `--save-every` Iterationen und am Ende; nach der Nachahmung zusätzlich `FILE.nachgeahmt` |
| `--resume FILE` | – | vom Checkpoint fortsetzen (ohne erneute Nachahmung) |
| `--save-every N` | 25 | Abstand für ONNX und Checkpoint |
| `--keep-every N` | 0 (aus) | Schnappschüsse `<stamm>_itNNNNN.onnx` |
| `--eval-every N` | 10 | bei `jump`: alle N Iterationen 5 Bewertungsepisoden; ab 4 von 5 echten Sprüngen meldet das Programm `FIRST REAL JUMP` und schreibt sofort `--out` |
| `--stop-after-jump` | aus | nach dem ersten echten Sprung aufhören (die Datei wird danach nicht überschrieben) |

Mit Nachahmung schreibt das Training außerdem `<stamm>_nachgeahmt.onnx` (das Netz nach der
Nachahmung, vor dem Üben).

**Bewerten und Nachvollziehen**

| Option | Bedeutung |
|---|---|
| `--fingerprint N` | Fingerabdruck über N Regeltakte (Referenz: `kern/tests/determinism_fingerprint.txt` mit 400) |
| `--eval FILE` | Sprung eines Netzes: sauberer Roboter und volle Variation (Bau, Servos, Sensoren, Start) |
| `--trace FILE\|builtin\|demo.json` | eine saubere Episode Takt für Takt: Höhe, Fußkontakte, Neigung, Belohnungsterme; mit `builtin` oder `.json` fährt das Vorbild |
| `--randomized` | für `--trace` und `--spielen-spur`: mit Variation |

**Kalibrierung** ([Kapitel 16](16-kalibrierung.md))

| Option | Bedeutung |
|---|---|
| `--kalibrieren a.csv,b.csv` | Prüfstand-Logs, an die angepasst wird |
| `--pruefen c.csv[,…]` | zurückgehaltene Prüf-Logs |
| `--abgleich robot.csv` | Roboter-Log gegen die Simulation (Gyro, Schwerkraft, Gelenke, je Gelenk) |
| `--beispiele DIR` | schreibt `servo_steps.csv`, `servo_chirp.csv`, `robot.csv` und `xl330.json` (in der Simulation erzeugt) |

**Vorbild**

| Option | Bedeutung |
|---|---|
| `--vorbild-suchen demo.json` | neues Sprung-Vorbild mit CMA-ES; optional Start mit `--vorbild` |
| `--vorbild-pruefen builtin\|demo.json` | Vorbild sauber und mit Variation bewerten, dazu Punktzahl über 10 Episoden |

**Spielen** (Ball und Socke; [Kapitel 14](14-lehrer.md))

| Option | Bedeutung |
|---|---|
| `--spielen` | Spiel-Netz durch Nachahmung des Vorbilds (DAgger); nach jeder Runde wird `--out` geschrieben |
| `--spielen-pruefen vorbild\|play.onnx` | Vorbild oder Netz bewerten (sauber und mit Variation) |
| `--spielen-ueben play.onnx` | Netz mit PPO üben (Hälfte der Enten sauber, Hälfte mit Variation; Critic wärmt 20 Iterationen vor); alle 20 Iterationen Bewertung und `<stamm>_itNNNN.onnx` |
| `--spielen-spur` | eine Episode, Entscheidung für Entscheidung; mit `--netz play.onnx` fährt das Netz, die Zeile zeigt beide Antworten |
| `--netz play.onnx` | Spiel-Netz für `--spielen-spur` und `--demo-spur` |
| `--biss-fenster` | wo der Schnabel eine Socke bekommt |
| `--halt-messen` | wie weit die Ente nach „Stopp“ geht |
| `--demo-spur` | Studio-Beispiellevel der Micro Duck mit 60-Hz-Frames, einmal pro Sekunde eine Zeile mit Zustand, Beobachtung und der Antwort des Spielnetzes; Netze aus `kern/policies/`, `--seed` ist die Dauer in Sekunden (Standard 120). Umgebungsvariable `KERN_JUMP_AT=<s>` löst in Sekunde s den Sprung aus (Taste 7) |

### Beispiele

```sh
kern/tools/lehrer.sh
kern/build/kern_lehrer --task jump --iterations 3000 --out jump.onnx --log jump.csv --checkpoint run.ckpt
kern/build/kern_lehrer --eval jump.onnx --episodes 40
kern/build/kern_lehrer --trace jump.onnx
kern/build/kern_lehrer --vorbild-suchen demo.json
kern/build/kern_lehrer --kalibrieren steps.csv,chirp.csv --pruefen rechteck.csv --out xl330_unit3.json
kern/build/kern_lehrer --abgleich roboter.csv --profil xl330_unit3.json
kern/build/kern_lehrer --task jump --profil xl330_unit3.json --out jump.onnx
kern/build/kern_lehrer --fingerprint 400 | diff - kern/tests/determinism_fingerprint.txt
```

---

## 17.3 Dateiformate

### `.klevel` – Levels

Ein Level ist JSON mit `"format": "kern-level"` und `"version": 1`. Der Schreiber
(`to_json(const Level&)` in `src/game/level.cpp`) legt diese Schlüssel auf oberster Ebene an:

| Schlüssel | Inhalt |
|---|---|
| `format`, `version` | `"kern-level"`, `1` |
| `name` | Name des Levels |
| `world` | World Settings |
| `camera` | Editor-Kamera: `position`, `yaw`, `pitch` |
| `nextId` | nächste freie Actor-Id |
| `actors` | Actors mit Transform und Komponenten |
| `textures` | eingebettete Texturen: `id`, `name`, `width`, `height`, `src` |
| `sounds` | importierte Sounds: `id`, `name`, `duration`, `src` |
| `logic` | Level-Graph (nur wenn vorhanden) |

Einheiten: Meter, Sekunden, Kilogramm, Z oben; Rotationen als Euler-Winkel in Grad (x Roll,
y Pitch, z Yaw; angewandt Yaw × Pitch × Roll); Farben sRGB 0…1. Ein anderes `format` wird
abgelehnt („not a Kern level“), ebenso eine höhere `version` („level was written by a newer Kern
Studio“). Komponenten und Felder beschreibt [Kapitel 8](08-spiel-laufzeit.md).

### ONNX-Policies

Kern liest und schreibt die **rsl_rl-Actors** im Format, das Pollen Robotics für die Micro Duck
exportiert (`include/kern/game/walker_policy.h`, `include/kern/lehrer/onnx_export.h`).

**Graph** (Opset 17, IR-Version 8):

```
obs [1, n] → Sub(obs_mean) → Div(obs_div) → (Gemm, Elu(alpha 1))* → Gemm → actions [1, m]
```

- Eingang `obs`, Ausgang `actions`, `float32`. Die Gemm-Gewichte liegen als `[out][in]` mit
  `transB = 1`.
- Der Normalisierer ist Teil des Graphen: `(obs − mean) / div` (laut `docs/lehrer.md`
  `div` = σ + 0,01).
- Bewegungsnetze haben 61 Beobachtungen und 14 Aktionen (Pollens `velstand`: 61 → 512 → 256 → 128 →
  14). Das Spiel-Netz `play.onnx` hat 23 → 9 (25 Hz).

**Lesen:** `parse_onnx_policy` akzeptiert genau diese Form und meldet sonst einen Grund, z. B.
`not an ONNX file (protobuf)`, `not an rsl_rl actor (Sub, Div, Gemm...)`, `ONNX tensor is not
float32`, `expected Elu after a hidden Gemm`, `unsupported ONNX operator`, `the output layer is not
linear`. Die Metadaten werden mitgelesen (`WalkerPolicy::metadata`, `meta(key)`); die
Laufparameter einer Bewegung (Art, Dauer, Kommando-Kodierung, `action_scale` …) kommen aus dem
Policy-Manifest `third_party/microduck/manifest.json`, das eingebaut ist
(`micro_duck_manifest()`).

**Metadaten beim Export** (`micro_duck_metadata(task)`):

| Schlüssel | Wert |
|---|---|
| `action_scale` | `1.0` |
| `command_names` | `twist` |
| `observation_names` | `base_ang_vel,projected_gravity,joint_pos,joint_vel,actions,command,head_command,body_command` |
| `joint_names` | `left_hip_yaw,left_hip_roll,left_hip_pitch,left_knee,left_ankle,neck_pitch,head_pitch,head_yaw,head_roll,right_hip_yaw,right_hip_roll,right_hip_pitch,right_knee,right_ankle` |
| `default_joint_pos` | `0.000,-0.087,-0.458,-0.005,0.453,0.349,0.349,0.000,0.000,0.000,0.087,0.458,0.005,-0.453` |
| `joint_stiffness` | vierzehnmal `1.000` |
| `joint_damping` | vierzehnmal `0.000` |
| `task` | die Aufgabe, z. B. `jump` |
| `lehrer` | `Kern (C++ PPO on Box3D)` |

Für das Spiel-Netz (`play_metadata()`): `task` = `play`, `lehrer` = `Kern (C++ DAgger + PPO on
Box3D)`, `rate_hz` = `25`, `observation_names` (23 Namen von `ball_known` bis `walking_s`),
`action_names` = `forward,strafe,turn,head_yaw,head_pitch_down,beak_open,kick_left,ground_pick,neck_bow`.

**Der Pick-up-Detektor** (`pickup_detector.onnx`, robotd) hat eine andere Form: Eingang ein Fenster
von 50 × 63 Merkmalen, Normieren und Maskieren, Gather der gelesenen Spalten, drei Conv1d mit ELU,
dann Gemm, ELU, Gemm, Sigmoid; Ausgang p(gehalten). Ihn liest `parse_onnx_pickup`
(`PickupDetector`); er ist als Bytes eingebaut (`third_party/microduck/pickup_detector.inc`,
`pickup_detector()`), [Kapitel 13](13-micro-duck.md).

**Wo die Dateien liegen:** Pollens Set in `kern/third_party/microduck/` (`velstand.onnx` und
`pickup_detector.onnx` sind zusätzlich eingebaut), Kerns eigene Netze in `kern/policies/` (`jump.onnx`, `play.onnx`). Auf der
veröffentlichten Seite liegen sie in `editor/microduck/` und `editor/policies/`. Ein im Browser
trainiertes Netz speichert die Lehrer-Seite in IndexedDB („kern-lehrer“); es ersetzt auf demselben
Gerät das mitgelieferte ([Kapitel 14](14-lehrer.md), [Kapitel 15](15-eigene-modelle.md)).

### Servoprofil (JSON)

Ein Servoprofil beschreibt einen Servotyp im Gerätemodell `game::servo_step`
(`include/kern/game/servo.h`, [Kapitel 16](16-kalibrierung.md)). Das XL330-Profil, wie
`docs/kalibrierung.md` es zeigt:

```json
{
  "kind": "servo",
  "name": "xl330",
  "stiffness": 0.554, "damping": 0.053, "friction": 0.0048, "torque": 0.963,
  "armature": 0.0018, "delay": 0.02,
  "spreadArmature": 0.1, "spreadFriction": 0.1, "delayMin": 0.015, "delayMax": 0.03
}
```

Der Schreiber (`game::to_json(const ServoProfile&)`, z. B. bei `kern_lehrer --beispiele DIR` →
`xl330.json` oder als Ergebnis von `--kalibrieren`) gibt mit zwei Leerzeichen Einrückung immer
**alle** Felder aus, in der Reihenfolge `kind`, `name`, `source` (nur wenn gesetzt), `stiffness`,
`damping`, `friction`, `torque`, `armature`, `delay`, `spreadStiffness`, `spreadDamping`,
`spreadFriction`, `spreadTorque`, `spreadArmature`, `delayMin`, `delayMax`. Die Werte sind intern
`f32` und werden als `f64` formatiert; in geschriebenen Dateien können deshalb längere
Dezimaldarstellungen stehen als oben.

| Feld | Einheit | Gültig | Bedeutung |
|---|---|---|---|
| `kind` | – | `"servo"` (optional) | Art des Profils; ein anderer Wert wird abgelehnt |
| `name`, `source` | – | Text | Name, Herkunft der Werte |
| `stiffness` | N·m/rad | 0 … 1e6, **> 0** | K |
| `damping` | N·m·s/rad | 0 … 1e6 | D (Gegen-EMK + viskos) |
| `friction` | N·m | 0 … 1e6 | F, Coulomb-Reibung im Gelenk |
| `torque` | N·m | 0 … 1e6, **> 0** | T, höchstes Moment |
| `armature` | kg·m² | 0 … 1e3 | A, Rotorträgheit durch das Getriebe |
| `delay` | s | 0 … 1 | Totzeit Kommando → Servo |
| `spreadStiffness`, `spreadDamping`, `spreadFriction`, `spreadTorque`, `spreadArmature` | relativ (±) | 0 … 1 | Streuung, aus der das Training je Servo zieht |
| `delayMin`, `delayMax` | s | 0 … 1 | Bereich der Bus-Totzeit im Training; `delayMax < delayMin` wird auf `delayMin` gesetzt |

Fehlende Felder behalten die Standardwerte von `ServoProfile` (0, Name `xl330`); da `stiffness` und
`torque` positiv sein müssen, braucht eine gültige Datei mindestens diese beiden. Meldungen:
`not a servo profile`, `bad value for <feld>`, `stiffness and torque must be positive`.

Das Servomodell: Moment = clamp(K × (Ziel − Winkel), −T, T) − D × Geschwindigkeit, dazu
Coulomb-Reibung F, Rotorträgheit A auf der Gelenkachse und das Kommando `delay` Sekunden verspätet.

### Prüfstand- und Roboter-Logs (CSV)

| Format | Aufbau | Quelle |
|---|---|---|
| Prüfstand-Log | Kommentarzeilen `# mass`, `# arm`, `# inertia`, `# horizontal` (Last), Kopfzeile, dann `time,target,angle` (s, rad, rad) | `parse_servo_log` |
| Roboter-Log | Kopfzeile, dann je Regeltakt (50 Hz) 61 Beobachtungen und 14 Aktionen; der Roboter steht zu Beginn still in Grundstellung | `parse_robot_log` |

Einzelheiten in [Kapitel 16](16-kalibrierung.md) und `docs/kalibrierung.md`.

---

## 17.4 Glossar

| Begriff | Bedeutung |
|---|---|
| **Actor** | Objekt eines Levels mit Transform (relativ zum Elternteil) und optionalen Komponenten ([Kapitel 8](08-spiel-laufzeit.md)) |
| **Armature** | Rotorträgheit eines Servos, durch das Getriebe auf die Gelenkachse übersetzt (kg·m²) |
| **Box3D** | 3D-Physik-Engine von Erin Catto, unverändert als C17 mitkompiliert ([Kapitel 6](06-physik.md)) |
| **C-ABI** | die flachen `extern "C"`-Funktionen des Moduls, über die JavaScript die Engine anspricht |
| **CMA-ES** | ableitungsfreies Optimierungsverfahren; im Lehrer für Kalibrierung und Vorbildsuche |
| **Collector / Sammler** | führt Umgebungen mit einer gegebenen Policy aus und liefert Rollouts |
| **Content Drawer** | Panel des Studios zum Platzieren von Actors |
| **Critic** | Wertfunktion im PPO; sieht im Lehrer den sauberen Zustand (asymmetrisch) |
| **DAgger** | Nachahmungslernen: Das Netz fährt, das Vorbild sagt je Takt die richtige Aktion |
| **Determinismus-Fingerabdruck** | Hash je Regeltakt über den Zustand der gehenden Ente; Referenz in `kern/tests/determinism_fingerprint.txt` |
| **`dm`** | `kern::dm`, deterministische transzendente Funktionen aus musl |
| **Domain Randomization / Variation** | Training mit zufälligen Abweichungen in Bau, Servos, Sensoren und Start |
| **DuckEnv** | Lehrer-Umgebung: eine Micro Duck in einer eigenen `GameWorld` |
| **Fehlerkanal** | `fail()` / `last_error()` statt Ausnahmen |
| **GameWorld** | stellt ein Level mit dem Renderer dar und spielt es auf Box3D |
| **GTAO** | Ground-Truth Ambient Occlusion (nach XeGTAO) im Renderer |
| **HostTransport** | Transport, dessen Datagramme die Browserseite bewegt (BroadcastChannel, WebSocket-Relay) |
| **JIT (Logik)** | Übersetzung eines Logik-Graphen in ein WebAssembly-Modul zur Laufzeit |
| **`.klevel`** | Dateiformat eines Levels (JSON, `"format": "kern-level"`) |
| **Lehrer** | Kerns Trainingssystem für Micro-Duck-Netze ([Kapitel 14](14-lehrer.md)) |
| **Lerner / Learner** | der PPO-Teil, der aus Rollouts lernt |
| **Logik-Graph** | Blueprint-artiges Programm aus Knoten und Verbindungen; läuft im Interpreter, als JIT oder als exportiertes C++ |
| **LoopbackNetwork** | In-Process-Netz für Tests und Mehrspieler im Editor |
| **Micro Duck** | zweibeiniger Roboter von Pollen Robotics; in Kern mit seinen echten Netzen simuliert |
| **NetGame** | Mehrspieler über Kern Net: autoritativer Server, Snapshots, Interpolation |
| **NightWindows** | Shader-Erweiterung für beleuchtete Fenster in der Nacht |
| **ONNX** | Austauschformat der Netze; Kern liest und schreibt die rsl_rl-Actor-Form |
| **Outliner** | Baumansicht der Actors im Studio |
| **Package Game** | Export eines Levels als eigenständige HTML-Datei |
| **Play / Simulate** | die Laufmodi im Viewport (`game::SimMode::play` bzw. `simulate`): Play spielt das Level, Simulate lässt Physik und Regeln laufen, während der Editor bedienbar bleibt (Körper greifen, simulierte Posen mit K übernehmen) |
| **Player-only** | Start des Studio-Moduls ohne Editor, für exportierte Spiele (`kern_studio_init(…, 1)`) |
| **Policy** | trainiertes Netz, das Beobachtungen auf Aktionen abbildet |
| **PPO** | Proximal Policy Optimization, das Lernverfahren des Lehrers (nach rsl_rl) |
| **Reactor (WASI)** | WebAssembly-Modul ohne `main`: `_initialize`, dann Exporte |
| **Rezept** | Trainingseinstellungen je Aufgabe (`recipe.h`), nativ und im Browser gleich |
| **Rollout** | gesammelte Übergänge (Beobachtung, Aktion, Belohnung …) mehrerer Umgebungen |
| **rsl_rl** | Reinforcement-Learning-Bibliothek, mit der Pollen die Netze trainiert; Vorbild für Kerns PPO und Exportformat |
| **Servoprofil** | Parameter eines Servotyps (K, D, F, T, A, Totzeit, Streuungen) als JSON |
| **ShaderExtension** | GLSL-Schnipsel an festen Hook-Punkten des Standard-Shaders, mit dem Spiele eigene Looks hinzufügen ([Kapitel 4](04-grafik.md)) |
| **Span** | nicht besitzende Sicht auf zusammenhängende Elemente (`kern::Span`) |
| **Substeps** | Unterschritte des Box3D-Lösers je Weltschritt; für die Micro Duck 16 |
| **Tier (Logik)** | Ausführungsstufe eines Graphen: `interpreter`, `compiling`, `compiled (WebAssembly)`, `native C++` |
| **velstand** | Pollens Lauf- und Stehnetz der Micro Duck; in Kern eingebaut |
| **Viewmodel-Ebene** | Renderebene nach einem Depth-Clear, z. B. für Gizmos |
| **Vorbild** | Demonstration, die das Netz zuerst nachahmt (Sprung, Spielen) |
| **Walker** | Controller-Art für gelenkige Roboter, die eine Policy fährt |
| **wasi-sdk** | LLVM-Toolchain für WebAssembly mit wasi-libc und libc++; Kern nutzt Release 25 |
| **Welding** | verlustfreies Indexieren einer Dreieckssuppe |

---

© 2026 Christian Heinrich Hohlfeld · Alle Rechte vorbehalten · [Impressum und Rechte](00-impressum.md)
