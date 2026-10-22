// Packet acks, round-trip time, packet loss, bandwidth and fragmentation on
// top of an unreliable datagram transport (Gaffer On Games "Reliability and
// Congestion Avoidance over UDP", "Packet Fragmentation and Reassembly";
// reliable.io).
//
// Every packet carries its 16-bit sequence and the receiver's view of the
// remote side: the most recent sequence received (ack) and 32 bits for the
// 32 before it. Sending a packet never waits; the layer above (channels)
// decides what to resend when a packet's ack never comes. Packets above
// `fragment_above` bytes are split into fragments and reassembled.
#pragma once

#include <vector>

#include "kern/net/serialize.h"

namespace kern::net {

/// Ring buffer indexed by 16-bit sequence numbers (wrap-around safe).
template <class T>
class SequenceBuffer {
 public:
  explicit SequenceBuffer(int size = 256) { resize(size); }
  void resize(int size) {
    entries_.assign(static_cast<std::size_t>(size), T{});
    sequences_.assign(static_cast<std::size_t>(size), kEmpty);
    sequence_ = 0;
  }
  void reset() {
    sequence_ = 0;
    std::fill(sequences_.begin(), sequences_.end(), kEmpty);
    for (T& e : entries_) e = T{};
  }
  int size() const { return static_cast<int>(entries_.size()); }
  /// The next sequence after the newest inserted one.
  u16 sequence() const { return sequence_; }
  /// Inserts (or replaces) `seq`; nullptr when it is too old for the window.
  T* insert(u16 seq) {
    if (sequence_less(seq, static_cast<u16>(sequence_ - size()))) return nullptr;
    if (sequence_greater(static_cast<u16>(seq + 1), sequence_)) {
      remove_range(sequence_, seq);
      sequence_ = static_cast<u16>(seq + 1);
    }
    const std::size_t i = seq % entries_.size();
    sequences_[i] = seq;
    entries_[i] = T{};
    return &entries_[i];
  }
  void remove(u16 seq) { sequences_[seq % entries_.size()] = kEmpty; }
  bool available(u16 seq) const { return sequences_[seq % entries_.size()] == kEmpty; }
  bool exists(u16 seq) const { return sequences_[seq % entries_.size()] == seq; }
  T* find(u16 seq) {
    const std::size_t i = seq % entries_.size();
    return sequences_[i] == seq ? &entries_[i] : nullptr;
  }
  const T* find(u16 seq) const {
    const std::size_t i = seq % entries_.size();
    return sequences_[i] == seq ? &entries_[i] : nullptr;
  }
  T& at_index(int i) { return entries_[static_cast<std::size_t>(i)]; }
  /// ack = newest received sequence; bit n of ack_bits = ack - 1 - n received.
  void generate_ack_bits(u16& ack, u32& ack_bits) const {
    ack = static_cast<u16>(sequence_ - 1);
    ack_bits = 0;
    for (u32 i = 0; i < 32; ++i)
      if (exists(static_cast<u16>(ack - 1 - i))) ack_bits |= 1u << i;
  }

 private:
  static constexpr u32 kEmpty = 0xFFFFFFFFu;
  void remove_range(u16 from, u16 to) {
    const int n = static_cast<u16>(to - from) + 1;
    if (n >= size()) { std::fill(sequences_.begin(), sequences_.end(), kEmpty); return; }
    for (int i = 0; i < n; ++i) remove(static_cast<u16>(from + i));
  }
  std::vector<T> entries_;
  std::vector<u32> sequences_;
  u16 sequence_ = 0;
};

struct EndpointConfig {
  int max_packet_size = 16 * 1024;     // payload bytes (before fragmentation)
  int fragment_above = 1024;           // bigger packets are fragmented
  int fragment_size = 1024;
  int max_fragments = 16;
  int ack_buffer_size = 256;
  int sent_packets_buffer_size = 256;
  int received_packets_buffer_size = 256;
  int fragment_reassembly_buffer_size = 64;
  f32 rtt_smoothing = 0.1f;            // per acked packet
  f32 packet_loss_smoothing = 0.1f;
  f32 bandwidth_smoothing = 0.1f;
  int packet_header_size = 28;         // IPv4 + UDP for bandwidth estimates
};

struct EndpointStats {
  f32 rtt_ms = 0;                      // smoothed round trip
  f32 packet_loss = 0;                 // percent
  f32 sent_kbps = 0, received_kbps = 0, acked_kbps = 0;
  u64 packets_sent = 0, packets_received = 0, packets_acked = 0, packets_stale = 0, packets_invalid = 0,
      packets_too_large_to_send = 0, packets_too_large_to_receive = 0, fragments_sent = 0, fragments_received = 0,
      fragments_invalid = 0;
};

class Endpoint {
 public:
  /// Called with each datagram to put on the wire.
  using Transmit = void (*)(void* ctx, u16 sequence, const u8* packet, int bytes);
  /// Called with each received payload; return true to ack it (false: rejected, not acked).
  using Process = bool (*)(void* ctx, u16 sequence, const u8* payload, int bytes);

  Endpoint(const EndpointConfig& config, Transmit transmit, Process process, void* ctx, f64 time = 0);
  void reset();
  u16 next_sequence() const { return sequence_; }
  /// Sends a payload (fragmented when needed). Returns its sequence.
  u16 send_packet(const u8* payload, int bytes);
  void receive_packet(const u8* packet, int bytes);
  /// Sequences of our packets the remote side has acked since the last clear_acks().
  const std::vector<u16>& acks() const { return acks_; }
  void clear_acks() { acks_.clear(); }
  void update(f64 time);
  f64 time() const { return time_; }
  const EndpointStats& stats() const { return stats_; }
  const EndpointConfig& config() const { return config_; }

 private:
  struct Sent { f64 time = 0; bool acked = false; u32 bytes = 0; };
  struct Received { f64 time = 0; u32 bytes = 0; };
  struct Reassembly {
    u16 sequence = 0;
    int ack_header_bytes = 0;  // unused (kept for layout clarity)
    int num_fragments = 0, received = 0, bytes = 0;
    bool got[256] = {};
    std::vector<u8> data;
  };
  int write_header(u8* out, u16 sequence, u16 ack, u32 ack_bits) const;
  int read_header(const u8* in, int bytes, u16& sequence, u16& ack, u32& ack_bits) const;
  void process_regular(const u8* packet, int bytes);

  EndpointConfig config_;
  Transmit transmit_;
  Process process_;
  void* ctx_;
  f64 time_;
  u16 sequence_ = 0;
  std::vector<u16> acks_;
  SequenceBuffer<Sent> sent_;
  SequenceBuffer<Received> received_;
  SequenceBuffer<Reassembly> reassembly_;
  EndpointStats stats_;
  std::vector<u8> scratch_;
};

}  // namespace kern::net
