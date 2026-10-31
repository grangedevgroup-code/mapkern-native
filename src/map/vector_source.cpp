#include "map/vector_source.hpp"

#include <mutex>

namespace mapkern {

ManagerEvents Source::tileEvents() {
  ManagerEvents e;
  auto alive = alive_;
  e.changed = [this, alive] {
    if (*alive && ctx_.changed) ctx_.changed();
  };
  e.failure = [this, alive](const TileFailure& failure, TileId tile) {
    if (!*alive || !ctx_.error) return;
    MapError err;
    err.code = failure.kind == FailureKind::Auth ? "auth" : failure.kind == FailureKind::Decode ? "decode" : "tile-error";
    err.message = "Tile " + std::to_string(tile.z) + "/" + std::to_string(tile.x) + "/" + std::to_string(tile.y) + " of source \"" + id +
                  "\" failed: " + failure.message;
    err.sourceId = id;
    ctx_.error(err);
  };
  e.wakeAt = [this, alive](double t) {
    if (*alive && ctx_.wakeAt) ctx_.wakeAt(t);
  };
  return e;
}

class VectorTileLoader : public TileLoader<VectorTileData> {
 public:
  VectorTileLoader(SourceContext& ctx, TileSet set, std::shared_ptr<bool> alive, std::function<std::shared_ptr<const std::vector<LayerProgram>>()> programs,
                   PromoteId promote)
      : ctx_(ctx), set_(std::move(set)), alive_(std::move(alive)), programs_(std::move(programs)), promote_(std::move(promote)) {}

  void load(TileId tile, CancelToken cancel, std::function<void()> onDecoding, std::function<void(LoadResult<VectorTileData>)> done) override {
    const std::string url = tileUrl(set_, tile.z, tile.x, tile.y);
    const RequestParameters params = resolveRequest(url, ResourceKind::Tile, ctx_.transformRequest);
    Mailbox* box = ctx_.mailbox;
    auto alive = alive_;
    auto programs = programs_();
    const PromoteId promote = promote_;
    fetchBytes(params, cancel, ctx_.requestTimeoutMs, ResourceKind::Tile, ctx_.diskCache,
               [box, alive, tile, cancel, onDecoding, done, programs, promote](FetchOutcome out) {
                 if (cancel.cancelled()) {
                   box->post([alive, done] {
                     if (*alive) done(Aborted{});
                   });
                   return;
                 }
                 if (auto* f = std::get_if<TileFailure>(&out)) {
                   TileFailure failure = *f;
                   box->post([alive, done, failure] {
                     if (*alive) done(failure);
                   });
                   return;
                 }
                 FetchResult r = std::get<FetchResult>(out);
                 if (r.empty || !r.data) {
                   box->post([alive, done] {
                     if (*alive) done(std::shared_ptr<VectorTileData>());
                   });
                   return;
                 }
                 box->post([alive, onDecoding] {
                   if (*alive) onDecoding();
                 });
                 workerPool().submit(
                     [box, alive, tile, cancel, done, programs, promote, r] {
                       if (cancel.cancelled()) {
                         box->post([alive, done] {
                           if (*alive) done(Aborted{});
                         });
                         return;
                       }
                       try {
                         VectorTileResult result = buildVectorTile(*r.data, tile.z, tile.x, tile.y, *programs, false, promote);
                         auto data = std::make_shared<VectorTileData>();
                         size_t bytes = 0;
                         for (auto& b : result.buckets) {
                           auto bucket = std::make_unique<VectorBucket>();
                           bucket->layerId = b.layerId;
                           bucket->type = b.type;
                           bucket->extent = b.extent;
                           bucket->features = std::move(b.features);
                           bucket->labels = std::move(b.labels);
                           if (b.main) {
                             bytes += b.main->vertices.size() + b.main->indices.size() * 4;
                             bucket->cpuMain = std::move(b.main);
                           }
                           if (b.outline) {
                             bytes += b.outline->indices.size() * 4;
                             bucket->cpuOutline = std::move(b.outline);
                           }
                           bytes += bucket->features.count() * 64;
                           data->buckets[bucket->layerId] = std::move(bucket);
                         }
                         data->featureCount = result.featureCount;
                         data->bytes = bytes + r.data->size();
                         data->raw = r.data;
                         box->post([alive, done, data] {
                           if (*alive) done(data);
                         });
                       } catch (const std::exception& e) {
                         TileFailure failure;
                         failure.kind = FailureKind::Decode;
                         failure.message = e.what();
                         failure.retryable = false;
                         box->post([alive, done, failure] {
                           if (*alive) done(failure);
                         });
                       }
                     },
                     tile.z);
               });
  }

  bool needsUpload(const VectorTileData& data) const override { return !data.uploaded; }

  void upload(VectorTileData& data) override {
    Gpu& gpu = Gpu::get();
    for (auto& [id, b] : data.buckets) {
      if (b->cpuMain) {
        GpuGeometry g;
        g.vertices = gpu.vertexBuffer(b->cpuMain->vertices.data(), b->cpuMain->vertices.size());
        g.indices = gpu.indexBuffer(b->cpuMain->indices.data(), b->cpuMain->indices.size() * 4);
        g.indexCount = static_cast<uint32_t>(b->cpuMain->indices.size());
        b->main = std::move(g);
        if (b->cpuOutline && !b->cpuOutline->indices.empty()) {
          GpuGeometry o;
          o.vertices = b->main->vertices;
          o.sharedVertices = true;
          o.indices = gpu.indexBuffer(b->cpuOutline->indices.data(), b->cpuOutline->indices.size() * 4);
          o.indexCount = static_cast<uint32_t>(b->cpuOutline->indices.size());
          b->outline = std::move(o);
        }
      }
      b->cpuMain.reset();
      b->cpuOutline.reset();
    }
    data.uploaded = true;
  }

  void release(VectorTileData& data) override {
    for (auto& [id, b] : data.buckets) {
      if (b->main) b->main->vertices.release(), b->main->indices.release();
      if (b->outline) b->outline->indices.release();
      b->paint.release();
    }
    data.buckets.clear();
  }

  size_t byteSize(const VectorTileData& data) const override {
    size_t bytes = data.raw ? data.raw->size() : 0;
    for (const auto& [id, b] : data.buckets) {
      if (b->main) bytes += b->main->vertices.bytes + b->main->indices.bytes;
      if (b->outline) bytes += b->outline->indices.bytes;
      bytes += b->paint.bytes() + b->features.count() * 64;
      if (b->cpuMain) bytes += b->cpuMain->vertices.size() + b->cpuMain->indices.size() * 4;
    }
    return bytes ? bytes : data.bytes;
  }

  bool emptyIsFinal() const override { return true; }

 private:
  SourceContext& ctx_;
  TileSet set_;
  std::shared_ptr<bool> alive_;
  std::function<std::shared_ptr<const std::vector<LayerProgram>>()> programs_;
  PromoteId promote_;
};

VectorSource::VectorSource(std::string id, Value spec, SourceContext& ctx) : Source(std::move(id), "vector", std::move(spec), ctx) {
  programs_ = std::make_shared<const std::vector<LayerProgram>>();
}

VectorSource::~VectorSource() {
  *alive_ = false;
  lifetime_.cancel();
  if (manager) manager->destroy();
}

void VectorSource::load() {
  auto alive = alive_;
  Mailbox* box = ctx_.mailbox;
  resolveTileSet(id, spec, {0, 14}, ctx_.transformRequest, ctx_.requestTimeoutMs, ctx_.diskCache, lifetime_,
                 [this, alive, box](TileSetResult result) {
                   box->post([this, alive, result = std::move(result)]() mutable {
                     if (!*alive) return;
                     if (auto* err = std::get_if<std::string>(&result)) {
                       if (ctx_.error) ctx_.error({"source-error", *err, id});
                       return;
                     }
                     start(std::get<TileSet>(std::move(result)));
                   });
                 });
}

void VectorSource::start(TileSet set) {
  tileSet = set;
  ctx_.scheduler->setGroupLimit(id, 16);
  if (manager) manager->destroy();
  PromoteId promote;
  const Value& p = spec["promoteId"];
  if (p.isString()) promote = p.asString();
  else if (p.isObject()) {
    std::vector<std::pair<std::string, std::string>> m;
    for (const auto& [k, v] : p.asObject())
      if (v.isString()) m.emplace_back(k, v.asString());
    promote = m;
  }
  loader_ = std::make_shared<VectorTileLoader>(ctx_, set, alive_, [this] { return programs_; }, promote);
  Tiling tiling;
  tiling.tileSize = 512;
  tiling.minZoom = set.minZoom;
  tiling.maxZoom = set.maxZoom;
  tiling.bounds = set.bounds;
  ManagerOptions options;
  options.fadeDuration = 0;
  options.cacheBytes = ctx_.cacheBytes;
  options.prefetchLimit = ctx_.prefetchLimit;
  manager = std::make_unique<TileManager<VectorTileData>>(id, tiling, loader_, *ctx_.scheduler, options, tileEvents());
  if (ctx_.content) ctx_.content(id);
  if (ctx_.changed) ctx_.changed();
}

void VectorSource::update(Transform& transform, double now, const std::vector<Transform*>& predicted) {
  if (manager) manager->update(transform, now, predicted);
}

bool VectorSource::uploadPending(double deadline, double now) { return manager ? manager->uploadPending(deadline, now) : false; }

bool VectorSource::isIdle() const { return !manager || manager->isLoaded(); }

std::optional<TileStats> VectorSource::stats() const {
  if (!manager) return std::nullopt;
  return manager->stats();
}

void VectorSource::reload() {
  if (manager) manager->reload();
}

void VectorSource::retryFailed() {
  if (manager) manager->retryFailed();
}

std::string VectorSource::attribution() const {
  if (spec["attribution"].isString()) return spec["attribution"].asString();
  return tileSet ? tileSet->attribution : "";
}

void VectorSource::setLayers(const std::vector<const StyleLayer*>& layers) {
  auto programs = std::make_shared<std::vector<LayerProgram>>();
  std::string signature;
  for (const StyleLayer* layer : layers) {
    if (auto p = layerProgram(*layer)) {
      signature += p->signature;
      programs->push_back(std::move(*p));
    }
  }
  const bool first = signature_.empty() && programs_->empty();
  programs_ = programs;
  if (signature == signature_) return;
  signature_ = signature;
  if (!first && manager) manager->refresh();
}

}
