#include "kern/net/serialize.h"

namespace kern::net {

namespace {

u32 crc_table_entry(u32 c) {
  for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
  return c;
}

struct CrcTable {
  u32 t[256];
  CrcTable() { for (u32 i = 0; i < 256; ++i) t[i] = crc_table_entry(i); }
};

}  // namespace

u32 crc32(const void* data, std::size_t bytes, u32 crc) {
  static const CrcTable table;
  const u8* p = static_cast<const u8*>(data);
  crc = ~crc;
  for (std::size_t i = 0; i < bytes; ++i) crc = table.t[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
  return ~crc;
}

// ---------------------------------------------------------------------------
// BitWriter

BitWriter::BitWriter(void* data, int bytes) : data_(static_cast<u32*>(data)), num_bits_(bytes / 4 * 32), num_words_(bytes / 4) {}

void BitWriter::write_bits(u32 value, int bits) {
  if (bits <= 0 || bits > 32) { overflow_ = true; return; }
  if (bits_written_ + bits > num_bits_) { overflow_ = true; return; }
  if (bits < 32) value &= (1u << bits) - 1;
  scratch_ |= static_cast<u64>(value) << scratch_bits_;
  scratch_bits_ += bits;
  if (scratch_bits_ >= 32) {
    const u32 w = static_cast<u32>(scratch_);
    u8* out = reinterpret_cast<u8*>(data_ + word_index_);
    out[0] = static_cast<u8>(w);
    out[1] = static_cast<u8>(w >> 8);
    out[2] = static_cast<u8>(w >> 16);
    out[3] = static_cast<u8>(w >> 24);
    scratch_ >>= 32;
    scratch_bits_ -= 32;
    ++word_index_;
  }
  bits_written_ += bits;
}

void BitWriter::write_align() {
  const int r = align_bits();
  if (r) write_bits(0, r);
}

void BitWriter::write_bytes(const u8* data, int bytes) {
  write_align();
  for (int i = 0; i < bytes; ++i) write_bits(data[i], 8);
}

void BitWriter::flush() {
  if (scratch_bits_ == 0 || word_index_ >= num_words_) return;
  const u32 w = static_cast<u32>(scratch_);
  u8* out = reinterpret_cast<u8*>(data_ + word_index_);
  out[0] = static_cast<u8>(w);
  out[1] = static_cast<u8>(w >> 8);
  out[2] = static_cast<u8>(w >> 16);
  out[3] = static_cast<u8>(w >> 24);
}

// ---------------------------------------------------------------------------
// BitReader

BitReader::BitReader(const void* data, int bytes)
    : data_(static_cast<const u8*>(data)), num_bytes_(bytes), num_bits_(bytes * 8), num_words_((bytes + 3) / 4) {}

u32 BitReader::read_word(int index) const {
  u32 w = 0;
  for (int b = 0; b < 4; ++b) {
    const int at = index * 4 + b;
    if (at < num_bytes_) w |= static_cast<u32>(data_[at]) << (8 * b);
  }
  return w;
}

u32 BitReader::read_bits(int bits) {
  if (bits <= 0 || bits > 32 || would_overflow(bits)) return 0;
  bits_read_ += bits;
  if (scratch_bits_ < bits) {
    scratch_ |= static_cast<u64>(read_word(word_index_)) << scratch_bits_;
    scratch_bits_ += 32;
    ++word_index_;
  }
  const u32 out = static_cast<u32>(scratch_ & ((bits == 32) ? 0xFFFFFFFFull : ((1ull << bits) - 1)));
  scratch_ >>= bits;
  scratch_bits_ -= bits;
  return out;
}

bool BitReader::read_align() {
  const int r = align_bits();
  if (!r) return true;
  if (would_overflow(r)) return false;
  return read_bits(r) == 0;
}

void BitReader::read_bytes(u8* data, int bytes) {
  for (int i = 0; i < bytes; ++i) data[i] = static_cast<u8>(read_bits(8));
}

// ---------------------------------------------------------------------------
// streams

bool WriteStream::serialize_integer(i32 value, i32 min, i32 max) {
  if (min >= max || value < min || value > max) return false;
  w_.write_bits(static_cast<u32>(value - min), bits_required(0, static_cast<u32>(max - min)));
  return !w_.overflow();
}

bool WriteStream::serialize_bits(u32 value, int bits) {
  if (bits <= 0 || bits > 32) return false;
  w_.write_bits(value, bits);
  return !w_.overflow();
}

bool WriteStream::serialize_bytes(const u8* data, int bytes) {
  if (bytes < 0) return false;
  w_.write_bytes(data, bytes);
  return !w_.overflow();
}

bool WriteStream::serialize_align() {
  w_.write_align();
  return !w_.overflow();
}

bool WriteStream::serialize_check(u32 magic) {
  w_.write_align();
  w_.write_bits(magic, 32);
  return !w_.overflow();
}

bool ReadStream::serialize_integer(i32& value, i32 min, i32 max) {
  if (min >= max) return false;
  const int bits = bits_required(0, static_cast<u32>(max - min));
  if (r_.would_overflow(bits)) return false;
  const u32 raw = r_.read_bits(bits);
  const i64 v = static_cast<i64>(raw) + min;
  if (v > max) return false;
  value = static_cast<i32>(v);
  return true;
}

bool ReadStream::serialize_bits(u32& value, int bits) {
  if (bits <= 0 || bits > 32 || r_.would_overflow(bits)) return false;
  value = r_.read_bits(bits);
  return true;
}

bool ReadStream::serialize_bytes(u8* data, int bytes) {
  if (bytes < 0 || !r_.read_align() || r_.would_overflow(bytes * 8)) return false;
  r_.read_bytes(data, bytes);
  return true;
}

bool ReadStream::serialize_align() { return r_.read_align(); }

bool ReadStream::serialize_check(u32 magic) {
  if (!r_.read_align() || r_.would_overflow(32)) return false;
  return r_.read_bits(32) == magic;
}

}  // namespace kern::net
