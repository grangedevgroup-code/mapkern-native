#include <cstddef>
#include <cstdint>
#include <exception>
#include <span>

#include "formats/inflate.hpp"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  const std::span<const uint8_t> input(data, size);
  try {
    mapkern::decompressAuto(input);
  } catch (const std::exception&) {
  }
  try {
    mapkern::inflateRaw(input, 0, 1u << 24);
  } catch (const std::exception&) {
  }
  try {
    mapkern::inflateZlib(input, size);
  } catch (const std::exception&) {
  }
  return 0;
}
