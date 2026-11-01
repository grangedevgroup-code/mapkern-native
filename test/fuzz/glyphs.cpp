#include <cstddef>
#include <cstdint>
#include <exception>

#include "text/glyphs.hpp"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  const mapkern::Bytes bytes(data, data + size);
  try {
    mapkern::decodeGlyphs(bytes);
  } catch (const std::exception&) {
  }
  return 0;
}
