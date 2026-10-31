#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "formats/bytes.hpp"

namespace mapkern {

struct StoredResponse {
  SharedBytes data;
  std::string type;
};

enum class StoredState { Fresh, Stale, Missing, Unknown };

class ResponseStore {
 public:
  virtual ~ResponseStore() = default;
  virtual StoredState state(const std::string& url) = 0;
  virtual std::optional<StoredResponse> read(const std::string& url) = 0;
  virtual void write(const std::string& url, SharedBytes data, const std::string& type, double expires, bool pinned = false) = 0;
};

struct HeaderLookup {
  std::function<std::string(std::string_view)> get;
};

std::optional<double> freshUntil(const HeaderLookup& headers, double now);
bool cacheable(const std::string& url);
bool offline();
void setOffline(bool offline);
bool serverDown(const std::string& url, double now);
void markServer(const std::string& url, bool ok, double now);
double wallMs();

struct CacheStats {
  uint64_t entries = 0;
  uint64_t bytes = 0;
  uint64_t pinnedEntries = 0;
  uint64_t pinnedBytes = 0;
  uint64_t maxBytes = 0;
};

class DiskCache : public ResponseStore {
 public:
  DiskCache(std::string directory, uint64_t maxBytes);
  ~DiskCache() override;

  StoredState state(const std::string& url) override;
  std::optional<StoredResponse> read(const std::string& url) override;
  void write(const std::string& url, SharedBytes data, const std::string& type, double expires, bool pinned = false) override;
  void pin(const std::string& url, bool pinned);
  bool contains(const std::string& url);
  void clear(bool keepPinned = false);
  void flush();
  CacheStats stats();
  void setMaxBytes(uint64_t bytes);
  const std::string& directory() const { return dir_; }
  bool loaded() const { return loaded_.load(); }
  void waitLoaded();

 private:
  struct Row {
    uint64_t size = 0;
    double used = 0;
    double expires = 0;
    bool pinned = false;
  };
  struct Pending {
    SharedBytes data;
    std::string type;
    double expires = 0;
    bool pinned = false;
  };
  std::string pathFor(const std::string& url) const;
  void load();
  void writer();
  void evictLocked();
  bool writeEntry(const std::string& url, const Pending& p);

  std::string dir_;
  std::atomic<uint64_t> maxBytes_;
  std::mutex mutex_;
  std::condition_variable cv_;
  std::unordered_map<std::string, Row> entries_;
  std::unordered_map<std::string, Pending> pending_;
  std::unordered_map<std::string, bool> pinChanges_;
  std::vector<std::string> touched_;
  uint64_t total_ = 0;
  std::atomic<bool> loaded_{false};
  bool broken_ = false;
  bool stop_ = false;
  std::thread loader_;
  std::thread writer_;
};

std::shared_ptr<DiskCache> sharedDiskCache(const std::string& directory, uint64_t maxBytes);
std::string defaultCacheDirectory();
void setDefaultCacheDirectory(std::string directory);

}
