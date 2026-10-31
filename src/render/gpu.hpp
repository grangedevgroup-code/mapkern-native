#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "formats/image.hpp"
#include "render/sokol.hpp"

namespace mapkern {

enum class Program { Solid, Fill, FillClip, Line, LineClip, Circle, CircleClip, Extrusion, ExtrusionClip, Raster, Globe, Atmosphere, Pole, Text, Icon, Terrain, Hillshade, Count };

enum class Blend { None, Premultiplied, Additive };
enum class StencilMode { Off, Write, Equal, Mask };
enum class DepthMode { Off, Test, TestWrite };

struct PipelineKey {
  Program program = Program::Solid;
  sg_primitive_type primitive = SG_PRIMITIVETYPE_TRIANGLES;
  Blend blend = Blend::Premultiplied;
  StencilMode stencil = StencilMode::Off;
  DepthMode depth = DepthMode::Off;
  sg_pixel_format color = _SG_PIXELFORMAT_DEFAULT;
  sg_pixel_format depthFormat = _SG_PIXELFORMAT_DEFAULT;
  int samples = 0;
  bool colorWrite = true;
  bool cullBack = false;
  auto tie() const {
    return std::tie(program, primitive, blend, stencil, depth, color, depthFormat, samples, colorWrite, cullBack);
  }
  bool operator<(const PipelineKey& o) const { return tie() < o.tie(); }
};

struct PassFormat {
  sg_pixel_format color = _SG_PIXELFORMAT_DEFAULT;
  sg_pixel_format depth = _SG_PIXELFORMAT_DEFAULT;
  int samples = 0;
};

struct GpuBuffer {
  sg_buffer buffer{};
  size_t bytes = 0;
  void release();
};

struct GpuTexture {
  sg_image image{};
  sg_view view{};
  int width = 0;
  int height = 0;
  size_t bytes = 0;
  void release();
  bool valid() const { return image.id != 0; }
};

struct SlowUpload {
  double ms = 0;
  int width = 0;
  int height = 0;
  int levels = 0;
  sg_pixel_format format = _SG_PIXELFORMAT_DEFAULT;
};

const char* pixelFormatName(sg_pixel_format format);

class Gpu {
 public:
  static Gpu& get();
  bool ready() const { return ready_; }
  void setup(const sg_environment& environment);
  void shutdown();
  sg_pipeline pipeline(const PipelineKey& key);
  sg_sampler sampler(bool linear, bool mipmap = false, bool repeat = false);
  sg_buffer quad() const { return quad_; }
  GpuBuffer vertexBuffer(const void* data, size_t bytes, const char* label = nullptr);
  GpuBuffer indexBuffer(const void* data, size_t bytes, const char* label = nullptr);
  GpuTexture texture(int width, int height, sg_pixel_format format, const void* pixels, size_t bytes, bool mipmaps = false);
  GpuTexture mipmappedTexture(int width, int height, const void* pixels, size_t bytes, const MipChain& below);
  GpuTexture floatTexture(int width, int height, const float* rgba);
  void updateTexture(GpuTexture& tex, const void* pixels, size_t bytes);
  const GpuTexture& blankTexture() const { return blank_; }
  const GpuTexture& blankFloatTexture() const { return blankFloat_; }
  const GpuTexture& whiteTexture() const { return white_; }
  PassFormat defaultFormat() const;
  size_t uploadedBytes() const { return uploaded_; }
  void resetUploadCounter() { uploaded_ = 0; }
  SlowUpload takeSlowestUpload() { return std::exchange(slowest_, SlowUpload{}); }
  sg_backend backend() const;

 private:
  GpuTexture make(sg_image_desc& desc, size_t bytes);
  bool ready_ = false;
  std::map<PipelineKey, sg_pipeline> pipelines_;
  std::map<int, sg_sampler> samplers_;
  sg_shader shaders_[static_cast<int>(Program::Count)]{};
  sg_buffer quad_{};
  GpuTexture blank_;
  GpuTexture blankFloat_;
  GpuTexture white_;
  size_t uploaded_ = 0;
  SlowUpload slowest_;
};

}
