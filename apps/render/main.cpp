#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "map/map_impl.hpp"
#include "mapkern/map.hpp"
#include "render/sokol.hpp"
#include "stb/stb_image_write.h"

#ifdef _WIN32
#include <d3d11.h>
#endif

namespace {

struct Args {
  mapkern::MapOptions options;
  std::string out = "map.png";
  int width = 1280;
  int height = 800;
  double ratio = 1;
  double timeoutMs = 30000;
  double settleMs = 400;
};

double num(int argc, char** argv, int& i) { return i + 1 < argc ? std::atof(argv[++i]) : 0; }

}

int main(int argc, char** argv) {
  Args a;
  a.options.fadeDuration = 0;
  for (int i = 1; i < argc; i++) {
    const std::string s = argv[i];
    if (s == "--style" && i + 1 < argc) a.options.style = argv[++i];
    else if (s == "--center") {
      a.options.lng = num(argc, argv, i);
      a.options.lat = num(argc, argv, i);
    } else if (s == "--zoom") a.options.zoom = num(argc, argv, i);
    else if (s == "--pitch") a.options.pitch = num(argc, argv, i);
    else if (s == "--bearing") a.options.bearing = num(argc, argv, i);
    else if (s == "--size") {
      a.width = static_cast<int>(num(argc, argv, i));
      a.height = static_cast<int>(num(argc, argv, i));
    } else if (s == "--ratio") a.ratio = num(argc, argv, i);
    else if (s == "--out" && i + 1 < argc) a.out = argv[++i];
    else if (s == "--flat") a.options.globe = false;
    else if (s == "--offline") a.options.offline = true;
    else if (s == "--cache" && i + 1 < argc) a.options.cacheDirectory = argv[++i];
    else if (s == "--timeout") a.timeoutMs = num(argc, argv, i);
  }
  a.options.pixelRatio = a.ratio;
  const int pw = static_cast<int>(a.width * a.ratio), ph = static_cast<int>(a.height * a.ratio);
#ifdef _WIN32
  ID3D11Device* device = nullptr;
  ID3D11DeviceContext* context = nullptr;
  D3D_FEATURE_LEVEL level;
  HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, &level, &context);
  if (FAILED(hr)) hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, &level, &context);
  if (FAILED(hr)) {
    std::fprintf(stderr, "could not create a D3D11 device\n");
    return 1;
  }
  sg_environment env{};
  env.defaults.color_format = SG_PIXELFORMAT_RGBA8;
  env.defaults.depth_format = SG_PIXELFORMAT_DEPTH_STENCIL;
  env.defaults.sample_count = 1;
  env.d3d11.device = device;
  env.d3d11.device_context = context;
#else
  std::fprintf(stderr, "headless rendering is only available on Windows in this build\n");
  return 1;
#endif
  mapkern::Map map(a.options);
  std::vector<std::string> errors;
  map.on([&](const mapkern::MapEvent& e) {
    if (e.type == "error") std::fprintf(stderr, "error: %s\n", e.message.c_str());
    if (e.type == "styleload")
      for (const auto& s : e.skipped) std::fprintf(stderr, "skipped: %s\n", s.c_str());
  });
  auto& impl = map.impl();
  impl.initGraphicsNative(env);
  map.resize(a.width, a.height, a.ratio);
  sg_image_desc cd{};
  cd.usage.color_attachment = true;
  cd.width = pw;
  cd.height = ph;
  cd.pixel_format = SG_PIXELFORMAT_RGBA8;
  cd.sample_count = 1;
  sg_image color = sg_make_image(&cd);
  sg_image_desc dd = cd;
  dd.usage.color_attachment = false;
  dd.usage.depth_stencil_attachment = true;
  dd.pixel_format = SG_PIXELFORMAT_DEPTH_STENCIL;
  sg_image depth = sg_make_image(&dd);
  sg_view_desc cv{};
  cv.color_attachment.image = color;
  sg_view_desc dv{};
  dv.depth_stencil_attachment.image = depth;
  mapkern::FrameTarget target;
  target.offscreen = true;
  target.attachments.colors[0] = sg_make_view(&cv);
  target.attachments.depth_stencil = sg_make_view(&dv);
  target.format = {SG_PIXELFORMAT_RGBA8, SG_PIXELFORMAT_DEPTH_STENCIL, 1};
  target.width = pw;
  target.height = ph;
  const double start = mapkern::nowMs();
  double idleSince = 0;
  int frames = 0;
  while (true) {
    impl.renderTarget(target);
    frames++;
    const double now = mapkern::nowMs();
    const bool idle = impl.styleLoaded && !impl.needsRender();
    bool sourcesReady = !impl.sources.empty();
    for (const auto& [id, s] : impl.sources) sourcesReady = sourcesReady && s->loaded() && s->isIdle();
    if (idle && sourcesReady) {
      if (idleSince == 0) idleSince = now;
      if (now - idleSince > a.settleMs) break;
    } else {
      idleSince = 0;
    }
    if (now - start > a.timeoutMs) {
      std::fprintf(stderr, "timed out waiting for the map to load\n");
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(8));
  }
  impl.renderTarget(target);
  const auto stats = map.stats();
  std::printf("frames=%d tiles=%d loading=%d blank=%.3f draws=%d frame=%.2fms elapsed=%.0fms\n", frames, stats.tiles, stats.loading,
              stats.blank, stats.drawCalls, stats.frameMs, mapkern::nowMs() - start);
#ifdef _WIN32
  auto* tex = static_cast<ID3D11Texture2D*>(const_cast<void*>(sg_d3d11_query_image_info(color).tex2d));
  D3D11_TEXTURE2D_DESC desc;
  tex->GetDesc(&desc);
  desc.Usage = D3D11_USAGE_STAGING;
  desc.BindFlags = 0;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  desc.MiscFlags = 0;
  ID3D11Texture2D* staging = nullptr;
  device->CreateTexture2D(&desc, nullptr, &staging);
  context->CopyResource(staging, tex);
  D3D11_MAPPED_SUBRESOURCE mapped;
  context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped);
  std::vector<uint8_t> pixels(static_cast<size_t>(pw) * ph * 4);
  for (int y = 0; y < ph; y++) {
    const uint8_t* row = static_cast<const uint8_t*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch;
    for (int x = 0; x < pw; x++) {
      const uint8_t* p = row + x * 4;
      uint8_t* o = &pixels[(static_cast<size_t>(y) * pw + x) * 4];
      const int alpha = p[3];
      for (int c = 0; c < 3; c++) o[c] = alpha > 0 ? static_cast<uint8_t>(std::min(255, p[c] * 255 / alpha)) : 0;
      o[3] = p[3];
    }
  }
  context->Unmap(staging, 0);
  staging->Release();
#endif
  if (!stbi_write_png(a.out.c_str(), pw, ph, 4, pixels.data(), pw * 4)) {
    std::fprintf(stderr, "could not write %s\n", a.out.c_str());
    return 1;
  }
  std::printf("wrote %s\n", a.out.c_str());
  return 0;
}
