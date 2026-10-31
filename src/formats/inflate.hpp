#pragma once

#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

namespace mapkern {

class FormatError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

std::vector<uint8_t> inflateRaw(std::span<const uint8_t> data, size_t sizeHint = 0, size_t maxSize = 512u << 20);
std::vector<uint8_t> inflateZlib(std::span<const uint8_t> data, size_t sizeHint = 0);
std::vector<uint8_t> gunzip(std::span<const uint8_t> data);
bool isGzip(std::span<const uint8_t> data);
bool isZlib(std::span<const uint8_t> data);
std::vector<uint8_t> decompressAuto(std::span<const uint8_t> data);
uint32_t crc32(std::span<const uint8_t> data, uint32_t crc = 0);

}
