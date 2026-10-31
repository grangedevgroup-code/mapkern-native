#include "formats/image.hpp"

#include <cstring>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_GIF
#define STBI_ONLY_BMP
#include "stb/stb_image.h"

#ifdef MAPKERN_HAS_WEBP
#include <webp/decode.h>
#endif

namespace mapkern {

bool isWebp(const Bytes& data) {
  return data.size() >= 12 && std::memcmp(data.data(), "RIFF", 4) == 0 && std::memcmp(data.data() + 8, "WEBP", 4) == 0;
}

void premultiplyAlpha(Image& image) {
  if (image.premultiplied) return;
  uint8_t* p = image.rgba.data();
  const size_t n = image.rgba.size() / 4;
  for (size_t i = 0; i < n; i++, p += 4) {
    const unsigned a = p[3];
    if (a == 255) continue;
    p[0] = static_cast<uint8_t>((p[0] * a + 127) / 255);
    p[1] = static_cast<uint8_t>((p[1] * a + 127) / 255);
    p[2] = static_cast<uint8_t>((p[2] * a + 127) / 255);
  }
  image.premultiplied = true;
}

std::optional<Image> decodeImage(const Bytes& data, bool premultiply, std::string* error) {
  Image image;
  if (isWebp(data)) {
#ifdef MAPKERN_HAS_WEBP
    int w = 0, h = 0;
    uint8_t* pixels = WebPDecodeRGBA(data.data(), data.size(), &w, &h);
    if (!pixels) {
      if (error) *error = "WebP image could not be decoded";
      return std::nullopt;
    }
    image.width = w;
    image.height = h;
    image.rgba.assign(pixels, pixels + static_cast<size_t>(w) * h * 4);
    WebPFree(pixels);
#else
    if (error) *error = "WebP support is not built in";
    return std::nullopt;
#endif
  } else {
    int w = 0, h = 0, channels = 0;
    stbi_uc* pixels = stbi_load_from_memory(data.data(), static_cast<int>(data.size()), &w, &h, &channels, 4);
    if (!pixels) {
      if (error) *error = std::string("Image could not be decoded: ") + (stbi_failure_reason() ? stbi_failure_reason() : "unknown");
      return std::nullopt;
    }
    image.width = w;
    image.height = h;
    image.rgba.assign(pixels, pixels + static_cast<size_t>(w) * h * 4);
    stbi_image_free(pixels);
  }
  if (premultiply) premultiplyAlpha(image);
  return image;
}

}
