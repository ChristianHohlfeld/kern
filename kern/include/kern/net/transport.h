// Datagram transports and a network simulator.
//
// Kern's network stack only needs "send these bytes to that address" and
// "what arrived". Addresses are opaque 64-bit ids the transport assigns:
//
//   LoopbackNetwork    in-process hub (tests, Kern Studio's multi-player Play
//                      with server and clients in one module)
//   HostTransport      queues the browser host fills and drains (BroadcastChannel
//                      between tabs, a WebSocket relay or WebRTC data channels;
//                      see kern/web/net.mjs)
//
// Every transport can run through the Simulator: latency, jitter, loss,
// duplicates and reordering with a seeded RNG, so bad networks are
// reproducible in tests and selectable in the editor.
#pragma once

#include <deque>
#include <memory>
#include <vector>

#include "kern/core/base.h"

namespace kern::net {

struct Address {
  u64 id = 0;  // 0 = none
  bool valid() const { return id != 0; }
  bool operator==(const Address&) const = default;
};

constexpr int kMaxDatagram = 1400;  // conservative MTU for every transport

class Transport {
 public:
  virtual ~Transport() = default;
  virtual Address address() const = 0;
  virtual bool send(Address to, const u8* data, int bytes) = 0;
  /// Next received datagram (bytes, 0 = none); `from` is the sender.
  virtual int receive(Address& from, u8* data, int max_bytes) = 0;
  /// Moves simulated time (delivers delayed packets).
  virtual void advance_time(f64 time) { (void)time; }
};

struct SimulatorSettings {
  f32 latency_ms = 0;        // one way
  f32 jitter_ms = 0;         // +- uniform
  f32 loss_percent = 0;
  f32 duplicate_percent = 0;
  bool active() const { return latency_ms > 0 || jitter_ms > 0 || loss_percent > 0 || duplicate_percent > 0; }
};

/// Delays and damages datagrams deterministically.
class Simulator {
 public:
  explicit Simulator(u32 seed = 1) : rng_(seed ? seed : 1) {}
  SimulatorSettings settings;
  struct Packet {
    Address from, to;
    f64 deliver = 0;
    std::vector<u8> data;
  };
  void send(Address from, Address to, const u8* data, int bytes, f64 now);
  /// Packets due at `now`, in delivery order.
  void take_due(f64 now, std::vector<Packet>& out);
  void clear() { queue_.clear(); }
  std::size_t in_flight() const { return queue_.size(); }
  u64 dropped() const { return dropped_; }
  u64 duplicated() const { return duplicated_; }

 private:
  f32 random01();
  u32 rng_;
  std::vector<Packet> queue_;
  u64 serial_ = 0, dropped_ = 0, duplicated_ = 0;
};

/// In-process network: endpoints created from one hub talk to each other.
class LoopbackNetwork {
 public:
  explicit LoopbackNetwork(u32 seed = 1) : sim_(seed) {}
  /// A new endpoint with a fresh address (owned by the caller, must not outlive the hub).
  std::unique_ptr<Transport> create();
  Simulator& simulator() { return sim_; }
  void advance_time(f64 time);
  f64 time() const { return time_; }
  u64 delivered() const { return delivered_; }

 private:
  friend class LoopbackTransport;
  struct Inbox { Address address; std::deque<std::pair<Address, std::vector<u8>>> packets; bool open = true; };
  Inbox* inbox(Address a);
  Simulator sim_;
  std::vector<std::unique_ptr<Inbox>> inboxes_;
  f64 time_ = 0;
  u64 next_ = 1, delivered_ = 0;
  std::vector<Simulator::Packet> due_;
};

/// Datagrams exchanged with the embedding host (the browser page).
class HostTransport final : public Transport {
 public:
  explicit HostTransport(Address self = {1}) : self_(self) {}
  Address address() const override { return self_; }
  void set_address(Address a) { self_ = a; }
  bool send(Address to, const u8* data, int bytes) override;
  int receive(Address& from, u8* data, int max_bytes) override;
  void advance_time(f64 time) override;
  /// Host side: a datagram arrived from `from`.
  void push_incoming(Address from, const u8* data, int bytes);
  /// Host side: the next datagram to put on the wire (false when none).
  bool take_outgoing(Address& to, std::vector<u8>& data);
  Simulator& simulator() { return sim_; }

 private:
  Address self_;
  Simulator sim_;
  f64 time_ = 0;
  std::deque<std::pair<Address, std::vector<u8>>> in_, out_;
  std::vector<Simulator::Packet> due_;
};

}  // namespace kern::net
