#include <cstddef>
#include <cstdint>
#include <exception>
#include <string>

#include "formats/image.hpp"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  const mapkern::Bytes bytes(data, data + size);
  try {
    std::string error;
    const auto image = mapkern::decodeImage(bytes, true, &error);
    if (image && image->width > 0 && image->height > 0 && image->width <= 4096 && image->height <= 4096)
      mapkern::mipChain(image->rgba.data(), image->width, image->height, 16);
  } catch (const std::exception&) {
  }
  return 0;
}
