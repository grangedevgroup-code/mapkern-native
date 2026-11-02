#define SOKOL_APP_IMPL
#define SOKOL_GLUE_IMPL
#define SOKOL_NO_ENTRY

#include "mapkern/window.hpp"

#include <cstdio>
#include <memory>

#include "map/map_impl.hpp"
#include "render/sokol.hpp"
#include "sokol/sokol_app.h"
#include "sokol/sokol_glue.h"

namespace mapkern {

namespace {

struct Window {
  MapOptions options;
  WindowOptions window;
  std::unique_ptr<Map> map;
  bool dragging = false;
  bool rotating = false;
  float lastX = 0, lastY = 0;
};

Window* current = nullptr;

void init() {
  Window& w = *current;
  w.map = std::make_unique<Map>(w.options);
  w.map->impl().initGraphicsNative(sglue_environment());
  w.map->resize(sapp_widthf() / sapp_dpi_scale(), sapp_heightf() / sapp_dpi_scale(), sapp_dpi_scale());
  w.map->on([](const MapEvent& e) {
    if (e.type == "error") std::fprintf(stderr, "error: %s\n", e.message.c_str());
    if (e.type == "styleload")
      for (const auto& s : e.skipped) std::fprintf(stderr, "skipped: %s\n", s.c_str());
  });
  if (w.window.ready) w.window.ready(*w.map);
}

void frame() {
  Window& w = *current;
  w.map->impl().renderSwapchain(sglue_swapchain());
  if (w.window.frame && !w.window.frame(*w.map)) sapp_request_quit();
}

void cleanup() { current->map.reset(); }

void event(const sapp_event* e) {
  Window& w = *current;
  Map& map = *w.map;
  const float scale = sapp_dpi_scale();
  const float x = e->mouse_x / scale, y = e->mouse_y / scale;
  switch (e->type) {
    case SAPP_EVENTTYPE_RESIZED: map.resize(e->window_width / scale, e->window_height / scale, scale); break;
    case SAPP_EVENTTYPE_MOUSE_DOWN:
      if (e->mouse_button == SAPP_MOUSEBUTTON_LEFT) {
        w.dragging = true;
        map.pointerDown(0, x, y);
      } else {
        w.rotating = true;
      }
      w.lastX = x;
      w.lastY = y;
      break;
    case SAPP_EVENTTYPE_MOUSE_UP:
      if (e->mouse_button == SAPP_MOUSEBUTTON_LEFT && w.dragging) {
        w.dragging = false;
        map.pointerUp(0, x, y);
      }
      w.rotating = false;
      break;
    case SAPP_EVENTTYPE_MOUSE_MOVE:
      if (w.dragging) map.pointerMove(0, x, y);
      if (w.rotating) {
        map.rotateBy((x - w.lastX) * 0.4);
        map.pitchBy(-(y - w.lastY) * 0.4);
      }
      w.lastX = x;
      w.lastY = y;
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
        else if (e->type == SAPP_EVENTTYPE_TOUCHES_ENDED) map.pointerUp(id, tx, ty);
        else map.pointerCancel(id);
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

}

int runWindow(const MapOptions& options, const WindowOptions& window) {
  Window w;
  w.options = options;
  w.window = window;
  current = &w;
  sapp_desc desc{};
  desc.init_cb = init;
  desc.frame_cb = frame;
  desc.cleanup_cb = cleanup;
  desc.event_cb = event;
  desc.width = w.window.width;
  desc.height = w.window.height;
  desc.sample_count = 4;
  desc.high_dpi = true;
  desc.window_title = w.window.title.c_str();
  desc.logger.func = nullptr;
  sapp_run(&desc);
  current = nullptr;
  return 0;
}

}
