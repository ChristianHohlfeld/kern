#include "kern/net/client_server.h"

namespace kern::net {

namespace {

constexpr int kHeader = 4 + 1 + 8;     // crc, type, salt
constexpr int kRequestBytes = 1000;    // padded requests: no amplification
constexpr int kMaxPending = 256;

u64 splitmix(u64& s) {
  u64 z = (s += 0x9E3779B97F4A7C15ull);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}

void put64(u8* p, u64 v) { for (int i = 0; i < 8; ++i) p[i] = static_cast<u8>(v >> (8 * i)); }
u64 get64(const u8* p) { u64 v = 0; for (int i = 0; i < 8; ++i) v |= static_cast<u64>(p[i]) << (8 * i); return v; }
void put32(u8* p, u32 v) { for (int i = 0; i < 4; ++i) p[i] = static_cast<u8>(v >> (8 * i)); }
u32 get32(const u8* p) { u32 v = 0; for (int i = 0; i < 4; ++i) v |= static_cast<u32>(p[i]) << (8 * i); return v; }

u32 packet_crc(u64 protocol, const u8* data, int bytes) {
  u8 id[8];
  put64(id, protocol);
  return crc32(data, static_cast<std::size_t>(bytes), crc32(id, 8));
}

/// Builds [crc][type][salt][body] into `out`; returns its size.
int build(u64 protocol, u8 type, u64 salt, const u8* body, int body_bytes, int pad_to, u8* out) {
  int n = kHeader + body_bytes;
  out[4] = type;
  put64(out + 5, salt);
  if (body_bytes) std::memcpy(out + kHeader, body, static_cast<std::size_t>(body_bytes));
  if (n < pad_to) { std::memset(out + n, 0, static_cast<std::size_t>(pad_to - n)); n = pad_to; }
  put32(out, packet_crc(protocol, out + 4, n - 4));
  return n;
}

/// Validates the CRC and splits a datagram.
bool parse(u64 protocol, const u8* data, int bytes, u8& type, u64& salt, const u8*& body, int& body_bytes) {
  if (bytes < kHeader || bytes > kMaxDatagram) return false;
  if (get32(data) != packet_crc(protocol, data + 4, bytes - 4)) return false;
  type = data[4];
  if (type < static_cast<u8>(PacketType::request) || type > static_cast<u8>(PacketType::disconnect)) return false;
  salt = get64(data + 5);
  body = data + kHeader;
  body_bytes = bytes - kHeader;
  return true;
}

}  // namespace

const char* client_state_name(ClientState s) {
  switch (s) {
    case ClientState::denied: return "denied";
    case ClientState::timed_out: return "timed out";
    case ClientState::error: return "error";
    case ClientState::disconnected: return "disconnected";
    case ClientState::connecting: return "connecting";
    case ClientState::challenged: return "challenged";
    case ClientState::connected: return "connected";
  }
  return "?";
}

// ---------------------------------------------------------------------------
// Client

Client::Client(Transport& transport, const ClientServerConfig& config, u64 salt_seed)
    : config_(config), transport_(transport), connection_(config.connection), rng_(salt_seed ^ 0x6b65726e636c6e74ull) {
  connection_.transmit = &Client::transmit;
  connection_.transmit_ctx = this;
}

void Client::transmit(void* ctx, const u8* data, int bytes) {
  Client* c = static_cast<Client*>(ctx);
  c->send_raw(static_cast<u8>(PacketType::payload), data, bytes);
}

void Client::send_raw(u8 type, const u8* body, int body_bytes, bool pad) {
  u8 buf[kMaxDatagram];
  if (kHeader + body_bytes > kMaxDatagram) return;
  const u64 salt = type == static_cast<u8>(PacketType::request) ? client_salt_ : (client_salt_ ^ server_salt_);
  const int n = build(config_.protocol_id, type, salt, body, body_bytes, pad ? kRequestBytes : 0, buf);
  transport_.send(server_, buf, n);
  last_send_ = time_;
}

void Client::connect(Address server, f64 time) {
  disconnect();
  server_ = server;
  time_ = time;
  client_salt_ = splitmix(rng_);
  server_salt_ = 0;
  index_ = -1;
  state_ = ClientState::connecting;
  connect_start_ = last_receive_ = time;
  last_send_ = -1e9;
  connection_.reset();
  connection_.advance_time(time);
}

void Client::disconnect() {
  if (state_ == ClientState::connected || state_ == ClientState::challenged)
    for (int i = 0; i < 3; ++i) send_raw(static_cast<u8>(PacketType::disconnect), nullptr, 0);
  if (state_ > ClientState::disconnected) state_ = ClientState::disconnected;
  index_ = -1;
}

void Client::receive_packets() {
  u8 buf[kMaxDatagram];
  Address from;
  for (int guard = 0; guard < 4096; ++guard) {
    const int n = transport_.receive(from, buf, sizeof buf);
    if (n <= 0) break;
    if (!(from == server_) || state_ <= ClientState::disconnected) continue;
    u8 type;
    u64 salt;
    const u8* body;
    int body_bytes;
    if (!parse(config_.protocol_id, buf, n, type, salt, body, body_bytes)) continue;
    const PacketType t = static_cast<PacketType>(type);
    if (t == PacketType::challenge && state_ == ClientState::connecting && salt == client_salt_ && body_bytes >= 8) {
      server_salt_ = get64(body);
      state_ = ClientState::challenged;
      last_receive_ = time_;
      last_send_ = -1e9;
      continue;
    }
    if (t == PacketType::denied && state_ == ClientState::connecting && salt == client_salt_) {
      state_ = ClientState::denied;
      continue;
    }
    if (state_ < ClientState::challenged || salt != (client_salt_ ^ server_salt_)) continue;
    last_receive_ = time_;
    if (t == PacketType::keepalive && body_bytes >= 2) {
      index_ = body[0];
      max_clients_ = body[1];
      state_ = ClientState::connected;
    } else if (t == PacketType::payload && state_ == ClientState::connected) {
      connection_.receive_packet(body, body_bytes);
      if (connection_.error() != ConnectionError::none) {
        state_ = ClientState::error;
        return;
      }
    } else if (t == PacketType::disconnect) {
      state_ = ClientState::disconnected;
      index_ = -1;
      return;
    }
  }
}

void Client::advance_time(f64 time) {
  time_ = time;
  transport_.advance_time(time);
  connection_.advance_time(time);
  if (state_ > ClientState::disconnected && state_ < ClientState::connected && time - connect_start_ > config_.connect_timeout) state_ = ClientState::timed_out;
  if (state_ == ClientState::connected && time - last_receive_ > config_.timeout) state_ = ClientState::timed_out;
}

void Client::send_packets() {
  switch (state_) {
    case ClientState::connecting:
      if (time_ - last_send_ >= config_.request_interval) send_raw(static_cast<u8>(PacketType::request), nullptr, 0, true);
      break;
    case ClientState::challenged:
      if (time_ - last_send_ >= config_.request_interval) send_raw(static_cast<u8>(PacketType::response), nullptr, 0);
      break;
    case ClientState::connected:
      connection_.send_packet();
      break;
    default: break;
  }
}

// ---------------------------------------------------------------------------
// Server

struct Server::Slot {
  explicit Slot(const ConnectionConfig& c) : connection(c) {}
  bool used = false;
  Address address;
  u64 salt = 0;
  f64 last_receive = 0, last_keepalive = -1e9;
  Connection connection;
  Server* server = nullptr;
};

Server::Server(Transport& transport, const ClientServerConfig& config, u64 salt_seed)
    : config_(config), transport_(transport), rng_(salt_seed ^ 0x6b65726e73727672ull) {}

Server::~Server() { stop(); }

void Server::start(int max_clients) {
  stop();
  max_clients_ = clamp(max_clients, 1, 255);
  slots_.clear();
  for (int i = 0; i < max_clients_; ++i) {
    auto s = std::make_unique<Slot>(config_.connection);
    s->server = this;
    s->connection.transmit = &Server::transmit;
    s->connection.transmit_ctx = s.get();
    slots_.push_back(std::move(s));
  }
  pending_.clear();
  events_.clear();
  running_ = true;
}

void Server::stop() {
  if (!running_) return;
  for (int i = 0; i < max_clients_; ++i)
    if (slots_[static_cast<std::size_t>(i)]->used) drop(i, true);
  running_ = false;
}

bool Server::client_connected(int index) const {
  return index >= 0 && index < max_clients_ && slots_[static_cast<std::size_t>(index)]->used;
}

int Server::connected_clients() const {
  int n = 0;
  for (int i = 0; i < max_clients_; ++i) n += client_connected(i);
  return n;
}

Address Server::client_address(int index) const { return client_connected(index) ? slots_[static_cast<std::size_t>(index)]->address : Address{}; }

void Server::transmit(void* ctx, const u8* data, int bytes) {
  Slot* s = static_cast<Slot*>(ctx);
  s->server->send_raw(s->address, static_cast<u8>(PacketType::payload), s->salt, data, bytes);
}

void Server::send_raw(Address to, u8 type, u64 salt, const u8* body, int body_bytes) {
  u8 buf[kMaxDatagram];
  if (kHeader + body_bytes > kMaxDatagram) return;
  const int n = build(config_.protocol_id, type, salt, body, body_bytes, 0, buf);
  transport_.send(to, buf, n);
}

int Server::find_slot(Address a) const {
  for (int i = 0; i < max_clients_; ++i)
    if (slots_[static_cast<std::size_t>(i)]->used && slots_[static_cast<std::size_t>(i)]->address == a) return i;
  return -1;
}

void Server::drop(int index, bool notify_remote) {
  Slot& s = *slots_[static_cast<std::size_t>(index)];
  if (!s.used) return;
  if (notify_remote)
    for (int i = 0; i < 3; ++i) send_raw(s.address, static_cast<u8>(PacketType::disconnect), s.salt, nullptr, 0);
  s.used = false;
  s.address = {};
  s.salt = 0;
  s.connection.reset();
  events_.push_back({index, false});
}

void Server::disconnect_client(int index) {
  if (client_connected(index)) drop(index, true);
}

void Server::receive_packets() {
  if (!running_) return;
  u8 buf[kMaxDatagram];
  Address from;
  for (int guard = 0; guard < 16384; ++guard) {
    const int n = transport_.receive(from, buf, sizeof buf);
    if (n <= 0) break;
    u8 type;
    u64 salt;
    const u8* body;
    int body_bytes;
    if (!parse(config_.protocol_id, buf, n, type, salt, body, body_bytes)) continue;
    const PacketType t = static_cast<PacketType>(type);
    const int slot = find_slot(from);
    if (t == PacketType::request) {
      if (slot >= 0 || n < kRequestBytes) continue;  // already connected, or an unpadded request
      int free = -1;
      for (int i = 0; i < max_clients_ && free < 0; ++i)
        if (!slots_[static_cast<std::size_t>(i)]->used) free = i;
      if (free < 0) { send_raw(from, static_cast<u8>(PacketType::denied), salt, nullptr, 0); continue; }
      Pending* p = nullptr;
      for (Pending& q : pending_)
        if (q.address == from && q.client_salt == salt) p = &q;
      if (!p) {
        if (static_cast<int>(pending_.size()) >= kMaxPending) pending_.erase(pending_.begin());
        pending_.push_back({from, salt, splitmix(rng_), time_});
        p = &pending_.back();
      }
      p->time = time_;
      u8 b[8];
      put64(b, p->server_salt);
      send_raw(from, static_cast<u8>(PacketType::challenge), salt, b, 8);
      continue;
    }
    if (t == PacketType::response) {
      if (slot >= 0) {
        // Our keepalive got lost: tell the client again.
        Slot& s = *slots_[static_cast<std::size_t>(slot)];
        if (s.salt == salt) s.last_keepalive = -1e9;
        continue;
      }
      for (std::size_t i = 0; i < pending_.size(); ++i) {
        const Pending& p = pending_[i];
        if (!(p.address == from) || (p.client_salt ^ p.server_salt) != salt) continue;
        int free = -1;
        for (int k = 0; k < max_clients_ && free < 0; ++k)
          if (!slots_[static_cast<std::size_t>(k)]->used) free = k;
        if (free < 0) { send_raw(from, static_cast<u8>(PacketType::denied), p.client_salt, nullptr, 0); break; }
        Slot& s = *slots_[static_cast<std::size_t>(free)];
        s.used = true;
        s.address = from;
        s.salt = salt;
        s.last_receive = time_;
        s.last_keepalive = -1e9;
        s.connection.reset();
        s.connection.advance_time(time_);
        pending_.erase(pending_.begin() + static_cast<std::ptrdiff_t>(i));
        events_.push_back({free, true});
        break;
      }
      continue;
    }
    if (slot < 0) continue;
    Slot& s = *slots_[static_cast<std::size_t>(slot)];
    if (s.salt != salt) continue;
    s.last_receive = time_;
    if (t == PacketType::payload) {
      s.connection.receive_packet(body, body_bytes);
      if (s.connection.error() != ConnectionError::none) drop(slot, true);
    } else if (t == PacketType::disconnect) {
      drop(slot, false);
    }
  }
}

void Server::advance_time(f64 time) {
  time_ = time;
  transport_.advance_time(time);
  if (!running_) return;
  for (int i = 0; i < max_clients_; ++i) {
    Slot& s = *slots_[static_cast<std::size_t>(i)];
    if (!s.used) continue;
    s.connection.advance_time(time);
    if (time - s.last_receive > config_.timeout) drop(i, true);
  }
  for (std::size_t i = 0; i < pending_.size();)
    if (time - pending_[i].time > config_.connect_timeout) pending_.erase(pending_.begin() + static_cast<std::ptrdiff_t>(i));
    else ++i;
}

void Server::send_packets() {
  if (!running_) return;
  for (int i = 0; i < max_clients_; ++i) {
    Slot& s = *slots_[static_cast<std::size_t>(i)];
    if (!s.used) continue;
    if (time_ - s.last_keepalive >= 0.5) {
      const u8 b[2] = {static_cast<u8>(i), static_cast<u8>(max_clients_)};
      send_raw(s.address, static_cast<u8>(PacketType::keepalive), s.salt, b, 2);
      s.last_keepalive = time_;
    }
    s.connection.send_packet();
  }
}

bool Server::send(int client, int channel, u16 type, std::vector<u8> data) {
  if (!client_connected(client)) return false;
  return slots_[static_cast<std::size_t>(client)]->connection.send(channel, type, std::move(data));
}

std::optional<Message> Server::receive(int client, int channel) {
  if (!client_connected(client)) return std::nullopt;
  return slots_[static_cast<std::size_t>(client)]->connection.receive(channel);
}

Connection* Server::connection(int client) {
  return client_connected(client) ? &slots_[static_cast<std::size_t>(client)]->connection : nullptr;
}

}  // namespace kern::net
