#include "core/url.hpp"

#include <cctype>
#include <vector>

namespace mapkern {

bool hasScheme(std::string_view url) {
  if (url.empty() || !std::isalpha(static_cast<unsigned char>(url[0]))) return false;
  for (size_t i = 1; i < url.size(); i++) {
    const char c = url[i];
    if (c == ':') return i > 1;
    if (!std::isalnum(static_cast<unsigned char>(c)) && c != '+' && c != '-' && c != '.') return false;
  }
  return false;
}

UrlParts parseUrl(std::string_view url) {
  UrlParts p;
  std::string_view rest = url;
  if (const size_t hash = rest.find('#'); hash != std::string_view::npos) {
    p.fragment = std::string(rest.substr(hash + 1));
    rest = rest.substr(0, hash);
  }
  if (const size_t q = rest.find('?'); q != std::string_view::npos) {
    p.query = std::string(rest.substr(q + 1));
    rest = rest.substr(0, q);
  }
  if (hasScheme(rest)) {
    const size_t colon = rest.find(':');
    p.scheme = std::string(rest.substr(0, colon));
    for (auto& c : p.scheme) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    rest = rest.substr(colon + 1);
  }
  if (rest.starts_with("//")) {
    rest = rest.substr(2);
    const size_t slash = rest.find('/');
    p.host = std::string(rest.substr(0, slash));
    rest = slash == std::string_view::npos ? std::string_view() : rest.substr(slash);
  }
  p.path = std::string(rest);
  return p;
}

static std::string normalizePath(const std::string& path) {
  std::vector<std::string> out;
  size_t i = 0;
  const bool absolute = !path.empty() && path[0] == '/';
  const bool trailing = !path.empty() && (path.back() == '/' || path.ends_with("/.") || path.ends_with("/.."));
  while (i <= path.size()) {
    size_t j = path.find('/', i);
    if (j == std::string::npos) j = path.size();
    const std::string seg = path.substr(i, j - i);
    if (seg == "..") {
      if (!out.empty()) out.pop_back();
    } else if (!seg.empty() && seg != ".") {
      out.push_back(seg);
    }
    i = j + 1;
  }
  std::string r = absolute ? "/" : "";
  for (size_t k = 0; k < out.size(); k++) r += (k ? "/" : "") + out[k];
  if (trailing && !out.empty()) r += "/";
  return r;
}

std::string resolveUrl(std::string_view url, std::string_view base) {
  if (base.empty() || hasScheme(url)) return std::string(url);
  const UrlParts b = parseUrl(base);
  std::string prefix = b.scheme.empty() ? "" : b.scheme + ":";
  if (url.starts_with("//")) return prefix + std::string(url);
  if (!b.host.empty() || b.scheme == "file") prefix += "//" + b.host;
  if (url.empty()) return std::string(base.substr(0, base.find('#')));
  if (url[0] == '#') return std::string(base.substr(0, base.find('#'))) + std::string(url);
  if (url[0] == '?') return prefix + b.path + std::string(url);
  std::string rel(url);
  std::string suffix;
  if (const size_t q = rel.find_first_of("?#"); q != std::string::npos) {
    suffix = rel.substr(q);
    rel = rel.substr(0, q);
  }
  std::string path;
  if (rel[0] == '/') {
    path = rel;
  } else {
    const size_t slash = b.path.rfind('/');
    path = (slash == std::string::npos ? std::string("/") : b.path.substr(0, slash + 1)) + rel;
  }
  return prefix + normalizePath(path) + suffix;
}

std::string urlOrigin(std::string_view url) {
  const UrlParts p = parseUrl(url);
  return p.scheme + "://" + p.host;
}

std::string percentDecode(std::string_view s) {
  std::string out;
  size_t i = 0;
  while (i < s.size()) {
    if (s[i] == '%' && i + 2 < s.size() && std::isxdigit(static_cast<unsigned char>(s[i + 1])) &&
        std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
      out += static_cast<char>(std::stoi(std::string(s.substr(i + 1, 2)), nullptr, 16));
      i += 3;
    } else {
      out += s[i++];
    }
  }
  return out;
}

std::string percentEncode(std::string_view s) {
  static const char* hex = "0123456789ABCDEF";
  std::string out;
  for (unsigned char c : s) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out += static_cast<char>(c);
    } else {
      out += '%';
      out += hex[c >> 4];
      out += hex[c & 15];
    }
  }
  return out;
}

std::string replaceAll(std::string s, std::string_view from, std::string_view to) {
  if (from.empty()) return s;
  size_t pos = 0;
  while ((pos = s.find(from, pos)) != std::string::npos) {
    s.replace(pos, from.size(), to);
    pos += to.size();
  }
  return s;
}

}
