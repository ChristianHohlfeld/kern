# 10 Netzwerk und Mehrspieler

Kern bringt einen eigenen Netzwerk-Stack mit, in C++ im Engine-Modul (`include/kern/net`,
`src/net`). Er folgt den Artikeln von Glenn Fiedler (Gaffer On Games) und dem Aufbau von yojimbo,
reliable.io und netcode.io. Darauf setzt `game::NetGame` (`include/kern/game/net_game.h`,
`src/game/net_game.cpp`) auf: Es macht **aus jedem Kern-Level ein Mehrspieler-Spiel**, ohne dass das
Level dafür vorbereitet sein muss.

Der Stack braucht von der Transportschicht nur eines: Datagramme verschicken und empfangen. Er läuft
deshalb überall:

- im Browser über einen **BroadcastChannel** (Tabs auf einem Rechner) oder einen
  **WebSocket-Relay** (verschiedene Rechner),
- nativ und in Tests über ein **In-Process-Netz** mit Netzwerksimulator,
- im Studio für „Number of Players“ über dasselbe In-Process-Netz.

---

## 10.1 Die Schichten

| Schicht | Datei | Aufgabe |
|---|---|---|
| Serialisierung | `serialize.h` | Bitpacker (32-Bit-Wörter, Little Endian), **eine** `serialize()`-Funktion je Typ für Lesen, Schreiben und Messen; Integer im Wertebereich, Bools, Floats, quantisierte Floats und Vektoren, Quaternionen „smallest three“, Strings, relative Sequenznummern. Leser prüfen alles. |
| Zuverlässigkeit | `reliable.h` | Jede Sendung trägt Sequenz, Ack und 32 Ack-Bits (komprimierter Header). Liefert Acks, RTT, Paketverlust und Bandbreite; Pakete über 1 KB werden fragmentiert und wieder zusammengesetzt. |
| Kanäle | `connection.h` | **Reliable ordered** (genau einmal, in Reihenfolge, Neuversand, große Nachrichten als Blöcke) und **unreliable unordered** (einmal, im nächsten Paket). Nachrichten sind typisierte Bytefolgen. |
| Verbindung | `client_server.h` | Handshake mit Client-Salt, Challenge-Salt und XOR-Verbindungs-ID, Slots, Keep-Alive, Timeouts, Disconnect, „Server voll“, CRC32 über Protokoll-ID und Inhalt. |
| Transport | `transport.h` | `LoopbackNetwork` (In-Process), `HostTransport` (Datagramme mit der Browser-Seite), `Simulator` für Latenz, Jitter, Verlust und Duplikate mit festem Seed. |
| Spiel | `kern/game/net_game.h` | Autoritativer Server, Snapshot-Interpolation, Eingaben, Ereignisse. |

---

## 10.2 Serialisierung

Nach „Reading and Writing Packets“ und „Serialization Strategies“: **Eine** Funktion je Typ
beschreibt das Format für Lesen, Schreiben und Messen.

```cpp
struct WelcomeMsg {                 // aus src/game/net_game.cpp
  u32 checksum = 0;
  i32 slot = 0;
  f32 tick_hz = 30;
  template <class S>
  bool serialize(S& s) {
    return net::serialize_u32(s, checksum) && net::serialize_int(s, slot, 0, 254) &&
           net::serialize_float(s, tick_hz);
  }
};
```

Drei Stream-Klassen setzen dieselbe Schnittstelle um: `WriteStream`, `ReadStream` und
`MeasureStream` (zählt Bits ohne zu schreiben, ungünstigster Fall bei Ausrichtungen). Jede hat
`kWriting`, `kReading`, `kMeasuring` als `constexpr`, damit eine `serialize`-Funktion z. B. beim
Lesen einen Vektor vorher vergrößern kann (`if constexpr (S::kReading) …`).

| Funktion | Kodierung |
|---|---|
| `serialize_int(s, v, min, max)` | genau so viele Bits wie der Bereich braucht (`bits_required`) |
| `serialize_bits(s, v, bits)` | feste Bitzahl |
| `serialize_bool` | 1 Bit |
| `serialize_u16`, `serialize_u32`, `serialize_u64` | 16, 32, 64 Bit |
| `serialize_float` | 32 Bit |
| `serialize_compressed_float(s, v, min, max, res)` | quantisiert auf `res` im Bereich |
| `serialize_vec3`, `serialize_compressed_vec3(s, v, bound, res)` | Vektor voll bzw. quantisiert in ±`bound` |
| `serialize_quat(s, q, bits)` | „smallest three“: die größte Komponente wird weggelassen, die übrigen drei mit `bits` Bits (Standard 10) |
| `serialize_bytes`, `serialize_string(s, str, max)` | Bytes, String mit Höchstlänge |
| `serialize_sequence_relative(s, base, v)` | Sequenznummer relativ zu einer Basis |

**Leser prüfen alles:** Werte außerhalb des Bereichs, überlaufende Puffer und falsche Längen lassen
`serialize` `false` liefern. Ein bösartiges oder kaputtes Paket wird verworfen, statt abzustürzen.
Wörter sind auf jeder Plattform Little Endian. `crc32(data, bytes, crc)` liegt ebenfalls hier.

---

## 10.3 Zuverlässigkeit: Endpoint

`Endpoint` (`reliable.h`, nach „Reliability and Congestion Avoidance over UDP“ und „Packet
Fragmentation and Reassembly“; reliable.io) liegt über einem unzuverlässigen Datagramm-Transport:

- Jedes Paket trägt seine **16-Bit-Sequenz** und die Sicht des Empfängers auf die Gegenseite: die
  zuletzt empfangene Sequenz (**Ack**) und **32 Bits** für die 32 davor.
- Der Header ist **komprimiert** (höchstens 9 Byte): ein Präfix-Byte, die Sequenz, das Ack als
  1-Byte-Abstand zur Sequenz, wenn er ≤ 255 ist (sonst 2 Byte), und von den vier Ack-Bit-Bytes nur
  die, die nicht `0xFF` sind.
- **Senden wartet nie.** Was neu gesendet wird, wenn das Ack eines Pakets ausbleibt, entscheidet die
  Schicht darüber (die Kanäle).
- Pakete über `fragment_above` werden in Fragmente zerlegt und wieder zusammengesetzt.
- `SequenceBuffer<T>` ist der Ringpuffer über 16-Bit-Sequenzen (sicher beim Überlauf).

`EndpointConfig` (Standardwerte): `max_packet_size` 16 KB Nutzlast, `fragment_above` 1024 Byte,
`fragment_size` 1024, `max_fragments` 16, Puffer für Acks, gesendete und empfangene Pakete je 256,
64 Reassembly-Plätze, Glättung von RTT, Verlust und Bandbreite je 0,1, `packet_header_size` 28
(IPv4 + UDP, für die Bandbreitenschätzung).

`EndpointStats`: `rtt_ms` (geglättete Umlaufzeit), `packet_loss` (Prozent), `sent_kbps`,
`received_kbps`, `acked_kbps`.

---

## 10.4 Kanäle: Connection

Eine `Connection` (`connection.h`, yojimbo-Stil) besitzt einen Endpoint und mehrere Kanäle:

- **reliable ordered:** Jede Nachricht kommt genau einmal und in Reihenfolge an. Nicht bestätigte
  Nachrichten werden neu gesendet (nach dem größeren Wert von `resend_time` und RTT × 1,25), bis das
  Paket, das sie trug, bestätigt ist. Nachrichten über `block_threshold` reisen
  als fragmentierte **Blöcke**, einer nach dem anderen, jedes Fragment bis zur Bestätigung.
- **unreliable unordered:** einmal im nächsten Paket gesendet – für Zustand, Eingaben und Ereignisse,
  die verspätet wertlos sind.

`ChannelConfig` (Standardwerte): `send_queue_size` und `receive_queue_size` 1024 Nachrichten,
`max_messages_per_packet` 256, `packet_budget` 0 (= was übrig ist), `max_message_size` 64 KB,
`block_threshold` 512 Byte, `block_fragment_size` 1024, `resend_time` 0,1 s.
`ConnectionConfig`: Kanäle (Standard: ein zuverlässiger, ein unzuverlässiger), `max_packet_size`
8 KB Nachrichten pro Paket, `max_message_types` 256, die Endpoint-Konfiguration.

Nachrichten sind typisierte Byte-Blobs (`Message`: `id`, `type`, `data`). Typisierte Structs gehen
über `serialize()`:

```cpp
struct Chat { std::string text;
  template <class S> bool serialize(S& s) { return serialize_string(s, text, 200); } };
conn.send(kReliable, kChat, Chat{"hi"});
while (auto m = conn.receive(kReliable)) { Chat c; if (m->type == kChat && read(*m, c)) ... }
```

`write_message` / `read` serialisieren Structs in Nachrichten und zurück. Fehler (`desync`,
`send_queue_full`, `message_too_large`, `bad_packet`, `out_of_memory`) setzen `error()`.

---

## 10.5 Verbindungen: Client und Server

`client_server.h` (Gaffer On Games „Client Server Connection“, yojimbos Client und Server):

**Handshake.**

1. Der Client sendet Verbindungsanfragen mit einem zufälligen **Salt**. Die Anfragen sind auf volle
   Größe **aufgepolstert** – ein gefälschter Request bringt den Server nie dazu, mehr zu senden, als
   er empfing (kein Verstärkungsangriff).
2. Der Server antwortet mit einer **Challenge**, die sein eigenes Salt trägt.
3. Der Client sendet Salt XOR Challenge zurück.
4. Der Server vergibt einen **Slot**, und beide Seiten nutzen das XOR als **Verbindungs-ID** auf
   jedem Paket.

Pakete von unbekannten Adressen oder mit falscher ID werden ignoriert. Untätige Verbindungen laufen
ab; jede Seite kann trennen (redundant gesendet). Danach tragen Nutzlastpakete eine `Connection`.
**Jedes Datagramm beginnt mit einem CRC32 über Protokoll-ID und Paket** – Verkehr eines anderen
Spiels oder einer anderen Version fällt vor dem Parsen heraus.

`PacketType` (erstes Byte nach dem CRC): `request`, `denied`, `challenge`, `response`, `keepalive`,
`payload`, `disconnect`.

`ClientServerConfig` (Standardwerte):

| Feld | Wert |
|---|---|
| `protocol_id` | `0x4b45524e30303031` („KERN0001“: andere Spiele und Versionen reden nicht miteinander) |
| `max_clients` | 16 |
| `timeout` | 6 s ohne Pakete |
| `request_interval` | 0,1 s (Handshake-Wiederholung) |
| `connect_timeout` | 6 s |
| `keepalive_interval` | 0,1 s (so oft gehen mindestens Pakete raus) |

`ClientState`: `denied` (Server voll), `timed_out`, `error`, `disconnected`, `connecting` (sendet
Anfragen), `challenged` (sendet Antworten), `connected`.

Beide Klassen folgen demselben Takt pro Frame: `receive_packets()`, `advance_time(t)`,
`send_packets()`. `Client` bietet `connect`, `disconnect`, `send`, `receive`, `state`,
`client_index`. `Server` bietet `start(max_clients)`, `stop`, `send(client, …)`, `broadcast`,
`receive(client, channel)`, `client_connected`, `connected_clients`, `disconnect_client` und
`take_events()` (Verbindungswechsel als (Slot, verbunden)).

---

## 10.6 Transporte

```cpp
struct Address { u64 id = 0; };      // 0 = keine; der Transport vergibt sie
constexpr int kMaxDatagram = 1400;   // vorsichtige MTU für jeden Transport
class Transport {
  virtual Address address() const = 0;
  virtual bool send(Address to, const u8* data, int bytes) = 0;
  virtual int receive(Address& from, u8* data, int max_bytes) = 0;   // 0 = nichts
  virtual void advance_time(f64 time);                               // liefert verzögerte Pakete
};
```

- **`LoopbackNetwork`**: ein In-Process-Hub; `create()` liefert Endpunkte mit frischen Adressen, die
  miteinander reden (Tests, Studios Mehrspieler-Play mit Server und Clients in einem Modul).
- **`HostTransport`**: Warteschlangen, die der einbettende Host (die Browser-Seite) füllt und leert
  – `push_incoming(from, data)` für Ankommendes, `take_outgoing(to, data)` für zu Sendendes.
  Die Seite kümmert sich um BroadcastChannel oder Relay (10.10).
- **`Simulator`**: `LoopbackNetwork` und `HostTransport` haben je einen eingebauten. `SimulatorSettings`: `latency_ms`
  (einfach), `jitter_ms` (± gleichverteilt), `loss_percent`, `duplicate_percent`. Mit festem Seed
  sind schlechte Netze **reproduzierbar** – in Tests und im Editor wählbar.

---

## 10.7 Spiele im Netz: NetGame

`NetGame` verbindet eine `GameWorld` mit dem Netz (Kopfkommentar von `net_game.h`, nach Gaffer On
Games „Snapshot Interpolation“ und „State Synchronization“):

- Der **Server** besitzt die Simulation (Box3D, Regeln, Pickups, Trigger). Jeder Spieler, der
  beitritt, bekommt einen **Pawn**: eine Kopie des Spieler-Actors des Levels. Jeden Netz-Tick
  schickt er jedem Client einen **Snapshot**: das HUD und die Posen bewegter Actors, priorisiert,
  damit ein Snapshot immer in ein Paket passt.
- Der **Client** spielt dasselbe Level als **Replika** (ohne eigene Physik), sendet jeden Tick seine
  Eingabe (die letzten redundant mit) und zeichnet die Replika etwa 100 ms in der Vergangenheit,
  interpoliert zwischen Snapshots und extrapoliert kurz über Lücken.
- **Ereignisse**, die ankommen müssen (Willkommen, Pawn kam/ging, Pickup gesammelt, Logik-Änderung),
  laufen über den zuverlässigen Kanal; Sounds und Schüsse über den unzuverlässigen.
- Beide Seiten brauchen **dasselbe Level**. Das Willkommen enthält eine Prüfsumme; ein anderes Level
  wird abgewiesen.

```cpp
game::NetGame net(world);
net.host(level, transport);                         // Server: startet Play und hört zu
// oder: net.join(level, transport, server_address); // Client
// jeden Frame:
net.update(time, dt, local_input);                  // Netz, dann die Welt
```

`NetSettings`:

| Feld | Standard | Bedeutung |
|---|---|---|
| `tick_hz` | 30 | Snapshots und Eingaben pro Sekunde (5..120) |
| `interpolation_delay` | 0,1 s | so weit zeichnet der Client hinter dem Server |
| `snapshot_budget` | 900 Byte | Entitätszustand pro Snapshot |
| `max_players` | 8 | (1..64) |

`role()` ist `none`, `server` oder `client`; `playing()` ist beim Server immer wahr, beim Client
nach dem Willkommen. `status()` liefert einen Text („Hosting – 3 players connected“, „Connected
(slot 2)“, „Refused: the server plays a different level“ …), `stats()` Spielerzahl, RTT, Verlust,
Bandbreite, Snapshot-Zähler, Entitäten im letzten Snapshot und beim Client den Puffer
(`buffer_ms`: wie weit der neueste Snapshot vor der Renderzeit liegt).

### Kanäle und Nachrichten

Zwei Kanäle: 0 zuverlässig-geordnet, 1 unzuverlässig (Nachrichten bis 2048 Byte, Pakete bis
2400 Byte – beide dürfen ganze Snapshots tragen).

| Nachricht | Kanal | Richtung | Inhalt |
|---|---|---|---|
| `kWelcome` | zuverlässig | S → C | Level-Prüfsumme, Slot, Tickrate |
| `kRefuse` | zuverlässig | S → C | Grund (bis 200 Zeichen) |
| `kPawn` | zuverlässig | S → C | Slot (255 = der eigene Pawn des Hosts), Pawn-Actor, Quell-Actor |
| `kUnpawn` | zuverlässig | S → C | Pawn entfernt |
| `kAlive` | zuverlässig | S → C | Actor lebt / nicht (Pickups, benutzte Trigger) |
| `kSpawn` | zuverlässig | S → C | von Logik gespawnter Actor: Vorlage, ID, Position (1 mm), Rotation (0,01°) |
| `kProperty` | zuverlässig | S → C | von Logik geänderte Eigenschaft: Actor, Pfad, Wert als JSON |
| `kInput` | unzuverlässig | C → S | Tick und die letzten bis zu vier Eingaben, neueste zuerst |
| `kSnapshot` | unzuverlässig | S → C | Tick, Serverzeit, HUD, eigener Soldatenzustand, Entitäten |
| `kCues` | unzuverlässig | S → C | bis zu 8 Sound-Cues pro Nachricht |
| `kShots` | unzuverlässig | S → C | bis zu 8 Schüsse (Tracer, Einschläge) pro Nachricht |

Eine **Eingabe** kodiert Bewegung (je 1/127), Sprung, Boost, Blickrichtung (Gieren 0,1°, Nicken
0,05°), Feuer, Zielen, Nachladen, Ducken, Interact. Ein **Soldatenzustand** für das HUD des
Clients: Gesundheit, Maximum, Rüstung, Munition, Magazin, Reserve, Nachladefortschritt, Streuung,
Respawn-Zeit, Treffer- und Schadenszähler, Kopftreffer, Kill, tot, Schadensrichtung.

### Der Server, Frame für Frame (`server_update`)

1. Zeit vorstellen, Pakete empfangen.
2. **Verbindungswechsel:** Ein neuer Client bekommt das Willkommen, dann die bestehenden Pawns (der
   des Hosts und die anderer Clients), dann alles, was Logik bisher geändert hat (erst die
   Spawns, dann die Eigenschaften, in Reihenfolge), dann die toten Actors. Danach legt der Server
   seinen Pawn an (`GameWorld::add_pawn`) und kündigt ihn allen an. Ein gegangener Client verliert
   seinen Pawn (`remove_pawn`, `kUnpawn` an alle).
3. **Eingaben:** Je Client gewinnt die neueste; ältere oder doppelte Ticks werden verworfen. Sprung,
   Nachladen und Feuer zählen, wenn sie in **irgendeiner** der neu angekommenen redundanten Eingaben
   stehen – ein Tipp zwischen zwei Paketen schießt trotzdem, ein Sprung geht nie verloren. Für die
   **Lag-Kompensation** setzt der Server `rewind` = RTT/2 + Interpolationsverzögerung (höchstens
   0,5 s): So weit in der Vergangenheit sah der Client die Welt
   ([Kapitel 8](08-spiel-laufzeit.md), Abschnitt 8.13).
4. **`world.step(dt, input)`** mit der Eingabe des Hosts.
5. Gesammelte Pickups / benutzte Trigger (`take_alive_changes`) und Logik-Änderungen
   (`take_replicated_changes`) **zuverlässig** an alle; neue Schüsse und Sound-Cues dieses Frames
   **unzuverlässig** in Paketen zu je acht.
6. Ist ein Netz-Tick fällig: **Snapshots** senden.
7. Pakete senden, Statistik.

### Snapshots und der Prioritätsakkumulator

Für jeden Client baut `server_send_snapshots` einen eigenen Snapshot:

- HUD (Status, Punkte, Zeiten, Boost, Meldung, Pickups) und, wenn sein Pawn ein Soldat ist, dessen
  Zustand.
- **Entitäten:** alle Actors, deren Pose sich bewegen kann (`networked_actors`: dynamische und
  kinematische Bodies, lebendig). Je Entität 24 Bit ID, Position quantisiert auf **1 mm** innerhalb
  ±8192 m, Rotation „smallest three“ mit **11 Bit** je Komponente.
- **Priorität:** Jeder Client führt pro Actor einen Akkumulator. Hat sich ein Actor seit dem letzten
  Senden bewegt (mehr als 1 mm oder spürbar gedreht), steigt er um 10, sonst um 0,25. Der **eigene
  Pawn des Clients geht immer zuerst**. Sortiert wird absteigend, aufgenommen wird, solange das Budget
  (`snapshot_budget` × 8 Bit) und 255 Entitäten reichen; Aufgenommene fallen auf 0 zurück.
  Was sich bewegt, ist also schnell wieder dran; Ruhendes wird selten gesendet; der Snapshot passt
  immer in ein Paket.

### Der Client, Frame für Frame (`client_update`)

1. Beim ersten Update verbinden (der Client braucht dafür die Uhr).
2. **Zuverlässiges:**
   - `kWelcome`: Prüfsumme vergleichen – ein anderes Level wird mit „the server plays a different
     level“ abgewiesen und getrennt. Sonst `world.start_replica(level)`.
   - `kPawn`: `add_pawn(source, actor)` mit **der ID des Servers**; ist es der eigene Slot, wird er
     der lokale Pawn.
   - `kUnpawn`, `kAlive`, `kSpawn` (mit der ID des Servers), `kProperty` (`set_property`).
3. **Unzuverlässiges:**
   - `kSnapshot`: **Uhrabgleich** – die Stichprobe Serverzeit − lokale Zeit; der Versatz folgt
     größeren Stichproben langsam (2 %), kleineren schnell (20 %), denn die kleinste Verzögerung ist
     die ehrlichste. HUD und Soldatenzustand übernehmen; jede Entität bekommt eine Stichprobe
     (verspätete und doppelte werden verworfen, höchstens 32 je Entität).
   - `kShots`, `kCues`: in die Listen der Welt – dort zeichnet bzw. spielt sie der Client wie lokal
     erzeugte.
4. **Eingabe** jeden Netz-Tick: die aktuelle Eingabe mit Blickrichtung der Kamera vorn in die
   Liste der letzten vier, dann senden.
5. **Interpolation:** Renderzeit = lokale Zeit + Versatz − `interpolation_delay`. Für jede Entität
   wird zwischen den beiden Stichproben um die Renderzeit interpoliert (Position linear, Rotation
   normiert linear). Liegt die Renderzeit hinter der neuesten Stichprobe, wird entlang der letzten
   Bewegung **bis zu 250 ms extrapoliert** und dann gehalten. `world.step_replica` stellt die Uhr
   der Replika auf die Renderzeit.

### Die Replika in der GameWorld

- `start_replica(level)` startet das Level wie Play, aber **ohne Logik**, und macht jeden nicht
  statischen Body kinematisch: Regeln laufen auf dem Server, die Bodies folgen nur den gesetzten
  Posen. Pawns nutzen keinen Character Mover.
- `set_replica_pose(id, p, q)` setzt die Pose eines Actors (und seines Bodys).
- `step_replica(dt, time, input)` lässt den **Stadtverkehr lokal** fahren (reine Dekoration), stellt
  die Simulationszeit auf die gezeigte Serverzeit, aktualisiert die Darstellung des eigenen Soldaten
  und die Tageszeit.

### Level-Prüfsumme

`level_checksum(level)` ist der CRC32 des Leveltexts – **ohne die Editor-Kamera**, die nicht zum
Spiel gehört. Studios Play-in-Editor nutzt das offene Level, exportierte Spiele das eingebettete.

---

## 10.8 Mehrspieler im Studio

![Mehrspieler-Play im Studio: Listen-Server und drei lokale Clients in vier Viewports](bilder/studio-multiplayer.png)

### Number of Players

Wie Unreals „Number of Players“ (Menü Play › Number of Players, 1–4): Die Welt des Levels ist ein
**Listen-Server**, jeder weitere Spieler ein Netzwerk-Client mit **eigener GameWorld und eigenem
Viewport**, verbunden über ein In-Process-Netz (`LoopbackNetwork`).

- `Studio::play` erkennt Mehrspieler (Play und mehr als ein Spieler), legt einen Hub an (Seed 7) und
  `NetGame::host` auf der Hauptwelt; `start_local_clients` baut für jeden weiteren Spieler eine
  `GameWorld` auf einem eigenen Render-Layer, einen Hub-Endpunkt und ein `NetGame::join`.
- Das Layout wechselt von einem auf **vier Viewports**; Client *i* zeigt sich in Viewport *i* + 1
  mit seiner eigenen Kamera. (Dass Mehrspieler-Play so umschaltet, gilt nicht als
  Einstellungsänderung des Nutzers.)
- **Eingabe:** Nur der aktive Viewport bekommt die Eingabe; die anderen Spieler stehen still.
- `update_net` taktet Server und Clients. Solange Clients noch beitreten, läuft der Austausch in vier
  Teilschritten pro Frame, damit der Handshake nicht auf gerenderte Frames wartet.
- **Network Emulation** (Play-Menü): Perfect, LAN (5 ms), Average (50 ms, 1 % Verlust), Bad (150 ms,
  5 %), Terrible (300 ms, 20 %), dazu jeweils Jitter (1, 10, 40, 100 ms) – der `Simulator` des Hubs
  (und des Host-Transports).
- Die Statuszeile zeigt Rolle, Status, Spieler, RTT, Verlust, Bandbreite und je lokalem Client
  Status, RTT, Puffer und HUD (`status().net`).

Befehle: `netPlayers {players}`, `netSimulate {latency, jitter, loss, duplicate}`,
`play {mode, players}`. Test `studio_multiplayer_play`: drei Spieler mit 40 ms Latenz, 5 ms Jitter
und 2 % Verlust; nach dem Start ist das Layout vierfach, der Server hat drei Pawns, jeder Client
sieht drei Pawns und spielt; mit dem aktiven Viewport 2 läuft der Pawn von Client 2 auf dem Server
messbar vorwärts; nach Stop gibt es keinen Netzstatus mehr.

### Online: Host und Join

Für Spieler in anderen Tabs oder auf anderen Rechnern: Play › **Host Online Game…** bzw. **Join
Online Game…**. Der Dialog fragt einen **Raum** (Standard „kern“) und optional die Adresse eines
**Relays** – leer für Tabs dieses Browsers; für andere Rechner `tools/net_relay.mjs` starten und
seine Adresse eintragen (`ws://host:8790`). Alle brauchen dasselbe Level.

Ablauf (`openNetwork` in `web/studio/game.mjs`):

1. Die Seite öffnet `KernNet` (lokal oder Relay) und erfährt ihre eigene Peer-ID und die des Hosts.
2. `netAddress {self}` gibt dem `HostTransport` der Engine diese Adresse.
3. `netHost` startet Play als Server über den Host-Transport (`NetGame::host`), `netJoin {server}`
   tritt als Client bei (`NetGame::join`).
4. „Leave Online Game“ (`netStop`) beendet die Sitzung.

---

## 10.9 Exportierte Spiele im Netz

Ein mit File › Package Game exportiertes Spiel ([Kapitel 11](11-studio.md)) liest seine URL:

| URL | Wirkung |
|---|---|
| `…?host=raum` | hostet im Raum „raum“ |
| `…?join=raum` | tritt dem Raum bei |
| `…&relay=ws://host:8790` | über diesen Relay statt BroadcastChannel |

Ohne Raumnamen gilt „kern“. Schlägt das Öffnen fehl, erscheint „Online play failed: … – playing
alone“, und das Spiel startet allein.

---

## 10.10 Der Browser-Transport: `KernNet`

`KernNet` (`web/studio/game.mjs`) bewegt Datagramme zwischen Browsern. Acks, Neuversand, Kanäle,
Handshake und Timeouts erledigt Kerns C++-Stack; `KernNet` liefert nur Bytes aus, wie UDP es täte.

- **`local`:** ein `BroadcastChannel("kern-net-<raum>")` – Tabs dieses Browsers auf diesem Rechner,
  kein Server. Die Peer-ID ist eine zufällige 53-Bit-Zahl (nie 0). Der Host kündigt sich alle 500 ms
  an; ein Beitretender fragt und wartet bis zu 4 s auf ein Hallo („no game is hosted in room …“).
- **`relay`:** ein WebSocket zum Relay. Nach dem Öffnen `{"host": raum}` bzw. `{"join": raum}`;
  die Antwort enthält die eigene ID und die des Hosts („nobody hosts room …“, wenn keiner da ist).
- Ankommende Datagramme gehen per `engine.netPush(from, bytes)` in den `HostTransport`; ein Timer
  (alle 8 ms) und zusätzlich jeder Frame holen mit `engine.netTake()` bis zu 256 ausgehende
  Datagramme und verschicken sie – lokal als Nachricht `{from, to, data}`, über den Relay als
  Binärrahmen mit vorangestellter 8-Byte-Ziel-ID.

---

## 10.11 Der Relay

Browser können kein UDP senden. Für Spieler auf verschiedenen Rechnern leitet `tools/net_relay.mjs`
(Node, Paket `ws`) Datagramme über WebSockets weiter – **ohne eigene Logik**. Kern Net darüber
verhält sich wie über UDP.

```sh
npm install ws
node tools/net_relay.mjs 8790        # Standardport 8790 (oder Umgebungsvariable PORT)
```

Protokoll je WebSocket:

| Nachricht | Antwort / Wirkung |
|---|---|
| Text `{"join": "<raum>"}` | `{"id": <peer-id>, "host": <host-id oder 0>}` |
| Text `{"host": "<raum>"}` | `{"id": <peer-id>, "host": <eigene id>}` – wird Host des Raums |
| Binär `[8 Byte Ziel-ID, LE][Datagramm]` | wird als `[8 Byte Absender-ID][Datagramm]` an das Ziel zugestellt |

- Peer-IDs werden fortlaufend vergeben.
- Datagramme sind höchstens 1400 Byte (plus 8 Byte ID); kürzere als 9 und längere Rahmen werden
  verworfen, ebenso Datagramme an unbekannte IDs.
- Raumnamen sind kürzer als 64 Zeichen.
- Verlässt der Host den Relay, ist der Raum für neue Beitretende geschlossen.
- Eine HTTP-Anfrage an den Port beantwortet er mit „Kern relay“.

Der Relay sieht nur Bytes; die Sicherheit gegen fremden Verkehr liefern CRC32 mit Protokoll-ID und
die Verbindungs-IDs des Handshakes (10.5).

---

## 10.12 Was wo läuft

| | Server (Host) | Client (Replika) |
|---|---|---|
| Box3D-Simulation, Regeln, Pickups, Trigger | ja | nein (kinematische Bodies folgen Snapshots) |
| Logik-Graphen | ja | nein; Änderungen kommen als `kSpawn`/`kProperty` |
| Schüsse, Treffer, Schaden | ja, lag-kompensiert | Darstellung aus `kShots`, HUD aus dem Snapshot |
| Sound-Cues | erzeugt | aus `kCues` |
| Stadtverkehr | simuliert | lokal (Dekoration) |
| Kamera, Touch, Eingabe | lokal | lokal; Eingabe geht an den Server |

Clients senden derzeit nichts Zuverlässiges; der Server leert den Kanal nur.

---

## 10.13 Tests

`kern/tests/run.sh` prüft:

| Test | Prüft |
|---|---|
| `net_serialization` | Rundreise, Grenzwerte, abgeschnittene Daten, 2000 Zufallspuffer |
| `net_endpoint_acks_and_fragments` | Acks und Fragmentierung |
| `net_reliable_channel_under_bad_network` | zuverlässiger Kanal unter 25 % Verlust, 60 ± 30 ms Latenz und 10 % Duplikaten: alle Nachrichten und Blöcke in Reihenfolge |
| `net_client_server` | Handshake, voller Server, Disconnect, Timeout, 500 Müll-Datagramme, Wiederverbinden |
| `net_game_server_and_clients` | Spiele im Netz |
| `net_game_soldier_replication` | Soldaten, Schüsse, Treffer über das Netz |
| `net_game_replicates_logic_changes` | Logik-Änderungen (Spawns, Eigenschaften) erreichen Clients |
| `studio_multiplayer_play` | Number of Players im Studio |

---

© 2026 Christian Heinrich Hohlfeld · Alle Rechte vorbehalten · [Impressum und Rechte](00-impressum.md)
