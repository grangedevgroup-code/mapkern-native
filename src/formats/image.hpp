#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "formats/bytes.hpp"

namespace mapkern {

struct Image {
  int width = 0;
  int height = 0;
  std::vector<uint8_t> rgba;
  bool premultiplied = false;
};

using MipChain = std::vector<std::vector<uint8_t>>;

std::optional<Image> decodeImage(const Bytes& data, bool premultiply = true, std::string* error = nullptr);
bool isWebp(const Bytes& data);
void premultiplyAlpha(Image& image);
MipChain mipChain(const uint8_t* rgba, int width, int height, int maxLevels);

}
