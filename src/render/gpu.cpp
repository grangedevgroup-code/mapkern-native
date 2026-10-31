#include "render/gpu.hpp"

#include <cstring>

#include "render/shaders/map.glsl.h"

namespace mapkern {

void GpuBuffer::release() {
  if (buffer.id && sg_isvalid()) sg_destroy_buffer(buffer);
  buffer = {};
  bytes = 0;
}

void GpuTexture::release() {
  if (sg_isvalid()) {
    if (view.id) sg_destroy_view(view);
    if (image.id) sg_destroy_image(image);
  }
  view = {};
  image = {};
  bytes = 0;
}

Gpu& Gpu::get() {
  static Gpu gpu;
  return gpu;
}

sg_backend Gpu::backend() const { return sg_query_backend(); }

static void logger(const char* tag, uint32_t level, uint32_t id, const char* message, uint32_t line, const char* file, void*) {
  if (level <= 1) std::fprintf(stderr, "[sokol %s] %u %s (%s:%u)\n", tag ? tag : "", id, message ? message : "", file ? file : "", line);
}

void Gpu::setup(const sg_environment& environment) {
  if (ready_) return;
  sg_desc desc{};
  desc.environment = environment;
  desc.logger.func = logger;
  desc.buffer_pool_size = 65536;
  desc.image_pool_size = 16384;
  desc.view_pool_size = 16384;
  desc.sampler_pool_size = 64;
  desc.shader_pool_size = 64;
  desc.pipeline_pool_size = 512;
  desc.uniform_buffer_size = 32 << 20;
  sg_setup(&desc);
  shaders_[static_cast<int>(Program::Solid)] = sg_make_shader(mk_solid_shader_desc(sg_query_backend()));
  shaders_[static_cast<int>(Program::Fill)] = sg_make_shader(mk_fill_shader_desc(sg_query_backend()));
  shaders_[static_cast<int>(Program::Line)] = sg_make_shader(mk_line_shader_desc(sg_query_backend()));
  shaders_[static_cast<int>(Program::Circle)] = sg_make_shader(mk_circle_shader_desc(sg_query_backend()));
  shaders_[static_cast<int>(Program::Extrusion)] = sg_make_shader(mk_extrusion_shader_desc(sg_query_backend()));
  shaders_[static_cast<int>(Program::FillClip)] = sg_make_shader(mk_fill_clip_shader_desc(sg_query_backend()));
  shaders_[static_cast<int>(Program::LineClip)] = sg_make_shader(mk_line_clip_shader_desc(sg_query_backend()));
  shaders_[static_cast<int>(Program::CircleClip)] = sg_make_shader(mk_circle_clip_shader_desc(sg_query_backend()));
  shaders_[static_cast<int>(Program::ExtrusionClip)] = sg_make_shader(mk_extrusion_clip_shader_desc(sg_query_backend()));
  shaders_[static_cast<int>(Program::Raster)] = sg_make_shader(mk_raster_shader_desc(sg_query_backend()));
  const float quad[] = {0, 0, 1, 0, 0, 1, 1, 0, 1, 1, 0, 1};
  sg_buffer_desc q{};
  q.data = SG_RANGE(quad);
  q.label = "quad";
  quad_ = sg_make_buffer(&q);
  const uint8_t white[4] = {0, 0, 0, 0};
  blank_ = texture(1, 1, SG_PIXELFORMAT_RGBA8, white, 4);
  const float zero[4] = {0, 0, 0, 0};
  blankFloat_ = floatTexture(1, 1, zero);
  ready_ = true;
}

void Gpu::shutdown() {
  if (!ready_) return;
  pipelines_.clear();
  samplers_.clear();
  blank_ = {};
  blankFloat_ = {};
  sg_shutdown();
  ready_ = false;
}

PassFormat Gpu::defaultFormat() const {
  const sg_environment env = sg_query_desc().environment;
  return {env.defaults.color_format, env.defaults.depth_format, env.defaults.sample_count};
}

sg_sampler Gpu::sampler(bool linear, bool mipmap, bool repeat) {
  const int key = (linear ? 1 : 0) | (mipmap ? 2 : 0) | (repeat ? 4 : 0);
  if (auto it = samplers_.find(key); it != samplers_.end()) return it->second;
  sg_sampler_desc d{};
  d.min_filter = linear ? SG_FILTER_LINEAR : SG_FILTER_NEAREST;
  d.mag_filter = linear ? SG_FILTER_LINEAR : SG_FILTER_NEAREST;
  d.mipmap_filter = mipmap ? SG_FILTER_LINEAR : SG_FILTER_NEAREST;
  if (!mipmap) d.max_lod = 0;
  d.wrap_u = repeat ? SG_WRAP_REPEAT : SG_WRAP_CLAMP_TO_EDGE;
  d.wrap_v = repeat ? SG_WRAP_REPEAT : SG_WRAP_CLAMP_TO_EDGE;
  if (linear && mipmap) d.max_anisotropy = 4;
  sg_sampler s = sg_make_sampler(&d);
  samplers_[key] = s;
  return s;
}

GpuBuffer Gpu::vertexBuffer(const void* data, size_t bytes, const char* label) {
  GpuBuffer b;
  if (bytes == 0) return b;
  sg_buffer_desc d{};
  d.data = {data, bytes};
  d.label = label;
  b.buffer = sg_make_buffer(&d);
  b.bytes = bytes;
  uploaded_ += bytes;
  return b;
}

GpuBuffer Gpu::indexBuffer(const void* data, size_t bytes, const char* label) {
  GpuBuffer b;
  if (bytes == 0) return b;
  sg_buffer_desc d{};
  d.usage.index_buffer = true;
  d.usage.vertex_buffer = false;
  d.data = {data, bytes};
  d.label = label;
  b.buffer = sg_make_buffer(&d);
  b.bytes = bytes;
  uploaded_ += bytes;
  return b;
}

GpuTexture Gpu::texture(int width, int height, sg_pixel_format format, const void* pixels, size_t bytes, bool mipmaps) {
  GpuTexture t;
  sg_image_desc d{};
  d.width = width;
  d.height = height;
  d.pixel_format = format;
  if (mipmaps && pixels && format == SG_PIXELFORMAT_RGBA8) {
    int levels = 1;
    for (int w = width, h = height; (w > 1 || h > 1) && levels < SG_MAX_MIPMAPS; levels++) {
      w = w > 1 ? w / 2 : 1;
      h = h > 1 ? h / 2 : 1;
    }
    std::vector<std::vector<uint8_t>> chain;
    chain.emplace_back(static_cast<const uint8_t*>(pixels), static_cast<const uint8_t*>(pixels) + bytes);
    int w = width, h = height;
    for (int l = 1; l < levels; l++) {
      const int nw = w > 1 ? w / 2 : 1, nh = h > 1 ? h / 2 : 1;
      const auto& prev = chain.back();
      std::vector<uint8_t> next(static_cast<size_t>(nw) * nh * 4);
      for (int y = 0; y < nh; y++)
        for (int x = 0; x < nw; x++)
          for (int c = 0; c < 4; c++) {
            const int x0 = std::min(x * 2, w - 1), x1 = std::min(x * 2 + 1, w - 1);
            const int y0 = std::min(y * 2, h - 1), y1 = std::min(y * 2 + 1, h - 1);
            const int sum = prev[(y0 * w + x0) * 4 + c] + prev[(y0 * w + x1) * 4 + c] + prev[(y1 * w + x0) * 4 + c] + prev[(y1 * w + x1) * 4 + c];
            next[(y * nw + x) * 4 + c] = static_cast<uint8_t>((sum + 2) / 4);
          }
      chain.push_back(std::move(next));
      w = nw;
      h = nh;
    }
    d.num_mipmaps = levels;
    for (int l = 0; l < levels; l++) d.data.mip_levels[l] = {chain[l].data(), chain[l].size()};
    t.image = sg_make_image(&d);
  } else {
    d.data.mip_levels[0] = {pixels, bytes};
    t.image = sg_make_image(&d);
  }
  sg_view_desc v{};
  v.texture.image = t.image;
  t.view = sg_make_view(&v);
  t.width = width;
  t.height = height;
  t.bytes = bytes ? bytes : static_cast<size_t>(width) * height * 4;
  uploaded_ += t.bytes;
  return t;
}

GpuTexture Gpu::floatTexture(int width, int height, const float* rgba) {
  return texture(width, height, SG_PIXELFORMAT_RGBA32F, rgba, static_cast<size_t>(width) * height * 16);
}

void Gpu::updateTexture(GpuTexture& tex, const void* pixels, size_t bytes) {
  const sg_pixel_format format = sg_query_image_desc(tex.image).pixel_format;
  const int w = tex.width, h = tex.height;
  tex.release();
  tex = texture(w, h, format, pixels, bytes);
}

sg_pipeline Gpu::pipeline(const PipelineKey& key) {
  if (auto it = pipelines_.find(key); it != pipelines_.end()) return it->second;
  sg_pipeline_desc d{};
  d.shader = shaders_[static_cast<int>(key.program)];
  d.primitive_type = key.primitive;
  auto& layout = d.layout;
  switch (key.program) {
    case Program::Solid:
    case Program::Raster:
      layout.attrs[0].format = SG_VERTEXFORMAT_FLOAT2;
      break;
    case Program::Fill:
    case Program::FillClip:
      layout.buffers[0].stride = 8;
      layout.attrs[ATTR_mk_fill_a_pos] = {0, 0, SG_VERTEXFORMAT_SHORT2};
      layout.attrs[ATTR_mk_fill_a_feature] = {0, 4, SG_VERTEXFORMAT_UINT};
      break;
    case Program::Line:
    case Program::LineClip:
      layout.buffers[0].stride = 20;
      layout.attrs[ATTR_mk_line_a_pos] = {0, 0, SG_VERTEXFORMAT_SHORT2};
      layout.attrs[ATTR_mk_line_a_extrude] = {0, 4, SG_VERTEXFORMAT_SHORT2};
      layout.attrs[ATTR_mk_line_a_side] = {0, 8, SG_VERTEXFORMAT_BYTE4};
      layout.attrs[ATTR_mk_line_a_dist] = {0, 12, SG_VERTEXFORMAT_FLOAT};
      layout.attrs[ATTR_mk_line_a_feature] = {0, 16, SG_VERTEXFORMAT_UINT};
      break;
    case Program::Circle:
    case Program::CircleClip:
      layout.buffers[0].stride = 12;
      layout.attrs[ATTR_mk_circle_a_pos] = {0, 0, SG_VERTEXFORMAT_SHORT2};
      layout.attrs[ATTR_mk_circle_a_corner] = {0, 4, SG_VERTEXFORMAT_BYTE4};
      layout.attrs[ATTR_mk_circle_a_feature] = {0, 8, SG_VERTEXFORMAT_UINT};
      break;
    case Program::Extrusion:
    case Program::ExtrusionClip:
      layout.buffers[0].stride = 12;
      layout.attrs[ATTR_mk_extrusion_a_pos] = {0, 0, SG_VERTEXFORMAT_SHORT2};
      layout.attrs[ATTR_mk_extrusion_a_normal] = {0, 4, SG_VERTEXFORMAT_BYTE4};
      layout.attrs[ATTR_mk_extrusion_a_feature] = {0, 8, SG_VERTEXFORMAT_UINT};
      break;
    default: break;
  }
  if (key.program != Program::Solid && key.program != Program::Raster) d.index_type = SG_INDEXTYPE_UINT32;
  if (key.blend == Blend::Premultiplied) {
    d.colors[0].blend.enabled = true;
    d.colors[0].blend.src_factor_rgb = SG_BLENDFACTOR_ONE;
    d.colors[0].blend.dst_factor_rgb = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    d.colors[0].blend.src_factor_alpha = SG_BLENDFACTOR_ONE;
    d.colors[0].blend.dst_factor_alpha = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
  }
  if (!key.colorWrite) d.colors[0].write_mask = SG_COLORMASK_NONE;
  d.colors[0].pixel_format = key.color;
  d.depth.pixel_format = key.depthFormat;
  d.sample_count = key.samples;
  if (key.depth == DepthMode::Test || key.depth == DepthMode::TestWrite) {
    d.depth.compare = SG_COMPAREFUNC_LESS_EQUAL;
    d.depth.write_enabled = key.depth == DepthMode::TestWrite;
  }
  if (key.stencil != StencilMode::Off) {
    d.stencil.enabled = true;
    d.stencil.read_mask = 0xff;
    d.stencil.write_mask = 0xff;
    sg_stencil_face_state face{};
    if (key.stencil == StencilMode::Write) {
      face.compare = SG_COMPAREFUNC_ALWAYS;
      face.pass_op = SG_STENCILOP_REPLACE;
    } else if (key.stencil == StencilMode::Equal) {
      face.compare = SG_COMPAREFUNC_EQUAL;
      face.pass_op = SG_STENCILOP_KEEP;
      d.stencil.write_mask = 0;
    } else {
      face.compare = SG_COMPAREFUNC_EQUAL;
      face.pass_op = SG_STENCILOP_INCR_CLAMP;
    }
    d.stencil.front = face;
    d.stencil.back = face;
  }
  if (key.cullBack) {
    d.cull_mode = SG_CULLMODE_BACK;
    d.face_winding = SG_FACEWINDING_CCW;
  }
  sg_pipeline p = sg_make_pipeline(&d);
  pipelines_[key] = p;
  return p;
}

}
