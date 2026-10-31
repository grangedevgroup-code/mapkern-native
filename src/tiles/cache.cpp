#include "tiles/cache.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <ctime>
#include <filesystem>

#include "core/url.hpp"
#include "net/http.hpp"

namespace fs = std::filesystem;

namespace mapkern {

namespace {

constexpr double kDownMs = 30000;
constexpr double kTouchMs = 60 * 60 * 1000;
constexpr char kMagic[4] = {'M', 'K', 'C', '1'};

std::mutex downMutex;
std::unordered_map<std::string, double> down;
std::atomic<bool> forcedOffline{false};

std::mutex dirMutex;
std::string configuredDir;

uint64_t fnv1a(const std::string& s) {
  uint64_t h = 1469598103934665603ull;
  for (unsigned char c : s) {
    h ^= c;
    h *= 1099511628211ull;
  }
  return h;
}

std::string lower(std::string s) {
  for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::optional<double> parseHttpDate(const std::string& s) {
  static const char* months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  char wd[8] = {}, mon[8] = {};
  int day = 0, year = 0, h = 0, m = 0, sec = 0;
  if (std::sscanf(s.c_str(), "%3s, %d %3s %d %d:%d:%d", wd, &day, mon, &year, &h, &m, &sec) != 7) return std::nullopt;
  int month = -1;
  for (int i = 0; i < 12; i++)
    if (std::strncmp(mon, months[i], 3) == 0) month = i + 1;
  if (month < 0) return std::nullopt;
  int y = year - (month <= 2);
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  const double days = era * 146097.0 + doe - 719468;
  return (days * 86400 + h * 3600 + m * 60 + sec) * 1000;
}

void putU32(std::string& out, uint32_t v) {
  for (int i = 0; i < 4; i++) out += static_cast<char>((v >> (8 * i)) & 0xff);
}

uint32_t getU32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }

double fileTimeMs(fs::file_time_type t) {
  using namespace std::chrono;
  const auto sys = system_clock::now() + duration_cast<system_clock::duration>(t - fs::file_time_type::clock::now());
  return static_cast<double>(duration_cast<milliseconds>(sys.time_since_epoch()).count());
}

}

double wallMs() {
  using namespace std::chrono;
  return static_cast<double>(duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
}

std::optional<double> freshUntil(const HeaderLookup& headers, double now) {
  const std::string control = lower(headers.get("cache-control"));
  auto has = [&](const char* token) {
    size_t pos = 0;
    while ((pos = control.find(token, pos)) != std::string::npos) {
      const bool left = pos == 0 || control[pos - 1] == ',' || control[pos - 1] == ' ';
      const size_t end = pos + std::strlen(token);
      const bool right = end >= control.size() || (!std::isalnum(static_cast<unsigned char>(control[end])) && control[end] != '-');
      if (left && right) return true;
      pos = end;
    }
    return false;
  };
  if (has("no-store")) return std::nullopt;
  if (has("no-cache")) return 0.0;
  const size_t ma = control.find("max-age");
  if (ma != std::string::npos) {
    size_t p = ma + 7;
    while (p < control.size() && (control[p] == ' ' || control[p] == '=' || control[p] == '"')) p++;
    if (p < control.size() && std::isdigit(static_cast<unsigned char>(control[p]))) {
      const double maxAge = std::strtod(control.c_str() + p, nullptr);
      const double age = std::strtod(headers.get("age").c_str(), nullptr);
      return now + std::max(0.0, maxAge - age) * 1000;
    }
  }
  const std::string expires = headers.get("expires");
  if (!expires.empty()) return parseHttpDate(expires).value_or(0);
  return 0.0;
}

bool cacheable(const std::string& url) {
  const UrlParts p = parseUrl(url);
  if (p.scheme != "http" && p.scheme != "https") return false;
  std::string host = lower(p.host);
  if (const size_t colon = host.rfind(':'); colon != std::string::npos && host.find(']') == std::string::npos) host = host.substr(0, colon);
  return !(host == "localhost" || host == "127.0.0.1" || host.starts_with("[::1]") || host.ends_with(".localhost"));
}

bool offline() { return forcedOffline.load() || !networkReachable(); }

void setOffline(bool value) { forcedOffline.store(value); }

bool serverDown(const std::string& url, double now) {
  std::lock_guard lock(downMutex);
  auto it = down.find(urlOrigin(url));
  return it != down.end() && it->second > now;
}

void markServer(const std::string& url, bool ok, double now) {
  std::lock_guard lock(downMutex);
  if (ok) down.erase(urlOrigin(url));
  else down[urlOrigin(url)] = now + kDownMs;
}

DiskCache::DiskCache(std::string directory, uint64_t maxBytes) : dir_(std::move(directory)), maxBytes_(maxBytes) {
  std::error_code ec;
  fs::create_directories(fsPath(dir_), ec);
  if (ec) {
    broken_ = true;
    loaded_ = true;
    return;
  }
  loader_ = std::thread([this] { load(); });
  writer_ = std::thread([this] { writer(); });
}

DiskCache::~DiskCache() {
  {
    std::lock_guard lock(mutex_);
    stop_ = true;
  }
  cv_.notify_all();
  if (loader_.joinable()) loader_.join();
  if (writer_.joinable()) writer_.join();
}

std::string DiskCache::pathFor(const std::string& url) const {
  char name[32];
  const uint64_t h = fnv1a(url);
  std::snprintf(name, sizeof name, "%016llx", static_cast<unsigned long long>(h));
  return dir_ + "/" + std::string(name, 2) + "/" + std::string(name + 2) + ".mkc";
}

void DiskCache::load() {
  std::unordered_map<std::string, Row> found;
  std::error_code ec;
  for (auto it = fs::recursive_directory_iterator(fsPath(dir_), ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
    {
      std::lock_guard lock(mutex_);
      if (stop_) return;
    }
    std::error_code fileError;
    if (!it->is_regular_file(fileError) || it->path().extension() != ".mkc") continue;
    const uint64_t size = it->file_size(fileError);
    if (fileError) continue;
    const auto mtime = it->last_write_time(fileError);
    if (fileError) continue;
    const auto u8 = it->path().u8string();
    std::FILE* f = openFile(std::string(u8.begin(), u8.end()), "rb");
    if (!f) continue;
    uint8_t head[24];
    if (std::fread(head, 1, sizeof head, f) == sizeof head && std::memcmp(head, kMagic, 4) == 0) {
      const uint32_t urlLen = getU32(head + 4);
      double expires;
      std::memcpy(&expires, head + 8, 8);
      const bool pinned = head[16] != 0;
      if (urlLen < 65536) {
        std::string url(urlLen, '\0');
        if (std::fread(url.data(), 1, urlLen, f) == urlLen) {
          Row row;
          row.size = it->file_size(ec);
          const auto mtime = it->last_write_time(ec);
          row.used = static_cast<double>(std::chrono::duration_cast<std::chrono::milliseconds>(mtime.time_since_epoch()).count());
          row.expires = expires;
          row.pinned = pinned;
          total += row.size;
          found[url] = row;
        }
      }
    }
    std::fclose(f);
  }
  std::lock_guard lock(mutex_);
  for (auto& [url, row] : found) {
    if (entries_.contains(url)) continue;
    entries_[url] = row;
    total_ += row.size;
  }
  loaded_ = true;
  cv_.notify_all();
}

void DiskCache::waitLoaded() {
  std::unique_lock lock(mutex_);
  cv_.wait(lock, [this] { return loaded_.load() || stop_; });
}

StoredState DiskCache::state(const std::string& url) {
  const double now = wallMs();
  std::lock_guard lock(mutex_);
  if (auto it = pending_.find(url); it != pending_.end()) return it->second.expires > now ? StoredState::Fresh : StoredState::Stale;
  if (broken_) return StoredState::Missing;
  auto it = entries_.find(url);
  if (it == entries_.end()) return loaded_ ? StoredState::Missing : StoredState::Unknown;
  return it->second.expires > now ? StoredState::Fresh : StoredState::Stale;
}

bool DiskCache::contains(const std::string& url) {
  std::lock_guard lock(mutex_);
  return pending_.contains(url) || entries_.contains(url);
}

std::optional<StoredResponse> DiskCache::read(const std::string& url) {
  {
    std::lock_guard lock(mutex_);
    if (auto it = pending_.find(url); it != pending_.end()) return StoredResponse{it->second.data, it->second.type};
    if (broken_) return std::nullopt;
  }
  const auto bytes = readFile(pathFor(url));
  if (!bytes || bytes->size() < 24 || std::memcmp(bytes->data(), kMagic, 4) != 0) return std::nullopt;
  const uint32_t urlLen = getU32(bytes->data() + 4);
  const size_t typeAt = 24 + urlLen;
  if (bytes->size() < typeAt + 4) return std::nullopt;
  if (std::string(reinterpret_cast<const char*>(bytes->data() + 24), urlLen) != url) return std::nullopt;
  const uint32_t typeLen = getU32(bytes->data() + typeAt);
  const size_t dataAt = typeAt + 4 + typeLen;
  if (bytes->size() < dataAt) return std::nullopt;
  StoredResponse out;
  out.type.assign(reinterpret_cast<const char*>(bytes->data() + typeAt + 4), typeLen);
  out.data = std::make_shared<const Bytes>(bytes->begin() + static_cast<ptrdiff_t>(dataAt), bytes->end());
  std::lock_guard lock(mutex_);
  auto it = entries_.find(url);
  if (it == entries_.end()) {
    Row row;
    row.size = bytes->size();
    row.used = wallMs();
    double expires;
    std::memcpy(&expires, bytes->data() + 8, 8);
    row.expires = expires;
    row.pinned = (*bytes)[16] != 0;
    entries_[url] = row;
    total_ += row.size;
  } else if (wallMs() - it->second.used > kTouchMs) {
    it->second.used = wallMs();
    touched_.push_back(url);
    cv_.notify_all();
  }
  return out;
}

void DiskCache::write(const std::string& url, SharedBytes data, const std::string& type, double expires, bool pinned) {
  if (!data) return;
  std::lock_guard lock(mutex_);
  if (broken_ || data->size() > maxBytes_ / 8) return;
  bool keepPin = pinned;
  if (auto it = entries_.find(url); it != entries_.end()) keepPin = keepPin || it->second.pinned;
  if (auto it = pending_.find(url); it != pending_.end()) keepPin = keepPin || it->second.pinned;
  pending_[url] = {std::move(data), type, expires, keepPin};
  cv_.notify_all();
}

void DiskCache::pin(const std::string& url, bool pinned) {
  std::lock_guard lock(mutex_);
  if (auto it = pending_.find(url); it != pending_.end()) it->second.pinned = pinned;
  if (auto it = entries_.find(url); it != entries_.end() && it->second.pinned != pinned) {
    it->second.pinned = pinned;
    pinChanges_[url] = pinned;
    cv_.notify_all();
  }
}

bool DiskCache::writeEntry(const std::string& url, const Pending& p) {
  const std::string path = pathFor(url);
  std::error_code ec;
  fs::create_directories(fsPath(path).parent_path(), ec);
  std::string head(kMagic, 4);
  putU32(head, static_cast<uint32_t>(url.size()));
  char exp[8];
  std::memcpy(exp, &p.expires, 8);
  head.append(exp, 8);
  head += static_cast<char>(p.pinned ? 1 : 0);
  head.append(7, '\0');
  head += url;
  putU32(head, static_cast<uint32_t>(p.type.size()));
  head += p.type;
  Bytes all(head.begin(), head.end());
  all.insert(all.end(), p.data->begin(), p.data->end());
  return writeFileAtomic(path, all.data(), all.size());
}

void DiskCache::writer() {
  while (true) {
    std::unordered_map<std::string, Pending> batch;
    std::unordered_map<std::string, bool> pins;
    std::vector<std::string> touched;
    {
      std::unique_lock lock(mutex_);
      cv_.wait_for(lock, std::chrono::milliseconds(400), [this] { return stop_; });
      if (pending_.empty() && pinChanges_.empty() && touched_.empty()) {
        if (stop_) return;
        continue;
      }
      batch = pending_;
      pins.swap(pinChanges_);
      touched.swap(touched_);
    }
    for (const auto& [url, p] : batch) {
      const bool ok = writeEntry(url, p);
      std::lock_guard lock(mutex_);
      auto it = pending_.find(url);
      if (it != pending_.end() && it->second.data == p.data) pending_.erase(it);
      if (!ok) continue;
      const uint64_t size = 28 + url.size() + p.type.size() + p.data->size();
      Row& row = entries_[url];
      total_ += size - row.size;
      row.size = size;
      row.used = wallMs();
      row.expires = p.expires;
      row.pinned = p.pinned || row.pinned;
    }
    for (const auto& [url, pinned] : pins) {
      const std::string path = pathFor(url);
      std::FILE* f = openFile(path, "r+b");
      if (!f) continue;
      std::fseek(f, 16, SEEK_SET);
      const char flag = pinned ? 1 : 0;
      std::fwrite(&flag, 1, 1, f);
      std::fclose(f);
    }
    for (const auto& url : touched) {
      std::error_code ec;
      fs::last_write_time(fsPath(pathFor(url)), fs::file_time_type::clock::now(), ec);
    }
    std::lock_guard lock(mutex_);
    evictLocked();
    if (stop_ && pending_.empty()) return;
  }
}

void DiskCache::evictLocked() {
  const uint64_t budget = maxBytes_;
  if (total_ <= budget) return;
  const uint64_t target = budget / 10 * 9;
  std::vector<std::pair<double, std::string>> oldest;
  oldest.reserve(entries_.size());
  for (const auto& [url, row] : entries_)
    if (!row.pinned) oldest.emplace_back(row.used, url);
  std::sort(oldest.begin(), oldest.end());
  for (const auto& [used, url] : oldest) {
    if (total_ <= target) break;
    auto it = entries_.find(url);
    total_ -= it->second.size;
    entries_.erase(it);
    std::error_code ec;
    fs::remove(fsPath(pathFor(url)), ec);
  }
}

void DiskCache::flush() {
  while (true) {
    {
      std::lock_guard lock(mutex_);
      if (pending_.empty() && pinChanges_.empty()) return;
    }
    cv_.notify_all();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
}

void DiskCache::clear(bool keepPinned) {
  std::lock_guard lock(mutex_);
  pending_.clear();
  touched_.clear();
  for (auto it = entries_.begin(); it != entries_.end();) {
    if (keepPinned && it->second.pinned) {
      ++it;
      continue;
    }
    std::error_code ec;
    fs::remove(fsPath(pathFor(it->first)), ec);
    total_ -= it->second.size;
    it = entries_.erase(it);
  }
}

CacheStats DiskCache::stats() {
  std::lock_guard lock(mutex_);
  CacheStats s;
  s.entries = entries_.size();
  s.bytes = total_;
  s.maxBytes = maxBytes_;
  for (const auto& [url, row] : entries_) {
    if (!row.pinned) continue;
    s.pinnedEntries++;
    s.pinnedBytes += row.size;
  }
  return s;
}

void DiskCache::setMaxBytes(uint64_t bytes) {
  maxBytes_ = bytes;
  std::lock_guard lock(mutex_);
  evictLocked();
}

std::string defaultCacheDirectory() {
  {
    std::lock_guard lock(dirMutex);
    if (!configuredDir.empty()) return configuredDir;
  }
#ifdef _WIN32
  if (const char* local = std::getenv("LOCALAPPDATA")) return std::string(local) + "/Mapkern/cache";
#elif defined(__APPLE__)
  if (const char* home = std::getenv("HOME")) return std::string(home) + "/Library/Caches/Mapkern";
#elif defined(__ANDROID__)
  return "/data/local/tmp/mapkern-cache";
#else
  if (const char* xdg = std::getenv("XDG_CACHE_HOME")) return std::string(xdg) + "/mapkern";
  if (const char* home = std::getenv("HOME")) return std::string(home) + "/.cache/mapkern";
#endif
  return "mapkern-cache";
}

void setDefaultCacheDirectory(std::string directory) {
  std::lock_guard lock(dirMutex);
  configuredDir = std::move(directory);
}

std::shared_ptr<DiskCache> sharedDiskCache(const std::string& directory, uint64_t maxBytes) {
  static std::mutex m;
  static std::unordered_map<std::string, std::weak_ptr<DiskCache>> caches;
  std::lock_guard lock(m);
  auto& slot = caches[directory];
  if (auto existing = slot.lock()) {
    if (maxBytes > existing->stats().maxBytes) existing->setMaxBytes(maxBytes);
    return existing;
  }
  auto cache = std::make_shared<DiskCache>(directory, maxBytes);
  slot = cache;
  return cache;
}

}
