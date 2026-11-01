#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "geo/mercator.hpp"
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
  double terrain = 0;
  bool hillshade = false;
  struct Action {
    double x0, y0, x1, y1;
    int steps;
  };
  std::vector<Action> actions;
  std::string trace;
  std::string framesDir;
};

const char* kDem = R"j({"type":"dem","tiles":["https://tiles.mapterhorn.com/{z}/{x}/{y}.webp"],"tileSize":512,"maxZoom":12,"encoding":"terrarium","attribution":"Mapterhorn"})j";

struct ArgReader {
  std::vector<std::string> items;
  size_t at = 0;
  bool more() const { return at < items.size(); }
  std::string next() { return at < items.size() ? items[at++] : std::string(); }
  double number() { return std::atof(next().c_str()); }
};

}

int main(int argc, char** argv) {
  Args a;
  a.options.fadeDuration = 0;
  ArgReader r{{argv + 1, argv + argc}};
  while (r.more()) {
    const std::string s = r.next();
    if (s == "--style") a.options.style = r.next();
    else if (s == "--center") {
      a.options.lng = r.number();
      a.options.lat = r.number();
    } else if (s == "--zoom") a.options.zoom = r.number();
    else if (s == "--pitch") a.options.pitch = r.number();
    else if (s == "--bearing") a.options.bearing = r.number();
    else if (s == "--size") {
      a.width = static_cast<int>(r.number());
      a.height = static_cast<int>(r.number());
    } else if (s == "--ratio") a.ratio = r.number();
    else if (s == "--out") a.out = r.next();
    else if (s == "--flat") a.options.globe = false;
    else if (s == "--offline") a.options.offline = true;
    else if (s == "--cache") a.options.cacheDirectory = r.next();
    else if (s == "--timeout") a.timeoutMs = r.number();
    else if (s == "--terrain") a.terrain = r.number();
    else if (s == "--hillshade") a.hillshade = true;
    else if (s == "--controls") a.options.controls = true;
    else if (s == "--tap") {
      const double x = r.number();
      const double y = r.number();
      a.actions.push_back({x, y, x, y, 0});
    } else if (s == "--drag") {
      Args::Action d{};
      d.x0 = r.number();
      d.y0 = r.number();
      d.x1 = r.number();
      d.y1 = r.number();
      d.steps = std::max(1, static_cast<int>(r.number() / 16));
      a.actions.push_back(d);
    } else if (s == "--trace") a.trace = r.next();
    else if (s == "--frames") a.framesDir = r.next();
    else if (s == "--mouse") a.options.touchControls = false;
    else if (s == "--view") a.options.view = r.next();
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
  auto capture = [&](const std::string& path) {
    std::vector<uint8_t> pixels(static_cast<size_t>(pw) * ph * 4);
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
    return stbi_write_png(path.c_str(), pw, ph, 4, pixels.data(), pw * 4) != 0;
  };
  FILE* trace = a.trace.empty() ? nullptr : std::fopen(a.trace.c_str(), "w");
  if (trace) std::fprintf(trace, "frame,ms,action,step,lng,lat,zoom,pitch,elevation,camera\n");
  const double start = mapkern::nowMs();
  double idleSince = 0;
  int frames = 0;
  int captured = 0;
  bool extrasAdded = false;
  int action = -1;
  int step = 0;
  while (true) {
    if (!extrasAdded && impl.styleLoaded) {
      extrasAdded = true;
      if (a.terrain > 0 || a.hillshade) map.addSource("mk-dem", kDem);
      if (a.hillshade) {
        std::string before;
        for (const auto& id : map.layerIds())
          if (map.impl().layer(id) && map.impl().layer(id)->type == mapkern::LayerType::Symbol) {
            before = id;
            break;
          }
        map.addLayer(R"j({"id":"mk-hillshade","type":"hillshade","source":"mk-dem"})j", before);
      }
      if (a.terrain > 0) map.setTerrain("mk-dem", a.terrain, 0.5);
    }
    if (action >= 0 && step > 0) {
      const auto& d = a.actions[static_cast<size_t>(action)];
      const double k = static_cast<double>(step) / d.steps;
      const double x = d.x0 + (d.x1 - d.x0) * k, y = d.y0 + (d.y1 - d.y0) * k;
      if (step < d.steps) {
        map.pointerMove(0, x, y);
        step++;
      } else {
        map.pointerMove(0, x, y);
        map.pointerUp(0, x, y);
        step = 0;
      }
    }
    impl.renderTarget(target);
    frames++;
    const double now = mapkern::nowMs();
    if (action >= 0) {
      auto& t = impl.transform;
      if (trace) {
        const auto c = t.centerWorld();
        std::fprintf(trace, "%d,%.1f,%d,%d,%.7f,%.7f,%.4f,%.2f,%.1f,%.1f\n", frames, now - start, action, step, c[0] * 360 - 180,
                     mapkern::latFromMercatorY(c[1]), t.zoom(), t.pitch(), t.elevation(), t.cameraPosition()[2] / t.pixelsPerMeter());
      }
      if (!a.framesDir.empty()) {
        char name[64];
        std::snprintf(name, sizeof name, "/f%05d.png", captured++);
        capture(a.framesDir + name);
      }
    }
    const bool idle = impl.styleLoaded && !impl.needsRender();
    bool sourcesReady = !impl.sources.empty();
    for (const auto& [id, s] : impl.sources) sourcesReady = sourcesReady && s->loaded() && s->isIdle();
    if (idle && sourcesReady && step == 0) {
      if (idleSince == 0) idleSince = now;
      if (now - idleSince > a.settleMs) {
        if (static_cast<size_t>(action + 1) >= a.actions.size()) break;
        action++;
        const auto& d = a.actions[static_cast<size_t>(action)];
        map.pointerDown(0, d.x0, d.y0);
        if (d.steps == 0) map.pointerUp(0, d.x0, d.y0);
        else step = 1;
        idleSince = 0;
      }
    } else {
      idleSince = 0;
    }
    if (now - start > a.timeoutMs) {
      std::fprintf(stderr, "timed out waiting for the map to load\n");
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(8));
  }
  if (trace) std::fclose(trace);
  impl.renderTarget(target);
  const auto stats = map.stats();
  std::printf("frames=%d tiles=%d loading=%d blank=%.3f draws=%d frame=%.2fms elapsed=%.0fms\n", frames, stats.tiles, stats.loading,
              stats.blank, stats.drawCalls, stats.frameMs, mapkern::nowMs() - start);
  if (!capture(a.out)) {
    std::fprintf(stderr, "could not write %s\n", a.out.c_str());
    return 1;
  }
  std::printf("wrote %s\n", a.out.c_str());
  return 0;
}
