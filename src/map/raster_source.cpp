#include "map/raster_source.hpp"

#include <cmath>
#include <cstdlib>

namespace mapkern {

namespace {

uint32_t fingerprint(const Bytes& bytes) {
  uint32_t h = 0x811c9dc5u ^ static_cast<uint32_t>(bytes.size());
  for (uint8_t b : bytes) {
    h ^= b;
    h *= 0x01000193u;
  }
  return h;
}

bool isPowerOfTwo(int n) { return n > 0 && (n & (n - 1)) == 0; }

class RasterTileLoader : public TileLoader<RasterTileData> {
 public:
  RasterTileLoader(SourceContext& ctx, TileSet set, std::shared_ptr<bool> alive, std::function<void(int, int, int)> forget)
      : ctx_(ctx), set_(std::move(set)), alive_(std::move(alive)), placeholders_(std::make_shared<Placeholders>(std::move(forget))) {}

  void load(TileId tile, CancelToken cancel, std::function<void()> onDecoding, std::function<void(LoadResult<RasterTileData>)> done) override {
    const RequestParameters params = resolveRequest(tileUrl(set_, tile.z, tile.x, tile.y), ResourceKind::Tile, ctx_.transformRequest);
    Mailbox* box = ctx_.mailbox;
    auto alive = alive_;
    auto placeholders = placeholders_;
    fetchBytes(params, cancel, ctx_.requestTimeoutMs, ResourceKind::Tile, ctx_.diskCache,
               [box, alive, tile, cancel, onDecoding, done, placeholders](FetchOutcome out) {
                 auto post = [box, alive, done](LoadResult<RasterTileData> r) {
                   box->post([alive, done, r = std::move(r)]() mutable {
                     if (*alive) done(std::move(r));
                   });
                 };
                 if (cancel.cancelled()) return post(Aborted{});
                 if (auto* f = std::get_if<TileFailure>(&out)) return post(*f);
                 FetchResult r = std::get<FetchResult>(out);
                 if (r.empty || !r.data) return post(std::shared_ptr<RasterTileData>());
                 const auto hash = placeholders->note(tile, *r.data);
                 if (!hash) return post(std::shared_ptr<RasterTileData>());
                 box->post([alive, onDecoding] {
                   if (*alive) onDecoding();
                 });
                 workerPool().submit(
                     [post, tile, cancel, r, hash, placeholders] {
                       if (cancel.cancelled()) return post(Aborted{});
                       std::string error;
                       auto image = decodeImage(*r.data, true, &error);
                       if (!image) {
                         TileFailure f;
                         f.kind = FailureKind::Decode;
                         f.message = "Tile image could not be decoded: " + error;
                         return post(f);
                       }
                       if (placeholders->suspect(tile, *hash) && !isFlatImage(*image)) {
                         placeholders->confirm(*hash, tile);
                         return post(std::shared_ptr<RasterTileData>());
                       }
                       auto data = std::make_shared<RasterTileData>();
                       data->width = image->width;
                       data->height = image->height;
                       data->image = std::move(image);
                       post(data);
                     },
                     tile.z);
               });
  }

  bool needsUpload(const RasterTileData& data) const override { return !data.texture.valid(); }

  void upload(RasterTileData& data) override {
    if (!data.image) return;
    const bool pow2 = isPowerOfTwo(data.width) && isPowerOfTwo(data.height);
    data.texture = Gpu::get().texture(data.width, data.height, SG_PIXELFORMAT_RGBA8, data.image->rgba.data(), data.image->rgba.size(), pow2);
    data.image.reset();
  }

  void release(RasterTileData& data) override {
    data.image.reset();
    data.texture.release();
  }

  size_t byteSize(const RasterTileData& data) const override {
    const size_t base = static_cast<size_t>(data.width) * data.height * 4;
    return data.texture.valid() ? static_cast<size_t>(std::lround(base * 1.34)) : base;
  }

 private:
  SourceContext& ctx_;
  TileSet set_;
  std::shared_ptr<bool> alive_;
  std::shared_ptr<Placeholders> placeholders_;
};

}

std::optional<uint32_t> Placeholders::note(TileId tile, const Bytes& data) {
  const uint32_t hash = fingerprint(data);
  std::lock_guard lock(mutex_);
  if (known_.contains(hash)) return std::nullopt;
  const uint64_t key = tileKey(tile);
  std::erase_if(keys_, [key](const auto& kv) { return kv.first == key; });
  keys_.emplace_back(key, hash);
  if (keys_.size() > 8192) keys_.pop_front();
  auto& twins = twins_[hash];
  if (twins_.size() > 8192) twins_.erase(twins_.begin());
  if (twins.size() < 16) twins[key] = tile.z;
  return hash;
}

bool Placeholders::suspect(TileId tile, uint32_t hash) {
  std::lock_guard lock(mutex_);
  auto it = twins_.find(hash);
  if (it == twins_.end() || it->second.size() < 2 || tile.z == 0) return false;
  for (const auto& [k, z] : it->second)
    if (z != tile.z) return true;
  return false;
}

void Placeholders::confirm(uint32_t hash, TileId tile) {
  std::vector<TileId> others;
  {
    std::lock_guard lock(mutex_);
    known_.insert(hash);
    const uint64_t current = tileKey(tile);
    if (auto it = twins_.find(hash); it != twins_.end())
      for (const auto& [k, z] : it->second)
        if (k != current) others.push_back(tileFromKey(k));
  }
  for (const auto& t : others) forget_(t.z, t.x, t.y);
}

bool isFlatImage(const Image& image) {
  const int size = 16;
  if (image.width <= 0 || image.height <= 0) return true;
  const uint8_t* first = image.rgba.data();
  for (int sy = 0; sy < size; sy++)
    for (int sx = 0; sx < size; sx++) {
      const int x = std::min(image.width - 1, (sx * image.width + image.width / 2) / size);
      const int y = std::min(image.height - 1, (sy * image.height + image.height / 2) / size);
      const uint8_t* p = image.rgba.data() + (static_cast<size_t>(y) * image.width + x) * 4;
      for (int c = 0; c < 4; c++)
        if (std::abs(p[c] - first[c]) > 3) return false;
    }
  return true;
}

RasterSource::RasterSource(std::string id, Value spec, SourceContext& ctx, std::string type)
    : Source(std::move(id), std::move(type), std::move(spec), ctx), fade_(ctx.fadeDuration) {}

RasterSource::~RasterSource() {
  *alive_ = false;
  lifetime_.cancel();
  if (manager) manager->destroy();
}

double RasterSource::tileSize() const { return spec["tileSize"].numberOr(type == "dem" ? 512 : 256); }

void RasterSource::load() {
  auto alive = alive_;
  Mailbox* box = ctx_.mailbox;
  resolveTileSet(id, spec, {0, 22}, ctx_.transformRequest, ctx_.requestTimeoutMs, ctx_.diskCache, lifetime_,
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

void RasterSource::start(TileSet set) {
  tileSet = set;
  ctx_.scheduler->setGroupLimit(id, 16);
  if (manager) manager->destroy();
  loader_ = std::make_shared<RasterTileLoader>(ctx_, set, alive_, [this](int z, int x, int y) {
    if (manager) manager->forget(z, x, y);
  });
  Tiling tiling;
  tiling.tileSize = tileSize();
  tiling.minZoom = set.minZoom;
  tiling.maxZoom = set.maxZoom;
  tiling.roundZoom = true;
  tiling.bounds = set.bounds;
  ManagerOptions options;
  options.fadeDuration = fade_;
  options.cacheBytes = ctx_.cacheBytes;
  options.prefetchLimit = ctx_.prefetchLimit;
  manager = std::make_unique<TileManager<RasterTileData>>(id, tiling, loader_, *ctx_.scheduler, options, tileEvents());
  if (ctx_.content) ctx_.content(id);
  if (ctx_.changed) ctx_.changed();
}

void RasterSource::setFadeDuration(double ms) {
  fade_ = ms;
  if (manager) manager->options.fadeDuration = ms;
}

void RasterSource::update(Transform& transform, double now, const std::vector<Transform*>& predicted) {
  if (manager) manager->update(transform, now, predicted);
}

bool RasterSource::uploadPending(double deadline, double now) { return manager ? manager->uploadPending(deadline, now) : false; }

bool RasterSource::isIdle() const { return manager && manager->isLoaded() && !manager->hasPendingUploads(); }

std::optional<TileStats> RasterSource::stats() const {
  if (!manager) return std::nullopt;
  return manager->stats();
}

void RasterSource::reload() {
  if (manager) manager->reload();
}

void RasterSource::retryFailed() {
  if (manager) manager->retryFailed();
}

std::string RasterSource::attribution() const {
  if (spec["attribution"].isString()) return spec["attribution"].asString();
  return tileSet ? tileSet->attribution : "";
}

}
