#include "map/geojson_source.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

#include "diag/diagnostics.hpp"

namespace mapkern {

namespace {

const Rect kNowhere{std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity(),
                    -std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity()};

std::string textOf(const Bytes& bytes) { return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()); }

}

class GeoJsonTileLoader : public VectorDataLoader {
 public:
  GeoJsonTileLoader(GeoJsonSource& source, Mailbox* box, std::shared_ptr<bool> alive)
      : source_(source), box_(box), alive_(std::move(alive)) {}

  void load(TileId tile, CancelToken cancel, std::function<void()> onDecoding,
            std::function<void(LoadResult<VectorTileData>)> done) override {
    onDecoding();
    auto index = source_.index_;
    auto programs = source_.programs_;
    Mailbox* box = box_;
    auto alive = alive_;
    workerPool().submit(
        [box, alive, tile, cancel, done, index, programs] {
          if (cancel.cancelled()) {
            box->post([alive, done] {
              if (*alive) done(Aborted{});
            });
            return;
          }
          try {
            const double started = diag::steadyMs();
            std::vector<std::shared_ptr<FeatureSource>> layers{index->tile(tile.z, tile.x, tile.y)};
            VectorTileResult result = buildBuckets(layers, tile.z, tile.x, tile.y, *programs, tile.z >= index->options().maxZoom);
            diag::stage(diag::Stage::VectorTile, diag::steadyMs() - started, 0);
            auto data = vectorTileData(std::move(result), nullptr);
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
  }

 private:
  GeoJsonSource& source_;
  Mailbox* box_;
  std::shared_ptr<bool> alive_;
};

GeoJsonSource::GeoJsonSource(std::string id, Value spec, SourceContext& ctx)
    : VectorSource(std::move(id), "geojson", std::move(spec), ctx) {}

GeoJsonSource::~GeoJsonSource() = default;

GeoJsonOptions GeoJsonSource::options() const {
  GeoJsonOptions o;
  o.maxZoom = static_cast<int>(std::clamp(std::round(spec["maxZoom"].numberOr(18)), 0.0, 24.0));
  o.tolerance = std::max(0.0, spec["tolerance"].numberOr(0.375));
  o.buffer = std::clamp(spec["buffer"].numberOr(64), 0.0, 512.0);
  o.promoteId = spec["promoteId"].stringOr("");
  o.generateId = spec["generateId"].boolOr(false);
  return o;
}

void GeoJsonSource::load() {
  started_ = true;
  sync();
}

bool GeoJsonSource::isIdle() const {
  if (!started_) return false;
  return !syncing_ && (!manager || manager->isLoaded());
}

void GeoJsonSource::setData(Value data) {
  spec = withKey(spec.asObject(), "data", std::move(data));
  text_.reset();
  version_++;
  if (started_) sync();
  if (ctx_.changed) ctx_.changed();
}

void GeoJsonSource::setDataText(std::string json) {
  text_ = std::move(json);
  version_++;
  if (started_) sync();
  if (ctx_.changed) ctx_.changed();
}

void GeoJsonSource::sync() {
  if (syncing_ || applied_ == version_) return;
  syncing_ = true;
  applied_ = version_;
  auto alive = alive_;
  Mailbox* box = ctx_.mailbox;
  const std::string name = id;
  auto finish = [this, alive, box, name](std::shared_ptr<GeoJsonIndex> index, std::string problem) {
    box->post([this, alive, name, index = std::move(index), problem = std::move(problem)]() mutable {
      if (!*alive) return;
      syncing_ = false;
      if (!problem.empty() && ctx_.error)
        ctx_.error({"source-error", "GeoJSON source \"" + name + "\" could not load its data: " + problem, name});
      if (index && applied_ == version_) apply(std::move(index));
      if (applied_ != version_) sync();
      if (ctx_.changed) ctx_.changed();
    });
  };
  const GeoJsonOptions opts = options();
  auto build = [finish, opts](const Value& data) {
    try {
      finish(std::make_shared<GeoJsonIndex>(data, opts), "");
    } catch (const std::exception& e) {
      finish(nullptr, e.what());
    }
  };
  auto parseAndBuild = [finish, build](const std::string& text) {
    std::string problem;
    auto value = parseJson(text, &problem);
    if (value) build(*value);
    else finish(nullptr, problem.empty() ? "it is not valid JSON" : problem);
  };
  if (text_) {
    workerPool().submit([text = std::move(*text_), parseAndBuild] { parseAndBuild(text); });
    text_.reset();
    return;
  }
  const Value& data = spec["data"];
  if (data.isString()) {
    const RequestParameters params = resolveRequest(data.asString(), ResourceKind::Source, ctx_.transformRequest);
    fetchBytes(params, lifetime_, ctx_.requestTimeoutMs, ResourceKind::Source, ctx_.diskCache,
               [finish, parseAndBuild](FetchOutcome out) {
                 if (auto* failure = std::get_if<TileFailure>(&out)) {
                   finish(nullptr, failure->message);
                   return;
                 }
                 const FetchResult result = std::get<FetchResult>(out);
                 if (result.empty || !result.data) {
                   finish(nullptr, "there is no data at that address");
                   return;
                 }
                 workerPool().submit([parseAndBuild, data = result.data] { parseAndBuild(textOf(*data)); });
               });
    return;
  }
  if (!data.isObject()) {
    finish(nullptr, "data must be GeoJSON or a URL");
    return;
  }
  workerPool().submit([build, data = Value(data)] { build(data); });
}

void GeoJsonSource::apply(std::shared_ptr<GeoJsonIndex> index) {
  index_ = std::move(index);
  const GeoJsonOptions& o = index_->options();
  const Rect bounds = index_->coverage().value_or(kNowhere);
  if (manager) {
    manager->tiling.bounds = bounds;
    manager->refresh();
  } else {
    ctx_.scheduler->setGroupLimit(id, 16);
    geoJsonLoader_ = std::make_shared<GeoJsonTileLoader>(*this, ctx_.mailbox, alive_);
    Tiling tiling;
    tiling.tileSize = 512;
    tiling.minZoom = 0;
    tiling.maxZoom = o.maxZoom;
    tiling.bounds = bounds;
    tiling.boundsPadding = o.buffer / 512;
    ManagerOptions options;
    options.fadeDuration = 0;
    options.maxRetries = 2;
    options.cacheBytes = cacheBudget();
    options.prefetchLimit = ctx_.prefetchLimit;
    manager = std::make_unique<TileManager<VectorTileData>>(id, tiling, geoJsonLoader_, *ctx_.scheduler, options, tileEvents());
  }
  if (ctx_.content) ctx_.content(id);
}

}
