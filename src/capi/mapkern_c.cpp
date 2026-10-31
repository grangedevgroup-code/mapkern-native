#include "mapkern/mapkern.h"

#include <cstring>
#include <memory>
#include <string>

#include "map/map_impl.hpp"
#include "mapkern/map.hpp"
#include "net/http.hpp"

struct mk_map {
  std::unique_ptr<mapkern::Map> map;
  mk_invalidate_fn invalidate = nullptr;
  void* invalidateUser = nullptr;
  mk_event_fn events = nullptr;
  void* eventsUser = nullptr;
};

namespace {

int toFormat(int f) {
  switch (f) {
    case MK_FORMAT_NONE: return SG_PIXELFORMAT_NONE;
    case MK_FORMAT_RGBA8: return SG_PIXELFORMAT_RGBA8;
    case MK_FORMAT_BGRA8: return SG_PIXELFORMAT_BGRA8;
    case MK_FORMAT_DEPTH: return SG_PIXELFORMAT_DEPTH;
    case MK_FORMAT_DEPTH_STENCIL: return SG_PIXELFORMAT_DEPTH_STENCIL;
    default: return _SG_PIXELFORMAT_DEFAULT;
  }
}

std::string str(const char* s) { return s ? s : ""; }

mapkern::MapOptions convert(const mk_options* o) {
  mapkern::MapOptions out;
  if (!o) return out;
  if (o->style) out.style = o->style;
  out.lng = o->lng;
  out.lat = o->lat;
  out.zoom = o->zoom;
  out.bearing = o->bearing;
  out.pitch = o->pitch;
  out.minZoom = o->min_zoom;
  out.maxZoom = o->max_zoom;
  out.maxPitch = o->max_pitch;
  out.globe = o->globe != 0;
  out.pixelRatio = o->pixel_ratio > 0 ? o->pixel_ratio : 1;
  out.cacheDirectory = str(o->cache_directory);
  out.diskCacheBytes = o->disk_cache_bytes;
  out.offline = o->offline != 0;
  out.controls = o->controls != 0;
  out.touchControls = o->touch_controls != 0;
  if (o->view) out.view = o->view;
  return out;
}

mapkern::CameraOptions camera(const mk_camera* c) {
  mapkern::CameraOptions o;
  if (!c) return o;
  o.lng = c->lng;
  o.lat = c->lat;
  o.zoom = c->zoom;
  o.bearing = c->bearing;
  o.pitch = c->pitch;
  return o;
}

}

extern "C" {

const char* mk_version(void) { return "0.1.0"; }

void mk_options_default(mk_options* o) {
  if (!o) return;
  const mapkern::MapOptions d;
  std::memset(o, 0, sizeof *o);
  o->style = "https://tiles.openfreemap.org/styles/liberty";
  o->min_zoom = d.minZoom;
  o->max_zoom = d.maxZoom;
  o->max_pitch = d.maxPitch;
  o->globe = d.globe ? 1 : 0;
  o->pixel_ratio = 1;
  o->disk_cache_bytes = d.diskCacheBytes;
  o->touch_controls = 1;
  o->view = "globe";
}

mk_map* mk_map_create(const mk_options* options) {
  auto* m = new mk_map();
  m->map = std::make_unique<mapkern::Map>(convert(options));
  m->map->setInvalidateCallback([m] {
    if (m->invalidate) m->invalidate(m->invalidateUser);
  });
  m->map->on([m](const mapkern::MapEvent& e) {
    if (m->events) m->events(m->eventsUser, e.type.c_str(), e.message.c_str(), e.sourceId.c_str());
  });
  return m;
}

void mk_map_destroy(mk_map* m) {
  if (!m) return;
  m->map->setInvalidateCallback(nullptr);
  delete m;
}

void mk_map_set_invalidate(mk_map* m, mk_invalidate_fn fn, void* user) {
  m->invalidate = fn;
  m->invalidateUser = user;
}

void mk_map_set_events(mk_map* m, mk_event_fn fn, void* user) {
  m->events = fn;
  m->eventsUser = user;
}

void mk_map_init_graphics(mk_map* m, const mk_graphics* g) {
  mapkern::GraphicsEnvironment e;
  if (g) {
    e.metalDevice = g->metal_device;
    e.d3d11Device = g->d3d11_device;
    e.d3d11DeviceContext = g->d3d11_device_context;
    e.colorFormat = toFormat(g->color_format);
    e.depthFormat = toFormat(g->depth_format);
    e.sampleCount = g->sample_count;
  }
  m->map->initGraphics(e);
}

void mk_map_shutdown_graphics(mk_map* m) { m->map->shutdownGraphics(); }

int mk_map_render(mk_map* m, const mk_surface* s) {
  if (!s) return 0;
  mapkern::RenderSurface r;
  r.width = s->width;
  r.height = s->height;
  r.sampleCount = s->sample_count;
  r.colorFormat = toFormat(s->color_format);
  r.depthFormat = toFormat(s->depth_format);
  r.metalCurrentDrawable = s->metal_current_drawable;
  r.metalDepthStencilTexture = s->metal_depth_stencil_texture;
  r.metalMsaaColorTexture = s->metal_msaa_color_texture;
  r.d3d11RenderView = s->d3d11_render_view;
  r.d3d11ResolveView = s->d3d11_resolve_view;
  r.d3d11DepthStencilView = s->d3d11_depth_stencil_view;
  r.glFramebuffer = s->gl_framebuffer;
  m->map->render(r);
  return m->map->needsRender() ? 1 : 0;
}

int mk_map_needs_render(const mk_map* m) { return m->map->needsRender() ? 1 : 0; }
void mk_map_resize(mk_map* m, double w, double h, double r) { m->map->resize(w, h, r); }

void mk_map_set_style(mk_map* m, const char* s) { m->map->setStyle(str(s)); }
int mk_map_style_loaded(const mk_map* m) { return m->map->isStyleLoaded() ? 1 : 0; }
void mk_map_add_source(mk_map* m, const char* id, const char* spec) { m->map->addSource(str(id), str(spec)); }
void mk_map_remove_source(mk_map* m, const char* id) { m->map->removeSource(str(id)); }
void mk_map_add_layer(mk_map* m, const char* spec, const char* before) { m->map->addLayer(str(spec), str(before)); }
void mk_map_remove_layer(mk_map* m, const char* id) { m->map->removeLayer(str(id)); }
void mk_map_set_paint_property(mk_map* m, const char* l, const char* n, const char* v) { m->map->setPaintProperty(str(l), str(n), str(v)); }
void mk_map_set_layout_property(mk_map* m, const char* l, const char* n, const char* v) { m->map->setLayoutProperty(str(l), str(n), str(v)); }
void mk_map_set_filter(mk_map* m, const char* l, const char* f) { m->map->setFilter(str(l), str(f)); }
void mk_map_set_layer_visible(mk_map* m, const char* l, int v) { m->map->setLayerVisible(str(l), v != 0); }

size_t mk_map_attribution(const mk_map* m, char* buffer, size_t size) {
  const std::string a = m->map->attribution();
  if (buffer && size > 0) {
    const size_t n = std::min(size - 1, a.size());
    std::memcpy(buffer, a.data(), n);
    buffer[n] = 0;
  }
  return a.size();
}

void mk_map_get_camera(const mk_map* m, mk_camera* c) {
  if (!c) return;
  c->lng = m->map->centerLng();
  c->lat = m->map->centerLat();
  c->zoom = m->map->zoom();
  c->bearing = m->map->bearing();
  c->pitch = m->map->pitch();
}

void mk_map_jump_to(mk_map* m, const mk_camera* c) { m->map->jumpTo(camera(c)); }

void mk_map_ease_to(mk_map* m, const mk_camera* c, double d) {
  mapkern::AnimationOptions a;
  a.duration = d;
  m->map->easeTo(camera(c), a);
}

void mk_map_fly_to(mk_map* m, const mk_camera* c, double d) {
  mapkern::AnimationOptions a;
  a.duration = d;
  m->map->flyTo(camera(c), a);
}

void mk_map_stop(mk_map* m) { m->map->stop(); }
void mk_map_set_projection(mk_map* m, int g) { m->map->setProjection(g != 0); }
int mk_map_globe(const mk_map* m) { return m->map->globe() ? 1 : 0; }
void mk_map_set_terrain(mk_map* m, const char* s, double e, double r) { m->map->setTerrain(str(s), e, r); }
void mk_map_clear_terrain(mk_map* m) { m->map->clearTerrain(); }

int mk_map_elevation_at(const mk_map* m, double lng, double lat, double* meters) {
  const auto e = m->map->elevationAt(lng, lat);
  if (!e) return 0;
  if (meters) *meters = *e;
  return 1;
}

int mk_map_unproject(const mk_map* m, double x, double y, double* lng, double* lat) {
  const auto p = m->map->unproject(x, y);
  if (!p) return 0;
  if (lng) *lng = p->first;
  if (lat) *lat = p->second;
  return 1;
}

void mk_map_project(const mk_map* m, double lng, double lat, double* x, double* y) {
  const auto p = m->map->project(lng, lat);
  if (x) *x = p.first;
  if (y) *y = p.second;
}

void mk_map_pointer_down(mk_map* m, int id, double x, double y) { m->map->pointerDown(id, x, y); }
void mk_map_pointer_move(mk_map* m, int id, double x, double y) { m->map->pointerMove(id, x, y); }
void mk_map_pointer_up(mk_map* m, int id, double x, double y) { m->map->pointerUp(id, x, y); }
void mk_map_pointer_cancel(mk_map* m, int id) { m->map->pointerCancel(id); }
void mk_map_wheel(mk_map* m, double d, double x, double y) { m->map->wheel(d, x, y); }
void mk_map_double_tap(mk_map* m, double x, double y) { m->map->doubleTap(x, y); }
void mk_map_zoom_by(mk_map* m, double d, double x, double y, int a) { m->map->zoomBy(d, x, y, a != 0); }
void mk_map_rotate_by(mk_map* m, double d) { m->map->rotateBy(d); }
void mk_map_pitch_by(mk_map* m, double d) { m->map->pitchBy(d); }

void mk_map_set_offline(mk_map* m, int o) { m->map->setOffline(o != 0); }
int mk_map_offline(const mk_map* m) { return m->map->offline() ? 1 : 0; }
void mk_map_clear_disk_cache(mk_map* m) { m->map->clearDiskCache(); }

void mk_map_download_region(mk_map* m, double w, double s, double e, double n, int z0, int z1, mk_progress_fn fn, void* user) {
  m->map->downloadRegion(w, s, e, n, z0, z1, [fn, user](size_t done, size_t total) {
    if (fn) fn(user, done, total);
  });
}

void mk_set_network_reachable(int r) { mapkern::setNetworkReachable(r != 0); }

}
