#include "map/dem_source.hpp"

#include <cmath>
#include <cstring>

#include "diag/diagnostics.hpp"
#include "formats/image.hpp"

namespace mapkern {

namespace {

size_t demBytes(const DemTileData& d) {
  const size_t size = static_cast<size_t>(d.grid.size);
  size_t cpu = d.grid.elevation.size() * 4 + d.grid.mask.size() + d.gradient.size() * 2;
  size_t gpu = (d.gradientTexture.valid() ? (size + 2) * (size + 2) * 4 : 0) + (d.maskTexture.valid() ? size * size : 0) +
               (d.elevationTexture.valid() ? d.grid.elevation.size() * 4 : 0);
  return cpu + gpu;
}

std::optional<DemGrid> decodeBytes(const Bytes& bytes, const Unpack& unpack, std::string& error) {
  if (isRawElevation(bytes.data(), bytes.size())) {
    uint32_t size = 0;
    std::memcpy(&size, bytes.data() + 4, 4);
    if (bytes.size() < 8 + static_cast<size_t>(size) * size * 4) {
      error = "Elevation tile is shorter than its size says";
      return std::nullopt;
    }
    std::vector<float> values(static_cast<size_t>(size) * size);
    std::memcpy(values.data(), bytes.data() + 8, values.size() * 4);
    return decodeElevations(values.data(), values.size(), static_cast<int>(size));
  }
  auto image = decodeImage(bytes, false, &error);
  if (!image) return std::nullopt;
  if (image->width != image->height || image->width < 2) {
    error = "Elevation tiles must be square, this one is " + std::to_string(image->width) + "x" + std::to_string(image->height);
    return std::nullopt;
  }
  const double started = diag::steadyMs();
  auto grid = decodeDem(image->rgba.data(), image->width, unpack, false);
  diag::stage(diag::Stage::Dem, diag::steadyMs() - started, bytes.size());
  return grid;
}

class DemTileLoader : public TileLoader<DemTileData> {
 public:
  DemTileLoader(SourceContext& ctx, TileSet set, Unpack unpack, std::shared_ptr<bool> alive, DemSource* owner)
      : ctx_(ctx), set_(std::move(set)), unpack_(unpack), alive_(std::move(alive)), owner_(owner) {}

  void load(TileId tile, CancelToken cancel, std::function<void()> onDecoding, std::function<void(LoadResult<DemTileData>)> done) override {
    const RequestParameters params = resolveRequest(tileUrl(set_, tile.z, tile.x, tile.y), ResourceKind::Tile, ctx_.transformRequest);
    Mailbox* box = ctx_.mailbox;
    auto alive = alive_;
    const Unpack unpack = unpack_;
    fetchBytes(params, cancel, ctx_.requestTimeoutMs, ResourceKind::Tile, ctx_.diskCache,
               [box, alive, tile, cancel, onDecoding, done, unpack](FetchOutcome out) {
                 auto post = [box, alive, done](LoadResult<DemTileData> r) {
                   box->post([alive, done, r = std::move(r)]() mutable {
                     if (*alive) done(std::move(r));
                   });
                 };
                 if (cancel.cancelled()) return post(Aborted{});
                 if (auto* f = std::get_if<TileFailure>(&out)) return post(*f);
                 FetchResult r = std::get<FetchResult>(out);
                 if (r.empty || !r.data) return post(std::shared_ptr<DemTileData>());
                 box->post([alive, onDecoding] {
                   if (*alive) onDecoding();
                 });
                 workerPool().submit(
                     [post, tile, cancel, r, unpack] {
                       if (cancel.cancelled()) return post(Aborted{});
                       std::string error;
                       auto grid = decodeBytes(*r.data, unpack, error);
                       if (!grid) {
                         if (error.empty()) return post(std::shared_ptr<DemTileData>());
                         TileFailure f;
                         f.kind = FailureKind::Decode;
                         f.message = "Elevation tile could not be decoded: " + error;
                         return post(f);
                       }
                       auto data = std::make_shared<DemTileData>();
                       data->z = tile.z;
                       data->x = tile.x;
                       data->y = tile.y;
                       data->grid = std::move(*grid);
                       data->gradient = gradients(data->grid);
                       post(data);
                     },
                     tile.z);
               });
  }

  bool needsUpload(const DemTileData& data) const override { return !data.gradientTexture.valid(); }

  void upload(DemTileData& data) override {
    if (*alive_) owner_->stitch(data);
    writeFrame(data.grid, data.gradient);
    const int n = data.grid.size + 2;
    data.gradientTexture = Gpu::get().texture(n, n, SG_PIXELFORMAT_RG16F, data.gradient.data(), data.gradient.size() * 2);
    if (!data.grid.mask.empty())
      data.maskTexture = Gpu::get().texture(data.grid.size, data.grid.size, SG_PIXELFORMAT_R8, data.grid.mask.data(), data.grid.mask.size());
    data.gradientDirty = false;
    data.elevationDirty = false;
  }

  void release(DemTileData& data) override {
    data.gradientTexture.release();
    data.maskTexture.release();
    data.elevationTexture.release();
  }

  size_t byteSize(const DemTileData& data) const override { return demBytes(data); }

 private:
  SourceContext& ctx_;
  TileSet set_;
  Unpack unpack_;
  std::shared_ptr<bool> alive_;
  DemSource* owner_;
};

}

DemSource::DemSource(std::string id, Value spec, SourceContext& ctx) : Source(std::move(id), "dem", std::move(spec), ctx) {
  unpack_ = unpackFor(this->spec.has("encoding") ? this->spec["encoding"] : this->spec["defaultEncoding"]);
}

DemSource::~DemSource() {
  *alive_ = false;
  lifetime_.cancel();
  if (manager) manager->destroy();
}

double DemSource::tileSize() const {
  const double size = spec["tileSize"].numberOr(512);
  return size > 0 ? size : 512;
}

void DemSource::load() {
  auto alive = alive_;
  Mailbox* box = ctx_.mailbox;
  resolveTileSet(id, spec, {0, 15}, ctx_.transformRequest, ctx_.requestTimeoutMs, ctx_.diskCache, lifetime_,
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

void DemSource::start(TileSet set) {
  tileSet = set;
  const std::string named = set.raw["encoding"].stringOr("");
  if (spec.has("encoding")) unpack_ = unpackFor(spec["encoding"]);
  else if (named == "terrarium" || named == "mapbox") unpack_ = unpackFor(Value(named));
  else unpack_ = unpackFor(spec["defaultEncoding"]);
  ctx_.scheduler->setGroupLimit(id, 12);
  if (manager) manager->destroy();
  loader_ = std::make_shared<DemTileLoader>(ctx_, set, unpack_, alive_, this);
  Tiling tiling;
  tiling.tileSize = tileSize();
  tiling.minZoom = set.minZoom;
  tiling.maxZoom = set.maxZoom;
  tiling.roundZoom = true;
  tiling.bounds = set.bounds;
  ManagerOptions options;
  options.fadeDuration = ctx_.fadeDuration;
  options.backstopLevels = {2, 4};
  options.maxRetries = 3;
  options.cacheBytes = cacheBudget();
  options.failureCooldownMs = 120000;
  options.prefetchLimit = ctx_.prefetchLimit;
  manager = std::make_unique<TileManager<DemTileData>>(id, tiling, loader_, *ctx_.scheduler, options, tileEvents());
  if (ctx_.content) ctx_.content(id);
  if (ctx_.changed) ctx_.changed();
}

void DemSource::stitch(DemTileData& data) {
  if (!manager) return;
  const int n = 1 << data.z;
  for (const auto& [dx, dy] : kNeighbors) {
    const int y = data.y + dy;
    if (y < 0 || y >= n) continue;
    const int x = ((data.x + dx) % n + n) % n;
    auto tile = manager->getTile(data.z, x, y);
    if (!tile || !tile->data) continue;
    DemTileData& other = *tile->data;
    if (&other == &data || !backfill(data.grid, other.grid, dx, dy)) continue;
    backfill(other.grid, data.grid, -dx, -dy);
    if (!other.gradient.empty()) writeFrame(other.grid, other.gradient);
    const bool queued = other.elevationDirty || other.gradientDirty;
    other.elevationDirty = other.elevationTexture.valid();
    other.gradientDirty = other.gradientTexture.valid();
    if (!queued && (other.elevationDirty || other.gradientDirty)) stale_.push_back({other.z, other.x, other.y});
  }
}

void DemSource::resize(DemTileData& data) {
  if (!manager) return;
  if (auto tile = manager->getTile(data.z, data.x, data.y); tile && tile->data.get() == &data) tile->bytes = demBytes(data);
}

bool DemSource::refresh(DemTileData& data) {
  bool uploaded = false;
  if (data.elevationDirty && data.elevationTexture.valid()) {
    const int stride = data.grid.stride;
    data.elevationTexture.release();
    data.elevationTexture = Gpu::get().texture(stride, stride, SG_PIXELFORMAT_R32F, data.grid.elevation.data(), data.grid.elevation.size() * 4);
    uploaded = true;
  }
  if (data.gradientDirty && data.gradientTexture.valid()) {
    const int n = data.grid.size + 2;
    data.gradientTexture.release();
    data.gradientTexture = Gpu::get().texture(n, n, SG_PIXELFORMAT_RG16F, data.gradient.data(), data.gradient.size() * 2);
    uploaded = true;
  }
  data.elevationDirty = false;
  data.gradientDirty = false;
  if (uploaded) {
    data.version++;
    resize(data);
  }
  return uploaded;
}

const GpuTexture* DemSource::elevationTexture(DemTileData& data) {
  if (!data.elevationTexture.valid()) {
    const int stride = data.grid.stride;
    data.elevationTexture = Gpu::get().texture(stride, stride, SG_PIXELFORMAT_R32F, data.grid.elevation.data(), data.grid.elevation.size() * 4);
    data.elevationDirty = false;
    resize(data);
  }
  return &data.elevationTexture;
}

const GpuTexture* DemSource::gradientTexture(DemTileData& data) { return data.gradientTexture.valid() ? &data.gradientTexture : nullptr; }

std::optional<double> DemSource::elevationAt(double wx, double wy) const {
  if (!manager) return std::nullopt;
  const double x = wx - std::floor(wx);
  if (wy < 0 || wy > 1) return std::nullopt;
  for (int z = manager->tiling.maxZoom; z >= manager->tiling.minZoom; z--) {
    const int n = 1 << z;
    const int tx = std::min(n - 1, static_cast<int>(std::floor(x * n)));
    const int ty = std::min(n - 1, static_cast<int>(std::floor(wy * n)));
    auto tile = manager->getTile(z, tx, ty);
    if (tile && tile->isReady() && tile->data) return sampleElevation(tile->data->grid, x * n - tx, wy * n - ty);
  }
  return std::nullopt;
}

void DemSource::update(Transform& transform, double now, const std::vector<Transform*>& predicted) {
  if (manager) manager->update(transform, now, predicted);
}

bool DemSource::uploadPending(UploadBudget& budget, double now) {
  if (!manager) return false;
  const bool more = manager->uploadPending(budget, now);
  size_t done = 0;
  for (; done < stale_.size(); done++) {
    if (budget.spent()) break;
    const TileId at = stale_[done];
    auto tile = manager->getTile(at.z, at.x, at.y);
    if (tile && tile->data && refresh(*tile->data)) budget.used = true;
  }
  stale_.erase(stale_.begin(), stale_.begin() + static_cast<std::ptrdiff_t>(done));
  return more || !stale_.empty();
}

bool DemSource::isIdle() const { return manager && manager->isLoaded() && !manager->hasPendingUploads() && stale_.empty(); }

std::optional<TileStats> DemSource::stats() const {
  if (!manager) return std::nullopt;
  return manager->stats();
}

void DemSource::reload() {
  if (manager) manager->reload();
}

void DemSource::retryFailed() {
  if (manager) manager->retryFailed();
}

std::string DemSource::attribution() const {
  if (spec["attribution"].isString()) return spec["attribution"].asString();
  return tileSet ? tileSet->attribution : "";
}

}
