#include "map/tileset.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <mutex>
#include <unordered_map>

#include "core/url.hpp"
#include "formats/pmtiles.hpp"
#include "formats/sqlite.hpp"
#include "geo/mercator.hpp"
#include "net/http.hpp"

namespace mapkern {

namespace {

class HttpRangeSource : public ByteSource {
 public:
  explicit HttpRangeSource(std::string url) : url_(std::move(url)) {}
  Bytes read(uint64_t offset, size_t length) override {
    HttpRequest req;
    req.url = url_;
    req.range = std::make_pair(offset, static_cast<uint64_t>(length));
    HttpResponse res = httpClient()->perform(req, CancelToken());
    if (res.networkError) throw FormatError("Could not read " + url_ + ": " + res.error);
    if (res.status == 200) {
      if (offset >= res.body.size()) return {};
      const size_t end = static_cast<size_t>(std::min<uint64_t>(res.body.size(), offset + length));
      return Bytes(res.body.begin() + static_cast<ptrdiff_t>(offset), res.body.begin() + static_cast<ptrdiff_t>(end));
    }
    if (res.status != 206) throw FormatError("Range request to " + url_ + " failed with HTTP " + std::to_string(res.status));
    return std::move(res.body);
  }
  uint64_t size() override { return UINT64_MAX; }
  std::string name() const override { return url_; }

 private:
  std::string url_;
};

std::shared_ptr<ByteSource> openSource(const std::string& location) {
  if (isLocalPath(location)) return std::make_shared<FileSource>(localPath(location));
  return std::make_shared<HttpRangeSource>(location);
}

struct MbTiles {
  std::shared_ptr<SqliteReader> db;
  uint32_t tilesRoot = 0;
  uint32_t indexRoot = 0;
  uint32_t mapRoot = 0, imagesRoot = 0, mapIndexRoot = 0, imagesIndexRoot = 0;
  Object metadata;
};

std::mutex archiveMutex;
std::unordered_map<std::string, std::shared_ptr<PmTilesReader>> pmReaders;
std::unordered_map<std::string, std::shared_ptr<MbTiles>> mbReaders;

std::shared_ptr<PmTilesReader> pmReader(const std::string& location) {
  std::lock_guard lock(archiveMutex);
  auto& r = pmReaders[location];
  if (!r) r = std::make_shared<PmTilesReader>(openSource(location));
  return r;
}

std::shared_ptr<MbTiles> mbReader(const std::string& location) {
  std::shared_ptr<MbTiles> m;
  {
    std::lock_guard lock(archiveMutex);
    auto& r = mbReaders[location];
    if (r) return r;
    r = std::make_shared<MbTiles>();
    r->db = std::make_shared<SqliteReader>(openSource(location));
    m = r;
  }
  try {
    for (const auto& row : m->db->schema()) {
      if (row.name == "tiles") m->tilesRoot = row.root;
      if (row.name == "map") m->mapRoot = row.root;
      if (row.name == "images") m->imagesRoot = row.root;
      if (row.type == "index" && row.table == "tiles") m->indexRoot = row.root;
      if (row.type == "index" && row.table == "map") m->mapIndexRoot = row.root;
      if (row.type == "index" && row.table == "images") m->imagesIndexRoot = row.root;
      if (row.name == "metadata" && row.type == "table") {
        m->db->scan(row.root, [&](int64_t, std::vector<SqlValue>& v) {
          if (v.size() >= 2)
            if (auto* k = std::get_if<std::string>(&v[0]))
              if (auto* val = std::get_if<std::string>(&v[1])) m->metadata.emplace_back(*k, *val);
        });
      }
    }
  } catch (...) {
    std::lock_guard lock(archiveMutex);
    mbReaders.erase(location);
    throw;
  }
  return m;
}

std::optional<Bytes> mbTile(MbTiles& m, int z, int x, int y) {
  const double row = static_cast<double>((int64_t{1} << z) - 1 - y);
  const std::vector<SqlValue> key{static_cast<double>(z), static_cast<double>(x), row};
  if (m.indexRoot) {
    if (auto found = m.db->lookup(m.indexRoot, key)) {
      if (found->size() >= 4)
        if (auto* rowid = std::get_if<double>(&(*found)[3])) {
          if (auto r = m.db->row(m.tilesRoot, static_cast<int64_t>(*rowid)))
            if (r->size() >= 4)
              if (auto* data = std::get_if<Bytes>(&(*r)[3])) return *data;
        }
      return std::nullopt;
    }
    if (m.tilesRoot) return std::nullopt;
  }
  if (m.mapIndexRoot && m.imagesIndexRoot) {
    auto mapRow = m.db->lookup(m.mapIndexRoot, key);
    if (!mapRow || mapRow->size() < 4) return std::nullopt;
    auto* rowid = std::get_if<double>(&(*mapRow)[3]);
    if (!rowid) return std::nullopt;
    auto full = m.db->row(m.mapRoot, static_cast<int64_t>(*rowid));
    if (!full || full->size() < 4) return std::nullopt;
    auto image = m.db->lookup(m.imagesIndexRoot, {(*full)[3]});
    if (!image || image->size() < 2) return std::nullopt;
    auto* imageRow = std::get_if<double>(&(*image)[1]);
    if (!imageRow) return std::nullopt;
    auto img = m.db->row(m.imagesRoot, static_cast<int64_t>(*imageRow));
    if (!img || img->empty()) return std::nullopt;
    if (auto* data = std::get_if<Bytes>(&(*img)[0])) return *data;
    return std::nullopt;
  }
  std::optional<Bytes> out;
  if (m.tilesRoot)
    m.db->scan(m.tilesRoot, [&](int64_t, std::vector<SqlValue>& v) {
      if (out || v.size() < 4) return;
      auto* zz = std::get_if<double>(&v[0]);
      auto* xx = std::get_if<double>(&v[1]);
      auto* yy = std::get_if<double>(&v[2]);
      if (zz && xx && yy && *zz == z && *xx == x && *yy == row)
        if (auto* data = std::get_if<Bytes>(&v[3])) out = *data;
    });
  return out;
}

bool parseZxyTail(std::string& location, int& z, int& x, int& y) {
  for (int k = 0; k < 3; k++) {
    const size_t slash = location.rfind('/');
    if (slash == std::string::npos) return false;
    std::string part = location.substr(slash + 1);
    if (k == 0) {
      const size_t dot = part.find('.');
      if (dot != std::string::npos) part = part.substr(0, dot);
    }
    if (part.empty() || !std::all_of(part.begin(), part.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); }))
      return false;
    const int v = std::stoi(part);
    if (k == 0) y = v;
    else if (k == 1) x = v;
    else z = v;
    location = location.substr(0, slash);
  }
  return true;
}

FetchOutcome bytesOutcome(std::optional<Bytes> bytes, const std::string& type) {
  if (!bytes || bytes->empty()) return FetchResult{};
  FetchResult r;
  r.empty = false;
  r.contentType = type;
  r.data = std::make_shared<const Bytes>(std::move(*bytes));
  return r;
}

FetchOutcome jsonOutcome(const Value& v) {
  const std::string text = v.toJson();
  return bytesOutcome(Bytes(text.begin(), text.end()), "application/json");
}

std::string tileTypeName(TileType t) {
  switch (t) {
    case TileType::Mvt: return "pbf";
    case TileType::Png: return "png";
    case TileType::Jpeg: return "jpg";
    case TileType::Webp: return "webp";
    case TileType::Avif: return "avif";
    default: return "";
  }
}

FetchOutcome pmtilesProtocol(const ProtocolRequest& request, const CancelToken&) {
  std::string location = request.url.substr(std::string("pmtiles://").size());
  int z, x, y;
  std::string tileLocation = location;
  if (parseZxyTail(tileLocation, z, x, y) && request.kind == ResourceKind::Tile) {
    auto reader = pmReader(tileLocation);
    const auto& h = reader->header();
    auto raw = reader->tile(z, static_cast<uint32_t>(x), static_cast<uint32_t>(y));
    if (!raw) return FetchResult{};
    return bytesOutcome(decompressPm(*raw, h.tileCompression), tileTypeName(h.tileType));
  }
  auto reader = pmReader(location);
  const auto& h = reader->header();
  Value meta = reader->metadata();
  Object tj = meta.isObject() ? meta.asObject() : Object{};
  tj = withKey(std::move(tj), "tilejson", "3.0.0");
  tj = withKey(std::move(tj), "tiles", Array{"pmtiles://" + location + "/{z}/{x}/{y}"});
  tj = withKey(std::move(tj), "minzoom", static_cast<int>(h.minZoom));
  tj = withKey(std::move(tj), "maxzoom", static_cast<int>(h.maxZoom));
  tj = withKey(std::move(tj), "bounds", Array{h.minLon, h.minLat, h.maxLon, h.maxLat});
  tj = withKey(std::move(tj), "center", Array{h.centerLon, h.centerLat, static_cast<int>(h.centerZoom)});
  tj = withKey(std::move(tj), "format", tileTypeName(h.tileType));
  if (meta["vector_layers"].isArray()) tj = withKey(std::move(tj), "vector_layers", meta["vector_layers"]);
  return jsonOutcome(Value(tj));
}

FetchOutcome mbtilesProtocol(const ProtocolRequest& request, const CancelToken&) {
  std::string location = request.url.substr(std::string("mbtiles://").size());
  int z, x, y;
  std::string tileLocation = location;
  if (parseZxyTail(tileLocation, z, x, y) && request.kind == ResourceKind::Tile) {
    auto m = mbReader(tileLocation);
    auto raw = mbTile(*m, z, x, y);
    if (!raw) return FetchResult{};
    return bytesOutcome(decompressAuto(*raw), "");
  }
  auto m = mbReader(location);
  const Value meta(m->metadata);
  Object tj{{"tilejson", "3.0.0"}, {"tiles", Array{"mbtiles://" + location + "/{z}/{x}/{y}"}}};
  if (meta["minzoom"].isString()) tj.emplace_back("minzoom", std::stod(meta["minzoom"].asString()));
  if (meta["maxzoom"].isString()) tj.emplace_back("maxzoom", std::stod(meta["maxzoom"].asString()));
  if (meta["bounds"].isString()) {
    Array b;
    std::string s = meta["bounds"].asString();
    size_t pos = 0;
    while (pos <= s.size() && b.size() < 4) {
      const size_t comma = s.find(',', pos);
      b.emplace_back(std::stod(s.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos)));
      if (comma == std::string::npos) break;
      pos = comma + 1;
    }
    if (b.size() == 4) tj.emplace_back("bounds", b);
  }
  if (meta["attribution"].isString()) tj.emplace_back("attribution", meta["attribution"]);
  if (meta["json"].isString()) {
    if (auto extra = parseJson(meta["json"].asString()))
      if ((*extra)["vector_layers"].isArray()) tj.emplace_back("vector_layers", (*extra)["vector_layers"]);
  }
  return jsonOutcome(Value(tj));
}

int zoomValue(const Value& v, int fallback) {
  if (!v.isNumber() || !std::isfinite(v.asNumber())) return fallback;
  return std::clamp(static_cast<int>(std::lround(v.asNumber())), 0, kMaxTileZoom);
}

std::string absolutize(const std::string& url, const std::string& base) {
  if (base.empty() || hasScheme(url)) return url;
  std::string encoded = replaceAll(replaceAll(url, "{", "%7B"), "}", "%7D");
  std::string r = resolveUrl(encoded, base);
  r = replaceAll(replaceAll(r, "%7B", "{"), "%7b", "{");
  return replaceAll(replaceAll(r, "%7D", "}"), "%7d", "}");
}

TileSetResult finish(const std::string& sourceId, const Value& spec, const Value& raw, const std::string& base, TileSetDefaults defaults) {
  const Value& tilesValue = spec.has("tiles") ? spec["tiles"] : raw["tiles"];
  if (!tilesValue.isArray() || tilesValue.size() == 0) return "Source \"" + sourceId + "\" needs a non-empty \"tiles\" list of URL templates";
  TileSet set;
  for (const auto& t : tilesValue.asArray()) {
    if (!t.isString()) return "Source \"" + sourceId + "\" needs a non-empty \"tiles\" list of URL templates";
    set.tiles.push_back(absolutize(t.asString(), base));
  }
  set.minZoom = zoomValue(spec.has("minZoom") ? spec["minZoom"] : raw["minzoom"], defaults.minZoom);
  set.maxZoom = std::max(set.minZoom, zoomValue(spec.has("maxZoom") ? spec["maxZoom"] : raw["maxzoom"], defaults.maxZoom));
  set.bounds = boundsRect(spec.has("bounds") ? spec["bounds"] : raw["bounds"]);
  set.tms = (spec.has("scheme") ? spec["scheme"] : raw["scheme"]).stringOr("") == "tms";
  set.attribution = spec["attribution"].isString() ? spec["attribution"].asString() : raw["attribution"].stringOr("");
  if (raw["vector_layers"].isArray()) {
    for (const auto& l : raw["vector_layers"].asArray()) {
      if (!l["id"].isString()) continue;
      VectorLayerInfo info;
      info.id = l["id"].asString();
      if (l["fields"].isObject()) info.fields = l["fields"].asObject();
      if (l["minzoom"].isNumber()) info.minZoom = l["minzoom"].asNumber();
      if (l["maxzoom"].isNumber()) info.maxZoom = l["maxzoom"].asNumber();
      set.vectorLayers.push_back(std::move(info));
    }
  }
  set.raw = raw;
  return set;
}

}

void registerArchiveProtocols() {
  static std::once_flag once;
  std::call_once(once, [] {
    addBuiltinProtocol("pmtiles", pmtilesProtocol);
    addBuiltinProtocol("mbtiles", mbtilesProtocol);
  });
}

Value archiveSpec(const Value& spec) {
  if (!spec["url"].isString()) return spec;
  const std::string url = spec["url"].asString();
  std::string lower = url;
  for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  if (lower.starts_with("pmtiles://") || lower.starts_with("mbtiles://")) return spec;
  std::string path = lower.substr(0, lower.find_first_of("?#"));
  std::string scheme;
  if (path.ends_with(".pmtiles")) scheme = "pmtiles";
  else if (path.ends_with(".mbtiles")) scheme = "mbtiles";
  if (scheme.empty()) return spec;
  return Value(withKey(spec.asObject(), "url", scheme + "://" + url));
}

std::optional<Rect> boundsRect(const Value& value) {
  if (!value.isArray() || value.size() != 4) return std::nullopt;
  double v[4];
  for (int i = 0; i < 4; i++) {
    if (!value[i].isNumber() || !std::isfinite(value[i].asNumber())) return std::nullopt;
    v[i] = value[i].asNumber();
  }
  const double w = v[0], s = v[1], e = v[2], n = v[3];
  if (w >= e || s >= n) return std::nullopt;
  if (w <= -180 && e >= 180 && s <= -85 && n >= 85) return std::nullopt;
  return Rect{mercatorX(std::max(-180.0, w)), mercatorY(clampLatitude(n)), mercatorX(std::min(180.0, e)), mercatorY(clampLatitude(s))};
}

void resolveTileSet(const std::string& sourceId, const Value& specIn, TileSetDefaults defaults, const TransformRequest& transform,
                    int timeoutMs, std::shared_ptr<ResponseStore> store, CancelToken cancel, std::function<void(TileSetResult)> done) {
  registerArchiveProtocols();
  const Value spec = archiveSpec(specIn);
  if (!spec["url"].isString()) {
    done(finish(sourceId, spec, Object{}, "", defaults));
    return;
  }
  const RequestParameters params = resolveRequest(spec["url"].asString(), ResourceKind::Source, transform);
  fetchBytes(params, cancel, timeoutMs, ResourceKind::Source, store, [=](FetchOutcome out) {
    if (auto* f = std::get_if<TileFailure>(&out)) {
      done("Could not load TileJSON for source \"" + sourceId + "\" from " + params.url + ": " + f->message);
      return;
    }
    const auto& r = std::get<FetchResult>(out);
    if (r.empty || !r.data) {
      done("TileJSON for source \"" + sourceId + "\" was not found at " + params.url);
      return;
    }
    auto raw = parseJson(std::string_view(reinterpret_cast<const char*>(r.data->data()), r.data->size()));
    if (!raw || !raw->isObject()) {
      done("TileJSON for source \"" + sourceId + "\" is not valid JSON");
      return;
    }
    const std::string base = hasScheme(params.url) && (params.url.starts_with("http")) ? params.url : "";
    done(finish(sourceId, spec, *raw, base, defaults));
  });
}

std::string tileUrl(const TileSet& set, int z, int x, int y) {
  const std::string& tmpl = set.tiles[static_cast<size_t>(x + y) % set.tiles.size()];
  const int yy = set.tms ? tmsY(z, y) : y;
  return expandTemplate(tmpl, z, x, yy, [&](const std::string& key) -> std::optional<std::string> {
    if (key == "quadkey") return quadkey(z, x, y);
    if (key == "bbox-epsg-3857") {
      const double n = std::exp2(z);
      const double size = kEarthCircumference / n;
      const double half = kEarthCircumference / 2;
      const double minX = x * size - half;
      const double maxY = half - y * size;
      char buf[160];
      std::snprintf(buf, sizeof buf, "%.17g,%.17g,%.17g,%.17g", minX, maxY - size, minX + size, maxY);
      return std::string(buf);
    }
    return std::nullopt;
  });
}

}
