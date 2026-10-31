#include "formats/inflate.hpp"

#include <array>
#include <cstring>

namespace mapkern {

namespace {

constexpr uint16_t kLengthBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                      31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr uint8_t kLengthExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr uint16_t kDistBase[30] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
                                    193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
constexpr uint8_t kCodeOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

constexpr int kFastBits = 10;

struct Huffman {
  std::array<uint16_t, 1 << kFastBits> fast{};
  uint16_t counts[16]{};
  uint16_t symbols[320]{};
  bool build(const uint8_t* lengths, int n) {
    std::memset(counts, 0, sizeof counts);
    for (int i = 0; i < n; i++) counts[lengths[i]]++;
    counts[0] = 0;
    int left = 1;
    for (int len = 1; len < 16; len++) {
      left <<= 1;
      left -= counts[len];
      if (left < 0) return false;
    }
    uint16_t offsets[16];
    offsets[1] = 0;
    for (int len = 1; len < 15; len++) offsets[len + 1] = offsets[len] + counts[len];
    for (int i = 0; i < n; i++)
      if (lengths[i]) symbols[offsets[lengths[i]]++] = static_cast<uint16_t>(i);
    fast.fill(0xffff);
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= kFastBits; len++) {
      const int count = counts[len];
      for (int k = 0; k < count; k++) {
        const int c = code + k;
        int rev = 0;
        for (int b = 0; b < len; b++) rev |= ((c >> b) & 1) << (len - 1 - b);
        const uint16_t entry = static_cast<uint16_t>((symbols[index + k] << 4) | len);
        for (int fill = rev; fill < (1 << kFastBits); fill += 1 << len) fast[fill] = entry;
      }
      index += count;
      first += count;
      code = (code + count) << 1;
    }
    (void)first;
    return true;
  }
};

struct Bits {
  const uint8_t* data;
  size_t size;
  size_t pos;
  uint64_t buf = 0;
  int count = 0;

  void fill() {
    while (count <= 56) {
      const uint64_t byte = pos < size ? data[pos] : 0;
      if (pos < size + 8) pos++;
      buf |= byte << count;
      count += 8;
    }
  }

  uint32_t take(int n) {
    if (n == 0) return 0;
    if (count < n) fill();
    if (pos > size + 4 && count < n) throw FormatError("Compressed data ends early");
    const uint32_t v = static_cast<uint32_t>(buf & ((uint64_t{1} << n) - 1));
    buf >>= n;
    count -= n;
    return v;
  }

  void align() {
    const int drop = count & 7;
    buf >>= drop;
    count -= drop;
  }

  size_t bytePos() const { return pos - static_cast<size_t>(count / 8); }

  int decode(const Huffman& h) {
    if (count < 16) fill();
    const uint16_t entry = h.fast[buf & ((1u << kFastBits) - 1)];
    if (entry != 0xffff) {
      const int len = entry & 15;
      buf >>= len;
      count -= len;
      return entry >> 4;
    }
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; len++) {
      code |= static_cast<int>(take(1));
      const int c = h.counts[len];
      if (code - c < first) return h.symbols[index + (code - first)];
      index += c;
      first += c;
      first <<= 1;
      code <<= 1;
    }
    throw FormatError("Bad Huffman code");
  }
};

const Huffman& fixedLiterals() {
  static const Huffman h = [] {
    Huffman t;
    uint8_t lengths[288];
    for (int i = 0; i < 144; i++) lengths[i] = 8;
    for (int i = 144; i < 256; i++) lengths[i] = 9;
    for (int i = 256; i < 280; i++) lengths[i] = 7;
    for (int i = 280; i < 288; i++) lengths[i] = 8;
    t.build(lengths, 288);
    return t;
  }();
  return h;
}

const Huffman& fixedDistances() {
  static const Huffman h = [] {
    Huffman t;
    uint8_t lengths[30];
    for (auto& l : lengths) l = 5;
    t.build(lengths, 30);
    return t;
  }();
  return h;
}

size_t inflateInto(Bits& in, std::vector<uint8_t>& out, size_t maxSize) {
  bool last = false;
  Huffman lit, dist;
  while (!last) {
    last = in.take(1) != 0;
    const uint32_t type = in.take(2);
    if (type == 0) {
      in.align();
      const uint32_t len = in.take(16);
      const uint32_t nlen = in.take(16);
      if ((len ^ 0xffff) != nlen) throw FormatError("Stored block length is corrupt");
      for (uint32_t i = 0; i < len; i++) out.push_back(static_cast<uint8_t>(in.take(8)));
      if (out.size() > maxSize) throw FormatError("Decompressed data is too large");
      continue;
    }
    const Huffman* l = &fixedLiterals();
    const Huffman* d = &fixedDistances();
    if (type == 2) {
      const int hlit = static_cast<int>(in.take(5)) + 257;
      const int hdist = static_cast<int>(in.take(5)) + 1;
      const int hclen = static_cast<int>(in.take(4)) + 4;
      if (hlit > 286 || hdist > 30) throw FormatError("Bad dynamic block header");
      uint8_t clen[19] = {};
      for (int i = 0; i < hclen; i++) clen[kCodeOrder[i]] = static_cast<uint8_t>(in.take(3));
      Huffman codes;
      if (!codes.build(clen, 19)) throw FormatError("Bad code lengths");
      uint8_t lengths[320] = {};
      int n = 0;
      while (n < hlit + hdist) {
        const int sym = in.decode(codes);
        if (sym < 16) {
          lengths[n++] = static_cast<uint8_t>(sym);
        } else {
          int repeat = 0;
          uint8_t value = 0;
          if (sym == 16) {
            if (n == 0) throw FormatError("Repeat with no previous length");
            value = lengths[n - 1];
            repeat = 3 + static_cast<int>(in.take(2));
          } else if (sym == 17) {
            repeat = 3 + static_cast<int>(in.take(3));
          } else {
            repeat = 11 + static_cast<int>(in.take(7));
          }
          if (n + repeat > hlit + hdist) throw FormatError("Too many code lengths");
          while (repeat--) lengths[n++] = value;
        }
      }
      if (lengths[256] == 0) throw FormatError("No end-of-block code");
      if (!lit.build(lengths, hlit) || !dist.build(lengths + hlit, hdist)) throw FormatError("Bad Huffman lengths");
      l = &lit;
      d = &dist;
    } else if (type != 1) {
      throw FormatError("Bad block type");
    }
    while (true) {
      const int sym = in.decode(*l);
      if (sym < 256) {
        out.push_back(static_cast<uint8_t>(sym));
        continue;
      }
      if (sym == 256) break;
      const int li = sym - 257;
      if (li >= 29) throw FormatError("Bad length code");
      const size_t len = kLengthBase[li] + in.take(kLengthExtra[li]);
      const int ds = in.decode(*d);
      if (ds >= 30) throw FormatError("Bad distance code");
      const size_t distance = kDistBase[ds] + in.take(kDistExtra[ds]);
      if (distance > out.size()) throw FormatError("Distance is too far back");
      if (out.size() + len > maxSize) throw FormatError("Decompressed data is too large");
      const size_t start = out.size() - distance;
      out.reserve(out.size() + len);
      for (size_t i = 0; i < len; i++) out.push_back(out[start + i]);
    }
    if (out.size() > maxSize) throw FormatError("Decompressed data is too large");
  }
  in.align();
  return in.bytePos();
}

std::array<uint32_t, 256> crcTable() {
  std::array<uint32_t, 256> t{};
  for (uint32_t i = 0; i < 256; i++) {
    uint32_t c = i;
    for (int k = 0; k < 8; k++) c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
    t[i] = c;
  }
  return t;
}

}

uint32_t crc32(std::span<const uint8_t> data, uint32_t crc) {
  static const auto table = crcTable();
  crc = ~crc;
  for (uint8_t b : data) crc = table[(crc ^ b) & 0xff] ^ (crc >> 8);
  return ~crc;
}

std::vector<uint8_t> inflateRaw(std::span<const uint8_t> data, size_t sizeHint, size_t maxSize) {
  std::vector<uint8_t> out;
  out.reserve(sizeHint ? sizeHint : data.size() * 4);
  Bits bits{data.data(), data.size(), 0};
  inflateInto(bits, out, maxSize);
  return out;
}

bool isGzip(std::span<const uint8_t> data) { return data.size() >= 3 && data[0] == 0x1f && data[1] == 0x8b && data[2] == 8; }

bool isZlib(std::span<const uint8_t> data) {
  return data.size() >= 2 && (data[0] & 0x0f) == 8 && (data[0] >> 4) <= 7 && ((data[0] << 8) | data[1]) % 31 == 0;
}

std::vector<uint8_t> inflateZlib(std::span<const uint8_t> data, size_t sizeHint) {
  if (!isZlib(data)) throw FormatError("Not zlib data");
  if (data[1] & 0x20) throw FormatError("zlib preset dictionaries are not supported");
  return inflateRaw(data.subspan(2), sizeHint);
}

std::vector<uint8_t> gunzip(std::span<const uint8_t> data) {
  std::vector<uint8_t> out;
  size_t pos = 0;
  while (pos + 10 <= data.size() && data[pos] == 0x1f && data[pos + 1] == 0x8b) {
    if (data[pos + 2] != 8) throw FormatError("Unknown gzip compression");
    const uint8_t flags = data[pos + 3];
    size_t p = pos + 10;
    if (flags & 4) {
      if (p + 2 > data.size()) throw FormatError("gzip header ends early");
      p += 2 + (data[p] | (data[p + 1] << 8));
    }
    if (flags & 8)
      while (p < data.size() && data[p++] != 0) {
      }
    if (flags & 16)
      while (p < data.size() && data[p++] != 0) {
      }
    if (flags & 2) p += 2;
    if (p > data.size()) throw FormatError("gzip header ends early");
    Bits bits{data.data() + p, data.size() - p, 0};
    const size_t before = out.size();
    const size_t used = inflateInto(bits, out, 1024u << 20);
    p += used;
    if (p + 8 > data.size()) break;
    const uint32_t want = data[p] | (data[p + 1] << 8) | (data[p + 2] << 16) | (static_cast<uint32_t>(data[p + 3]) << 24);
    if (crc32(std::span<const uint8_t>(out.data() + before, out.size() - before)) != want) throw FormatError("gzip checksum mismatch");
    pos = p + 8;
  }
  if (out.empty() && !isGzip(data)) throw FormatError("Not gzip data");
  return out;
}

std::vector<uint8_t> decompressAuto(std::span<const uint8_t> data) {
  if (isGzip(data)) return gunzip(data);
  if (isZlib(data)) return inflateZlib(data);
  return std::vector<uint8_t>(data.begin(), data.end());
}

}
