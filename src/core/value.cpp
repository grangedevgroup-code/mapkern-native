#include "core/value.hpp"

#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace mapkern {

const Value& Value::null() {
  static const Value v;
  return v;
}

const Value* Value::find(std::string_view key) const {
  if (!isObject()) return nullptr;
  for (const auto& [k, v] : asObject())
    if (k == key) return &v;
  return nullptr;
}

const Value& Value::operator[](std::string_view key) const {
  const Value* v = find(key);
  return v ? *v : null();
}

const Value& Value::operator[](size_t index) const {
  if (!isArray() || index >= asArray().size()) return null();
  return asArray()[index];
}

size_t Value::size() const {
  if (isArray()) return asArray().size();
  if (isObject()) return asObject().size();
  return 0;
}

bool Value::operator==(const Value& other) const {
  if (v_.index() != other.v_.index()) return false;
  switch (type()) {
    case Type::Null: return true;
    case Type::Bool: return asBool() == other.asBool();
    case Type::Number: return asNumber() == other.asNumber();
    case Type::String: return asString() == other.asString();
    case Type::Color: return asColor() == other.asColor();
    case Type::Array: {
      const auto& a = asArray();
      const auto& b = other.asArray();
      if (a.size() != b.size()) return false;
      for (size_t i = 0; i < a.size(); i++)
        if (!(a[i] == b[i])) return false;
      return true;
    }
    case Type::Object: {
      const auto& a = asObject();
      const auto& b = other.asObject();
      if (a.size() != b.size()) return false;
      for (const auto& [k, v] : a) {
        const Value* w = other.find(k);
        if (!w || !(v == *w)) return false;
      }
      return true;
    }
  }
  return false;
}

bool Value::sameIdentity(const Value& other) const {
  if (v_.index() != other.v_.index()) return false;
  if (isArray()) return std::get<4>(v_) == std::get<4>(other.v_);
  if (isObject()) return std::get<5>(v_) == std::get<5>(other.v_);
  return *this == other;
}

std::string formatNumber(double v) {
  if (std::isnan(v)) return "NaN";
  if (std::isinf(v)) return v > 0 ? "Infinity" : "-Infinity";
  if (v == 0) return "0";
  if (std::abs(v) < 1e21 && v == std::floor(v)) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.0f", v);
    return buf;
  }
  char buf[40];
  for (int precision = 15; precision <= 17; precision++) {
    std::snprintf(buf, sizeof buf, "%.*g", precision, v);
    if (parseDouble(buf, buf + std::strlen(buf)) == v) break;
  }
  return buf;
}

double parseDouble(const char* begin, const char* end) {
#if defined(__APPLE__)
  static locale_t c = newlocale(LC_ALL_MASK, "C", nullptr);
  std::string tmp(begin, end);
  return strtod_l(tmp.c_str(), nullptr, c);
#else
  double d = 0;
  auto res = std::from_chars(begin, end, d);
  if (res.ec == std::errc::result_out_of_range) return *begin == '-' ? -HUGE_VAL : HUGE_VAL;
  return d;
#endif
}

static void escape(std::string& out, const std::string& s) {
  out += '"';
  for (unsigned char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof buf, "\\u%04x", c);
          out += buf;
        } else {
          out += static_cast<char>(c);
        }
    }
  }
  out += '"';
}

static void write(std::string& out, const Value& v) {
  switch (v.type()) {
    case Value::Type::Null: out += "null"; break;
    case Value::Type::Bool: out += v.asBool() ? "true" : "false"; break;
    case Value::Type::Number: {
      const double n = v.asNumber();
      out += std::isfinite(n) ? formatNumber(n) : "null";
      break;
    }
    case Value::Type::String: escape(out, v.asString()); break;
    case Value::Type::Color: {
      const auto& c = v.asColor();
      out += "[" + formatNumber(c[0]) + "," + formatNumber(c[1]) + "," + formatNumber(c[2]) + "," + formatNumber(c[3]) + "]";
      break;
    }
    case Value::Type::Array: {
      out += '[';
      bool first = true;
      for (const auto& item : v.asArray()) {
        if (!first) out += ',';
        first = false;
        write(out, item);
      }
      out += ']';
      break;
    }
    case Value::Type::Object: {
      out += '{';
      bool first = true;
      for (const auto& [k, item] : v.asObject()) {
        if (!first) out += ',';
        first = false;
        escape(out, k);
        out += ':';
        write(out, item);
      }
      out += '}';
      break;
    }
  }
}

std::string Value::toJson() const {
  std::string out;
  write(out, *this);
  return out;
}

std::string Value::toDisplayString() const {
  switch (type()) {
    case Type::Null: return "";
    case Type::Bool: return asBool() ? "true" : "false";
    case Type::Number: return formatNumber(asNumber());
    case Type::String: return asString();
    default: return toJson();
  }
}

Object withKey(Object object, std::string key, Value value) {
  for (auto& [k, v] : object) {
    if (k == key) {
      v = std::move(value);
      return object;
    }
  }
  object.emplace_back(std::move(key), std::move(value));
  return object;
}

namespace {

struct Parser {
  const char* p;
  const char* end;
  std::string error;
  int depth = 0;

  void ws() {
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
  }

  bool fail(const char* what) {
    if (error.empty()) error = what;
    return false;
  }

  static void utf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
      out += static_cast<char>(cp);
    } else if (cp < 0x800) {
      out += static_cast<char>(0xc0 | (cp >> 6));
      out += static_cast<char>(0x80 | (cp & 0x3f));
    } else if (cp < 0x10000) {
      out += static_cast<char>(0xe0 | (cp >> 12));
      out += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
      out += static_cast<char>(0x80 | (cp & 0x3f));
    } else {
      out += static_cast<char>(0xf0 | (cp >> 18));
      out += static_cast<char>(0x80 | ((cp >> 12) & 0x3f));
      out += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
      out += static_cast<char>(0x80 | (cp & 0x3f));
    }
  }

  bool hex4(uint32_t& out) {
    if (end - p < 4) return fail("bad unicode escape");
    out = 0;
    for (int i = 0; i < 4; i++) {
      const char c = *p++;
      out <<= 4;
      if (c >= '0' && c <= '9') out |= c - '0';
      else if (c >= 'a' && c <= 'f') out |= c - 'a' + 10;
      else if (c >= 'A' && c <= 'F') out |= c - 'A' + 10;
      else return fail("bad unicode escape");
    }
    return true;
  }

  bool string(std::string& out) {
    p++;
    while (p < end) {
      const char c = *p++;
      if (c == '"') return true;
      if (c != '\\') {
        out += c;
        continue;
      }
      if (p >= end) break;
      const char e = *p++;
      switch (e) {
        case '"': out += '"'; break;
        case '\\': out += '\\'; break;
        case '/': out += '/'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case 't': out += '\t'; break;
        case 'u': {
          uint32_t cp;
          if (!hex4(cp)) return false;
          if (cp >= 0xd800 && cp < 0xdc00 && end - p >= 6 && p[0] == '\\' && p[1] == 'u') {
            p += 2;
            uint32_t lo;
            if (!hex4(lo)) return false;
            if (lo >= 0xdc00 && lo < 0xe000) cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
            else {
              utf8(out, 0xfffd);
              cp = lo;
            }
          }
          utf8(out, cp);
          break;
        }
        default: return fail("bad escape");
      }
    }
    return fail("unterminated string");
  }

  bool value(Value& out) {
    if (++depth > 512) return fail("nesting too deep");
    ws();
    if (p >= end) return fail("unexpected end");
    const char c = *p;
    bool ok = true;
    if (c == '{') {
      p++;
      Object obj;
      ws();
      if (p < end && *p == '}') {
        p++;
      } else {
        while (true) {
          ws();
          if (p >= end || *p != '"') {
            ok = fail("expected key");
            break;
          }
          std::string key;
          if (!string(key)) {
            ok = false;
            break;
          }
          ws();
          if (p >= end || *p != ':') {
            ok = fail("expected ':'");
            break;
          }
          p++;
          Value item;
          if (!value(item)) {
            ok = false;
            break;
          }
          bool replaced = false;
          for (auto& [k, v] : obj)
            if (k == key) {
              v = std::move(item);
              replaced = true;
              break;
            }
          if (!replaced) obj.emplace_back(std::move(key), std::move(item));
          ws();
          if (p < end && *p == ',') {
            p++;
            continue;
          }
          if (p < end && *p == '}') {
            p++;
            break;
          }
          ok = fail("expected ',' or '}'");
          break;
        }
      }
      if (ok) out = Value(std::move(obj));
    } else if (c == '[') {
      p++;
      Array arr;
      ws();
      if (p < end && *p == ']') {
        p++;
      } else {
        while (true) {
          Value item;
          if (!value(item)) {
            ok = false;
            break;
          }
          arr.push_back(std::move(item));
          ws();
          if (p < end && *p == ',') {
            p++;
            continue;
          }
          if (p < end && *p == ']') {
            p++;
            break;
          }
          ok = fail("expected ',' or ']'");
          break;
        }
      }
      if (ok) out = Value(std::move(arr));
    } else if (c == '"') {
      std::string s;
      ok = string(s);
      if (ok) out = Value(std::move(s));
    } else if (end - p >= 4 && std::memcmp(p, "true", 4) == 0) {
      p += 4;
      out = Value(true);
    } else if (end - p >= 5 && std::memcmp(p, "false", 5) == 0) {
      p += 5;
      out = Value(false);
    } else if (end - p >= 4 && std::memcmp(p, "null", 4) == 0) {
      p += 4;
      out = Value();
    } else if (c == '-' || (c >= '0' && c <= '9')) {
      const char* start = p;
      if (*p == '-') p++;
      while (p < end && ((*p >= '0' && *p <= '9') || *p == '.' || *p == 'e' || *p == 'E' || *p == '+' || *p == '-')) p++;
      double d = 0;
      auto res = std::from_chars(start, p, d);
      if (res.ec != std::errc() || res.ptr != p) {
        if (res.ec == std::errc::result_out_of_range) d = (*start == '-') ? -HUGE_VAL : HUGE_VAL;
        else ok = fail("bad number");
      }
      if (ok) out = Value(d);
    } else {
      ok = fail("unexpected character");
    }
    depth--;
    return ok;
  }
};

}

std::optional<Value> parseJson(std::string_view text, std::string* error) {
  Parser parser{text.data(), text.data() + text.size(), {}};
  if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xef && static_cast<unsigned char>(text[1]) == 0xbb &&
      static_cast<unsigned char>(text[2]) == 0xbf)
    parser.p += 3;
  Value out;
  if (!parser.value(out)) {
    if (error) *error = parser.error + " at byte " + std::to_string(parser.p - text.data());
    return std::nullopt;
  }
  parser.ws();
  if (parser.p != parser.end) {
    if (error) *error = "trailing characters at byte " + std::to_string(parser.p - text.data());
    return std::nullopt;
  }
  return out;
}

}
