#include <cstring>
#include <filesystem>

#include "check.hpp"
#include "formats/bytes.hpp"
#include "formats/inflate.hpp"
#include "formats/mvt.hpp"
#include "formats/pmtiles.hpp"
#include "formats/protobuf.hpp"
#include "formats/sqlite.hpp"

using namespace mapkern;

TEST("inflate: gzip with stored, fixed and dynamic blocks") {
  const uint8_t hello[] = {0x1f, 0x8b, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0xcb, 0x48, 0xcd, 0xc9, 0xc9,
                           0x57, 0x28, 0xcf, 0x2f, 0xca, 0x49, 0x01, 0x00, 0x85, 0x11, 0x4a, 0x0d, 0x0b, 0x00, 0x00, 0x00};
  const auto out = gunzip(hello);
  EXPECT(std::string(out.begin(), out.end()) == "hello world");
  const auto gpkg = readFile("fixtures/layers.gpkg.gz");
  EXPECT(gpkg.has_value());
  if (gpkg) {
    const auto db = gunzip(*gpkg);
    EXPECT(db.size() > 4096);
    EXPECT(std::memcmp(db.data(), "SQLite format 3", 15) == 0);
  }
  bool threw = false;
  try {
    uint8_t bad[] = {0x1f, 0x8b, 0x08, 0, 0, 0, 0, 0, 0, 3, 0xff, 0xff, 0xff};
    gunzip(bad);
  } catch (const FormatError&) {
    threw = true;
  }
  EXPECT(threw);
}

TEST("sqlite: reads tables from a GeoPackage") {
  const auto gz = readFile("fixtures/layers.gpkg.gz");
  if (!gz) return;
  auto data = std::make_shared<const Bytes>(gunzip(*gz));
  SqliteReader db(std::make_shared<MemorySource>(data));
  const auto schema = db.schema();
  uint32_t places = 0;
  for (const auto& row : schema)
    if (row.type == "table" && row.name == "places") places = row.root;
  EXPECT(places > 0);
  std::vector<std::string> names;
  db.scan(places, [&](int64_t, std::vector<SqlValue>& values) {
    for (const auto& v : values)
      if (auto* s = std::get_if<std::string>(&v)) names.push_back(*s);
  });
  EXPECT(std::find(names.begin(), names.end(), "Space Needle") != names.end());
  EXPECT(std::find(names.begin(), names.end(), "Pike Place") != names.end());
}

static std::string encodeTile() {
  pbf::Writer feature;
  feature.uintField(1, 42);
  {
    pbf::Writer tags;
    tags.varint(0);
    tags.varint(0);
    tags.varint(1);
    tags.varint(1);
    feature.bytes(2, tags.out);
  }
  feature.uintField(3, 3);
  {
    pbf::Writer g;
    g.varint((1 << 3) | 1);
    g.svarint(0);
    g.svarint(0);
    g.varint((3 << 3) | 2);
    g.svarint(100);
    g.svarint(0);
    g.svarint(0);
    g.svarint(100);
    g.svarint(-100);
    g.svarint(0);
    g.varint(7 | (1 << 3));
    feature.bytes(4, g.out);
  }
  pbf::Writer layer;
  layer.uintField(15, 2);
  layer.bytes(1, "water");
  layer.bytes(2, feature.out);
  layer.bytes(3, "class");
  layer.bytes(3, "depth");
  {
    pbf::Writer v;
    v.bytes(1, "lake");
    layer.bytes(4, v.out);
  }
  {
    pbf::Writer v;
    v.doubleField(3, 12.5);
    layer.bytes(4, v.out);
  }
  layer.uintField(5, 4096);
  pbf::Writer tile;
  tile.bytes(3, layer.out);
  return tile.out;
}

TEST("mvt: decodes layers, properties and polygons") {
  const std::string raw = encodeTile();
  auto bytes = std::make_shared<const Bytes>(raw.begin(), raw.end());
  const auto tile = decodeVectorTile(bytes);
  EXPECT(tile.size() == 1 && tile.contains("water"));
  const auto& layer = *tile.at("water");
  EXPECT(layer.extent == 4096 && layer.size() == 1);
  const auto f = layer.feature(0);
  EXPECT(f.id && *f.id == 42);
  EXPECT(f.type == 3);
  const auto props = Value(f.properties());
  EXPECT(props["class"] == Value("lake"));
  EXPECT(props["depth"] == Value(12.5));
  EXPECT(f.property("depth") && *f.property("depth") == Value(12.5));
  const auto rings = f.geometry();
  EXPECT(rings.size() == 1 && rings[0].size() == 10);
  EXPECT(rings[0][2] == 100 && rings[0][5] == 100);
  const auto polys = classifyRings(rings);
  EXPECT(polys.size() == 1);
  EXPECT(std::abs(ringArea(rings[0])) == 5000);
}

TEST("pmtiles: hilbert tile ids round trip") {
  EXPECT(zxyToTileId(0, 0, 0) == 0);
  EXPECT(zxyToTileId(1, 0, 0) == 1);
  EXPECT(zxyToTileId(1, 0, 1) == 2);
  EXPECT(zxyToTileId(1, 1, 1) == 3);
  EXPECT(zxyToTileId(1, 1, 0) == 4);
  EXPECT(zxyToTileId(2, 0, 0) == 5);
  for (int z = 0; z < 12; z++) {
    for (uint32_t i = 0; i < 50; i++) {
      const uint32_t n = 1u << z;
      const uint32_t x = (i * 2654435761u) % n, y = (i * 40503u + 7) % n;
      int zz;
      uint32_t xx, yy;
      tileIdToZxy(zxyToTileId(z, x, y), zz, xx, yy);
      EXPECT(zz == z && xx == x && yy == y);
    }
  }
}

TEST("pmtiles: reads a local archive when present") {
  const std::string path = "../../Mapkern GL JS/data/cli-test.pmtiles";
  if (!std::filesystem::exists(path)) {
    std::printf("    skipped: %s not present\n", path.c_str());
    return;
  }
  PmTilesReader reader(std::make_shared<FileSource>(path));
  const auto& h = reader.header();
  EXPECT(h.tileType == TileType::Mvt);
  EXPECT(h.maxZoom >= h.minZoom);
  const Value meta = reader.metadata();
  EXPECT(meta.isObject());
  int found = 0;
  const int z = h.maxZoom;
  const double n = std::exp2(z);
  const uint32_t x = static_cast<uint32_t>((h.centerLon + 180) / 360 * n);
  const double lat = h.centerLat * 3.14159265358979 / 180;
  const uint32_t y = static_cast<uint32_t>((1 - std::log(std::tan(lat) + 1 / std::cos(lat)) / 3.14159265358979) / 2 * n);
  if (auto t = reader.tile(z, x, y)) {
    const auto data = std::make_shared<const Bytes>(decompressPm(*t, h.tileCompression));
    const auto tile = decodeVectorTile(data);
    found = static_cast<int>(tile.size());
  }
  EXPECT(found > 0);
}
