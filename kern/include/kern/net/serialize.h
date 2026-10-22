// Bit packing and unified serialization (Gaffer On Games, "Reading and
// Writing Packets" / "Serialization Strategies").
//
// One serialize function per type describes the wire format for reading,
// writing and measuring:
//
//   template <class Stream> bool serialize(Stream& s) {
//     serialize_int(s, health, 0, 100);       // 7 bits
//     serialize_bool(s, alive);
//     serialize_float(s, speed);              // 32 bits
//     serialize_compressed_float(s, x, -512, 512, 0.01f);
//     return true;
//   }
//
// Readers validate everything they read: out-of-range values, overflowing
// buffers and bad lengths make serialize return false, so a malicious or
// corrupt packet is dropped instead of crashing. Words are little-endian on
// the wire on every platform.
#pragma once

#include <cmath>
#include <string>
#include <vector>

#include "kern/core/base.h"
#include "kern/math/math.h"

namespace kern::net {

/// Bits needed to store values 0..n (n >= 0).
constexpr int bits_required(u32 min, u32 max) {
  u32 range = max - min;
  int bits = 0;
  while (range) { ++bits; range >>= 1; }
  return bits;
}

u32 crc32(const void* data, std::size_t bytes, u32 crc = 0);

// ---------------------------------------------------------------------------
// bit writer / reader (32-bit words, scratch of 64 bits)

class BitWriter {
 public:
  BitWriter(void* data, int bytes);  // bytes must be a multiple of 4
  void write_bits(u32 value, int bits);
  void write_align();
  void write_bytes(const u8* data, int bytes);
  void flush();
  int align_bits() const { return (8 - bits_written_ % 8) % 8; }
  int bits_written() const { return bits_written_; }
  int bits_available() const { return num_bits_ - bits_written_; }
  int bytes_written() const { return (bits_written_ + 7) / 8; }
  bool overflow() const { return overflow_; }

 private:
  u32* data_;
  u64 scratch_ = 0;
  int num_bits_, num_words_;
  int bits_written_ = 0, word_index_ = 0, scratch_bits_ = 0;
  bool overflow_ = false;
};

class BitReader {
 public:
  BitReader(const void* data, int bytes);  // any size; reads past the end fail
  bool would_overflow(int bits) const { return bits_read_ + bits > num_bits_; }
  u32 read_bits(int bits);
  bool read_align();  // false when the padding bits are not zero
  void read_bytes(u8* data, int bytes);
  int align_bits() const { return (8 - bits_read_ % 8) % 8; }
  int bits_read() const { return bits_read_; }
  int bits_remaining() const { return num_bits_ - bits_read_; }
  int bytes_read() const { return (bits_read_ + 7) / 8; }

 private:
  u32 read_word(int index) const;
  const u8* data_;
  int num_bytes_, num_bits_, num_words_;
  u64 scratch_ = 0;
  int scratch_bits_ = 0, word_index_ = 0, bits_read_ = 0;
};

// ---------------------------------------------------------------------------
// streams

class WriteStream {
 public:
  static constexpr bool kWriting = true, kReading = false, kMeasuring = false;
  WriteStream(void* buffer, int bytes) : w_(buffer, bytes) {}
  bool serialize_integer(i32 value, i32 min, i32 max);
  bool serialize_bits(u32 value, int bits);
  bool serialize_bytes(const u8* data, int bytes);
  bool serialize_align();
  bool serialize_check(u32 magic);  // debug marker, costs 32 bits
  void flush() { w_.flush(); }
  int bits_processed() const { return w_.bits_written(); }
  int bytes_processed() const { return w_.bytes_written(); }
  int align_bits() const { return w_.align_bits(); }
  bool overflow() const { return w_.overflow(); }

 private:
  BitWriter w_;
};

class ReadStream {
 public:
  static constexpr bool kWriting = false, kReading = true, kMeasuring = false;
  ReadStream(const void* buffer, int bytes) : r_(buffer, bytes) {}
  bool serialize_integer(i32& value, i32 min, i32 max);
  bool serialize_bits(u32& value, int bits);
  bool serialize_bytes(u8* data, int bytes);
  bool serialize_align();
  bool serialize_check(u32 magic);
  int bits_processed() const { return r_.bits_read(); }
  int bytes_processed() const { return r_.bytes_read(); }
  int bits_remaining() const { return r_.bits_remaining(); }
  int align_bits() const { return r_.align_bits(); }

 private:
  BitReader r_;
};

/// Counts bits without writing (worst case for aligns).
class MeasureStream {
 public:
  static constexpr bool kWriting = false, kReading = false, kMeasuring = true;
  bool serialize_integer(i32, i32 min, i32 max) { bits_ += bits_required(0, static_cast<u32>(max - min)); return true; }
  bool serialize_bits(u32, int bits) { bits_ += bits; return true; }
  bool serialize_bytes(const u8*, int bytes) { serialize_align(); bits_ += bytes * 8; return true; }
  bool serialize_align() { bits_ += 7; return true; }
  bool serialize_check(u32) { serialize_align(); bits_ += 32; return true; }
  int bits_processed() const { return bits_; }
  int bytes_processed() const { return (bits_ + 7) / 8; }
  int align_bits() const { return 7; }

 private:
  int bits_ = 0;
};

// ---------------------------------------------------------------------------
// serialize helpers (template over the stream)

template <class S, class T>
bool serialize_int(S& s, T& value, i64 min, i64 max) {
  static_assert(std::is_integral_v<T> || std::is_enum_v<T>);
  if (min >= max) return false;
  if constexpr (S::kReading) {
    i32 v = 0;
    if (!s.serialize_integer(v, static_cast<i32>(min), static_cast<i32>(max))) return false;
    value = static_cast<T>(v);
    return true;
  } else {
    const i64 v = static_cast<i64>(value);
    if (v < min || v > max) return false;
    return s.serialize_integer(static_cast<i32>(v), static_cast<i32>(min), static_cast<i32>(max));
  }
}

template <class S, class T>
bool serialize_bits(S& s, T& value, int bits) {
  if (bits <= 0 || bits > 32) return false;
  if constexpr (S::kReading) {
    u32 v = 0;
    if (!s.serialize_bits(v, bits)) return false;
    value = static_cast<T>(v);
    return true;
  } else {
    return s.serialize_bits(static_cast<u32>(value), bits);
  }
}

template <class S>
bool serialize_bool(S& s, bool& value) {
  u32 v = value ? 1 : 0;
  if (!serialize_bits(s, v, 1)) return false;
  if constexpr (S::kReading) value = v != 0;
  return true;
}

template <class S>
bool serialize_u16(S& s, u16& value) { return serialize_bits(s, value, 16); }

template <class S>
bool serialize_u32(S& s, u32& value) { return serialize_bits(s, value, 32); }

template <class S>
bool serialize_u64(S& s, u64& value) {
  u32 lo = static_cast<u32>(value), hi = static_cast<u32>(value >> 32);
  if (!serialize_bits(s, lo, 32) || !serialize_bits(s, hi, 32)) return false;
  if constexpr (S::kReading) value = (static_cast<u64>(hi) << 32) | lo;
  return true;
}

template <class S>
bool serialize_float(S& s, f32& value) {
  u32 bits = 0;
  if constexpr (!S::kReading) std::memcpy(&bits, &value, 4);
  if (!serialize_bits(s, bits, 32)) return false;
  if constexpr (S::kReading) {
    std::memcpy(&value, &bits, 4);
    if (!std::isfinite(value)) return false;
  }
  return true;
}

/// Quantised float in [min, max] with resolution `res` (values outside are clamped when writing).
template <class S>
bool serialize_compressed_float(S& s, f32& value, f32 min, f32 max, f32 res) {
  const f32 delta = max - min;
  const f32 values = delta / res;
  const u32 max_int = static_cast<u32>(std::ceil(values));
  const int bits = bits_required(0, max_int);
  u32 q = 0;
  if constexpr (!S::kReading) {
    const f32 n = (clamp(std::isfinite(value) ? value : min, min, max) - min) / delta;
    q = static_cast<u32>(std::floor(n * static_cast<f32>(max_int) + 0.5f));
  }
  if (!serialize_bits(s, q, bits)) return false;
  if constexpr (S::kReading) {
    if (q > max_int) return false;
    value = min + static_cast<f32>(q) / static_cast<f32>(max_int) * delta;
  }
  return true;
}

template <class S>
bool serialize_vec3(S& s, Vec3& v) { return serialize_float(s, v.x) && serialize_float(s, v.y) && serialize_float(s, v.z); }

template <class S>
bool serialize_compressed_vec3(S& s, Vec3& v, f32 bound, f32 res) {
  return serialize_compressed_float(s, v.x, -bound, bound, res) && serialize_compressed_float(s, v.y, -bound, bound, res) &&
         serialize_compressed_float(s, v.z, -bound, bound, res);
}

/// Unit quaternion, "smallest three" (Gaffer, "Snapshot Compression"): the largest
/// component is dropped (its sign normalised), the other three in [-1/sqrt2, 1/sqrt2]
/// with `bits` each. 2 + 3 * bits bits.
template <class S>
bool serialize_quat(S& s, Quat& q, int bits = 10) {
  constexpr f32 kMin = -0.70710678f, kMax = 0.70710678f;
  const u32 scale = (1u << bits) - 1;
  u32 largest = 0, a = 0, b = 0, c = 0;
  if constexpr (!S::kReading) {
    f32 v[4] = {q.x, q.y, q.z, q.w};
    f32 len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2] + v[3] * v[3]);
    if (!(len > 1e-6f) || !std::isfinite(len)) { v[0] = v[1] = v[2] = 0; v[3] = len = 1; }
    for (f32& x : v) x /= len;
    for (u32 i = 1; i < 4; ++i)
      if (std::fabs(v[i]) > std::fabs(v[largest])) largest = i;
    const f32 sign = v[largest] < 0 ? -1.0f : 1.0f;
    u32 out[3];
    for (u32 i = 0, k = 0; i < 4; ++i) {
      if (i == largest) continue;
      const f32 n = (clamp(v[i] * sign, kMin, kMax) - kMin) / (kMax - kMin);
      out[k++] = static_cast<u32>(std::floor(n * static_cast<f32>(scale) + 0.5f));
    }
    a = out[0];
    b = out[1];
    c = out[2];
  }
  if (!serialize_bits(s, largest, 2) || !serialize_bits(s, a, bits) || !serialize_bits(s, b, bits) || !serialize_bits(s, c, bits)) return false;
  if constexpr (S::kReading) {
    const u32 in[3] = {a, b, c};
    f32 v[4];
    f32 sum = 0;
    for (u32 i = 0, k = 0; i < 4; ++i) {
      if (i == largest) continue;
      v[i] = kMin + static_cast<f32>(in[k++]) / static_cast<f32>(scale) * (kMax - kMin);
      sum += v[i] * v[i];
    }
    v[largest] = std::sqrt(max_of(0.0f, 1.0f - sum));
    q = Quat{v[0], v[1], v[2], v[3]};
  }
  return true;
}

template <class S>
bool serialize_bytes(S& s, u8* data, int bytes) {
  if constexpr (S::kReading) return s.serialize_bytes(data, bytes);
  else return s.serialize_bytes(data, bytes);
}

/// Length-prefixed string of at most `max` bytes.
template <class S>
bool serialize_string(S& s, std::string& str, int max = 255) {
  i32 length = static_cast<i32>(str.size());
  if constexpr (!S::kReading) if (length > max) return false;
  if (!serialize_int(s, length, 0, max)) return false;
  if constexpr (S::kReading) str.assign(static_cast<std::size_t>(length), '\0');
  if (!length) return true;
  return s.serialize_bytes(reinterpret_cast<u8*>(str.data()), length);
}

/// Sequence number relative to a base (small deltas cost few bits).
template <class S>
bool serialize_sequence_relative(S& s, u16 base, u16& value) {
  u32 delta = 0;
  if constexpr (!S::kReading) delta = static_cast<u16>(value - base);
  bool one = delta == 1;
  if (!serialize_bool(s, one)) return false;
  if (one) {
    if constexpr (S::kReading) value = static_cast<u16>(base + 1);
    return true;
  }
  bool small = delta <= 32;
  if (!serialize_bool(s, small)) return false;
  if (small) {
    u32 d = delta - 2;
    if constexpr (S::kReading) d = 0;
    if (!serialize_bits(s, d, 5)) return false;
    if constexpr (S::kReading) value = static_cast<u16>(base + d + 2);
    return true;
  }
  if (!serialize_bits(s, delta, 16)) return false;
  if constexpr (S::kReading) value = static_cast<u16>(base + delta);
  return true;
}

// Sequence numbers wrap: a is newer than b when it is ahead by less than half the range.
constexpr bool sequence_greater(u16 a, u16 b) { return ((a > b) && (a - b <= 32768)) || ((a < b) && (b - a > 32768)); }
constexpr bool sequence_less(u16 a, u16 b) { return sequence_greater(b, a); }

}  // namespace kern::net
