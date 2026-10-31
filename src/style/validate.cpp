#include "style/validate.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <set>

#include "style/color.hpp"
#include "style/expression.hpp"
#include "style/layer.hpp"

namespace mapkern {

const std::vector<std::string> kFeatureFormats = {"geojson", "topojson", "kml",  "kmz",        "gpx", "csv",
                                                  "shapefile", "flatgeobuf", "wkt", "cot", "geopackage", "gml",
                                                  "osm",     "tcx",     "igc",  "nmea"};

namespace {

bool finite(const Value& v) { return v.isNumber() && std::isfinite(v.asNumber()); }

bool isLngLat(const Value& v) { return v.isArray() && v.size() == 2 && finite(v[0]) && finite(v[1]); }

bool in(const std::set<std::string>& s, const std::string& k) { return s.contains(k); }

const std::set<std::string> kSourceTypes = {"raster", "vector", "dem", "geojson", "image", "video", "canvas"};

const std::set<std::string>& sourceKeys(const std::string& type) {
  static const std::set<std::string> raster = {"type", "tiles", "url", "minZoom", "maxZoom", "bounds", "scheme", "attribution", "tileSize"};
  static const std::set<std::string> image = {"type", "url", "coordinates", "attribution"};
  static const std::set<std::string> video = {"type", "urls", "coordinates", "attribution"};
  static const std::set<std::string> canvas = {"type", "canvas", "coordinates", "animate", "attribution"};
  static const std::set<std::string> vector = {"type", "tiles", "url", "minZoom", "maxZoom", "bounds", "scheme", "attribution", "promoteId"};
  static const std::set<std::string> dem = {"type",   "tiles",       "url",      "minZoom",  "maxZoom",        "bounds",
                                            "scheme", "attribution", "tileSize", "encoding", "defaultEncoding"};
  static const std::set<std::string> geojson = {"type",          "data",       "format",   "maxZoom",          "tolerance",
                                                "buffer",        "attribution", "promoteId", "generateId",      "cluster",
                                                "clusterRadius", "clusterMaxZoom", "clusterMinPoints", "clusterProperties"};
  if (type == "raster") return raster;
  if (type == "image") return image;
  if (type == "video") return video;
  if (type == "canvas") return canvas;
  if (type == "vector") return vector;
  if (type == "dem") return dem;
  return geojson;
}

const std::set<std::string> kLayerKeys = {"id",     "type",  "source", "sourceLayer", "minZoom",      "maxZoom",
                                          "visible", "filter", "paint", "layout",      "sortKey", "renderingMode"};

const std::set<std::string> kStyleKeys = {"version", "name",    "center", "zoom",   "bearing", "pitch",  "sources", "layers",
                                          "terrain", "sky",     "sun",    "fonts",  "sprite",  "glyphs", "transition", "metadata"};

Issues checkExpression(const Value& value, const std::string& path, bool allowState, bool allowGeometry = false) {
  try {
    compile(value, OutputKind::Any, Value(), path, {allowState, allowGeometry});
    return {};
  } catch (const ExpressionError& e) {
    std::string message = e.what();
    const std::string prefix = path + ": ";
    if (message.starts_with(prefix)) message = message.substr(prefix.size());
    return {{path, message}};
  }
}

bool isSprite(const Value& v) {
  if (v.isString()) return !v.asString().empty();
  if (!v.isArray() || v.size() == 0) return false;
  std::set<std::string> ids;
  for (const auto& e : v.asArray()) {
    if (!e.isObject() || !e["id"].isString() || !e["url"].isString() || e["url"].asString().empty()) return false;
    if (!ids.insert(e["id"].asString()).second) return false;
  }
  return true;
}

bool isPromoteId(const Value& v, bool perLayer) {
  if (v.isString()) return !v.asString().empty();
  if (!perLayer || !v.isObject()) return false;
  for (const auto& [k, name] : v.asObject())
    if (!name.isString() || name.asString().empty()) return false;
  return true;
}

bool isEncoding(const Value& v) {
  if (v.isString()) return v.asString() == "terrarium" || v.asString() == "mapbox";
  if (!v.isObject() || v.size() != 4) return false;
  for (const char* k : {"red", "green", "blue", "offset"})
    if (!finite(v[k])) return false;
  return true;
}

Issues checkClusterProperties(const Value& value, const std::string& path) {
  if (!value.isObject()) return {{path + ".clusterProperties", "clusterProperties must be an object"}};
  Issues issues;
  for (const auto& [name, spec] : value.asObject()) {
    const std::string at = path + ".clusterProperties." + name;
    if (!spec.isArray() || spec.size() != 2) {
      issues.push_back({at, "each cluster property must be [operator, expression]"});
      continue;
    }
    const Value reducer = spec[0].isString() ? Value(Array{spec[0], Array{"accumulated"}, Array{"get", name}}) : spec[0];
    for (auto& i : checkExpression(spec[1], at, false)) issues.push_back(i);
    for (auto& i : checkExpression(reducer, at, false)) issues.push_back(i);
  }
  return issues;
}

Issues checkSourceType(LayerType type, const Value& source, const Value& layer, const std::string& path) {
  const std::string st = source["type"].stringOr("");
  if (type == LayerType::Raster && st != "raster" && st != "image" && st != "video" && st != "canvas")
    return {{path + ".source", "raster layers need a raster, image, video or canvas source"}};
  if (type == LayerType::Hillshade && st != "dem") return {{path + ".source", "hillshade layers need a dem source"}};
  const bool featureLayer = type == LayerType::Fill || type == LayerType::Line || type == LayerType::Circle ||
                            type == LayerType::Heatmap || type == LayerType::Symbol || type == LayerType::Extrusion;
  if (featureLayer) {
    if (st != "vector" && st != "geojson")
      return {{path + ".source", std::string(layerTypeName(type)) + " layers need a vector or geojson source"}};
    if (st == "vector" && !layer["sourceLayer"].isString())
      return {{path + ".sourceLayer", "layers on a vector source need sourceLayer"}};
  }
  return {};
}

Issues checkGroup(const Value& group, LayerType type, bool paint, const std::string& path) {
  if (group.isNull()) return {};
  if (!group.isObject()) return {{path, "must be an object"}};
  Issues issues;
  for (const auto& [key, value] : group.asObject()) {
    const PropertyDef* def = paint ? findPaint(type, key) : findLayout(type, key);
    if (!def && paint && transitionOf(type, key)) {
      if (auto problem = checkTransition(value)) issues.push_back({path + "." + key, *problem});
      continue;
    }
    if (!def) {
      issues.push_back({path + "." + key, "\"" + key + "\" is not a property of " + layerTypeName(type) + " layers"});
      continue;
    }
    if (def->expression) {
      std::optional<std::string> problem;
      if (def->check && isPlainValue(value, *def)) problem = def->check(value);
      if (problem) issues.push_back({path + "." + key, *problem});
      else
        for (auto& i : checkExpression(literalList(value), path + "." + key, paint)) issues.push_back(i);
    } else if (def->check) {
      if (auto problem = def->check(value)) issues.push_back({path + "." + key, *problem});
    }
  }
  return issues;
}

void append(Issues& a, Issues b) { a.insert(a.end(), b.begin(), b.end()); }

std::optional<double> parseIsoTime(const std::string& s) {
  int y = 0, mo = 1, d = 1, h = 0, mi = 0;
  double sec = 0;
  char tz[16] = {0};
  const int n = std::sscanf(s.c_str(), "%d-%d-%dT%d:%d:%lf%15s", &y, &mo, &d, &h, &mi, &sec, tz);
  if (n < 3) return std::nullopt;
  if (mo < 1 || mo > 12 || d < 1 || d > 31) return std::nullopt;
  auto days = [](int yy, unsigned m, unsigned dd) {
    yy -= m <= 2;
    const int era = (yy >= 0 ? yy : yy - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(yy - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + dd - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<int>(doe) - 719468;
  };
  double ms = (days(y, mo, d) * 86400.0 + h * 3600.0 + mi * 60.0 + sec) * 1000;
  std::string zone = tz;
  if (!zone.empty() && zone != "Z") {
    int zh = 0, zm = 0;
    char sign = zone[0];
    if ((sign == '+' || sign == '-') && std::sscanf(zone.c_str() + 1, "%d:%d", &zh, &zm) >= 1) {
      const double off = (zh * 60.0 + zm) * 60000;
      ms += sign == '+' ? -off : off;
    }
  }
  return ms;
}

}

std::optional<double> sunTime(const Value& time) {
  using namespace std::chrono;
  if (time.isNull() || (time.isString() && time.asString() == "now"))
    return static_cast<double>(duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
  if (time.isNumber()) return std::isfinite(time.asNumber()) ? std::optional<double>(time.asNumber()) : std::nullopt;
  if (time.isString()) return parseIsoTime(time.asString());
  return std::nullopt;
}

Issues validateSky(const Value& sky) {
  if (!sky.isObject()) return {{"sky", "sky must be an object"}};
  Issues issues;
  for (const auto& [key, value] : sky.asObject()) {
    OutputKind kind;
    if (key == "color" || key == "horizonColor" || key == "fogColor") kind = OutputKind::Color;
    else if (key == "fogBlend" || key == "horizonFogBlend" || key == "horizonBlend") kind = OutputKind::Number;
    else {
      issues.push_back({"sky." + key, "\"" + key + "\" is not a sky property"});
      continue;
    }
    if (kind == OutputKind::Number && value.isNumber() && !(value.asNumber() >= 0 && value.asNumber() <= 1)) {
      issues.push_back({"sky." + key, "must be between 0 and 1"});
      continue;
    }
    if (kind == OutputKind::Color && value.isString() && !parseColor(value.asString())) {
      issues.push_back({"sky." + key, "\"" + value.asString() + "\" is not a color"});
      continue;
    }
    append(issues, checkExpression(value, "sky." + key, false));
  }
  return issues;
}

Issues validateSun(const Value& sun) {
  if (!sun.isObject()) return {{"sun", "sun must be an object"}};
  Issues issues;
  for (const auto& [key, value] : sun.asObject()) {
    const std::string path = "sun." + key;
    if (key == "time") {
      if (!sunTime(value)) issues.push_back({path, "must be \"now\", a date and time like \"2026-06-21T20:00:00Z\", or milliseconds"});
    } else if (key == "night") {
      if (!value.isString() || !parseColor(value.asString())) issues.push_back({path, "must be a color"});
    } else if (key == "twilight") {
      if (!value.isNumber() || !(value.asNumber() >= 0.5 && value.asNumber() <= 18))
        issues.push_back({path, "must be a number of degrees from 0.5 to 18"});
    } else if (key == "shade") {
      if (!value.isBool()) issues.push_back({path, "must be true or false"});
    } else {
      issues.push_back({path, "\"" + key + "\" is not a sun property"});
    }
  }
  return issues;
}

Issues validateSource(const std::string& id, const Value& source) {
  const std::string path = "sources." + id;
  if (!source.isObject()) return {{path, "source must be an object"}};
  const std::string type = source["type"].stringOr("");
  if (!kSourceTypes.contains(type))
    return {{path + ".type", "source type must be raster, vector, dem, geojson, image, video or canvas"}};
  Issues issues;
  const auto& allowed = sourceKeys(type);
  for (const auto& [key, v] : source.asObject())
    if (!in(allowed, key)) issues.push_back({path + "." + key, "unknown " + type + " source property \"" + key + "\""});
  if (type == "image" || type == "video" || type == "canvas") {
    const Value& corners = source["coordinates"];
    bool ok = corners.isArray() && corners.size() == 4;
    if (ok)
      for (const auto& c : corners.asArray()) ok = ok && isLngLat(c);
    if (!ok)
      issues.push_back({path + ".coordinates", "coordinates must be four [lng, lat] corners: top left, top right, bottom right, bottom left"});
    if (type == "image" && (!source["url"].isString() || source["url"].asString().empty()))
      issues.push_back({path + ".url", "an image source needs a url"});
    if (type == "video") {
      const Value& urls = source["urls"];
      bool good = urls.isArray() && urls.size() > 0;
      if (good)
        for (const auto& u : urls.asArray()) good = good && u.isString();
      if (!good) issues.push_back({path + ".urls", "a video source needs a list of urls"});
    }
    if (type == "canvas" && !source["canvas"].isString() && !source["canvas"].isObject())
      issues.push_back({path + ".canvas", "canvas must be the id of a canvas element or the element itself"});
    return issues;
  }
  if (type == "geojson") {
    const Value& data = source["data"];
    if (!data.isString() && !data.isObject()) issues.push_back({path + ".data", "geojson data must be GeoJSON, a URL or a file"});
    if (source.has("format")) {
      const std::string f = source["format"].stringOr("");
      if (std::find(kFeatureFormats.begin(), kFeatureFormats.end(), f) == kFeatureFormats.end()) {
        std::string all;
        for (size_t i = 0; i < kFeatureFormats.size(); i++) all += (i ? ", " : "") + kFeatureFormats[i];
        issues.push_back({path + ".format", "format must be one of " + all});
      }
    }
    if (finite(source["maxZoom"]) && source["maxZoom"].asNumber() > 24)
      issues.push_back({path + ".maxZoom", "geojson maxZoom must be 24 or less"});
    if (finite(source["buffer"]) && source["buffer"].asNumber() > 512)
      issues.push_back({path + ".buffer", "buffer must be between 0 and 512 pixels"});
  } else {
    const Value& tiles = source["tiles"];
    bool hasTiles = tiles.isArray() && tiles.size() > 0;
    if (hasTiles)
      for (const auto& t : tiles.asArray()) hasTiles = hasTiles && t.isString();
    if (!hasTiles && !source["url"].isString()) issues.push_back({path, type + " source needs \"tiles\" or a TileJSON \"url\""});
    if (source.has("scheme") && source["scheme"].stringOr("") != "xyz" && source["scheme"].stringOr("") != "tms")
      issues.push_back({path + ".scheme", "scheme must be xyz or tms"});
    if (source.has("bounds")) {
      const Value& b = source["bounds"];
      bool ok = b.isArray() && b.size() == 4;
      if (ok)
        for (const auto& n : b.asArray()) ok = ok && finite(n);
      if (!ok) issues.push_back({path + ".bounds", "bounds must be [west, south, east, north]"});
    }
    if (type == "dem") {
      for (const char* key : {"encoding", "defaultEncoding"})
        if (source.has(key) && !isEncoding(source[key]))
          issues.push_back({path + "." + key, std::string(key) + " must be terrarium, mapbox or { red, green, blue, offset }"});
    }
  }
  if (source.has("promoteId") && !isPromoteId(source["promoteId"], type == "vector"))
    issues.push_back({path + ".promoteId", type == "vector" ? "promoteId must be a property name or { sourceLayer: propertyName }"
                                                            : "promoteId must be a property name"});
  if (source.has("generateId") && !source["generateId"].isBool())
    issues.push_back({path + ".generateId", "generateId must be true or false"});
  if (source.has("cluster") && !source["cluster"].isBool()) issues.push_back({path + ".cluster", "cluster must be true or false"});
  for (const char* key : {"clusterRadius", "clusterMaxZoom", "clusterMinPoints", "minZoom", "maxZoom", "tileSize", "tolerance", "buffer"}) {
    const Value& v = source[key];
    if (source.has(key) && (!finite(v) || v.asNumber() < 0))
      issues.push_back({path + "." + key, std::string(key) + " must be a non-negative number"});
  }
  if (source.has("clusterProperties")) append(issues, checkClusterProperties(source["clusterProperties"], path));
  return issues;
}

Issues validateLayer(const Value& layer, const std::string& path, const Value& sources) {
  if (!layer.isObject()) return {{path, "layer must be an object"}};
  Issues issues;
  if (!layer["id"].isString() || layer["id"].asString().empty()) issues.push_back({path + ".id", "layer id must be a non-empty string"});
  const auto type = layerTypeFromName(layer["type"].stringOr(""));
  if (!type) {
    issues.push_back({path + ".type",
                      "layer type must be one of custom, background, raster, fill, line, circle, heatmap, symbol, extrusion, hillshade"});
    return issues;
  }
  for (const auto& [key, v] : layer.asObject())
    if (!kLayerKeys.contains(key)) issues.push_back({path + "." + key, "unknown layer property \"" + key + "\""});
  if (*type == LayerType::Custom) {
    if (layer.has("renderingMode") && layer["renderingMode"].stringOr("") != "2d" && layer["renderingMode"].stringOr("") != "3d")
      issues.push_back({path + ".renderingMode", "renderingMode must be \"2d\" or \"3d\""});
  } else if (*type != LayerType::Background) {
    if (!layer["source"].isString()) {
      issues.push_back({path + ".source", std::string(layerTypeName(*type)) + " layer needs a source"});
    } else {
      const Value* source = sources.find(layer["source"].asString());
      if (!source) issues.push_back({path + ".source", "source \"" + layer["source"].asString() + "\" does not exist"});
      else append(issues, checkSourceType(*type, *source, layer, path));
    }
  }
  for (const char* key : {"minZoom", "maxZoom"})
    if (layer.has(key) && !finite(layer[key])) issues.push_back({path + "." + key, std::string(key) + " must be a number"});
  if (layer.has("visible") && !layer["visible"].isBool()) issues.push_back({path + ".visible", "visible must be true or false"});
  if (layer.has("filter")) append(issues, checkExpression(layer["filter"], path + ".filter", false, true));
  append(issues, checkGroup(layer["paint"], *type, true, path + ".paint"));
  append(issues, checkGroup(layer["layout"], *type, false, path + ".layout"));
  return issues;
}

Issues validateStyle(const Value& style) {
  if (!style.isObject()) return {{"", "style must be an object"}};
  Issues issues;
  if (!(style["version"].isNumber() && style["version"].asNumber() == 1)) issues.push_back({"version", "version must be 1"});
  for (const auto& [key, v] : style.asObject())
    if (!kStyleKeys.contains(key)) issues.push_back({key, "unknown style property \"" + key + "\""});
  if (style.has("sprite") && !isSprite(style["sprite"])) issues.push_back({"sprite", "sprite must be a URL or a list of { id, url }"});
  if (style.has("center") && !isLngLat(style["center"])) issues.push_back({"center", "center must be [lng, lat]"});
  for (const char* key : {"zoom", "bearing", "pitch"})
    if (style.has(key) && !finite(style[key])) issues.push_back({key, std::string(key) + " must be a number"});
  const Value& sources = style["sources"];
  if (!sources.isObject()) issues.push_back({"sources", "sources must be an object"});
  else
    for (const auto& [id, source] : sources.asObject()) append(issues, validateSource(id, source));
  const Value& layers = style["layers"];
  if (!layers.isArray()) {
    issues.push_back({"layers", "layers must be an array"});
  } else {
    std::set<std::string> ids;
    for (size_t i = 0; i < layers.size(); i++) {
      const std::string p = "layers[" + std::to_string(i) + "]";
      append(issues, validateLayer(layers[i], p, sources));
      if (layers[i]["id"].isString()) {
        if (!ids.insert(layers[i]["id"].asString()).second)
          issues.push_back({p + ".id", "duplicate layer id \"" + layers[i]["id"].asString() + "\""});
      }
    }
  }
  if (style.has("glyphs")) {
    const Value& g = style["glyphs"];
    if (!g.isString() || g.asString().find("{fontstack}") == std::string::npos || g.asString().find("{range}") == std::string::npos)
      issues.push_back({"glyphs", "glyphs must be a URL with {fontstack} and {range} in it"});
  }
  if (style.has("transition")) {
    if (auto problem = checkTransition(style["transition"])) issues.push_back({"transition", *problem});
  }
  if (style.has("sky")) append(issues, validateSky(style["sky"]));
  if (style.has("sun")) append(issues, validateSun(style["sun"]));
  if (style.has("terrain")) {
    const Value& t = style["terrain"];
    if (!t.isObject() || !t["source"].isString()) {
      issues.push_back({"terrain", "terrain must be { source, exaggeration? }"});
    } else if (sources.isObject() && sources[t["source"].asString()]["type"].stringOr("") != "dem") {
      issues.push_back({"terrain.source", "terrain source \"" + t["source"].asString() + "\" must be a dem source"});
    } else if (t.has("relief") && !(t["relief"].isNumber() && t["relief"].asNumber() >= 0 && t["relief"].asNumber() <= 1)) {
      issues.push_back({"terrain.relief", "terrain.relief must be a number from 0 to 1"});
    }
  }
  return issues;
}

std::string formatIssues(const Issues& issues) {
  std::string out;
  for (size_t i = 0; i < issues.size(); i++) {
    if (i) out += "\n";
    out += issues[i].path.empty() ? issues[i].message : issues[i].path + ": " + issues[i].message;
  }
  return out;
}

}
