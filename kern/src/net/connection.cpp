#include "kern/net/connection.h"

#include <algorithm>

namespace kern::net {

const char* connection_error_name(ConnectionError e) {
  switch (e) {
    case ConnectionError::none: return "none";
    case ConnectionError::desync: return "desync";
    case ConnectionError::send_queue_full: return "send queue full";
    case ConnectionError::message_too_large: return "message too large";
    case ConnectionError::bad_packet: return "bad packet";
    case ConnectionError::out_of_memory: return "out of memory";
  }
  return "?";
}

// ---------------------------------------------------------------------------
// wire entries (one per channel per packet)

namespace {

struct WireMessage {
  u16 id = 0, type = 0;
  std::vector<u8> data;
};

struct WireFragment {
  u16 id = 0, type = 0;
  u16 fragment = 0, count = 0;
  std::vector<u8> data;
};

struct ChannelEntry {
  int channel = 0;
  bool block = false;
  std::vector<WireMessage> messages;
  std::vector<WireFragment> fragments;
};

constexpr int kMaxFragmentsPerPacket = 4;
constexpr int kMaxBlockFragments = 1024;

template <class S>
bool serialize_entry(S& s, ChannelEntry& e, const ConnectionConfig& c) {
  if (!serialize_int(s, e.channel, 0, max_of(1, static_cast<int>(c.channels.size()) - 1))) return false;
  if constexpr (S::kReading) if (e.channel >= static_cast<int>(c.channels.size())) return false;
  const ChannelConfig& cc = c.channels[static_cast<std::size_t>(e.channel)];
  const bool reliable = cc.type == ChannelType::reliable_ordered;
  if (reliable && !serialize_bool(s, e.block)) return false;
  if (e.block) {
    i32 n = static_cast<i32>(e.fragments.size());
    if (!serialize_int(s, n, 1, kMaxFragmentsPerPacket)) return false;
    if constexpr (S::kReading) e.fragments.resize(static_cast<std::size_t>(n));
    for (WireFragment& f : e.fragments) {
      if (!serialize_u16(s, f.id)) return false;
      if (!serialize_int(s, f.type, 0, c.max_message_types - 1)) return false;
      if (!serialize_int(s, f.count, 1, kMaxBlockFragments)) return false;
      if (!serialize_int(s, f.fragment, 0, kMaxBlockFragments - 1)) return false;
      if (f.fragment >= f.count) return false;
      i32 size = static_cast<i32>(f.data.size());
      if (!serialize_int(s, size, 1, cc.block_fragment_size)) return false;
      if constexpr (S::kReading) f.data.resize(static_cast<std::size_t>(size));
      if (!s.serialize_bytes(f.data.data(), size)) return false;
    }
    return true;
  }
  i32 n = static_cast<i32>(e.messages.size());
  if (!serialize_int(s, n, 1, cc.max_messages_per_packet)) return false;
  if constexpr (S::kReading) e.messages.resize(static_cast<std::size_t>(n));
  const int max_size = reliable ? cc.block_threshold : cc.max_message_size;
  for (std::size_t i = 0; i < e.messages.size(); ++i) {
    WireMessage& m = e.messages[i];
    if (reliable) {
      if (i == 0) { if (!serialize_u16(s, m.id)) return false; }
      else if (!serialize_sequence_relative(s, e.messages[i - 1].id, m.id)) return false;
    }
    if (!serialize_int(s, m.type, 0, c.max_message_types - 1)) return false;
    i32 size = static_cast<i32>(m.data.size());
    if (!serialize_int(s, size, 0, max_size)) return false;
    if constexpr (S::kReading) m.data.resize(static_cast<std::size_t>(size));
    if (size && !s.serialize_bytes(m.data.data(), size)) return false;
  }
  return true;
}

int entry_bits(const ChannelEntry& e, const ConnectionConfig& c) {
  MeasureStream m;
  serialize_entry(m, const_cast<ChannelEntry&>(e), c);
  return m.bits_processed();
}

}  // namespace

// ---------------------------------------------------------------------------
// channels

class Channel {
 public:
  Channel(const ChannelConfig& config, const ConnectionConfig& conn, int index) : config_(config), conn_(conn), index_(index) {}
  virtual ~Channel() = default;
  virtual void reset() = 0;
  virtual bool can_send() const = 0;
  virtual bool send(Message m, ConnectionError& error) = 0;
  virtual std::optional<Message> receive() = 0;
  /// Fills `entry` for packet `sequence` within `budget_bits`; false when nothing to send.
  virtual bool packet_data(u16 sequence, f64 time, f32 rtt_ms, int budget_bits, ChannelEntry& entry) = 0;
  virtual bool process(const ChannelEntry& entry, ConnectionError& error) = 0;
  virtual void ack(u16 sequence) = 0;
  virtual int pending() const = 0;

 protected:
  ChannelConfig config_;
  const ConnectionConfig& conn_;
  int index_;
};

namespace {

class ReliableChannel final : public Channel {
 public:
  ReliableChannel(const ChannelConfig& c, const ConnectionConfig& conn, int index)
      : Channel(c, conn, index), send_queue_(c.send_queue_size), sent_packets_(1024), receive_queue_(c.receive_queue_size) {}

  void reset() override {
    send_queue_.reset();
    sent_packets_.reset();
    receive_queue_.reset();
    send_id_ = oldest_unacked_ = receive_id_ = 0;
    out_block_ = OutBlock{};
    in_block_ = InBlock{};
  }

  bool can_send() const override { return send_queue_.available(send_id_); }

  bool send(Message m, ConnectionError& error) override {
    if (!can_send()) { error = ConnectionError::send_queue_full; return false; }
    if (static_cast<int>(m.data.size()) > config_.max_message_size) { error = ConnectionError::message_too_large; return false; }
    m.id = send_id_;
    SendEntry* e = send_queue_.insert(send_id_);
    e->block = static_cast<int>(m.data.size()) > config_.block_threshold;
    e->msg = std::move(m);
    e->last_sent = -1e9;
    ++send_id_;
    return true;
  }

  std::optional<Message> receive() override {
    Message* m = receive_queue_.find(receive_id_);
    if (!m) return std::nullopt;
    Message out = std::move(*m);
    receive_queue_.remove(receive_id_);
    ++receive_id_;
    return out;
  }

  int pending() const override { return static_cast<u16>(send_id_ - oldest_unacked_); }

  bool packet_data(u16 sequence, f64 time, f32 rtt_ms, int budget_bits, ChannelEntry& entry) override {
    const f64 resend = max_of(static_cast<f64>(config_.resend_time), rtt_ms * 1.25 / 1000.0);
    entry.channel = index_;
    if (oldest_unacked_ == send_id_) return false;
    SendEntry* oldest = send_queue_.find(oldest_unacked_);
    if (oldest && oldest->block) {
      // One block at a time, oldest first; a few fragments per packet.
      if (!out_block_.active || out_block_.id != oldest->msg.id) {
        const int size = static_cast<int>(oldest->msg.data.size());
        out_block_ = OutBlock{};
        out_block_.active = true;
        out_block_.id = oldest->msg.id;
        out_block_.count = (size + config_.block_fragment_size - 1) / config_.block_fragment_size;
        out_block_.acked.assign(static_cast<std::size_t>(out_block_.count), false);
        out_block_.last_sent.assign(static_cast<std::size_t>(out_block_.count), -1e9);
      }
      entry.block = true;
      std::vector<u16> frags;
      for (int f = 0; f < out_block_.count && static_cast<int>(entry.fragments.size()) < kMaxFragmentsPerPacket; ++f) {
        if (out_block_.acked[static_cast<std::size_t>(f)] || out_block_.last_sent[static_cast<std::size_t>(f)] + resend > time) continue;
        WireFragment w;
        w.id = oldest->msg.id;
        w.type = oldest->msg.type;
        w.count = static_cast<u16>(out_block_.count);
        w.fragment = static_cast<u16>(f);
        const std::size_t from = static_cast<std::size_t>(f) * static_cast<std::size_t>(config_.block_fragment_size);
        const std::size_t to = std::min(oldest->msg.data.size(), from + static_cast<std::size_t>(config_.block_fragment_size));
        w.data.assign(oldest->msg.data.begin() + static_cast<std::ptrdiff_t>(from), oldest->msg.data.begin() + static_cast<std::ptrdiff_t>(to));
        entry.fragments.push_back(std::move(w));
        if (entry_bits(entry, conn_) > budget_bits) { entry.fragments.pop_back(); break; }
        frags.push_back(static_cast<u16>(f));
      }
      if (entry.fragments.empty()) return false;
      SentPacket* sp = sent_packets_.insert(sequence);
      sp->block = true;
      sp->block_id = out_block_.id;
      sp->ids = std::move(frags);
      for (u16 f : sp->ids) out_block_.last_sent[f] = time;
      return true;
    }
    entry.block = false;
    std::vector<u16> ids;
    const int window = send_queue_.size();
    for (int i = 0; i < window && static_cast<int>(entry.messages.size()) < config_.max_messages_per_packet; ++i) {
      const u16 id = static_cast<u16>(oldest_unacked_ + i);
      if (id == send_id_) break;
      SendEntry* e = send_queue_.find(id);
      if (!e) continue;
      if (e->block) break;  // blocks go alone, when they are the oldest
      if (e->last_sent + resend > time) continue;
      WireMessage w;
      w.id = id;
      w.type = e->msg.type;
      w.data = e->msg.data;
      entry.messages.push_back(std::move(w));
      if (entry_bits(entry, conn_) > budget_bits) { entry.messages.pop_back(); break; }
      ids.push_back(id);
    }
    if (entry.messages.empty()) return false;
    SentPacket* sp = sent_packets_.insert(sequence);
    sp->block = false;
    sp->ids = ids;
    for (u16 id : ids) send_queue_.find(id)->last_sent = time;
    return true;
  }

  bool process(const ChannelEntry& entry, ConnectionError& error) override {
    const u16 window = static_cast<u16>(receive_queue_.size());
    if (entry.block) {
      for (const WireFragment& f : entry.fragments) {
        if (sequence_less(f.id, receive_id_) || receive_queue_.exists(f.id)) continue;  // already have it
        if (sequence_greater(f.id, static_cast<u16>(receive_id_ + window - 1))) { error = ConnectionError::desync; return false; }
        if (!in_block_.active || in_block_.id != f.id) {
          in_block_ = InBlock{};
          in_block_.active = true;
          in_block_.id = f.id;
          in_block_.type = f.type;
          in_block_.count = f.count;
          in_block_.got.assign(f.count, false);
          in_block_.data.assign(static_cast<std::size_t>(f.count) * static_cast<std::size_t>(config_.block_fragment_size), 0);
        }
        if (f.count != in_block_.count || f.type != in_block_.type) return false;
        if (f.fragment < f.count - 1 && static_cast<int>(f.data.size()) != config_.block_fragment_size) return false;
        if (in_block_.got[f.fragment]) continue;
        in_block_.got[f.fragment] = true;
        ++in_block_.received;
        std::copy(f.data.begin(), f.data.end(), in_block_.data.begin() + static_cast<std::ptrdiff_t>(f.fragment) * config_.block_fragment_size);
        if (f.fragment == f.count - 1) in_block_.size = static_cast<int>((f.count - 1) * config_.block_fragment_size + f.data.size());
        if (in_block_.received == in_block_.count) {
          if (in_block_.size > config_.max_message_size) { error = ConnectionError::message_too_large; return false; }
          Message* m = receive_queue_.insert(f.id);
          if (!m) { error = ConnectionError::desync; return false; }
          m->id = f.id;
          m->type = in_block_.type;
          in_block_.data.resize(static_cast<std::size_t>(in_block_.size));
          m->data = std::move(in_block_.data);
          in_block_ = InBlock{};
        }
      }
      return true;
    }
    for (const WireMessage& w : entry.messages) {
      if (sequence_less(w.id, receive_id_) || receive_queue_.exists(w.id)) continue;  // duplicate
      if (sequence_greater(w.id, static_cast<u16>(receive_id_ + window - 1))) { error = ConnectionError::desync; return false; }
      Message* m = receive_queue_.insert(w.id);
      if (!m) { error = ConnectionError::desync; return false; }
      m->id = w.id;
      m->type = w.type;
      m->data = w.data;
    }
    return true;
  }

  void ack(u16 sequence) override {
    SentPacket* sp = sent_packets_.find(sequence);
    if (!sp || sp->acked) return;
    sp->acked = true;
    if (sp->block) {
      if (out_block_.active && out_block_.id == sp->block_id) {
        for (u16 f : sp->ids) {
          if (out_block_.acked[f]) continue;
          out_block_.acked[f] = true;
          ++out_block_.num_acked;
        }
        if (out_block_.num_acked == out_block_.count) {
          send_queue_.remove(out_block_.id);
          out_block_ = OutBlock{};
        }
      }
    } else {
      for (u16 id : sp->ids) send_queue_.remove(id);
    }
    while (oldest_unacked_ != send_id_ && !send_queue_.exists(oldest_unacked_)) ++oldest_unacked_;
  }

 private:
  struct SendEntry {
    Message msg;
    f64 last_sent = -1e9;
    bool block = false;
  };
  struct SentPacket {
    std::vector<u16> ids;  // message ids, or fragment indices of block_id
    bool acked = false, block = false;
    u16 block_id = 0;
  };
  struct OutBlock {
    bool active = false;
    u16 id = 0;
    int count = 0, num_acked = 0;
    std::vector<bool> acked;
    std::vector<f64> last_sent;
  };
  struct InBlock {
    bool active = false;
    u16 id = 0, type = 0, count = 0;
    int received = 0, size = 0;
    std::vector<bool> got;
    std::vector<u8> data;
  };
  SequenceBuffer<SendEntry> send_queue_;
  SequenceBuffer<SentPacket> sent_packets_;
  SequenceBuffer<Message> receive_queue_;
  u16 send_id_ = 0, oldest_unacked_ = 0, receive_id_ = 0;
  OutBlock out_block_;
  InBlock in_block_;
};

class UnreliableChannel final : public Channel {
 public:
  using Channel::Channel;
  void reset() override { send_.clear(); receive_.clear(); }
  bool can_send() const override { return static_cast<int>(send_.size()) < config_.send_queue_size; }
  bool send(Message m, ConnectionError& error) override {
    if (static_cast<int>(m.data.size()) > config_.max_message_size) { error = ConnectionError::message_too_large; return false; }
    if (!can_send()) return false;  // dropped: unreliable messages are not worth an error
    send_.push_back(std::move(m));
    return true;
  }
  std::optional<Message> receive() override {
    if (receive_.empty()) return std::nullopt;
    Message m = std::move(receive_.front());
    receive_.pop_front();
    return m;
  }
  int pending() const override { return static_cast<int>(send_.size()); }
  bool packet_data(u16, f64, f32, int budget_bits, ChannelEntry& entry) override {
    entry.channel = index_;
    entry.block = false;
    while (!send_.empty() && static_cast<int>(entry.messages.size()) < config_.max_messages_per_packet) {
      WireMessage w;
      w.type = send_.front().type;
      w.data = send_.front().data;
      entry.messages.push_back(std::move(w));
      if (entry_bits(entry, conn_) > budget_bits) {
        entry.messages.pop_back();
        // A message bigger than a whole packet would never go out: drop it.
        if (entry.messages.empty()) send_.pop_front();
        break;
      }
      send_.pop_front();
    }
    return !entry.messages.empty();
  }
  bool process(const ChannelEntry& entry, ConnectionError&) override {
    for (const WireMessage& w : entry.messages) {
      if (static_cast<int>(receive_.size()) >= config_.receive_queue_size) break;
      Message m;
      m.type = w.type;
      m.data = w.data;
      receive_.push_back(std::move(m));
    }
    return true;
  }
  void ack(u16) override {}

 private:
  std::deque<Message> send_, receive_;
};

}  // namespace

// ---------------------------------------------------------------------------
// Connection

Connection::Connection(const ConnectionConfig& config, f64 time)
    : config_(config), endpoint_(config.endpoint, &Connection::on_transmit, &Connection::on_process, this, time), time_(time) {
  if (config_.channels.empty()) config_.channels.push_back(ChannelConfig{});
  for (std::size_t i = 0; i < config_.channels.size(); ++i) {
    const ChannelConfig& c = config_.channels[i];
    if (c.type == ChannelType::reliable_ordered) channels_.push_back(std::make_unique<ReliableChannel>(c, config_, static_cast<int>(i)));
    else channels_.push_back(std::make_unique<UnreliableChannel>(c, config_, static_cast<int>(i)));
  }
  packet_.resize(static_cast<std::size_t>((config_.max_packet_size + 3) / 4 * 4));
}

Connection::~Connection() = default;

void Connection::reset() {
  endpoint_.reset();
  for (auto& c : channels_) c->reset();
  error_ = ConnectionError::none;
}

bool Connection::can_send(int channel) const {
  return channel >= 0 && channel < channel_count() && channels_[static_cast<std::size_t>(channel)]->can_send();
}

bool Connection::send(int channel, u16 type, std::vector<u8> data) {
  if (channel < 0 || channel >= channel_count() || type >= config_.max_message_types) return false;
  Message m;
  m.type = type;
  m.data = std::move(data);
  ConnectionError e = ConnectionError::none;
  const bool ok = channels_[static_cast<std::size_t>(channel)]->send(std::move(m), e);
  if (e != ConnectionError::none && error_ == ConnectionError::none) error_ = e;
  return ok;
}

std::optional<Message> Connection::receive(int channel) {
  if (channel < 0 || channel >= channel_count()) return std::nullopt;
  return channels_[static_cast<std::size_t>(channel)]->receive();
}

int Connection::pending(int channel) const {
  return channel >= 0 && channel < channel_count() ? channels_[static_cast<std::size_t>(channel)]->pending() : 0;
}

void Connection::send_packet() {
  const u16 sequence = endpoint_.next_sequence();
  std::vector<ChannelEntry> entries;
  // Header: entry count (bits for 0..channels).
  const int count_bits = bits_required(0, static_cast<u32>(channels_.size()));
  int used = count_bits + 32;  // + flush slack
  const int total = config_.max_packet_size * 8;
  for (auto& ch : channels_) {
    const int idx = static_cast<int>(&ch - channels_.data());
    const int budget_cfg = config_.channels[static_cast<std::size_t>(idx)].packet_budget;
    int budget = total - used;
    if (budget_cfg > 0) budget = min_of(budget, budget_cfg * 8);
    if (budget <= 64) break;
    ChannelEntry e;
    if (!ch->packet_data(sequence, time_, endpoint_.stats().rtt_ms, budget, e)) continue;
    used += entry_bits(e, config_);
    entries.push_back(std::move(e));
  }
  WriteStream w(packet_.data(), static_cast<int>(packet_.size()));
  i32 n = static_cast<i32>(entries.size());
  bool ok = serialize_int(w, n, 0, static_cast<i64>(channels_.size()));
  for (ChannelEntry& e : entries) ok = ok && serialize_entry(w, e, config_);
  w.flush();
  if (!ok || w.overflow()) {
    // Should not happen (budgets are measured); send an empty packet so acks still flow.
    WriteStream e(packet_.data(), static_cast<int>(packet_.size()));
    i32 zero = 0;
    serialize_int(e, zero, 0, static_cast<i64>(channels_.size()));
    e.flush();
    endpoint_.send_packet(packet_.data(), e.bytes_processed());
    return;
  }
  endpoint_.send_packet(packet_.data(), w.bytes_processed());
}

void Connection::receive_packet(const u8* data, int bytes) {
  endpoint_.receive_packet(data, bytes);
  for (u16 seq : endpoint_.acks())
    for (auto& c : channels_) c->ack(seq);
  endpoint_.clear_acks();
}

void Connection::advance_time(f64 time) {
  time_ = time;
  endpoint_.update(time);
}

void Connection::on_transmit(void* ctx, u16, const u8* packet, int bytes) {
  Connection* c = static_cast<Connection*>(ctx);
  if (c->transmit) c->transmit(c->transmit_ctx, packet, bytes);
}

bool Connection::on_process(void* ctx, u16 sequence, const u8* payload, int bytes) {
  return static_cast<Connection*>(ctx)->process_payload(sequence, payload, bytes);
}

bool Connection::process_payload(u16, const u8* payload, int bytes) {
  ReadStream r(payload, bytes);
  i32 n = 0;
  if (!serialize_int(r, n, 0, static_cast<i64>(channels_.size()))) return false;
  std::vector<ChannelEntry> entries(static_cast<std::size_t>(n));
  for (ChannelEntry& e : entries)
    if (!serialize_entry(r, e, config_)) return false;
  for (const ChannelEntry& e : entries) {
    ConnectionError err = ConnectionError::none;
    if (!channels_[static_cast<std::size_t>(e.channel)]->process(e, err)) {
      if (err != ConnectionError::none && error_ == ConnectionError::none) error_ = err;
      return false;
    }
  }
  return true;
}

}  // namespace kern::net
