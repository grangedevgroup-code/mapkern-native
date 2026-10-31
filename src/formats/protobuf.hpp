#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

#include "formats/inflate.hpp"

namespace mapkern::pbf {

inline constexpr int kVarint = 0;
inline constexpr int kFixed64 = 1;
inline constexpr int kBytes = 2;
inline constexpr int kFixed32 = 5;

class Reader {
 public:
  Reader(const uint8_t* data, size_t start, size_t end) : buf(data), pos(start), end(end) {}
  Reader(const uint8_t* data, size_t size) : buf(data), pos(0), end(size) {}

  const uint8_t* buf;
  size_t pos;
  size_t end;

  bool done() const { return pos >= end; }

  uint64_t varint64() {
    uint64_t value = 0;
    for (int shift = 0; shift < 70; shift += 7) {
      if (pos >= end) throw FormatError("Truncated varint");
      const uint8_t b = buf[pos++];
      value |= static_cast<uint64_t>(b & 0x7f) << shift;
      if (b < 0x80) return value;
    }
    throw FormatError("Varint is too long");
  }

  uint32_t varint() {
    if (pos < end && buf[pos] < 0x80) return buf[pos++];
    return static_cast<uint32_t>(varint64());
  }

  uint32_t tag() { return varint(); }

  int64_t svarint64() {
    const uint64_t v = varint64();
    return static_cast<int64_t>(v >> 1) ^ -static_cast<int64_t>(v & 1);
  }

  int32_t svarint() {
    const uint32_t v = varint();
    return static_cast<int32_t>(v >> 1) ^ -static_cast<int32_t>(v & 1);
  }

  int64_t int64() { return static_cast<int64_t>(varint64()); }
  bool boolean() { return varint64() != 0; }

  uint32_t fixed32() {
    if (pos + 4 > end) throw FormatError("Truncated fixed32");
    uint32_t v;
    std::memcpy(&v, buf + pos, 4);
    pos += 4;
    return v;
  }

  uint64_t fixed64() {
    if (pos + 8 > end) throw FormatError("Truncated fixed64");
    uint64_t v;
    std::memcpy(&v, buf + pos, 8);
    pos += 8;
    return v;
  }

  float float32() {
    const uint32_t bits = fixed32();
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
  }

  double float64() {
    const uint64_t bits = fixed64();
    double d;
    std::memcpy(&d, &bits, 8);
    return d;
  }

  size_t lengthEnd() {
    const uint64_t len = varint64();
    if (len > end - pos) throw FormatError("Field runs past the end of its message");
    return pos + static_cast<size_t>(len);
  }

  std::string_view bytes() {
    const size_t e = lengthEnd();
    std::string_view v(reinterpret_cast<const char*>(buf + pos), e - pos);
    pos = e;
    return v;
  }

  std::string string() { return std::string(bytes()); }

  Reader sub() {
    const size_t e = lengthEnd();
    Reader r(buf, pos, e);
    pos = e;
    return r;
  }

  void skip(int wire) {
    switch (wire) {
      case kVarint: varint64(); break;
      case kFixed64:
        if (pos + 8 > end) throw FormatError("Truncated field");
        pos += 8;
        break;
      case kBytes: pos = lengthEnd(); break;
      case kFixed32:
        if (pos + 4 > end) throw FormatError("Truncated field");
        pos += 4;
        break;
      default: throw FormatError("Unknown wire type " + std::to_string(wire));
    }
  }
};

class Writer {
 public:
  std::string out;
  void varint(uint64_t v) {
    while (v >= 0x80) {
      out += static_cast<char>((v & 0x7f) | 0x80);
      v >>= 7;
    }
    out += static_cast<char>(v);
  }
  void tag(int field, int wire) { varint(static_cast<uint64_t>(field) << 3 | wire); }
  void svarint(int64_t v) { varint(static_cast<uint64_t>((v << 1) ^ (v >> 63))); }
  void bytes(int field, std::string_view s) {
    tag(field, kBytes);
    varint(s.size());
    out += s;
  }
  void uintField(int field, uint64_t v) {
    tag(field, kVarint);
    varint(v);
  }
  void doubleField(int field, double d) {
    tag(field, kFixed64);
    char b[8];
    std::memcpy(b, &d, 8);
    out.append(b, 8);
  }
};

}
