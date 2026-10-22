#include "kern/net/reliable.h"

namespace kern::net {

namespace {

constexpr u8 kFragmentFlag = 1;
constexpr int kMaxHeader = 9;
constexpr int kFragmentHeader = 5;  // prefix, sequence, fragment id, count - 1

void put16(u8* p, u16 v) { p[0] = static_cast<u8>(v); p[1] = static_cast<u8>(v >> 8); }
u16 get16(const u8* p) { return static_cast<u16>(p[0] | (p[1] << 8)); }

}  // namespace

Endpoint::Endpoint(const EndpointConfig& config, Transmit transmit, Process process, void* ctx, f64 time)
    : config_(config),
      transmit_(transmit),
      process_(process),
      ctx_(ctx),
      time_(time),
      sent_(config.sent_packets_buffer_size),
      received_(config.received_packets_buffer_size),
      reassembly_(config.fragment_reassembly_buffer_size) {
  config_.max_fragments = clamp(config_.max_fragments, 1, 256);
  config_.fragment_size = clamp(config_.fragment_size, 64, 16 * 1024);
  acks_.reserve(static_cast<std::size_t>(config.ack_buffer_size));
}

void Endpoint::reset() {
  sequence_ = 0;
  acks_.clear();
  sent_.reset();
  received_.reset();
  reassembly_.reset();
  stats_ = EndpointStats{};
}

int Endpoint::write_header(u8* out, u16 sequence, u16 ack, u32 ack_bits) const {
  u8 prefix = 0;
  for (int i = 0; i < 4; ++i)
    if (((ack_bits >> (8 * i)) & 0xFF) != 0xFF) prefix |= static_cast<u8>(1 << (1 + i));
  const u16 diff = static_cast<u16>(sequence - ack);
  if (diff <= 255) prefix |= 1 << 5;
  int n = 0;
  out[n++] = prefix;
  put16(out + n, sequence);
  n += 2;
  if (diff <= 255) out[n++] = static_cast<u8>(diff);
  else { put16(out + n, ack); n += 2; }
  for (int i = 0; i < 4; ++i)
    if (prefix & (1 << (1 + i))) out[n++] = static_cast<u8>(ack_bits >> (8 * i));
  return n;
}

int Endpoint::read_header(const u8* in, int bytes, u16& sequence, u16& ack, u32& ack_bits) const {
  if (bytes < 3) return -1;
  const u8 prefix = in[0];
  if (prefix & kFragmentFlag) return -1;
  if (prefix & 0xC0) return -1;  // unused bits must be zero
  int n = 1;
  sequence = get16(in + n);
  n += 2;
  if (prefix & (1 << 5)) {
    if (bytes < n + 1) return -1;
    ack = static_cast<u16>(sequence - in[n++]);
  } else {
    if (bytes < n + 2) return -1;
    ack = get16(in + n);
    n += 2;
  }
  ack_bits = 0xFFFFFFFFu;
  for (int i = 0; i < 4; ++i) {
    if (!(prefix & (1 << (1 + i)))) continue;
    if (bytes < n + 1) return -1;
    ack_bits &= ~(0xFFu << (8 * i));
    ack_bits |= static_cast<u32>(in[n++]) << (8 * i);
  }
  return n;
}

u16 Endpoint::send_packet(const u8* payload, int bytes) {
  const u16 sequence = sequence_;
  if (bytes < 0 || bytes > config_.max_packet_size) {
    ++stats_.packets_too_large_to_send;
    return sequence;
  }
  u16 ack;
  u32 ack_bits;
  received_.generate_ack_bits(ack, ack_bits);
  if (Sent* s = sent_.insert(sequence)) {
    s->time = time_;
    s->acked = false;
    s->bytes = static_cast<u32>(config_.packet_header_size + bytes);
  }
  ++sequence_;
  ++stats_.packets_sent;
  scratch_.resize(static_cast<std::size_t>(kMaxHeader + bytes));
  const int header = write_header(scratch_.data(), sequence, ack, ack_bits);
  if (bytes) std::memcpy(scratch_.data() + header, payload, static_cast<std::size_t>(bytes));
  const int total = header + bytes;
  if (total <= config_.fragment_above) {
    transmit_(ctx_, sequence, scratch_.data(), total);
    return sequence;
  }
  const int count = (total + config_.fragment_size - 1) / config_.fragment_size;
  if (count > config_.max_fragments) {
    ++stats_.packets_too_large_to_send;
    return sequence;
  }
  std::vector<u8> frag(static_cast<std::size_t>(kFragmentHeader + config_.fragment_size));
  for (int f = 0; f < count; ++f) {
    const int offset = f * config_.fragment_size;
    const int n = min_of(config_.fragment_size, total - offset);
    frag[0] = kFragmentFlag;
    put16(frag.data() + 1, sequence);
    frag[3] = static_cast<u8>(f);
    frag[4] = static_cast<u8>(count - 1);
    std::memcpy(frag.data() + kFragmentHeader, scratch_.data() + offset, static_cast<std::size_t>(n));
    transmit_(ctx_, sequence, frag.data(), kFragmentHeader + n);
    ++stats_.fragments_sent;
  }
  return sequence;
}

void Endpoint::receive_packet(const u8* packet, int bytes) {
  if (!packet || bytes <= 0) { ++stats_.packets_invalid; return; }
  if (bytes > config_.max_packet_size + kMaxHeader + kFragmentHeader) { ++stats_.packets_too_large_to_receive; return; }
  if (!(packet[0] & kFragmentFlag)) { process_regular(packet, bytes); return; }
  // Fragment.
  if (bytes < kFragmentHeader + 1 || packet[0] != kFragmentFlag) { ++stats_.fragments_invalid; return; }
  const u16 sequence = get16(packet + 1);
  const int id = packet[3], count = packet[4] + 1;
  const int n = bytes - kFragmentHeader;
  if (count > config_.max_fragments || id >= count || n > config_.fragment_size || (id < count - 1 && n != config_.fragment_size)) {
    ++stats_.fragments_invalid;
    return;
  }
  Reassembly* r = reassembly_.find(sequence);
  if (!r) {
    r = reassembly_.insert(sequence);
    if (!r) { ++stats_.fragments_invalid; return; }  // too old
    r->sequence = sequence;
    r->num_fragments = count;
    r->received = 0;
    r->bytes = 0;
    r->data.assign(static_cast<std::size_t>(count * config_.fragment_size), 0);
  }
  if (r->num_fragments != count) { ++stats_.fragments_invalid; return; }
  if (r->got[id]) return;  // duplicate
  r->got[id] = true;
  ++r->received;
  ++stats_.fragments_received;
  std::memcpy(r->data.data() + id * config_.fragment_size, packet + kFragmentHeader, static_cast<std::size_t>(n));
  if (id == count - 1) r->bytes = (count - 1) * config_.fragment_size + n;
  if (r->received == count && r->bytes > 0) {
    std::vector<u8> whole(r->data.begin(), r->data.begin() + r->bytes);
    reassembly_.remove(sequence);
    process_regular(whole.data(), static_cast<int>(whole.size()));
  }
}

void Endpoint::process_regular(const u8* packet, int bytes) {
  u16 sequence, ack;
  u32 ack_bits;
  const int header = read_header(packet, bytes, sequence, ack, ack_bits);
  if (header < 0) { ++stats_.packets_invalid; return; }
  if (sequence_less(sequence, static_cast<u16>(received_.sequence() - received_.size())) || received_.exists(sequence)) {
    ++stats_.packets_stale;  // too old for the window, or a duplicate
    return;
  }
  if (!process_(ctx_, sequence, packet + header, bytes - header)) { ++stats_.packets_invalid; return; }
  if (Received* r = received_.insert(sequence)) {
    r->time = time_;
    r->bytes = static_cast<u32>(config_.packet_header_size + bytes);
  }
  ++stats_.packets_received;
  for (u32 i = 0; i < 33; ++i) {
    if (i > 0 && !(ack_bits & (1u << (i - 1)))) continue;
    const u16 s = static_cast<u16>(ack - i);
    Sent* sp = sent_.find(s);
    if (!sp || sp->acked) continue;
    sp->acked = true;
    if (acks_.size() < static_cast<std::size_t>(config_.ack_buffer_size)) acks_.push_back(s);
    ++stats_.packets_acked;
    const f32 rtt = static_cast<f32>((time_ - sp->time) * 1000.0);
    if (rtt >= 0) {
      if (stats_.rtt_ms == 0) stats_.rtt_ms = rtt;
      else stats_.rtt_ms += (rtt - stats_.rtt_ms) * clamp(config_.rtt_smoothing, 0.0f, 1.0f);
    }
  }
}

void Endpoint::update(f64 time) {
  time_ = time;
  // Packet loss over sent packets old enough to have been acked (the older half of the window).
  {
    const int half = sent_.size() / 2;
    const u16 base = static_cast<u16>(sent_.sequence() - sent_.size() + 1);
    int total = 0, dropped = 0;
    for (int i = 0; i < half; ++i) {
      const u16 s = static_cast<u16>(base + i);
      const Sent* p = sent_.find(s);
      if (!p) continue;
      ++total;
      dropped += p->acked ? 0 : 1;
    }
    if (total > 0) {
      const f32 loss = 100.0f * static_cast<f32>(dropped) / static_cast<f32>(total);
      stats_.packet_loss += (loss - stats_.packet_loss) * config_.packet_loss_smoothing;
    }
  }
  // Bandwidth over the sent / received windows.
  auto kbps = [&](auto& buffer, bool only_acked) {
    const u16 base = static_cast<u16>(buffer.sequence() - buffer.size() + 1);
    f64 start = 1e300, finish = 0;
    u64 bytes = 0;
    for (int i = 0; i < buffer.size(); ++i) {
      const auto* p = buffer.find(static_cast<u16>(base + i));
      if (!p) continue;
      if constexpr (requires { p->acked; }) if (only_acked && !p->acked) continue;
      bytes += p->bytes;
      start = std::min(start, p->time);
      finish = std::max(finish, p->time);
    }
    const f64 span = finish - start;
    return span > 0.05 ? static_cast<f32>(bytes * 8.0 / span / 1000.0) : 0.0f;
  };
  const f32 k = config_.bandwidth_smoothing;
  stats_.sent_kbps += (kbps(sent_, false) - stats_.sent_kbps) * k;
  stats_.acked_kbps += (kbps(sent_, true) - stats_.acked_kbps) * k;
  stats_.received_kbps += (kbps(received_, false) - stats_.received_kbps) * k;
}

}  // namespace kern::net
