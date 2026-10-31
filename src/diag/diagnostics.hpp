#pragma once

#include <array>
#include <cstdint>

namespace mapkern::diag {

enum class Stage { VectorTile, Image, Dem, Count };
enum class NetResult { Ok, Empty, ClientError, ServerError, Timeout, Error, Cancelled, Count };

inline constexpr int kKinds = 6;

struct Timing {
  uint64_t count = 0;
  double totalMs = 0;
  double maxMs = 0;
};

struct Totals {
  std::array<uint64_t, kKinds> requests{};
  std::array<uint64_t, static_cast<int>(NetResult::Count)> results{};
  uint64_t bytesNetwork = 0;
  uint64_t bytesCache = 0;
  uint64_t bytesLocal = 0;
  uint64_t cacheHits = 0;
  uint64_t cacheStale = 0;
  uint64_t localReads = 0;
  Timing latency;
  std::array<Timing, static_cast<int>(Stage::Count)> stages{};
  std::array<uint64_t, static_cast<int>(Stage::Count)> stageBytes{};
};

void request(int kind);
void response(NetResult result, uint64_t bytes, double ms);
void cacheServed(uint64_t bytes, bool stale);
void localServed(uint64_t bytes);
void stage(Stage s, double ms, uint64_t bytes);
Totals totals();
double steadyMs();

}
