#pragma once

#include <string>
#include <vector>

#include "core/value.hpp"

namespace mapkern {

struct StyleIssue {
  std::string path;
  std::string message;
};

using Issues = std::vector<StyleIssue>;

extern const std::vector<std::string> kFeatureFormats;

Issues validateStyle(const Value& style);
Issues validateSky(const Value& sky);
Issues validateSun(const Value& sun);
Issues validateSource(const std::string& id, const Value& source);
Issues validateLayer(const Value& layer, const std::string& path, const Value& sources);
std::string formatIssues(const Issues& issues);
std::optional<double> sunTime(const Value& time);

}
