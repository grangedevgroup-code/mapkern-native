#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

#include "core/url.hpp"
#include "geo/globe.hpp"
#include "geo/mercator.hpp"
#include "map/map_impl.hpp"
#include "map/dem_source.hpp"
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
  if (options.tileCacheBytes == 0) options.tileCacheBytes = autoTileCacheBytes();
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
  fitWorld = o.zoom == 0 && o.lng == 0 && o.lat == 0;
  if (o.offline) setOffline(true);
  const std::string dir = o.cacheDirectory.empty() ? defaultCacheDirectory() : o.cacheDirectory;
  if (o.diskCacheBytes > 0) diskCache = sharedDiskCache(dir, o.diskCacheBytes);
  sourceCtx.scheduler = &scheduler;
  sourceCtx.mailbox = &mailbox;
  sourceCtx.requestTimeoutMs = o.requestTimeoutMs;
  sourceCtx.diskCache = diskCache;
  sourceCtx.cacheBytes = options.tileCacheBytes;
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
  if (o.controls) setupControls();
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
  errorCount++;
  recentErrors.emplace_back(nowMs(), code + ": " + message);
  while (recentErrors.size() > 20) recentErrors.pop_front();
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
  const bool live = sg_isvalid();
  for (auto& [id, s] : sources) s->reload();
  if (live) {
    painter.~Painter();
    new (&painter) Painter();
  } else {
    painter.resetGpu();
  }
  if (overlay) overlay->resetGpu(live);
  graphics = false;
}

void MapImpl::resize(double width, double height, double ratio) {
  transform.resize(width, height);
  pixelRatio = ratio;
  if (fitWorld && width > 0 && height > 0) {
    fitWorld = false;
    const double top = mercatorY(75), bottom = mercatorY(-55);
    const double lat = latFromMercatorY((top + bottom) / 2);
    const double zoom = options.globe ? globeFitZoom(width, height, transform.fov(), lat)
                                      : std::log2(std::min(width / 512, height / (512 * (bottom - top))));
    transform.setZoom(zoom);
    transform.setCenter(0, lat);
  }
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
  if (overlay) ensureDemoTerrain();
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
  else if (type == "dem") s = std::make_unique<DemSource>(id, spec, sourceCtx);
  else {
    error("unsupported-source", "Source \"" + id + "\" has type " + type + ", which is not drawn yet", id);
    return;
  }
  Source* raw = s.get();
  sources[id] = std::move(s);
  shareTileBudget();
  if (styleLoaded) {
    layersChanged();
    raw->load();
  }
}

void MapImpl::shareTileBudget() {
  const size_t budget = std::max<size_t>(options.tileCacheBytes, size_t{16} << 20);
  for (auto& [id, s] : sources) s->setCacheBytes(budget);
}

void MapImpl::trimTiles() {
  const size_t budget = std::max<size_t>(options.tileCacheBytes, size_t{16} << 20);
  std::vector<Source*> owners;
  size_t total = 0;
  for (auto& [id, s] : sources) {
    owners.push_back(s.get());
    total += s->cachedBytes();
  }
  if (total <= budget) return;
  std::vector<TileCandidate> candidates;
  for (size_t i = 0; i < owners.size(); i++) owners[i]->evictionCandidates(candidates, i);
  std::sort(candidates.begin(), candidates.end(),
            [](const TileCandidate& a, const TileCandidate& b) { return a.lastUsed != b.lastUsed ? a.lastUsed < b.lastUsed : a.z > b.z; });
  for (const auto& c : candidates) {
    if (total <= budget) break;
    total -= std::min(total, c.bytes);
    owners[c.owner]->dropTile(c.key);
  }
}

void MapImpl::removeSource(const std::string& id) {
  sources.erase(id);
  shareTileBudget();
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
  elevationIntent = ElevationIntent::Zoom;
  elevationTo.reset();
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
  aimMotion(to);
  motion = easeMotion(from, to, a.duration < 0 ? 500 : a.duration, easeOutCubic, anchor);
  motionStart = nowMs();
  invalidate();
}

void MapImpl::aimMotion(const CameraState& to) {
  elevationIntent = ElevationIntent::Zoom;
  elevationFrom = transform.elevation();
  elevationTo = terrain ? terrain->elevationAt(to.x - std::floor(to.x), to.y) : std::nullopt;
}

void MapImpl::flyTo(const CameraOptions& c, const AnimationOptions& a) {
  stop();
  const CameraState from = readState(transform);
  const CameraState to = targetState(c);
  FlyOptions fly;
  fly.curve = a.curve;
  if (a.duration >= 0) fly.duration = a.duration;
  aimMotion(to);
  motion = flyMotion(from, to, std::max(transform.width, transform.height), fly);
  motionStart = nowMs();
  invalidate();
}

void MapImpl::stop() {
  motion.reset();
  gestures.stopMomentum();
}

std::optional<Point3> MapImpl::pickWorld(double px, double py) {
  const double ws = transform.worldSize();
  if (terrain && transform.globeAmount() <= 0) {
    std::array<double, 3> origin, dir;
    transform.screenRay(px, py, origin, dir);
    const std::array<double, 3> o{origin[0] / ws, origin[1] / ws, origin[2] / ws};
    const std::array<double, 3> d{dir[0] / ws, dir[1] / ws, dir[2] / ws};
    if (auto hit = terrain->raycast(o, d, std::hypot(d[0], d[1], d[2]))) return *hit;
  }
  const auto w = transform.unprojectExact(px, py);
  if (!w) return std::nullopt;
  return Point3{(*w)[0], (*w)[1], transform.centerElevationPixels() / ws};
}

void MapImpl::easeZoom(double delta, double x, double y) {
  CameraOptions c;
  c.zoom = std::round((transform.zoom() + delta) * 1000) / 1000;
  c.aroundX = x;
  c.aroundY = y;
  AnimationOptions a;
  a.duration = 300;
  easeTo(c, a);
}

void MapImpl::pointerDown(int id, double x, double y, double time) {
  if (overlay && overlay->pointerDown(id, x, y)) {
    overlayPointers.insert(id);
    invalidate();
    return;
  }
  gestures.down(id, x, y, time);
  invalidate();
}

void MapImpl::pointerMove(int id, double x, double y, double time) {
  if (overlayPointers.contains(id)) return;
  gestures.move(id, x, y, time);
}

void MapImpl::pointerUp(int id, double x, double y, double time, bool cancelled) {
  if (overlayPointers.erase(id)) {
    if (overlay) {
      if (cancelled) overlay->pointerCancel(id);
      else overlay->pointerUp(id, x, y);
    }
    invalidate();
    return;
  }
  gestures.up(id, x, y, time, cancelled);
  invalidate();
}

void MapImpl::wheel(double delta, double x, double y) {
  gestures.wheel(delta, x, y, nowMs());
  invalidate();
}

std::string MapImpl::attribution() const {
  std::vector<std::string> parts;
  for (const auto& [id, s] : sources) {
    bool used = false;
    for (const auto& l : layers) used = used || (l->source == id && l->visible);
    used = used || (terrain && terrain->source == s.get());
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
  std::optional<double> progress;
  if (motion) {
    const double t = motion->duration > 0 ? (now - motionStart) / motion->duration : 1;
    motion->apply(transform, t);
    progress = std::clamp(t, 0.0, 1.0);
    if (t >= 1) motion.reset();
    dirty = true;
  }
  if (gestures.step(now)) dirty = true;
  for (auto& l : layers)
    if (l->settle(now)) dirty = true;
  updateTerrain(progress);
  for (auto& [id, s] : sources) s->update(transform, now, {});
  trimTiles();
  const double uploadStart = nowMs();
  UploadBudget budget{uploadStart + 6};
  std::vector<Source*> order;
  order.reserve(sources.size());
  for (auto& [id, s] : sources) order.push_back(s.get());
  if (!order.empty()) std::rotate(order.begin(), order.begin() + static_cast<std::ptrdiff_t>(uploadTurn++ % order.size()), order.end());
  for (Source* s : order)
    if (s->uploadPending(budget, now)) dirty = true;
  uploadMs = nowMs() - uploadStart;
  scheduler.pump();
  if (wakeAt > 0 && now >= wakeAt) {
    wakeAt = 0;
    dirty = true;
  }
}

bool MapImpl::needsRender() const {
  if (dirty || motion || gestures.animating() || gestures.touching()) return true;
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
  const double tickMs = nowMs() - start;
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
  frame.terrain = terrain.get();
  frame.moving = motion.has_value() || gestures.touching() || gestures.animating();
  if (overlay) {
    updateOverlay();
    frame.overlay = overlay.get();
  }
  painter.render(frame, target);
  dirty = painter.drapesBehind() || painter.labelsAnimating();
  lastFrameMs = nowMs() - start;
  FrameWindow& f = frameWindow;
  f.frames++;
  f.cpuSum += lastFrameMs;
  f.cpuMax = std::max(f.cpuMax, lastFrameMs);
  f.tickSum += tickMs;
  f.tickMax = std::max(f.tickMax, tickMs);
  if (lastFrameMs > 16.7) f.slow++;
  const PaintStats& ps = painter.stats();
  const uint64_t draws = static_cast<uint64_t>(ps.drawCalls);
  f.draws += draws;
  f.drawsMax = std::max(f.drawsMax, draws);
  f.drapes += static_cast<uint64_t>(ps.drapes);
  f.uploadSum += uploadMs;
  f.uploadMax = std::max(f.uploadMax, uploadMs);
  f.drapeSum += ps.drapeMs;
  f.drapeMax = std::max(f.drapeMax, ps.drapeMs);
  f.labelSum += ps.labelMs;
  f.labelMax = std::max(f.labelMax, ps.labelMs);
  const double ppm = transform.pixelsPerMeter();
  const double cameraZ = transform.cameraPosition()[2] / ppm;
  const double height = std::max(1.0, cameraZ - transform.centerElevationPixels() / ppm);
  const double step = (cameraZ - lastCameraZ) / height;
  if (lastCameraZ != 0) {
    f.cameraStepMax = std::max(f.cameraStepMax, std::abs(step));
    if (std::abs(step) > 0.01 && std::abs(lastCameraStep) > 0.01 && (step > 0) != (lastCameraStep > 0)) f.cameraFlips++;
  }
  lastCameraZ = cameraZ;
  lastCameraStep = step;
  const World centre = transform.centerWorld();
  if (lastCenter) {
    const double turn = globeTurn((*lastCenter)[0], (*lastCenter)[1], centre[0], centre[1]) * 180 / kPi;
    f.turnMax = std::max(f.turnMax, turn);
    const double elapsed = std::max(1.0, start - lastFrameStart);
    if (turn > 5 && turn / elapsed > 0.3 && std::abs(transform.zoom() - (*lastCenter)[2]) < 0.05) f.bigTurns++;
  }
  lastCenter = std::array<double, 3>{centre[0], centre[1], transform.zoom()};
  if (lastFrameMs > f.worst.back().ms) {
    f.worst.back() = {start, lastFrameMs, tickMs, uploadMs, ps.drapeMs, ps.labelMs, draws};
    std::sort(f.worst.begin(), f.worst.end(), [](const SlowFrame& a, const SlowFrame& b) { return a.ms > b.ms; });
  }
  if (gestures.touching()) {
    f.touchFrames++;
    if (lastFrameStart > 0 && start - lastFrameStart < 250) {
      const double gap = start - lastFrameStart;
      f.touchMs += gap;
      f.touchGapMax = std::max(f.touchGapMax, gap);
      f.touchGaps[gap < 12 ? 0 : gap < 17 ? 1 : gap < 25 ? 2 : gap < 34 ? 3 : gap < 50 ? 4 : 5]++;
    }
  }
  if (lastFrameStart > 0 && start - lastFrameStart < 250) {
    const double gap = start - lastFrameStart;
    f.intervals++;
    f.intervalSum += gap;
    f.intervalMax = std::max(f.intervalMax, gap);
    const int bucket = gap < 12 ? 0 : gap < 17 ? 1 : gap < 25 ? 2 : gap < 34 ? 3 : gap < 50 ? 4 : 5;
    f.buckets[bucket]++;
  }
  lastFrameStart = start;
  return true;
}

std::vector<std::pair<std::string, std::string>> defaultBasemaps() {
  return {{"Map", "https://tiles.openfreemap.org/styles/liberty"},
          {"Satellite",
           R"j({"version":1,"sources":{"imagery":{"type":"raster","tiles":["https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/{z}/{y}/{x}"],"tileSize":256,"maxZoom":19,"attribution":"Esri, Vantor, Earthstar Geographics, and the GIS User Community"}},"layers":[{"id":"imagery","type":"raster","source":"imagery"}]})j"}};
}

std::string defaultTerrainSource() {
  return R"j({"type":"dem","tiles":["https://tiles.mapterhorn.com/{z}/{x}/{y}.webp"],"tileSize":512,"maxZoom":12,"encoding":"terrarium","attribution":"<a href=\"https://mapterhorn.com/attribution\">&copy; Mapterhorn</a>"})j";
}

void MapImpl::setupControls() {
  overlay = std::make_unique<ui::Overlay>();
  overlay->touch = options.touchControls;
  if (options.basemaps.empty()) options.basemaps = defaultBasemaps();
  if (options.terrainSource.empty()) options.terrainSource = defaultTerrainSource();
  for (const auto& [name, style] : options.basemaps) overlay->basemaps.push_back({name, style});
  for (size_t i = 0; i < options.basemaps.size(); i++)
    if (options.basemaps[i].second == options.style) overlay->selected = i;
  overlay->actions.zoomIn = [this] {
    CameraOptions c;
    c.zoom = std::min(transform.maxZoom, transform.zoom() + 1);
    easeTo(c, {});
  };
  overlay->actions.zoomOut = [this] {
    CameraOptions c;
    c.zoom = std::max(transform.lowestZoom(), transform.zoom() - 1);
    easeTo(c, {});
  };
  overlay->actions.resetNorth = [this] {
    CameraOptions c;
    c.bearing = 0;
    c.pitch = 0;
    easeTo(c, {});
  };
  overlay->actions.selectBasemap = [this](size_t i) {
    if (i < options.basemaps.size()) setStyle(options.basemaps[i].second);
  };
  overlay->actions.setView = [this](const std::string& v) { setView(v); };
  const std::string start = options.view == "tilted" || options.view == "terrain" ? options.view : "globe";
  transform.setProjection(start == "globe" ? Projection::Globe : Projection::Mercator);
  if (start == "tilted" && transform.pitch() < 10) transform.setPitch(60);
  if (start != "tilted") transform.setPitch(0);
}

void MapImpl::ensureDemoTerrain() {
  if (options.terrainSource.empty()) return;
  const std::string id = "mapkern-terrain";
  if (!sources.contains(id)) {
    auto spec = parseJson(options.terrainSource);
    if (!spec) return;
    addSource(id, *spec);
  }
  if (!styleRoot["terrain"].isObject()) setTerrain(Value(Object{{"source", id}, {"exaggeration", 1.0}, {"relief", 0.5}}));
}

std::string MapImpl::currentView() {
  if (transform.projection() == Projection::Globe) return "globe";
  return transform.pitch() > 5 ? "tilted" : "terrain";
}

void MapImpl::setView(const std::string& v) {
  ensureDemoTerrain();
  const bool globe = v == "globe";
  const double pitch = v == "tilted" ? std::min(60.0, options.maxPitch) : 0;
  if (!globe) {
    motion.reset();
    gestures.stopMomentum();
    transform.setZoom(transform.flatZoom(pitch));
  }
  transform.setProjection(globe ? Projection::Globe : Projection::Mercator);
  CameraOptions c;
  c.pitch = pitch;
  AnimationOptions a;
  a.duration = 500;
  easeTo(c, a);
}

static double haversineMeters(LngLat a, LngLat b) {
  const double k = kPi / 180;
  const double dlat = (b.lat - a.lat) * k, dlng = (b.lng - a.lng) * k;
  const double h = std::sin(dlat / 2) * std::sin(dlat / 2) + std::cos(a.lat * k) * std::cos(b.lat * k) * std::sin(dlng / 2) * std::sin(dlng / 2);
  return 2 * kEarthRadius * std::asin(std::min(1.0, std::sqrt(h)));
}

void MapImpl::updateOverlay() {
  ui::OverlayState s;
  s.width = transform.width;
  s.height = transform.height;
  s.pixelRatio = pixelRatio;
  s.zoom = transform.zoom();
  s.minZoom = transform.lowestZoom();
  s.maxZoom = transform.maxZoom;
  s.bearing = transform.bearing();
  s.pitch = transform.pitch();
  s.view = currentView();
  s.attribution = ui::plainAttribution(attribution());
  const double h = transform.height / 2;
  const auto a = transform.unprojectLngLat(0, h);
  const auto b = transform.unprojectLngLat(100, h);
  s.metersPerMaxWidth = a && b ? haversineMeters(*a, *b) : 0;
  s.hud = hudLines;
  overlay->update(s);
}

std::string MapImpl::diagnosticsJson() {
  const double now = nowMs();
  const double span = lastSnapshot > 0 ? (now - lastSnapshot) / 1000 : 0;
  lastSnapshot = now;
  const diag::Totals t = diag::totals();
  const diag::Totals& p = lastTotals;
  auto r2 = [](double v) { return std::round(v * 100) / 100; };
  auto d = [](uint64_t a, uint64_t b) { return static_cast<double>(a >= b ? a - b : 0); };
  auto rate = [&](double v) { return span > 0 ? r2(v / span) : 0.0; };
  const FrameWindow& f = frameWindow;
  const double frames = static_cast<double>(f.frames);
  Array buckets;
  for (uint64_t b : f.buckets) buckets.emplace_back(static_cast<double>(b));
  Array touchGaps;
  for (uint64_t b : f.touchGaps) touchGaps.emplace_back(static_cast<double>(b));
  Array worst;
  for (const SlowFrame& w : f.worst)
    if (w.ms > 0)
      worst.emplace_back(Object{{"ms", r2(w.ms)},
                                {"tickMs", r2(w.tick)},
                                {"uploadMs", r2(w.upload)},
                                {"drapeMs", r2(w.drapes)},
                                {"labelMs", r2(w.labels)},
                                {"draws", static_cast<double>(w.draws)},
                                {"agoMs", std::round(now - w.at)}});
  Object frame{{"frames", frames},
               {"fps", rate(frames)},
               {"cpuAvgMs", f.frames ? r2(f.cpuSum / frames) : 0.0},
               {"cpuMaxMs", r2(f.cpuMax)},
               {"tickAvgMs", f.frames ? r2(f.tickSum / frames) : 0.0},
               {"tickMaxMs", r2(f.tickMax)},
               {"slow", static_cast<double>(f.slow)},
               {"intervalAvgMs", f.intervals ? r2(f.intervalSum / static_cast<double>(f.intervals)) : 0.0},
               {"intervalMaxMs", r2(f.intervalMax)},
               {"intervalBuckets", Value(std::move(buckets))},
               {"drawsAvg", f.frames ? r2(static_cast<double>(f.draws) / frames) : 0.0},
               {"drawsMax", static_cast<double>(f.drawsMax)},
               {"pieces", painter.stats().pieces},
               {"uploadAvgMs", f.frames ? r2(f.uploadSum / frames) : 0.0},
               {"uploadMaxMs", r2(f.uploadMax)},
               {"drapes", static_cast<double>(f.drapes)},
               {"drapeAvgMs", f.frames ? r2(f.drapeSum / frames) : 0.0},
               {"drapeMaxMs", r2(f.drapeMax)},
               {"labelAvgMs", f.frames ? r2(f.labelSum / frames) : 0.0},
               {"labelMaxMs", r2(f.labelMax)},
               {"cameraFlips", static_cast<double>(f.cameraFlips)},
               {"cameraStepMaxPct", r2(f.cameraStepMax * 100)},
               {"turnMaxDeg", r2(f.turnMax)},
               {"bigTurns", static_cast<double>(f.bigTurns)},
               {"touchFrames", static_cast<double>(f.touchFrames)},
               {"touchMs", r2(f.touchMs)},
               {"touchGapMax", r2(f.touchGapMax)},
               {"touchGaps", Value(std::move(touchGaps))},
               {"worst", Value(std::move(worst))}};
  static const char* kinds[] = {"style", "source", "tile", "glyphs", "sprite", "image"};
  Object requests;
  for (int i = 0; i < diag::kKinds; i++) requests.emplace_back(kinds[i], d(t.requests[i], p.requests[i]));
  static const char* results[] = {"ok", "empty", "http4xx", "http5xx", "timeout", "error", "cancelled"};
  Object outcomes;
  for (int i = 0; i < static_cast<int>(diag::NetResult::Count); i++) outcomes.emplace_back(results[i], d(t.results[i], p.results[i]));
  const double netBytes = d(t.bytesNetwork, p.bytesNetwork);
  const double answered = d(t.latency.count, p.latency.count);
  Object net{{"requests", Value(std::move(requests))},
             {"results", Value(std::move(outcomes))},
             {"bytes", netBytes},
             {"kbps", rate(netBytes * 8 / 1000)},
             {"latencyAvgMs", answered > 0 ? r2((t.latency.totalMs - p.latency.totalMs) / answered) : 0.0},
             {"latencyMaxMs", r2(t.latency.maxMs)},
             {"queued", static_cast<double>(scheduler.queuedCount())},
             {"active", static_cast<double>(scheduler.activeCount())}};
  Object cache{{"hits", d(t.cacheHits, p.cacheHits)}, {"stale", d(t.cacheStale, p.cacheStale)}, {"bytes", d(t.bytesCache, p.bytesCache)},
               {"localReads", d(t.localReads, p.localReads)}, {"localBytes", d(t.bytesLocal, p.bytesLocal)}};
  if (diskCache) {
    const auto c = diskCache->stats();
    cache.emplace_back("diskBytes", static_cast<double>(c.bytes));
    cache.emplace_back("diskEntries", static_cast<double>(c.entries));
  }
  auto stage = [&](diag::Stage s) {
    const int i = static_cast<int>(s);
    const double n = d(t.stages[i].count, p.stages[i].count);
    return Object{{"n", n},
                  {"avgMs", n > 0 ? r2((t.stages[i].totalMs - p.stages[i].totalMs) / n) : 0.0},
                  {"maxMs", r2(t.stages[i].maxMs)},
                  {"kb", r2(d(t.stageBytes[i], p.stageBytes[i]) / 1024)}};
  };
  Object decode{{"vectorTile", stage(diag::Stage::VectorTile)}, {"image", stage(diag::Stage::Image)}, {"dem", stage(diag::Stage::Dem)}};
  const SlowUpload slowest = Gpu::get().takeSlowestUpload();
  Object upload{{"textures", stage(diag::Stage::Texture)},
                {"buffers", stage(diag::Stage::Buffer)},
                {"slowest", Object{{"ms", r2(slowest.ms)},
                                   {"width", slowest.width},
                                   {"height", slowest.height},
                                   {"levels", slowest.levels},
                                   {"format", pixelFormatName(slowest.format)}}}};
  Object tiles;
  double tileBytes = 0;
  for (const auto& [id, src] : sources) {
    const auto st = src->stats();
    if (!st) continue;
    tileBytes += static_cast<double>(st->bytes);
    tiles.emplace_back(id, Object{{"tiles", static_cast<double>(st->tiles)},
                                  {"ready", static_cast<double>(st->ready)},
                                  {"loading", static_cast<double>(st->loading)},
                                  {"failed", static_cast<double>(st->failed)},
                                  {"empty", static_cast<double>(st->empty)},
                                  {"visible", static_cast<double>(st->visibleSlots)},
                                  {"fallback", static_cast<double>(st->fallbackPieces)},
                                  {"blank", r2(st->blankPending + st->blankFailed)},
                                  {"requested", static_cast<double>(st->requested)},
                                  {"completed", static_cast<double>(st->completed)},
                                  {"failures", static_cast<double>(st->failures)},
                                  {"evicted", static_cast<double>(st->evicted)},
                                  {"mb", r2(static_cast<double>(st->bytes) / 1048576)}});
  }
  const LngLat c = transform.center();
  Object camera{{"lng", std::round(c.lng * 1e5) / 1e5},
                {"lat", std::round(c.lat * 1e5) / 1e5},
                {"zoom", std::round(transform.zoom() * 1000) / 1000},
                {"bearing", r2(transform.bearing())},
                {"pitch", r2(transform.pitch())},
                {"globe", r2(transform.globeAmount())},
                {"view", currentView()},
                {"terrain", terrain != nullptr},
                {"width", transform.width},
                {"height", transform.height},
                {"ratio", pixelRatio}};
  const PanStats pan = gestures.takePanStats();
  Object panning{{"moves", static_cast<double>(pan.moves)},
                 {"wild", static_cast<double>(pan.wild)},
                 {"maxSlipPx", r2(pan.maxSlipPx)},
                 {"maxTurnDeg", r2(pan.maxTurnDeg)},
                 {"maxTurnRatio", r2(pan.maxTurnRatio)}};
  Object state{{"styleLoaded", styleLoaded},
               {"offline", mapkern::offline()},
               {"reachable", networkReachable()},
               {"touching", gestures.touching()},
               {"gesture", gestures.active()},
               {"animating", motion.has_value() || gestures.animating()}};
  Array fresh;
  const uint64_t added = errorCount - errorsReported;
  const size_t take = static_cast<size_t>(std::min<uint64_t>(added, recentErrors.size()));
  for (size_t i = recentErrors.size() - take; i < recentErrors.size(); i++) fresh.emplace_back(recentErrors[i].second);
  Object errors{{"new", static_cast<double>(added)}, {"total", static_cast<double>(errorCount)}, {"recent", Value(std::move(fresh))}};
  Object root{{"span", r2(span)},       {"frame", Value(std::move(frame))},   {"net", Value(std::move(net))},
              {"cache", Value(std::move(cache))}, {"decode", Value(std::move(decode))}, {"upload", Value(std::move(upload))},
              {"tiles", Value(std::move(tiles))},
              {"tileMb", r2(tileBytes / 1048576)}, {"camera", Value(std::move(camera))}, {"pan", Value(std::move(panning))},
              {"state", Value(std::move(state))},
              {"errors", Value(std::move(errors))}};
  frameWindow = FrameWindow{};
  lastTotals = t;
  errorsReported = errorCount;
  return Value(std::move(root)).toJson();
}

void MapImpl::setTerrain(const Value& spec) {
  Object root = styleRoot.isObject() ? styleRoot.asObject() : Object{};
  std::erase_if(root, [](const auto& kv) { return kv.first == "terrain"; });
  if (spec.isObject() && spec["source"].isString()) root.emplace_back("terrain", spec);
  styleRoot = Value(std::move(root));
  invalidate();
}

std::optional<double> MapImpl::cameraFloor(Terrain& t) {
  const double ws = transform.worldSize();
  const auto camera = transform.cameraPosition();
  const auto ground = t.elevationAt(camera[0] / ws, camera[1] / ws);
  if (!ground) return std::nullopt;
  const double ppm = transform.pixelsPerMeter();
  const double above = (camera[2] - transform.centerElevationPixels()) / ppm;
  return *ground + 50 * t.exaggeration + 2 / ppm - above;
}

double MapImpl::nearPeak(Terrain& t) {
  const double ws = transform.worldSize();
  const auto camera = transform.cameraPosition();
  const double ground = transform.ground() * transform.pixelsPerMeter();
  double peak = -std::numeric_limits<double>::infinity();
  auto sample = [&](double x, double y) {
    if (auto m = t.elevationAt(x, y); m && *m > peak) peak = *m;
  };
  const double cx = camera[0] / ws, cy = camera[1] / ws;
  sample(cx, cy);
  for (double px : {0.0, transform.width / 2, transform.width}) {
    const World h = transform.unprojectClamped(px, transform.height, ground);
    for (int i = 1; i <= 8; i++) {
      const double f = i / 8.0;
      sample(cx + (h[0] - cx) * f, cy + (h[1] - cy) * f);
    }
  }
  return peak;
}

void MapImpl::updateTerrain(std::optional<double> progress) {
  const Value& spec = styleRoot["terrain"];
  DemSource* source = nullptr;
  if (spec.isObject() && spec["source"].isString())
    if (auto it = sources.find(spec["source"].asString()); it != sources.end()) source = dynamic_cast<DemSource*>(it->second.get());
  if (!source || !source->manager) {
    if (terrain) {
      terrain.reset();
      transform.setGround(0);
      if (transform.elevation() != 0) transform.setElevation(0);
      dirty = true;
    }
    return;
  }
  const double exaggeration = std::max(0.0, spec["exaggeration"].numberOr(1));
  if (!terrain || terrain->source != source) terrain = std::make_unique<Terrain>(source, exaggeration);
  terrain->exaggeration = exaggeration;
  terrain->relief = std::clamp(spec["relief"].numberOr(0), 0.0, 1.0);
  const World c = transform.centerWorld();
  auto target = terrain->elevationAt(c[0], c[1]);
  const bool camera = elevationIntent == ElevationIntent::Camera && !progress;
  if (progress && elevationTo) target = elevationFrom + (*elevationTo - elevationFrom) * *progress;
  else if (camera)
    if (auto aim = aimElevation(*terrain)) target = aim;
  if (target) {
    const double before = transform.elevation();
    const double zoom = transform.zoom();
    if (camera && std::abs(*target - transform.elevation()) > 1e-3) transform.setElevationKeepCamera(*target);
    const auto floor = cameraFloor(*terrain);
    if (camera && floor && *floor > transform.elevation() + 1e-3) {
      backOff(*terrain);
    } else {
      const double wanted = floor ? std::max(*target, *floor) : *target;
      if (std::abs(wanted - transform.elevation()) > 1e-3) transform.setElevation(wanted);
    }
    if (transform.elevation() != before || transform.zoom() != zoom) dirty = true;
  }
  transform.setPeak(nearPeak(*terrain));
  terrain->update(transform);
  transform.setGround(std::min(terrain->ground(), transform.elevation()));
}

std::optional<double> MapImpl::aimElevation(Terrain& t) {
  if (transform.globeAmount() > 0) return std::nullopt;
  const double ws = transform.worldSize();
  const Point p = transform.centerPoint();
  std::array<double, 3> origin, dir;
  transform.screenRay(p.x, p.y, origin, dir);
  const std::array<double, 3> d{dir[0] / ws, dir[1] / ws, dir[2] / ws};
  const auto hit = t.raycast({origin[0] / ws, origin[1] / ws, origin[2] / ws}, d, std::hypot(d[0], d[1], d[2]));
  if (!hit) return std::nullopt;
  return t.elevationAt((*hit)[0], (*hit)[1]);
}

void MapImpl::backOff(Terrain& t) {
  auto clear = [&](double zoom) {
    transform.setZoom(zoom);
    const auto floor = cameraFloor(t);
    return !floor || *floor <= transform.elevation() + 1e-3;
  };
  const double requested = transform.zoom();
  const auto zoom = highestClearZoom(requested, std::max(transform.minZoom, requested - 8), clear);
  if (!zoom) {
    transform.setZoom(requested);
    if (const auto floor = cameraFloor(t)) transform.setElevation(*floor);
    return;
  }
  transform.setZoom(*zoom);
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
bool Map::isMoving() const { return impl_->motion.has_value() || impl_->gestures.animating() || impl_->gestures.touching(); }
double Map::zoom() const { return impl_->transform.zoom(); }
double Map::bearing() const { return impl_->transform.bearing(); }
double Map::pitch() const { return impl_->transform.pitch(); }
double Map::centerLng() const { return impl_->transform.center().lng; }
double Map::centerLat() const { return impl_->transform.center().lat; }
void Map::setProjection(bool g) {
  Transform& t = impl_->transform;
  if (!g && t.projection() == Projection::Globe) t.setZoom(t.flatZoom(t.pitch()));
  t.setProjection(g ? Projection::Globe : Projection::Mercator);
  impl_->invalidate();
}
bool Map::globe() const { return impl_->transform.projection() == Projection::Globe; }
void Map::setTerrain(const std::string& sourceId, double exaggeration, double relief) {
  impl_->setTerrain(Value(Object{{"source", sourceId}, {"exaggeration", exaggeration}, {"relief", relief}}));
}
void Map::clearTerrain() { impl_->setTerrain(Value()); }
bool Map::terrain() const { return impl_->terrain != nullptr; }
std::optional<double> Map::elevationAt(double lng, double lat) const {
  if (!impl_->terrain) return std::nullopt;
  return impl_->terrain->elevationAt(mercatorX(lng), mercatorY(clampLatitude(lat)));
}
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
void Map::pointerDown(int id, double x, double y, double timeMs) { impl_->pointerDown(id, x, y, timeMs < 0 ? nowMs() : timeMs); }
void Map::pointerMove(int id, double x, double y, double timeMs) { impl_->pointerMove(id, x, y, timeMs < 0 ? nowMs() : timeMs); }
void Map::pointerUp(int id, double x, double y, double timeMs) { impl_->pointerUp(id, x, y, timeMs < 0 ? nowMs() : timeMs, false); }
void Map::pointerCancel(int id) { impl_->pointerUp(id, 0, 0, nowMs(), true); }
std::string Map::diagnostics() { return impl_->diagnosticsJson(); }
void Map::setView(const std::string& view) { impl_->setView(view); }
void Map::selectBasemap(size_t index) {
  auto& o = impl_->options;
  if (index >= o.basemaps.size()) return;
  if (impl_->overlay) impl_->overlay->selected = index;
  impl_->setStyle(o.basemaps[index].second);
}
void Map::setHud(std::vector<std::string> lines) {
  if (lines == impl_->hudLines) return;
  impl_->hudLines = std::move(lines);
  impl_->invalidate();
}
void Map::wheel(double d, double x, double y) { impl_->wheel(d, x, y); }
void Map::doubleTap(double x, double y) {
  if (impl_->overlay && impl_->overlay->contains(x, y)) return;
  zoomBy(1, x, y, true);
}
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
