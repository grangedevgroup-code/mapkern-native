#include "formats/pmtiles.hpp"

#include <algorithm>
#include <cstring>

#include "formats/protobuf.hpp"

namespace mapkern {

namespace {

uint64_t le64(const uint8_t* p) {
  uint64_t v = 0;
  for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
  return v;
}

int32_t le32s(const uint8_t* p) {
  uint32_t v = 0;
  for (int i = 3; i >= 0; i--) v = (v << 8) | p[i];
  return static_cast<int32_t>(v);
}

void rotate(uint64_t n, uint64_t& x, uint64_t& y, uint64_t rx, uint64_t ry) {
  if (ry == 0) {
    if (rx == 1) {
      x = n - 1 - x;
      y = n - 1 - y;
    }
    std::swap(x, y);
  }
}

}

uint64_t zxyToTileId(int z, uint32_t x, uint32_t y) {
  if (z > 31) throw FormatError("Tile zoom exceeds 31");
  uint64_t acc = 0;
  for (int t = 0; t < z; t++) acc += uint64_t{1} << (2 * t);
  const uint64_t n = uint64_t{1} << z;
  uint64_t xx = x, yy = y, d = 0;
  for (uint64_t s = n / 2; s > 0; s /= 2) {
    const uint64_t rx = (xx & s) > 0 ? 1 : 0;
    const uint64_t ry = (yy & s) > 0 ? 1 : 0;
    d += s * s * ((3 * rx) ^ ry);
    rotate(s, xx, yy, rx, ry);
  }
  return acc + d;
}

void tileIdToZxy(uint64_t id, int& z, uint32_t& x, uint32_t& y) {
  uint64_t acc = 0;
  for (z = 0; z < 32; z++) {
    const uint64_t count = uint64_t{1} << (2 * z);
    if (acc + count > id) break;
    acc += count;
  }
  uint64_t t = id - acc;
  const uint64_t n = uint64_t{1} << z;
  uint64_t xx = 0, yy = 0;
  for (uint64_t s = 1; s < n; s *= 2) {
    const uint64_t rx = 1 & (t / 2);
    const uint64_t ry = 1 & (t ^ rx);
    rotate(s, xx, yy, rx, ry);
    xx += s * rx;
    yy += s * ry;
    t /= 4;
  }
  x = static_cast<uint32_t>(xx);
  y = static_cast<uint32_t>(yy);
}

PmHeader parsePmHeader(const Bytes& b) {
  if (b.size() < 127 || std::memcmp(b.data(), "PMTiles", 7) != 0) throw FormatError("The file is not a PMTiles archive");
  if (b[7] != 3) throw FormatError("Only PMTiles version 3 is supported, this file is version " + std::to_string(b[7]));
  PmHeader h;
  h.rootOffset = le64(&b[8]);
  h.rootLength = le64(&b[16]);
  h.metadataOffset = le64(&b[24]);
  h.metadataLength = le64(&b[32]);
  h.leafOffset = le64(&b[40]);
  h.leafLength = le64(&b[48]);
  h.dataOffset = le64(&b[56]);
  h.dataLength = le64(&b[64]);
  h.addressedTiles = le64(&b[72]);
  h.tileEntries = le64(&b[80]);
  h.tileContents = le64(&b[88]);
  h.clustered = b[96] == 1;
  h.internalCompression = b[97];
  h.tileCompression = b[98];
  h.tileType = static_cast<TileType>(b[99]);
  h.minZoom = b[100];
  h.maxZoom = b[101];
  h.minLon = le32s(&b[102]) / 1e7;
  h.minLat = le32s(&b[106]) / 1e7;
  h.maxLon = le32s(&b[110]) / 1e7;
  h.maxLat = le32s(&b[114]) / 1e7;
  h.centerZoom = b[118];
  h.centerLon = le32s(&b[119]) / 1e7;
  h.centerLat = le32s(&b[123]) / 1e7;
  return h;
}

Bytes decompressPm(const Bytes& data, uint8_t compression) {
  switch (compression) {
    case 0:
    case 1: return data;
    case 2: return gunzip(data);
    default: throw FormatError("PMTiles compression " + std::to_string(compression) + " is not supported");
  }
}

std::vector<PmEntry> parsePmDirectory(const Bytes& bytes) {
  pbf::Reader r(bytes.data(), bytes.size());
  const uint64_t count = r.varint64();
  if (count > bytes.size()) throw FormatError("PMTiles directory is corrupt");
  std::vector<PmEntry> entries(static_cast<size_t>(count));
  uint64_t last = 0;
  for (auto& e : entries) {
    last += r.varint64();
    e.tileId = last;
  }
  for (auto& e : entries) e.runLength = static_cast<uint32_t>(r.varint64());
  for (auto& e : entries) e.length = static_cast<uint32_t>(r.varint64());
  for (size_t i = 0; i < entries.size(); i++) {
    const uint64_t v = r.varint64();
    if (v == 0 && i > 0) entries[i].offset = entries[i - 1].offset + entries[i - 1].length;
    else entries[i].offset = v - 1;
  }
  return entries;
}

const PmEntry* findPmEntry(const std::vector<PmEntry>& entries, uint64_t tileId) {
  auto it = std::upper_bound(entries.begin(), entries.end(), tileId, [](uint64_t id, const PmEntry& e) { return id < e.tileId; });
  if (it == entries.begin()) return nullptr;
  const PmEntry& e = *std::prev(it);
  if (e.tileId == tileId) return &e;
  if (e.runLength == 0) return &e;
  if (tileId - e.tileId < e.runLength) return &e;
  return nullptr;
}

PmTilesReader::PmTilesReader(std::shared_ptr<ByteSource> source) : source_(std::move(source)) {}

void PmTilesReader::open() {
  std::lock_guard lock(mutex_);
  if (opened_) return;
  headPrefix_ = source_->read(0, 16384);
  header_ = parsePmHeader(headPrefix_);
  opened_ = true;
}

const PmHeader& PmTilesReader::header() {
  open();
  return header_;
}

Value PmTilesReader::metadata() {
  open();
  if (header_.metadataLength == 0) return Object{};
  const Bytes raw = source_->read(header_.metadataOffset, static_cast<size_t>(header_.metadataLength));
  const Bytes text = decompressPm(raw, header_.internalCompression);
  return parseJson(std::string_view(reinterpret_cast<const char*>(text.data()), text.size())).value_or(Value(Object{}));
}

std::shared_ptr<const std::vector<PmEntry>> PmTilesReader::directory(uint64_t offset, uint64_t length) {
  {
    std::lock_guard lock(mutex_);
    if (auto it = dirs_.find(offset); it != dirs_.end()) {
      order_.splice(order_.end(), order_, it->second.second);
      return it->second.first;
    }
  }
  if (length > (64u << 20)) throw FormatError("PMTiles directory is too large");
  Bytes raw;
  if (offset + length <= headPrefix_.size())
    raw.assign(headPrefix_.begin() + static_cast<ptrdiff_t>(offset), headPrefix_.begin() + static_cast<ptrdiff_t>(offset + length));
  else
    raw = source_->read(offset, static_cast<size_t>(length));
  auto entries = std::make_shared<const std::vector<PmEntry>>(parsePmDirectory(decompressPm(raw, header_.internalCompression)));
  std::lock_guard lock(mutex_);
  if (!dirs_.contains(offset)) {
    order_.push_back(offset);
    dirs_[offset] = {entries, std::prev(order_.end())};
    if (dirs_.size() > 64) {
      dirs_.erase(order_.front());
      order_.pop_front();
    }
  }
  return entries;
}

std::optional<Bytes> PmTilesReader::tile(int z, uint32_t x, uint32_t y) {
  open();
  if (z < header_.minZoom || z > header_.maxZoom) return std::nullopt;
  const uint64_t id = zxyToTileId(z, x, y);
  uint64_t offset = header_.rootOffset, length = header_.rootLength;
  for (int depth = 0; depth < 4; depth++) {
    const auto dir = directory(offset, length);
    const PmEntry* e = findPmEntry(*dir, id);
    if (!e) return std::nullopt;
    if (e->runLength > 0) {
      Bytes data = source_->read(header_.dataOffset + e->offset, e->length);
      if (data.size() < e->length) throw FormatError("PMTiles tile runs past the end of the file");
      return data;
    }
    offset = header_.leafOffset + e->offset;
    length = e->length;
  }
  throw FormatError("PMTiles directories are nested too deeply");
}

}
