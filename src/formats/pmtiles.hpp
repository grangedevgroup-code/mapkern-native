#pragma once

#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/value.hpp"
#include "formats/bytes.hpp"

namespace mapkern {

enum class TileType : uint8_t { Unknown = 0, Mvt = 1, Png = 2, Jpeg = 3, Webp = 4, Avif = 5, Mlt = 6 };

struct PmHeader {
  uint64_t rootOffset = 0, rootLength = 0;
  uint64_t metadataOffset = 0, metadataLength = 0;
  uint64_t leafOffset = 0, leafLength = 0;
  uint64_t dataOffset = 0, dataLength = 0;
  uint64_t addressedTiles = 0, tileEntries = 0, tileContents = 0;
  bool clustered = false;
  uint8_t internalCompression = 0;
  uint8_t tileCompression = 0;
  TileType tileType = TileType::Unknown;
  uint8_t minZoom = 0, maxZoom = 0;
  double minLon = -180, minLat = -85, maxLon = 180, maxLat = 85;
  uint8_t centerZoom = 0;
  double centerLon = 0, centerLat = 0;
};

struct PmEntry {
  uint64_t tileId = 0;
  uint64_t offset = 0;
  uint32_t length = 0;
  uint32_t runLength = 0;
};

uint64_t zxyToTileId(int z, uint32_t x, uint32_t y);
void tileIdToZxy(uint64_t id, int& z, uint32_t& x, uint32_t& y);
PmHeader parsePmHeader(const Bytes& bytes);
std::vector<PmEntry> parsePmDirectory(const Bytes& bytes);
const PmEntry* findPmEntry(const std::vector<PmEntry>& entries, uint64_t tileId);
Bytes decompressPm(const Bytes& data, uint8_t compression);

class PmTilesReader {
 public:
  explicit PmTilesReader(std::shared_ptr<ByteSource> source);
  const PmHeader& header();
  Value metadata();
  std::optional<Bytes> tile(int z, uint32_t x, uint32_t y);
  std::string name() const { return source_->name(); }

 private:
  std::shared_ptr<const std::vector<PmEntry>> directory(uint64_t offset, uint64_t length);
  void open();

  std::shared_ptr<ByteSource> source_;
  PmHeader header_;
  bool opened_ = false;
  Bytes headPrefix_;
  std::mutex mutex_;
  std::list<uint64_t> order_;
  std::unordered_map<uint64_t, std::pair<std::shared_ptr<const std::vector<PmEntry>>, std::list<uint64_t>::iterator>> dirs_;
};

}
