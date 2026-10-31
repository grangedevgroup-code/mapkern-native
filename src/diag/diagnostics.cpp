#include "diag/diagnostics.hpp"

#include <atomic>
#include <chrono>

namespace mapkern::diag {

namespace {

struct AtomicTiming {
  std::atomic<uint64_t> count{0};
  std::atomic<uint64_t> totalUs{0};
  std::atomic<uint64_t> maxUs{0};

  void record(double ms) {
    const uint64_t us = ms > 0 ? static_cast<uint64_t>(ms * 1000) : 0;
    count.fetch_add(1, std::memory_order_relaxed);
    totalUs.fetch_add(us, std::memory_order_relaxed);
    uint64_t seen = maxUs.load(std::memory_order_relaxed);
    while (us > seen && !maxUs.compare_exchange_weak(seen, us, std::memory_order_relaxed)) {
    }
  }

  Timing read() {
    Timing t;
    t.count = count.load(std::memory_order_relaxed);
    t.totalMs = static_cast<double>(totalUs.load(std::memory_order_relaxed)) / 1000;
    t.maxMs = static_cast<double>(maxUs.exchange(0, std::memory_order_relaxed)) / 1000;
    return t;
  }
};

struct Counters {
  std::array<std::atomic<uint64_t>, kKinds> requests{};
  std::array<std::atomic<uint64_t>, static_cast<int>(NetResult::Count)> results{};
  std::atomic<uint64_t> bytesNetwork{0}, bytesCache{0}, bytesLocal{0}, cacheHits{0}, cacheStale{0}, localReads{0};
  AtomicTiming latency;
  std::array<AtomicTiming, static_cast<int>(Stage::Count)> stages;
  std::array<std::atomic<uint64_t>, static_cast<int>(Stage::Count)> stageBytes{};
};

Counters& counters() {
  static Counters c;
  return c;
}

}

double steadyMs() {
  using namespace std::chrono;
  return static_cast<double>(duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count()) / 1000;
}

void request(int kind) {
  if (kind >= 0 && kind < kKinds) counters().requests[kind].fetch_add(1, std::memory_order_relaxed);
}

void response(NetResult result, uint64_t bytes, double ms) {
  auto& c = counters();
  c.results[static_cast<int>(result)].fetch_add(1, std::memory_order_relaxed);
  c.bytesNetwork.fetch_add(bytes, std::memory_order_relaxed);
  if (result != NetResult::Cancelled) c.latency.record(ms);
}

void cacheServed(uint64_t bytes, bool stale) {
  auto& c = counters();
  c.cacheHits.fetch_add(1, std::memory_order_relaxed);
  if (stale) c.cacheStale.fetch_add(1, std::memory_order_relaxed);
  c.bytesCache.fetch_add(bytes, std::memory_order_relaxed);
}

void localServed(uint64_t bytes) {
  auto& c = counters();
  c.localReads.fetch_add(1, std::memory_order_relaxed);
  c.bytesLocal.fetch_add(bytes, std::memory_order_relaxed);
}

void stage(Stage s, double ms, uint64_t bytes) {
  auto& c = counters();
  c.stages[static_cast<int>(s)].record(ms);
  c.stageBytes[static_cast<int>(s)].fetch_add(bytes, std::memory_order_relaxed);
}

Totals totals() {
  auto& c = counters();
  Totals t;
  for (int i = 0; i < kKinds; i++) t.requests[i] = c.requests[i].load(std::memory_order_relaxed);
  for (int i = 0; i < static_cast<int>(NetResult::Count); i++) t.results[i] = c.results[i].load(std::memory_order_relaxed);
  t.bytesNetwork = c.bytesNetwork.load(std::memory_order_relaxed);
  t.bytesCache = c.bytesCache.load(std::memory_order_relaxed);
  t.bytesLocal = c.bytesLocal.load(std::memory_order_relaxed);
  t.cacheHits = c.cacheHits.load(std::memory_order_relaxed);
  t.cacheStale = c.cacheStale.load(std::memory_order_relaxed);
  t.localReads = c.localReads.load(std::memory_order_relaxed);
  t.latency = c.latency.read();
  for (int i = 0; i < static_cast<int>(Stage::Count); i++) {
    t.stages[i] = c.stages[i].read();
    t.stageBytes[i] = c.stageBytes[i].load(std::memory_order_relaxed);
  }
  return t;
}

}
