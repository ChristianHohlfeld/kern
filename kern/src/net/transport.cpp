#include "kern/net/transport.h"

#include <algorithm>

namespace kern::net {

// ---------------------------------------------------------------------------
// Simulator

f32 Simulator::random01() {
  // xorshift32
  rng_ ^= rng_ << 13;
  rng_ ^= rng_ >> 17;
  rng_ ^= rng_ << 5;
  return static_cast<f32>(rng_ >> 8) / static_cast<f32>(1u << 24);
}

void Simulator::send(Address from, Address to, const u8* data, int bytes, f64 now) {
  if (bytes <= 0) return;
  if (settings.loss_percent > 0 && random01() * 100.0f < settings.loss_percent) { ++dropped_; return; }
  const int copies = settings.duplicate_percent > 0 && random01() * 100.0f < settings.duplicate_percent ? 2 : 1;
  if (copies == 2) ++duplicated_;
  for (int c = 0; c < copies; ++c) {
    f64 delay = settings.latency_ms / 1000.0;
    if (settings.jitter_ms > 0) delay += (random01() * 2 - 1) * settings.jitter_ms / 1000.0;
    if (c == 1) delay += random01() * 0.05;  // the duplicate trails a little
    Packet p;
    p.from = from;
    p.to = to;
    p.deliver = now + max_of(0.0, delay) + static_cast<f64>(serial_++) * 1e-9;  // stable order for equal times
    p.data.assign(data, data + bytes);
    queue_.push_back(std::move(p));
  }
}

void Simulator::take_due(f64 now, std::vector<Packet>& out) {
  out.clear();
  if (queue_.empty()) return;
  std::sort(queue_.begin(), queue_.end(), [](const Packet& a, const Packet& b) { return a.deliver < b.deliver; });
  std::size_t n = 0;
  while (n < queue_.size() && queue_[n].deliver <= now) ++n;
  out.assign(std::make_move_iterator(queue_.begin()), std::make_move_iterator(queue_.begin() + static_cast<std::ptrdiff_t>(n)));
  queue_.erase(queue_.begin(), queue_.begin() + static_cast<std::ptrdiff_t>(n));
}

// ---------------------------------------------------------------------------
// Loopback

class LoopbackTransport final : public Transport {
 public:
  LoopbackTransport(LoopbackNetwork& hub, Address a) : hub_(hub), self_(a) {}
  ~LoopbackTransport() override {
    if (LoopbackNetwork::Inbox* in = hub_.inbox(self_)) { in->open = false; in->packets.clear(); }
  }
  Address address() const override { return self_; }
  bool send(Address to, const u8* data, int bytes) override {
    if (bytes <= 0 || bytes > kMaxDatagram || !to.valid()) return false;
    hub_.sim_.send(self_, to, data, bytes, hub_.time_);
    return true;
  }
  int receive(Address& from, u8* data, int max_bytes) override {
    LoopbackNetwork::Inbox* in = hub_.inbox(self_);
    if (!in || in->packets.empty()) return 0;
    auto& [src, bytes] = in->packets.front();
    const int n = static_cast<int>(bytes.size());
    if (n > max_bytes) { in->packets.pop_front(); return 0; }
    from = src;
    std::memcpy(data, bytes.data(), bytes.size());
    in->packets.pop_front();
    return n;
  }

 private:
  LoopbackNetwork& hub_;
  Address self_;
};

LoopbackNetwork::Inbox* LoopbackNetwork::inbox(Address a) {
  for (auto& in : inboxes_)
    if (in->address == a) return in.get();
  return nullptr;
}

std::unique_ptr<Transport> LoopbackNetwork::create() {
  const Address a{next_++};
  auto in = std::make_unique<Inbox>();
  in->address = a;
  inboxes_.push_back(std::move(in));
  return std::make_unique<LoopbackTransport>(*this, a);
}

void LoopbackNetwork::advance_time(f64 time) {
  time_ = time;
  sim_.take_due(time, due_);
  for (Simulator::Packet& p : due_) {
    Inbox* in = inbox(p.to);
    if (!in || !in->open) continue;
    if (in->packets.size() >= 4096) in->packets.pop_front();  // nobody reads: keep the newest
    in->packets.emplace_back(p.from, std::move(p.data));
    ++delivered_;
  }
}

// ---------------------------------------------------------------------------
// Host

bool HostTransport::send(Address to, const u8* data, int bytes) {
  if (bytes <= 0 || bytes > kMaxDatagram || !to.valid()) return false;
  if (out_.size() >= 4096) out_.pop_front();
  out_.emplace_back(to, std::vector<u8>(data, data + bytes));
  return true;
}

int HostTransport::receive(Address& from, u8* data, int max_bytes) {
  if (in_.empty()) return 0;
  auto& [src, bytes] = in_.front();
  const int n = static_cast<int>(bytes.size());
  if (n <= max_bytes) {
    from = src;
    std::memcpy(data, bytes.data(), bytes.size());
  }
  in_.pop_front();
  return n <= max_bytes ? n : 0;
}

void HostTransport::advance_time(f64 time) {
  time_ = time;
  sim_.take_due(time, due_);
  for (Simulator::Packet& p : due_) {
    if (in_.size() >= 4096) in_.pop_front();
    in_.emplace_back(p.from, std::move(p.data));
  }
}

void HostTransport::push_incoming(Address from, const u8* data, int bytes) {
  if (bytes <= 0 || bytes > kMaxDatagram) return;
  if (sim_.settings.active()) { sim_.send(from, self_, data, bytes, time_); return; }
  if (in_.size() >= 4096) in_.pop_front();
  in_.emplace_back(from, std::vector<u8>(data, data + bytes));
}

bool HostTransport::take_outgoing(Address& to, std::vector<u8>& data) {
  if (out_.empty()) return false;
  to = out_.front().first;
  data = std::move(out_.front().second);
  out_.pop_front();
  return true;
}

}  // namespace kern::net
