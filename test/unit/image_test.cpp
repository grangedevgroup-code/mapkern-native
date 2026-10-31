#include <algorithm>
#include <cstdlib>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "stb/stb_image_write.h"

#include "check.hpp"
#include "formats/image.hpp"

using namespace mapkern;

namespace {

void append(void* context, void* data, int size) {
  auto* out = static_cast<Bytes*>(context);
  const auto* p = static_cast<const uint8_t*>(data);
  out->insert(out->end(), p, p + size);
}

std::vector<uint8_t> pattern(int w, int h, bool alpha) {
  std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4);
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++) {
      uint8_t* p = &px[(static_cast<size_t>(y) * w + x) * 4];
      p[0] = static_cast<uint8_t>(x * 255 / std::max(1, w - 1));
      p[1] = static_cast<uint8_t>(y * 255 / std::max(1, h - 1));
      p[2] = 128;
      p[3] = alpha && (x + y) % 2 == 0 ? 64 : 255;
    }
  return px;
}

Bytes png(int w, int h, const std::vector<uint8_t>& px) {
  Bytes out;
  stbi_write_png_to_func(append, &out, w, h, 4, px.data(), w * 4);
  return out;
}

}

TEST("image: PNG keeps every pixel and its alpha") {
  const auto px = pattern(7, 5, true);
  const auto image = decodeImage(png(7, 5, px), false);
  EXPECT(image && image->width == 7 && image->height == 5 && image->rgba == px);
  const auto premultiplied = decodeImage(png(7, 5, px), true);
  EXPECT(premultiplied && premultiplied->premultiplied);
  if (premultiplied) EXPECT(premultiplied->rgba[3] == 64 && premultiplied->rgba[2] == (128 * 64 + 127) / 255);
}

TEST("image: BMP and JPEG decode") {
  const auto px = pattern(16, 16, false);
  Bytes bmp, jpg;
  EXPECT(stbi_write_bmp_to_func(append, &bmp, 16, 16, 4, px.data()) != 0);
  EXPECT(stbi_write_jpg_to_func(append, &jpg, 16, 16, 4, px.data(), 100) != 0);
  const auto fromBmp = decodeImage(bmp, false);
  EXPECT(fromBmp && fromBmp->rgba == px);
  const auto fromJpg = decodeImage(jpg, false);
  EXPECT(fromJpg && fromJpg->width == 16 && fromJpg->height == 16);
  if (fromJpg) {
    int worst = 0;
    for (size_t i = 0; i < px.size(); i++) worst = std::max(worst, std::abs(int(px[i]) - int(fromJpg->rgba[i])));
    EXPECT(worst <= 12);
  }
}

TEST("image: GIF decodes through its palette") {
  const Bytes gif = {0x47, 0x49, 0x46, 0x38, 0x39, 0x61, 0x02, 0x00, 0x01, 0x00, 0x80, 0x00, 0x00, 0xff, 0x00, 0x00, 0x00, 0x00,
                     0xff, 0x2c, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x01, 0x00, 0x00, 0x02, 0x02, 0x44, 0x0a, 0x00, 0x3b};
  const auto image = decodeImage(gif, false);
  EXPECT(image && image->width == 2 && image->height == 1);
  if (image) EXPECT((image->rgba == std::vector<uint8_t>{255, 0, 0, 255, 0, 0, 255, 255}));
}

TEST("image: tiny files that claim huge sizes are refused") {
  std::string error;
  EXPECT(!decodeImage(Bytes{0x47, 0x49, 0x46, 0x38, 0x39, 0x61, 0x6c, 0x5d, 0x0a, 0x29}, true, &error));
  EXPECT(!error.empty());
  Bytes claims = {0x47, 0x49, 0x46, 0x38, 0x39, 0x61, 0x00, 0x40, 0x40, 0x40};
  claims.resize(64, 0x40);
  EXPECT(!decodeImage(claims));
  EXPECT(!decodeImage(Bytes{1, 2, 3, 4, 5}));
  EXPECT(!decodeImage(Bytes{}));
}

TEST("image: a truncated PNG is an error, not half an image") {
  auto data = png(32, 32, pattern(32, 32, true));
  data.resize(data.size() / 2);
  std::string error;
  EXPECT(!decodeImage(data, true, &error));
  EXPECT(!error.empty());
}

TEST("image: mip chain averages each 2x2 block down to one pixel") {
  const std::vector<uint8_t> rgba{0, 0, 0, 255, 100, 40, 8, 255, 200, 80, 16, 255, 255, 120, 24, 251, 10, 10, 10, 10, 20, 20, 20, 20, 30, 30, 30, 30, 40, 40, 40, 40};
  const MipChain chain = mipChain(rgba.data(), 4, 2, 16);
  EXPECT(chain.size() == 2);
  EXPECT(chain[0].size() == 2 * 1 * 4);
  EXPECT(chain[1].size() == 4);
  EXPECT(chain[0][0] == (0 + 100 + 10 + 20 + 2) / 4);
  EXPECT(chain[0][4] == (200 + 255 + 30 + 40 + 2) / 4);
  EXPECT(chain[0][7] == (255 + 251 + 30 + 40 + 2) / 4);
  EXPECT(chain[1][0] == (chain[0][0] + chain[0][4] + chain[0][0] + chain[0][4] + 2) / 4);
}

TEST("image: mip chain stops at one pixel or the level limit") {
  const std::vector<uint8_t> rgba(64 * 64 * 4, 7);
  EXPECT(mipChain(rgba.data(), 64, 64, 16).size() == 6);
  EXPECT(mipChain(rgba.data(), 64, 64, 3).size() == 2);
  EXPECT(mipChain(rgba.data(), 64, 64, 16).back().size() == 4);
  EXPECT(mipChain(rgba.data(), 64, 64, 16).back()[0] == 7);
  EXPECT(mipChain(nullptr, 64, 64, 16).empty());
  EXPECT(mipChain(rgba.data(), 1, 1, 16).empty());
}
