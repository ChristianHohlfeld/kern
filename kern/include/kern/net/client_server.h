// Client / server connections over any Transport (Gaffer On Games "Client
// Server Connection", yojimbo's client and server).
//
// Handshake: the client sends connection requests carrying a random salt
// (padded to full size, so a spoofed request never makes the server send
// more than it received); the server answers with a challenge holding its
// own salt; the client echoes salt XOR challenge; the server assigns a slot
// and both sides use the XOR as the connection's id on every packet. Packets
// from unknown addresses or with the wrong id are ignored. Idle connections
// time out; either side can disconnect (sent redundantly).
//
// After the handshake, payload packets carry a Connection (channels with
// reliable-ordered and unreliable messages). Every datagram starts with a
// CRC32 over the protocol id and the packet, so traffic of another game or
// version is dropped before parsing.
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "kern/net/connection.h"
#include "kern/net/transport.h"

namespace kern::net {

struct ClientServerConfig {
  u64 protocol_id = 0x4b45524e30303031ull;  // "KERN0001": different games / versions do not talk
  int max_clients = 16;
  f64 timeout = 6;                           // seconds without packets
  f64 request_interval = 0.1;                // handshake resend
  f64 connect_timeout = 6;
  f64 keepalive_interval = 0.1;              // payload packets go out at least this often
  ConnectionConfig connection;
};

enum class ClientState : i8 {
  denied = -3,          // server full
  timed_out = -2,
  error = -1,           // connection error (desync, bad packets)
  disconnected = 0,
  connecting = 1,       // sending requests
  challenged = 2,       // sending responses
  connected = 3,
};
const char* client_state_name(ClientState s);

class Client {
 public:
  Client(Transport& transport, const ClientServerConfig& config, u64 salt_seed);
  void connect(Address server, f64 time);
  void disconnect();
  ClientState state() const { return state_; }
  bool connected() const { return state_ == ClientState::connected; }
  int client_index() const { return index_; }
  int max_clients() const { return max_clients_; }

  /// Per frame: receive, advance, send.
  void receive_packets();
  void advance_time(f64 time);
  void send_packets();

  bool send(int channel, u16 type, std::vector<u8> data) { return connected() && connection_.send(channel, type, std::move(data)); }
  template <class T>
  bool send(int channel, u16 type, const T& value) { return connected() && connection_.send(channel, type, value); }
  std::optional<Message> receive(int channel) { return connected() ? connection_.receive(channel) : std::nullopt; }
  Connection& connection() { return connection_; }
  const Connection& connection() const { return connection_; }
  Address server() const { return server_; }

 private:
  static void transmit(void* ctx, const u8* data, int bytes);
  void send_raw(u8 type, const u8* body, int body_bytes, bool pad = false);
  ClientServerConfig config_;
  Transport& transport_;
  Connection connection_;
  ClientState state_ = ClientState::disconnected;
  Address server_;
  u64 rng_;
  u64 client_salt_ = 0, server_salt_ = 0;
  int index_ = -1, max_clients_ = 0;
  f64 time_ = 0, last_send_ = -1e9, last_receive_ = 0, connect_start_ = 0;
};

class Server {
 public:
  Server(Transport& transport, const ClientServerConfig& config, u64 salt_seed);
  ~Server();
  void start(int max_clients);
  void stop();
  bool running() const { return running_; }
  int max_clients() const { return max_clients_; }
  bool client_connected(int index) const;
  int connected_clients() const;
  Address client_address(int index) const;
  void disconnect_client(int index);

  void receive_packets();
  void advance_time(f64 time);
  void send_packets();

  bool send(int client, int channel, u16 type, std::vector<u8> data);
  template <class T>
  bool send(int client, int channel, u16 type, const T& value) {
    std::vector<u8> bytes;
    return write_message(value, bytes) && send(client, channel, type, std::move(bytes));
  }
  /// Same message to every connected client.
  template <class T>
  void broadcast(int channel, u16 type, const T& value) {
    std::vector<u8> bytes;
    if (!write_message(value, bytes)) return;
    for (int i = 0; i < max_clients_; ++i)
      if (client_connected(i)) send(i, channel, type, bytes);
  }
  std::optional<Message> receive(int client, int channel);
  Connection* connection(int client);

  /// Connection changes since the last call: (index, connected).
  std::vector<std::pair<int, bool>> take_events() { return std::move(events_); }

 private:
  struct Slot;
  struct Pending { Address address; u64 client_salt = 0, server_salt = 0; f64 time = 0; };
  static void transmit(void* ctx, const u8* data, int bytes);
  void send_raw(Address to, u8 type, u64 salt, const u8* body, int body_bytes);
  int find_slot(Address a) const;
  void drop(int index, bool notify_remote);

  ClientServerConfig config_;
  Transport& transport_;
  std::vector<std::unique_ptr<Slot>> slots_;
  std::vector<Pending> pending_;
  std::vector<std::pair<int, bool>> events_;
  u64 rng_;
  int max_clients_ = 0;
  bool running_ = false;
  f64 time_ = 0;
};

/// Packet types (first byte after the CRC).
enum class PacketType : u8 { request = 1, denied, challenge, response, keepalive, payload, disconnect };

}  // namespace kern::net
