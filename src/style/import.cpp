#include "style/import.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <set>

#include "core/url.hpp"
#include "style/expression.hpp"
#include "style/layer.hpp"
#include "style/validate.hpp"

namespace mapkern {

namespace {

struct Target {
  bool paint;
  const char* name;
};

using PropertyMap = std::vector<std::pair<const char*, Target>>;

Target P(const char* n) { return {true, n}; }
Target L(const char* n) { return {false, n}; }

const PropertyMap& properties(LayerType type) {
  static const PropertyMap none;
  static const PropertyMap background = {{"background-color", P("color")}, {"background-opacity", P("opacity")}};
  static const PropertyMap fill = {{"fill-antialias", P("antialias")}, {"fill-opacity", P("opacity")},
                                   {"fill-color", P("color")},         {"fill-outline-color", P("outlineColor")},
                                   {"fill-translate", P("translate")}, {"fill-translate-anchor", P("translateAnchor")},
                                   {"fill-pattern", P("pattern")}};
  static const PropertyMap line = {{"line-cap", L("cap")},          {"line-join", L("join")},
                                   {"line-miter-limit", L("miterLimit")}, {"line-opacity", P("opacity")},
                                   {"line-color", P("color")},      {"line-width", P("width")},
                                   {"line-gap-width", P("gapWidth")}, {"line-offset", P("offset")},
                                   {"line-blur", P("blur")},        {"line-dasharray", P("dash")}};
  static const PropertyMap circle = {{"circle-radius", P("radius")},
                                     {"circle-color", P("color")},
                                     {"circle-blur", P("blur")},
                                     {"circle-opacity", P("opacity")},
                                     {"circle-translate", P("translate")},
                                     {"circle-translate-anchor", P("translateAnchor")},
                                     {"circle-pitch-scale", P("pitchScale")},
                                     {"circle-pitch-alignment", P("pitchAlignment")},
                                     {"circle-stroke-width", P("strokeWidth")},
                                     {"circle-stroke-color", P("strokeColor")},
                                     {"circle-stroke-opacity", P("strokeOpacity")}};
  static const PropertyMap heatmap = {{"heatmap-radius", P("radius")},
                                      {"heatmap-weight", P("weight")},
                                      {"heatmap-intensity", P("intensity")},
                                      {"heatmap-color", P("color")},
                                      {"heatmap-opacity", P("opacity")}};
  static const PropertyMap extrusion = {{"fill-extrusion-opacity", P("opacity")},
                                        {"fill-extrusion-color", P("color")},
                                        {"fill-extrusion-height", P("height")},
                                        {"fill-extrusion-base", P("base")}};
  static const PropertyMap raster = {{"raster-opacity", P("opacity")},
                                     {"raster-hue-rotate", P("hueRotate")},
                                     {"raster-brightness-min", P("brightnessMin")},
                                     {"raster-brightness-max", P("brightnessMax")},
                                     {"raster-saturation", P("saturation")},
                                     {"raster-contrast", P("contrast")},
                                     {"raster-resampling", P("resampling")},
                                     {"raster-fade-duration", P("fadeDuration")}};
  static const PropertyMap hillshade = {{"hillshade-illumination-direction", P("illuminationDirection")},
                                        {"hillshade-illumination-anchor", P("illuminationAnchor")},
                                        {"hillshade-exaggeration", P("exaggeration")},
                                        {"hillshade-shadow-color", P("shadowColor")},
                                        {"hillshade-highlight-color", P("highlightColor")},
                                        {"hillshade-accent-color", P("accentColor")}};
  static const PropertyMap symbol = {{"symbol-placement", L("placement")},
                                     {"symbol-spacing", L("spacing")},
                                     {"icon-allow-overlap", L("iconAllowOverlap")},
                                     {"icon-ignore-placement", L("iconIgnorePlacement")},
                                     {"icon-optional", L("iconOptional")},
                                     {"icon-rotation-alignment", L("iconRotationAlignment")},
                                     {"icon-size", L("iconSize")},
                                     {"icon-image", L("icon")},
                                     {"icon-rotate", L("iconRotate")},
                                     {"icon-padding", L("iconPadding")},
                                     {"icon-offset", L("iconOffset")},
                                     {"icon-anchor", L("iconAnchor")},
                                     {"icon-text-fit", L("iconTextFit")},
                                     {"icon-text-fit-padding", L("iconTextFitPadding")},
                                     {"text-rotation-alignment", L("rotationAlignment")},
                                     {"text-pitch-alignment", L("pitchAlignment")},
                                     {"text-field", L("text")},
                                     {"text-size", L("size")},
                                     {"text-max-width", L("maxWidth")},
                                     {"text-line-height", L("lineHeight")},
                                     {"text-letter-spacing", L("letterSpacing")},
                                     {"text-justify", L("justify")},
                                     {"text-radial-offset", L("radialOffset")},
                                     {"text-variable-anchor", L("variableAnchor")},
                                     {"text-anchor", L("anchor")},
                                     {"text-max-angle", L("maxAngle")},
                                     {"text-rotate", L("rotate")},
                                     {"text-padding", L("padding")},
                                     {"text-keep-upright", L("keepUpright")},
                                     {"text-transform", L("transform")},
                                     {"text-offset", L("offset")},
                                     {"text-allow-overlap", L("allowOverlap")},
                                     {"text-ignore-placement", L("ignorePlacement")},
                                     {"text-optional", L("optional")},
                                     {"text-color", P("color")},
                                     {"text-halo-color", P("haloColor")},
                                     {"text-halo-width", P("haloWidth")},
                                     {"text-halo-blur", P("haloBlur")},
                                     {"text-opacity", P("opacity")},
                                     {"text-translate", P("translate")},
                                     {"text-translate-anchor", P("translateAnchor")},
                                     {"icon-color", P("iconColor")},
                                     {"icon-halo-color", P("iconHaloColor")},
                                     {"icon-halo-width", P("iconHaloWidth")},
                                     {"icon-opacity", P("iconOpacity")},
                                     {"icon-translate", P("iconTranslate")},
                                     {"icon-translate-anchor", P("iconTranslateAnchor")}};
  switch (type) {
    case LayerType::Background: return background;
    case LayerType::Fill: return fill;
    case LayerType::Line: return line;
    case LayerType::Circle: return circle;
    case LayerType::Heatmap: return heatmap;
    case LayerType::Extrusion: return extrusion;
    case LayerType::Raster: return raster;
    case LayerType::Hillshade: return hillshade;
    case LayerType::Symbol: return symbol;
    default: return none;
  }
}

const Target* findTarget(LayerType type, std::string_view key) {
  for (const auto& [k, t] : properties(type))
    if (key == k) return &t;
  return nullptr;
}

std::optional<LayerType> v8Type(const std::string& t) {
  if (t == "fill-extrusion") return LayerType::Extrusion;
  if (t == "background" || t == "fill" || t == "line" || t == "symbol" || t == "circle" || t == "heatmap" || t == "raster" ||
      t == "hillshade")
    return layerTypeFromName(t);
  return std::nullopt;
}

const std::set<std::string> kQuiet = {"line-round-limit", "fill-extrusion-vertical-gradient", "symbol-z-order",
                                      "icon-keep-upright", "symbol-avoid-edges"};
const char* kDroppedRoot[] = {"light", "lights", "imports", "models", "camera", "state", "schema"};
const std::set<std::string> kLegacyCompare = {"==", "!=", "<", "<=", ">", ">="};
const std::set<std::string> kStepped = {"line-dasharray", "text-variable-anchor", "text-font", "icon-image", "text-field"};

class Skipped {
 public:
  void add(const std::string& reason, const std::string& layer = "") {
    auto it = std::find_if(reasons_.begin(), reasons_.end(), [&](const auto& r) { return r.first == reason; });
    if (it == reasons_.end()) {
      reasons_.push_back({reason, {}});
      it = reasons_.end() - 1;
    }
    if (!layer.empty() && std::find(it->second.begin(), it->second.end(), layer) == it->second.end()) it->second.push_back(layer);
  }

  std::vector<std::string> list() const {
    std::vector<std::string> out;
    for (const auto& [reason, layers] : reasons_) {
      if (layers.empty()) {
        out.push_back(reason);
      } else if (layers.size() <= 3) {
        std::string s;
        for (size_t i = 0; i < layers.size(); i++) s += (i ? ", " : "") + layers[i];
        out.push_back(reason + " (" + s + ")");
      } else {
        out.push_back(reason + " (" + layers[0] + ", " + layers[1] + ", " + layers[2] + " and " +
                      std::to_string(layers.size() - 3) + " more)");
      }
    }
    return out;
  }

 private:
  std::vector<std::pair<std::string, std::vector<std::string>>> reasons_;
};

double numberOr(const Value& v, double fallback) { return v.isNumber() && std::isfinite(v.asNumber()) ? v.asNumber() : fallback; }

void put(Object& o, const std::string& key, Value v) { o = withKey(std::move(o), key, std::move(v)); }

const Value* get(const Object& o, std::string_view key) {
  for (const auto& [k, v] : o)
    if (k == key) return &v;
  return nullptr;
}

void erase(Object& o, std::string_view key) {
  o.erase(std::remove_if(o.begin(), o.end(), [&](const auto& kv) { return kv.first == key; }), o.end());
}

std::string absolute(const std::string& url, const std::string& base) {
  if (base.empty() || hasScheme(url)) return url;
  std::string r = resolveUrl(url, base);
  r = replaceAll(r, "%7B", "{");
  r = replaceAll(r, "%7b", "{");
  r = replaceAll(r, "%7D", "}");
  return replaceAll(r, "%7d", "}");
}

Value keyOf(const std::string& key) {
  if (key == "$type") return Array{"geometry-type"};
  if (key == "$id") return Array{"id"};
  return Array{"get", key};
}

bool isFunction(const Value& v) { return v.isObject() && (v["stops"].isArray() || v["type"].stringOr("") == "identity"); }

const std::regex* tokenRegexUnused = nullptr;

Value plainFormat(const Value& value) {
  if (!value.isArray()) return value;
  if (value[0].isString() && value[0].asString() == "literal") return value;
  if (value[0].isString() && value[0].asString() == "format") {
    Array parts;
    for (size_t i = 1; i < value.size(); i++) {
      const Value& section = value[i];
      if (section.isObject()) continue;
      if (section.isArray() && section[0].isString() && section[0].asString() == "image") continue;
      parts.push_back(plainFormat(section));
    }
    if (parts.empty()) return "";
    if (parts.size() == 1) return Array{"to-string", parts[0]};
    Array out{"concat"};
    out.insert(out.end(), parts.begin(), parts.end());
    return out;
  }
  Array out;
  for (const auto& v : value.asArray()) out.push_back(plainFormat(v));
  return out;
}

std::optional<std::vector<std::string>> fontList(const Value& value) {
  const Value& list = value.isArray() && value[0].isString() && value[0].asString() == "literal" && value[1].isArray() ? value[1] : value;
  if (!list.isArray() || list.size() == 0) return std::nullopt;
  std::vector<std::string> out;
  for (const auto& v : list.asArray()) {
    if (!v.isString()) return std::nullopt;
    out.push_back(v.asString());
  }
  if (isOperator(out[0])) return std::nullopt;
  return out;
}

std::optional<NamedFont> fontOf(const Value& value) {
  auto list = fontList(value);
  if (!list) return std::nullopt;
  return fontFromNames(*list);
}

Value familiesValue(const NamedFont& f) {
  Array a;
  for (const auto& s : f.families) a.emplace_back(s);
  return a;
}

Value fontLeaves(const Value& value, const std::function<Value(const NamedFont&)>& leaf) {
  if (auto f = fontOf(value)) return leaf(*f);
  if (!value.isArray()) return value;
  const Array& a = value.asArray();
  Array out;
  const bool match = a.size() > 0 && a[0].isString() && a[0].asString() == "match";
  for (size_t i = 0; i < a.size(); i++) {
    if (i == 0) out.push_back(a[i]);
    else if (match && i >= 2 && i < a.size() - 1 && i % 2 == 0) out.push_back(a[i]);
    else out.push_back(fontLeaves(a[i], leaf));
  }
  return out;
}

Value membership(const std::string& key, const Array& values) {
  const Value getter = keyOf(key);
  Array unique;
  for (const auto& v : values) {
    bool seen = false;
    for (const auto& u : unique) seen = seen || u.sameIdentity(v);
    if (!seen) unique.push_back(v);
  }
  if (unique.empty()) return false;
  bool strings = true, ints = true;
  for (const auto& v : unique) {
    strings = strings && v.isString();
    ints = ints && v.isNumber() && v.asNumber() == std::floor(v.asNumber());
  }
  if (strings || ints) return Array{"match", getter, Value(unique), true, false};
  Array any{"any"};
  for (const auto& v : unique) any.push_back(Array{"==", getter, v});
  return any;
}

bool isLegacyFilter(const Value& f) {
  if (!f.isArray() || f.size() == 0) return false;
  const std::string op = f[0].stringOr("");
  if (op == "has") return f.size() == 2 && f[1].isString() && (f[1].asString() == "$id" || f[1].asString() == "$type");
  if (op == "!has" || op == "!in" || op == "none") return true;
  if (op == "in") return f.size() >= 3 && f[1].isString() && !f[2].isArray();
  if (op == "all" || op == "any") {
    for (size_t i = 1; i < f.size(); i++)
      if (f[i].isArray() && isLegacyFilter(f[i])) return true;
    return false;
  }
  if (kLegacyCompare.contains(op)) return f.size() == 3 && f[1].isString() && !f[2].isArray();
  return false;
}

Value legacy(const Value& f) {
  const std::string op = f[0].asString();
  const Array& a = f.asArray();
  if (op == "all" || op == "any") {
    Array out{op};
    for (size_t i = 1; i < a.size(); i++) out.push_back(a[i].isArray() && isLegacyFilter(a[i]) ? legacy(a[i]) : a[i]);
    return out;
  }
  if (op == "none") {
    Array any{"any"};
    for (size_t i = 1; i < a.size(); i++) any.push_back(a[i].isArray() ? convertFilter(a[i]) : a[i]);
    return Array{"!", Value(any)};
  }
  if (op == "has") {
    const std::string k = f[1].stringOr("");
    if (k == "$type") return true;
    if (k == "$id") return Array{"!=", Array{"id"}, Value()};
    return Array{"has", f[1]};
  }
  if (op == "!has") return Array{"!", legacy(Array{"has", f[1]})};
  if (op == "in") return membership(f[1].asString(), Array(a.begin() + 2, a.end()));
  if (op == "!in") return Array{"!", membership(f[1].asString(), Array(a.begin() + 2, a.end()))};
  const Value getter = keyOf(f[1].asString());
  const Value& value = f[2];
  if (op == "==" || op == "!=") return Array{op, getter, value};
  if (value.isNumber()) return Array{"all", Array{"==", Array{"typeof", getter}, "number"}, Array{op, getter, value}};
  if (value.isString()) return Array{"all", Array{"==", Array{"typeof", getter}, "string"}, Array{op, getter, value}};
  return false;
}

std::vector<std::pair<double, Value>> ascending(const std::vector<std::pair<Value, Value>>& pairs) {
  std::vector<std::pair<double, Value>> out;
  for (const auto& [k, v] : pairs) {
    if (!k.isNumber()) continue;
    if (!out.empty() && k.asNumber() <= out.back().first) continue;
    out.emplace_back(k.asNumber(), v);
  }
  return out;
}

Value categorical(const Value& input, const std::vector<std::pair<Value, Value>>& pairs, const Value& fallback) {
  std::vector<std::pair<Value, Value>> unique;
  for (const auto& p : pairs) {
    bool seen = false;
    for (const auto& u : unique) seen = seen || u.first.sameIdentity(p.first);
    if (!seen) unique.push_back(p);
  }
  bool strings = true, ints = true;
  for (const auto& [k, v] : unique) {
    strings = strings && k.isString();
    ints = ints && k.isNumber() && k.asNumber() == std::floor(k.asNumber());
  }
  if (strings || ints) {
    Array out{"match", input};
    for (const auto& [k, v] : unique) {
      out.push_back(k);
      out.push_back(v);
    }
    out.push_back(fallback);
    return out;
  }
  Array out{"case"};
  for (const auto& [k, v] : unique) {
    out.push_back(Array{"==", input, k});
    out.push_back(v);
  }
  out.push_back(fallback);
  return out;
}

Value identity(const Value& input, const PropertyDef* def, const Value& fallback) {
  const OutputKind kind = def ? def->kind : OutputKind::Any;
  switch (kind) {
    case OutputKind::Color: return fallback.isNull() ? Value(Array{"to-color", input}) : Value(Array{"to-color", input, fallback});
    case OutputKind::Number: {
      const Value f = fallback.isNull() ? Value(0) : fallback;
      return Array{"coalesce", Array{"to-number", input, f}, f};
    }
    case OutputKind::String: return Array{"to-string", input};
    case OutputKind::Boolean: return Array{"to-boolean", input};
    default: return input;
  }
}

Value functionValue(const Value& fn, const PropertyDef* def, bool text, bool smooth) {
  auto output = [text](const Value& v) -> Value {
    if (text && v.isString()) return tokens(v.asString());
    if (v.isArray()) return Array{"literal", v};
    return v;
  };
  std::vector<std::pair<Value, Value>> stops;
  if (fn["stops"].isArray())
    for (const auto& s : fn["stops"].asArray())
      if (s.isArray() && s.size() == 2) stops.emplace_back(s[0], s[1]);
  const Value fallback = output(fn.has("default") ? fn["default"] : (def ? def->def : Value()));
  bool numeric = true;
  for (const auto& [k, v] : stops) {
    bool ok = v.isNumber();
    if (!ok && v.isArray()) {
      ok = true;
      for (const auto& n : v.asArray()) ok = ok && n.isNumber();
    }
    numeric = numeric && ok;
  }
  const bool interpolatable =
      smooth && (def ? def->kind == OutputKind::Number || def->kind == OutputKind::Color || (def->kind == OutputKind::Any && numeric) : numeric);
  const double base = numberOr(fn["base"], 1);
  const Value curve = base == 1 ? Value(Array{"linear"}) : Value(Array{"exponential", base});
  const std::string space = fn["colorSpace"].stringOr("");
  const std::string op = space == "lab" ? "interpolate-lab" : space == "hcl" ? "interpolate-hcl" : "interpolate";
  const bool hasProperty = fn["property"].isString();
  const Value input = hasProperty ? keyOf(fn["property"].asString()) : Value(Array{"zoom"});
  const bool keyed = !stops.empty() && stops[0].first.isObject();
  std::string type;
  if (fn["type"].isString()) {
    type = fn["type"].asString();
  } else {
    bool categoricalKeys = false;
    for (const auto& [k, v] : stops) categoricalKeys = categoricalKeys || k.isString() || k.isBool();
    type = hasProperty && categoricalKeys ? "categorical" : interpolatable ? "exponential" : "interval";
  }
  auto build = [&](const std::vector<std::pair<Value, Value>>& pairs, const Value& arg) -> Value {
    if (pairs.empty()) return fallback;
    if (type == "categorical") {
      std::vector<std::pair<Value, Value>> mapped;
      for (const auto& [k, v] : pairs) mapped.emplace_back(k, output(v));
      return categorical(arg, mapped, fallback);
    }
    const auto sorted = ascending(pairs);
    if (sorted.empty()) return fallback;
    Value body;
    if (type == "interval" || !interpolatable) {
      Array step{"step", arg, output(sorted[0].second)};
      for (size_t i = 1; i < sorted.size(); i++) {
        step.push_back(sorted[i].first);
        step.push_back(output(sorted[i].second));
      }
      body = step;
    } else {
      Array interp{op, curve, arg};
      for (const auto& [k, v] : sorted) {
        interp.push_back(k);
        interp.push_back(output(v));
      }
      body = interp;
    }
    if (!hasProperty || keyed) return body;
    return Array{"case", Array{"==", Array{"typeof", arg}, "number"}, body, fallback};
  };
  if (type == "identity") return identity(input, def, fallback);
  if (!keyed) return build(stops, input);
  std::map<double, std::vector<std::pair<Value, Value>>> byZoom;
  for (const auto& [k, v] : stops) byZoom[numberOr(k["zoom"], 0)].emplace_back(k["value"], v);
  if (byZoom.size() == 1) return build(byZoom.begin()->second, input);
  if (interpolatable) {
    Array out{op, curve, Array{"zoom"}};
    for (const auto& [z, pairs] : byZoom) {
      out.push_back(z);
      out.push_back(build(pairs, input));
    }
    return out;
  }
  Array out{"step", Array{"zoom"}, build(byZoom.begin()->second, input)};
  for (auto it = std::next(byZoom.begin()); it != byZoom.end(); ++it) {
    out.push_back(it->first);
    out.push_back(build(it->second, input));
  }
  return out;
}

Value convertValue(const Value& value, const PropertyDef* def, const std::string& key) {
  const bool text = key == "text-field" || key == "icon-image";
  if (isFunction(value)) return functionValue(value, def, text, !kStepped.contains(key));
  if (text && value.isString()) return tokens(value.asString());
  if (key == "text-field") return plainFormat(value);
  if (def && !def->expression && value.isArray() && value[0].isString() && value[0].asString() == "literal") return value[1];
  return value;
}

bool perFeature(const Value& value) {
  if (isFunction(value)) return value["property"].isString();
  if (!value.isArray() || !value[0].isString()) return false;
  try {
    return compile(value, OutputKind::String, "", "pattern").featureDependent;
  } catch (...) {
    return false;
  }
}

Value convertSky(const Value& input, Skipped& skipped) {
  Object sky;
  auto set = [&](const std::string& name, const Value& value) {
    PropertyDef def;
    def.kind = name.ends_with("olor") ? OutputKind::Color : OutputKind::Number;
    put(sky, name, convertValue(value, &def, name));
  };
  static const std::vector<std::pair<std::string, std::string>> skyNames = {
      {"sky-color", "color"},         {"horizon-color", "horizonColor"},       {"fog-color", "fogColor"},
      {"fog-ground-blend", "fogBlend"}, {"horizon-fog-blend", "horizonFogBlend"}, {"sky-horizon-blend", "horizonBlend"}};
  if (input["sky"].isObject()) {
    for (const auto& [key, value] : input["sky"].asObject()) {
      auto it = std::find_if(skyNames.begin(), skyNames.end(), [&](const auto& p) { return p.first == key; });
      if (it != skyNames.end()) set(it->second, value);
      else if (key != "atmosphere-blend") skipped.add("sky " + key + " isn't supported yet");
    }
  } else if (input["fog"].isObject()) {
    for (const auto& [key, value] : input["fog"].asObject()) {
      if (key == "color") {
        set("horizonColor", value);
        set("fogColor", value);
      } else if (key == "high-color") {
        set("color", value);
      } else if (key == "horizon-blend") {
        set("horizonBlend", value);
      } else {
        skipped.add("fog " + key + " isn't supported yet, the fog colors are used for the sky");
      }
    }
  }
  if (sky.empty()) return {};
  const Value out(sky);
  const auto issues = validateSky(out);
  if (!issues.empty()) {
    skipped.add("sky couldn't be read: " + issues[0].message);
    return {};
  }
  return out;
}

Object tiled(const std::string& type, const Value& raw, const std::string& base) {
  Object out;
  put(out, "type", type);
  if (raw["url"].isString()) put(out, "url", absolute(raw["url"].asString(), base));
  if (raw["tiles"].isArray()) {
    Array tiles;
    for (const auto& t : raw["tiles"].asArray()) tiles.emplace_back(absolute(t.toDisplayString(), base));
    put(out, "tiles", tiles);
    if (!raw.has("maxzoom")) put(out, "maxZoom", 22);
  }
  if (raw.has("minzoom")) put(out, "minZoom", raw["minzoom"]);
  if (raw.has("maxzoom")) put(out, "maxZoom", raw["maxzoom"]);
  for (const char* key : {"bounds", "scheme", "attribution"})
    if (raw.has(key)) put(out, key, raw[key]);
  return out;
}

Value convertSource(const std::string& id, const Value& raw, const std::string& base, Skipped& skipped) {
  if (!raw.isObject()) {
    skipped.add("source \"" + id + "\" isn't an object");
    return {};
  }
  std::vector<Value> urls{raw["url"], raw["data"]};
  if (raw["tiles"].isArray())
    for (const auto& t : raw["tiles"].asArray()) urls.push_back(t);
  for (const auto& u : urls)
    if (u.isString() && u.asString().starts_with("mapbox://")) {
      skipped.add("source \"" + id + "\" uses a mapbox:// address, which only Mapbox's servers understand");
      return {};
    }
  const std::string type = raw["type"].stringOr("");
  if (type == "vector") {
    if (raw.has("encoding") && raw["encoding"].stringOr("") != "mvt") {
      skipped.add("source \"" + id + "\" uses " + raw["encoding"].toDisplayString() + " tiles, which aren't supported yet");
      return {};
    }
    Object out = tiled("vector", raw, base);
    if (raw.has("promoteId")) put(out, "promoteId", raw["promoteId"]);
    return out;
  }
  if (type == "raster") {
    Object out = tiled("raster", raw, base);
    put(out, "tileSize", numberOr(raw["tileSize"], 512));
    return out;
  }
  if (type == "raster-dem") {
    Object out = tiled("dem", raw, base);
    put(out, "tileSize", numberOr(raw["tileSize"], 512));
    const std::string enc = raw["encoding"].stringOr("");
    if (enc == "terrarium" || enc == "mapbox") {
      put(out, "encoding", enc);
    } else if (enc == "custom") {
      put(out, "encoding",
          Object{{"red", numberOr(raw["redFactor"], 1)},
                 {"green", numberOr(raw["greenFactor"], 1)},
                 {"blue", numberOr(raw["blueFactor"], 1)},
                 {"offset", numberOr(raw["baseShift"], 0)}});
    } else {
      put(out, "defaultEncoding", "mapbox");
    }
    return out;
  }
  if (type == "geojson") {
    Object out;
    put(out, "type", "geojson");
    put(out, "data", raw["data"].isString() ? Value(absolute(raw["data"].asString(), base)) : raw["data"]);
    if (raw.has("maxzoom")) put(out, "maxZoom", raw["maxzoom"]);
    for (const char* key : {"attribution", "buffer", "tolerance", "generateId", "promoteId", "cluster", "clusterRadius",
                            "clusterMaxZoom", "clusterMinPoints", "clusterProperties"})
      if (raw.has(key)) put(out, key, raw[key]);
    if (raw["lineMetrics"].boolOr(false)) skipped.add("source \"" + id + "\": lineMetrics isn't supported yet");
    if (raw.has("filter")) skipped.add("source \"" + id + "\": a source filter isn't supported yet, use layer filters");
    return out;
  }
  if (type == "image") {
    Object out{{"type", "image"}, {"url", raw["url"].isString() ? absolute(raw["url"].asString(), base) : ""}, {"coordinates", raw["coordinates"]}};
    if (raw.has("attribution")) put(out, "attribution", raw["attribution"]);
    return out;
  }
  if (type == "video") {
    Array list;
    if (raw["urls"].isArray())
      for (const auto& u : raw["urls"].asArray()) list.emplace_back(absolute(u.toDisplayString(), base));
    Object out{{"type", "video"}, {"urls", list}, {"coordinates", raw["coordinates"]}};
    if (raw.has("attribution")) put(out, "attribution", raw["attribution"]);
    return out;
  }
  if (type == "canvas") {
    Object out{{"type", "canvas"}, {"canvas", raw["canvas"]}, {"coordinates", raw["coordinates"]}};
    if (raw.has("animate")) put(out, "animate", raw["animate"]);
    return out;
  }
  skipped.add("source \"" + id + "\" has type " + raw["type"].toDisplayString() + ", which isn't supported");
  return {};
}

struct Converted {
  Object layer;
  std::map<std::string, std::string> origin;
};

const Value& heatV8() {
  static const Value v = *parseJson(R"(["interpolate",["linear"],["heatmap-density"],0,"rgba(0, 0, 255, 0)",0.1,"royalblue",0.3,"cyan",0.5,"lime",0.7,"yellow",1,"red"])");
  return v;
}

std::optional<Converted> convertLayer(const Value& raw, const Value& sources, Skipped& skipped, bool serverFonts) {
  const std::string id = raw["id"].asString();
  const auto type = v8Type(raw["type"].stringOr(""));
  if (!type) {
    skipped.add(raw["type"].toDisplayString() + " layers aren't supported yet", id);
    return std::nullopt;
  }
  Converted c;
  Object& out = c.layer;
  put(out, "id", id);
  put(out, "type", layerTypeName(*type));
  if (*type != LayerType::Background) {
    if (!raw["source"].isString() || !sources.has(raw["source"].asString())) {
      skipped.add("layers whose source was left out", id);
      return std::nullopt;
    }
    put(out, "source", raw["source"]);
    if (raw.has("source-layer")) put(out, "sourceLayer", raw["source-layer"]);
  }
  if (raw.has("minzoom")) put(out, "minZoom", raw["minzoom"]);
  if (raw.has("maxzoom")) put(out, "maxZoom", raw["maxzoom"]);
  if (raw.has("filter")) put(out, "filter", convertFilter(raw["filter"]));
  Object paintOut, layoutOut;
  Value overlap, iconOverlap;
  bool hasOverlap = false, hasIconOverlap = false, pattern = false;
  std::vector<std::pair<std::string, Value>> entries;
  if (raw["paint"].isObject())
    for (const auto& kv : raw["paint"].asObject()) entries.push_back(kv);
  if (raw["layout"].isObject())
    for (const auto& kv : raw["layout"].asObject()) entries.push_back(kv);
  for (const auto& [key, value] : entries) {
    if (key == "visibility") {
      if (value.stringOr("") == "none") put(out, "visible", false);
      continue;
    }
    if (key.ends_with("-transition")) {
      const Target* target = findTarget(*type, key.substr(0, key.size() - 11));
      if (target && target->paint && transitionOf(*type, std::string(target->name) + "Transition")) {
        put(paintOut, std::string(target->name) + "Transition", value);
        c.origin["paint." + std::string(target->name) + "Transition"] = key;
      }
      continue;
    }
    if (key == "text-font") {
      const Value source = isFunction(value) ? functionValue(value, nullptr, false, false) : value;
      if (serverFonts) {
        put(layoutOut, "font", source);
        c.origin["layout.font"] = key;
        continue;
      }
      if (auto constant = fontOf(source)) {
        put(layoutOut, "font", familiesValue(*constant));
        if (constant->bold) put(layoutOut, "weight", "bold");
        if (constant->italic) put(layoutOut, "italic", true);
      } else {
        put(layoutOut, "font", fontLeaves(source, [](const NamedFont& f) { return Value(Array{"literal", familiesValue(f)}); }));
        put(layoutOut, "weight", fontLeaves(source, [](const NamedFont& f) { return Value(f.bold ? "bold" : "normal"); }));
        put(layoutOut, "italic", fontLeaves(source, [](const NamedFont& f) { return Value(f.italic); }));
      }
      for (const char* name : {"font", "weight", "italic"}) c.origin[std::string("layout.") + name] = key;
      continue;
    }
    if (key == "fill-sort-key" || key == "line-sort-key" || key == "circle-sort-key") {
      put(out, "sortKey", convertValue(value, nullptr, key));
      continue;
    }
    if (key == "symbol-sort-key") {
      const Value v = convertValue(value, nullptr, key);
      put(layoutOut, "priority", v.isNumber() ? Value(-v.asNumber()) : Value(Array{"*", -1, v}));
      c.origin["layout.priority"] = key;
      continue;
    }
    if (key == "text-overlap") {
      overlap = value;
      hasOverlap = true;
      continue;
    }
    if (key == "icon-overlap") {
      iconOverlap = value;
      hasIconOverlap = true;
      continue;
    }
    if (key == "fill-pattern") {
      if (perFeature(value)) {
        pattern = true;
        continue;
      }
    } else if (key == "line-pattern") {
      pattern = true;
      continue;
    } else if (key == "background-pattern" || key == "fill-extrusion-pattern") {
      skipped.add(key + " isn't drawn yet, the plain color is used", id);
      continue;
    } else if (key == "line-gradient") {
      skipped.add("line-gradient isn't drawn yet, the line color is used", id);
      continue;
    } else if (key == "hillshade-method") {
      if (value.stringOr("") != "standard") skipped.add("hillshade-method " + value.toDisplayString() + " is drawn as standard", id);
      continue;
    } else if (key == "icon-pitch-alignment") {
      if (value.stringOr("") == "map") skipped.add(key + " map isn't supported yet, icons face the screen", id);
      continue;
    }
    const Target* target = findTarget(*type, key);
    if (!target) {
      if (!kQuiet.contains(key)) skipped.add(key + " isn't supported yet", id);
      continue;
    }
    const PropertyDef* def = target->paint ? findPaint(*type, target->name) : findLayout(*type, target->name);
    const Value converted = convertValue(value, def, key);
    put(target->paint ? paintOut : layoutOut, target->name, converted);
    c.origin[std::string(target->paint ? "paint." : "layout.") + target->name] = key;
  }
  if (pattern) {
    skipped.add("line patterns and patterns that change per feature aren't drawn yet", id);
    return std::nullopt;
  }
  if (hasOverlap) {
    put(layoutOut, "allowOverlap", overlap.stringOr("") == "always");
    c.origin["layout.allowOverlap"] = "text-overlap";
    if (overlap.stringOr("") == "cooperative") skipped.add("text-overlap cooperative is treated as never", id);
  }
  if (hasIconOverlap) {
    put(layoutOut, "iconAllowOverlap", iconOverlap.stringOr("") == "always");
    c.origin["layout.iconAllowOverlap"] = "icon-overlap";
    if (iconOverlap.stringOr("") == "cooperative") skipped.add("icon-overlap cooperative is treated as never", id);
  }
  if (*type == LayerType::Symbol && get(layoutOut, "text")) {
    if (!get(layoutOut, "size")) put(layoutOut, "size", 16);
    if (!get(layoutOut, "font"))
      put(layoutOut, "font",
          serverFonts ? Value(Array{"Open Sans Regular", "Arial Unicode MS Regular"}) : Value(Array{"Open Sans", "Arial Unicode MS", "sans-serif"}));
  }
  if (*type == LayerType::Raster && !get(paintOut, "fadeDuration")) put(paintOut, "fadeDuration", 300);
  if (*type == LayerType::Hillshade) put(paintOut, "method", "standard");
  if (*type == LayerType::Circle && !get(paintOut, "pitchScale")) put(paintOut, "pitchScale", "map");
  if (*type == LayerType::Heatmap && !get(paintOut, "color")) put(paintOut, "color", heatV8());
  if (!paintOut.empty()) put(out, "paint", paintOut);
  if (!layoutOut.empty()) put(out, "layout", layoutOut);
  return c;
}

Value settle(Object layer, const std::map<std::string, std::string>& origin, const std::string& id, const Value& sources,
             Skipped& skipped) {
  for (int attempt = 0; attempt < 64; attempt++) {
    const auto issues = validateLayer(Value(layer), "layer", sources);
    if (issues.empty()) return layer;
    bool removed = false;
    for (const auto& issue : issues) {
      std::string group, name;
      for (const char* g : {"paint", "layout"}) {
        const std::string prefix = std::string("layer.") + g + ".";
        if (issue.path.starts_with(prefix)) {
          group = g;
          name = issue.path.substr(prefix.size());
          name = name.substr(0, name.find_first_of(".["));
        }
      }
      if (!group.empty()) {
        const Value* section = get(layer, group);
        if (section && section->has(name)) {
          Object s = section->asObject();
          erase(s, name);
          put(layer, group, s);
          auto o = origin.find(group + "." + name);
          skipped.add((o != origin.end() ? o->second : name) + " couldn't be used: " + issue.message, id);
          removed = true;
          continue;
        }
      }
      if (issue.path == "layer.sortKey" && get(layer, "sortKey")) {
        erase(layer, "sortKey");
        skipped.add("sort key couldn't be used: " + issue.message, id);
        removed = true;
      }
    }
    if (!removed) {
      skipped.add("layers that couldn't be read: " + issues[0].message, id);
      return {};
    }
  }
  return {};
}

Value withRef(const Value& layer, const Value* parent) {
  if (!parent || parent->has("ref")) return {};
  Object out = layer.asObject();
  erase(out, "ref");
  for (const char* key : {"type", "source", "source-layer", "minzoom", "maxzoom", "filter", "layout"})
    if (parent->has(key)) put(out, key, (*parent)[key]);
  return out;
}

Value spriteOf(const Value& sprite, const std::string& base) {
  if (sprite.isString()) return sprite.asString().starts_with("mapbox://") ? Value() : Value(absolute(sprite.asString(), base));
  if (sprite.isArray()) {
    Array out;
    for (const auto& s : sprite.asArray()) {
      if (!s.isObject() || !s["id"].isString() || !s["url"].isString()) continue;
      if (s["url"].asString().starts_with("mapbox://")) continue;
      out.push_back(Object{{"id", s["id"]}, {"url", absolute(s["url"].asString(), base)}});
    }
    return out.empty() ? Value() : Value(out);
  }
  return {};
}

std::string lowerAscii(std::string s) {
  for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

bool hasWord(const std::string& lower, const std::vector<std::string>& words) {
  for (const auto& w : words) {
    size_t pos = 0;
    while ((pos = lower.find(w, pos)) != std::string::npos) {
      const bool left = pos == 0 || !std::isalnum(static_cast<unsigned char>(lower[pos - 1]));
      const size_t end = pos + w.size();
      const bool right = end >= lower.size() || !std::isalnum(static_cast<unsigned char>(lower[end]));
      if (left && right) return true;
      pos++;
    }
  }
  return false;
}

}

NamedFont fontFromNames(const std::vector<std::string>& names) {
  static const std::vector<std::string> styleWords = {
      "regular", "normal",    "book",        "roman", "medium", "semi bold", "semibold", "demi bold", "demibold", "extra bold",
      "extrabold", "ultra bold", "ultrabold", "bold", "black", "heavy", "extra light", "extralight", "ultra light", "ultralight",
      "light", "thin", "hairline", "italic", "oblique", "condensed", "narrow"};
  NamedFont font;
  for (const auto& name : names) {
    std::string s = name;
    std::string lower = lowerAscii(s);
    bool changed = true;
    while (changed) {
      changed = false;
      for (const auto& w : styleWords) {
        size_t pos = 0;
        while ((pos = lower.find(w, pos)) != std::string::npos) {
          const bool left = pos == 0 || !std::isalnum(static_cast<unsigned char>(lower[pos - 1]));
          const size_t end = pos + w.size();
          const bool right = end >= lower.size() || !std::isalnum(static_cast<unsigned char>(lower[end]));
          if (left && right) {
            s.replace(pos, w.size(), " ");
            lower.replace(pos, w.size(), " ");
            changed = true;
          } else {
            pos++;
          }
        }
      }
    }
    std::string collapsed;
    for (char ch : s) {
      if (std::isspace(static_cast<unsigned char>(ch))) {
        if (!collapsed.empty() && collapsed.back() != ' ') collapsed += ' ';
      } else {
        collapsed += ch;
      }
    }
    while (!collapsed.empty() && collapsed.back() == ' ') collapsed.pop_back();
    if (!collapsed.empty() && std::find(font.families.begin(), font.families.end(), collapsed) == font.families.end())
      font.families.push_back(collapsed);
  }
  if (std::find(font.families.begin(), font.families.end(), "sans-serif") == font.families.end()) font.families.push_back("sans-serif");
  const std::string first = names.empty() ? "" : lowerAscii(names[0]);
  font.bold = hasWord(first, {"semi bold", "semibold", "demi bold", "demibold", "extra bold", "extrabold", "ultra bold", "ultrabold",
                              "bold", "black", "heavy"});
  font.italic = hasWord(first, {"italic", "oblique"});
  return font;
}

Value tokens(const std::string& text) {
  Array parts;
  size_t last = 0, i = 0;
  while (i < text.size()) {
    if (text[i] == '{') {
      const size_t close = text.find_first_of("{}", i + 1);
      if (close != std::string::npos && text[close] == '}' && close > i + 1) {
        if (i > last) parts.emplace_back(text.substr(last, i - last));
        parts.push_back(Array{"to-string", Array{"get", text.substr(i + 1, close - i - 1)}});
        last = close + 1;
        i = close + 1;
        continue;
      }
    }
    i++;
  }
  if (parts.empty()) return text;
  if (last < text.size()) parts.emplace_back(text.substr(last));
  if (parts.size() == 1) return parts[0];
  Array out{"concat"};
  out.insert(out.end(), parts.begin(), parts.end());
  return out;
}

Value convertFilter(const Value& filter) {
  if (!filter.isArray()) return filter;
  return isLegacyFilter(filter) ? legacy(filter) : filter;
}

bool isStyleV8(const Value& value) { return value.isObject() && value["version"].isNumber() && value["version"].asNumber() == 8; }

std::string serverGlyphs(const Value& glyphs) {
  return glyphs.isString() && !glyphs.asString().starts_with("mapbox://") ? glyphs.asString() : "";
}

ImportedLayer importLayer(const Value& raw, const Value& sources, bool serverFonts) {
  if (!raw.isObject() || !raw["id"].isString()) throw StyleError("A layer needs an id and a type");
  Skipped skipped;
  auto converted = convertLayer(raw, sources, skipped, serverFonts);
  Value layer = converted ? settle(converted->layer, converted->origin, raw["id"].asString(), sources, skipped) : Value();
  return {layer, skipped.list()};
}

ImportedSource importSource(const std::string& id, const Value& raw, const std::string& base) {
  Skipped skipped;
  Value source = convertSource(id, raw, base, skipped);
  if (source.isNull()) return {{}, skipped.list()};
  const auto issues = validateSource(id, source);
  if (!issues.empty()) {
    skipped.add("source \"" + id + "\" couldn't be read: " + issues[0].message);
    return {{}, skipped.list()};
  }
  return {source, skipped.list()};
}

ImportedStyle importStyle(const Value& input, const std::string& base) {
  if (!isStyleV8(input)) throw StyleError("importStyle reads styles written for the version 8 style format");
  Skipped skipped;
  Object style{{"version", 1}};
  for (const char* key : {"name", "center", "zoom", "bearing", "pitch", "metadata", "transition"})
    if (input.has(key)) put(style, key, input[key]);
  const std::string glyphs = serverGlyphs(input["glyphs"]);
  if (!glyphs.empty()) put(style, "glyphs", absolute(glyphs, base));
  else if (input.has("glyphs")) skipped.add("the glyphs use a mapbox:// address, so labels use the device's fonts");
  if (input.has("sprite")) {
    const Value sprite = spriteOf(input["sprite"], base);
    if (sprite.isNull()) skipped.add("the sprite uses a mapbox:// address, which only Mapbox's servers understand");
    else put(style, "sprite", sprite);
  }
  for (const char* key : kDroppedRoot)
    if (input.has(key)) skipped.add(std::string(key) + " isn't supported yet");
  if (input.has("projection")) skipped.add("projection is set on the map with setProjection, not in the style");
  const Value sky = convertSky(input, skipped);
  if (!sky.isNull()) put(style, "sky", sky);
  Object sources;
  if (input["sources"].isObject()) {
    for (const auto& [id, raw] : input["sources"].asObject()) {
      const Value source = convertSource(id, raw, base, skipped);
      if (source.isNull()) continue;
      const auto issues = validateSource(id, source);
      if (!issues.empty()) {
        skipped.add("source \"" + id + "\" couldn't be read: " + issues[0].message);
        continue;
      }
      put(sources, id, source);
    }
  }
  const Value sourcesValue(sources);
  if (input["terrain"].isObject() && input["terrain"]["source"].isString()) {
    const std::string src = input["terrain"]["source"].asString();
    if (sourcesValue[src]["type"].stringOr("") == "dem") {
      put(style, "terrain", Object{{"source", src}, {"exaggeration", numberOr(input["terrain"]["exaggeration"], 1)}});
    } else {
      skipped.add("terrain needs the raster-dem source \"" + src + "\"");
    }
  }
  Array layers;
  std::set<std::string> ids;
  const Value& list = input["layers"];
  std::map<std::string, const Value*> byId;
  if (list.isArray()) {
    for (const auto& raw : list.asArray())
      if (raw.isObject() && raw["id"].isString() && !byId.contains(raw["id"].asString())) byId[raw["id"].asString()] = &raw;
    for (const auto& original : list.asArray()) {
      if (!original.isObject() || !original["id"].isString() || ids.contains(original["id"].asString())) continue;
      const std::string id = original["id"].asString();
      Value raw = original;
      if (original["ref"].isString()) {
        auto it = byId.find(original["ref"].asString());
        raw = withRef(original, it == byId.end() ? nullptr : it->second);
        if (raw.isNull()) {
          skipped.add("layers whose ref names no other layer", id);
          continue;
        }
      }
      auto converted = convertLayer(raw, sourcesValue, skipped, !glyphs.empty());
      if (!converted) continue;
      Value checked = settle(converted->layer, converted->origin, id, sourcesValue, skipped);
      if (checked.isNull()) continue;
      ids.insert(id);
      layers.push_back(checked);
    }
  }
  put(style, "sources", sourcesValue);
  put(style, "layers", layers);
  for (int attempt = 0; attempt < 16; attempt++) {
    Issues root;
    for (const auto& issue : validateStyle(Value(style)))
      if (!issue.path.starts_with("layers") && !issue.path.starts_with("sources")) root.push_back(issue);
    if (root.empty()) break;
    for (const auto& issue : root) {
      const std::string key = issue.path.substr(0, issue.path.find('.'));
      if (get(style, key) && key != "version") {
        erase(style, key);
        skipped.add(key + " couldn't be used: " + issue.message);
      }
    }
  }
  return {Value(style), skipped.list()};
}

}
