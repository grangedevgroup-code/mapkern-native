#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <thread>

#include "check.hpp"
#include "net/http.hpp"
#include "tiles/cache.hpp"
#include "tiles/fetch.hpp"
#include "tiles/manager.hpp"

using namespace mapkern;

namespace {

struct FakeData {
  TileId id;
};

class FakeLoader : public TileLoader<FakeData> {
 public:
  Mailbox* box;
  int loads = 0;
  bool emptyAboveZ10 = false;
  explicit FakeLoader(Mailbox* b) : box(b) {}
  void load(TileId id, CancelToken, std::function<void()> onDecoding, std::function<void(LoadResult<FakeData>)> done) override {
    loads++;
    box->post([id, onDecoding, done, this] {
      onDecoding();
      if (emptyAboveZ10 && id.z > 10) done(std::shared_ptr<FakeData>());
      else done(std::make_shared<FakeData>(FakeData{id}));
    });
  }
  bool needsUpload(const FakeData&) const override { return true; }
  void upload(FakeData&) override {}
  size_t byteSize(const FakeData&) const override { return 1000; }
};

Transform view(double zoom) {
  Transform t;
  t.resize(800, 600);
  t.setZoom(zoom);
  t.setCenter(-122.33, 47.6);
  return t;
}

void settle(TileManager<FakeData>& m, Mailbox& box, Transform& t) {
  for (int i = 0; i < 50; i++) {
    m.update(t, nowMs());
    box.drain();
    m.uploadPending(nowMs() + 100, nowMs());
  }
}

std::string tempDir(const char* name) {
  auto p = std::filesystem::temp_directory_path() / name;
  std::error_code ec;
  std::filesystem::remove_all(p, ec);
  return p.string();
}

}

TEST("manager: loads every visible tile and shows no blank area") {
  Mailbox box;
  RequestScheduler scheduler;
  auto loader = std::make_shared<FakeLoader>(&box);
  TileManager<FakeData> m("test", Tiling{}, loader, scheduler, ManagerOptions{}, ManagerEvents{});
  auto t = view(12.3);
  settle(m, box, t);
  const auto s = m.stats();
  EXPECT(s.visibleSlots > 0);
  EXPECT(s.blankPending == 0 && s.blankFailed == 0);
  EXPECT(m.isLoaded());
  EXPECT(s.pieces == s.visibleSlots);
}

TEST("manager: a zoom in draws parents until children arrive") {
  Mailbox box;
  RequestScheduler scheduler;
  auto loader = std::make_shared<FakeLoader>(&box);
  TileManager<FakeData> m("test", Tiling{}, loader, scheduler, ManagerOptions{}, ManagerEvents{});
  auto t = view(10.2);
  settle(m, box, t);
  t.setZoom(11.2);
  m.update(t, nowMs());
  const auto s = m.stats();
  EXPECT(s.blankPending == 0);
  EXPECT(s.fallbackPieces > 0);
  for (const auto& p : m.pieces) EXPECT(p.tile->z <= 11);
}

TEST("manager: empty tiles fall back to their parent") {
  Mailbox box;
  RequestScheduler scheduler;
  auto loader = std::make_shared<FakeLoader>(&box);
  loader->emptyAboveZ10 = true;
  TileManager<FakeData> m("test", Tiling{}, loader, scheduler, ManagerOptions{}, ManagerEvents{});
  auto t = view(12.2);
  settle(m, box, t);
  const auto s = m.stats();
  EXPECT(s.blankPending == 0);
  EXPECT(!m.pieces.empty());
  for (const auto& p : m.pieces) EXPECT(p.tile->z <= 10);
}

TEST("cache: entries survive a restart and pinned ones survive eviction") {
  const std::string dir = tempDir("mapkern-cache-test");
  {
    DiskCache cache(dir, 1 << 20);
    cache.waitLoaded();
    auto data = std::make_shared<const Bytes>(Bytes{1, 2, 3, 4});
    cache.write("https://example.org/a.pbf", data, "application/x-protobuf", wallMs() + 60000);
    cache.write("https://example.org/pinned.pbf", data, "x", wallMs() - 1, true);
    EXPECT(cache.state("https://example.org/a.pbf") == StoredState::Fresh);
    cache.flush();
  }
  {
    DiskCache cache(dir, 1 << 20);
    cache.waitLoaded();
    EXPECT(cache.state("https://example.org/a.pbf") == StoredState::Fresh);
    EXPECT(cache.state("https://example.org/pinned.pbf") == StoredState::Stale);
    EXPECT(cache.state("https://example.org/none.pbf") == StoredState::Missing);
    const auto hit = cache.read("https://example.org/a.pbf");
    EXPECT(hit && hit->data->size() == 4 && (*hit->data)[3] == 4 && hit->type == "application/x-protobuf");
    for (int i = 0; i < 40; i++)
      cache.write("https://example.org/big" + std::to_string(i), std::make_shared<const Bytes>(Bytes(100000, 7)), "x", 0);
    cache.flush();
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    const auto s = cache.stats();
    EXPECT(s.bytes <= (1u << 20));
    EXPECT(cache.contains("https://example.org/pinned.pbf"));
    EXPECT(s.pinnedEntries == 1);
  }
}

TEST("fetch: cache freshness from headers") {
  auto lookup = [](std::string control, std::string expires = "") {
    return HeaderLookup{[=](std::string_view n) { return n == "cache-control" ? control : n == "expires" ? expires : std::string(); }};
  };
  EXPECT(!freshUntil(lookup("no-store"), 1000).has_value());
  EXPECT(*freshUntil(lookup("no-cache"), 1000) == 0);
  EXPECT(*freshUntil(lookup("public, max-age=60"), 1000) == 61000);
  EXPECT(*freshUntil(lookup("", "Thu, 01 Jan 1970 00:00:10 GMT"), 0) == 10000);
  EXPECT(classifyStatus(403, "").kind == FailureKind::Auth);
  EXPECT(classifyStatus(429, "5").retryAfterMs == 5000);
  EXPECT(classifyStatus(500, "").retryable);
  EXPECT(expandTemplate("https://t/{z}/{x}/{y}.png?{-y}", 3, 1, 2) == "https://t/3/1/2.png?5");
  EXPECT(cacheable("https://a.org/x") && !cacheable("http://localhost:8080/x") && !cacheable("file:///x"));
}

TEST("net: real HTTPS request (MAPKERN_NET_TESTS=1)") {
  const char* on = std::getenv("MAPKERN_NET_TESTS");
  if (!on || std::string(on) != "1") {
    std::printf("    skipped: set MAPKERN_NET_TESTS=1\n");
    return;
  }
  const std::string dir = tempDir("mapkern-net-test");
  auto cache = std::make_shared<DiskCache>(dir, 64 << 20);
  cache->waitLoaded();
  std::atomic<int> done{0};
  FetchOutcome first, second;
  fetchBytes({"https://tiles.openfreemap.org/planet", {}}, CancelToken(), 20000, ResourceKind::Source, cache, [&](FetchOutcome out) {
    first = std::move(out);
    done = 1;
  });
  for (int i = 0; i < 400 && done == 0; i++) std::this_thread::sleep_for(std::chrono::milliseconds(50));
  auto* r = std::get_if<FetchResult>(&first);
  EXPECT(r && !r->empty && r->data->size() > 100);
  if (r && r->data) EXPECT(std::string(r->data->begin(), r->data->begin() + 1) == "{");
  setOffline(true);
  done = 0;
  fetchBytes({"https://tiles.openfreemap.org/planet", {}}, CancelToken(), 20000, ResourceKind::Source, cache, [&](FetchOutcome out) {
    second = std::move(out);
    done = 1;
  });
  for (int i = 0; i < 400 && done == 0; i++) std::this_thread::sleep_for(std::chrono::milliseconds(50));
  setOffline(false);
  auto* c = std::get_if<FetchResult>(&second);
  EXPECT(c && c->fromCache && c->data && c->data->size() == r->data->size());
}
