#include "formats/image.hpp"

#include <cstdlib>
#include <cstring>
#include <new>

#include "diag/diagnostics.hpp"
#include "formats/wuffs.hpp"

#ifdef MAPKERN_HAS_WEBP
#include <webp/decode.h>
#endif

namespace mapkern {

namespace {

constexpr uint32_t kMaxImageSide = 16384;

class RgbaDecode : public wuffs_aux::DecodeImageCallbacks {
 public:
  std::vector<uint8_t> pixels;

 private:
  wuffs_base__pixel_format SelectPixfmt(const wuffs_base__image_config&) override {
    return wuffs_base__make_pixel_format(WUFFS_BASE__PIXEL_FORMAT__RGBA_NONPREMUL);
  }

  AllocPixbufResult AllocPixbuf(const wuffs_base__image_config& config, bool) override {
    const uint64_t len = config.pixcfg.pixbuf_len();
    if (config.pixcfg.width() == 0 || config.pixcfg.height() == 0 || len == 0 || len > SIZE_MAX)
      return AllocPixbufResult(wuffs_aux::DecodeImage_UnsupportedPixelConfiguration);
    try {
      pixels.assign(static_cast<size_t>(len), 0);
    } catch (const std::bad_alloc&) {
      return AllocPixbufResult(wuffs_aux::DecodeImage_OutOfMemory);
    }
    wuffs_base__pixel_buffer pixbuf;
    const wuffs_base__status status = pixbuf.set_from_slice(&config.pixcfg, wuffs_base__make_slice_u8(pixels.data(), pixels.size()));
    if (!status.is_ok()) return AllocPixbufResult(status.message());
    return AllocPixbufResult(wuffs_aux::MemOwner(nullptr, &std::free), pixbuf);
  }
};

}

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
  const double started = diag::steadyMs();
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
    RgbaDecode callbacks;
    wuffs_aux::sync_io::MemoryInput input(data.data(), data.size());
    const wuffs_aux::DecodeImageResult result =
        wuffs_aux::DecodeImage(callbacks, input, wuffs_aux::DecodeImageArgQuirks::DefaultValue(), wuffs_aux::DecodeImageArgFlags::DefaultValue(),
                               wuffs_aux::DecodeImageArgPixelBlend::DefaultValue(), wuffs_aux::DecodeImageArgBackgroundColor::DefaultValue(),
                               wuffs_aux::DecodeImageArgMaxInclDimension(kMaxImageSide));
    if (!result.error_message.empty() || !result.pixbuf.pixcfg.is_valid()) {
      if (error) *error = "Image could not be decoded: " + (result.error_message.empty() ? std::string("unknown") : result.error_message);
      return std::nullopt;
    }
    image.width = static_cast<int>(result.pixbuf.pixcfg.width());
    image.height = static_cast<int>(result.pixbuf.pixcfg.height());
    image.rgba = std::move(callbacks.pixels);
  }
  if (premultiply) premultiplyAlpha(image);
  return image;
}

}
