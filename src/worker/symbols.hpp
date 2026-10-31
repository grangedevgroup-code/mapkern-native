#pragma once

#include <memory>
#include <string>
#include <vector>

#include "formats/mvt.hpp"
#include "style/expression.hpp"
#include "worker/vector_task.hpp"

namespace mapkern {

struct PackedLabels {
  std::vector<std::string> texts;
  std::vector<std::string> icons;
  std::vector<uint32_t> feature;
  std::vector<float> size;
  std::vector<float> priority;
  std::vector<uint8_t> line;
  std::vector<uint32_t> anchorStart{0};
  std::vector<float> anchors;
  std::vector<uint32_t> pathStart{0};
  std::vector<int32_t> paths;
  size_t count() const { return texts.size(); }
  void add(const std::string& text, const std::string& icon, uint32_t featureIndex, double sz, double prio, bool isLine,
           const std::vector<float>& anchorList, const Ring* path);
};

struct SymbolLayout {
  Compiled text, icon, size, priority, transform;
  std::string placement = "point";
  double spacing = 250;
};

struct LabelInfo {
  std::string text;
  std::string icon;
  double size = 14;
  double priority = 0;
};

std::optional<LabelInfo> labelInfo(const EvalContext& ctx, const SymbolLayout& layout);
double lineLength(const Ring& line);
std::array<double, 2> pointAlong(const Ring& line, double distance);
std::vector<Ring> mergeParts(const std::vector<Ring>& parts);
void installSymbolBuilder();

}
