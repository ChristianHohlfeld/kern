#include "kern/game/net_game.h"

#include <algorithm>
#include <cmath>

namespace kern::game {

namespace {

constexpr int kReliable = 0, kUnreliable = 1;
enum Msg : u16 { kWelcome = 1, kRefuse, kPawn, kUnpawn, kAlive, kInput, kSnapshot, kCues };
constexpr f32 kWorldBound = 8192;    // metres from the origin a networked pose may be
constexpr int kMaxEntities = 255;
constexpr int kInputHistory = 4;

// ---- messages ----

struct WelcomeMsg {
  u32 checksum = 0;
  i32 slot = 0;
  f32 tick_hz = 30;
  template <class S>
  bool serialize(S& s) { return net::serialize_u32(s, checksum) && net::serialize_int(s, slot, 0, 254) && net::serialize_float(s, tick_hz); }
};

struct RefuseMsg {
  std::string reason;
  template <class S>
  bool serialize(S& s) { return net::serialize_string(s, reason, 200); }
};

struct PawnMsg {
  i32 slot = 0;  // 255 = the host's own pawn
  u32 actor = 0, source = 0;
  template <class S>
  bool serialize(S& s) { return net::serialize_int(s, slot, 0, 255) && net::serialize_u32(s, actor) && net::serialize_u32(s, source); }
};

struct ActorMsg {
  u32 actor = 0;
  bool alive = true;
  template <class S>
  bool serialize(S& s) { return net::serialize_u32(s, actor) && net::serialize_bool(s, alive); }
};

template <class S>
bool serialize_input(S& s, PlayInput& in) {
  using namespace net;
  return serialize_compressed_float(s, in.move_x, -1, 1, 1.0f / 127) && serialize_compressed_float(s, in.move_y, -1, 1, 1.0f / 127) &&
         serialize_compressed_float(s, in.up, -1, 1, 1.0f / 127) && serialize_bool(s, in.jump) && serialize_bool(s, in.boost) &&
         serialize_compressed_float(s, in.view_yaw, -360, 360, 0.1f);
}

struct InputMsg {
  u32 tick = 0;
  std::vector<PlayInput> inputs;  // newest first
  template <class S>
  bool serialize(S& s) {
    i32 n = static_cast<i32>(inputs.size());
    if (!net::serialize_u32(s, tick) || !net::serialize_int(s, n, 1, kInputHistory)) return false;
    if constexpr (S::kReading) inputs.resize(static_cast<std::size_t>(n));
    for (PlayInput& in : inputs)
      if (!serialize_input(s, in)) return false;
    return true;
  }
};

struct EntityState {
  u32 actor = 0;
  Vec3 position;
  Quat rotation;
};

struct SnapshotMsg {
  u32 tick = 0;
  f32 time = 0;                 // server simulation time
  i32 status = 0;
  f32 score = 0, elapsed = 0, remaining = -1, boost = -1, message_age = 0;
  i32 pickups = 0, pickups_total = 0;
  std::string message;
  std::vector<EntityState> entities;
  template <class S>
  bool serialize(S& s) {
    using namespace net;
    if (!serialize_u32(s, tick) || !serialize_float(s, time) || !serialize_int(s, status, 0, 3) || !serialize_float(s, score) ||
        !serialize_float(s, elapsed) || !serialize_float(s, remaining) || !serialize_compressed_float(s, boost, -1, 1, 1.0f / 255) ||
        !serialize_compressed_float(s, message_age, 0, 60, 0.05f) || !serialize_int(s, pickups, 0, 65535) || !serialize_int(s, pickups_total, 0, 65535) ||
        !serialize_string(s, message, 120))
      return false;
    i32 n = static_cast<i32>(entities.size());
    if (!serialize_int(s, n, 0, kMaxEntities)) return false;
    if constexpr (S::kReading) entities.resize(static_cast<std::size_t>(n));
    for (EntityState& e : entities)
      if (!serialize_bits(s, e.actor, 24) || !serialize_compressed_vec3(s, e.position, kWorldBound, 0.001f) || !serialize_quat(s, e.rotation, 11))
        return false;
    return true;
  }
};

constexpr int kEntityBits = 24 + 3 * 24 + 2 + 3 * 11;  // id, position (8 km at 1 mm), smallest-three quaternion

struct CueMsg {
  std::vector<SoundCue> cues;
  template <class S>
  bool serialize(S& s) {
    using namespace net;
    i32 n = static_cast<i32>(cues.size());
    if (!serialize_int(s, n, 0, 32)) return false;
    if constexpr (S::kReading) cues.resize(static_cast<std::size_t>(n));
    for (SoundCue& c : cues) {
      i32 bus = static_cast<i32>(c.bus);
      if (!serialize_string(s, c.sound, 64) || !serialize_compressed_vec3(s, c.position, kWorldBound, 0.05f) ||
          !serialize_compressed_float(s, c.volume, 0, 4, 1.0f / 255) || !serialize_compressed_float(s, c.pitch, 0, 8, 1.0f / 255) ||
          !serialize_bool(s, c.spatial) || !serialize_int(s, bus, 0, static_cast<i32>(audio::Bus::count) - 1) || !serialize_bits(s, c.actor, 32))
        return false;
      if constexpr (S::kReading) c.bus = static_cast<audio::Bus>(bus);
    }
    return true;
  }
};

net::ClientServerConfig net_config(const NetSettings& s) {
  net::ClientServerConfig c;
  c.max_clients = clamp(s.max_players, 1, 64);
  // Reliable events, unreliable state: both may carry whole snapshots.
  net::ChannelConfig reliable;
  reliable.type = net::ChannelType::reliable_ordered;
  net::ChannelConfig unreliable;
  unreliable.type = net::ChannelType::unreliable_unordered;
  unreliable.max_message_size = 2048;
  c.connection.channels = {reliable, unreliable};
  c.connection.max_packet_size = 2400;
  return c;
}

Quat nlerp(Quat a, Quat b, f32 t) {
  if (a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w < 0) b = Quat{-b.x, -b.y, -b.z, -b.w};
  Quat q{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t};
  const f32 l = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
  return l > 1e-6f ? Quat{q.x / l, q.y / l, q.z / l, q.w / l} : a;
}

}  // namespace

u32 level_checksum(const Level& level) {
  Level copy = level;
  copy.camera = {};  // the editor camera is not part of the game
  const std::string text = to_text(copy, -1);
  return net::crc32(text.data(), text.size());
}

// ---------------------------------------------------------------------------

struct NetGame::Remote {
  bool active = false;
  ActorId pawn = 0;
  u32 last_input_tick = 0;
  bool have_input = false;
  std::unordered_map<ActorId, f32> priority;
  std::unordered_map<ActorId, EntityState> last_sent;
};

struct NetGame::Entity {
  struct Sample { f64 time; Vec3 position; Quat rotation; };
  std::vector<Sample> samples;  // ascending time
};

NetGame::NetGame(GameWorld& world) : world_(world) {}
NetGame::~NetGame() { stop(); }

bool NetGame::host(const Level& level, net::Transport& transport, const NetSettings& settings, u64 seed) {
  stop();
  settings_ = settings;
  level_ = level;
  checksum_ = level_checksum(level);
  if (!world_.start(level, SimMode::play)) return false;
  server_ = std::make_unique<net::Server>(transport, net_config(settings), seed);
  server_->start(clamp(settings.max_players, 1, 64));
  remotes_.clear();
  for (int i = 0; i < server_->max_clients(); ++i) remotes_.push_back(std::make_unique<Remote>());
  role_ = NetRole::server;
  local_slot_ = 255;
  tick_ = 0;
  tick_accum_ = 0;
  stats_ = NetStats{};
  return true;
}

bool NetGame::join(const Level& level, net::Transport& transport, net::Address server, const NetSettings& settings, u64 seed) {
  stop();
  settings_ = settings;
  level_ = level;
  checksum_ = level_checksum(level);
  client_ = std::make_unique<net::Client>(transport, net_config(settings), seed);
  server_address_ = server;
  connect_pending_ = true;
  role_ = NetRole::client;
  welcomed_ = false;
  local_slot_ = -1;
  clock_valid_ = false;
  entities_.clear();
  recent_inputs_.clear();
  refused_.clear();
  stats_ = NetStats{};
  return true;
}

void NetGame::stop() {
  if (server_) server_->stop();
  if (client_) client_->disconnect();
  server_.reset();
  client_.reset();
  remotes_.clear();
  entities_.clear();
  if (role_ != NetRole::none && world_.running()) world_.stop();
  role_ = NetRole::none;
  welcomed_ = false;
}

bool NetGame::connected() const {
  if (role_ == NetRole::server) return server_ && server_->running();
  return client_ && client_->connected();
}

ActorId NetGame::local_pawn() const { return world_.player(); }

std::string NetGame::status() const {
  if (role_ == NetRole::server) return "Hosting - " + std::to_string(stats_.players) + " player" + (stats_.players == 1 ? "" : "s") + " connected";
  if (role_ == NetRole::client) {
    if (!refused_.empty()) return "Refused: " + refused_;
    if (!client_) return "Not connected";
    if (client_->connected()) return welcomed_ ? "Connected (slot " + std::to_string(local_slot_ + 1) + ")" : "Connected - waiting for the server";
    return std::string("Client ") + net::client_state_name(client_->state());
  }
  return "Offline";
}

void NetGame::update(f64 time, f32 dt, const PlayInput& input) {
  time_ = time;
  if (role_ == NetRole::server) server_update(time, dt, input);
  else if (role_ == NetRole::client) client_update(time, dt, input);
}

// ---------------------------------------------------------------------------
// server

void NetGame::server_update(f64 time, f32 dt, const PlayInput& input) {
  server_->advance_time(time);
  server_->receive_packets();
  for (const auto& [slot, joined] : server_->take_events()) {
    Remote& r = *remotes_[static_cast<std::size_t>(slot)];
    if (joined) {
      r = Remote{};
      r.active = true;
      WelcomeMsg w;
      w.checksum = checksum_;
      w.slot = slot;
      w.tick_hz = settings_.tick_hz;
      server_->send(slot, kReliable, kWelcome, w);
      // Existing pawns (the host's and other clients'), then this client's own.
      if (world_.player()) server_->send(slot, kReliable, kPawn, PawnMsg{255, static_cast<u32>(world_.player()), static_cast<u32>(world_.pawn_source(world_.player()))});
      for (std::size_t i = 0; i < remotes_.size(); ++i)
        if (remotes_[i]->active && remotes_[i]->pawn) server_->send(slot, kReliable, kPawn, PawnMsg{static_cast<i32>(i), static_cast<u32>(remotes_[i]->pawn), static_cast<u32>(world_.pawn_source(remotes_[i]->pawn))});
      for (ActorId dead : world_.dead_actors()) server_->send(slot, kReliable, kAlive, ActorMsg{static_cast<u32>(dead), false});
      r.pawn = world_.add_pawn(0);
      if (r.pawn) server_->broadcast(kReliable, kPawn, PawnMsg{slot, static_cast<u32>(r.pawn), static_cast<u32>(world_.pawn_source(r.pawn))});
    } else {
      if (r.pawn) {
        world_.remove_pawn(r.pawn);
        server_->broadcast(kReliable, kUnpawn, ActorMsg{static_cast<u32>(r.pawn), false});
      }
      r = Remote{};
    }
  }
  // Inputs: the newest wins; a jump anywhere in the redundant history still counts.
  for (int slot = 0; slot < server_->max_clients(); ++slot) {
    Remote& r = *remotes_[static_cast<std::size_t>(slot)];
    if (!r.active) continue;
    while (auto m = server_->receive(slot, kUnreliable)) {
      if (m->type != kInput) continue;
      InputMsg in;
      if (!net::read(*m, in) || in.inputs.empty()) continue;
      if (r.have_input && !net::sequence_greater(static_cast<u16>(in.tick), static_cast<u16>(r.last_input_tick)) && in.tick <= r.last_input_tick) continue;
      PlayInput latest = in.inputs.front();
      const u32 fresh = r.have_input ? min_of<u32>(in.tick - r.last_input_tick, static_cast<u32>(in.inputs.size())) : 1;
      for (u32 k = 0; k < fresh; ++k) latest.jump = latest.jump || in.inputs[k].jump;
      latest.has_view_yaw = true;
      r.last_input_tick = in.tick;
      r.have_input = true;
      if (r.pawn) world_.set_pawn_input(r.pawn, latest);
    }
    while (server_->receive(slot, kReliable)) {}  // clients send nothing reliable yet
  }
  // Collected pickups and used triggers reach every client reliably.
  const std::vector<SoundCue> cues = world_.sound_cues();  // GameAudio drains them after us
  std::vector<std::pair<ActorId, bool>> changes;
  world_.take_alive_changes(changes);
  world_.step(dt, input);
  world_.take_alive_changes(changes);
  for (const auto& [id, alive] : changes) server_->broadcast(kReliable, kAlive, ActorMsg{static_cast<u32>(id), alive});
  // Sound cues produced this frame.
  {
    std::vector<SoundCue> fresh(world_.sound_cues().begin() + static_cast<std::ptrdiff_t>(min_of(cues.size(), world_.sound_cues().size())), world_.sound_cues().end());
    for (std::size_t i = 0; i < fresh.size(); i += 8) {
      CueMsg m;  // 8 cues stay well below one packet
      m.cues.assign(fresh.begin() + static_cast<std::ptrdiff_t>(i), fresh.begin() + static_cast<std::ptrdiff_t>(min_of(fresh.size(), i + 8)));
      server_->broadcast(kUnreliable, kCues, m);
    }
  }
  tick_accum_ += dt;
  const f64 tick = 1.0 / clamp(settings_.tick_hz, 5.0f, 120.0f);
  if (tick_accum_ >= tick) {
    tick_accum_ = std::fmod(tick_accum_, tick);
    ++tick_;
    server_send_snapshots();
  }
  server_->send_packets();
  stats_.players = server_->connected_clients() + 1;
  for (int slot = 0; slot < server_->max_clients(); ++slot)
    if (net::Connection* c = server_->connection(slot)) {
      stats_.rtt_ms = c->stats().rtt_ms;
      stats_.packet_loss = c->stats().packet_loss;
      stats_.sent_kbps = c->stats().sent_kbps;
      stats_.received_kbps = c->stats().received_kbps;
    }
}

void NetGame::server_send_snapshots() {
  const Hud& h = world_.hud();
  std::vector<ActorId> moving;
  world_.networked_actors(moving);
  for (int slot = 0; slot < server_->max_clients(); ++slot) {
    Remote& r = *remotes_[static_cast<std::size_t>(slot)];
    if (!r.active || !server_->client_connected(slot)) continue;
    SnapshotMsg s;
    s.tick = tick_;
    s.time = world_.sim_time();
    s.status = static_cast<i32>(h.status);
    s.score = h.score;
    s.elapsed = h.elapsed;
    s.remaining = h.remaining;
    s.boost = clamp(h.boost, -1.0f, 1.0f);
    s.message = h.message.substr(0, 120);
    s.message_age = clamp(h.message_age, 0.0f, 60.0f);
    s.pickups = static_cast<i32>(min_of<u32>(h.pickups, 65535));
    s.pickups_total = static_cast<i32>(min_of<u32>(h.pickups_total, 65535));
    // Priority accumulator: what changed grows fast, the client's own pawn always goes first.
    std::vector<std::pair<f32, EntityState>> candidates;
    for (ActorId id : moving) {
      Mat4 m;
      if (!world_.actor_matrix(id, m) || id > 0xFFFFFF) continue;
      EntityState e;
      e.actor = static_cast<u32>(id);
      e.position = m.translation();
      e.rotation = euler_to_quat(matrix_transform(m).rotation);
      f32& p = r.priority[id];
      const auto last = r.last_sent.find(id);
      const bool changed = last == r.last_sent.end() || distance(last->second.position, e.position) > 0.001f ||
                           std::fabs(last->second.rotation.x * e.rotation.x + last->second.rotation.y * e.rotation.y + last->second.rotation.z * e.rotation.z +
                                     last->second.rotation.w * e.rotation.w) < 0.99999f;
      p += changed ? 10.0f : 0.25f;
      const f32 key = id == r.pawn ? 1e9f : p;
      candidates.push_back({key, e});
    }
    std::stable_sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    const int budget = settings_.snapshot_budget * 8;
    int bits = 0;
    for (const auto& [key, e] : candidates) {
      if (bits + kEntityBits > budget || static_cast<int>(s.entities.size()) >= kMaxEntities) break;
      s.entities.push_back(e);
      bits += kEntityBits;
      r.priority[e.actor] = 0;
      r.last_sent[e.actor] = e;
    }
    server_->send(slot, kUnreliable, kSnapshot, s);
    ++stats_.snapshots_sent;
    stats_.entities_last_snapshot = static_cast<u32>(s.entities.size());
  }
}

// ---------------------------------------------------------------------------
// client

void NetGame::client_update(f64 time, f32 dt, const PlayInput& input) {
  if (connect_pending_) {
    client_->connect(server_address_, time);
    connect_pending_ = false;
  }
  client_->advance_time(time);
  client_->receive_packets();
  if (client_->connected()) {
    while (auto m = client_->receive(kReliable)) {
      switch (m->type) {
        case kWelcome: {
          WelcomeMsg w;
          if (!net::read(*m, w)) break;
          if (w.checksum != checksum_) {
            refused_ = "the server plays a different level";
            client_->disconnect();
            return;
          }
          local_slot_ = w.slot;
          settings_.tick_hz = w.tick_hz;
          if (!world_.start_replica(level_)) { refused_ = last_error(); client_->disconnect(); return; }
          welcomed_ = true;
          break;
        }
        case kPawn: {
          PawnMsg p;
          if (!net::read(*m, p) || !welcomed_) break;
          world_.add_pawn(static_cast<ActorId>(p.source), static_cast<ActorId>(p.actor));
          if (p.slot == local_slot_) world_.set_local_pawn(static_cast<ActorId>(p.actor));
          break;
        }
        case kUnpawn: {
          ActorMsg a;
          if (net::read(*m, a) && welcomed_) { world_.remove_pawn(static_cast<ActorId>(a.actor)); entities_.erase(static_cast<ActorId>(a.actor)); }
          break;
        }
        case kAlive: {
          ActorMsg a;
          if (net::read(*m, a) && welcomed_) world_.set_alive(static_cast<ActorId>(a.actor), a.alive);
          break;
        }
        default: break;
      }
    }
    while (auto m = client_->receive(kUnreliable)) {
      if (!welcomed_) continue;
      if (m->type == kSnapshot) {
        SnapshotMsg s;
        if (!net::read(*m, s)) continue;
        ++stats_.snapshots_received;
        stats_.entities_last_snapshot = static_cast<u32>(s.entities.size());
        // Clock: server time ~= local time + offset (the smallest offset seen is the least delayed packet).
        const f64 sample = static_cast<f64>(s.time) - time;
        if (!clock_valid_) { clock_offset_ = sample; clock_valid_ = true; }
        else clock_offset_ = sample > clock_offset_ ? clock_offset_ + (sample - clock_offset_) * 0.02 : clock_offset_ + (sample - clock_offset_) * 0.2;
        server_time_ = std::max(server_time_, static_cast<f64>(s.time));
        Hud& h = world_.hud_mut();
        h.status = static_cast<GameStatus>(s.status);
        h.score = s.score;
        h.elapsed = s.elapsed;
        h.remaining = s.remaining;
        h.boost = s.boost;
        if (h.message != s.message || s.message_age < h.message_age) { h.message = s.message; }
        h.message_age = s.message_age;
        h.pickups = static_cast<u32>(s.pickups);
        h.pickups_total = static_cast<u32>(s.pickups_total);
        for (const EntityState& e : s.entities) {
          auto& ent = entities_[static_cast<ActorId>(e.actor)];
          if (!ent) ent = std::make_unique<Entity>();
          auto& v = ent->samples;
          if (!v.empty() && v.back().time >= s.time) continue;  // late or duplicate
          v.push_back({static_cast<f64>(s.time), e.position, e.rotation});
          if (v.size() > 32) v.erase(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(v.size() - 32));
        }
      } else if (m->type == kCues) {
        CueMsg c;
        if (!net::read(*m, c)) continue;
        for (SoundCue& cue : c.cues) world_.sound_cues().push_back(std::move(cue));
      }
    }
  }
  // Input to the server every tick (with the last few for redundancy).
  tick_accum_ += dt;
  const f64 tick = 1.0 / clamp(settings_.tick_hz, 5.0f, 120.0f);
  if (welcomed_ && tick_accum_ >= tick) {
    tick_accum_ = std::fmod(tick_accum_, tick);
    ++tick_;
    PlayInput in = input;
    in.has_view_yaw = true;
    in.view_yaw = std::remainder(world_.camera_yaw(), 360.0f);
    recent_inputs_.insert(recent_inputs_.begin(), in);
    if (recent_inputs_.size() > kInputHistory) recent_inputs_.resize(kInputHistory);
    InputMsg msg;
    msg.tick = tick_;
    msg.inputs = recent_inputs_;
    client_->send(kUnreliable, kInput, msg);
  }
  client_->send_packets();
  if (welcomed_) {
    const f64 render_time = time + clock_offset_ - settings_.interpolation_delay;
    apply_snapshot_buffer(render_time);
    world_.step_replica(dt, static_cast<f32>(render_time), input);
    stats_.buffer_ms = static_cast<f32>((server_time_ - render_time) * 1000.0);
  }
  if (client_) {
    const net::EndpointStats& es = client_->connection().stats();
    stats_.rtt_ms = es.rtt_ms;
    stats_.packet_loss = es.packet_loss;
    stats_.sent_kbps = es.sent_kbps;
    stats_.received_kbps = es.received_kbps;
    stats_.players = 0;
  }
}

void NetGame::apply_snapshot_buffer(f64 render_time) {
  for (auto& [id, ent] : entities_) {
    auto& v = ent->samples;
    if (v.empty()) continue;
    // Drop samples older than the one just before the render time.
    while (v.size() > 2 && v[1].time <= render_time) v.erase(v.begin());
    Vec3 p;
    Quat q;
    if (render_time <= v.front().time || v.size() == 1) {
      p = v.front().position;
      q = v.front().rotation;
    } else if (render_time >= v.back().time) {
      // Past the newest sample: extrapolate briefly along the last motion, then hold.
      const auto& a = v[v.size() - 2];
      const auto& b = v.back();
      const f64 span = b.time - a.time;
      const f64 over = std::min(render_time - b.time, 0.25);
      p = span > 1e-6 ? b.position + (b.position - a.position) * static_cast<f32>(over / span) : b.position;
      q = b.rotation;
    } else {
      const auto& a = v[0];
      const auto& b = v[1];
      const f32 t = static_cast<f32>((render_time - a.time) / std::max(1e-6, b.time - a.time));
      p = a.position + (b.position - a.position) * t;
      q = nlerp(a.rotation, b.rotation, t);
    }
    world_.set_replica_pose(id, p, q);
  }
}

}  // namespace kern::game
