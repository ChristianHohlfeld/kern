// Messages and channels over the reliability layer (yojimbo style).
//
// A Connection owns an Endpoint and a set of channels:
//
//   reliable ordered     every message arrives exactly once and in order;
//                        unacked messages are resent (after ~RTT) until the
//                        packet carrying them is acked. Messages larger than
//                        `block_threshold` travel as fragmented blocks, one
//                        block at a time, each fragment resent until acked.
//   unreliable unordered sent once in the next packet (state, inputs, events
//                        that are stale when late).
//
// Messages are typed byte blobs. Typed structs go through serialize():
//
//   struct Chat { std::string text;
//     template <class S> bool serialize(S& s) { return serialize_string(s, text, 200); } };
//   conn.send(kReliable, kChat, Chat{"hi"});
//   while (auto m = conn.receive(kReliable)) { Chat c; if (m->type == kChat && read(*m, c)) ... }
#pragma once

#include <deque>
#include <memory>
#include <optional>
#include <vector>

#include "kern/net/reliable.h"

namespace kern::net {

enum class ChannelType : u8 { reliable_ordered, unreliable_unordered };

struct ChannelConfig {
  ChannelType type = ChannelType::reliable_ordered;
  int send_queue_size = 1024;            // messages (reliable: window)
  int receive_queue_size = 1024;
  int max_messages_per_packet = 256;
  int packet_budget = 0;                 // bytes per packet for this channel (0 = whatever is left)
  int max_message_size = 64 * 1024;      // larger messages are refused
  int block_threshold = 512;             // reliable: bigger messages go as blocks
  int block_fragment_size = 1024;
  f32 resend_time = 0.1f;                // seconds (at least; RTT * 1.25 when known)
};

struct ConnectionConfig {
  std::vector<ChannelConfig> channels{ChannelConfig{}, ChannelConfig{ChannelType::unreliable_unordered}};
  int max_packet_size = 8 * 1024;        // message payload per packet
  int max_message_types = 256;
  EndpointConfig endpoint;
};

struct Message {
  u16 id = 0;                            // reliable: sequence within the channel
  u16 type = 0;
  std::vector<u8> data;
};

/// Serialises `value` (serialize(WriteStream&)) into a message payload.
template <class T>
bool write_message(const T& value, std::vector<u8>& out, int max_bytes = 64 * 1024) {
  std::vector<u8> buf(static_cast<std::size_t>((max_bytes + 3) / 4 * 4));
  WriteStream w(buf.data(), static_cast<int>(buf.size()));
  if (!const_cast<T&>(value).serialize(w)) return false;
  w.flush();
  if (w.overflow()) return false;
  buf.resize(static_cast<std::size_t>(w.bytes_processed()));
  out = std::move(buf);
  return true;
}
template <class T>
bool read(const Message& m, T& out) {
  ReadStream r(m.data.data(), static_cast<int>(m.data.size()));
  return out.serialize(r);
}

enum class ConnectionError : u8 { none, desync, send_queue_full, message_too_large, bad_packet, out_of_memory };
const char* connection_error_name(ConnectionError e);

class Channel;

class Connection {
 public:
  explicit Connection(const ConnectionConfig& config = {}, f64 time = 0);
  ~Connection();
  Connection(const Connection&) = delete;
  Connection& operator=(const Connection&) = delete;

  void reset();
  bool can_send(int channel) const;
  /// Queues a message; false (and an error) when the channel cannot take it.
  bool send(int channel, u16 type, std::vector<u8> data);
  template <class T>
  bool send(int channel, u16 type, const T& value) {
    std::vector<u8> bytes;
    return write_message(value, bytes, config_.channels[static_cast<std::size_t>(channel)].max_message_size) && send(channel, type, std::move(bytes));
  }
  std::optional<Message> receive(int channel);

  /// Builds the next packet payload (messages of every channel) and sends it through the endpoint.
  void send_packet();
  /// Hands a datagram received from the remote side to the endpoint.
  void receive_packet(const u8* data, int bytes);
  void advance_time(f64 time);
  ConnectionError error() const { return error_; }
  const EndpointStats& stats() const { return endpoint_.stats(); }
  Endpoint& endpoint() { return endpoint_; }
  int channel_count() const { return static_cast<int>(channels_.size()); }
  /// Messages waiting to be acked / sent in a channel.
  int pending(int channel) const;

  /// Where datagrams go (set by Client / Server or a test harness).
  void (*transmit)(void* ctx, const u8* data, int bytes) = nullptr;
  void* transmit_ctx = nullptr;

 private:
  static void on_transmit(void* ctx, u16 sequence, const u8* packet, int bytes);
  static bool on_process(void* ctx, u16 sequence, const u8* payload, int bytes);
  bool process_payload(u16 sequence, const u8* payload, int bytes);

  ConnectionConfig config_;
  Endpoint endpoint_;
  std::vector<std::unique_ptr<Channel>> channels_;
  ConnectionError error_ = ConnectionError::none;
  f64 time_ = 0;
  std::vector<u8> packet_;
};

}  // namespace kern::net
