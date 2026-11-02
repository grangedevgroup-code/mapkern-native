#include <algorithm>
#include <cmath>
#include <atomic>
#include <limits>
#include <thread>

#include "check.hpp"
#include "formats/mvt.hpp"
#include "style/layer.hpp"
#include "worker/geojson.hpp"
#include "worker/vector_task.hpp"

using namespace mapkern;

namespace {

Value json(const std::string& text) { return *parseJson(text); }

GeoJsonIndex index(const std::string& text, GeoJsonOptions options = {}) { return GeoJsonIndex(json(text), std::move(options)); }

const char* kMixed = R"j({"type":"FeatureCollection","features":[
  {"type":"Feature","id":7,"properties":{"name":"dot","rank":3,"tags":["a"]},"geometry":{"type":"Point","coordinates":[0,0]}},
  {"type":"Feature","id":"road-1","properties":{"name":"line"},"geometry":{"type":"LineString","coordinates":[[-10,0],[10,0]]}},
  {"type":"Feature","properties":{"name":"area"},"geometry":{"type":"Polygon","coordinates":[[[-20,-20],[20,-20],[20,20],[-20,20],[-20,-20]],[[-5,-5],[-5,5],[5,5],[5,-5],[-5,-5]]]}}
]})j";

const Value* prop(const Object& properties, const std::string& key) {
  for (const auto& [k, v] : properties)
    if (k == key) return &v;
  return nullptr;
}

}

TEST("geojson reads points lines and polygons") {
  auto i = index(kMixed);
  EXPECT(i.featureCount() == 3);
  const auto tile = i.tile(0, 0, 0);
  EXPECT(tile->size() == 3);
  EXPECT(tile->name() == "geojson");
  EXPECT(tile->type(0) == 1);
  EXPECT(tile->type(1) == 2);
  EXPECT(tile->type(2) == 3);
  EXPECT(*tile->id(0) == 7);
  EXPECT(tile->textId(1) == "road-1");
  EXPECT(tile->id(1).has_value());
  EXPECT(!tile->id(2).has_value());
  EXPECT(tile->sourceIndex(2) == 2);
  const auto point = tile->geometry(0);
  EXPECT(point.size() == 1);
  EXPECT(point[0][0] == 2048);
  EXPECT(point[0][1] == 2048);
}

TEST("geojson keeps scalar properties and turns the rest into json") {
  auto i = index(kMixed);
  const auto tile = i.tile(0, 0, 0);
  const Object& p = tile->properties(0);
  EXPECT(prop(p, "name")->asString() == "dot");
  EXPECT(prop(p, "rank")->asNumber() == 3);
  EXPECT(prop(p, "tags")->asString() == "[\"a\"]");
}

TEST("geojson orients outer rings and holes like vector tiles") {
  auto i = index(kMixed);
  const auto rings = i.tile(0, 0, 0)->geometry(2);
  EXPECT(rings.size() == 2);
  EXPECT(ringArea(rings[0]) > 0);
  EXPECT(ringArea(rings[1]) < 0);
  EXPECT(classifyRings(rings).size() == 1);
  EXPECT(classifyRings(rings)[0].size() == 2);
}

TEST("geojson clips to the tile and its buffer") {
  auto i = index(R"j({"type":"Feature","properties":{},"geometry":{"type":"LineString","coordinates":[[-170,0],[170,0]]}})j");
  const auto tile = i.tile(1, 1, 0);
  EXPECT(tile->size() == 1);
  const auto parts = tile->geometry(0);
  EXPECT(parts.size() == 1);
  for (size_t k = 0; k < parts[0].size(); k += 2) {
    EXPECT(parts[0][k] >= -513);
    EXPECT(parts[0][k] <= 4096 + 513);
  }
  auto far = index(R"j({"type":"Feature","properties":{},"geometry":{"type":"Point","coordinates":[10,10]}})j");
  EXPECT(far.tile(5, 0, 0)->size() == 0);
}

TEST("geojson polygons stay closed after clipping") {
  auto i = index(R"j({"type":"Feature","properties":{},"geometry":{"type":"Polygon","coordinates":[[[-40,-40],[40,-40],[40,40],[-40,40],[-40,-40]]]}})j");
  const auto parts = i.tile(1, 1, 0)->geometry(0);
  EXPECT(parts.size() == 1);
  const Ring& ring = parts[0];
  EXPECT(ring.size() >= 8);
  EXPECT(ring[0] == ring[ring.size() - 2]);
  EXPECT(ring[1] == ring[ring.size() - 1]);
  EXPECT(ringArea(ring) > 0);
}

TEST("geojson a big polygon with a tile sized hole leaves the tile empty") {
  auto i = index(R"j({"type":"Feature","properties":{},"geometry":{"type":"Polygon","coordinates":[[[-80,-70],[80,-70],[80,70],[-80,70],[-80,-70]],[[-60,-50],[-60,50],[60,50],[60,-50],[-60,-50]]]}})j");
  EXPECT(i.tile(4, 8, 8)->size() == 0);
  EXPECT(i.tile(1, 1, 0)->size() == 1);
}

TEST("geojson shows features past the antimeridian on the other side") {
  auto i = index(R"j({"type":"Feature","properties":{},"geometry":{"type":"Point","coordinates":[190,0]}})j");
  const auto wrapped = i.tile(1, 0, 0);
  EXPECT(wrapped->size() == 1);
  const auto parts = wrapped->geometry(0);
  const double expected = (190.0 - 360 + 180) / 360 * 2 * 4096;
  EXPECT(std::abs(parts[0][0] - expected) <= 1);
}

TEST("geojson simplifies at low zoom and keeps every vertex at the top") {
  std::string coords;
  for (int k = 0; k <= 200; k++) {
    const double lng = -5 + k * 0.05;
    const double lat = (k % 2) * 0.0001;
    coords += (k ? "," : "") + std::string("[") + std::to_string(lng) + "," + std::to_string(lat) + "]";
  }
  GeoJsonOptions options;
  options.maxZoom = 12;
  auto i = index(R"j({"type":"Feature","properties":{},"geometry":{"type":"LineString","coordinates":[)j" + coords + "]}}", options);
  const auto low = i.tile(0, 0, 0)->geometry(0);
  EXPECT(low.size() == 1);
  EXPECT(low[0].size() / 2 <= 10);
  EXPECT(low[0].size() / 2 >= 2);
  const int z = 12;
  const int n = 1 << z;
  const int x = static_cast<int>((-5 + 180) / 360.0 * n);
  const auto top = i.tile(z, x, n / 2);
  EXPECT(top->size() == 1);
  size_t vertices = 0;
  for (const auto& part : top->geometry(0)) vertices += part.size() / 2;
  EXPECT(vertices >= 3);
}

TEST("geojson ids can come from a property or be generated") {
  GeoJsonOptions promoted;
  promoted.promoteId = "code";
  auto a = index(R"j({"type":"FeatureCollection","features":[{"type":"Feature","id":1,"properties":{"code":42},"geometry":{"type":"Point","coordinates":[0,0]}},{"type":"Feature","properties":{"code":"abc"},"geometry":{"type":"Point","coordinates":[1,1]}}]})j", promoted);
  const auto ta = a.tile(0, 0, 0);
  EXPECT(*ta->id(0) == 42);
  EXPECT(ta->textId(1) == "abc");
  GeoJsonOptions generated;
  generated.generateId = true;
  auto b = index(R"j({"type":"FeatureCollection","features":[{"type":"Feature","id":9,"properties":{},"geometry":{"type":"Point","coordinates":[0,0]}},{"type":"Feature","properties":{},"geometry":{"type":"Point","coordinates":[1,1]}}]})j", generated);
  const auto tb = b.tile(0, 0, 0);
  EXPECT(*tb->id(0) == 0);
  EXPECT(*tb->id(1) == 1);
}

TEST("geojson accepts bare geometries and collections") {
  auto bare = index(R"j({"type":"Point","coordinates":[0,0]})j");
  EXPECT(bare.featureCount() == 1);
  auto collection = index(R"j({"type":"Feature","properties":{},"geometry":{"type":"GeometryCollection","geometries":[{"type":"Point","coordinates":[0,0]},{"type":"LineString","coordinates":[[0,0],[1,1]]}]}})j");
  EXPECT(collection.featureCount() == 1);
  const auto tile = collection.tile(0, 0, 0);
  EXPECT(tile->size() == 2);
  EXPECT(tile->sourceIndex(0) == tile->sourceIndex(1));
  EXPECT(collection.itemsOf(0).size() == 2);
  EXPECT(geoJsonGeometry(collection.itemsOf(0))["type"].asString() == "GeometryCollection");
}

TEST("geojson ignores broken input without failing") {
  for (const char* text : {
           R"j({})j",
           R"j([])j",
           R"j({"type":"FeatureCollection"})j",
           R"j({"type":"FeatureCollection","features":[1,"x",null]})j",
           R"j({"type":"Feature"})j",
           R"j({"type":"Feature","geometry":{"type":"Point"}})j",
           R"j({"type":"Feature","geometry":{"type":"Point","coordinates":["a","b"]}})j",
           R"j({"type":"Feature","geometry":{"type":"LineString","coordinates":[[0,0]]}})j",
           R"j({"type":"Feature","geometry":{"type":"Polygon","coordinates":[[[0,0],[1,1]]]}})j",
           R"j({"type":"Feature","geometry":{"type":"Polygon","coordinates":[[[0,0],[1,1],[0,0],[1,1]]]}})j",
           R"j({"type":"Feature","geometry":{"type":"MultiPolygon","coordinates":7}})j",
           R"j({"type":"Feature","geometry":{"type":"GeometryCollection","geometries":[{"type":"Nope"}]}})j",
       }) {
    auto i = index(text);
    EXPECT(i.featureCount() == 0);
    EXPECT(!i.coverage().has_value());
    EXPECT(i.tile(0, 0, 0)->size() == 0);
  }
}

TEST("geojson geometry round trips to longitude and latitude") {
  auto i = index(kMixed);
  const Value point = geoJsonGeometry(i.itemsOf(0));
  EXPECT(point["type"].asString() == "Point");
  EXPECT(std::abs(point["coordinates"][size_t{0}].asNumber()) < 1e-9);
  const Value line = geoJsonGeometry(i.itemsOf(1));
  EXPECT(line["type"].asString() == "LineString");
  EXPECT(std::abs(line["coordinates"][size_t{1}][size_t{0}].asNumber() - 10) < 1e-9);
  const Value polygon = geoJsonGeometry(i.itemsOf(2));
  EXPECT(polygon["type"].asString() == "Polygon");
  EXPECT(polygon["coordinates"].size() == 2);
  EXPECT(geoJsonGeometry({}).isNull());
}

TEST("geojson tiles build buckets for every layer type") {
  auto i = index(kMixed);
  std::vector<LayerProgram> programs;
  std::vector<std::unique_ptr<StyleLayer>> layers;
  for (const char* spec : {R"j({"id":"f","type":"fill","source":"s"})j", R"j({"id":"l","type":"line","source":"s"})j",
                           R"j({"id":"c","type":"circle","source":"s"})j"}) {
    layers.push_back(std::make_unique<StyleLayer>(json(spec)));
    programs.push_back(*layerProgram(*layers.back()));
  }
  std::vector<std::shared_ptr<FeatureSource>> tile{i.tile(0, 0, 0)};
  const VectorTileResult result = buildBuckets(tile, 0, 0, 0, programs, false);
  EXPECT(result.buckets.size() == 3);
  for (const auto& bucket : result.buckets) {
    EXPECT(bucket.main.has_value());
    EXPECT(bucket.features.count() > 0);
  }
}

TEST("geojson tiles are safe to build from many threads") {
  auto i = index(kMixed);
  std::vector<std::thread> threads;
  std::atomic<int> total{0};
  for (int t = 0; t < 4; t++) {
    threads.emplace_back([&i, &total, t] {
      for (int k = 0; k < 200; k++) {
        const int z = (k + t) % 6;
        const int n = 1 << z;
        total += static_cast<int>(i.tile(z, n / 2, n / 2)->size());
      }
    });
  }
  for (auto& thread : threads) thread.join();
  EXPECT(total.load() > 0);
}
