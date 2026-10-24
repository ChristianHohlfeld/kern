# Kapitel 5 – Szene und Geometrie

Dieses Kapitel behandelt zwei Bausteine, die rund um den Renderer liegen:

- **`kern::scene`** (`include/kern/scene`, `src/scene`): Sichtbarkeitstest gegen das Kamerafrustum, eine Bounding Volume Hierarchy (BVH) und das Picking, also die Frage „welches Dreieck liegt unter dem Mauszeiger?“.
- **`kern::geo`** (`include/kern/geo`, `src/geo`): Erzeugen von Geometrie auf der CPU – Polygon-Triangulierung mit Earcut, Primitive, verlustfreies Welding, Gebäude aus Grundrissen, Straßenbänder, Terrain-Kacheln.

Danach folgt, wie die Spiel-Laufzeit daraus die prozedurale Stadt baut, und welche Meshes fest in die Engine einkompiliert sind.

Der Renderer selbst steht in [Kapitel 4](04-grafik.md). Die mathematischen Grundlagen (Matrizen, Koordinatenräume, Projektion) stehen in [Anhang B](B-grundlagen-rendering.md).

## 5.1 Konventionen

- **Z ist oben**, Einheiten sind Meter. Das gilt für Renderer, Physik, Audio und alle Geometriebauer.
- **`MeshData`** (`geo/mesh_data.h`) ist der gemeinsame Container aller Bauer: `positions`, `normals`, `colors`, `uvs` als flache `f32`-Vektoren, optional ein Zusatzstrom `extra` mit `extra_size` Floats pro Vertex und `indices`. Ohne Indizes ist das Mesh eine *Triangle Soup*: je drei aufeinanderfolgende Vertices sind ein Dreieck.
- **Farben in `MeshData` sind sRGB**, wie alle Vertexfarben, die der Standard-Shader bekommt.
- `MeshData::as_input(extra_slot)` liefert eine geliehene `MeshInput`-Sicht für `Renderer::upsert`. Der Zusatzstrom landet im Erweiterungs-Slot `extra_slot`; ein Wert ≥ `kMaxExtensionAttributes` (2) lässt ihn weg.

## 5.2 Frustum-Culling

### Was es leistet

Ein Mesh, das vollständig außerhalb des Sichtvolumens der Kamera liegt, muss nicht gezeichnet werden. Der Test dafür muss viel billiger sein als das Zeichnen selbst – sonst lohnt er nicht. Kern testet deshalb nicht die Dreiecke, sondern eine einzige Kugel pro Mesh gegen sechs Ebenen: sechs Skalarprodukte statt tausender Vertex-Transformationen und eines Drawcalls.

### Ebenen aus der Matrix (Gribb/Hartmann)

`Frustum::from_matrix(vp)` gewinnt die sechs Ebenen direkt aus der View-Projektionsmatrix. Ein Punkt `p` liegt im Clip-Raum innerhalb, wenn `-w ≤ x, y, z ≤ w` gilt. Jede dieser sechs Ungleichungen ist linear in `p` und damit eine Ebene: `zeile4 ± zeile_i`. Der Code liest die Zeilen aus der spaltenweise gespeicherten Matrix und normiert jede Ebene:

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

Normierte Ebenen sind nötig, damit `a*x + b*y + c*z + d` der echte Abstand in Metern ist und mit dem Kugelradius verglichen werden kann.

### Der Kugeltest

```cpp
bool sphere_visible(const Sphere& s) const {
  if (!(s.radius >= 0) || !finite(s.center)) return true;
  for (const Vec4& p : planes)
    if (p.x * s.center.x + p.y * s.center.y + p.z * s.center.z + p.w < -s.radius) return false;
  return true;
}
```

Der Test ist **konservativ**: Er gibt nur dann `false` zurück, wenn die Kugel sicher außerhalb einer Ebene liegt. Eine Kugel nahe einer Frustum-Ecke kann als sichtbar gelten, obwohl sie es nicht ist – dann wird sie unnötig gezeichnet, aber nie fälschlich weggelassen. Ungültige Kugeln gelten als sichtbar.

### Woher die Kugeln kommen

Der Renderer berechnet beim `upsert` zwei Kugeln (`src/gfx/mesh.cpp`):

- `local_bounds`: Mittelpunkt der Bounding Box der Positionen, Radius bis zum fernsten Vertex.
- `world_bounds`: Über alle Instanzen wird die lokale Kugel transformiert. Für den Radius nimmt der Code die Frobenius-Norm des 3×3-Teils der Instanzmatrix. Sie ist immer mindestens so groß wie die stärkste Streckung der Matrix, also eine sichere obere Schranke. Aus allen Instanzkugeln entsteht eine Box und daraus die Weltkugel.

Bei instanzierten Meshes umschließt `world_bounds` alle Instanzen gemeinsam. Ein Wald, dessen Bäume über die ganze Karte verteilt sind, ist deshalb fast immer „sichtbar“. Wer viele Instanzen über große Flächen verteilt, gewinnt durch Aufteilen in räumliche Gruppen mehr Culling.

Ausnahmen beim Culling: Geskinnte Meshes werden nie über ihre Bounds verworfen (eine Pose kann die Bind-Pose-Bounds verlassen), und Meshes mit `Material::frustum_culled = false` ebenfalls. Der Schattenpass benutzt denselben Test gegen das Frustum der Sonne.

## 5.3 BVH

### Wozu

Picking muss einen Strahl gegen Dreiecke testen. Eine Stadtkachel hat leicht zehntausende Dreiecke. Gegen jedes einzeln zu testen, kostet bei jedem Mausklick oder jedem Hover zehntausende Möller-Trumbore-Tests. Eine BVH ordnet die Dreiecke in eine Hierarchie von Achsen-ausgerichteten Boxen. Verfehlt der Strahl eine Box, sind alle Dreiecke darin auf einmal erledigt. Bei einem ausgewogenen Baum sinkt der Aufwand von linear auf ungefähr logarithmisch in der Dreieckszahl, plus die wenigen Dreiecke in den getroffenen Blättern.

### Aufbau

`Bvh::build(positions, vertex_count, indices, index_count, leaf_size = 8)` (`src/scene/bvh.cpp`):

1. Pro Dreieck eine Box und deren Mittelpunkt.
2. Ein Arbeitsstapel von Aufgaben `(Knoten, Anfang, Ende)` über ein Index-Array `order_`.
3. Pro Aufgabe: Box aller Dreiecke berechnen. Sind es höchstens `leaf_size` Dreiecke oder liegen alle Mittelpunkte aufeinander, wird der Knoten ein Blatt.
4. Sonst die Achse mit der größten Ausdehnung der Mittelpunkte wählen und mit `std::nth_element` am **Median** teilen. Die beiden Kinder liegen immer direkt hintereinander im Knoten-Array (`right = left + 1`).

`nth_element` partitioniert in linearer Zeit; über alle Ebenen ergibt das O(n log n). Der Mediansplit halbiert jede Ebene, die Tiefe ist also höchstens 32. Deshalb reicht beim Traversieren ein fester Stapel mit 64 Einträgen. Alles liegt in flachen Arrays, ohne Allokation pro Knoten.

### Traversierung

`traverse(ray, max_distance, visit)` arbeitet mit einem eigenen Stapel:

- Strahl gegen die Knotenbox testen (Slab-Test `intersect_aabb`, NaN-sicher, wenn der Strahl genau auf einer Box-Kante liegt).
- Im Blatt für jedes Dreieck `visit(triangle, max_distance)` aufrufen; `visit` gibt die neue Höchstdistanz zurück, also den bisher nächsten Treffer.
- Bei inneren Knoten beide Kinder testen und **das fernere zuerst auf den Stapel legen**, damit das nähere zuerst bearbeitet wird. Wird früh ein naher Treffer gefunden, schrumpft `max_distance`, und fernere Boxen fallen beim nächsten Slab-Test weg.

### Wann der Renderer eine BVH baut

Nur für statische Weltgeometrie: keine Linien, genau eine Instanz mit Einheitsmatrix, nicht geskinnt, mindestens 36 Dreiecke (`kBvhMinTriangles` in `renderer.cpp`). Wird ein solches Mesh später bewegt oder bekommt mehrere Instanzen, verwirft `update_instances` die BVH; wird es wieder statisch, baut es sie neu. Alles andere nutzt den exakten Brute-Force-Weg.

## 5.4 Picking

### Der Bildschirmstrahl

`screen_ray(camera, x, y, width, height, ray)` (`src/scene/picking.cpp`) invertiert die View-Projektionsmatrix (in `f64`), rechnet den Pixel in normalisierte Gerätekoordinaten um (`nx = x/width*2-1`, `ny = 1-y/height*2`) und projiziert ihn auf Nah- und Fernebene zurück. Die Richtung ist die normierte Differenz. Bei einer perspektivischen Kamera beginnt der Strahl im Auge mit `near`/`far` der Kamera; bei einer orthografischen Kamera beginnt er auf der Nahebene unter dem Cursor, und alle Strahlen sind parallel.

`Renderer::pick(x, y, query, hit)` ist `screen_ray` plus `pick_ray`. Die Koordinaten sind Pixel im Drawing Buffer.

### `pick_meshes`

Für jedes Mesh, das nicht ausgeschlossen ist (unsichtbar ohne `include_invisible`, `ignore_id`, Linien, falsche Ebenen, leer):

1. **Grobtest:** Strahl gegen die Weltkugel. Verfehlt er sie oder liegt sie hinter dem bisher besten Treffer, ist das Mesh erledigt (`bounds_rejected`). Geskinnte Meshes überspringen diesen Test.
2. **Feintest:**
   - mit BVH: Traversierung, Dreiecke in Weltkoordinaten,
   - ohne BVH: jede Instanz, jedes Dreieck, mit der Instanzmatrix transformiert. Bei geskinnten Meshes liefert `Mesh::posed_vertex` die aktuelle Pose aus den Knochenmatrizen. Was man sieht, kann man also auch anklicken, auch bei animierten Figuren.
3. **Dreieckstest:** Möller-Trumbore (`intersect_triangle` in `bvh.h`). Rückseiten zählen nur bei `double_sided`-Materialien.

Das Ergebnis `PickHit` enthält Mesh-Id, Instanz, Dreieck, Distanz, Trefferpunkt, Flächennormale und baryzentrische Koordinaten. `PickStats` zählt untersuchte Dreiecke und per Bounds verworfene Meshes.

```cpp
PickHit hit;
if (engine.renderer.pick(in.pointer().x, in.pointer().y, {}, hit) && hit.mesh == boxes_) {
  // examples/hello: die angeklickte Kiste rot färben
  tints_[hit.instance * 4 + 0] = 1.0f; tints_[hit.instance * 4 + 1] = 0.25f; tints_[hit.instance * 4 + 2] = 0.2f;
}
```

`PickQuery` hat `max_distance` (1800), `ignore_id`, `include_invisible` und `layers`. Kern Studio pickt nur die Ebene `layer::scene`, bildet Mesh-Ids mit `GameWorld::actor_for_mesh` auf Actors ab und setzt den Strahl mit `ignore_id` hinter dem Boden oder gesperrten Actors fort. Actors ohne Mesh (Lichter, Empties) wählt es über den Bildschirmabstand ihres Icons (`Studio::pick` in `src/studio/viewport.cpp`).

## 5.5 Earcut: Polygone triangulieren

### Wofür

Ein Gebäudegrundriss ist ein Polygon, oft nicht konvex (L-Form) und manchmal mit Löchern (Innenhöfe). Für Dach und Boden braucht der Renderer Dreiecke. `kern::Earcut` (`geo/earcut.h`, `src/geo/earcut.cpp`) ist eine C++-Portierung von Mapbox Earcut v3.0.2 (ISC-Lizenz, siehe `LICENSES.md`). Der Port folgt der JavaScript-Vorlage und liefert laut Kopfkommentar dieselben Ergebnisse. Der Paritätstest gegen die JS-Version, den `kern/README.md` nennt (`tests/kern_geo_parity_test.mjs`), liegt im SpawnCity-Repository, nicht in diesem; hier prüft `earcut_shapes` Flächentreue und Dreieckszahlen.

### Schnittstelle

```cpp
Earcut earcut;
std::vector<u32> tris;
// xy: äußerer Ring, dann die Löcher; hole_starts: Vertexindex, an dem jedes Loch beginnt
earcut.triangulate(xy, vertex_count, hole_starts, hole_count, tris);
f64 err = Earcut::deviation(xy, vertex_count, hole_starts, hole_count, tris.data(), tris.size());
```

Koordinaten sind `f64`. Die Knoten liegen in Blöcken zu 512, die zwischen Aufrufen wiederverwendet werden; ein `Earcut`-Objekt pro Bauer spart Allokationen. `deviation` vergleicht die Polygonfläche (äußerer Ring minus Löcher) mit der Summe der Dreiecksflächen; 0 bedeutet exakt.

### Wie Ear Clipping funktioniert

Ein einfaches Polygon mit n Ecken hat immer mindestens zwei „Ohren“: Ecken, deren Dreieck aus Vorgänger, Ecke und Nachfolger ganz im Polygon liegt. Schneidet man ein Ohr ab, bleibt ein Polygon mit n−1 Ecken. Wiederholt man das, entstehen n−2 Dreiecke.

Earcut führt das Polygon als doppelt verkettete Ringliste. `is_ear(ear)` prüft für Ecke b mit Nachbarn a und c:

1. Ist die Ecke konvex? (`area(a, b, c) < 0` in Earcuts Orientierung.) Eine reflexe Ecke kann kein Ohr sein.
2. Liegt eine andere Polygonecke im Dreieck abc? Getestet werden nur Ecken in der Bounding Box des Dreiecks und nur reflexe Ecken (eine konvexe Ecke im Dreieck setzt eine reflexe voraus). Liegt eine drin, ist b kein Ohr.

`earcut_linked` läuft im Kreis über die Liste, schneidet jedes gefundene Ohr ab und fährt beim Nachfolger fort.

### Die Sonderfälle

Ein Polygon aus echten Daten (etwa OpenStreetMap) ist selten sauber. Earcut hat dafür eine Kette von Rückfallstufen. Findet ein Durchlauf einmal rundherum kein Ohr mehr:

- **Pass 0 → 1:** `filter_points` entfernt doppelte Punkte und kollineare Ecken, dann ein neuer Versuch.
- **Pass 1 → 2:** `cure_local_intersections` repariert lokale Selbstüberschneidungen (zwei sich kreuzende Nachbarkanten) und gibt dabei direkt ein Dreieck aus.
- **Pass 2:** `split_earcut` sucht eine gültige Diagonale, teilt das Polygon in zwei und trianguliert beide Hälften getrennt.

### Löcher

Earcut kann Ohren nur in einem einzigen Ring abschneiden. Löcher werden deshalb vorher in den äußeren Ring eingeflochten (`eliminate_holes`): Für jedes Loch wird sein am weitesten links liegender Punkt gesucht; die Löcher werden nach diesem x sortiert. `find_hole_bridge` sucht nach dem Verfahren von Eberly eine sichtbare Ecke des äußeren Rings links davon. Entlang dieser „Brücke“ wird das Loch eingeschnitten: zwei Kanten hin und zurück, mit duplizierten Endpunkten (`split_polygon`). Danach ist alles ein einziger, wenn auch an der Brücke sich berührender Ring.

### z-Order-Hashing für große Polygone

Der Test „liegt eine andere Ecke im Dreieck?“ ist für jede Ecke linear in n; das Ganze wird quadratisch. Bei mehr als 80 Vertices schaltet Earcut auf `is_ear_hashed` um:

- Alle Koordinaten werden auf ein Raster von 0 bis 32767 skaliert.
- Jeder Knoten bekommt einen **z-Order-Wert** (Morton-Code): Die Bits von x und y werden verschränkt. Punkte, die im Raum nahe beieinander liegen, haben meist nahe z-Werte.
- Die Knoten werden zusätzlich in einer nach z sortierten Liste verkettet (`prev_z`, `next_z`).
- Für ein Ohr-Kandidaten-Dreieck berechnet Earcut den z-Bereich seiner Bounding Box und läuft nur durch die Knoten in diesem z-Bereich, von der Ecke aus in beide Richtungen.

So werden statt aller Ecken nur die räumlich nahen geprüft.

### Wie die Bauer Earcut absichern

`BuildingBuilder` ruft Earcut nur mit gereinigten Ringen auf und prüft danach `deviation`. Weicht die Dreiecksfläche um mehr als 0,7 % von der Polygonfläche ab, gilt das Gebäude als defekt und wird übersprungen (`skipped()`), statt mit falschem Dach in der Stadt zu stehen.

## 5.6 Primitive

`kern::primitives` (`geo/primitives.h`) erzeugt indizierte Meshes, Z oben, um den Ursprung zentriert, sofern nicht anders angegeben:

| Funktion | Form | Details |
|---|---|---|
| `box(size, color)` | Quader | 6 Quads mit flachen Normalen, 24 Vertices, UV 0..1 pro Seite |
| `box_on_ground(size, color)` | Quader | wie `box`, Boden bei z = 0 |
| `sphere(radius, segments = 24, rings = 16, color)` | UV-Kugel | glatte Normalen |
| `capsule(radius, length, segments = 16, rings = 8, color)` | Kapsel entlang Z | Zylinder der Länge `length` zwischen zwei Halbkugeln |
| `torus(major, minor, segments = 24, tube_segments = 8, color)` | Torus um Z | |
| `cylinder(radius, height, segments = 20, color)` | Zylinder mit Deckeln | steht auf z = 0 |
| `cone(radius, height, segments = 16, color)` | Kegel mit Boden | steht auf z = 0 |
| `plane(width, depth, color, uv_scale = 1)` | Rechteck, Normale +Z | UVs in Metern × `uv_scale` |
| `append(dst, src, m)` | – | hängt `src` mit Matrix `m` an `dst` an |

`append` transformiert Positionen mit `m` und Normalen mit der Normalenmatrix von `m`. Damit lassen sich statische Teile zu einem Mesh zusammenführen – Orb Run baut so sein Auto aus drei Quadern und alle Bodenplatten der Stadt als ein Mesh. Den Zusatzstrom `extra` kopiert `append` nicht mit.

## 5.7 Welding und Inhalts-Hash

### Warum Welding

Die Gebäude- und Straßenbauer erzeugen Triangle Soups: Jedes Dreieck hat seine eigenen drei Vertices. Das ist einfach zu schreiben, aber Ecken, an denen mehrere Dreiecke zusammenstoßen, stehen mehrfach im Speicher. Ein indiziertes Mesh speichert jeden eindeutigen Vertex einmal und verweist mit Indizes darauf. Das spart Speicher und Upload-Zeit, und der Grafikprozessor kann bereits transformierte Vertices wiederverwenden.

### `weld(mesh, min_saving)`

`weld` (`src/geo/mesh_data.cpp`) verschmilzt Vertices, deren Position, Normale, Farbe, UV und Zusatzstrom **bitgleich** sind – verglichen werden die IEEE-754-Bitmuster, nie gerundete Werte. Dadurch ist das Verfahren verlustfrei: Eine harte Kante (zwei Flächen mit verschiedenen Normalen an derselben Position) bleibt hart, weil die Normalen sich unterscheiden.

Ablauf:

1. Für jeden Vertex eine Schlüsselzeile aus 11 bis 15 `u32`-Wörtern (3 Position, 3 Normale, 3 Farbe, 2 UV, bis zu 4 Zusatzwerte).
2. Eine Hashtabelle mit offener Adressierung (Kapazität: nächste Zweierpotenz ≥ 2n), Hash FNV-1a über die Wörter, nachgemischt mit einem 64-Bit-Finalizer. Kollisionen werden mit linearer Sondierung aufgelöst, Gleichheit mit `memcmp` der ganzen Zeile geprüft.
3. Jeder Vertex bekommt die Nummer seines ersten Vorkommens (`remap`).
4. **Nur wenn es sich lohnt:** Der Code rechnet die Bytes danach aus (eindeutige Vertices plus Indizes zu 2 oder 4 Byte) und indiziert nur, wenn die Ersparnis größer als `min_saving` ist. Sonst bleibt die Soup unverändert.

Voraussetzung: Das Mesh ist eine Soup (keine Indizes) und der Zusatzstrom hat höchstens 4 Floats. `WeldStats` meldet Vertices vorher und nachher, Bytes vorher und nachher und ob indiziert wurde.

Der README nennt eine Messung auf einem echten Konstanz-Snapshot mit 1300 Gebäuden: Bau und Welding brauchen in Kern etwa 45 ms, die vorherige JavaScript-Variante etwa 1260 ms. Ein Grund ist, dass `weld` Bitmuster in einer Hashtabelle vergleicht statt Zeichenketten als Schlüssel zu bauen.

### `content_hash(mesh)`

FNV-1a (32 Bit) über die Bitmuster aller Ströme und Indizes. Damit erkennt ein Host, ob ein neu gebauter Chunk sich überhaupt vom alten unterscheidet. Die C-ABI speichert ihn im `GeoChunk::hash`.

## 5.8 Gebäude aus Grundrissen

`BuildingBuilder` (`geo/buildings.h`, `src/geo/buildings.cpp`) extrudiert 2D-Grundrisse zu Gebäuden: Dach (über Earcut), Wände und optional ein Gesims. Er arbeitet **inkrementell unter einem Zeitbudget** und gruppiert das Ergebnis in quadratische Chunks.

### Der Eingabestrom

Grundrisse kommen als flacher `f64`-Strom, damit JavaScript-Hosts sie ohne Objektgraph übergeben können:

```
count,
dann pro Gebäude:
  seed_hash, base, top, has_center, center_x, center_y, ring_count,
  { vertex_count, x0, y0, x1, y1, ... } × ring_count        (erster Ring = außen)
```

In C++ schreibt `FootprintStream` diesen Strom:

```cpp
FootprintStream footprints;
footprints.begin(seed, 0, height);        // Gebäude beginnen: Seed, Fuß, Traufe
footprints.ring(outer, 4);                // äußerer Ring
footprints.ring(court, 4);                // Loch (Innenhof)
```

`seed_hash` (u32) steuert Farben und Fassadenvariation. Ohne `has_center` bestimmt der erste Punkt des äußeren Rings, in welchen Chunk das Gebäude fällt.

### Was pro Gebäude passiert

1. **Ringe reinigen** (`clean_ring`): nicht-endliche Punkte und Punkte unter 1,5 cm Abstand zum Vorgänger fallen weg, ebenso der schließende Duplikatpunkt. Bleiben weniger als 3 Punkte oder eine Fläche ≤ 0,08 m², wird der Ring leer.
2. **Orientierung vereinheitlichen:** äußerer Ring gegen den Uhrzeigersinn, Löcher im Uhrzeigersinn (`ring_area`, nötigenfalls umdrehen).
3. **Verwerfen**, wenn ein Ring leer ist, die Wandhöhe ≤ 0,1 m ist, Earcut nichts liefert oder die Flächenabweichung über 0,7 % liegt.
4. **Farben** aus dem Stil: Wandfarbe `walls[h % n]`, Dachfarbe `roofs[(h >> 8) % n]`, Gesimsfarbe als Mischung `mix(Wand, trim, trim_mix)` in `f64`, damit sie exakt einer Referenz in doppelter Genauigkeit entspricht. Die Stockwerkshöhe liegt zwischen 3,1 und 3,6 m, abhängig vom Seed.
5. **Dach:** jedes Earcut-Dreieck auf Höhe `top`, Normale +Z; der Umlaufsinn wird pro Dreieck so gedreht, dass die Fläche nach oben zeigt. Innenhöfe bleiben offen.
6. **Wände:** für jede Kante jedes Rings (auch der Hofringe) zwei Dreiecke von `base` bis `top`, Normale nach außen. UVs: u = Meter entlang der Wand, v = Meter über dem Fuß.
7. **Gesims** (wenn `cornice`, Kante > 1,2 m, Wandhöhe > 3,5 m): ein 0,55 m hohes, 0,24 m vorstehendes Band an der Traufe mit Front, Ober- und Unterseite.

### Der Wand-Datensatz

Ist `wall_records` an (Standard), trägt jeder Vertex vier Zusatzwerte in `MeshData::extra`:

| Wert | Bedeutung |
|---|---|
| x | Wandlänge in Metern; −1 = Dach; −2 = Gesims |
| y | Wandhöhe (`top − base`) |
| z | Gebäude-Seed in [0, 1) (`(h % 997) / 997`) |
| w | Stockwerkshöhe |

Diese Werte sind für Fassaden-Shader gedacht. Die Shader-Erweiterung `NightWindows` aus [Kapitel 4](04-grafik.md#413-beispiel-eine-eigene-shader-erweiterung-orb-run) liest sie als Attribut `aWall` und zeichnet daraus Fensterraster.

### Chunks und Zeitbudget

`chunk_for(x, y)` ordnet das Gebäude dem Chunk `floor(x / chunk_size), floor(y / chunk_size)` zu; jeder Chunk ist ein eigenes `MeshData` und wird später ein eigenes Mesh. Größere Chunks bedeuten weniger Drawcalls, kleinere besseres Frustum-Culling.

```cpp
BuildingBuilder b;
b.begin(stream, length, options);
while (!b.step(4.0)) { /* nächster Frame */ }    // höchstens ~4 ms pro Aufruf
for (GeoChunk& c : b.chunks()) { weld(c.mesh); /* hochladen */ }
```

`step(budget_ms)` baut Gebäude, bis das Budget aufgebraucht ist, und gibt `true` zurück, wenn alles fertig ist. Ein Host kann so eine große Stadt über mehrere Frames verteilen, ohne dass ein Frame hängt. Orb Run und die Laufzeit bauen ihre kleine Stadt mit `step(1e9)` in einem Zug.

`BuildingOptions`: `chunk_size` (320, mindestens 20), `max_buildings` (2600), `cornice` (an), `wall_records` (an), `style` (`BuildingStyle` mit Listen von Wand- und Dachfarben, `trim`, `trim_mix`). Zähler: `processed()`, `built()`, `skipped()`, `triangles()`.

## 5.9 Straßen

`RoadBuilder` (`geo/roads.h`, `src/geo/roads.cpp`) legt Straßenbänder auf das Gelände: optional ein Bordsteinband, die Fahrbahn und eine gestrichelte Mittellinie, ebenfalls in Chunks und unter Zeitbudget.

### Eingabe

```
count,
dann pro Straße:
  width, flags (1 = Bordstein, 2 = Markierung), surface_color, marking_color,
  point_count, x0, y0, x1, y1, ...
```

`surface_color` und `marking_color` sind Indizes in `RoadOptions::palette`. In C++ schreibt `RoadStream::road(points, count, width, curb, surface_color, marking_color)` den Strom; `marking_color = -1` bedeutet keine Markierung.

### Bänder

Für jedes Polylinien-Segment erzeugt `band()` ein Rechteck der gewünschten Breite. Damit das Band einem hügeligen Gelände folgt, wird es in Stücke von höchstens `segment_length` (6 m) geteilt; jeder Vertex bekommt seine Höhe aus dem `HeightSampler` plus einen kleinen Versatz:

| Ebene | Höhe über Gelände | Breite |
|---|---|---|
| Bordstein | `curb_z` = 0,09 m | `width + curb_extra` (3,4 m) |
| Fahrbahn | `surface_z` = 0,112 m | `width` |
| Markierung | `marking_z` = 0,135 m | `marking_width` = 0,42 m |

Die gestaffelten Höhen verhindern Z-Fighting zwischen den übereinanderliegenden Bändern. Die Markierung besteht aus Strichen der Länge `dash_length` (3 m) im Abstand `dash_period` (7 m) ab `dash_start` (1 m). Segmente, die weiter als `radius` (plus Rand) von `center_x/center_y` entfernt sind, werden übersprungen; so lässt sich ein Ausschnitt einer großen Karte bauen. UVs sind Weltkoordinaten × `uv_scale`.

### `HeightSampler`

```cpp
struct HeightSampler {
  f64 (*fn)(void* user, f64 x, f64 y) = nullptr;
  void* user = nullptr;
  f64 operator()(f64 x, f64 y) const { return fn ? fn(user, x, y) : 0.0; }
};
```

Ein leerer Sampler liefert 0, also flachen Boden. Der Kommentar nennt den Sampler „physics truth“: Das Spiel soll dieselbe Höhenfunktion liefern, die auch seine Physik benutzt, damit Fahrzeuge genau auf der sichtbaren Straße fahren. Liefert der Sampler einen nicht-endlichen Wert, bricht der Bauer mit Fehler ab (`failed()`). In JavaScript setzt man `kern.heightSampler = (x, y) => ...`; die Engine ruft ihn über den Import `kern_env.terrain_height`.

## 5.10 Terrain-Kacheln

```cpp
bool build_terrain_tile(f64 x0, f64 y0, f64 step, u32 cells, Rgb albedo, HeightSampler heights,
                        MeshData& out, f64* min_height = nullptr, f64* max_height = nullptr);
```

Baut ein regelmäßiges Gitter von `cells × cells` Quads (1 bis 128) mit Kantenlänge `step`, ein Vertex pro Höhenknoten, als indiziertes Mesh. Jedes Quad wird entlang der Diagonale Südwest–Nordost geteilt.

Die Normalen entstehen aus zentralen Differenzen der Nachbarknoten. Dafür tastet die Funktion ein Gitter mit einem Knoten Rand ab, damit auch Randvertices korrekte Normalen haben und benachbarte Kacheln nahtlos aneinanderpassen:

```
dx = h(i+1, j) − h(i−1, j),   dy = h(i, j+1) − h(i, j−1)
n  = normalize(−dx, −dy, 2·step)
```

Die UVs laufen über die Kachel von 0 bis 1; `min_height`/`max_height` geben den Höhenbereich zurück (nützlich für Bounds oder LOD-Entscheidungen).

`GeoProjector` rechnet WGS84-Längen- und Breitengrade in Meter östlich und nördlich eines Ursprungs um (lokale Tangentialebene, Erdradius 6 378 137 m). So lassen sich Grundrisse aus Kartendaten direkt in Kern-Koordinaten bringen.

## 5.11 Geometrie aus JavaScript

Die Bauer sind auch über die C-ABI erreichbar (`src/api/geo_api.cpp`) und in `KernRenderer` gekapselt:

```js
const job = renderer.beginBuildings(footprintStream /* Float64Array */, options /* Float64Array */);
await job.run({ yieldFrame: () => new Promise(requestAnimationFrame), budgetMs: 4 });
const { chunks } = job.stats();              // processed, built, skipped, triangles, chunks
for (let i = 0; i < chunks; i++) job.commit(i, "buildings-" + i, { material, extraKey: "wall" });
job.free();
```

`extraKey` benennt den Erweiterungs-Strom, der den Datensatz des Bauers bekommt; ohne `extraKey` ist es der erste. Einen Namen, den der Renderer nicht als Strom kennt, lehnt `commit` mit einem Fehler ab (`KernGeoJob.commit: no extension stream "…"`), statt still einen anderen Strom zu nehmen. `kern_geo_commit` weldet den Chunk, lädt ihn als Mesh hoch und gibt den Chunk-Speicher sofort frei, weil der Renderer eine eigene Kopie hält. Er meldet die Bytes vor und nach dem Welding. `renderer.upsertTerrainTile(id, {x0, y0, step, cells, albedo, material})` baut und lädt eine Terrain-Kachel in einem Aufruf. Ein `MeshInput`-Deskriptor kann außerdem das Flag `KERN_MF_WELD` tragen (`input.weld === true` in der Fassade), dann weldet die Engine eine übergebene Soup vor dem Hochladen – sofern sie eine reine Dreiecks-Soup mit Normalen ist (keine Indizes, kein Skinning, keine Linien, höchstens ein Erweiterungs-Strom mit 1–4 Floats); andernfalls wird sie unverändert hochgeladen.

## 5.12 Die prozedurale Stadt der Spiel-Laufzeit

Die Spiel-Laufzeit hat eine Komponente **Procedural City** (`CityComponent` in `include/kern/game/level.h`). Sie ist der Stadtgenerator von Orb Run als Level-Komponente: Im Studio legt man sie über den Katalogeintrag „Procedural City“ (Gruppe „World“ in `src/studio/catalog.cpp`) in ein Level, und sie erzeugt Gebäude, Straßen, Bodenplatten, Bäume, Kollisionskörper und Verkehr.

![Ein exportiertes Spiel: die prozedurale Stadt mit Gebäuden, Straßen und Bäumen](bilder/game.png)

### Parameter

```cpp
struct CityComponent {
  u32 seed = 20261022;
  i32 blocks = 7;               // Blöcke pro Seite
  f32 pitch = 80;               // Meter von Straße zu Straße
  f32 street = 16;              // Straßenbreite
  f32 max_height = 84;          // höchstes Gebäude in der Mitte
  bool trees = true;
  i32 traffic = 22;             // Autos auf den Fahrspuren
  bool night_windows = true;    // beleuchtete Fenster nachts (Shader-Erweiterung)
};
```

`build_city` lehnt ungültige Werte ab: `blocks` 1–16, `pitch` ≥ 30, `street` ≥ 6 und höchstens die halbe `pitch`.

### Der Generator

`build_city(city, out)` in `src/game/geometry.cpp` ist deterministisch für einen Seed. Der Zufallsgenerator `game::Rng` ist ein xorshift32 – laut Kommentar dieselbe Folge wie in Orb Run.

1. **Blockraster.** `blocks × blocks` Blöcke mit Mittenabstand `pitch`, dazwischen Straßen der Breite `street`. Jeder Block bekommt eine 18 cm hohe Bodenplatte (alle Platten zusammen ein Mesh über `primitives::append`) und einen Kollisionsquader dafür.
2. **Höhe nach Zentrumsnähe.** `centrality = 1 − clamp(|Mitte| / (half · 1,2), 0, 1)`, Höchstwert `14 + centrality² · (max_height − 14)`. Die Mitte wird so zur Hochhausgegend, der Rand bleibt niedrig.
3. **Bebauungsart pro Block** (gleichverteilt aus vier Fällen, Grundstück 3 m eingerückt):
   - ein blockgroßes Gebäude, in 60 % der Fälle mit 14 m tiefem Innenhof (ein Loch im Grundriss, Kollision als vier Flügel),
   - vier Einzelgebäude mit 5 m Gassen,
   - ein L-förmiges Gebäude plus ein kleineres in der freien Ecke,
   - drei Riegel.
4. **Straßen.** Jede Blockkante ist eine Straßenmittellinie, 60 m über die Stadt hinaus verlängert, Breite `street · 0,75`, mit Bordstein und Mittellinie.
5. **Bauen.** `BuildingBuilder` mit `chunk_size = 200`, sechs Wand- und drei Dachfarben; `RoadBuilder` auf flachem Boden mit `chunk_size = 200`. Alle Chunks werden geweldet.
6. **Bäume** (wenn `trees`): entlang der Blockränder alle 11 m, mit 35 % Lücken, zufälliger Größe, Drehung und Grünton. Stämme sind Zylinder, Kronen Kegel; beide als je **ein** instanziertes Mesh.

Das Ergebnis `CityGeometry` enthält die Gebäude-Chunks (mit Wand-Datensätzen), Straßen-Chunks, Bodenplatten, Baum-Meshes mit Instanzmatrizen und Tints, die Kollisionsquader (`CityBox`), die halbe Ausdehnung und die Zahl der Gebäude.

### Darstellung

`GameWorld::sync_city` (`src/game/runtime.cpp`) lädt die Geometrie hoch:

- Jeder Chunk wird ein Mesh mit der Weltmatrix des City-Actors als einziger Instanz. Verschiebt man den Actor, ändert sich nur diese Matrix (`update_instances`), nichts wird neu gebaut.
- Gebäude bekommen den Wand-Datensatz in Erweiterungs-Slot 0, wenn `night_windows` an ist; sonst keinen Strom.
- Straßen: kein Schatten, zweiseitig, Rauheit 1. Bodenplatten: Rauheit 0,95.
- Bäume: Instanzmatrizen = Stadtmatrix × lokale Baummatrix.
- Ein Inhalts-Schlüssel (FNV über das JSON der Komponente) entscheidet, ob neu gebaut werden muss. Nur eine echte Parameteränderung löst den Neubau aus.

Liegt die Stadt im Ursprung ohne Drehung, ist die Instanzmatrix die Einheitsmatrix, und der Renderer baut für jeden Chunk eine BVH. Picking in der Stadt (etwa im Studio) läuft dann über die Hierarchie.

### Physik und Verkehr

`plan_bodies` macht aus jeder Stadt einen statischen Box3D-Körper mit einem Quader pro Gebäudeflügel und Bodenplatte (Reibung 0,5); siehe [Kapitel 6](06-physik.md). Der Verkehr (`plan_traffic`, Seed `city.seed ^ 0x7a3f1c9b`) setzt `traffic` Autos auf zufällige Straßenlinien und Fahrtrichtungen, mit 9–15 m/s und einer von fünf Farben. Im Editor stehen sie geparkt; in Play und Simulate sind es kinematische Box3D-Körper, die ihre Spur entlangfahren, am Stadtrand auf die andere Seite springen und anhalten, wenn der Spieler weniger als 10 m vor ihnen auf der Spur steht. Alle Autos sind ein einziges instanziertes Mesh.

`city_street_point(city, rng)` liefert einen zufälligen Punkt auf einer Straßenlinie, etwa zum Verteilen von Orbs.

## 5.13 Formen der Laufzeit und eingebaute Meshes

### `shape_mesh`

Ein Static Mesh im Level (`MeshComponent`) hat eine Form: `box`, `sphere`, `cylinder`, `cone`, `capsule`, `plane`, `torus`, `ramp` oder `mesh`. `shape_mesh(mc)` erzeugt dafür ein Mesh, zentriert auf den Actor-Ursprung, mit weißen Vertexfarben (das Material färbt es). Quader, Ebene und Rampe bekommen UVs in Metern × `tiling`, damit eine Textur auf großen und kleinen Flächen gleich groß erscheint; die runden Formen kommen aus `primitives` mit skalierten UVs, Zylinder und Kegel um die halbe Höhe nach unten verschoben. Die Rampe ist ein Keil, an +X voll hoch, an −X null.

Daneben gibt es `shape_bounds`, `shape_hull_points` (Punkte für konvexe Hüllen bei Zylinder, Kegel, Rampe, Torus) und `fit_collider`, das zur Form einen passenden Collider wählt (Box, Kugel, Kapsel, sonst Hülle). Das ist Thema von [Kapitel 6](06-physik.md).

### Eingebaute Meshes

Die Form `mesh` verweist mit `MeshComponent::asset` auf ein Dreiecksmesh, das in die Engine einkompiliert ist (`include/kern/game/builtin_meshes.h`). Heute sind das die Glieder der Micro Duck: Pollen Robotics' Meshes, vereinfacht von `tools/micro_duck_import.py` (`third_party/microduck/micro_duck_meshes.inc`, CC BY-NC-SA 4.0). Die Namen beginnen mit `duck/`, zum Beispiel `duck/trunk_base`, `duck/neck`, `duck/upper_leg_left`, `duck/ankle_right` oder `duck/roller/passive_LF_wheel`; `builtin_mesh_names()` liefert die vollständige Liste.

Gespeichert sind sie kompakt:

- Positionen als 16-Bit-Werte über die Bounding Box (`lo`, `hi`) quantisiert,
- Normalen als 8-Bit-Werte × 127 (nach dem Dekodieren neu normiert),
- sRGB-Farben als 8 Bit,
- 16-Bit-Indizes,
- dazu die Punkte der konvexen Hülle für Kollisionskörper.

`builtin_mesh(name)` dekodiert beim ersten Zugriff und behält das Ergebnis für die Laufzeit des Programms. Nativ schützt ein Mutex den Cache, weil der Lehrer Welten auf mehreren Threads gleichzeitig baut ([Kapitel 14](14-lehrer.md)); WebAssembly läuft auf einem Thread, dort gibt es keinen Mutex (`#if !defined(__wasi__)`). `shape_mesh` skaliert ein eingebautes Mesh pro Achse mit `size` (Normalen mit dem Kehrwert) und dreht bei negativer Gesamtskalierung den Umlaufsinn um. Ein unbekannter Name ergibt einen 10-cm-Würfel, damit der Fehler im Level sichtbar ist und behoben werden kann.

Wie die Ente aus diesen Teilen zusammengesetzt wird, beschreibt [Kapitel 13](13-micro-duck.md).

---

© 2026 Christian Heinrich Hohlfeld · Alle Rechte vorbehalten · [Impressum und Rechte](00-impressum.md)
