#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "camera/transform.hpp"
#include "core/async.hpp"
#include "core/value.hpp"
#include "style/layer.hpp"
#include "tiles/cache.hpp"
#include "tiles/fetch.hpp"
#include "tiles/manager.hpp"
#include "tiles/scheduler.hpp"

namespace mapkern {

struct MapError {
  std::string code;
  std::string message;
  std::string sourceId;
};

struct SourceContext {
  RequestScheduler* scheduler = nullptr;
  Mailbox* mailbox = nullptr;
  TransformRequest transformRequest;
  int requestTimeoutMs = 30000;
  std::shared_ptr<ResponseStore> diskCache;
  size_t cacheBytes = 192u << 20;
  double fadeDuration = 0;
  int prefetchLimit = 64;
  std::function<double()> pixelRatio;
  std::function<void(const MapError&)> error;
  std::function<void(const std::string&)> content;
  std::function<void()> changed;
  std::function<void(double)> wakeAt;
};

class Source {
 public:
  Source(std::string id, std::string type, Value spec, SourceContext& ctx) : id(std::move(id)), type(std::move(type)), spec(std::move(spec)), ctx_(ctx) {}
  virtual ~Source() = default;
  const std::string id;
  const std::string type;
  Value spec;
  virtual std::string attribution() const { return spec["attribution"].stringOr(""); }
  virtual bool loaded() const = 0;
  virtual void load() = 0;
  virtual void update(Transform& transform, double now, const std::vector<Transform*>& predicted) = 0;
  virtual bool uploadPending(UploadBudget& budget, double now) = 0;
  virtual bool isIdle() const = 0;
  virtual bool isFading() const { return false; }
  virtual std::optional<TileStats> stats() const { return std::nullopt; }
  virtual void reload() = 0;
  virtual void setLayers(const std::vector<const StyleLayer*>&) {}
  virtual bool adopt(const Value&) { return false; }
  virtual void retryFailed() {}
  virtual void setCacheBytes(size_t bytes) { cacheBytes_ = bytes; }

 protected:
  ManagerEvents tileEvents();
  size_t cacheBudget() const { return cacheBytes_ ? cacheBytes_ : ctx_.cacheBytes; }
  size_t cacheBytes_ = 0;
  SourceContext& ctx_;
  std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

}
