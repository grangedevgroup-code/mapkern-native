#include <algorithm>
#include <cmath>
#include <set>

#include "core/url.hpp"
#include "geo/mercator.hpp"
#include "map/map_impl.hpp"
#include "map/raster_source.hpp"
#include "map/tileset.hpp"
#include "map/vector_source.hpp"
#include "net/http.hpp"
#include "render/gpu.hpp"
#include "style/color.hpp"
#include "style/import.hpp"
#include "style/validate.hpp"
#include "formats/image.hpp"
#include "worker/symbols.hpp"
#include <cstring>

namespace mapkern {

namespace {

sg_swapchain toSwapchain(const RenderSurface& s) {
  sg_swapchain sc{};
  sc.width = s.width;
  sc.height = s.height;
  sc.sample_count = s.sampleCount > 0 ? s.sampleCount : 1;
  sc.color_format = static_cast<sg_pixel_format>(s.colorFormat);
  sc.depth_format = static_cast<sg_pixel_format>(s.depthFormat);
  sc.metal.current_drawable = s.metalCurrentDrawable;
  sc.metal.depth_stencil_texture = s.metalDepthStencilTexture;
  sc.metal.msaa_color_texture = s.metalMsaaColorTexture;
  sc.d3d11.render_view = s.d3d11RenderView;
  sc.d3d11.resolve_view = s.d3d11ResolveView;
  sc.d3d11.depth_stencil_view = s.d3d11DepthStencilView;
  sc.gl.framebuffer = s.glFramebuffer;
  return sc;
}

}

MapImpl::MapImpl(const MapOptions& o) : options(o) {
  registerArchiveProtocols();
  installSymbolBuilder();
  pixelRatio = o.pixelRatio;
  transform.setPitchRange(0, o.maxPitch);
  transform.setZoomRange(o.minZoom, o.maxZoom);
  transform.resize(800, 600);
  transform.setProjection(o.globe ? Projection::Globe : Projection::Mercator);
  transform.setZoom(o.zoom);
  transform.setCenter(o.lng, o.lat);
  transform.setBearing(o.bearing);
  transform.setPitch(o.pitch);
  if (o.offline) setOffline(true);
  const std::string dir = o.cacheDirectory.empty() ? defaultCacheDirectory() : o.cacheDirectory;
  if (o.diskCacheBytes > 0) diskCache = sharedDiskCache(dir, o.diskCacheBytes);
  sourceCtx.scheduler = &scheduler;
  sourceCtx.mailbox = &mailbox;
  sourceCtx.requestTimeoutMs = o.requestTimeoutMs;
  sourceCtx.diskCache = diskCache;
  sourceCtx.cacheBytes = o.tileCacheBytes;
  sourceCtx.fadeDuration = o.fadeDuration;
  sourceCtx.pixelRatio = [this] { return pixelRatio; };
  sourceCtx.error = [this](const MapError& e) { error(e.code, e.message, e.sourceId); };
  sourceCtx.content = [this](const std::string& id) {
    MapEvent e;
    e.type = "sourcedata";
    e.sourceId = id;
    emit(e);
  };
  sourceCtx.changed = [this] { invalidate(); };
  sourceCtx.wakeAt = [this](double t) {
    if (wakeAt == 0 || t < wakeAt) wakeAt = t;
  };
  mailbox.setWake([this] {
    if (invalidateCallback) invalidateCallback();
  });
  if (!o.style.empty()) setStyle(o.style);
}

MapImpl::~MapImpl() {
  styleRequest.cancel();
  mailbox.setWake(nullptr);
  sources.clear();
  mailbox.drain();
}

void MapImpl::invalidate() {
  dirty = true;
  if (invalidateCallback) invalidateCallback();
}

void MapImpl::emit(const MapEvent& e) {
  for (auto& l : listeners) l(e);
}

void MapImpl::error(const std::string& code, const std::string& message, const std::string& sourceId) {
  MapEvent e;
  e.type = "error";
  e.message = message;
  e.sourceId = sourceId;
  e.skipped = {code};
  emit(e);
}

void MapImpl::initGraphicsNative(const sg_environment& e) {
  Gpu::get().setup(e);
  graphics = true;
  invalidate();
}

void MapImpl::initGraphics(const GraphicsEnvironment& env) {
  sg_environment e{};
  e.defaults.color_format = static_cast<sg_pixel_format>(env.colorFormat);
  e.defaults.depth_format = static_cast<sg_pixel_format>(env.depthFormat);
  e.defaults.sample_count = env.sampleCount > 0 ? env.sampleCount : 1;
  e.metal.device = env.metalDevice;
  e.d3d11.device = env.d3d11Device;
  e.d3d11.device_context = env.d3d11DeviceContext;
  Gpu::get().setup(e);
  graphics = true;
  invalidate();
}

void MapImpl::shutdownGraphics() {
  if (!graphics) return;
  for (auto& [id, s] : sources) s->reload();
  graphics = false;
}

void MapImpl::resize(double width, double height, double ratio) {
  transform.resize(width, height);
  pixelRatio = ratio;
  invalidate();
}

void MapImpl::setStyle(const std::string& urlOrJson) {
  styleRequest.cancel();
  styleRequest = CancelToken();
  std::string trimmed = urlOrJson;
  trimmed.erase(0, trimmed.find_first_not_of(" \t\r\n"));
  if (trimmed.starts_with("{")) {
    std::string err;
    auto v = parseJson(trimmed, &err);
    if (!v) {
      error("invalid-style", "Style JSON could not be read: " + err);
      return;
    }
    if (isStyleV8(*v)) {
      auto imported = importStyle(*v);
      applyStyle(imported.style, imported.skipped);
    } else {
      applyStyle(*v, {});
    }
    return;
  }
  const std::string url = urlOrJson;
  const RequestParameters params = resolveRequest(url, ResourceKind::Style, sourceCtx.transformRequest);
  auto token = styleRequest;
  fetchBytes(params, token, options.requestTimeoutMs, ResourceKind::Style, diskCache, [this, url, token](FetchOutcome out) {
    mailbox.post([this, url, token, out = std::move(out)] {
      if (token.cancelled()) return;
      if (auto* f = std::get_if<TileFailure>(&out)) {
        error("style-error", "Style could not be loaded from " + url + ": " + f->message);
        return;
      }
      const auto& r = std::get<FetchResult>(out);
      if (r.empty || !r.data) {
        error("style-error", "Style was not found at " + url);
        return;
      }
      std::string err;
      auto v = parseJson(std::string_view(reinterpret_cast<const char*>(r.data->data()), r.data->size()), &err);
      if (!v) {
        error("invalid-style", "Style at " + url + " is not valid JSON: " + err);
        return;
      }
      if (isStyleV8(*v)) {
        auto imported = importStyle(*v, hasScheme(url) ? url : "");
        applyStyle(imported.style, imported.skipped);
      } else {
        applyStyle(*v, {});
      }
    });
  });
}

void MapImpl::applyStyle(const Value& style, const std::vector<std::string>& skipped) {
  sources.clear();
  layers.clear();
  styleRoot = style;
  std::vector<std::string> problems = skipped;
  if (style["sources"].isObject())
    for (const auto& [id, spec] : style["sources"].asObject()) {
      const auto issues = validateSource(id, spec);
      if (!issues.empty()) {
        problems.push_back(formatIssues(issues));
        continue;
      }
      addSource(id, spec);
    }
  const Value sourcesValue = style["sources"];
  if (style["layers"].isArray()) {
    for (const auto& l : style["layers"].asArray()) {
      const auto issues = validateLayer(l, "layer " + l["id"].stringOr(""), sourcesValue);
      if (!issues.empty()) {
        problems.push_back(formatIssues(issues));
        continue;
      }
      try {
        layers.push_back(std::make_unique<StyleLayer>(l));
      } catch (const std::exception& e) {
        problems.push_back(e.what());
      }
    }
  }
  styleLoaded = true;
  painter.labels().clear();
  painter.labels().setGlyphs(serverGlyphs(style["glyphs"]), diskCache, &mailbox, [this] { invalidate(); });
  loadSprite(style["sprite"]);
  layersChanged();
  for (auto& [id, s] : sources)
    if (!s->loaded()) s->load();
  MapEvent e;
  e.type = "styleload";
  e.skipped = problems;
  emit(e);
  invalidate();
}

Value MapImpl::styleJson() const {
  Object root = styleRoot.isObject() ? styleRoot.asObject() : Object{{"version", 1}};
  Object srcs;
  for (const auto& [id, s] : sources) srcs.emplace_back(id, s->spec);
  Array ls;
  for (const auto& l : layers) ls.push_back(l->serialize());
  root = withKey(std::move(root), "sources", Value(srcs));
  root = withKey(std::move(root), "layers", Value(ls));
  return Value(root);
}

void MapImpl::loadSprite(const Value& sprite) {
  painter.labels().images.clear();
  std::vector<std::pair<std::string, std::string>> sheets;
  if (sprite.isString()) sheets.emplace_back("", sprite.asString());
  else if (sprite.isArray())
    for (const auto& s : sprite.asArray())
      if (s["url"].isString()) sheets.emplace_back(s["id"].stringOr("default") == "default" ? "" : s["id"].asString() + ":", s["url"].asString());
  const std::string scale = pixelRatio > 1 ? "@2x" : "";
  auto token = styleRequest;
  for (const auto& [prefix, base] : sheets) {
    std::string root = base, query;
    if (const size_t q = root.find('?'); q != std::string::npos) {
      query = root.substr(q);
      root = root.substr(0, q);
    }
    auto json = std::make_shared<std::optional<FetchOutcome>>();
    auto png = std::make_shared<std::optional<FetchOutcome>>();
    auto finish = [this, json, png, prefix, token] {
      if (token.cancelled() || !json->has_value() || !png->has_value()) return;
      auto* j = std::get_if<FetchResult>(&**json);
      auto* p = std::get_if<FetchResult>(&**png);
      if (!j || !p || !j->data || !p->data) {
        error("sprite-error", "Sprite could not be loaded");
        return;
      }
      auto index = parseJson(std::string_view(reinterpret_cast<const char*>(j->data->data()), j->data->size()));
      auto image = decodeImage(*p->data, true);
      if (!index || !index->isObject() || !image) {
        error("sprite-error", "Sprite could not be read");
        return;
      }
      for (const auto& [name, entry] : index->asObject()) {
        const int x = static_cast<int>(entry["x"].numberOr(0)), y = static_cast<int>(entry["y"].numberOr(0));
        const int w = static_cast<int>(entry["width"].numberOr(0)), h = static_cast<int>(entry["height"].numberOr(0));
        if (w <= 0 || h <= 0 || x < 0 || y < 0 || x + w > image->width || y + h > image->height) continue;
        StyleImage img;
        img.width = w;
        img.height = h;
        img.pixelRatio = entry["pixelRatio"].numberOr(1);
        img.sdf = entry["sdf"].boolOr(false);
        img.rgba.resize(static_cast<size_t>(w) * h * 4);
        for (int row = 0; row < h; row++)
          std::memcpy(&img.rgba[static_cast<size_t>(row) * w * 4], &image->rgba[(static_cast<size_t>(y + row) * image->width + x) * 4], static_cast<size_t>(w) * 4);
        painter.labels().images.add(prefix + name, std::move(img));
      }
      invalidate();
    };
    for (int k = 0; k < 2; k++) {
      const std::string url = root + scale + (k == 0 ? ".json" : ".png") + query;
      auto slot = k == 0 ? json : png;
      fetchBytes(resolveRequest(url, ResourceKind::Sprite, sourceCtx.transformRequest), token, options.requestTimeoutMs, ResourceKind::Sprite, diskCache,
                 [this, slot, finish](FetchOutcome out) {
                   mailbox.post([slot, finish, out = std::move(out)]() mutable {
                     *slot = std::move(out);
                     finish();
                   });
                 });
    }
  }
}

void MapImpl::addSource(const std::string& id, const Value& spec) {
  if (sources.contains(id)) {
    error("invalid-source", "There is already a source with id \"" + id + "\"");
    return;
  }
  const std::string type = spec["type"].stringOr("");
  std::unique_ptr<Source> s;
  if (type == "vector") s = std::make_unique<VectorSource>(id, spec, sourceCtx);
  else if (type == "raster") s = std::make_unique<RasterSource>(id, spec, sourceCtx);
  else if (type == "dem") s = std::make_unique<RasterSource>(id, spec, sourceCtx, "dem");
  else {
    error("unsupported-source", "Source \"" + id + "\" has type " + type + ", which is not drawn yet", id);
    return;
  }
  Source* raw = s.get();
  sources[id] = std::move(s);
  if (styleLoaded) {
    layersChanged();
    raw->load();
  }
}

void MapImpl::removeSource(const std::string& id) {
  sources.erase(id);
  invalidate();
}

void MapImpl::addLayer(const Value& spec, const std::string& beforeId) {
  try {
    auto l = std::make_unique<StyleLayer>(spec);
    if (layer(l->id)) {
      error("invalid-layer", "There is already a layer with id \"" + l->id + "\"");
      return;
    }
    auto it = std::find_if(layers.begin(), layers.end(), [&](const auto& x) { return x->id == beforeId; });
    layers.insert(it, std::move(l));
    layersChanged();
  } catch (const std::exception& e) {
    error("invalid-layer", e.what());
  }
}

void MapImpl::removeLayer(const std::string& id) {
  std::erase_if(layers, [&](const auto& l) { return l->id == id; });
  layersChanged();
}

StyleLayer* MapImpl::layer(const std::string& id) {
  for (auto& l : layers)
    if (l->id == id) return l.get();
  return nullptr;
}

void MapImpl::layersChanged() {
  for (auto& [id, s] : sources) {
    std::vector<const StyleLayer*> mine;
    for (const auto& l : layers)
      if (l->source == id) mine.push_back(l.get());
    s->setLayers(mine);
  }
  invalidate();
}

CameraState MapImpl::targetState(const CameraOptions& c) {
  CameraState s = readState(transform);
  if (c.lng && c.lat) {
    s.x = mercatorX(*c.lng);
    s.y = mercatorY(clampLatitude(*c.lat));
  }
  if (c.zoom) s.zoom = std::clamp(*c.zoom, transform.minZoom, transform.maxZoom);
  if (c.bearing) s.bearing = *c.bearing;
  if (c.pitch) s.pitch = std::clamp(*c.pitch, transform.minPitch, transform.maxPitch);
  return s;
}

void MapImpl::jumpTo(const CameraOptions& c) {
  stop();
  const CameraState to = targetState(c);
  transform.setZoom(to.zoom);
  transform.setBearing(to.bearing);
  transform.setPitch(to.pitch);
  transform.setCenterWorld(to.x, to.y);
  invalidate();
}

void MapImpl::easeTo(const CameraOptions& c, const AnimationOptions& a) {
  stop();
  const CameraState from = readState(transform);
  const CameraState to = targetState(c);
  std::optional<Anchor> anchor;
  if (c.aroundX && c.aroundY && !(c.lng && c.lat)) {
    if (auto w = transform.unproject(*c.aroundX, *c.aroundY)) anchor = Anchor{*w, *c.aroundX, *c.aroundY, std::nullopt};
  }
  motion = easeMotion(from, to, a.duration < 0 ? 500 : a.duration, easeOutCubic, anchor);
  motionStart = nowMs();
  invalidate();
}

void MapImpl::flyTo(const CameraOptions& c, const AnimationOptions& a) {
  stop();
  const CameraState from = readState(transform);
  const CameraState to = targetState(c);
  FlyOptions fly;
  fly.curve = a.curve;
  if (a.duration >= 0) fly.duration = a.duration;
  motion = flyMotion(from, to, std::max(transform.width, transform.height), fly);
  motionStart = nowMs();
  invalidate();
}

void MapImpl::stop() {
  motion.reset();
  inertia.reset();
}

void MapImpl::pointerDown(int id, double x, double y) {
  stop();
  pointers[id] = {x, y};
  gesture.samples.clear();
  if (pointers.size() == 1) {
    if (auto w = transform.unproject(x, y)) gesture.anchor = *w;
    else gesture.anchor = transform.unprojectClamped(x, y);
    gesture.x = x;
    gesture.y = y;
  } else if (pointers.size() == 2) {
    auto a = pointers.begin()->second, b = std::next(pointers.begin())->second;
    gesture.x = (a.x + b.x) / 2;
    gesture.y = (a.y + b.y) / 2;
    gesture.distance = std::hypot(b.x - a.x, b.y - a.y);
    gesture.angle = std::atan2(b.y - a.y, b.x - a.x);
    gesture.zoom = transform.zoom();
    gesture.bearing = transform.bearing();
    gesture.anchor = transform.unprojectClamped(gesture.x, gesture.y);
  }
}

void MapImpl::pointerMove(int id, double x, double y) {
  auto it = pointers.find(id);
  if (it == pointers.end()) return;
  it->second = {x, y};
  if (pointers.size() == 1) {
    transform.setLocationAtPoint(gesture.anchor, x, y);
    gesture.samples.emplace_back(nowMs(), std::array<double, 2>{x, y});
    while (gesture.samples.size() > 8) gesture.samples.pop_front();
  } else if (pointers.size() >= 2) {
    auto a = pointers.begin()->second, b = std::next(pointers.begin())->second;
    const double cx = (a.x + b.x) / 2, cy = (a.y + b.y) / 2;
    const double distance = std::hypot(b.x - a.x, b.y - a.y);
    const double angle = std::atan2(b.y - a.y, b.x - a.x);
    if (gesture.distance > 0) transform.setZoom(gesture.zoom + std::log2(distance / gesture.distance));
    transform.setBearing(gesture.bearing - (angle - gesture.angle) * 180 / kPi);
    transform.setLocationAtPoint(gesture.anchor, cx, cy);
  }
  invalidate();
}

void MapImpl::pointerUp(int id, double x, double y) {
  pointers.erase(id);
  if (pointers.empty() && gesture.samples.size() >= 2) {
    const auto& first = gesture.samples.front();
    const auto& last = gesture.samples.back();
    const double dt = last.first - first.first;
    if (dt > 0 && nowMs() - last.first < 60) {
      const double vx = (last.second[0] - first.second[0]) / dt;
      const double vy = (last.second[1] - first.second[1]) / dt;
      if (std::hypot(vx, vy) > 0.15) {
        inertia = std::array<double, 2>{vx, vy};
        inertiaTime = nowMs();
      }
    }
  }
  if (pointers.size() == 1) {
    const auto p = pointers.begin()->second;
    gesture.anchor = transform.unprojectClamped(p.x, p.y);
    gesture.samples.clear();
  }
  invalidate();
}

void MapImpl::wheel(double delta, double x, double y) {
  stop();
  transform.zoomAround(transform.zoom() + delta, x, y);
  invalidate();
}

std::string MapImpl::attribution() const {
  std::vector<std::string> parts;
  for (const auto& [id, s] : sources) {
    bool used = false;
    for (const auto& l : layers) used = used || (l->source == id && l->visible);
    const std::string a = s->attribution();
    if (used && !a.empty() && std::find(parts.begin(), parts.end(), a) == parts.end()) parts.push_back(a);
  }
  std::string out;
  for (size_t i = 0; i < parts.size(); i++) out += (i ? " | " : "") + parts[i];
  return out;
}

MapStats MapImpl::stats() const {
  MapStats s;
  s.drawCalls = painter.stats().drawCalls;
  s.pieces = painter.stats().pieces;
  s.frameMs = lastFrameMs;
  double blank = 0;
  for (const auto& [id, src] : sources)
    if (auto st = src->stats()) {
      s.tiles += static_cast<int>(st->tiles);
      s.loading += static_cast<int>(st->loading);
      blank = std::max(blank, st->blankPending + st->blankFailed);
    }
  s.blank = blank;
  if (diskCache) {
    const auto c = diskCache->stats();
    s.diskCacheBytes = c.bytes;
    s.diskCacheEntries = c.entries;
  }
  return s;
}

void MapImpl::tick(double now) {
  mailbox.drain(4096);
  if (motion) {
    const double t = motion->duration > 0 ? (now - motionStart) / motion->duration : 1;
    motion->apply(transform, t);
    if (t >= 1) motion.reset();
    dirty = true;
  }
  if (inertia) {
    const double dt = std::clamp(now - inertiaTime, 0.0, 50.0);
    inertiaTime = now;
    const double cx = transform.width / 2, cy = transform.height / 2;
    const auto w = transform.unprojectClamped(cx - (*inertia)[0] * dt, cy - (*inertia)[1] * dt);
    transform.setLocationAtPoint(w, cx, cy);
    const double k = std::exp(-dt / 325.0);
    (*inertia)[0] *= k;
    (*inertia)[1] *= k;
    if (std::hypot((*inertia)[0], (*inertia)[1]) < 0.01) inertia.reset();
    dirty = true;
  }
  for (auto& l : layers)
    if (l->settle(now)) dirty = true;
  for (auto& [id, s] : sources) s->update(transform, now, {});
  const double deadline = nowMs() + 6;
  for (auto& [id, s] : sources)
    if (s->uploadPending(deadline, now)) dirty = true;
  scheduler.pump();
  if (wakeAt > 0 && now >= wakeAt) {
    wakeAt = 0;
    dirty = true;
  }
}

bool MapImpl::needsRender() const {
  if (dirty || motion || inertia || !pointers.empty()) return true;
  for (const auto& [id, s] : sources)
    if (s->isFading() || !s->isIdle()) return true;
  for (const auto& l : layers)
    if (l->animating()) return true;
  return wakeAt > 0 && nowMs() >= wakeAt;
}

bool MapImpl::render(const RenderSurface& surface) { return renderSwapchain(toSwapchain(surface)); }

bool MapImpl::renderSwapchain(const sg_swapchain& swapchain) {
  FrameTarget target;
  target.swapchain = swapchain;
  target.width = swapchain.width;
  target.height = swapchain.height;
  return renderTarget(target);
}

bool MapImpl::renderTarget(const FrameTarget& target) {
  const double start = nowMs();
  tick(start);
  if (!graphics) return false;
  RenderView view;
  view.zoom = transform.zoom();
  view.bearing = transform.bearing();
  view.width = transform.width;
  view.height = transform.height;
  view.tileMatrix = [this](int z, int x, int y, int wrap, double extent) { return transform.tileMatrix(z, x, y, wrap, extent); };
  FrameInput frame;
  frame.view = &view;
  frame.transform = &transform;
  frame.layers = &layers;
  frame.sources = &sources;
  frame.states = &states;
  frame.pixelRatio = pixelRatio;
  frame.now = start;
  frame.clearColor = {0, 0, 0, 0};
  painter.render(frame, target);
  dirty = painter.drapesBehind() || painter.labelsAnimating();
  lastFrameMs = nowMs() - start;
  return true;
}

void MapImpl::downloadRegion(double west, double south, double east, double north, int minZoom, int maxZoom,
                             std::function<void(size_t, size_t)> progress) {
  struct Job {
    std::vector<std::string> urls;
    std::atomic<size_t> done{0};
  };
  auto job = std::make_shared<Job>();
  for (const auto& [id, s] : sources) {
    const TileSet* set = nullptr;
    if (auto* v = dynamic_cast<VectorSource*>(s.get()); v && v->tileSet) set = &*v->tileSet;
    if (auto* r = dynamic_cast<RasterSource*>(s.get()); r && r->tileSet) set = &*r->tileSet;
    if (!set) continue;
    const int lo = std::max(minZoom, set->minZoom), hi = std::min(maxZoom, set->maxZoom);
    for (int z = lo; z <= hi; z++) {
      const double n = std::exp2(z);
      const int x0 = static_cast<int>(std::floor(mercatorX(west) * n)), x1 = static_cast<int>(std::floor(mercatorX(east) * n));
      const int y0 = static_cast<int>(std::floor(mercatorY(north) * n)), y1 = static_cast<int>(std::floor(mercatorY(south) * n));
      for (int y = std::max(0, y0); y <= std::min(static_cast<int>(n) - 1, y1); y++)
        for (int x = std::max(0, x0); x <= std::min(static_cast<int>(n) - 1, x1); x++) job->urls.push_back(tileUrl(*set, z, x, y));
    }
  }
  const size_t total = job->urls.size();
  if (total == 0) {
    if (progress) progress(0, 0);
    return;
  }
  auto cache = diskCache;
  Mailbox* box = &mailbox;
  for (const auto& url : job->urls) {
    fetchBytes({url, {}}, CancelToken(), options.requestTimeoutMs, ResourceKind::Tile, cache, [job, url, cache, total, progress, box](FetchOutcome out) {
      if (auto* r = std::get_if<FetchResult>(&out); r && r->data && cache) {
        if (auto* dc = dynamic_cast<DiskCache*>(cache.get())) {
          if (!dc->contains(url)) dc->write(url, r->data, r->contentType, 0, true);
          else dc->pin(url, true);
        }
      }
      const size_t d = ++job->done;
      if (progress) box->post([progress, d, total] { progress(d, total); });
    });
  }
}

Map::Map(const MapOptions& options) : impl_(std::make_unique<MapImpl>(options)) {}
Map::~Map() = default;
void Map::initGraphics(const GraphicsEnvironment& e) { impl_->initGraphics(e); }
void Map::shutdownGraphics() { impl_->shutdownGraphics(); }
bool Map::render(const RenderSurface& s) { return impl_->render(s); }
bool Map::needsRender() const { return impl_->needsRender(); }
void Map::setInvalidateCallback(std::function<void()> cb) { impl_->invalidateCallback = std::move(cb); }
void Map::resize(double w, double h, double r) { impl_->resize(w, h, r); }
void Map::setStyle(const std::string& s) { impl_->setStyle(s); }
std::string Map::styleJson() const { return impl_->styleJson().toJson(); }
bool Map::isStyleLoaded() const { return impl_->styleLoaded; }
bool Map::isIdle() const { return !impl_->needsRender(); }
void Map::jumpTo(const CameraOptions& c) { impl_->jumpTo(c); }
void Map::easeTo(const CameraOptions& c, const AnimationOptions& a) { impl_->easeTo(c, a); }
void Map::flyTo(const CameraOptions& c, const AnimationOptions& a) { impl_->flyTo(c, a); }
void Map::stop() { impl_->stop(); }
bool Map::isMoving() const { return impl_->motion.has_value() || impl_->inertia.has_value() || !impl_->pointers.empty(); }
double Map::zoom() const { return impl_->transform.zoom(); }
double Map::bearing() const { return impl_->transform.bearing(); }
double Map::pitch() const { return impl_->transform.pitch(); }
double Map::centerLng() const { return impl_->transform.center().lng; }
double Map::centerLat() const { return impl_->transform.center().lat; }
void Map::setProjection(bool g) {
  impl_->transform.setProjection(g ? Projection::Globe : Projection::Mercator);
  impl_->invalidate();
}
bool Map::globe() const { return impl_->transform.projection() == Projection::Globe; }
void Map::panBy(double dx, double dy) {
  auto& t = impl_->transform;
  const auto c = t.centerPoint();
  const auto w = t.unprojectClamped(c.x + dx, c.y + dy);
  t.setLocationAtPoint(w, c.x, c.y);
  impl_->invalidate();
}
void Map::zoomBy(double delta, double x, double y, bool animate) {
  if (animate) {
    CameraOptions c;
    c.zoom = impl_->transform.zoom() + delta;
    c.aroundX = x;
    c.aroundY = y;
    AnimationOptions a;
    a.duration = 250;
    impl_->easeTo(c, a);
  } else {
    impl_->wheel(delta, x, y);
  }
}
void Map::rotateBy(double degrees) {
  impl_->transform.setBearing(impl_->transform.bearing() + degrees);
  impl_->invalidate();
}
void Map::pitchBy(double degrees) {
  impl_->transform.setPitch(impl_->transform.pitch() + degrees);
  impl_->invalidate();
}
void Map::pointerDown(int id, double x, double y) { impl_->pointerDown(id, x, y); }
void Map::pointerMove(int id, double x, double y) { impl_->pointerMove(id, x, y); }
void Map::pointerUp(int id, double x, double y) { impl_->pointerUp(id, x, y); }
void Map::wheel(double d, double x, double y) { impl_->wheel(d, x, y); }
void Map::doubleTap(double x, double y) { zoomBy(1, x, y, true); }
std::optional<std::pair<double, double>> Map::unproject(double x, double y) const {
  auto ll = impl_->transform.unprojectLngLat(x, y);
  if (!ll) return std::nullopt;
  return std::make_pair(ll->lng, ll->lat);
}
std::pair<double, double> Map::project(double lng, double lat) const {
  const auto p = impl_->transform.projectLngLat(lng, lat);
  return {p.x, p.y};
}
void Map::addSource(const std::string& id, const std::string& spec) {
  if (auto v = parseJson(spec)) impl_->addSource(id, *v);
  else impl_->error("invalid-source", "Source JSON could not be read");
}
void Map::removeSource(const std::string& id) { impl_->removeSource(id); }
void Map::addLayer(const std::string& spec, const std::string& beforeId) {
  if (auto v = parseJson(spec)) impl_->addLayer(*v, beforeId);
  else impl_->error("invalid-layer", "Layer JSON could not be read");
}
void Map::removeLayer(const std::string& id) { impl_->removeLayer(id); }
void Map::setPaintProperty(const std::string& layerId, const std::string& name, const std::string& value) {
  auto* l = impl_->layer(layerId);
  auto v = parseJson(value);
  if (!l || !v) return;
  try {
    l->setPaint(name, *v, impl_->styleRoot["transition"]);
  } catch (const std::exception& e) {
    impl_->error("invalid-paint", e.what());
  }
  impl_->invalidate();
}
void Map::setLayoutProperty(const std::string& layerId, const std::string& name, const std::string& value) {
  auto* l = impl_->layer(layerId);
  auto v = parseJson(value);
  if (!l || !v) return;
  try {
    l->setLayout(name, *v);
  } catch (const std::exception& e) {
    impl_->error("invalid-layout", e.what());
  }
  impl_->layersChanged();
}
void Map::setFilter(const std::string& layerId, const std::string& filter) {
  auto* l = impl_->layer(layerId);
  if (!l) return;
  try {
    l->setFilter(filter.empty() ? Value() : parseJson(filter).value_or(Value()));
  } catch (const std::exception& e) {
    impl_->error("invalid-filter", e.what());
  }
  impl_->layersChanged();
}
void Map::setLayerVisible(const std::string& layerId, bool visible) {
  if (auto* l = impl_->layer(layerId)) l->setVisible(visible);
  impl_->layersChanged();
}
std::vector<std::string> Map::layerIds() const {
  std::vector<std::string> ids;
  for (const auto& l : impl_->layers) ids.push_back(l->id);
  return ids;
}
std::string Map::attribution() const { return impl_->attribution(); }
void Map::setOffline(bool o) {
  mapkern::setOffline(o);
  if (!o)
    for (auto& [id, s] : impl_->sources) s->retryFailed();
  impl_->invalidate();
}
bool Map::offline() const { return mapkern::offline(); }
void Map::clearDiskCache() {
  if (impl_->diskCache) impl_->diskCache->clear();
}
void Map::downloadRegion(double w, double s, double e, double n, int minZoom, int maxZoom, std::function<void(size_t, size_t)> progress) {
  impl_->downloadRegion(w, s, e, n, minZoom, maxZoom, std::move(progress));
}
void Map::on(std::function<void(const MapEvent&)> listener) { impl_->listeners.push_back(std::move(listener)); }
MapStats Map::stats() const { return impl_->stats(); }

}
