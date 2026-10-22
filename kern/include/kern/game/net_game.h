// Multiplayer for Kern games: an authoritative server runs the GameWorld,
// clients send their inputs and draw the world from snapshots (Gaffer On
// Games "Snapshot Interpolation" / "State Synchronization").
//
//   server  owns the simulation (Box3D, rules, pickups, triggers). Every
//           player who joins gets a pawn: a copy of the level's player actor.
//           Each network tick it sends every client a snapshot: the HUD and
//           the poses of moving actors, prioritised so a snapshot always fits
//           one packet (bodies that changed most and the client's own pawn
//           first; resting bodies are sent rarely).
//   client  runs the same level as a replica (no physics of its own), sends
//           its input every tick (the last few inputs redundantly, so a lost
//           packet costs nothing) and renders the replica ~100 ms in the past,
//           interpolating between snapshots (jitter buffer), extrapolating
//           briefly over gaps.
//   events  that must arrive (pawn spawned / left, pickup collected, game
//           over, the welcome) go over the reliable-ordered channel; sound
//           cues over the unreliable one.
//
// Both sides need the same level (Studio's Play-in-editor uses the open
// level; packaged games embed it). The welcome carries a level checksum and
// the client refuses a different level.
#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "kern/game/runtime.h"
#include "kern/net/client_server.h"

namespace kern::game {

enum class NetRole : u8 { none, server, client };

struct NetSettings {
  f32 tick_hz = 30;              // snapshots and inputs per second
  f32 interpolation_delay = 0.1f;  // seconds the client renders behind the server
  int snapshot_budget = 900;     // bytes of entity state per snapshot
  int max_players = 8;
};

struct NetStats {
  int players = 0;
  f32 rtt_ms = 0, packet_loss = 0, sent_kbps = 0, received_kbps = 0;
  u32 snapshots_sent = 0, snapshots_received = 0, entities_last_snapshot = 0;
  f32 buffer_ms = 0;             // client: how far ahead the newest snapshot is of the render time
};

/// Serialises the level for the checksum both sides compare.
u32 level_checksum(const Level& level);

class NetGame {
 public:
  explicit NetGame(GameWorld& world);
  ~NetGame();

  /// Server: starts the level (Play) and listens on `transport`.
  bool host(const Level& level, net::Transport& transport, const NetSettings& settings = {}, u64 seed = 1);
  /// Client: connects to `server` and plays the level as a replica once welcomed.
  bool join(const Level& level, net::Transport& transport, net::Address server, const NetSettings& settings = {}, u64 seed = 2);
  void stop();
  NetRole role() const { return role_; }
  bool connected() const;
  /// Client: welcomed by the server and playing.
  bool playing() const { return role_ == NetRole::server || (role_ == NetRole::client && welcomed_); }
  std::string status() const;

  /// Per rendered frame: networking, then the world (server: step; client: replica poses).
  /// `input` is this machine's player; `camera_yaw` (degrees) its view direction.
  void update(f64 time, f32 dt, const PlayInput& input);
  const NetStats& stats() const { return stats_; }
  int local_slot() const { return local_slot_; }
  ActorId local_pawn() const;

 private:
  struct Remote;
  struct Entity;
  void server_update(f64 time, f32 dt, const PlayInput& input);
  void client_update(f64 time, f32 dt, const PlayInput& input);
  void server_send_snapshots();
  void apply_snapshot_buffer(f64 render_time);

  GameWorld& world_;
  NetRole role_ = NetRole::none;
  NetSettings settings_;
  Level level_;
  u32 checksum_ = 0;
  std::unique_ptr<net::Server> server_;
  std::unique_ptr<net::Client> client_;
  std::vector<std::unique_ptr<Remote>> remotes_;  // server: per client slot
  std::unordered_map<ActorId, std::unique_ptr<Entity>> entities_;  // client: interpolation buffers
  f64 time_ = 0, tick_accum_ = 0, server_time_ = 0, clock_offset_ = 0;
  bool clock_valid_ = false;
  u32 tick_ = 0;
  bool welcomed_ = false;
  bool connect_pending_ = false;  // the client connects on its first update (it needs the clock)
  net::Address server_address_;
  int local_slot_ = -1;
  std::vector<PlayInput> recent_inputs_;
  NetStats stats_;
  std::string refused_;
};

}  // namespace kern::game
