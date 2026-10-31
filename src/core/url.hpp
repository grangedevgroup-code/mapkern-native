#pragma once

#include <string>
#include <string_view>

namespace mapkern {

struct UrlParts {
  std::string scheme;
  std::string host;
  std::string path;
  std::string query;
  std::string fragment;
};

bool hasScheme(std::string_view url);
UrlParts parseUrl(std::string_view url);
std::string resolveUrl(std::string_view url, std::string_view base);
std::string urlOrigin(std::string_view url);
std::string percentDecode(std::string_view s);
std::string percentEncode(std::string_view s);
std::string replaceAll(std::string s, std::string_view from, std::string_view to);

}
