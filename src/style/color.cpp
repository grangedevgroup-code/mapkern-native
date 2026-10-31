#include "style/color.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace mapkern {

namespace {

struct Named {
  const char* name;
  unsigned value;
};

const Named kNamed[] = {
    {"aliceblue", 0xf0f8ff},
    {"antiquewhite", 0xfaebd7},
    {"aqua", 0x00ffff},
    {"aquamarine", 0x7fffd4},
    {"azure", 0xf0ffff},
    {"beige", 0xf5f5dc},
    {"bisque", 0xffe4c4},
    {"black", 0x000000},
    {"blanchedalmond", 0xffebcd},
    {"blue", 0x0000ff},
    {"blueviolet", 0x8a2be2},
    {"brown", 0xa52a2a},
    {"burlywood", 0xdeb887},
    {"cadetblue", 0x5f9ea0},
    {"chartreuse", 0x7fff00},
    {"chocolate", 0xd2691e},
    {"coral", 0xff7f50},
    {"cornflowerblue", 0x6495ed},
    {"cornsilk", 0xfff8dc},
    {"crimson", 0xdc143c},
    {"cyan", 0x00ffff},
    {"darkblue", 0x00008b},
    {"darkcyan", 0x008b8b},
    {"darkgoldenrod", 0xb8860b},
    {"darkgray", 0xa9a9a9},
    {"darkgreen", 0x006400},
    {"darkgrey", 0xa9a9a9},
    {"darkkhaki", 0xbdb76b},
    {"darkmagenta", 0x8b008b},
    {"darkolivegreen", 0x556b2f},
    {"darkorange", 0xff8c00},
    {"darkorchid", 0x9932cc},
    {"darkred", 0x8b0000},
    {"darksalmon", 0xe9967a},
    {"darkseagreen", 0x8fbc8f},
    {"darkslateblue", 0x483d8b},
    {"darkslategray", 0x2f4f4f},
    {"darkslategrey", 0x2f4f4f},
    {"darkturquoise", 0x00ced1},
    {"darkviolet", 0x9400d3},
    {"deeppink", 0xff1493},
    {"deepskyblue", 0x00bfff},
    {"dimgray", 0x696969},
    {"dimgrey", 0x696969},
    {"dodgerblue", 0x1e90ff},
    {"firebrick", 0xb22222},
    {"floralwhite", 0xfffaf0},
    {"forestgreen", 0x228b22},
    {"fuchsia", 0xff00ff},
    {"gainsboro", 0xdcdcdc},
    {"ghostwhite", 0xf8f8ff},
    {"gold", 0xffd700},
    {"goldenrod", 0xdaa520},
    {"gray", 0x808080},
    {"green", 0x008000},
    {"greenyellow", 0xadff2f},
    {"grey", 0x808080},
    {"honeydew", 0xf0fff0},
    {"hotpink", 0xff69b4},
    {"indianred", 0xcd5c5c},
    {"indigo", 0x4b0082},
    {"ivory", 0xfffff0},
    {"khaki", 0xf0e68c},
    {"lavender", 0xe6e6fa},
    {"lavenderblush", 0xfff0f5},
    {"lawngreen", 0x7cfc00},
    {"lemonchiffon", 0xfffacd},
    {"lightblue", 0xadd8e6},
    {"lightcoral", 0xf08080},
    {"lightcyan", 0xe0ffff},
    {"lightgoldenrodyellow", 0xfafad2},
    {"lightgray", 0xd3d3d3},
    {"lightgreen", 0x90ee90},
    {"lightgrey", 0xd3d3d3},
    {"lightpink", 0xffb6c1},
    {"lightsalmon", 0xffa07a},
    {"lightseagreen", 0x20b2aa},
    {"lightskyblue", 0x87cefa},
    {"lightslategray", 0x778899},
    {"lightslategrey", 0x778899},
    {"lightsteelblue", 0xb0c4de},
    {"lightyellow", 0xffffe0},
    {"lime", 0x00ff00},
    {"limegreen", 0x32cd32},
    {"linen", 0xfaf0e6},
    {"magenta", 0xff00ff},
    {"maroon", 0x800000},
    {"mediumaquamarine", 0x66cdaa},
    {"mediumblue", 0x0000cd},
    {"mediumorchid", 0xba55d3},
    {"mediumpurple", 0x9370db},
    {"mediumseagreen", 0x3cb371},
    {"mediumslateblue", 0x7b68ee},
    {"mediumspringgreen", 0x00fa9a},
    {"mediumturquoise", 0x48d1cc},
    {"mediumvioletred", 0xc71585},
    {"midnightblue", 0x191970},
    {"mintcream", 0xf5fffa},
    {"mistyrose", 0xffe4e1},
    {"moccasin", 0xffe4b5},
    {"navajowhite", 0xffdead},
    {"navy", 0x000080},
    {"oldlace", 0xfdf5e6},
    {"olive", 0x808000},
    {"olivedrab", 0x6b8e23},
    {"orange", 0xffa500},
    {"orangered", 0xff4500},
    {"orchid", 0xda70d6},
    {"palegoldenrod", 0xeee8aa},
    {"palegreen", 0x98fb98},
    {"paleturquoise", 0xafeeee},
    {"palevioletred", 0xdb7093},
    {"papayawhip", 0xffefd5},
    {"peachpuff", 0xffdab9},
    {"peru", 0xcd853f},
    {"pink", 0xffc0cb},
    {"plum", 0xdda0dd},
    {"powderblue", 0xb0e0e6},
    {"purple", 0x800080},
    {"rebeccapurple", 0x663399},
    {"red", 0xff0000},
    {"rosybrown", 0xbc8f8f},
    {"royalblue", 0x4169e1},
    {"saddlebrown", 0x8b4513},
    {"salmon", 0xfa8072},
    {"sandybrown", 0xf4a460},
    {"seagreen", 0x2e8b57},
    {"seashell", 0xfff5ee},
    {"sienna", 0xa0522d},
    {"silver", 0xc0c0c0},
    {"skyblue", 0x87ceeb},
    {"slateblue", 0x6a5acd},
    {"slategray", 0x708090},
    {"slategrey", 0x708090},
    {"snow", 0xfffafa},
    {"springgreen", 0x00ff7f},
    {"steelblue", 0x4682b4},
    {"tan", 0xd2b48c},
    {"teal", 0x008080},
    {"thistle", 0xd8bfd8},
    {"tomato", 0xff6347},
    {"turquoise", 0x40e0d0},
    {"violet", 0xee82ee},
    {"wheat", 0xf5deb3},
    {"white", 0xffffff},
    {"whitesmoke", 0xf5f5f5},
    {"yellow", 0xffff00},
    {"yellowgreen", 0x9acd32},
};

double clamp01(double v) { return std::clamp(v, 0.0, 1.0); }

bool parseFloat(std::string_view s, double& out) {
  std::string tmp(s);
  const char* begin = tmp.c_str();
  char* endp = nullptr;
  out = std::strtod(begin, &endp);
  return endp != begin && std::isfinite(out);
}

std::optional<double> channel(std::string_view p) {
  double v;
  if (!parseFloat(p, v)) return std::nullopt;
  return clamp01(p.ends_with('%') ? v / 100 : v / 255);
}

std::optional<double> alpha(std::string_view p) {
  double v;
  if (!parseFloat(p, v)) return std::nullopt;
  return clamp01(p.ends_with('%') ? v / 100 : v);
}

std::optional<double> percent(std::string_view p) {
  double v;
  if (!parseFloat(p, v)) return std::nullopt;
  return clamp01(v / 100);
}

void hslToRgb(double h, double s, double l, double out[3]) {
  if (s == 0) {
    out[0] = out[1] = out[2] = l;
    return;
  }
  const double q = l < 0.5 ? l * (1 + s) : l + s - l * s;
  const double p = 2 * l - q;
  auto hue = [&](double t) {
    const double u = t < 0 ? t + 1 : t > 1 ? t - 1 : t;
    if (u < 1.0 / 6) return p + (q - p) * 6 * u;
    if (u < 1.0 / 2) return q;
    if (u < 2.0 / 3) return p + (q - p) * (2.0 / 3 - u) * 6;
    return p;
  };
  out[0] = hue(h + 1.0 / 3);
  out[1] = hue(h);
  out[2] = hue(h - 1.0 / 3);
}

int hexDigit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

std::optional<Color> parseHex(std::string_view hex) {
  for (char c : hex)
    if (hexDigit(c) < 0) return std::nullopt;
  if (hex.size() == 3 || hex.size() == 4) {
    Color c{0, 0, 0, 1};
    for (size_t i = 0; i < hex.size(); i++) c[i] = hexDigit(hex[i]) * 17 / 255.0;
    return c;
  }
  if (hex.size() == 6 || hex.size() == 8) {
    Color c{0, 0, 0, 1};
    for (size_t i = 0; i < hex.size(); i += 2) c[i / 2] = (hexDigit(hex[i]) * 16 + hexDigit(hex[i + 1])) / 255.0;
    return c;
  }
  return std::nullopt;
}

std::vector<std::string_view> splitParts(std::string_view s) {
  std::vector<std::string_view> parts;
  size_t i = 0;
  while (i < s.size()) {
    while (i < s.size() && (s[i] == ' ' || s[i] == ',' || s[i] == '\t')) i++;
    size_t j = i;
    while (j < s.size() && s[j] != ' ' && s[j] != ',' && s[j] != '\t') j++;
    if (j > i) parts.push_back(s.substr(i, j - i));
    i = j;
  }
  return parts;
}

std::string_view trim(std::string_view s) {
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
  return s;
}

std::optional<Color> parse(std::string_view s) {
  if (s == "transparent") return Color{0, 0, 0, 0};
  for (const auto& n : kNamed) {
    if (s == n.name) return Color{((n.value >> 16) & 255) / 255.0, ((n.value >> 8) & 255) / 255.0, (n.value & 255) / 255.0, 1};
  }
  if (s.starts_with('#')) return parseHex(s.substr(1));
  const size_t open = s.find('(');
  if (open == std::string_view::npos || !s.ends_with(')')) return std::nullopt;
  const std::string_view fn = s.substr(0, open);
  if (fn != "rgb" && fn != "rgba" && fn != "hsl" && fn != "hsla") return std::nullopt;
  const std::string_view body = s.substr(open + 1, s.size() - open - 2);
  if (body.find(')') != std::string_view::npos) return std::nullopt;
  const size_t slash = body.find('/');
  std::string_view main = body;
  std::optional<std::string_view> slashed;
  if (slash != std::string_view::npos) {
    if (body.find('/', slash + 1) != std::string_view::npos) return std::nullopt;
    main = body.substr(0, slash);
    slashed = body.substr(slash + 1);
  }
  auto parts = splitParts(main);
  if (slashed) {
    const auto last = trim(*slashed);
    if (parts.size() != 3 || last.empty() || last.find_first_of(" ,\t") != std::string_view::npos) return std::nullopt;
    parts.push_back(last);
  }
  if (parts.size() < 3 || parts.size() > 4) return std::nullopt;
  double a = 1;
  if (parts.size() == 4) {
    auto v = alpha(parts[3]);
    if (!v) return std::nullopt;
    a = *v;
  }
  if (fn.starts_with("rgb")) {
    auto r = channel(parts[0]), g = channel(parts[1]), b = channel(parts[2]);
    if (!r || !g || !b) return std::nullopt;
    return Color{*r, *g, *b, a};
  }
  std::string_view hue = parts[0];
  if (hue.ends_with("deg")) hue.remove_suffix(3);
  double h;
  if (!parseFloat(hue, h)) return std::nullopt;
  auto sat = percent(parts[1]), light = percent(parts[2]);
  if (!sat || !light) return std::nullopt;
  double rgb[3];
  hslToRgb(std::fmod(std::fmod(h, 360) + 360, 360) / 360, *sat, *light, rgb);
  return Color{rgb[0], rgb[1], rgb[2], a};
}

}

std::optional<Color> parseColor(std::string_view input) {
  std::string key(trim(input));
  for (auto& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  static std::mutex mutex;
  static std::unordered_map<std::string, std::optional<Color>> cache;
  {
    std::lock_guard lock(mutex);
    if (auto it = cache.find(key); it != cache.end()) return it->second;
  }
  auto value = parse(key);
  std::lock_guard lock(mutex);
  if (cache.size() > 2048) cache.clear();
  cache.emplace(std::move(key), value);
  return value;
}

Color premultiply(const Color& c, double opacity) {
  const double a = c[3] * opacity;
  return {c[0] * a, c[1] * a, c[2] * a, a};
}

Color mixColor(const Color& a, const Color& b, double t) {
  return {a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t, a[3] + (b[3] - a[3]) * t};
}

std::string colorToCss(const Color& c) {
  auto to = [](double v) { return static_cast<int>(std::lround(clamp01(v) * 255)); };
  return "rgba(" + std::to_string(to(c[0])) + ", " + std::to_string(to(c[1])) + ", " + std::to_string(to(c[2])) + ", " +
         formatNumber(std::round(clamp01(c[3]) * 1000) / 1000) + ")";
}

std::optional<Color> toColor(const Value& v) {
  if (v.isColor()) return v.asColor();
  if (v.isString()) return parseColor(v.asString());
  if (v.isArray() && v.size() == 4) {
    Color c{};
    for (size_t i = 0; i < 4; i++) {
      if (!v[i].isNumber()) return std::nullopt;
      c[i] = v[i].asNumber();
    }
    return c;
  }
  return std::nullopt;
}

}
