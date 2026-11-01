#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <vector>

#include "terrain/dem.hpp"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size < 2) return 0;
  const int side = 1 + data[0] % 64;
  const size_t need = static_cast<size_t>(side) * side * 4;
  if (size - 1 < need) return 0;
  const uint8_t* rgba = data + 1;
  try {
    for (const char* name : {"terrarium", "mapbox"}) {
      if (auto grid = mapkern::decodeDem(rgba, side, mapkern::unpackFor(mapkern::Value(name)), data[0] & 0x80)) {
        auto halves = mapkern::gradients(*grid);
        mapkern::writeFrame(*grid, halves);
        mapkern::sampleElevation(*grid, 0.5, 0.5);
        mapkern::DemGrid other = *grid;
        mapkern::backfill(*grid, other, 1, 0);
      }
    }
    std::vector<float> values(static_cast<size_t>(side) * side);
    std::memcpy(values.data(), rgba, need);
    mapkern::decodeElevations(values.data(), values.size(), side);
  } catch (const std::exception&) {
  }
  return 0;
}
