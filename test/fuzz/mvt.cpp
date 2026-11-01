#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <vector>

#include "formats/mvt.hpp"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  auto bytes = std::make_shared<const std::vector<uint8_t>>(data, data + size);
  try {
    const mapkern::VectorTile tile = mapkern::decodeVectorTile(bytes);
    for (const auto& [name, layer] : tile) {
      for (size_t i = 0; i < layer->size() && i < 4096; i++) {
        const mapkern::VectorFeature feature = layer->feature(i);
        feature.properties();
        const auto rings = feature.geometry();
        if (feature.type == 3) mapkern::classifyRings(rings);
      }
    }
  } catch (const std::exception&) {
  }
  return 0;
}
