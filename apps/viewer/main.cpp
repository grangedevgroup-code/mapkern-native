#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

#include "map/map_impl.hpp"
#include "mapkern/map.hpp"
#include "render/sokol.hpp"
#include "sokol/sokol_app.h"
#include "sokol/sokol_glue.h"

namespace {

struct Viewer {
  std::unique_ptr<mapkern::Map> map;
  mapkern::MapOptions options;
  bool dragging = false;
  bool rotating = false;
  float lastX = 0, lastY = 0;
  std::string shot;
  int frames = 0;
  int shotAfter = 0;
  double started = 0;
  int exitAfter = 0;
};

Viewer viewer;

void init() {
  viewer.map = std::make_unique<mapkern::Map>(viewer.options);
  viewer.map->impl().initGraphicsNative(sglue_environment());
  viewer.map->resize(sapp_widthf() / sapp_dpi_scale(), sapp_heightf() / sapp_dpi_scale(), sapp_dpi_scale());
  viewer.map->on([](const mapkern::MapEvent& e) {
    if (e.type == "error") std::fprintf(stderr, "error: %s\n", e.message.c_str());
    if (e.type == "styleload")
      for (const auto& s : e.skipped) std::fprintf(stderr, "skipped: %s\n", s.c_str());
  });
  viewer.started = mapkern::nowMs();
}

void frame() {
  viewer.map->impl().renderSwapchain(sglue_swapchain());
  viewer.frames++;
  if (viewer.exitAfter > 0 && mapkern::nowMs() - viewer.started > viewer.exitAfter) {
    const auto s = viewer.map->stats();
    std::printf("frames=%d tiles=%d loading=%d blank=%.3f draws=%d frame=%.2fms cache=%llu\n", viewer.frames, s.tiles, s.loading, s.blank,
                s.drawCalls, s.frameMs, static_cast<unsigned long long>(s.diskCacheEntries));
    sapp_request_quit();
  }
}

void cleanup() { viewer.map.reset(); }

void event(const sapp_event* e) {
  auto& map = *viewer.map;
  const float scale = sapp_dpi_scale();
  const float x = e->mouse_x / scale, y = e->mouse_y / scale;
  switch (e->type) {
    case SAPP_EVENTTYPE_RESIZED: map.resize(e->window_width / scale, e->window_height / scale, scale); break;
    case SAPP_EVENTTYPE_MOUSE_DOWN:
      if (e->mouse_button == SAPP_MOUSEBUTTON_LEFT) {
        viewer.dragging = true;
        map.pointerDown(0, x, y);
      } else {
        viewer.rotating = true;
      }
      viewer.lastX = x;
      viewer.lastY = y;
      break;
    case SAPP_EVENTTYPE_MOUSE_UP:
      if (e->mouse_button == SAPP_MOUSEBUTTON_LEFT && viewer.dragging) {
        viewer.dragging = false;
        map.pointerUp(0, x, y);
      }
      viewer.rotating = false;
      break;
    case SAPP_EVENTTYPE_MOUSE_MOVE:
      if (viewer.dragging) map.pointerMove(0, x, y);
      if (viewer.rotating) {
        map.rotateBy((x - viewer.lastX) * 0.4);
        map.pitchBy(-(y - viewer.lastY) * 0.4);
      }
      viewer.lastX = x;
      viewer.lastY = y;
      break;
    case SAPP_EVENTTYPE_MOUSE_SCROLL: map.wheel(e->scroll_y / 4.0, x, y); break;
    case SAPP_EVENTTYPE_TOUCHES_BEGAN:
    case SAPP_EVENTTYPE_TOUCHES_MOVED:
    case SAPP_EVENTTYPE_TOUCHES_ENDED:
    case SAPP_EVENTTYPE_TOUCHES_CANCELLED:
      for (int i = 0; i < e->num_touches; i++) {
        const auto& t = e->touches[i];
        if (!t.changed) continue;
        const double tx = t.pos_x / scale, ty = t.pos_y / scale;
        const int id = static_cast<int>(t.identifier);
        if (e->type == SAPP_EVENTTYPE_TOUCHES_BEGAN) map.pointerDown(id, tx, ty);
        else if (e->type == SAPP_EVENTTYPE_TOUCHES_MOVED) map.pointerMove(id, tx, ty);
        else map.pointerUp(id, tx, ty);
      }
      break;
    case SAPP_EVENTTYPE_KEY_DOWN:
      if (e->key_code == SAPP_KEYCODE_G) map.setProjection(!map.globe());
      if (e->key_code == SAPP_KEYCODE_O) map.setOffline(!map.offline());
      if (e->key_code == SAPP_KEYCODE_EQUAL || e->key_code == SAPP_KEYCODE_KP_ADD) map.zoomBy(1, sapp_widthf() / scale / 2, sapp_heightf() / scale / 2, true);
      if (e->key_code == SAPP_KEYCODE_MINUS || e->key_code == SAPP_KEYCODE_KP_SUBTRACT)
        map.zoomBy(-1, sapp_widthf() / scale / 2, sapp_heightf() / scale / 2, true);
      if (e->key_code == SAPP_KEYCODE_ESCAPE) sapp_request_quit();
      break;
    default: break;
  }
}

double arg(int argc, char** argv, int& i) { return i + 1 < argc ? std::atof(argv[++i]) : 0; }

}

sapp_desc sokol_main(int argc, char* argv[]) {
  for (int i = 1; i < argc; i++) {
    const std::string a = argv[i];
    if (a == "--style" && i + 1 < argc) viewer.options.style = argv[++i];
    else if (a == "--center" && i + 2 < argc) {
      viewer.options.lng = arg(argc, argv, i);
      viewer.options.lat = arg(argc, argv, i);
    } else if (a == "--zoom") viewer.options.zoom = arg(argc, argv, i);
    else if (a == "--pitch") viewer.options.pitch = arg(argc, argv, i);
    else if (a == "--bearing") viewer.options.bearing = arg(argc, argv, i);
    else if (a == "--flat") viewer.options.globe = false;
    else if (a == "--offline") viewer.options.offline = true;
    else if (a == "--cache" && i + 1 < argc) viewer.options.cacheDirectory = argv[++i];
    else if (a == "--exit-after") viewer.exitAfter = static_cast<int>(arg(argc, argv, i));
  }
  sapp_desc desc{};
  desc.init_cb = init;
  desc.frame_cb = frame;
  desc.cleanup_cb = cleanup;
  desc.event_cb = event;
  desc.width = 1280;
  desc.height = 800;
  desc.sample_count = 4;
  desc.high_dpi = true;
  desc.window_title = "Mapkern";
  desc.logger.func = nullptr;
  return desc;
}
