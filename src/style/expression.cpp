#include "style/expression.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <numbers>
#include <unordered_map>

#include "core/utf8.hpp"
#include "style/color.hpp"

namespace mapkern {

const char* geometryTypeName(GeometryType t) {
  switch (t) {
    case GeometryType::Point: return "Point";
    case GeometryType::LineString: return "LineString";
    case GeometryType::Polygon: return "Polygon";
    default: return "Unknown";
  }
}

namespace {

const std::unordered_set<std::string_view>& ops() {
  static const std::unordered_set<std::string_view> all = {
      "literal", "get", "has", "id", "geometry-type", "zoom", "properties", "==", "!=", "<", "<=", ">", ">=", "all",
      "any", "!", "none", "in", "match", "case", "coalesce", "interpolate", "step", "+", "-", "*", "/", "%", "^",
      "abs", "min", "max", "round", "floor", "ceil", "sqrt", "ln", "log10", "log2", "sin", "cos", "tan", "pi", "e",
      "concat", "upcase", "downcase", "length", "slice", "to-string", "to-number", "to-boolean", "to-color", "rgb",
      "rgba", "typeof", "feature-state", "let", "var", "at", "index-of", "split", "number", "string", "boolean",
      "object", "array", "number-format", "hsl", "hsla", "to-rgba", "to-hsla", "interpolate-hcl", "interpolate-lab",
      "acos", "asin", "atan", "ln2", "is-supported-script", "image", "accumulated", "heatmap-density", "within"};
  return all;
}

struct Node {
  EvalFn fn;
  bool zoom = false;
  bool feature = false;
  bool state = false;
};

struct Scope {
  std::shared_ptr<std::unordered_set<std::string>> properties;
  std::string path;
  bool allowState = false;
  bool allowGeometry = false;
  std::map<std::string, Node> vars;
};

Node constant(Value v) {
  return {[v = std::move(v)](const EvalContext&) { return v; }, false, false, false};
}

const Object& emptyProps() {
  static const Object o;
  return o;
}

const Value* featureProperty(const EvalContext& ctx, std::string_view key) {
  if (!ctx.feature || !ctx.feature->properties) return nullptr;
  for (const auto& [k, v] : *ctx.feature->properties)
    if (k == key) return &v;
  return nullptr;
}

std::optional<Array> asList(const Value& v) {
  if (v.isArray()) return v.asArray();
  if (v.isColor()) {
    const auto& c = v.asColor();
    return Array{c[0], c[1], c[2], c[3]};
  }
  return std::nullopt;
}

bool isListLike(const Value& v) { return v.isArray() || v.isColor(); }

std::string stringify(const Value& v) {
  if (v.isNull()) return "";
  if (v.isString()) return v.asString();
  if (v.isNumber() || v.isBool()) return v.toDisplayString();
  return v.toJson();
}

const char* jsTypeOf(const Value& v) {
  switch (v.type()) {
    case Value::Type::Null: return "null";
    case Value::Type::Bool: return "boolean";
    case Value::Type::Number: return "number";
    case Value::Type::String: return "string";
    case Value::Type::Array:
    case Value::Type::Color: return "array";
    case Value::Type::Object: return "object";
  }
  return "null";
}

double jsRound(double x) { return std::floor(x + 0.5); }

Color hslToRgba(double h, double s, double l, double a) {
  const double hue = std::fmod(std::fmod(h, 360) + 360, 360) / 360;
  const double sat = std::clamp(s, 0.0, 1.0);
  const double light = std::clamp(l, 0.0, 1.0);
  const double q = light < 0.5 ? light * (1 + sat) : light + sat - light * sat;
  const double p = 2 * light - q;
  auto channel = [&](double t) {
    const double k = t < 0 ? t + 1 : t > 1 ? t - 1 : t;
    if (k < 1.0 / 6) return p + (q - p) * 6 * k;
    if (k < 1.0 / 2) return q;
    if (k < 2.0 / 3) return p + (q - p) * (2.0 / 3 - k) * 6;
    return p;
  };
  return {channel(hue + 1.0 / 3), channel(hue), channel(hue - 1.0 / 3), std::clamp(a, 0.0, 1.0)};
}

Array rgbaToHsla(const Color& c) {
  const double r = c[0], g = c[1], b = c[2], a = c[3];
  const double mx = std::max({r, g, b});
  const double mn = std::min({r, g, b});
  const double l = (mx + mn) / 2;
  if (mx == mn) return {0.0, 0.0, l * 100, a};
  const double d = mx - mn;
  const double s = l > 0.5 ? d / (2 - mx - mn) : d / (mx + mn);
  const double h = mx == r ? (g - b) / d + (g < b ? 6 : 0) : mx == g ? (b - r) / d + 2 : (r - g) / d + 4;
  return {h * 60, s * 100, l * 100, a};
}

constexpr double kLabWhite[3] = {0.95047, 1, 1.08883};

std::array<double, 4> toLab(const Color& c) {
  auto lin = [](double v) { return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); };
  const double r = lin(c[0]), g = lin(c[1]), b = lin(c[2]);
  auto f = [](double t) { return t > 216.0 / 24389 ? std::cbrt(t) : (24389.0 / 27 / 116) * t + 16.0 / 116; };
  const double x = f((0.4124564 * r + 0.3575761 * g + 0.1804375 * b) / kLabWhite[0]);
  const double y = f((0.2126729 * r + 0.7151522 * g + 0.072175 * b) / kLabWhite[1]);
  const double z = f((0.0193339 * r + 0.119192 * g + 0.9503041 * b) / kLabWhite[2]);
  return {116 * y - 16, 500 * (x - y), 200 * (y - z), c[3]};
}

Color fromLab(double l, double a, double b, double alpha) {
  const double fy = (l + 16) / 116;
  const double fx = fy + a / 500;
  const double fz = fy - b / 200;
  auto inv = [](double t) { return t * t * t > 216.0 / 24389 ? t * t * t : (t - 16.0 / 116) / (24389.0 / 27 / 116); };
  const double x = inv(fx) * kLabWhite[0];
  const double y = inv(fy) * kLabWhite[1];
  const double z = inv(fz) * kLabWhite[2];
  auto gamma = [](double v) {
    const double c = std::clamp(v, 0.0, 1.0);
    return c <= 0.0031308 ? 12.92 * c : 1.055 * std::pow(c, 1 / 2.4) - 0.055;
  };
  return {gamma(3.2404542 * x - 1.5371385 * y - 0.4985314 * z), gamma(-0.969266 * x + 1.8760108 * y + 0.041556 * z),
          gamma(0.0556434 * x - 0.2040259 * y + 1.0572252 * z), alpha};
}

enum class Space { Rgb, Lab, Hcl };

Color mixInSpace(const Color& a, const Color& b, double t, Space space) {
  const auto la = toLab(a);
  const auto lb = toLab(b);
  const double alpha = a[3] + (b[3] - a[3]) * t;
  if (space == Space::Lab)
    return fromLab(la[0] + (lb[0] - la[0]) * t, la[1] + (lb[1] - la[1]) * t, la[2] + (lb[2] - la[2]) * t, alpha);
  const double ca = std::hypot(la[1], la[2]);
  const double cb = std::hypot(lb[1], lb[2]);
  double ha = std::atan2(la[2], la[1]);
  double hb = std::atan2(lb[2], lb[1]);
  if (ca < 1e-6) ha = hb;
  if (cb < 1e-6) hb = ha;
  double dh = hb - ha;
  if (dh > std::numbers::pi) dh -= 2 * std::numbers::pi;
  if (dh < -std::numbers::pi) dh += 2 * std::numbers::pi;
  const double h = ha + dh * t;
  const double c = ca + (cb - ca) * t;
  return fromLab(la[0] + (lb[0] - la[0]) * t, c * std::cos(h), c * std::sin(h), alpha);
}

double exponentialT(double x, double base, double lo, double hi) {
  const double range = hi - lo;
  const double progress = x - lo;
  if (range == 0) return 0;
  if (base == 1) return progress / range;
  return (std::pow(base, progress) - 1) / (std::pow(base, range) - 1);
}

Value mix(const Value& a, const Value& b, double t, Space space) {
  if (a.isNumber() && b.isNumber()) return a.asNumber() + (b.asNumber() - a.asNumber()) * t;
  std::optional<Color> ca, cb;
  if (a.isString() || a.isColor() || (a.isArray() && a.size() == 4)) ca = toColor(a);
  if (b.isString() || b.isColor() || (b.isArray() && b.size() == 4)) cb = toColor(b);
  if (ca && cb) return space == Space::Rgb ? mixColor(*ca, *cb, t) : mixInSpace(*ca, *cb, t, space);
  if (a.isArray() && b.isArray() && a.size() == b.size()) {
    Array out;
    out.reserve(a.size());
    for (size_t i = 0; i < a.size(); i++) {
      if (a[i].isNumber() && b[i].isNumber()) out.emplace_back(a[i].asNumber() + (b[i].asNumber() - a[i].asNumber()) * t);
      else out.push_back(a[i]);
    }
    return out;
  }
  return t < 0.5 ? a : b;
}

std::function<double(double)> cubicBezier(double x1, double y1, double x2, double y2) {
  const double cx = 3 * x1, bx = 3 * (x2 - x1) - cx, ax = 1 - cx - bx;
  const double cy = 3 * y1, by = 3 * (y2 - y1) - cy, ay = 1 - cy - by;
  return [=](double x) {
    auto sampleX = [&](double t) { return ((ax * t + bx) * t + cx) * t; };
    auto sampleY = [&](double t) { return ((ay * t + by) * t + cy) * t; };
    auto slopeX = [&](double t) { return (3 * ax * t + 2 * bx) * t + cx; };
    double t = x;
    for (int i = 0; i < 8; i++) {
      const double err = sampleX(t) - x;
      if (std::abs(err) < 1e-6) break;
      const double d = slopeX(t);
      if (std::abs(d) < 1e-6) break;
      t -= err / d;
    }
    if (t < 0 || t > 1 || std::abs(sampleX(t) - x) > 1e-4) {
      double lo = 0, hi = 1;
      t = x;
      for (int i = 0; i < 30; i++) {
        const double v = sampleX(t);
        if (std::abs(v - x) < 1e-6) break;
        if (v < x) lo = t;
        else hi = t;
        t = (lo + hi) / 2;
      }
    }
    return sampleY(t);
  };
}

struct Area {
  std::vector<std::vector<std::vector<double>>> rings;
  double bounds[4];
};

bool insidePolygon(double x, double y, const std::vector<std::vector<double>>& rings) {
  bool inside = false;
  for (const auto& ring : rings) {
    const size_t n = ring.size();
    if (n < 2) continue;
    for (size_t i = 0, j = n - 2; i < n; j = i, i += 2) {
      const double xi = ring[i], yi = ring[i + 1], xj = ring[j], yj = ring[j + 1];
      if ((yi > y) != (yj > y) && x < (xj - xi) * (y - yi) / (yj - yi) + xi) inside = !inside;
    }
  }
  return inside;
}

bool crosses(double ax, double ay, double bx, double by, const std::vector<std::vector<double>>& rings) {
  auto side = [](double px, double py, double qx, double qy, double rx, double ry) {
    return (qx - px) * (ry - py) - (qy - py) * (rx - px);
  };
  for (const auto& ring : rings) {
    const size_t n = ring.size();
    if (n < 2) continue;
    for (size_t i = 0, j = n - 2; i < n; j = i, i += 2) {
      const double cx = ring[j], cy = ring[j + 1], dx = ring[i], dy = ring[i + 1];
      const double d1 = side(ax, ay, bx, by, cx, cy);
      const double d2 = side(ax, ay, bx, by, dx, dy);
      const double d3 = side(cx, cy, dx, dy, ax, ay);
      const double d4 = side(cx, cy, dx, dy, bx, by);
      if (d1 * d2 < 0 && d3 * d4 < 0) return true;
    }
  }
  return false;
}

bool insideArea(const WorldGeometry& geometry, const Area& area) {
  bool any = false;
  for (const auto& part : geometry.parts) {
    for (size_t i = 0; i + 1 < part.size(); i += 2) {
      const double x = part[i], y = part[i + 1];
      if (x < area.bounds[0] || x > area.bounds[2] || y < area.bounds[1] || y > area.bounds[3]) return false;
      bool in = false;
      for (const auto& rings : area.rings)
        if (insidePolygon(x, y, rings)) {
          in = true;
          break;
        }
      if (!in) return false;
      any = true;
      if (geometry.type != 1 && i + 3 < part.size()) {
        for (const auto& rings : area.rings)
          if (crosses(x, y, part[i + 2], part[i + 3], rings)) return false;
      }
    }
  }
  return any;
}

Area withinArea(const Value& value, const std::string& path) {
  std::vector<const Value*> polygons;
  std::function<void(const Value&)> visit = [&](const Value& node) {
    if (!node.isObject()) return;
    const std::string type = node["type"].stringOr("");
    if (type == "FeatureCollection" && node["features"].isArray()) {
      for (const auto& f : node["features"].asArray()) visit(f);
    } else if (type == "Feature") {
      visit(node["geometry"]);
    } else if (type == "Polygon" && node["coordinates"].isArray()) {
      polygons.push_back(&node["coordinates"]);
    } else if (type == "MultiPolygon" && node["coordinates"].isArray()) {
      for (const auto& p : node["coordinates"].asArray()) polygons.push_back(&p);
    }
  };
  visit(value);
  if (polygons.empty()) throw ExpressionError(path + ": \"within\" needs a GeoJSON Polygon or MultiPolygon");
  Area area{{}, {INFINITY, INFINITY, -INFINITY, -INFINITY}};
  for (const Value* polygon : polygons) {
    std::vector<std::vector<double>> rings;
    if (!polygon->isArray()) continue;
    for (const auto& ring : polygon->asArray()) {
      if (!ring.isArray()) throw ExpressionError(path + ": \"within\" has a ring that is not a list of positions");
      std::vector<double> out;
      for (const auto& pos : ring.asArray()) {
        const double lng = pos[0].numberOr(NAN), lat = pos[1].numberOr(NAN);
        if (!std::isfinite(lng) || !std::isfinite(lat))
          throw ExpressionError(path + ": \"within\" has a position that is not [lng, lat]");
        const double x = (lng + 180) / 360;
        const double s = std::sin(std::clamp(lat, -85.0511, 85.0511) * std::numbers::pi / 180);
        const double y = 0.5 - std::log((1 + s) / (1 - s)) / (4 * std::numbers::pi);
        out.push_back(x);
        out.push_back(y);
        area.bounds[0] = std::min(area.bounds[0], x);
        area.bounds[1] = std::min(area.bounds[1], y);
        area.bounds[2] = std::max(area.bounds[2], x);
        area.bounds[3] = std::max(area.bounds[3], y);
      }
      rings.push_back(std::move(out));
    }
    area.rings.push_back(std::move(rings));
  }
  return area;
}

struct NumberLocale {
  std::string group = ",";
  std::string decimal = ".";
  bool symbolAfter = false;
};

NumberLocale numberLocale(const std::string& tag) {
  const std::string lang = tag.substr(0, tag.find_first_of("-_"));
  NumberLocale l;
  static const char* dotGroup[] = {"de", "es", "it", "nl", "pt", "id", "da", "tr", "el", "ro", "hr", "sl", "vi"};
  static const char* spaceGroup[] = {"fr", "ru", "pl", "cs", "sv", "fi", "nb", "no", "uk", "sk", "hu", "bg", "lt", "lv", "et"};
  for (const char* d : dotGroup)
    if (lang == d) {
      l.group = ".";
      l.decimal = ",";
      l.symbolAfter = true;
    }
  for (const char* d : spaceGroup)
    if (lang == d) {
      l.group = "\xe2\x80\xaf";
      l.decimal = ",";
      l.symbolAfter = true;
    }
  if (tag == "pt-BR") l.symbolAfter = false;
  return l;
}

std::string formatFixed(double v, int minDigits, int maxDigits, const NumberLocale& locale) {
  const bool negative = v < 0;
  v = std::abs(v);
  const double scale = std::pow(10.0, maxDigits);
  const double rounded = std::floor(v * scale + 0.5) / scale;
  char buf[400];
  std::snprintf(buf, sizeof buf, "%.*f", maxDigits, rounded);
  std::string s = buf;
  std::string intPart = s, frac;
  if (const size_t dot = s.find('.'); dot != std::string::npos) {
    intPart = s.substr(0, dot);
    frac = s.substr(dot + 1);
  }
  while (static_cast<int>(frac.size()) > minDigits && !frac.empty() && frac.back() == '0') frac.pop_back();
  std::string grouped;
  const size_t n = intPart.size();
  for (size_t i = 0; i < n; i++) {
    grouped += intPart[i];
    const size_t left = n - 1 - i;
    if (left > 0 && left % 3 == 0) grouped += locale.group;
  }
  std::string out = (negative && rounded != 0 ? "-" : "") + grouped;
  if (!frac.empty()) out += locale.decimal + frac;
  return out;
}

std::string matchKey(const Value& v) {
  if (v.isString()) return "s" + v.asString();
  if (v.isNumber()) return "n" + formatNumber(v.asNumber());
  if (v.isBool()) return v.asBool() ? "bt" : "bf";
  return "";
}

Node build(const Value& value, Scope& scope);

using Nodes = std::vector<std::shared_ptr<Node>>;

Node combineAll(const Nodes& nodes, EvalFn fn) {
  Node out{std::move(fn), false, false, false};
  for (const auto& n : nodes) {
    out.zoom = out.zoom || n->zoom;
    out.feature = out.feature || n->feature;
    out.state = out.state || n->state;
  }
  return out;
}

Node buildInterpolate(const Array& args, Scope& scope, Space space) {
  if (args.size() < 4 || args.size() % 2 != 0)
    throw ExpressionError(scope.path + ": \"interpolate\" expects a type, an input and stop/output pairs");
  const Value& type = args[0];
  if (!type.isArray() || !type[0].isString())
    throw ExpressionError(scope.path +
                          ": \"interpolate\" type must be [\"linear\"], [\"exponential\", base] or "
                          "[\"cubic-bezier\", x1, y1, x2, y2]");
  std::function<double(double, double, double, double)> curve;
  const std::string& kind = type[0].asString();
  if (kind == "linear") {
    curve = [](double t, double, double, double) { return t; };
  } else if (kind == "exponential") {
    const double base = type[1].numberOr(1);
    curve = [base](double, double lo, double hi, double x) { return exponentialT(x, base, lo, hi); };
  } else if (kind == "cubic-bezier") {
    auto bez = cubicBezier(type[1].numberOr(0), type[2].numberOr(0), type[3].numberOr(1), type[4].numberOr(1));
    curve = [bez](double t, double, double, double) { return bez(t); };
  } else {
    throw ExpressionError(scope.path + ": unknown interpolation \"" + kind + "\"");
  }
  auto input = std::make_shared<Node>(build(args[1], scope));
  std::vector<double> stops;
  Nodes outs;
  for (size_t i = 2; i < args.size(); i += 2) {
    if (!args[i].isNumber()) throw ExpressionError(scope.path + ": \"interpolate\" stops must be numbers");
    const double stop = args[i].asNumber();
    if (!stops.empty() && stop <= stops.back()) throw ExpressionError(scope.path + ": \"interpolate\" stops must increase");
    stops.push_back(stop);
    outs.push_back(std::make_shared<Node>(build(args[i + 1], scope)));
  }
  Nodes all = outs;
  all.push_back(input);
  return combineAll(all, [input, stops, outs, curve, space](const EvalContext& ctx) -> Value {
    const Value xv = input->fn(ctx);
    if (!xv.isNumber()) return {};
    const double x = xv.asNumber();
    const size_t n = stops.size();
    if (x <= stops[0]) return outs[0]->fn(ctx);
    if (x >= stops[n - 1]) return outs[n - 1]->fn(ctx);
    size_t i = 0;
    while (i < n - 1 && x > stops[i + 1]) i++;
    const double lo = stops[i], hi = stops[i + 1];
    const double t = curve((x - lo) / (hi - lo), lo, hi, x);
    return mix(outs[i]->fn(ctx), outs[i + 1]->fn(ctx), t, space);
  });
}

using Unary = double (*)(double);

Node build(const Value& value, Scope& scope) {
  if (value.isNull() || value.isNumber() || value.isString() || value.isBool() || value.isObject() || value.isColor())
    return constant(value);
  const Array& arr = value.asArray();
  if (arr.empty() || !arr[0].isString()) return constant(value);
  const std::string& op = arr[0].asString();
  if (!ops().contains(op))
    throw ExpressionError(scope.path + ": unknown expression \"" + op +
                          "\". To use a list of values as it is, write [\"literal\", [...]]");
  const Array args(arr.begin() + 1, arr.end());
  auto sub = [&](const Value& v) { return std::make_shared<Node>(build(v, scope)); };
  auto many = [&](size_t from = 0) {
    Nodes nodes;
    for (size_t i = from; i < args.size(); i++) nodes.push_back(sub(args[i]));
    return nodes;
  };
  auto need = [&](size_t mn, size_t mx = SIZE_MAX) {
    if (args.size() < mn || args.size() > mx)
      throw ExpressionError(scope.path + ": \"" + op + "\" expects " +
                            (mn == mx ? std::to_string(mn) : std::to_string(mn) + "+") + " arguments");
  };

  if (op == "literal") {
    need(1, 1);
    return constant(args[0]);
  }
  if (op == "zoom") {
    need(0, 0);
    return {[](const EvalContext& ctx) { return Value(ctx.zoom); }, true, false, false};
  }
  if (op == "id")
    return {[](const EvalContext& ctx) { return ctx.feature ? ctx.feature->id : Value(); }, false, true, false};
  if (op == "geometry-type")
    return {[](const EvalContext& ctx) {
              return Value(geometryTypeName(ctx.feature ? ctx.feature->geometryType : GeometryType::Unknown));
            },
            false, true, false};
  if (op == "properties")
    return {[](const EvalContext& ctx) {
              return Value(ctx.feature && ctx.feature->properties ? *ctx.feature->properties : emptyProps());
            },
            false, true, false};
  if (op == "get") {
    need(1, 2);
    if (args.size() == 1 && args[0].isString()) {
      const std::string key = args[0].asString();
      scope.properties->insert(key);
      return {[key](const EvalContext& ctx) {
                const Value* v = featureProperty(ctx, key);
                return v ? *v : Value();
              },
              false, true, false};
    }
    auto key = sub(args[0]);
    std::shared_ptr<Node> obj = args.size() > 1 ? sub(args[1]) : nullptr;
    return {[key, obj](const EvalContext& ctx) -> Value {
              const Value k = key->fn(ctx);
              if (!k.isString()) return {};
              if (obj) {
                const Value source = obj->fn(ctx);
                const Value* v = source.find(k.asString());
                return v ? *v : Value();
              }
              const Value* v = featureProperty(ctx, k.asString());
              return v ? *v : Value();
            },
            key->zoom || (obj && obj->zoom), true, false};
  }
  if (op == "has") {
    need(1, 2);
    if (args.size() == 1 && args[0].isString()) {
      const std::string key = args[0].asString();
      scope.properties->insert(key);
      return {[key](const EvalContext& ctx) { return Value(featureProperty(ctx, key) != nullptr); }, false, true, false};
    }
    auto key = sub(args[0]);
    return {[key](const EvalContext& ctx) {
              const Value k = key->fn(ctx);
              return Value(k.isString() && featureProperty(ctx, k.asString()) != nullptr);
            },
            key->zoom, true, false};
  }
  if (op == "==" || op == "!=") {
    need(2, 2);
    auto nodes = many();
    const bool eq = op == "==";
    return combineAll(nodes, [a = nodes[0], b = nodes[1], eq](const EvalContext& ctx) {
      return Value(a->fn(ctx).sameIdentity(b->fn(ctx)) == eq);
    });
  }
  if (op == "<" || op == "<=" || op == ">" || op == ">=") {
    need(2, 2);
    auto nodes = many();
    const int which = op == "<" ? 0 : op == "<=" ? 1 : op == ">" ? 2 : 3;
    return combineAll(nodes, [a = nodes[0], b = nodes[1], which](const EvalContext& ctx) {
      const Value x = a->fn(ctx);
      const Value y = b->fn(ctx);
      int c;
      if (x.isNumber() && y.isNumber()) {
        const double p = x.asNumber(), q = y.asNumber();
        if (std::isnan(p) || std::isnan(q)) return Value(false);
        c = p < q ? -1 : p > q ? 1 : 0;
      } else if (x.isString() && y.isString()) {
        const int r = x.asString().compare(y.asString());
        c = r < 0 ? -1 : r > 0 ? 1 : 0;
      } else {
        return Value(false);
      }
      switch (which) {
        case 0: return Value(c < 0);
        case 1: return Value(c <= 0);
        case 2: return Value(c > 0);
        default: return Value(c >= 0);
      }
    });
  }
  if (op == "all" || op == "any" || op == "none") {
    auto nodes = many();
    const int which = op == "all" ? 0 : op == "any" ? 1 : 2;
    return combineAll(nodes, [nodes, which](const EvalContext& ctx) {
      if (which == 0) {
        for (const auto& n : nodes) {
          const Value v = n->fn(ctx);
          if (!(v.isBool() && v.asBool())) return Value(false);
        }
        return Value(true);
      }
      bool any = false;
      for (const auto& n : nodes) {
        const Value v = n->fn(ctx);
        if (v.isBool() && v.asBool()) {
          any = true;
          break;
        }
      }
      return Value(which == 1 ? any : !any);
    });
  }
  if (op == "!") {
    need(1, 1);
    auto a = sub(args[0]);
    return combineAll({a}, [a](const EvalContext& ctx) {
      const Value v = a->fn(ctx);
      return Value(!(v.isBool() && v.asBool()));
    });
  }
  if (op == "in") {
    need(2, 2);
    auto needle = sub(args[0]);
    if (args[1].isArray() && !isExpression(args[1])) {
      std::unordered_set<std::string> set;
      bool hasNull = false;
      for (const auto& item : args[1].asArray()) {
        if (item.isNull()) hasNull = true;
        const std::string k = matchKey(item);
        if (!k.empty()) set.insert(k);
      }
      return combineAll({needle}, [needle, set, hasNull](const EvalContext& ctx) {
        const Value v = needle->fn(ctx);
        if (v.isNull()) return Value(hasNull);
        const std::string k = matchKey(v);
        return Value(!k.empty() && set.contains(k));
      });
    }
    auto haystack = sub(args[1]);
    return combineAll({needle, haystack}, [needle, haystack](const EvalContext& ctx) {
      const Value n = needle->fn(ctx);
      const Value h = haystack->fn(ctx);
      if (auto list = asList(h)) {
        for (const auto& item : *list)
          if (item.sameIdentity(n)) return Value(true);
        return Value(false);
      }
      if (h.isString() && n.isString()) return Value(h.asString().find(n.asString()) != std::string::npos);
      return Value(false);
    });
  }
  if (op == "match") {
    if (args.size() < 4 || args.size() % 2 != 0)
      throw ExpressionError(scope.path + ": \"match\" expects input, label/output pairs and a fallback");
    auto input = sub(args[0]);
    Nodes outputs;
    std::unordered_map<std::string, size_t> table;
    for (size_t i = 1; i + 1 < args.size(); i += 2) {
      outputs.push_back(sub(args[i + 1]));
      const Value& label = args[i];
      std::vector<Value> labels = label.isArray() ? label.asArray() : std::vector<Value>{label};
      for (const auto& l : labels) {
        if (!l.isString() && !l.isNumber() && !l.isBool())
          throw ExpressionError(scope.path + ": \"match\" labels must be literals");
        table.emplace(matchKey(l), outputs.size() - 1);
      }
    }
    auto otherwise = sub(args.back());
    Nodes all = outputs;
    all.push_back(input);
    all.push_back(otherwise);
    return combineAll(all, [input, table, outputs, otherwise](const EvalContext& ctx) {
      const Value v = input->fn(ctx);
      const std::string k = matchKey(v);
      if (!k.empty()) {
        if (auto it = table.find(k); it != table.end()) return outputs[it->second]->fn(ctx);
      }
      return otherwise->fn(ctx);
    });
  }
  if (op == "case") {
    if (args.size() < 3 || args.size() % 2 != 1)
      throw ExpressionError(scope.path + ": \"case\" expects condition/output pairs and a fallback");
    Nodes conds, outs;
    for (size_t i = 0; i + 1 < args.size(); i += 2) {
      conds.push_back(sub(args[i]));
      outs.push_back(sub(args[i + 1]));
    }
    auto otherwise = sub(args.back());
    Nodes all = conds;
    all.insert(all.end(), outs.begin(), outs.end());
    all.push_back(otherwise);
    return combineAll(all, [conds, outs, otherwise](const EvalContext& ctx) {
      for (size_t i = 0; i < conds.size(); i++) {
        const Value c = conds[i]->fn(ctx);
        if (c.isBool() && c.asBool()) return outs[i]->fn(ctx);
      }
      return otherwise->fn(ctx);
    });
  }
  if (op == "coalesce") {
    auto nodes = many();
    return combineAll(nodes, [nodes](const EvalContext& ctx) {
      for (const auto& n : nodes) {
        Value v = n->fn(ctx);
        if (!v.isNull()) return v;
      }
      return Value();
    });
  }
  if (op == "step") {
    if (args.size() < 2 || args.size() % 2 != 0)
      throw ExpressionError(scope.path + ": \"step\" expects input, a base output and stop/output pairs");
    auto input = sub(args[0]);
    auto base = sub(args[1]);
    std::vector<double> stops;
    Nodes outs;
    for (size_t i = 2; i < args.size(); i += 2) {
      if (!args[i].isNumber()) throw ExpressionError(scope.path + ": \"step\" stops must be numbers");
      stops.push_back(args[i].asNumber());
      outs.push_back(sub(args[i + 1]));
    }
    Nodes all = outs;
    all.push_back(input);
    all.push_back(base);
    return combineAll(all, [input, base, stops, outs](const EvalContext& ctx) {
      const Value x = input->fn(ctx);
      if (!x.isNumber()) return base->fn(ctx);
      const Node* chosen = base.get();
      for (size_t i = 0; i < stops.size(); i++) {
        if (x.asNumber() >= stops[i]) chosen = outs[i].get();
        else break;
      }
      return chosen->fn(ctx);
    });
  }
  if (op == "interpolate") return buildInterpolate(args, scope, Space::Rgb);
  if (op == "interpolate-hcl") return buildInterpolate(args, scope, Space::Hcl);
  if (op == "interpolate-lab") return buildInterpolate(args, scope, Space::Lab);
  if (op == "feature-state") {
    need(1, 1);
    if (!scope.allowState) throw ExpressionError(scope.path + ": \"feature-state\" only works in paint properties");
    auto key = sub(args[0]);
    return {[key](const EvalContext& ctx) -> Value {
              const Value k = key->fn(ctx);
              if (!k.isString() || !ctx.state) return {};
              for (const auto& [name, v] : *ctx.state)
                if (name == k.asString()) return v;
              return {};
            },
            key->zoom, true, true};
  }
  if (op == "let") {
    if (args.size() < 3 || args.size() % 2 != 1)
      throw ExpressionError(scope.path + ": \"let\" expects name/value pairs and then an expression");
    const auto saved = scope.vars;
    Nodes bound;
    for (size_t i = 0; i + 1 < args.size(); i += 2) {
      if (!args[i].isString()) throw ExpressionError(scope.path + ": \"let\" names must be strings");
      auto v = sub(args[i + 1]);
      scope.vars[args[i].asString()] = *v;
      bound.push_back(v);
    }
    auto body = sub(args.back());
    scope.vars = saved;
    bound.push_back(body);
    return combineAll(bound, body->fn);
  }
  if (op == "var") {
    need(1, 1);
    if (args[0].isString()) {
      if (auto it = scope.vars.find(args[0].asString()); it != scope.vars.end()) return it->second;
    }
    throw ExpressionError(scope.path + ": \"var\" refers to an unknown name " + args[0].toJson());
  }
  if (op == "at") {
    need(2, 2);
    auto nodes = many();
    return combineAll(nodes, [index = nodes[0], list = nodes[1]](const EvalContext& ctx) -> Value {
      const Value i = index->fn(ctx);
      const auto a = asList(list->fn(ctx));
      if (!a || !i.isNumber()) return {};
      const double d = i.asNumber();
      if (d != std::floor(d) || d < 0 || d >= static_cast<double>(a->size())) return {};
      return (*a)[static_cast<size_t>(d)];
    });
  }
  if (op == "index-of") {
    need(2, 3);
    auto nodes = many();
    return combineAll(nodes, [nodes](const EvalContext& ctx) -> Value {
      const Value needle = nodes[0]->fn(ctx);
      const Value haystack = nodes[1]->fn(ctx);
      double start = 0;
      if (nodes.size() > 2) start = nodes[2]->fn(ctx).numberOr(0);
      if (haystack.isString() && needle.isString()) {
        const auto h = utf8::decode(haystack.asString());
        const auto n = utf8::decode(needle.asString());
        size_t from = static_cast<size_t>(std::max(0.0, start));
        if (n.empty()) return static_cast<double>(std::min(from, h.size()));
        for (size_t i = from; i + n.size() <= h.size(); i++)
          if (std::equal(n.begin(), n.end(), h.begin() + i)) return static_cast<double>(i);
        return -1.0;
      }
      if (auto list = asList(haystack)) {
        for (size_t i = static_cast<size_t>(std::max(0.0, start)); i < list->size(); i++)
          if ((*list)[i].sameIdentity(needle)) return static_cast<double>(i);
      }
      return -1.0;
    });
  }
  if (op == "split") {
    need(2, 2);
    auto nodes = many();
    return combineAll(nodes, [text = nodes[0], sep = nodes[1]](const EvalContext& ctx) -> Value {
      const Value t = text->fn(ctx);
      const Value s = sep->fn(ctx);
      if (!t.isString() || !s.isString()) return {};
      Array out;
      const std::string& str = t.asString();
      const std::string& d = s.asString();
      if (d.empty()) {
        for (uint32_t cp : utf8::decode(str)) {
          std::string piece;
          utf8::append(piece, cp);
          out.emplace_back(piece);
        }
        return out;
      }
      size_t pos = 0;
      while (true) {
        const size_t next = str.find(d, pos);
        if (next == std::string::npos) {
          out.emplace_back(str.substr(pos));
          break;
        }
        out.emplace_back(str.substr(pos, next - pos));
        pos = next + d.size();
      }
      return out;
    });
  }
  if (op == "number" || op == "string" || op == "boolean" || op == "object") {
    need(1);
    auto nodes = many();
    const int which = op == "number" ? 0 : op == "string" ? 1 : op == "boolean" ? 2 : 3;
    return combineAll(nodes, [nodes, which](const EvalContext& ctx) {
      for (const auto& n : nodes) {
        Value v = n->fn(ctx);
        const bool ok = which == 0 ? v.isNumber() : which == 1 ? v.isString() : which == 2 ? v.isBool() : v.isObject();
        if (ok) return v;
      }
      return Value();
    });
  }
  if (op == "array") {
    need(1, 3);
    auto v = sub(args.back());
    const std::string itemType = args.size() >= 2 && args[0].isString() ? args[0].asString() : "";
    const double length = args.size() == 3 && args[1].isNumber() ? args[1].asNumber() : -1;
    return combineAll({v}, [v, itemType, length](const EvalContext& ctx) -> Value {
      const Value x = v->fn(ctx);
      auto list = asList(x);
      if (!list) return {};
      if (length >= 0 && static_cast<double>(list->size()) != length) return {};
      if (!itemType.empty() && itemType != "value") {
        for (const auto& item : *list)
          if (jsTypeOf(item) != itemType) return {};
      }
      return x.isColor() ? Value(*list) : x;
    });
  }
  if (op == "number-format") {
    need(2, 2);
    auto v = sub(args[0]);
    if (!args[1].isObject())
      throw ExpressionError(scope.path + ": \"number-format\" expects a number and an options object");
    const Value& o = args[1];
    const std::string currency = o["currency"].stringOr("");
    const bool hasMin = o["min-fraction-digits"].isNumber();
    const bool hasMax = o["max-fraction-digits"].isNumber();
    int mn = hasMin ? static_cast<int>(o["min-fraction-digits"].asNumber()) : (currency.empty() ? 0 : 2);
    int mx = hasMax ? std::max(static_cast<int>(o["max-fraction-digits"].asNumber()), mn) : (currency.empty() ? std::max(3, mn) : std::max(2, mn));
    if (mn < 0 || mn > 20 || mx < 0 || mx > 20) throw ExpressionError(scope.path + ": \"number-format\" options are not valid");
    std::string symbol;
    if (!currency.empty()) {
      if (currency == "USD") symbol = "$";
      else if (currency == "EUR") symbol = "\xe2\x82\xac";
      else if (currency == "GBP") symbol = "\xc2\xa3";
      else if (currency == "JPY") symbol = "\xc2\xa5";
      else symbol = currency;
    }
    const NumberLocale locale = numberLocale(o["locale"].stringOr("en-US"));
    return combineAll({v}, [v, mn, mx, symbol, locale](const EvalContext& ctx) -> Value {
      const Value x = v->fn(ctx);
      if (!x.isNumber()) return {};
      std::string s = formatFixed(x.asNumber(), mn, mx, locale);
      if (symbol.empty()) return s;
      const bool negative = !s.empty() && s[0] == '-';
      if (negative) s = s.substr(1);
      std::string out;
      if (locale.symbolAfter) out = s + "\xc2\xa0" + symbol;
      else out = symbol.size() == 3 && symbol[0] >= 'A' && symbol[0] <= 'Z' ? symbol + "\xc2\xa0" + s : symbol + s;
      return negative ? "-" + out : out;
    });
  }
  if (op == "hsl" || op == "hsla") {
    const size_t n = op == "hsl" ? 3 : 4;
    need(n, n);
    auto nodes = many();
    return combineAll(nodes, [nodes](const EvalContext& ctx) -> Value {
      double v[4] = {0, 0, 0, 1};
      for (size_t i = 0; i < nodes.size(); i++) {
        const Value x = nodes[i]->fn(ctx);
        if (!x.isNumber()) return {};
        v[i] = x.asNumber();
      }
      return hslToRgba(v[0], v[1] / 100, v[2] / 100, v[3]);
    });
  }
  if (op == "to-rgba" || op == "to-hsla") {
    need(1, 1);
    auto a = sub(args[0]);
    const bool hsla = op == "to-hsla";
    return combineAll({a}, [a, hsla](const EvalContext& ctx) -> Value {
      const Value v = a->fn(ctx);
      std::optional<Color> c;
      if (v.isString()) c = parseColor(v.asString());
      else if (v.isColor() || (v.isArray() && v.size() == 4)) c = toColor(v);
      if (!c) return {};
      if (hsla) return rgbaToHsla(*c);
      return Array{(*c)[0] * 255, (*c)[1] * 255, (*c)[2] * 255, (*c)[3]};
    });
  }
  if (op == "acos" || op == "asin" || op == "atan" || op == "abs" || op == "round" || op == "floor" || op == "ceil" ||
      op == "sqrt" || op == "ln" || op == "log10" || op == "log2" || op == "sin" || op == "cos" || op == "tan") {
    need(1, 1);
    auto a = sub(args[0]);
    Unary f = nullptr;
    if (op == "acos") f = [](double x) { return std::acos(x); };
    else if (op == "asin") f = [](double x) { return std::asin(x); };
    else if (op == "atan") f = [](double x) { return std::atan(x); };
    else if (op == "abs") f = [](double x) { return std::abs(x); };
    else if (op == "round") f = [](double x) { return jsRound(x); };
    else if (op == "floor") f = [](double x) { return std::floor(x); };
    else if (op == "ceil") f = [](double x) { return std::ceil(x); };
    else if (op == "sqrt") f = [](double x) { return std::sqrt(x); };
    else if (op == "ln") f = [](double x) { return std::log(x); };
    else if (op == "log10") f = [](double x) { return std::log10(x); };
    else if (op == "log2") f = [](double x) { return std::log2(x); };
    else if (op == "sin") f = [](double x) { return std::sin(x); };
    else if (op == "cos") f = [](double x) { return std::cos(x); };
    else f = [](double x) { return std::tan(x); };
    return combineAll({a}, [a, f](const EvalContext& ctx) -> Value {
      const Value x = a->fn(ctx);
      return x.isNumber() ? Value(f(x.asNumber())) : Value();
    });
  }
  if (op == "ln2") return constant(std::numbers::ln2);
  if (op == "pi") return constant(std::numbers::pi);
  if (op == "e") return constant(std::numbers::e);
  if (op == "is-supported-script") {
    need(1, 1);
    auto a = sub(args[0]);
    return combineAll({a}, [](const EvalContext&) { return Value(true); });
  }
  if (op == "within") {
    need(1, 1);
    if (!scope.allowGeometry) throw ExpressionError(scope.path + ": \"within\" only works in layer filters");
    auto area = std::make_shared<Area>(withinArea(args[0], scope.path));
    return {[area](const EvalContext& ctx) {
              if (!ctx.feature || !ctx.feature->geometry) return Value(false);
              const WorldGeometry* g = ctx.feature->geometry();
              return Value(g ? insideArea(*g, *area) : false);
            },
            false, true, false};
  }
  if (op == "heatmap-density") {
    need(0, 0);
    return {[](const EvalContext& ctx) { return Value(ctx.density); }, false, true, false};
  }
  if (op == "accumulated") {
    need(0, 0);
    return {[](const EvalContext& ctx) { return ctx.accumulated ? *ctx.accumulated : Value(); }, false, true, false};
  }
  if (op == "image") {
    need(1, 1);
    auto name = sub(args[0]);
    return combineAll({name}, [name](const EvalContext& ctx) -> Value {
      Value id = name->fn(ctx);
      if (!id.isString() || id.asString().empty()) return {};
      if (!ctx.images || (*ctx.images)(id.asString())) return id;
      return {};
    });
  }
  if (op == "+" || op == "*" || op == "min" || op == "max") {
    auto nodes = many();
    const int which = op == "+" ? 0 : op == "*" ? 1 : op == "min" ? 2 : 3;
    return combineAll(nodes, [nodes, which](const EvalContext& ctx) -> Value {
      bool started = false;
      double acc = 0;
      for (const auto& n : nodes) {
        const Value v = n->fn(ctx);
        if (!v.isNumber()) return {};
        const double x = v.asNumber();
        if (!started) {
          acc = x;
          started = true;
        } else if (which == 0) {
          acc += x;
        } else if (which == 1) {
          acc *= x;
        } else if (which == 2) {
          acc = (std::isnan(acc) || std::isnan(x)) ? NAN : std::min(acc, x);
        } else {
          acc = (std::isnan(acc) || std::isnan(x)) ? NAN : std::max(acc, x);
        }
      }
      return started ? Value(acc) : Value();
    });
  }
  if (op == "-") {
    need(1, 2);
    auto nodes = many();
    return combineAll(nodes, [nodes](const EvalContext& ctx) -> Value {
      const Value a = nodes[0]->fn(ctx);
      if (!a.isNumber()) return {};
      if (nodes.size() == 1) return -a.asNumber();
      const Value b = nodes[1]->fn(ctx);
      return b.isNumber() ? Value(a.asNumber() - b.asNumber()) : Value();
    });
  }
  if (op == "/" || op == "%" || op == "^") {
    need(2, 2);
    auto nodes = many();
    const int which = op == "/" ? 0 : op == "%" ? 1 : 2;
    return combineAll(nodes, [a = nodes[0], b = nodes[1], which](const EvalContext& ctx) -> Value {
      const Value x = a->fn(ctx);
      const Value y = b->fn(ctx);
      if (!x.isNumber() || !y.isNumber()) return {};
      if (which == 0) return x.asNumber() / y.asNumber();
      if (which == 1) return std::fmod(x.asNumber(), y.asNumber());
      return std::pow(x.asNumber(), y.asNumber());
    });
  }
  if (op == "concat") {
    auto nodes = many();
    return combineAll(nodes, [nodes](const EvalContext& ctx) {
      std::string out;
      for (const auto& n : nodes) out += stringify(n->fn(ctx));
      return Value(out);
    });
  }
  if (op == "upcase" || op == "downcase") {
    need(1, 1);
    auto a = sub(args[0]);
    const bool up = op == "upcase";
    return combineAll({a}, [a, up](const EvalContext& ctx) {
      const std::string s = stringify(a->fn(ctx));
      return Value(up ? utf8::upper(s) : utf8::lower(s));
    });
  }
  if (op == "length") {
    need(1, 1);
    auto a = sub(args[0]);
    return combineAll({a}, [a](const EvalContext& ctx) -> Value {
      const Value v = a->fn(ctx);
      if (v.isString()) return static_cast<double>(utf8::decode(v.asString()).size());
      if (auto list = asList(v)) return static_cast<double>(list->size());
      return {};
    });
  }
  if (op == "slice") {
    need(2, 3);
    auto nodes = many();
    return combineAll(nodes, [nodes](const EvalContext& ctx) -> Value {
      const Value v = nodes[0]->fn(ctx);
      const Value start = nodes[1]->fn(ctx);
      if (!start.isNumber()) return {};
      std::optional<double> end;
      if (nodes.size() > 2) {
        const Value e = nodes[2]->fn(ctx);
        if (e.isNumber()) end = e.asNumber();
      }
      auto range = [&](size_t n, size_t& from, size_t& to) {
        auto norm = [n](double i) {
          i = std::trunc(i);
          if (i < 0) i = std::max(0.0, static_cast<double>(n) + i);
          return static_cast<size_t>(std::min(i, static_cast<double>(n)));
        };
        from = norm(start.asNumber());
        to = end ? norm(*end) : n;
        if (to < from) to = from;
      };
      if (v.isString()) {
        const auto cps = utf8::decode(v.asString());
        size_t from, to;
        range(cps.size(), from, to);
        return utf8::encode(cps, from, to);
      }
      if (auto list = asList(v)) {
        size_t from, to;
        range(list->size(), from, to);
        return Array(list->begin() + from, list->begin() + to);
      }
      return {};
    });
  }
  if (op == "to-string") {
    need(1, 1);
    auto a = sub(args[0]);
    return combineAll({a}, [a](const EvalContext& ctx) { return Value(stringify(a->fn(ctx))); });
  }
  if (op == "to-number") {
    auto nodes = many();
    return combineAll(nodes, [nodes](const EvalContext& ctx) -> Value {
      for (const auto& n : nodes) {
        const Value v = n->fn(ctx);
        if (v.isNumber()) return v;
        if (v.isBool()) return v.asBool() ? 1.0 : 0.0;
        if (v.isString()) {
          std::string s = v.asString();
          const auto first = s.find_first_not_of(" \t\n\r");
          if (first == std::string::npos) continue;
          s = s.substr(first, s.find_last_not_of(" \t\n\r") - first + 1);
          char* endp = nullptr;
          double num;
          if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) num = static_cast<double>(std::strtoll(s.c_str() + 2, &endp, 16));
          else num = std::strtod(s.c_str(), &endp);
          if (endp && *endp == 0 && std::isfinite(num)) return num;
        }
      }
      return {};
    });
  }
  if (op == "to-boolean") {
    need(1, 1);
    auto a = sub(args[0]);
    return combineAll({a}, [a](const EvalContext& ctx) {
      const Value v = a->fn(ctx);
      if (v.isNull()) return Value(false);
      if (v.isBool()) return v;
      if (v.isNumber()) return Value(v.asNumber() != 0 && !std::isnan(v.asNumber()));
      if (v.isString()) return Value(!v.asString().empty());
      return Value(true);
    });
  }
  if (op == "to-color") {
    auto nodes = many();
    return combineAll(nodes, [nodes](const EvalContext& ctx) -> Value {
      for (const auto& n : nodes) {
        const Value v = n->fn(ctx);
        if (v.isColor()) return v;
        if (v.isArray() && v.size() == 4) {
          if (auto c = toColor(v)) return *c;
        }
        if (v.isString()) {
          if (auto c = parseColor(v.asString())) return *c;
        }
      }
      return {};
    });
  }
  if (op == "rgb" || op == "rgba") {
    const size_t n = op == "rgb" ? 3 : 4;
    need(n, n);
    auto nodes = many();
    return combineAll(nodes, [nodes](const EvalContext& ctx) -> Value {
      double v[4] = {0, 0, 0, 1};
      for (size_t i = 0; i < nodes.size(); i++) {
        const Value x = nodes[i]->fn(ctx);
        if (!x.isNumber()) return {};
        v[i] = x.asNumber();
      }
      return Color{v[0] / 255, v[1] / 255, v[2] / 255, v[3]};
    });
  }
  if (op == "typeof") {
    need(1, 1);
    auto a = sub(args[0]);
    return combineAll({a}, [a](const EvalContext& ctx) { return Value(jsTypeOf(a->fn(ctx))); });
  }
  throw ExpressionError(scope.path + ": unknown operator \"" + op + "\"");
}

}

Value coerce(const Value& v, OutputKind kind) {
  switch (kind) {
    case OutputKind::Number: return v.isNumber() && std::isfinite(v.asNumber()) ? v : Value();
    case OutputKind::Color: {
      if (v.isColor()) return v;
      if (v.isArray() && v.size() == 4) {
        if (auto c = toColor(v)) return *c;
      }
      if (v.isString()) {
        if (auto c = parseColor(v.asString())) return *c;
      }
      return {};
    }
    case OutputKind::String:
      if (v.isString()) return v;
      if (v.isNumber() || v.isBool()) return v.toDisplayString();
      return {};
    case OutputKind::Boolean: return v.isBool() ? v : Value();
    default: return v;
  }
}

bool isExpression(const Value& value) {
  return value.isArray() && value.size() > 0 && value[0].isString() && ops().contains(value[0].asString());
}

bool isOperator(std::string_view name) { return ops().contains(name); }

Compiled compile(const Value& value, OutputKind kind, Value fallback, const std::string& path, CompileFlags flags) {
  auto props = std::make_shared<std::unordered_set<std::string>>();
  Scope scope{props, path, flags.state, flags.geometry, {}};
  Node node = build(value, scope);
  Compiled c;
  c.kind = kind;
  c.fallback = fallback;
  c.properties = props;
  c.zoomDependent = node.zoom;
  c.featureDependent = node.feature;
  c.stateDependent = node.state;
  c.constant = !node.zoom && !node.feature;
  c.fn = std::move(node.fn);
  if (c.constant) {
    EvalContext ctx;
    c.constantValue = c.evaluate(ctx);
    c.fn = nullptr;
  }
  return c;
}

Value Compiled::evaluate(const EvalContext& ctx) const {
  if (!fn) return constantValue;
  Value out = coerce(fn(ctx), kind);
  return out.isNull() ? fallback : out;
}

double Compiled::number(const EvalContext& ctx) const {
  const Value v = evaluate(ctx);
  return v.isNumber() ? v.asNumber() : fallback.numberOr(0);
}

Color Compiled::color(const EvalContext& ctx) const {
  const Value v = evaluate(ctx);
  if (v.isColor()) return v.asColor();
  if (fallback.isColor()) return fallback.asColor();
  return {0, 0, 0, 0};
}

std::string Compiled::string(const EvalContext& ctx) const {
  const Value v = evaluate(ctx);
  return v.isString() ? v.asString() : fallback.stringOr("");
}

bool Compiled::boolean(const EvalContext& ctx) const {
  const Value v = evaluate(ctx);
  return v.isBool() ? v.asBool() : fallback.boolOr(false);
}

}
