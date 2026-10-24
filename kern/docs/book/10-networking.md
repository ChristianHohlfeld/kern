# 10 Networking and Multiplayer

Kern ships its own networking stack, in C++ in the engine module (`include/kern/net`, `src/net`). It
follows Glenn Fiedler's articles (Gaffer On Games) and the design of yojimbo, reliable.io and
netcode.io. On top of it sits `game::NetGame` (`include/kern/game/net_game.h`,
`src/game/net_game.cpp`): it turns **any Kern level into a multiplayer game**, without the level
having to be prepared for it.

The stack needs only one thing from the transport layer: sending and receiving datagrams. It
therefore runs everywhere:

- in the browser over a **BroadcastChannel** (tabs on one machine) or a **WebSocket relay**
  (different machines),
- natively and in tests over an **in-process network** with a network simulator,
- in Studio for "Number of Players" over the same in-process network.

---

## 10.1 The layers

| Layer | File | Task |
|---|---|---|
| Serialization | `serialize.h` | bit packer (32-bit words, little endian), **one** `serialize()` function per type for reading, writing and measuring; integers within a range, bools, floats, quantized floats and vectors, "smallest three" quaternions, strings, relative sequence numbers. Readers check everything. |
| Reliability | `reliable.h` | Every send carries a sequence, an ack and 32 ack bits (compressed header). Provides acks, RTT, packet loss and bandwidth; packets above 1 KB are fragmented and reassembled. |
| Channels | `connection.h` | **Reliable ordered** (exactly once, in order, resending, large messages as blocks) and **unreliable unordered** (once, in the next packet). Messages are typed byte sequences. |
| Connection | `client_server.h` | Handshake with client salt, challenge salt and XOR connection ID, slots, keep-alive, timeouts, disconnect, "server full", CRC32 over protocol ID and content. |
| Transport | `transport.h` | `LoopbackNetwork` (in-process), `HostTransport` (datagrams with the browser side), `Simulator` for latency, jitter, loss and duplicates with a fixed seed. |
| Game | `kern/game/net_game.h` | Authoritative server, snapshot interpolation, inputs, events. |

---

## 10.2 Serialization

Following "Reading and Writing Packets" and "Serialization Strategies": **one** function per type
describes the format for reading, writing and measuring.

```cpp
struct WelcomeMsg {                 // from src/game/net_game.cpp
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

Three stream classes implement the same interface: `WriteStream`, `ReadStream` and `MeasureStream`
(counts bits without writing, worst case for alignments). Each has `kWriting`, `kReading`,
`kMeasuring` as `constexpr`, so that a `serialize` function can, for example, resize a vector
beforehand when reading (`if constexpr (S::kReading) …`).

| Function | Encoding |
|---|---|
| `serialize_int(s, v, min, max)` | exactly as many bits as the range needs (`bits_required`) |
| `serialize_bits(s, v, bits)` | fixed number of bits |
| `serialize_bool` | 1 bit |
| `serialize_u16`, `serialize_u32`, `serialize_u64` | 16, 32, 64 bits |
| `serialize_float` | 32 bits |
| `serialize_compressed_float(s, v, min, max, res)` | quantized to `res` within the range |
| `serialize_vec3`, `serialize_compressed_vec3(s, v, bound, res)` | vector in full, or quantized within ±`bound` |
| `serialize_quat(s, q, bits)` | "smallest three": the largest component is omitted, the other three use `bits` bits (default 10) |
| `serialize_bytes`, `serialize_string(s, str, max)` | bytes, string with a maximum length |
| `serialize_sequence_relative(s, base, v)` | sequence number relative to a base |

**Readers check everything:** values outside the range, overflowing buffers and wrong lengths make
`serialize` return `false`. A malicious or broken packet is discarded instead of causing a crash.
Words are little endian on every platform. `crc32(data, bytes, crc)` also lives here.

---

## 10.3 Reliability: Endpoint

`Endpoint` (`reliable.h`, following "Reliability and Congestion Avoidance over UDP" and "Packet
Fragmentation and Reassembly"; reliable.io) sits on top of an unreliable datagram transport:

- Every packet carries its **16-bit sequence** and the receiver's view of the other side: the most
  recently received sequence (**ack**) and **32 bits** for the 32 before it.
- The header is **compressed** (at most 9 bytes): a prefix byte, the sequence, the ack as a 1-byte
  distance from the sequence if it is ≤ 255 (otherwise 2 bytes), and of the four ack-bit bytes only
  those that are not `0xFF`.
- **Sending never waits.** What gets resent when a packet's ack fails to arrive is decided by the
  layer above (the channels).
- Packets above `fragment_above` are split into fragments and reassembled.
- `SequenceBuffer<T>` is the ring buffer over 16-bit sequences (safe on wraparound).

`EndpointConfig` (defaults): `max_packet_size` 16 KB payload, `fragment_above` 1024 bytes,
`fragment_size` 1024, `max_fragments` 16, buffers for acks, sent and received packets of 256 each,
64 reassembly slots, smoothing of RTT, loss and bandwidth of 0.1 each, `packet_header_size` 28
(IPv4 + UDP, for the bandwidth estimate).

`EndpointStats`: `rtt_ms` (smoothed round-trip time), `packet_loss` (percent), `sent_kbps`,
`received_kbps`, `acked_kbps`.

---

## 10.4 Channels: Connection

A `Connection` (`connection.h`, yojimbo style) owns an endpoint and several channels:

- **reliable ordered:** every message arrives exactly once and in order. Unacknowledged messages are
  resent (after the larger of `resend_time` and RTT × 1.25) until the packet that carried them is
  acknowledged. Messages above `block_threshold` travel as fragmented **blocks**, one after the
  other, each fragment until acknowledged.
- **unreliable unordered:** sent once in the next packet – for state, inputs and events that are
  worthless when late.

`ChannelConfig` (defaults): `send_queue_size` and `receive_queue_size` 1024 messages,
`max_messages_per_packet` 256, `packet_budget` 0 (= whatever is left), `max_message_size` 64 KB,
`block_threshold` 512 bytes, `block_fragment_size` 1024, `resend_time` 0.1 s.
`ConnectionConfig`: channels (default: one reliable, one unreliable), `max_packet_size` 8 KB of
messages per packet, `max_message_types` 256, the endpoint configuration.

Messages are typed byte blobs (`Message`: `id`, `type`, `data`). Typed structs go through
`serialize()`:

```cpp
struct Chat { std::string text;
  template <class S> bool serialize(S& s) { return serialize_string(s, text, 200); } };
conn.send(kReliable, kChat, Chat{"hi"});
while (auto m = conn.receive(kReliable)) { Chat c; if (m->type == kChat && read(*m, c)) ... }
```

`write_message` / `read` serialize structs into messages and back. Errors (`desync`,
`send_queue_full`, `message_too_large`, `bad_packet`, `out_of_memory`) set `error()`.

---

## 10.5 Connections: client and server

`client_server.h` (Gaffer On Games "Client Server Connection", yojimbo's client and server):

**Handshake.**

1. The client sends connection requests with a random **salt**. The requests are **padded** to full
   size – a forged request never makes the server send more than it received (no amplification
   attack).
2. The server answers with a **challenge** that carries its own salt.
3. The client sends back salt XOR challenge.
4. The server assigns a **slot**, and both sides use the XOR as the **connection ID** on every
   packet.

Packets from unknown addresses or with a wrong ID are ignored. Idle connections time out; either
side can disconnect (sent redundantly). After that, payload packets carry a `Connection`. **Every
datagram begins with a CRC32 over protocol ID and packet** – traffic from another game or another
version drops out before parsing.

`PacketType` (first byte after the CRC): `request`, `denied`, `challenge`, `response`, `keepalive`,
`payload`, `disconnect`.

`ClientServerConfig` (defaults):

| Field | Value |
|---|---|
| `protocol_id` | `0x4b45524e30303031` ("KERN0001": other games and versions do not talk to each other) |
| `max_clients` | 16 |
| `timeout` | 6 s without packets |
| `request_interval` | 0.1 s (handshake repetition) |
| `connect_timeout` | 6 s |
| `keepalive_interval` | 0.1 s (packets go out at least this often) |

`ClientState`: `denied` (server full), `timed_out`, `error`, `disconnected`, `connecting` (sends
requests), `challenged` (sends responses), `connected`.

Both classes follow the same rhythm per frame: `receive_packets()`, `advance_time(t)`,
`send_packets()`. `Client` offers `connect`, `disconnect`, `send`, `receive`, `state`,
`client_index`. `Server` offers `start(max_clients)`, `stop`, `send(client, …)`, `broadcast`,
`receive(client, channel)`, `client_connected`, `connected_clients`, `disconnect_client` and
`take_events()` (connection changes as (slot, connected)).

---

## 10.6 Transports

```cpp
struct Address { u64 id = 0; };      // 0 = none; the transport assigns them
constexpr int kMaxDatagram = 1400;   // conservative MTU for every transport
class Transport {
  virtual Address address() const = 0;
  virtual bool send(Address to, const u8* data, int bytes) = 0;
  virtual int receive(Address& from, u8* data, int max_bytes) = 0;   // 0 = nothing
  virtual void advance_time(f64 time);                               // delivers delayed packets
};
```

- **`LoopbackNetwork`**: an in-process hub; `create()` returns endpoints with fresh addresses that
  talk to each other (tests, Studio's multiplayer Play with server and clients in one module).
- **`HostTransport`**: queues that the embedding host (the browser side) fills and drains –
  `push_incoming(from, data)` for incoming data, `take_outgoing(to, data)` for data to send. The page
  takes care of the BroadcastChannel or relay (10.10).
- **`Simulator`**: `LoopbackNetwork` and `HostTransport` each have one built in.
  `SimulatorSettings`: `latency_ms` (one way), `jitter_ms` (± uniformly distributed),
  `loss_percent`, `duplicate_percent`. With a fixed seed, bad networks are **reproducible** –
  selectable in tests and in the editor.

---

## 10.7 Games over the network: NetGame

`NetGame` connects a `GameWorld` to the network (header comment of `net_game.h`, following Gaffer On
Games "Snapshot Interpolation" and "State Synchronization"):

- The **server** owns the simulation (Box3D, rules, pickups, triggers). Every player who joins gets a
  **pawn**: a copy of the level's player actor. Every network tick it sends each client a
  **snapshot**: the HUD and the poses of moving actors, prioritized so that a snapshot always fits
  into one packet.
- The **client** plays the same level as a **replica** (without its own physics), sends its input
  every tick (with the most recent ones redundantly) and draws the replica about 100 ms in the past,
  interpolating between snapshots and briefly extrapolating across gaps.
- **Events** that must arrive (welcome, pawn joined/left, pickup collected, logic change) go over the
  reliable channel; sounds and shots over the unreliable one.
- Both sides need **the same level**. The welcome contains a checksum; a different level is
  refused.

```cpp
game::NetGame net(world);
net.host(level, transport);                         // server: starts Play and listens
// or: net.join(level, transport, server_address);  // client
// every frame:
net.update(time, dt, local_input);                  // network, then the world
```

`NetSettings`:

| Field | Default | Meaning |
|---|---|---|
| `tick_hz` | 30 | snapshots and inputs per second (5..120) |
| `interpolation_delay` | 0.1 s | how far the client draws behind the server |
| `snapshot_budget` | 900 bytes | entity state per snapshot |
| `max_players` | 8 | (1..64) |

`role()` is `none`, `server` or `client`; `playing()` is always true on the server, and on the
client after the welcome. `status()` returns a text ("Hosting – 3 players connected", "Connected
(slot 2)", "Refused: the server plays a different level" …), `stats()` returns player count, RTT,
loss, bandwidth, snapshot counter, entities in the last snapshot and, on the client, the buffer
(`buffer_ms`: how far the newest snapshot is ahead of the render time).

### Channels and messages

Two channels: 0 reliable-ordered, 1 unreliable (messages up to 2048 bytes, packets up to 2400 bytes –
both may carry whole snapshots).

| Message | Channel | Direction | Content |
|---|---|---|---|
| `kWelcome` | reliable | S → C | level checksum, slot, tick rate |
| `kRefuse` | reliable | S → C | reason (up to 200 characters) |
| `kPawn` | reliable | S → C | slot (255 = the host's own pawn), pawn actor, source actor |
| `kUnpawn` | reliable | S → C | pawn removed |
| `kAlive` | reliable | S → C | actor alive / not (pickups, used triggers) |
| `kSpawn` | reliable | S → C | actor spawned by logic: template, ID, position (1 mm), rotation (0.01°) |
| `kProperty` | reliable | S → C | property changed by logic: actor, path, value as JSON |
| `kInput` | unreliable | C → S | tick and the last up to four inputs, newest first |
| `kSnapshot` | unreliable | S → C | tick, server time, HUD, own soldier state, entities |
| `kCues` | unreliable | S → C | up to 8 sound cues per message |
| `kShots` | unreliable | S → C | up to 8 shots (tracers, impacts) per message |

An **input** encodes movement (1/127 each), jump, boost, view direction (yaw 0.1°, pitch 0.05°),
fire, aim, reload, crouch, interact. A **soldier state** for the client's HUD: health, maximum,
armor, ammunition, magazine, reserve, reload progress, spread, respawn time, hit and damage
counters, headshot, kill, dead, damage direction.

### The server, frame by frame (`server_update`)

1. Advance time, receive packets.
2. **Connection changes:** a new client gets the welcome, then the existing pawns (the host's and
   those of other clients), then everything logic has changed so far (first the spawns, then the
   properties, in order), then the dead actors. After that the server creates the client's pawn
   (`GameWorld::add_pawn`) and announces it to everyone. A client that left loses its pawn
   (`remove_pawn`, `kUnpawn` to all).
3. **Inputs:** per client the newest wins; older or duplicate ticks are discarded. Jump, reload and
   fire count if they appear in **any** of the newly arrived redundant inputs – a tap between two
   packets still fires, a jump is never lost. For **lag compensation** the server sets `rewind` =
   RTT/2 + interpolation delay (at most 0.5 s): that is how far in the past the client saw the world
   ([Chapter 8](08-game-runtime.md), Section 8.13).
4. **`world.step(dt, input)`** with the host's input.
5. Collected pickups / used triggers (`take_alive_changes`) and logic changes
   (`take_replicated_changes`) go **reliably** to everyone; new shots and sound cues of this frame go
   **unreliably** in packets of eight.
6. If a network tick is due: send **snapshots**.
7. Send packets, statistics.

### Snapshots and the priority accumulator

For each client, `server_send_snapshots` builds a snapshot of its own:

- HUD (status, score, times, boost, message, pickups) and, if the client's pawn is a soldier, its
  state.
- **Entities:** all actors whose pose can move (`networked_actors`: dynamic and kinematic bodies,
  alive). Per entity a 24-bit ID, position quantized to **1 mm** within ±8192 m, rotation "smallest
  three" with **11 bits** per component.
- **Priority:** every client keeps an accumulator per actor. If an actor has moved since it was last
  sent (more than 1 mm or noticeably rotated), it increases by 10, otherwise by 0.25. The **client's
  own pawn always goes first**. Entities are sorted in descending order and included as long as the
  budget (`snapshot_budget` × 8 bits) and 255 entities suffice; included ones drop back to 0. What
  moves therefore gets its turn again quickly; what rests is sent rarely; the snapshot always fits
  into one packet.

### The client, frame by frame (`client_update`)

1. Connect on the first update (the client needs the clock for that).
2. **Reliable messages:**
   - `kWelcome`: compare the checksum – a different level is refused with "the server plays a
     different level" and disconnected. Otherwise `world.start_replica(level)`.
   - `kPawn`: `add_pawn(source, actor)` with **the server's ID**; if it is the client's own slot, it
     becomes the local pawn.
   - `kUnpawn`, `kAlive`, `kSpawn` (with the server's ID), `kProperty` (`set_property`).
3. **Unreliable messages:**
   - `kSnapshot`: **clock synchronization** – the sample server time − local time; the offset follows
     larger samples slowly (2 %) and smaller ones quickly (20 %), because the smallest delay is the
     most honest. Adopt HUD and soldier state; every entity gets a sample (late and duplicate ones
     are discarded, at most 32 per entity).
   - `kShots`, `kCues`: into the world's lists – there the client draws or plays them like locally
     generated ones.
4. **Input** every network tick: the current input with the camera's view direction at the front of
   the list of the last four, then send.
5. **Interpolation:** render time = local time + offset − `interpolation_delay`. For each entity, it
   interpolates between the two samples around the render time (position linear, rotation
   normalized linear). If the render time is past the newest sample, it **extrapolates up to
   250 ms** along the last motion and then holds. `world.step_replica` sets the replica's clock to
   the render time.

### The replica in the GameWorld

- `start_replica(level)` starts the level like Play, but **without logic**, and makes every
  non-static body kinematic: rules run on the server, the bodies merely follow the set poses. Pawns
  do not use a character mover.
- `set_replica_pose(id, p, q)` sets the pose of an actor (and its body).
- `step_replica(dt, time, input)` lets **city traffic run locally** (pure decoration), sets the
  simulation time to the displayed server time, and updates the presentation of the client's own
  soldier and the time of day.

### Level checksum

`level_checksum(level)` is the CRC32 of the level text – **without the editor camera**, which is not
part of the game. Studio's Play-in-Editor uses the open level, exported games the embedded one.

---

## 10.8 Multiplayer in Studio

![Multiplayer Play in Studio: a listen server and one local client, with two orthographic views](../buch/bilder/studio-multiplayer.png)

### Number of Players

Like Unreal's "Number of Players" (menu Play › Number of Players, 1–4): the level's world is a
**listen server**, and every additional player is a network client with **its own GameWorld and its
own viewport**, connected via an in-process network (`LoopbackNetwork`).

- `Studio::play` detects multiplayer (Play and more than one player), creates a hub (seed 7) and
  `NetGame::host` on the main world; `start_local_clients` builds a `GameWorld` on its own render
  layer, a hub endpoint and a `NetGame::join` for each additional player.
- The layout switches from one to **four viewports**; client *i* appears in viewport *i* + 1 with its
  own camera. (Multiplayer Play switching the layout like this does not count as a change of the
  user's settings.)
- **Input:** only the active viewport receives input; the other players stand still.
- `update_net` clocks server and clients. While clients are still joining, the exchange runs in four
  sub-steps per frame, so that the handshake does not wait for rendered frames.
- **Network Emulation** (Play menu): Perfect, LAN (5 ms), Average (50 ms, 1 % loss), Bad (150 ms,
  5 %), Terrible (300 ms, 20 %), each with jitter (1, 10, 40, 100 ms) – the `Simulator` of the hub
  (and of the host transport).
- The status line shows role, status, players, RTT, loss, bandwidth and, per local client, status,
  RTT, buffer and HUD (`status().net`).

Commands: `netPlayers {players}`, `netSimulate {latency, jitter, loss, duplicate}`,
`play {mode, players}`. Test `studio_multiplayer_play`: three players with 40 ms latency, 5 ms jitter
and 2 % loss; after starting, the layout is fourfold, the server has three pawns, every client sees
three pawns and plays; with viewport 2 active, client 2's pawn measurably moves forward on the
server; after Stop there is no network status any more.

### Online: Host and Join

For players in other tabs or on other machines: Play › **Host Online Game…** or **Join Online
Game…**. The dialog asks for a **room** (default "kern") and optionally the address of a **relay** –
empty for tabs of this browser; for other machines, start `tools/net_relay.mjs` and enter its address
(`ws://host:8790`). Everyone needs the same level.

Sequence (`openNetwork` in `web/studio/game.mjs`):

1. The page opens `KernNet` (local or relay) and learns its own peer ID and the host's.
2. `netAddress {self}` gives the engine's `HostTransport` this address.
3. `netHost` starts Play as a server over the host transport (`NetGame::host`); `netJoin {server}`
   joins as a client (`NetGame::join`).
4. "Leave Online Game" (`netStop`) ends the session.

---

## 10.9 Exported games over the network

A game exported with File › Package Game ([Chapter 11](11-studio.md)) reads its URL:

| URL | Effect |
|---|---|
| `…?host=raum` | hosts in the room "raum" |
| `…?join=raum` | joins the room |
| `…&relay=ws://host:8790` | via this relay instead of BroadcastChannel |

(`raum` – German for "room" – is just an example room name.) Without a room name, "kern" applies. If
opening fails, "Online play failed: … – playing alone" appears, and the game starts on its own.

---

## 10.10 The browser transport: `KernNet`

`KernNet` (`web/studio/game.mjs`) moves datagrams between browsers. Acks, resending, channels,
handshake and timeouts are handled by Kern's C++ stack; `KernNet` merely delivers bytes, as UDP
would.

- **`local`:** a `BroadcastChannel("kern-net-<raum>")` – tabs of this browser on this machine, no
  server. The peer ID is a random 53-bit number (never 0). The host announces itself every 500 ms; a
  joiner asks and waits up to 4 s for a hello ("no game is hosted in room …").
- **`relay`:** a WebSocket to the relay. After opening, `{"host": raum}` or `{"join": raum}`; the
  answer contains the peer's own ID and the host's ("nobody hosts room …" if there is none).
- Incoming datagrams go into the `HostTransport` via `engine.netPush(from, bytes)`; a timer (every
  8 ms) and additionally every frame fetch up to 256 outgoing datagrams with `engine.netTake()` and
  send them – locally as a message `{from, to, data}`, via the relay as a binary frame prefixed with
  an 8-byte destination ID.

---

## 10.11 The relay

Browsers cannot send UDP. For players on different machines, `tools/net_relay.mjs` (Node, package
`ws`) forwards datagrams over WebSockets – **without any logic of its own**. Kern Net on top of it
behaves as it would over UDP.

```sh
npm install ws
node tools/net_relay.mjs 8790        # default port 8790 (or environment variable PORT)
```

Protocol per WebSocket:

| Message | Response / effect |
|---|---|
| Text `{"join": "<raum>"}` | `{"id": <peer-id>, "host": <host-id or 0>}` |
| Text `{"host": "<raum>"}` | `{"id": <peer-id>, "host": <own id>}` – becomes the room's host |
| Binary `[8-byte destination ID, LE][datagram]` | is delivered to the destination as `[8-byte sender ID][datagram]` |

- Peer IDs are assigned sequentially.
- Datagrams are at most 1400 bytes (plus 8 bytes of ID); frames shorter than 9 bytes or longer are
  discarded, as are datagrams to unknown IDs.
- Room names are shorter than 64 characters.
- If the host leaves the relay, the room is closed to new joiners.
- It answers an HTTP request to the port with "Kern relay".

The relay sees only bytes; protection against foreign traffic comes from the CRC32 with protocol ID
and the handshake's connection IDs (10.5).

---

## 10.12 What runs where

| | Server (host) | Client (replica) |
|---|---|---|
| Box3D simulation, rules, pickups, triggers | yes | no (kinematic bodies follow snapshots) |
| Logic graphs | yes | no; changes arrive as `kSpawn`/`kProperty` |
| Shots, hits, damage | yes, lag-compensated | presentation from `kShots`, HUD from the snapshot |
| Sound cues | generated | from `kCues` |
| City traffic | simulated | local (decoration) |
| Camera, touch, input | local | local; input goes to the server |

Clients currently send nothing reliable; the server merely drains the channel.

---

## 10.13 Tests

`kern/tests/run.sh` checks:

| Test | Checks |
|---|---|
| `net_serialization` | round trip, limit values, truncated data, 2000 random buffers |
| `net_endpoint_acks_and_fragments` | acks and fragmentation |
| `net_reliable_channel_under_bad_network` | reliable channel under 25 % loss, 60 ± 30 ms latency and 10 % duplicates: all messages and blocks in order |
| `net_client_server` | handshake, full server, disconnect, timeout, 500 garbage datagrams, reconnecting |
| `net_game_server_and_clients` | games over the network |
| `net_game_soldier_replication` | soldiers, shots, hits over the network |
| `net_game_replicates_logic_changes` | logic changes (spawns, properties) reach clients |
| `studio_multiplayer_play` | Number of Players in Studio |

---

© 2026 Christian Heinrich Hohlfeld · All rights reserved · [Imprint and rights](00-imprint.md)
