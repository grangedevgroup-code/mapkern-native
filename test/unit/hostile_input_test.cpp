#include <chrono>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "check.hpp"
#include "formats/inflate.hpp"
#include "formats/pmtiles.hpp"
#include "formats/sqlite.hpp"

using namespace mapkern;

namespace {

double secondsSince(std::chrono::steady_clock::time_point start) {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

class BitWriter {
 public:
  std::vector<uint8_t> bytes;
  void put(uint32_t value, int bits) {
    for (int i = 0; i < bits; i++) push((value >> i) & 1);
  }
  void code(uint32_t value, int bits) {
    for (int i = bits - 1; i >= 0; i--) push((value >> i) & 1);
  }

 private:
  int used_ = 8;
  void push(uint32_t bit) {
    if (used_ == 8) {
      bytes.push_back(0);
      used_ = 0;
    }
    bytes.back() |= static_cast<uint8_t>(bit << used_++);
  }
};

std::vector<uint8_t> zeroRuns(int runs) {
  BitWriter w;
  w.put(1, 1);
  w.put(1, 2);
  w.code(0x30, 8);
  for (int i = 0; i < runs; i++) {
    w.code(0xc5, 8);
    w.code(0, 5);
  }
  w.code(0, 7);
  return w.bytes;
}

Bytes interiorPage(const std::vector<uint16_t>& cells, uint32_t rightmost, const std::vector<std::pair<uint16_t, uint32_t>>& pointers) {
  Bytes page(512, 0);
  std::memcpy(page.data(), "SQLite format 3\0", 16);
  page[16] = 0x02;
  page[17] = 0x00;
  page[59] = 1;
  page[100] = 0x05;
  page[103] = static_cast<uint8_t>(cells.size() >> 8);
  page[104] = static_cast<uint8_t>(cells.size());
  page[108] = static_cast<uint8_t>(rightmost >> 24);
  page[109] = static_cast<uint8_t>(rightmost >> 16);
  page[110] = static_cast<uint8_t>(rightmost >> 8);
  page[111] = static_cast<uint8_t>(rightmost);
  for (size_t i = 0; i < cells.size(); i++) {
    page[112 + i * 2] = static_cast<uint8_t>(cells[i] >> 8);
    page[113 + i * 2] = static_cast<uint8_t>(cells[i]);
  }
  for (const auto& [at, child] : pointers) {
    page[at] = static_cast<uint8_t>(child >> 24);
    page[at + 1] = static_cast<uint8_t>(child >> 16);
    page[at + 2] = static_cast<uint8_t>(child >> 8);
    page[at + 3] = static_cast<uint8_t>(child);
    page[at + 4] = 1;
  }
  return page;
}

template <class F>
bool fails(F f) {
  try {
    f();
  } catch (const FormatError&) {
    return true;
  }
  return false;
}

}

TEST("hostile input: a truncated gzip stream fails at once instead of inflating zeros") {
  const Bytes stream = {0x1f, 0x8b, 0x08, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0x63, 0x66, 0x00, 0x2c, 0x2c, 0x2c, 0x2c, 0x2c, 0x2c,
                        0x2c, 0x2c, 0x2c, 0x2c, 0x2c, 0x2c, 0x2c, 0x2c, 0x2c, 0x2c, 0x2c, 0x2c, 0x2c, 0x2c, 0xeb, 0x2c, 0x2c, 0x2c, 0x2c, 0x2c,
                        0x2c, 0x2c, 0x2c, 0x2c, 0x2c, 0x2c, 0x66, 0x60, 0x64, 0x64, 0x64, 0x64, 0x8b, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  const auto start = std::chrono::steady_clock::now();
  EXPECT(fails([&] { gunzip(stream); }));
  EXPECT(fails([&] { decompressPm(stream, 2); }));
  EXPECT(secondsSince(start) < 1);
}

TEST("hostile input: every cut of a deflate stream fails fast") {
  const auto whole = zeroRuns(40);
  EXPECT(inflateRaw(whole).size() == 1 + 40 * 258);
  const auto start = std::chrono::steady_clock::now();
  for (size_t cut = 0; cut + 1 < whole.size(); cut++) {
    const std::vector<uint8_t> part(whole.begin(), whole.begin() + static_cast<std::ptrdiff_t>(cut));
    try {
      const auto out = inflateRaw(part);
      EXPECT(out.size() <= 1 + 40 * 258);
    } catch (const FormatError&) {
    }
  }
  EXPECT(secondsSince(start) < 1);
}

TEST("hostile input: decompression stops at the size limit") {
  const auto bomb = zeroRuns(20000);
  EXPECT(bomb.size() < 40000);
  EXPECT(inflateRaw(bomb).size() == 1 + 20000u * 258);
  EXPECT(fails([&] { inflateRaw(bomb, 0, 1u << 20); }));
  EXPECT(kMaxInflate <= (64u << 20));
}

TEST("hostile input: an SQLite child pointer past the page end is an error, not a read past the buffer") {
  SqliteReader reader(std::make_shared<MemorySource>(std::make_shared<const Bytes>(interiorPage({510}, 1, {}))));
  EXPECT(fails([&] { reader.schema(); }));
}

TEST("hostile input: an SQLite record that claims a huge header is an error, not a read past the record") {
  Bytes page(512, 0);
  std::memcpy(page.data(), "SQLite format 3\0", 16);
  page[16] = 0x02;
  page[59] = 1;
  page[100] = 0x0d;
  page[104] = 1;
  page[108] = 0x01;
  page[109] = 0x90;
  const uint8_t cell[] = {0x0c, 0x01, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x01, 0x00, 0x00};
  std::memcpy(page.data() + 400, cell, sizeof cell);
  SqliteReader reader(std::make_shared<MemorySource>(std::make_shared<const Bytes>(page)));
  EXPECT(fails([&] { reader.schema(); }));
}

TEST("hostile input: an SQLite table whose pages point back at themselves is rejected quickly") {
  std::vector<uint16_t> cells;
  std::vector<std::pair<uint16_t, uint32_t>> pointers;
  for (uint16_t i = 0; i < 50; i++) cells.push_back(static_cast<uint16_t>(240 + i * 5));
  for (const uint16_t at : cells) pointers.push_back({at, 1});
  SqliteReader reader(std::make_shared<MemorySource>(std::make_shared<const Bytes>(interiorPage(cells, 1, pointers))));
  const auto start = std::chrono::steady_clock::now();
  EXPECT(fails([&] { reader.schema(); }));
  EXPECT(secondsSince(start) < 1);
}
