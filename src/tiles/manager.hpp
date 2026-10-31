#pragma once

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

#include "camera/transform.hpp"
#include "core/async.hpp"
#include "geo/tile.hpp"
#include "style/layer.hpp"
#include "tiles/fetch.hpp"
#include "tiles/scheduler.hpp"

namespace mapkern {

enum class TileState { Queued, Fetching, Decoding, Prepared, Ready, Empty, Failed, Evicted };

struct Aborted {};

template <class D>
using LoadResult = std::variant<std::shared_ptr<D>, TileFailure, Aborted>;

template <class D>
class TileLoader {
 public:
  virtual ~TileLoader() = default;
  virtual void load(TileId tile, CancelToken cancel, std::function<void()> onDecoding, std::function<void(LoadResult<D>)> done) = 0;
  virtual bool needsUpload(const D& data) const = 0;
  virtual void upload(D& data) = 0;
  virtual void release(D& data) {}
  virtual size_t byteSize(const D& data) const = 0;
  virtual bool emptyIsFinal() const { return false; }
};

struct Tiling {
  double tileSize = 512;
  int minZoom = 0;
  int maxZoom = 22;
  bool roundZoom = false;
  std::optional<Rect> bounds;
  double boundsPadding = 0;
};

struct ManagerOptions {
  double fadeDuration = 0;
  std::vector<int> backstopLevels{2, 4};
  int maxRetries = 3;
  size_t cacheBytes = 192u << 20;
  double failureCooldownMs = 120000;
  int prefetchLimit = 64;
};

struct TileStats {
  size_t tiles = 0, ready = 0, loading = 0, failed = 0, empty = 0, refreshing = 0, bytes = 0;
  size_t visibleSlots = 0, pieces = 0, fallbackPieces = 0;
  uint64_t requested = 0, completed = 0, aborted = 0, retried = 0, failures = 0, evicted = 0;
  double blankPending = 0, blankFailed = 0, noData = 0;
};

struct ManagerEvents {
  std::function<void()> changed;
  std::function<void(const TileFailure&, TileId)> failure;
  std::function<void(double)> wakeAt;
};

template <class D>
struct Tile {
  Tile(int z, int x, int y) : z(z), x(x), y(y), key(tileKey(z, x, y)) {}
  const int z, x, y;
  const uint64_t key;
  TileState state = TileState::Queued;
  std::shared_ptr<D> data;
  std::shared_ptr<D> next;
  double nextAt = 0;
  std::optional<TileFailure> failure;
  int attempts = 0;
  double retryAt = 0;
  double priority = std::numeric_limits<double>::infinity();
  bool wanted = false;
  double unwantedSince = std::numeric_limits<double>::quiet_NaN();
  bool retained = false;
  uint64_t lastUsed = 0;
  double readyAt = -std::numeric_limits<double>::infinity();
  bool shown = false;
  double requestedAt = 0;
  size_t bytes = 0;
  int64_t generation = 0;
  uint64_t revision = 0;
  std::optional<CancelToken> controller;

  TileId id() const { return {z, x, y}; }
  bool isReady() const { return state == TileState::Ready; }
  bool isLoading() const {
    return state == TileState::Queued || state == TileState::Fetching || state == TileState::Decoding || state == TileState::Prepared;
  }
  bool isInFlight() const { return state == TileState::Fetching || state == TileState::Decoding; }
  bool isEvicted() const { return state == TileState::Evicted; }
  bool isSettled() const { return state == TileState::Ready || state == TileState::Empty || state == TileState::Failed; }
};

template <class D>
struct RenderPiece {
  std::shared_ptr<Tile<D>> tile;
  int wrap = 0;
  Rect clip{0, 0, 1, 1};
  double opacity = 1;
  int slotZ = 0;
};

inline constexpr double kSpeculative = 9000;

template <class D>
class TileManager {
 public:
  TileManager(std::string id, const Tiling& tiling, std::shared_ptr<TileLoader<D>> loader, RequestScheduler& scheduler, ManagerOptions options,
              ManagerEvents events)
      : id(std::move(id)), tiling(tiling), options(std::move(options)), loader_(std::move(loader)), scheduler_(scheduler),
        events_(std::move(events)) {}

  ~TileManager() { destroy(); }

  const std::string id;
  Tiling tiling;
  ManagerOptions options;
  std::vector<RenderPiece<D>> pieces;

  bool isFading() const { return fading_; }

  bool hasPendingUploads() const {
    for (const auto& [k, t] : tiles_)
      if ((t->state == TileState::Prepared && !speculative(*t)) || t->next) return true;
    return false;
  }

  bool isLoaded() const {
    for (const auto& [k, t] : tiles_) {
      if (speculative(*t)) continue;
      if (t->isLoading() || (isOutdated(*t) && !t->failure)) return false;
    }
    return true;
  }

  TileStats stats() const {
    TileStats s;
    for (const auto& [k, t] : tiles_) {
      if (t->state == TileState::Ready) s.ready++;
      else if (t->isLoading()) s.loading++;
      else if (t->state == TileState::Failed) s.failed++;
      else if (t->state == TileState::Empty) s.empty++;
      if (isOutdated(*t) || t->next) s.refreshing++;
      s.bytes += t->bytes;
    }
    const double area = viewArea_ > 0 ? viewArea_ : 1;
    s.tiles = tiles_.size();
    s.visibleSlots = visibleCount_;
    s.pieces = pieces.size();
    s.fallbackPieces = fallbackCount_;
    s.requested = counters_.requested;
    s.completed = counters_.completed;
    s.aborted = counters_.aborted;
    s.retried = counters_.retried;
    s.failures = counters_.failures;
    s.evicted = counters_.evicted;
    s.blankPending = blankPendingArea_ / area;
    s.blankFailed = blankFailedArea_ / area;
    s.noData = noDataArea_ / area;
    return s;
  }

  std::shared_ptr<Tile<D>> getTile(int z, int x, int y) const {
    auto it = tiles_.find(tileKey(z, x, y));
    return it == tiles_.end() ? nullptr : it->second;
  }

  template <class F>
  void forEachTile(F&& fn) const {
    for (const auto& [k, t] : tiles_) fn(*t);
  }

  void update(Transform& transform, double now, const std::vector<Transform*>& predicted = {}) {
    if (destroyed_) return;
    frame_++;
    for (auto& [k, t] : tiles_) {
      t->wanted = false;
      t->retained = false;
      t->priority = std::numeric_limits<double>::infinity();
    }
    CoverOptions cover;
    cover.tileSize = tiling.tileSize;
    cover.minZoom = tiling.minZoom;
    cover.maxZoom = tiling.maxZoom;
    cover.round = tiling.roundZoom;
    const auto ideal = transform.coveringTiles(cover);
    visibleCount_ = ideal.size();
    std::vector<CoveringTile> inBounds;
    for (size_t i = 0; i < ideal.size(); i++) {
      const auto& slot = ideal[i];
      if (!withinBounds(slot.z, slot.x, slot.y)) continue;
      inBounds.push_back(slot);
      want(slot.z, slot.x, slot.y, static_cast<double>(i), now);
    }
    auto levels = options.backstopLevels;
    std::sort(levels.begin(), levels.end(), std::greater<>());
    for (size_t l = 0; l < levels.size(); l++) {
      const int level = levels[l];
      if (level <= 0) continue;
      double order = 0;
      const double base = -10000.0 * static_cast<double>(levels.size() - l);
      for (const auto& slot : inBounds) {
        const int z = std::max(tiling.minZoom, slot.z - level);
        if (z >= slot.z) continue;
        const TileId p = parentTile(slot.id(), slot.z - z);
        auto existing = getTile(p.z, p.x, p.y);
        if (existing && existing->wanted) continue;
        want(p.z, p.x, p.y, base + order++, now);
      }
    }
    double climb = 0;
    for (const auto& slot : inBounds) {
      auto t = getTile(slot.z, slot.x, slot.y);
      if (!t || (t->state != TileState::Empty && t->state != TileState::Failed)) continue;
      if (t->state == TileState::Empty && loader_->emptyIsFinal()) continue;
      for (int z = slot.z - 1; z >= tiling.minZoom; z--) {
        const TileId p = parentTile(slot.id(), slot.z - z);
        auto& parent = want(p.z, p.x, p.y, -5000 + climb++, now);
        if (parent.state != TileState::Empty && parent.state != TileState::Failed) break;
      }
    }
    int prefetched = 0;
    double order = 0;
    for (Transform* future : predicted) {
      const auto ftiles = future->coveringTiles(cover);
      for (const int level : levels) {
        if (level <= 0) continue;
        for (const auto& slot : ftiles) {
          if (!withinBounds(slot.z, slot.x, slot.y)) continue;
          const int z = std::max(tiling.minZoom, slot.z - level);
          if (z >= slot.z) continue;
          const TileId p = parentTile(slot.id(), slot.z - z);
          auto existing = getTile(p.z, p.x, p.y);
          if (existing && existing->wanted) continue;
          want(p.z, p.x, p.y, 9000 + order++, now);
        }
      }
      for (size_t i = 0; i < ftiles.size() && prefetched < options.prefetchLimit; i++) {
        const auto& slot = ftiles[i];
        if (!withinBounds(slot.z, slot.x, slot.y)) continue;
        auto existing = getTile(slot.z, slot.x, slot.y);
        if (existing && existing->wanted) continue;
        want(slot.z, slot.x, slot.y, 10000 + static_cast<double>(i), now);
        prefetched++;
      }
    }
    for (auto& [k, t] : tiles_) {
      if (t->wanted) t->unwantedSince = std::numeric_limits<double>::quiet_NaN();
      else if (std::isnan(t->unwantedSince)) t->unwantedSince = now;
    }
    buildPieces(transform, ideal, now);
    dispatch(now);
    evict();
  }

  bool uploadPending(double deadline, double now) {
    std::vector<std::shared_ptr<Tile<D>>> pending;
    int swaps = 0;
    for (auto& [k, t] : tiles_) {
      if (t->state == TileState::Prepared) {
        pending.push_back(t);
      } else if (t->next) {
        swaps++;
        if (loader_->needsUpload(*t->next)) pending.push_back(t);
      }
    }
    if (pending.empty() && swaps == 0) return false;
    std::stable_sort(pending.begin(), pending.end(), [](const auto& a, const auto& b) {
      return (a->wanted ? a->priority : 1e12) < (b->wanted ? b->priority : 1e12);
    });
    bool uploaded = false, more = false;
    for (size_t i = 0; i < pending.size(); i++) {
      auto& t = pending[i];
      if (uploaded && nowMs() > deadline) {
        for (size_t j = i; j < pending.size(); j++)
          if (pending[j]->next || !speculative(*pending[j])) more = true;
        break;
      }
      if (t->next) {
        loader_->upload(*t->next);
        uploaded = true;
        continue;
      }
      if (!t->data) continue;
      loader_->upload(*t->data);
      t->bytes = loader_->byteSize(*t->data);
      t->state = TileState::Ready;
      t->readyAt = now;
      uploaded = true;
    }
    if (swaps > 0) {
      if (!more && swapDue()) swapAll();
      else more = true;
    }
    return more;
  }

  void reload() {
    auto all = tiles_;
    for (auto& [k, t] : all) drop(t);
    pieces.clear();
  }

  void forget(int z, int x, int y) {
    if (auto t = getTile(z, x, y)) drop(t);
  }

  void refreshTiles(const std::vector<TileId>& coords) {
    for (const auto& c : coords) {
      auto t = getTile(c.z, c.x, c.y);
      if (!t) continue;
      if (t->state == TileState::Ready) t->generation = -1;
      else if (t->state == TileState::Empty || t->state == TileState::Failed) t->state = TileState::Queued;
      else continue;
      t->attempts = 0;
      t->retryAt = 0;
      t->failure.reset();
    }
  }

  void refresh() {
    generation_++;
    auto all = tiles_;
    for (auto& [k, t] : all) {
      if (!t->wanted && !t->retained) {
        drop(t);
        continue;
      }
      if (t->state == TileState::Ready) {
        t->attempts = 0;
        t->retryAt = 0;
        t->failure.reset();
      } else if (t->state == TileState::Empty || (t->state == TileState::Failed && t->failure && t->failure->kind == FailureKind::Decode)) {
        t->state = TileState::Queued;
        t->attempts = 0;
        t->retryAt = 0;
        t->failure.reset();
      }
    }
  }

  void retryFailed() {
    for (auto& [k, t] : tiles_) {
      if (t->state != TileState::Failed) continue;
      t->state = TileState::Queued;
      t->attempts = 0;
      t->retryAt = 0;
      t->failure.reset();
    }
    pausedUntil_ = 0;
    consecutiveFailures_ = 0;
  }

  void destroy() {
    if (destroyed_) return;
    destroyed_ = true;
    *alive_ = false;
    auto all = tiles_;
    for (auto& [k, t] : all) drop(t);
    pieces.clear();
    scheduler_.removeGroup(id);
  }

 private:
  struct Counters {
    uint64_t requested = 0, completed = 0, aborted = 0, retried = 0, failures = 0, evicted = 0;
  };

  static bool speculative(const Tile<D>& t) { return !t.wanted || t.priority >= kSpeculative; }

  bool withinBounds(int z, int x, int y) const {
    if (!tiling.bounds) return true;
    const Rect& b = *tiling.bounds;
    const double n = std::exp2(z);
    const double pad = tiling.boundsPadding / n;
    return x / n < b.x1 + pad && (x + 1) / n > b.x0 - pad && y / n < b.y1 + pad && (y + 1) / n > b.y0 - pad;
  }

  Tile<D>& want(int z, int x, int y, double priority, double now) {
    const uint64_t key = tileKey(z, x, y);
    auto it = tiles_.find(key);
    if (it == tiles_.end()) {
      it = tiles_.emplace(key, std::make_shared<Tile<D>>(z, x, y)).first;
    } else if (it->second->state == TileState::Failed && now >= it->second->retryAt) {
      it->second->state = TileState::Queued;
      it->second->attempts = 0;
      it->second->failure.reset();
    }
    Tile<D>& t = *it->second;
    t.wanted = true;
    if (priority < t.priority) t.priority = priority;
    t.lastUsed = frame_;
    return t;
  }

  void openGate(Transform& transform, const std::vector<CoveringTile>& ideal, double now) {
    holding_ = false;
    if (transform.globeAmount() <= 0) {
      gating_ = false;
      gateSince_ = std::numeric_limits<double>::quiet_NaN();
      return;
    }
    if (!gating_) {
      gating_ = true;
      for (auto& [k, t] : tiles_)
        if (t->isReady()) t->shown = true;
    }
    bool fresh = false;
    for (auto& [k, t] : tiles_)
      if (t->isReady() && !t->shown) {
        fresh = true;
        break;
      }
    if (!fresh) {
      gateSince_ = std::numeric_limits<double>::quiet_NaN();
      return;
    }
    bool pending = false;
    for (const auto& slot : ideal) {
      if (!withinBounds(slot.z, slot.x, slot.y)) continue;
      auto t = getTile(slot.z, slot.x, slot.y);
      if (!t || !t->isSettled()) {
        pending = true;
        break;
      }
    }
    if (std::isnan(gateSince_)) gateSince_ = now;
    if (pending && now - gateSince_ < kRevealWaitMs) {
      holding_ = true;
      if (events_.wakeAt) events_.wakeAt(gateSince_ + kRevealWaitMs);
      return;
    }
    for (auto& [k, t] : tiles_) {
      if (!t->isReady() || t->shown) continue;
      t->shown = true;
      t->readyAt = std::min(t->readyAt, now - options.fadeDuration);
    }
    gateSince_ = std::numeric_limits<double>::quiet_NaN();
  }

  bool usable(const std::shared_ptr<Tile<D>>& t) const { return t && t->isReady() && (t->shown || !holding_); }

  static double slotArea(Transform& transform, int z, int x, int y, int wrap) {
    const double n = std::exp2(z);
    const double x0 = wrap + x / n, x1 = wrap + (x + 1) / n, y0 = y / n, y1 = (y + 1) / n;
    const auto a = transform.project(x0, y0);
    const auto b = transform.project(x1, y0);
    const auto c = transform.project(x1, y1);
    const auto d = transform.project(x0, y1);
    if (a.w <= 0 || b.w <= 0 || c.w <= 0 || d.w <= 0) return 0;
    const double s = a.x * b.y - b.x * a.y + (b.x * c.y - c.x * b.y) + (c.x * d.y - d.x * c.y) + (d.x * a.y - a.x * d.y);
    return std::abs(s) / 2;
  }

  void buildPieces(Transform& transform, const std::vector<CoveringTile>& ideal, double now) {
    openGate(transform, ideal, now);
    std::vector<RenderPiece<D>> out;
    fading_ = false;
    fallbackCount_ = 0;
    blankPendingArea_ = blankFailedArea_ = noDataArea_ = viewArea_ = 0;
    const Rect full{0, 0, 1, 1};
    for (const auto& slot : ideal) {
      const double area = slotArea(transform, slot.z, slot.x, slot.y, slot.wrap);
      viewArea_ += area;
      if (!withinBounds(slot.z, slot.x, slot.y)) {
        noDataArea_ += area;
        continue;
      }
      auto t = getTile(slot.z, slot.x, slot.y);
      if (usable(t)) {
        const double age = now - t->readyAt;
        if (options.fadeDuration > 0 && age < options.fadeDuration) {
          fallback(out, slot.id(), slot.wrap, slot.z, 0);
          out.push_back(piece(t, slot.wrap, full, std::max(0.0, age / options.fadeDuration), slot.z));
          fading_ = true;
        } else {
          out.push_back(piece(t, slot.wrap, full, 1, slot.z));
        }
        continue;
      }
      if (t && t->state == TileState::Empty && loader_->emptyIsFinal()) {
        noDataArea_ += area;
        continue;
      }
      const size_t start = out.size();
      const size_t counted = fallbackCount_;
      bool covered = fallback(out, slot.id(), slot.wrap, slot.z, 0);
      if (!covered && holding_) {
        out.resize(start);
        fallbackCount_ = counted;
        holding_ = false;
        covered = fallback(out, slot.id(), slot.wrap, slot.z, 0);
        holding_ = true;
        for (size_t i = start; i < out.size(); i++) out[i].tile->shown = true;
      }
      if (!covered && t && t->isReady()) {
        t->shown = true;
        out.push_back(piece(t, slot.wrap, full, 1, slot.z));
        covered = true;
      }
      if (!covered) {
        if (t && t->state == TileState::Empty) noDataArea_ += area;
        else if (t && t->state == TileState::Failed) blankFailedArea_ += area;
        else blankPendingArea_ += area;
      }
    }
    pieces = std::move(out);
  }

  bool fallback(std::vector<RenderPiece<D>>& out, TileId region, int wrap, int slotZ, int depth) {
    const Rect full{0, 0, 1, 1};
    if (depth < kMaxFallbackDepth && region.z + 1 <= tiling.maxZoom) {
      const TileId kids[4] = {{region.z + 1, region.x * 2, region.y * 2},
                              {region.z + 1, region.x * 2 + 1, region.y * 2},
                              {region.z + 1, region.x * 2, region.y * 2 + 1},
                              {region.z + 1, region.x * 2 + 1, region.y * 2 + 1}};
      bool anyReady = false;
      for (const auto& kid : kids)
        if (hasReadyDescendant(kid, kMaxFallbackDepth - depth - 1)) {
          anyReady = true;
          break;
        }
      if (anyReady) {
        bool all = true;
        for (const auto& kid : kids) {
          auto t = getTile(kid.z, kid.x, kid.y);
          if (usable(t)) {
            out.push_back(piece(t, wrap, full, 1, slotZ));
            fallbackCount_++;
          } else if (!fallback(out, kid, wrap, slotZ, depth + 1)) {
            all = false;
          }
        }
        return all;
      }
    }
    for (int z = region.z - 1; z >= tiling.minZoom; z--) {
      const TileId a = parentTile(region, region.z - z);
      auto t = getTile(a.z, a.x, a.y);
      if (usable(t)) {
        out.push_back(piece(t, wrap, subRect(a, region), 1, slotZ));
        fallbackCount_++;
        return true;
      }
    }
    return false;
  }

  bool hasReadyDescendant(TileId t, int levels) const {
    if (usable(getTile(t.z, t.x, t.y))) return true;
    if (levels <= 0 || t.z + 1 > tiling.maxZoom) return false;
    for (int dy = 0; dy < 2; dy++)
      for (int dx = 0; dx < 2; dx++)
        if (hasReadyDescendant({t.z + 1, t.x * 2 + dx, t.y * 2 + dy}, levels - 1)) return true;
    return false;
  }

  RenderPiece<D> piece(const std::shared_ptr<Tile<D>>& t, int wrap, Rect clip, double opacity, int slotZ) {
    t->retained = true;
    t->lastUsed = frame_;
    return {t, wrap, clip, opacity, slotZ};
  }

  bool needsJob(const Tile<D>& t) const {
    if (!t.wanted || jobs_.contains(t.key)) return false;
    return t.state == TileState::Queued || (isOutdated(t) && !t.next);
  }

  bool isOutdated(const Tile<D>& t) const { return t.state == TileState::Ready && t.generation < generation_; }

  void dispatch(double now) {
    bool waiting = false;
    for (auto it = jobs_.begin(); it != jobs_.end();) {
      auto t = getTile(tileFromKey(it->first).z, tileFromKey(it->first).x, tileFromKey(it->first).y);
      if (t && t->wanted && it->second->state == JobState::Queued) waiting = true;
      if (!t && it->second->state == JobState::Queued) {
        scheduler_.cancel(it->second);
        it = jobs_.erase(it);
        continue;
      }
      ++it;
    }
    for (auto& [k, t] : tiles_)
      if (needsJob(*t) && t->retryAt <= now) waiting = true;
    std::vector<std::shared_ptr<Tile<D>>> toDrop;
    for (auto it = jobs_.begin(); it != jobs_.end();) {
      auto tileIt = tiles_.find(it->first);
      if (tileIt == tiles_.end()) {
        ++it;
        continue;
      }
      auto& t = tileIt->second;
      if (!t->wanted) {
        const bool keep = t->state == TileState::Ready;
        if (it->second->state == JobState::Queued) {
          scheduler_.cancel(it->second);
          it = jobs_.erase(it);
          counters_.aborted++;
          if (!keep) toDrop.push_back(t);
          continue;
        }
        if (it->second->state == JobState::Active && t->controller) {
          const double lingering = now - t->unwantedSince;
          if (lingering > kLingerMs || (waiting && lingering > 150)) {
            t->controller->cancel();
            counters_.aborted++;
            if (!keep) toDrop.push_back(t);
          }
        }
      } else {
        it->second->priority = t->priority;
      }
      ++it;
    }
    for (auto& t : toDrop) drop(t);
    if (now < pausedUntil_) {
      if (events_.wakeAt) events_.wakeAt(pausedUntil_);
      return;
    }
    double nextWake = std::numeric_limits<double>::infinity();
    std::vector<std::shared_ptr<Tile<D>>> start;
    for (auto& [k, t] : tiles_) {
      if (!needsJob(*t)) continue;
      if (t->retryAt > now) {
        nextWake = std::min(nextWake, t->retryAt);
        continue;
      }
      start.push_back(t);
    }
    for (auto& t : start) startJob(t);
    if (std::isfinite(nextWake) && events_.wakeAt) events_.wakeAt(nextWake);
    scheduler_.pump();
  }

  void startJob(const std::shared_ptr<Tile<D>>& t) {
    std::weak_ptr<Tile<D>> weak = t;
    auto handle = scheduler_.enqueue(id, t->priority, [this, weak](std::function<void()> done) {
      auto tile = weak.lock();
      if (!tile) {
        done();
        return;
      }
      load(tile, std::move(done));
    });
    jobs_[t->key] = handle;
  }

  void load(const std::shared_ptr<Tile<D>>& t, std::function<void()> done) {
    const bool refreshing = isOutdated(*t);
    if (destroyed_ || (t->state != TileState::Queued && !refreshing)) {
      jobs_.erase(t->key);
      done();
      return;
    }
    const int64_t generation = generation_;
    CancelToken controller;
    t->controller = controller;
    t->attempts++;
    t->requestedAt = nowMs();
    if (!refreshing) t->state = TileState::Fetching;
    counters_.requested++;
    if (t->attempts > 1) counters_.retried++;
    auto alive = alive_;
    std::weak_ptr<Tile<D>> weak = t;
    loader_->load(
        t->id(), controller,
        [weak, alive]() {
          if (!*alive) return;
          if (auto tile = weak.lock())
            if (tile->state == TileState::Fetching) tile->state = TileState::Decoding;
        },
        [this, weak, alive, controller, generation, refreshing, done = std::move(done)](LoadResult<D> result) {
          if (!*alive) return;
          auto tile = weak.lock();
          finishLoad(tile, controller, generation, refreshing, std::move(result));
          if (tile) {
            if (auto it = jobs_.find(tile->key); it != jobs_.end() && it->second->state == JobState::Active) jobs_.erase(it);
          }
          done();
          scheduler_.pump();
        });
  }

  void finishLoad(const std::shared_ptr<Tile<D>>& t, const CancelToken& controller, int64_t generation, bool refreshing,
                  LoadResult<D> result) {
    if (!t) {
      if (auto* data = std::get_if<std::shared_ptr<D>>(&result); data && *data) loader_->release(**data);
      return;
    }
    if (controller.cancelled() || t->isEvicted() || destroyed_) {
      if (auto* data = std::get_if<std::shared_ptr<D>>(&result); data && *data) loader_->release(**data);
      t->controller.reset();
      return;
    }
    t->controller.reset();
    if (std::holds_alternative<Aborted>(result)) {
      if (t->isInFlight()) t->state = TileState::Queued;
      return;
    }
    if (auto* data = std::get_if<std::shared_ptr<D>>(&result)) {
      consecutiveFailures_ = 0;
      counters_.completed++;
      t->failure.reset();
      t->generation = generation;
      if (refreshing) {
        replace(*t, *data);
      } else if (!*data) {
        t->state = TileState::Empty;
      } else {
        t->data = *data;
        t->revision++;
        t->bytes = loader_->byteSize(**data);
        t->state = TileState::Prepared;
        if (!loader_->needsUpload(**data)) {
          t->state = TileState::Ready;
          t->readyAt = nowMs();
        }
      }
    } else {
      const TileFailure failure = std::get<TileFailure>(result);
      counters_.failures++;
      consecutiveFailures_++;
      t->failure = failure;
      const double now = nowMs();
      const bool retry = failure.retryable && t->attempts <= options.maxRetries;
      if (refreshing) {
        t->retryAt = now + (retry ? retryDelay(t->attempts, failure) : options.failureCooldownMs);
      } else if (retry) {
        t->state = TileState::Queued;
        t->retryAt = now + retryDelay(t->attempts, failure);
      } else {
        t->state = TileState::Failed;
        t->retryAt = now + options.failureCooldownMs;
      }
      if (consecutiveFailures_ >= kBreakerThreshold) {
        const int level = consecutiveFailures_ / kBreakerThreshold;
        pausedUntil_ = now + std::min(60000.0, 2000 * std::exp2(level - 1));
      }
      if (events_.failure) events_.failure(failure, t->id());
    }
    if (!destroyed_ && !speculative(*t) && events_.changed) events_.changed();
  }

  bool swapDue() const {
    double oldest = std::numeric_limits<double>::infinity();
    for (const auto& [k, t] : tiles_) {
      if (!t->next) continue;
      if (loader_->needsUpload(*t->next)) return false;
      oldest = std::min(oldest, t->nextAt);
    }
    if (!std::isfinite(oldest)) return false;
    if (nowMs() - oldest >= kSwapWaitMs) return true;
    for (const auto& [k, t] : tiles_) {
      if (t->next || !t->wanted || t->priority >= 10000) continue;
      if (isOutdated(*t) && !t->failure) return false;
    }
    return true;
  }

  void swapAll() {
    for (auto& [k, t] : tiles_) {
      if (!t->next) continue;
      auto next = std::move(t->next);
      t->next = nullptr;
      auto previous = std::move(t->data);
      t->data = next;
      t->revision++;
      if (previous) loader_->release(*previous);
      t->bytes = loader_->byteSize(*next);
    }
  }

  void replace(Tile<D>& t, const std::shared_ptr<D>& data) {
    if (!data) {
      if (t.data) loader_->release(*t.data);
      t.data = nullptr;
      t.revision++;
      t.bytes = 0;
      t.state = TileState::Empty;
      return;
    }
    if (loader_->needsUpload(*data)) {
      t.next = data;
      t.nextAt = nowMs();
      t.bytes = (t.data ? loader_->byteSize(*t.data) : 0) + loader_->byteSize(*data);
      return;
    }
    auto previous = std::move(t.data);
    t.data = data;
    t.revision++;
    if (previous) loader_->release(*previous);
    t.bytes = loader_->byteSize(*data);
  }

  void drop(std::shared_ptr<Tile<D>> t) {
    if (auto it = jobs_.find(t->key); it != jobs_.end()) {
      scheduler_.cancel(it->second);
      jobs_.erase(it);
    }
    if (t->controller) {
      t->controller->cancel();
      t->controller.reset();
    }
    if (t->next) {
      loader_->release(*t->next);
      t->next = nullptr;
    }
    if (t->data) {
      loader_->release(*t->data);
      t->data = nullptr;
    }
    t->bytes = 0;
    t->state = TileState::Evicted;
    tiles_.erase(t->key);
  }

  void evict() {
    size_t bytes = 0;
    for (const auto& [k, t] : tiles_) bytes += t->bytes;
    const size_t budget = options.cacheBytes;
    const size_t maxEntries = 4096;
    if (bytes <= budget && tiles_.size() <= maxEntries) return;
    std::vector<std::shared_ptr<Tile<D>>> candidates;
    for (const auto& [k, t] : tiles_) {
      if (t->retained || t->wanted || jobs_.contains(t->key)) continue;
      candidates.push_back(t);
    }
    std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) {
      return a->lastUsed != b->lastUsed ? a->lastUsed < b->lastUsed : a->z > b->z;
    });
    for (auto& t : candidates) {
      if (bytes <= budget && tiles_.size() <= maxEntries) break;
      bytes -= t->bytes;
      drop(t);
      counters_.evicted++;
    }
  }

  static constexpr double kLingerMs = 2500;
  static constexpr int kMaxFallbackDepth = 2;
  static constexpr double kRevealWaitMs = 1200;
  static constexpr double kSwapWaitMs = 100;
  static constexpr int kBreakerThreshold = 16;

  std::shared_ptr<TileLoader<D>> loader_;
  RequestScheduler& scheduler_;
  ManagerEvents events_;
  std::unordered_map<uint64_t, std::shared_ptr<Tile<D>>> tiles_;
  std::unordered_map<uint64_t, std::shared_ptr<JobHandle>> jobs_;
  std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
  uint64_t frame_ = 0;
  int64_t generation_ = 0;
  bool fading_ = false;
  bool holding_ = false;
  double gateSince_ = std::numeric_limits<double>::quiet_NaN();
  bool gating_ = false;
  int consecutiveFailures_ = 0;
  double pausedUntil_ = 0;
  bool destroyed_ = false;
  size_t visibleCount_ = 0;
  size_t fallbackCount_ = 0;
  double blankPendingArea_ = 0, blankFailedArea_ = 0, noDataArea_ = 0, viewArea_ = 0;
  Counters counters_;

 public:
  void markDead() { *alive_ = false; }
};

}
