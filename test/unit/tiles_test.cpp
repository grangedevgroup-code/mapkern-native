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
  int uploads = 0;
  bool needsUpload(const FakeData&) const override { return true; }
  void upload(FakeData&) override { uploads++; }
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
    UploadBudget budget{nowMs() + 100};
    m.uploadPending(budget, nowMs());
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

TEST("manager: uploads share one frame budget") {
  Mailbox box;
  RequestScheduler scheduler;
  auto loader = std::make_shared<FakeLoader>(&box);
  TileManager<FakeData> m("test", Tiling{}, loader, scheduler, ManagerOptions{}, ManagerEvents{});
  auto t = view(12.3);
  for (int i = 0; i < 5; i++) {
    m.update(t, nowMs());
    box.drain();
  }
  EXPECT(loader->loads > 1);
  UploadBudget spent{nowMs() - 1, true};
  EXPECT(m.uploadPending(spent, nowMs()));
  EXPECT(loader->uploads == 0);
  UploadBudget late{nowMs() - 1, false};
  EXPECT(m.uploadPending(late, nowMs()));
  EXPECT(loader->uploads == 1);
  EXPECT(late.used);
  UploadBudget ample{nowMs() + 1000, false};
  m.uploadPending(ample, nowMs());
  EXPECT(loader->uploads == loader->loads);
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

TEST("cache: entries from an earlier run are evicted before ones used now") {
  namespace fs = std::filesystem;
  const std::string dir = tempDir("mapkern-cache-order-test");
  const std::string old = "https://example.org/old.pbf";
  {
    DiskCache cache(dir, 1 << 20);
    cache.waitLoaded();
    cache.write(old, std::make_shared<const Bytes>(Bytes(120000, 1)), "x", 0);
    cache.flush();
  }
  std::error_code ec;
  for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
    if (it->is_regular_file()) fs::last_write_time(it->path(), fs::file_time_type::clock::now() - std::chrono::hours(1), ec);
  DiskCache cache(dir, 1 << 20);
  cache.waitLoaded();
  EXPECT(cache.contains(old));
  for (int i = 0; i < 8; i++) cache.write("https://example.org/new" + std::to_string(i), std::make_shared<const Bytes>(Bytes(120000, 2)), "x", 0);
  cache.flush();
  for (int i = 0; i < 100 && cache.stats().bytes > (1u << 20); i++) std::this_thread::sleep_for(std::chrono::milliseconds(20));
  EXPECT(cache.stats().bytes <= (1u << 20));
  EXPECT(!cache.contains(old));
  EXPECT(cache.stats().entries == 7);
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
  if (auto* f = std::get_if<TileFailure>(&first)) std::printf("    failure: %s\n", f->message.c_str());
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
  EXPECT(r && c && c->fromCache && c->data && c->data->size() == r->data->size());
}

#include <fstream>
#include <sstream>

#include "style/import.hpp"
#include "worker/vector_task.hpp"

TEST("net: Liberty buckets from an OpenFreeMap tile (MAPKERN_NET_TESTS=1)") {
  const char* on = std::getenv("MAPKERN_NET_TESTS");
  std::ifstream in("../data/liberty.json", std::ios::binary);
  if (!on || std::string(on) != "1" || !in) {
    std::printf("    skipped\n");
    return;
  }
  std::stringstream ss;
  ss << in.rdbuf();
  const auto style = importStyle(*parseJson(ss.str()), "https://tiles.openfreemap.org/styles/liberty");
  std::vector<LayerProgram> programs;
  for (const auto& l : style.style["layers"].asArray()) {
    StyleLayer layer(l);
    if (auto p = layerProgram(layer)) programs.push_back(std::move(*p));
  }
  std::atomic<int> done{0};
  FetchOutcome tilejson;
  fetchBytes({"https://tiles.openfreemap.org/planet", {}}, CancelToken(), 20000, ResourceKind::Source, nullptr, [&](FetchOutcome o) {
    tilejson = std::move(o);
    done = 1;
  });
  for (int i = 0; i < 400 && !done; i++) std::this_thread::sleep_for(std::chrono::milliseconds(50));
  auto* tj = std::get_if<FetchResult>(&tilejson);
  EXPECT(tj && tj->data);
  if (!tj || !tj->data) return;
  const auto json = parseJson(std::string_view(reinterpret_cast<const char*>(tj->data->data()), tj->data->size()));
  EXPECT(json && (*json)["tiles"].isArray());
  if (!json) return;
  const std::string url = expandTemplate((*json)["tiles"][0].asString(), 14, 2624, 5722);
  done = 0;
  FetchOutcome tile;
  fetchBytes({url, {}}, CancelToken(), 20000, ResourceKind::Tile, nullptr, [&](FetchOutcome o) {
    tile = std::move(o);
    done = 1;
  });
  for (int i = 0; i < 400 && !done; i++) std::this_thread::sleep_for(std::chrono::milliseconds(50));
  auto* t = std::get_if<FetchResult>(&tile);
  EXPECT(t && t->data);
  if (!t || !t->data) return;
  const auto result = buildVectorTile(*t->data, 14, 2624, 5722, programs, false, {});
  size_t fills = 0, lines = 0, extrusions = 0;
  for (const auto& b : result.buckets) {
    fills += b.type == BucketType::Fill;
    lines += b.type == BucketType::Line;
    extrusions += b.type == BucketType::Extrusion;
  }
  std::printf("    Seattle z14: %zu buckets (%zu fill, %zu line, %zu extrusion), %zu features, %.1f ms\n", result.buckets.size(), fills,
              lines, extrusions, result.featureCount, result.decodeMs);
  EXPECT(fills > 3 && lines > 10);
}

TEST("manager: eviction candidates are only tiles off screen, oldest first, and dropping one frees its bytes") {
  Mailbox box;
  RequestScheduler scheduler;
  auto loader = std::make_shared<FakeLoader>(&box);
  ManagerOptions options;
  options.cacheBytes = size_t{1} << 30;
  TileManager<FakeData> m("test", Tiling{}, loader, scheduler, options, ManagerEvents{});
  auto t = view(12.3);
  settle(m, box, t);
  const size_t shown = m.cachedBytes();
  std::vector<TileCandidate> none;
  m.evictionCandidates(none, 3);
  EXPECT(none.empty());
  t.setCenter(-100, 40);
  settle(m, box, t);
  std::vector<TileCandidate> old;
  m.evictionCandidates(old, 3);
  EXPECT(!old.empty());
  for (const auto& c : old) EXPECT(c.owner == 3 && c.bytes == 1000);
  const size_t before = m.cachedBytes();
  EXPECT(before > shown);
  m.dropTile(old.front().key);
  EXPECT(m.cachedBytes() == before - 1000);
  EXPECT(m.stats().evicted == 1);
  std::vector<TileCandidate> visible;
  for (const auto& p : m.pieces) visible.push_back({0, p.tile->z, 0, p.tile->key, 0});
  for (const auto& c : visible) m.dropTile(c.key);
  EXPECT(m.stats().evicted == 1);
  EXPECT(m.stats().blankPending == 0);
}

TEST("manager: the two coarsest levels are never offered for eviction") {
  Mailbox box;
  RequestScheduler scheduler;
  auto loader = std::make_shared<FakeLoader>(&box);
  ManagerOptions options;
  options.cacheBytes = size_t{1} << 30;
  TileManager<FakeData> m("test", Tiling{}, loader, scheduler, options, ManagerEvents{});
  auto t = view(1.2);
  settle(m, box, t);
  t.setZoom(9.3);
  settle(m, box, t);
  t.setCenter(-50, 10);
  settle(m, box, t);
  std::vector<TileCandidate> c;
  m.evictionCandidates(c, 0);
  EXPECT(!c.empty());
  for (const auto& k : c) EXPECT(k.z > 1);
}
