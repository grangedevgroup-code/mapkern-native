#include <cstddef>
#include <cstdint>
#include <exception>

#include "formats/pmtiles.hpp"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  const mapkern::Bytes bytes(data, data + size);
  try {
    mapkern::parsePmHeader(bytes);
  } catch (const std::exception&) {
  }
  try {
    const auto entries = mapkern::parsePmDirectory(bytes);
    mapkern::findPmEntry(entries, mapkern::zxyToTileId(5, 9, 12));
  } catch (const std::exception&) {
  }
  for (uint8_t compression = 0; compression < 6; compression++) {
    try {
      mapkern::decompressPm(bytes, compression);
    } catch (const std::exception&) {
    }
  }
  return 0;
}
